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
 * wp_color_representation_v1: the matrix coefficients and range a client
 * encoded its YCbCr buffers with (BT.601, BT.709 or BT.2020; full or
 * limited range), used to convert them to RGB. Only the default alpha mode
 * (premultiplied electrical) is offered, and chroma locations are accepted
 * but not used (chroma is sampled bilinearly).
 */

#include "config.h"

#include "wayland/meta-wayland-color-representation.h"

#include "wayland/meta-wayland-buffer.h"
#include "wayland/meta-wayland-dma-buf.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-wayland-versions.h"

#include "color-representation-v1-server-protocol.h"

static const uint32_t supported_coefficients[] = {
  WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601,
  WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
  WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020,
};

static gboolean
is_supported_coefficients (uint32_t coefficients)
{
  size_t i;

  for (i = 0; i < G_N_ELEMENTS (supported_coefficients); i++)
    {
      if (supported_coefficients[i] == coefficients)
        return TRUE;
    }

  return FALSE;
}

static MetaWaylandSurface *
surface_from_resource (struct wl_resource *resource)
{
  return wl_resource_get_user_data (resource);
}

static gboolean
check_inert (struct wl_resource *resource)
{
  if (surface_from_resource (resource))
    return TRUE;

  wl_resource_post_error (resource,
                          WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
                          "The surface was destroyed");
  return FALSE;
}

static void
set_pending (MetaWaylandSurface *surface,
             uint32_t            coefficients,
             uint32_t            range)
{
  MetaWaylandSurfaceState *pending = meta_wayland_surface_get_pending_state (surface);

  pending->has_new_color_representation = TRUE;
  pending->color_coefficients = coefficients;
  pending->color_range = range;
}

static void
representation_surface_destructor (struct wl_resource *resource)
{
  MetaWaylandSurface *surface = surface_from_resource (resource);

  if (!surface)
    return;

  /* Unsetting is double-buffered */
  set_pending (surface, 0, 0);
  surface->color_representation.resource = NULL;
}

static void
representation_surface_destroy (struct wl_client   *client,
                                struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
representation_surface_set_alpha_mode (struct wl_client   *client,
                                       struct wl_resource *resource,
                                       uint32_t            alpha_mode)
{
  if (!check_inert (resource))
    return;

  if (alpha_mode != WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL)
    wl_resource_post_error (resource,
                            WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE,
                            "Unsupported alpha mode %u", alpha_mode);
}

static void
representation_surface_set_coefficients_and_range (struct wl_client   *client,
                                                   struct wl_resource *resource,
                                                   uint32_t            coefficients,
                                                   uint32_t            range)
{
  MetaWaylandSurface *surface;

  if (!check_inert (resource))
    return;

  if (!is_supported_coefficients (coefficients) ||
      (range != WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL &&
       range != WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED))
    {
      wl_resource_post_error (resource,
                              WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
                              "Unsupported coefficients %u with range %u",
                              coefficients, range);
      return;
    }

  surface = surface_from_resource (resource);
  set_pending (surface, coefficients, range);
}

static void
representation_surface_set_chroma_location (struct wl_client   *client,
                                            struct wl_resource *resource,
                                            uint32_t            chroma_location)
{
  if (!check_inert (resource))
    return;

  if (chroma_location < WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_0 ||
      chroma_location > WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_5)
    wl_resource_post_error (resource,
                            WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_CHROMA_LOCATION,
                            "Invalid chroma location %u", chroma_location);
}

static const struct wp_color_representation_surface_v1_interface
  representation_surface_implementation = {
  representation_surface_destroy,
  representation_surface_set_alpha_mode,
  representation_surface_set_coefficients_and_range,
  representation_surface_set_chroma_location,
};

static void
manager_destroy (struct wl_client   *client,
                 struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
manager_get_surface (struct wl_client   *client,
                     struct wl_resource *resource,
                     uint32_t            id,
                     struct wl_resource *surface_resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_resource *representation_resource;

  if (surface->color_representation.resource)
    {
      wl_resource_post_error (resource,
                              WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS,
                              "The surface already has a color representation object");
      return;
    }

  representation_resource =
    wl_resource_create (client, &wp_color_representation_surface_v1_interface,
                        wl_resource_get_version (resource), id);
  wl_resource_set_implementation (representation_resource,
                                  &representation_surface_implementation,
                                  surface,
                                  representation_surface_destructor);
  surface->color_representation.resource = representation_resource;
}

static const struct wp_color_representation_manager_v1_interface
  manager_implementation = {
  manager_destroy,
  manager_get_surface,
};

static void
manager_bind (struct wl_client *client,
              void             *data,
              uint32_t          version,
              uint32_t          id)
{
  struct wl_resource *resource;
  size_t i;

  resource = wl_resource_create (client,
                                 &wp_color_representation_manager_v1_interface,
                                 version, id);
  wl_resource_set_implementation (resource, &manager_implementation,
                                  data, NULL);

  wp_color_representation_manager_v1_send_supported_alpha_mode (
    resource,
    WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL);

  for (i = 0; i < G_N_ELEMENTS (supported_coefficients); i++)
    {
      wp_color_representation_manager_v1_send_supported_coefficients_and_ranges (
        resource, supported_coefficients[i],
        WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
      wp_color_representation_manager_v1_send_supported_coefficients_and_ranges (
        resource, supported_coefficients[i],
        WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);
    }

  wp_color_representation_manager_v1_send_done (resource);
}

void
meta_wayland_init_color_representation (MetaWaylandCompositor *compositor)
{
  if (!wl_global_create (compositor->wayland_display,
                         &wp_color_representation_manager_v1_interface,
                         META_WP_COLOR_REPRESENTATION_V1_VERSION,
                         compositor, manager_bind))
    g_error ("Failed to register a global color representation object");
}

/* The H.273 luma weights of the coefficients */
static void
coefficients_to_weights (uint32_t  coefficients,
                         float    *kr,
                         float    *kb)
{
  switch (coefficients)
    {
    case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601:
      *kr = 0.299f;
      *kb = 0.114f;
      return;
    case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020:
      *kr = 0.2627f;
      *kb = 0.0593f;
      return;
    case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709:
    default:
      *kr = 0.2126f;
      *kb = 0.0722f;
      return;
    }
}

gboolean
meta_wayland_color_representation_commit (MetaWaylandSurface      *surface,
                                          MetaWaylandSurfaceState *state)
{
  MetaWaylandBuffer *buffer = state->newly_attached ? state->buffer : NULL;
  uint32_t coefficients;
  float kr, kb;

  if (state->has_new_color_representation)
    {
      surface->color_representation.coefficients = state->color_coefficients;
      surface->color_representation.range = state->color_range;
    }

  coefficients = surface->color_representation.coefficients;
  if (!buffer)
    return TRUE;

  if (!meta_wayland_dma_buf_buffer_is_yuv (buffer))
    {
      /* None of the offered coefficients apply to RGB */
      if (coefficients && surface->color_representation.resource)
        {
          wl_resource_post_error (surface->color_representation.resource,
                                  WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT,
                                  "Coefficients %u set for an RGB buffer",
                                  coefficients);
          return FALSE;
        }
      return TRUE;
    }

  if (!coefficients)
    {
      meta_wayland_dma_buf_buffer_set_yuv_conversion (buffer, FALSE, 0.0f, 0.0f,
                                                      FALSE);
      return TRUE;
    }

  coefficients_to_weights (coefficients, &kr, &kb);
  meta_wayland_dma_buf_buffer_set_yuv_conversion (
    buffer, TRUE, kr, kb,
    surface->color_representation.range ==
      WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
  return TRUE;
}

void
meta_wayland_color_representation_surface_destroyed (MetaWaylandSurface *surface)
{
  if (surface->color_representation.resource)
    {
      wl_resource_set_user_data (surface->color_representation.resource, NULL);
      surface->color_representation.resource = NULL;
    }
}
