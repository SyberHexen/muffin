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
 * wp_commit_timing_v1: a content update can carry the time it is meant to
 * be shown at, as closely as possible but not before. It is held back with
 * the surface's queued updates until the next frame its main view would
 * draw would be shown at or after that time, so it lands on the first
 * refresh at or after it.
 */

#include "config.h"

#include "wayland/meta-wayland-commit-timing.h"

#include "wayland/meta-wayland-presentation-time.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-wayland-versions.h"

#include "commit-timing-v1-server-protocol.h"

/* While an update isn't due yet, it is checked again no more often than
 * this */
#define MIN_RECHECK_US 1000

gboolean
meta_wayland_commit_timing_is_ready (MetaWaylandSurface *surface,
                                     int64_t             target_us,
                                     int64_t             now_us,
                                     int64_t            *wake_up_us)
{
  ClutterStageView *view;
  int64_t next_presentation_us;
  int64_t refresh_interval_us = G_USEC_PER_SEC / 60;
  int64_t check_us;

  view = meta_wayland_presentation_time_get_main_view (surface);
  if (!view)
    return TRUE;

  next_presentation_us =
    clutter_stage_view_get_next_presentation_time (view, &refresh_interval_us);

  /* Without the monitor's timing, apply it at its time */
  if (next_presentation_us <= 0)
    {
      if (target_us <= now_us)
        return TRUE;

      *wake_up_us = MIN (*wake_up_us, target_us);
      return FALSE;
    }

  if (target_us <= next_presentation_us)
    return TRUE;

  /* Check again about when the frame shown at or after the target would
   * be drawn: a refresh before it */
  check_us = MAX (target_us - refresh_interval_us, now_us + MIN_RECHECK_US);
  *wake_up_us = MIN (*wake_up_us, check_us);

  return FALSE;
}

void
meta_wayland_commit_timing_surface_destroyed (MetaWaylandSurface *surface)
{
  if (surface->commit_timing.resource)
    {
      wl_resource_set_user_data (surface->commit_timing.resource, NULL);
      surface->commit_timing.resource = NULL;
    }
}

static void
commit_timer_destructor (struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);

  if (surface)
    surface->commit_timing.resource = NULL;
}

static void
commit_timer_set_timestamp (struct wl_client   *client,
                            struct wl_resource *resource,
                            uint32_t            tv_sec_hi,
                            uint32_t            tv_sec_lo,
                            uint32_t            tv_nsec)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);
  MetaWaylandSurfaceState *pending;
  uint64_t tv_sec;

  if (!surface)
    {
      wl_resource_post_error (resource,
                              WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED,
                              "The commit timer's surface was destroyed");
      return;
    }

  if (tv_nsec >= 1000000000)
    {
      wl_resource_post_error (resource,
                              WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
                              "tv_nsec %u is out of range", tv_nsec);
      return;
    }

  pending = meta_wayland_surface_get_pending_state (surface);
  if (pending->has_target_time)
    {
      wl_resource_post_error (resource,
                              WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS,
                              "The commit already has a timestamp");
      return;
    }

  tv_sec = ((uint64_t) tv_sec_hi << 32) | tv_sec_lo;

  /* Rounded up: never shown before it */
  pending->has_target_time = TRUE;
  pending->target_time_us = (int64_t) (tv_sec * G_USEC_PER_SEC) +
                            (tv_nsec + 999) / 1000;
}

static void
commit_timer_destroy (struct wl_client   *client,
                      struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wp_commit_timer_v1_interface commit_timer_implementation = {
  commit_timer_set_timestamp,
  commit_timer_destroy,
};

static void
commit_timing_manager_destroy (struct wl_client   *client,
                               struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
commit_timing_manager_get_timer (struct wl_client   *client,
                                 struct wl_resource *resource,
                                 uint32_t            id,
                                 struct wl_resource *surface_resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_resource *timer_resource;

  if (surface->commit_timing.resource)
    {
      wl_resource_post_error (resource,
                              WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
                              "The surface already has a commit timer");
      return;
    }

  timer_resource = wl_resource_create (client, &wp_commit_timer_v1_interface,
                                       wl_resource_get_version (resource), id);
  if (!timer_resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (timer_resource, &commit_timer_implementation,
                                  surface, commit_timer_destructor);
  surface->commit_timing.resource = timer_resource;
}

static const struct wp_commit_timing_manager_v1_interface commit_timing_manager_implementation = {
  commit_timing_manager_destroy,
  commit_timing_manager_get_timer,
};

static void
commit_timing_manager_bind (struct wl_client *client,
                            void             *data,
                            uint32_t          version,
                            uint32_t          id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_commit_timing_manager_v1_interface,
                                 version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource,
                                  &commit_timing_manager_implementation,
                                  data, NULL);
}

void
meta_wayland_init_commit_timing (MetaWaylandCompositor *compositor)
{
  if (!wl_global_create (compositor->wayland_display,
                         &wp_commit_timing_manager_v1_interface,
                         META_WP_COMMIT_TIMING_V1_VERSION,
                         compositor, commit_timing_manager_bind))
    g_error ("Failed to register the global wp_commit_timing_manager_v1");
}
