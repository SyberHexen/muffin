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
 * wp_tearing_control_v1: a surface can hint that its frames should be
 * shown at once instead of at the monitor's vertical blank, accepting
 * tearing for lower latency (games with vsync off; Xwayland sends it for
 * X11 games that present asynchronously).
 *
 * The hint is honoured only while the surface is a monitor's direct
 * scanout (a fullscreen window): see meta-compositor-native.c. Composited
 * frames never tear.
 */

#include "config.h"

#include "wayland/meta-wayland-tearing-control.h"

#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-wayland-versions.h"

#include "tearing-control-v1-server-protocol.h"

gboolean
meta_wayland_surface_wants_tearing (MetaWaylandSurface *surface)
{
  return surface->tearing.async;
}

void
meta_wayland_tearing_control_surface_destroyed (MetaWaylandSurface *surface)
{
  if (surface->tearing.resource)
    {
      wl_resource_set_user_data (surface->tearing.resource, NULL);
      surface->tearing.resource = NULL;
    }
}

static void
set_pending_hint (MetaWaylandSurface *surface,
                  gboolean            async)
{
  MetaWaylandSurfaceState *pending;

  pending = meta_wayland_surface_get_pending_state (surface);
  pending->has_new_tearing_hint = TRUE;
  pending->tearing_async = async;
}

static void
tearing_control_destructor (struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);

  if (!surface)
    return;

  /* Back to vsync with the next commit */
  set_pending_hint (surface, FALSE);
  surface->tearing.resource = NULL;
}

static void
tearing_control_set_presentation_hint (struct wl_client   *client,
                                       struct wl_resource *resource,
                                       uint32_t            hint)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);

  if (!surface)
    return;

  set_pending_hint (surface,
                    hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
}

static void
tearing_control_destroy (struct wl_client   *client,
                         struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wp_tearing_control_v1_interface tearing_control_implementation = {
  tearing_control_set_presentation_hint,
  tearing_control_destroy,
};

static void
tearing_control_manager_destroy (struct wl_client   *client,
                                 struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
tearing_control_manager_get_tearing_control (struct wl_client   *client,
                                             struct wl_resource *resource,
                                             uint32_t            id,
                                             struct wl_resource *surface_resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_resource *tearing_resource;

  if (surface->tearing.resource)
    {
      wl_resource_post_error (resource,
                              WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS,
                              "The surface already has a tearing control object");
      return;
    }

  tearing_resource = wl_resource_create (client, &wp_tearing_control_v1_interface,
                                         wl_resource_get_version (resource), id);
  if (!tearing_resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (tearing_resource,
                                  &tearing_control_implementation,
                                  surface, tearing_control_destructor);
  surface->tearing.resource = tearing_resource;
}

static const struct wp_tearing_control_manager_v1_interface tearing_control_manager_implementation = {
  tearing_control_manager_destroy,
  tearing_control_manager_get_tearing_control,
};

static void
tearing_control_manager_bind (struct wl_client *client,
                              void             *data,
                              uint32_t          version,
                              uint32_t          id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_tearing_control_manager_v1_interface,
                                 version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource,
                                  &tearing_control_manager_implementation,
                                  data, NULL);
}

void
meta_wayland_init_tearing_control (MetaWaylandCompositor *compositor)
{
  if (!wl_global_create (compositor->wayland_display,
                         &wp_tearing_control_manager_v1_interface,
                         META_WP_TEARING_CONTROL_V1_VERSION,
                         compositor, tearing_control_manager_bind))
    g_error ("Failed to register the global wp_tearing_control_manager_v1");
}
