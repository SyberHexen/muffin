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
 * wp_fifo_v1: a content update can set a barrier on its surface, and a
 * later one can wait for it. The barrier is cleared at the latching
 * deadline after it was set, i.e. once the update that set it was drawn
 * for its monitor's next frame: an update that waits is shown for at least
 * one refresh of the one before. That's FIFO (vsync) presentation without
 * the client blocking on frame callbacks.
 *
 * The surface's monitor is its main view, the one presentation feedback
 * reports. A waiting update is held back with the surface's queued
 * updates.
 */

#include "config.h"

#include "wayland/meta-wayland-fifo.h"

#include "wayland/meta-wayland-presentation-time.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-wayland-versions.h"

#include "fifo-v1-server-protocol.h"

static void
remove_barrier_surface (MetaWaylandSurface *surface)
{
  MetaWaylandCompositor *compositor = surface->compositor;

  compositor->fifo_barrier_surfaces =
    g_list_remove (compositor->fifo_barrier_surfaces, surface);
}

void
meta_wayland_fifo_set_barrier (MetaWaylandSurface *surface,
                               gboolean            had_damage)
{
  MetaWaylandCompositor *compositor = surface->compositor;

  surface->fifo.barrier_needs_paint = had_damage;

  if (surface->fifo.barrier)
    return;

  surface->fifo.barrier = TRUE;
  compositor->fifo_barrier_surfaces =
    g_list_prepend (compositor->fifo_barrier_surfaces, surface);
}

/* Whether the latching deadline after the barrier was set has passed: the
 * main view was drawn since, or had nothing of the surface's to draw */
static gboolean
barrier_deadline_passed (MetaWaylandSurface *surface)
{
  ClutterStageView *view;

  view = meta_wayland_presentation_time_get_main_view (surface);
  if (!view)
    return TRUE;

  if (clutter_stage_view_was_painted (view))
    return TRUE;

  return !surface->fifo.barrier_needs_paint &&
         !clutter_stage_view_has_pending_frame (view);
}

void
meta_wayland_fifo_after_paint (MetaWaylandCompositor *compositor)
{
  GList *l;

  l = compositor->fifo_barrier_surfaces;
  while (l)
    {
      GList *l_cur = l;
      MetaWaylandSurface *surface = l->data;

      l = l->next;

      if (!barrier_deadline_passed (surface))
        continue;

      surface->fifo.barrier = FALSE;
      compositor->fifo_barrier_surfaces =
        g_list_delete_link (compositor->fifo_barrier_surfaces, l_cur);
    }
}

void
meta_wayland_fifo_surface_destroyed (MetaWaylandSurface *surface)
{
  remove_barrier_surface (surface);
  surface->fifo.barrier = FALSE;

  if (surface->fifo.resource)
    {
      wl_resource_set_user_data (surface->fifo.resource, NULL);
      surface->fifo.resource = NULL;
    }
}

static void
fifo_destructor (struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);

  if (surface)
    surface->fifo.resource = NULL;
}

static MetaWaylandSurface *
get_fifo_surface (struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);

  if (!surface)
    {
      wl_resource_post_error (resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
                              "The fifo object's surface was destroyed");
      return NULL;
    }

  return surface;
}

static void
fifo_set_barrier (struct wl_client   *client,
                  struct wl_resource *resource)
{
  MetaWaylandSurface *surface = get_fifo_surface (resource);

  if (surface)
    meta_wayland_surface_get_pending_state (surface)->fifo_set_barrier = TRUE;
}

static void
fifo_wait_barrier (struct wl_client   *client,
                   struct wl_resource *resource)
{
  MetaWaylandSurface *surface = get_fifo_surface (resource);

  if (surface)
    meta_wayland_surface_get_pending_state (surface)->fifo_wait_barrier = TRUE;
}

static void
fifo_destroy (struct wl_client   *client,
              struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wp_fifo_v1_interface fifo_implementation = {
  fifo_set_barrier,
  fifo_wait_barrier,
  fifo_destroy,
};

static void
fifo_manager_destroy (struct wl_client   *client,
                      struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
fifo_manager_get_fifo (struct wl_client   *client,
                       struct wl_resource *resource,
                       uint32_t            id,
                       struct wl_resource *surface_resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_resource *fifo_resource;

  if (surface->fifo.resource)
    {
      wl_resource_post_error (resource, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
                              "The surface already has a fifo object");
      return;
    }

  fifo_resource = wl_resource_create (client, &wp_fifo_v1_interface,
                                      wl_resource_get_version (resource), id);
  if (!fifo_resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (fifo_resource, &fifo_implementation,
                                  surface, fifo_destructor);
  surface->fifo.resource = fifo_resource;
}

static const struct wp_fifo_manager_v1_interface fifo_manager_implementation = {
  fifo_manager_destroy,
  fifo_manager_get_fifo,
};

static void
fifo_manager_bind (struct wl_client *client,
                   void             *data,
                   uint32_t          version,
                   uint32_t          id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_fifo_manager_v1_interface,
                                 version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &fifo_manager_implementation,
                                  data, NULL);
}

void
meta_wayland_init_fifo (MetaWaylandCompositor *compositor)
{
  if (!wl_global_create (compositor->wayland_display,
                         &wp_fifo_manager_v1_interface,
                         META_WP_FIFO_V1_VERSION,
                         compositor, fifo_manager_bind))
    g_error ("Failed to register the global wp_fifo_manager_v1");
}
