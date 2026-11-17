/*
 * Copyright (C) 2026 Andrew Hexen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * wp_presentation: tells clients when what they committed reached the
 * screen, at what refresh interval and vertical retrace count.
 *
 * A feedback requested with a commit waits on the surface until a redraw
 * draws the surface's main view, the one monitor whose timing the surface
 * reports (the fastest one it is on). It then waits for that view's frame
 * and is presented with the frame's timing. A commit that comes before the
 * last one was drawn, a surface that isn't shown or goes away, and a
 * monitor that goes away discard it.
 */

#include "config.h"

#include "wayland/meta-wayland-presentation-time.h"

#include <time.h>

#include "backends/meta-logical-monitor.h"
#include "wayland/meta-wayland-outputs.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-wayland-versions.h"

#include "presentation-time-server-protocol.h"

typedef struct _MetaWaylandPresentationFeedback
{
  struct wl_list link;
  struct wl_resource *resource;

  /* Once drawn: the view whose frame it waits for (weak) */
  ClutterStageView *view;
} MetaWaylandPresentationFeedback;

static void
feedback_destructor (struct wl_resource *resource)
{
  MetaWaylandPresentationFeedback *feedback =
    wl_resource_get_user_data (resource);

  wl_list_remove (&feedback->link);
  g_clear_weak_pointer (&feedback->view);
  g_free (feedback);
}

void
meta_wayland_presentation_time_discard_list (struct wl_list *list)
{
  MetaWaylandPresentationFeedback *feedback, *next;

  wl_list_for_each_safe (feedback, next, list, link)
    {
      wp_presentation_feedback_send_discarded (feedback->resource);
      wl_resource_destroy (feedback->resource);
    }
}

static void
remove_feedback_surface (MetaWaylandCompositor *compositor,
                         MetaWaylandSurface    *surface)
{
  compositor->presentation_time.feedback_surfaces =
    g_list_remove (compositor->presentation_time.feedback_surfaces, surface);
}

void
meta_wayland_presentation_time_commit (MetaWaylandSurface *surface,
                                       struct wl_list     *feedback_list,
                                       gboolean            had_damage)
{
  MetaWaylandCompositor *compositor = surface->compositor;
  MetaSurfaceActor *actor;

  /* This commit replaces what an earlier one put up but wasn't drawn */
  meta_wayland_presentation_time_discard_list (&surface->presentation_time.feedback_list);

  if (wl_list_empty (feedback_list))
    {
      remove_feedback_surface (compositor, surface);
      return;
    }

  /* Not shown (minimized, on another workspace, not mapped yet): nothing
   * would draw it */
  actor = meta_wayland_surface_get_actor (surface);
  if (!actor || !clutter_actor_is_mapped (CLUTTER_ACTOR (actor)))
    {
      meta_wayland_presentation_time_discard_list (feedback_list);
      remove_feedback_surface (compositor, surface);
      return;
    }

  wl_list_insert_list (&surface->presentation_time.feedback_list,
                       feedback_list);
  wl_list_init (feedback_list);

  if (!g_list_find (compositor->presentation_time.feedback_surfaces, surface))
    {
      compositor->presentation_time.feedback_surfaces =
        g_list_prepend (compositor->presentation_time.feedback_surfaces,
                        surface);
    }

  /* With nothing changed, nothing would redraw the surface: draw it again,
   * so the client learns when its content (still) reached the screen */
  if (!had_damage)
    clutter_actor_queue_redraw (CLUTTER_ACTOR (actor));
}

void
meta_wayland_presentation_time_surface_destroyed (MetaWaylandSurface *surface)
{
  meta_wayland_presentation_time_discard_list (&surface->presentation_time.feedback_list);
  remove_feedback_surface (surface->compositor, surface);
}

static MetaWaylandOutput *
find_output_for_view (MetaWaylandCompositor *compositor,
                      ClutterStageView      *view)
{
  cairo_rectangle_int_t view_layout;
  GHashTableIter iter;
  MetaWaylandOutput *output;

  clutter_stage_view_get_layout (view, &view_layout);

  g_hash_table_iter_init (&iter, compositor->outputs);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &output))
    {
      MetaRectangle *rect;

      if (!output->logical_monitor)
        continue;

      rect = &output->logical_monitor->rect;
      if (view_layout.x >= rect->x &&
          view_layout.y >= rect->y &&
          view_layout.x + view_layout.width <= rect->x + rect->width &&
          view_layout.y + view_layout.height <= rect->y + rect->height)
        return output;
    }

  return NULL;
}

static float
get_view_refresh_rate (MetaWaylandCompositor *compositor,
                       ClutterStageView      *view)
{
  MetaWaylandOutput *output = find_output_for_view (compositor, view);

  return output ? output->refresh_rate : 0.0f;
}

/* The view whose timing the surface's feedbacks report: the fastest monitor
 * it is on, and the same one as before while it is still on it, so the
 * timing a client sees doesn't jump between monitors */
static ClutterStageView *
get_main_view (MetaWaylandCompositor *compositor,
               MetaWaylandSurface    *surface,
               GList                 *views)
{
  ClutterStageView *main_view = NULL;
  float main_refresh_rate = 0.0f;
  GList *l;

  if (g_list_find (views, surface->presentation_time.main_view))
    return surface->presentation_time.main_view;

  for (l = views; l; l = l->next)
    {
      ClutterStageView *view = l->data;
      float refresh_rate = get_view_refresh_rate (compositor, view);

      if (!main_view || refresh_rate > main_refresh_rate)
        {
          main_view = view;
          main_refresh_rate = refresh_rate;
        }
    }

  surface->presentation_time.main_view = main_view;

  return main_view;
}

/**
 * meta_wayland_presentation_time_get_main_view:
 *
 * Returns: (nullable): the view whose frames time @surface: the fastest
 *   monitor it is on, NULL if it isn't shown
 */
ClutterStageView *
meta_wayland_presentation_time_get_main_view (MetaWaylandSurface *surface)
{
  MetaSurfaceActor *actor = meta_wayland_surface_get_actor (surface);
  GList *views;

  if (!actor || !clutter_actor_is_mapped (CLUTTER_ACTOR (actor)))
    return NULL;

  views = clutter_actor_peek_stage_views (CLUTTER_ACTOR (actor));
  if (!views)
    return NULL;

  return get_main_view (surface->compositor, surface, views);
}

static void
send_presented (MetaWaylandCompositor           *compositor,
                MetaWaylandPresentationFeedback *feedback,
                ClutterStageView                *view,
                int64_t                          time_ns,
                float                            refresh_rate,
                int64_t                          sequence,
                uint32_t                         flags)
{
  struct wl_client *client = wl_resource_get_client (feedback->resource);
  MetaWaylandOutput *output;
  uint64_t tv_sec;
  uint32_t tv_nsec;
  uint32_t refresh_ns;

  output = view ? find_output_for_view (compositor, view) : NULL;
  if (output)
    {
      GList *l;

      for (l = output->resources; l; l = l->next)
        {
          struct wl_resource *output_resource = l->data;

          if (wl_resource_get_client (output_resource) == client)
            wp_presentation_feedback_send_sync_output (feedback->resource,
                                                       output_resource);
        }
    }

  /* With VRR, the refresh interval is the shortest one the monitor takes */
  if (wl_resource_get_version (feedback->resource) >= 3)
    {
      if (view && clutter_stage_view_is_vrr_active (view))
        flags |= WP_PRESENTATION_FEEDBACK_KIND_VARIABLE_RATE;
      else
        flags |= WP_PRESENTATION_FEEDBACK_KIND_FIXED_RATE;
    }

  tv_sec = time_ns / G_GINT64_CONSTANT (1000000000);
  tv_nsec = time_ns % G_GINT64_CONSTANT (1000000000);
  refresh_ns = refresh_rate > 0.0f ? (uint32_t) (1e9 / refresh_rate + 0.5) : 0;

  wp_presentation_feedback_send_presented (feedback->resource,
                                           (uint32_t) (tv_sec >> 32),
                                           (uint32_t) tv_sec,
                                           tv_nsec,
                                           refresh_ns,
                                           (uint32_t) ((uint64_t) sequence >> 32),
                                           (uint32_t) sequence,
                                           flags);
  wl_resource_destroy (feedback->resource);
}

static int64_t
get_monotonic_time_ns (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * G_GINT64_CONSTANT (1000000000) + ts.tv_nsec;
}

static void
discard_orphans (MetaWaylandCompositor *compositor)
{
  MetaWaylandPresentationFeedback *feedback, *next;

  /* Their monitor went away */
  wl_list_for_each_safe (feedback, next,
                         &compositor->presentation_time.in_flight, link)
    {
      if (feedback->view)
        continue;

      wp_presentation_feedback_send_discarded (feedback->resource);
      wl_resource_destroy (feedback->resource);
    }
}

void
meta_wayland_presentation_time_after_paint (MetaWaylandCompositor *compositor)
{
  GList *l;

  discard_orphans (compositor);

  l = compositor->presentation_time.feedback_surfaces;
  while (l)
    {
      GList *l_cur = l;
      MetaWaylandSurface *surface = l->data;
      MetaWaylandPresentationFeedback *feedback, *next;
      MetaSurfaceActor *actor;
      ClutterStageView *view;
      GList *views = NULL;

      l = l->next;

      actor = meta_wayland_surface_get_actor (surface);
      if (actor && clutter_actor_is_mapped (CLUTTER_ACTOR (actor)))
        views = clutter_actor_peek_stage_views (CLUTTER_ACTOR (actor));

      if (!views)
        {
          meta_wayland_presentation_time_discard_list (&surface->presentation_time.feedback_list);
          compositor->presentation_time.feedback_surfaces =
            g_list_delete_link (compositor->presentation_time.feedback_surfaces,
                                l_cur);
          continue;
        }

      view = get_main_view (compositor, surface, views);
      if (!clutter_stage_view_was_painted (view))
        continue;

      wl_list_for_each_safe (feedback, next,
                             &surface->presentation_time.feedback_list, link)
        {
          /* The frame finished already (no page flip to wait for): it is
           * on the screen now */
          if (!clutter_stage_view_has_pending_frame (view))
            {
              send_presented (compositor, feedback, view,
                              get_monotonic_time_ns (),
                              get_view_refresh_rate (compositor, view),
                              0, 0);
              continue;
            }

          wl_list_remove (&feedback->link);
          g_set_weak_pointer (&feedback->view, view);
          wl_list_insert (compositor->presentation_time.in_flight.prev,
                          &feedback->link);
        }

      compositor->presentation_time.feedback_surfaces =
        g_list_delete_link (compositor->presentation_time.feedback_surfaces,
                            l_cur);
    }
}

void
meta_wayland_presentation_time_present (MetaWaylandCompositor *compositor,
                                        ClutterFrameInfo      *frame_info)
{
  MetaWaylandPresentationFeedback *feedback, *next;
  int64_t time_ns;
  uint32_t flags;

  discard_orphans (compositor);

  if (wl_list_empty (&compositor->presentation_time.in_flight))
    return;

  /* A frame flipped at once wasn't synchronized to the vertical blank */
  flags = frame_info->tearing ? 0 : WP_PRESENTATION_FEEDBACK_KIND_VSYNC;
  if (frame_info->hw_clock && frame_info->presentation_time != 0)
    {
      /* The page flip's time, CLOCK_MONOTONIC like the clock we announce */
      time_ns = frame_info->presentation_time;
      flags |= WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK |
               WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION;
    }
  else
    {
      time_ns = get_monotonic_time_ns ();
    }

  wl_list_for_each_safe (feedback, next,
                         &compositor->presentation_time.in_flight, link)
    {
      /* Without a view, the frame is the whole stage's */
      if (frame_info->view && feedback->view != frame_info->view)
        continue;

      send_presented (compositor, feedback, feedback->view,
                      time_ns,
                      frame_info->refresh_rate,
                      frame_info->sequence,
                      flags);
    }
}

static void
presentation_destroy (struct wl_client   *client,
                      struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
presentation_feedback (struct wl_client   *client,
                       struct wl_resource *resource,
                       struct wl_resource *surface_resource,
                       uint32_t            callback_id)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  MetaWaylandSurfaceState *pending;
  MetaWaylandPresentationFeedback *feedback;

  feedback = g_new0 (MetaWaylandPresentationFeedback, 1);
  wl_list_init (&feedback->link);
  feedback->resource = wl_resource_create (client,
                                           &wp_presentation_feedback_interface,
                                           wl_resource_get_version (resource),
                                           callback_id);
  if (!feedback->resource)
    {
      g_free (feedback);
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (feedback->resource, NULL, feedback,
                                  feedback_destructor);

  if (!surface)
    {
      wp_presentation_feedback_send_discarded (feedback->resource);
      wl_resource_destroy (feedback->resource);
      return;
    }

  pending = meta_wayland_surface_get_pending_state (surface);
  wl_list_insert (pending->presentation_feedback_list.prev, &feedback->link);
}

static const struct wp_presentation_interface presentation_implementation = {
  presentation_destroy,
  presentation_feedback,
};

static void
presentation_bind (struct wl_client *client,
                   void             *data,
                   uint32_t          version,
                   uint32_t          id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_presentation_interface,
                                 version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &presentation_implementation,
                                  data, NULL);

  wp_presentation_send_clock_id (resource, CLOCK_MONOTONIC);
}

void
meta_wayland_init_presentation_time (MetaWaylandCompositor *compositor)
{
  wl_list_init (&compositor->presentation_time.in_flight);

  if (!wl_global_create (compositor->wayland_display,
                         &wp_presentation_interface,
                         META_WP_PRESENTATION_VERSION,
                         compositor, presentation_bind))
    g_error ("Failed to register the global wp_presentation");
}
