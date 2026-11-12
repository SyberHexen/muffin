/*
 * Wayland Support
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

/*
 * A subset of wp_color_management_v1: parametric image descriptions only,
 * with the named primaries and transfer functions advertised below. Muffin
 * does no color conversion; the descriptions let clients tell it what their
 * content is (so an HDR10 surface can be shown on an output in the BT.2100
 * color mode as is), and let it tell clients what each output expects.
 */

#include "config.h"

#include <string.h>

#include "wayland/meta-wayland-color-management.h"

#include "backends/meta-logical-monitor.h"
#include "backends/meta-monitor.h"
#include "backends/meta-monitor-manager-private.h"
#include "backends/meta-output.h"
#include "core/window-private.h"
#include "wayland/meta-wayland-outputs.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-wayland-versions.h"

#include "color-management-v1-server-protocol.h"

struct _MetaWaylandColorDescription
{
  grefcount ref_count;
  uint64_t id;

  uint32_t tf;
  uint32_t primaries;

  /* Primary color volume, in cd/m² */
  double min_lum;
  double max_lum;
  double reference_lum;

  /* Target color volume (mastering display), when given */
  gboolean has_target_primaries;
  int32_t target_primaries[8];
  gboolean has_target_luminance;
  double target_min_lum;
  double target_max_lum;
  uint32_t max_cll;
  uint32_t max_fall;
};

typedef struct
{
  MetaWaylandCompositor *compositor;
  /* wp_color_management_output_v1 resources */
  GList *output_resources;
  /* wp_color_management_surface_feedback_v1 resources */
  GList *feedback_resources;
  MetaWaylandColorDescription *sdr_output;
  MetaWaylandColorDescription *hdr10_output;
} MetaWaylandColorManager;

typedef struct
{
  MetaWaylandColorManager *manager;
  struct wl_resource *resource;
  /* The wl_output it was created for; NULL once that is gone */
  struct wl_resource *output_resource;
  struct wl_listener output_destroy_listener;
  uint64_t sent_id;
} ColorManagementOutput;

typedef struct
{
  MetaWaylandColorDescription *description;
  /* Whether get_information is allowed on it */
  gboolean allow_information;
} ImageDescription;

typedef struct
{
  uint32_t tf;
  uint32_t primaries;
  gboolean has_luminances;
  double min_lum;
  double max_lum;
  double reference_lum;
  gboolean has_target_primaries;
  int32_t target_primaries[8];
  gboolean has_target_luminance;
  double target_min_lum;
  double target_max_lum;
  gboolean has_max_cll;
  uint32_t max_cll;
  gboolean has_max_fall;
  uint32_t max_fall;
} ParametricCreator;

static MetaWaylandColorManager *color_manager;
static uint64_t next_description_id = 1;

/* Chromaticities, x and y of red, green, blue and white, times 1000000 */
static const int32_t primaries_srgb[8] = {
  640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000,
};
static const int32_t primaries_display_p3[8] = {
  680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000,
};
static const int32_t primaries_bt2020[8] = {
  708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000,
};

static const int32_t *
get_named_primaries (uint32_t primaries)
{
  switch (primaries)
    {
    case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB:
      return primaries_srgb;
    case WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3:
      return primaries_display_p3;
    case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
      return primaries_bt2020;
    default:
      return NULL;
    }
}

static gboolean
is_supported_tf (uint32_t tf)
{
  switch (tf)
    {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB:
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
      return TRUE;
    default:
      return FALSE;
    }
}

/* The default luminances each transfer function implies */
static void
get_default_luminances (uint32_t  tf,
                        double   *min_lum,
                        double   *max_lum,
                        double   *reference_lum)
{
  switch (tf)
    {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
      *min_lum = 0.005;
      *max_lum = 10000.0;
      *reference_lum = 203.0;
      break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
      *min_lum = 0.01;
      *max_lum = 100.0;
      *reference_lum = 100.0;
      break;
    default:
      *min_lum = 0.2;
      *max_lum = 80.0;
      *reference_lum = 80.0;
      break;
    }
}

static MetaWaylandColorDescription *
color_description_new (uint32_t tf,
                       uint32_t primaries)
{
  MetaWaylandColorDescription *description;

  description = g_new0 (MetaWaylandColorDescription, 1);
  g_ref_count_init (&description->ref_count);
  description->id = next_description_id++;
  description->tf = tf;
  description->primaries = primaries;
  get_default_luminances (tf,
                          &description->min_lum,
                          &description->max_lum,
                          &description->reference_lum);

  return description;
}

MetaWaylandColorDescription *
meta_wayland_color_description_ref (MetaWaylandColorDescription *description)
{
  g_ref_count_inc (&description->ref_count);
  return description;
}

void
meta_wayland_color_description_unref (MetaWaylandColorDescription *description)
{
  if (g_ref_count_dec (&description->ref_count))
    g_free (description);
}

gboolean
meta_wayland_color_description_is_hdr10 (MetaWaylandColorDescription *description)
{
  return description &&
         description->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ &&
         description->primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
}

/* What an output expects, from the color mode of its main output */
static MetaWaylandColorDescription *
get_logical_monitor_description (MetaLogicalMonitor *logical_monitor)
{
  GList *monitors;

  if (!logical_monitor)
    return color_manager->sdr_output;

  monitors = meta_logical_monitor_get_monitors (logical_monitor);
  if (monitors)
    {
      MetaOutput *output = meta_monitor_get_main_output (monitors->data);

      if (output->color_mode == META_OUTPUT_COLOR_MODE_BT2100)
        return color_manager->hdr10_output;
    }

  return color_manager->sdr_output;
}

/* wp_image_description_info_v1 */

static void
send_information (struct wl_resource          *info_resource,
                  MetaWaylandColorDescription *description)
{
  const int32_t *p = get_named_primaries (description->primaries);
  const int32_t *t;

  wp_image_description_info_v1_send_primaries (info_resource,
                                               p[0], p[1], p[2], p[3],
                                               p[4], p[5], p[6], p[7]);
  wp_image_description_info_v1_send_primaries_named (info_resource,
                                                     description->primaries);
  wp_image_description_info_v1_send_tf_named (info_resource, description->tf);
  wp_image_description_info_v1_send_luminances (info_resource,
                                                (uint32_t) (description->min_lum * 10000.0 + 0.5),
                                                (uint32_t) (description->max_lum + 0.5),
                                                (uint32_t) (description->reference_lum + 0.5));

  t = description->has_target_primaries ? description->target_primaries : p;
  wp_image_description_info_v1_send_target_primaries (info_resource,
                                                      t[0], t[1], t[2], t[3],
                                                      t[4], t[5], t[6], t[7]);

  if (description->has_target_luminance)
    wp_image_description_info_v1_send_target_luminance (info_resource,
                                                        (uint32_t) (description->target_min_lum * 10000.0 + 0.5),
                                                        (uint32_t) (description->target_max_lum + 0.5));
  else
    wp_image_description_info_v1_send_target_luminance (info_resource,
                                                        (uint32_t) (description->min_lum * 10000.0 + 0.5),
                                                        (uint32_t) (description->max_lum + 0.5));

  if (description->max_cll)
    wp_image_description_info_v1_send_target_max_cll (info_resource,
                                                      description->max_cll);
  if (description->max_fall)
    wp_image_description_info_v1_send_target_max_fall (info_resource,
                                                       description->max_fall);

  wp_image_description_info_v1_send_done (info_resource);
  wl_resource_destroy (info_resource);
}

/* wp_image_description_v1 */

static void
image_description_destructor (struct wl_resource *resource)
{
  ImageDescription *image_description = wl_resource_get_user_data (resource);

  g_clear_pointer (&image_description->description,
                   meta_wayland_color_description_unref);
  g_free (image_description);
}

static void
image_description_destroy (struct wl_client   *client,
                           struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
image_description_get_information (struct wl_client   *client,
                                   struct wl_resource *resource,
                                   uint32_t            id)
{
  ImageDescription *image_description = wl_resource_get_user_data (resource);
  struct wl_resource *info_resource;

  if (!image_description->description)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY,
                              "image description is not ready");
      return;
    }

  if (!image_description->allow_information)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION,
                              "get_information is not allowed on this "
                              "image description");
      return;
    }

  info_resource = wl_resource_create (client,
                                      &wp_image_description_info_v1_interface,
                                      wl_resource_get_version (resource),
                                      id);
  send_information (info_resource, image_description->description);
}

static const struct wp_image_description_v1_interface image_description_interface = {
  .destroy = image_description_destroy,
  .get_information = image_description_get_information,
};

static struct wl_resource *
create_image_description (struct wl_client            *client,
                          int                          version,
                          uint32_t                     id,
                          MetaWaylandColorDescription *description,
                          gboolean                     allow_information)
{
  ImageDescription *image_description;
  struct wl_resource *resource;

  image_description = g_new0 (ImageDescription, 1);
  image_description->allow_information = allow_information;

  resource = wl_resource_create (client,
                                 &wp_image_description_v1_interface,
                                 version, id);
  wl_resource_set_implementation (resource,
                                  &image_description_interface,
                                  image_description,
                                  image_description_destructor);

  if (!description)
    {
      wp_image_description_v1_send_failed (resource,
                                           WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
                                           "unsupported image description");
      return resource;
    }

  image_description->description =
    meta_wayland_color_description_ref (description);

  if (version >= WP_IMAGE_DESCRIPTION_V1_READY2_SINCE_VERSION)
    wp_image_description_v1_send_ready2 (resource,
                                         description->id >> 32,
                                         description->id & 0xffffffff);
  else
    wp_image_description_v1_send_ready (resource,
                                        description->id & 0xffffffff);

  return resource;
}

/* wp_color_management_output_v1 */

static MetaLogicalMonitor *
get_cm_output_logical_monitor (ColorManagementOutput *cm_output)
{
  MetaWaylandOutput *wayland_output;

  if (!cm_output->output_resource)
    return NULL;

  wayland_output = wl_resource_get_user_data (cm_output->output_resource);
  if (!wayland_output)
    return NULL;

  return wayland_output->logical_monitor;
}

static void
cm_output_destructor (struct wl_resource *resource)
{
  ColorManagementOutput *cm_output = wl_resource_get_user_data (resource);

  if (cm_output->output_resource)
    wl_list_remove (&cm_output->output_destroy_listener.link);

  if (color_manager)
    color_manager->output_resources =
      g_list_remove (color_manager->output_resources, resource);

  g_free (cm_output);
}

static void
on_output_resource_destroyed (struct wl_listener *listener,
                              void               *data)
{
  ColorManagementOutput *cm_output =
    wl_container_of (listener, cm_output, output_destroy_listener);

  wl_list_remove (&cm_output->output_destroy_listener.link);
  cm_output->output_resource = NULL;
}

static void
cm_output_destroy (struct wl_client   *client,
                   struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
cm_output_get_image_description (struct wl_client   *client,
                                 struct wl_resource *resource,
                                 uint32_t            id)
{
  ColorManagementOutput *cm_output = wl_resource_get_user_data (resource);
  MetaWaylandColorDescription *description = NULL;

  /* An output that is gone has no description; tell the client so */
  if (cm_output->output_resource &&
      wl_resource_get_user_data (cm_output->output_resource))
    description =
      get_logical_monitor_description (get_cm_output_logical_monitor (cm_output));

  if (!description)
    {
      struct wl_resource *description_resource;

      description_resource =
        create_image_description (client, wl_resource_get_version (resource),
                                  id, NULL, FALSE);
      wp_image_description_v1_send_failed (description_resource,
                                           WP_IMAGE_DESCRIPTION_V1_CAUSE_NO_OUTPUT,
                                           "the output is gone");
      return;
    }

  cm_output->sent_id = description->id;
  create_image_description (client, wl_resource_get_version (resource),
                            id, description, TRUE);
}

static const struct wp_color_management_output_v1_interface cm_output_interface = {
  .destroy = cm_output_destroy,
  .get_image_description = cm_output_get_image_description,
};

/* wp_color_management_surface_v1 */

static void
cm_surface_destructor (struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);
  MetaWaylandSurfaceState *pending;

  if (!surface)
    return;

  /* Destroying the object unsets the description on the next commit */
  pending = meta_wayland_surface_get_pending_state (surface);
  g_clear_pointer (&pending->color_description,
                   meta_wayland_color_description_unref);
  pending->has_new_color_description = TRUE;

  surface->color_management.resource = NULL;
}

static void
cm_surface_destroy (struct wl_client   *client,
                    struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
cm_surface_set_image_description (struct wl_client   *client,
                                  struct wl_resource *resource,
                                  struct wl_resource *description_resource,
                                  uint32_t            render_intent)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);
  ImageDescription *image_description =
    wl_resource_get_user_data (description_resource);
  MetaWaylandSurfaceState *pending;

  if (!surface)
    {
      wl_resource_post_error (resource,
                              WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
                              "the wl_surface is gone");
      return;
    }

  if (render_intent != WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL)
    {
      wl_resource_post_error (resource,
                              WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT,
                              "unsupported render intent %u", render_intent);
      return;
    }

  if (!image_description->description)
    {
      wl_resource_post_error (resource,
                              WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION,
                              "the image description is not ready");
      return;
    }

  pending = meta_wayland_surface_get_pending_state (surface);
  g_clear_pointer (&pending->color_description,
                   meta_wayland_color_description_unref);
  pending->color_description =
    meta_wayland_color_description_ref (image_description->description);
  pending->has_new_color_description = TRUE;
}

static void
cm_surface_unset_image_description (struct wl_client   *client,
                                    struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);
  MetaWaylandSurfaceState *pending;

  if (!surface)
    {
      wl_resource_post_error (resource,
                              WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
                              "the wl_surface is gone");
      return;
    }

  pending = meta_wayland_surface_get_pending_state (surface);
  g_clear_pointer (&pending->color_description,
                   meta_wayland_color_description_unref);
  pending->has_new_color_description = TRUE;
}

static const struct wp_color_management_surface_v1_interface cm_surface_interface = {
  .destroy = cm_surface_destroy,
  .set_image_description = cm_surface_set_image_description,
  .unset_image_description = cm_surface_unset_image_description,
};

/* wp_color_management_surface_feedback_v1 */

static MetaWaylandColorDescription *
get_surface_preferred_description (MetaWaylandSurface *surface)
{
  return get_logical_monitor_description (
    meta_wayland_surface_get_preferred_scale_monitor (surface));
}

static void
cm_feedback_destructor (struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);

  if (color_manager)
    color_manager->feedback_resources =
      g_list_remove (color_manager->feedback_resources, resource);

  if (surface)
    surface->color_management.feedback_resources =
      g_list_remove (surface->color_management.feedback_resources, resource);
}

static void
cm_feedback_destroy (struct wl_client   *client,
                     struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
cm_feedback_get_preferred (struct wl_client   *client,
                           struct wl_resource *resource,
                           uint32_t            id)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);
  MetaWaylandColorDescription *description;

  if (!surface)
    {
      wl_resource_post_error (resource,
                              WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT,
                              "the wl_surface is gone");
      return;
    }

  description = get_surface_preferred_description (surface);
  surface->color_management.preferred_id = description->id;
  create_image_description (client, wl_resource_get_version (resource),
                            id, description, TRUE);
}

static const struct wp_color_management_surface_feedback_v1_interface cm_feedback_interface = {
  .destroy = cm_feedback_destroy,
  .get_preferred = cm_feedback_get_preferred,
  /* Every description here is parametric */
  .get_preferred_parametric = cm_feedback_get_preferred,
};

static void
send_preferred_changed (struct wl_resource          *resource,
                        MetaWaylandColorDescription *description)
{
  if (wl_resource_get_version (resource) >=
      WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_PREFERRED_CHANGED2_SINCE_VERSION)
    wp_color_management_surface_feedback_v1_send_preferred_changed2 (resource,
                                                                      description->id >> 32,
                                                                      description->id & 0xffffffff);
  else
    wp_color_management_surface_feedback_v1_send_preferred_changed (resource,
                                                                     description->id & 0xffffffff);
}

void
meta_wayland_color_management_update_surface (MetaWaylandSurface *surface)
{
  MetaWaylandColorDescription *description;
  GList *l;

  if (!color_manager || !surface->color_management.feedback_resources)
    return;

  description = get_surface_preferred_description (surface);
  if (description->id == surface->color_management.preferred_id)
    return;

  surface->color_management.preferred_id = description->id;
  for (l = surface->color_management.feedback_resources; l; l = l->next)
    send_preferred_changed (l->data, description);
}

void
meta_wayland_color_management_surface_destroyed (MetaWaylandSurface *surface)
{
  GList *l;

  if (surface->color_management.resource)
    {
      wl_resource_set_user_data (surface->color_management.resource, NULL);
      surface->color_management.resource = NULL;
    }

  for (l = surface->color_management.feedback_resources; l; l = l->next)
    wl_resource_set_user_data (l->data, NULL);
  g_clear_pointer (&surface->color_management.feedback_resources, g_list_free);

  g_clear_pointer (&surface->color_management.description,
                   meta_wayland_color_description_unref);
}

/* wp_image_description_creator_params_v1 */

static void
creator_params_destructor (struct wl_resource *resource)
{
  g_free (wl_resource_get_user_data (resource));
}

static void
creator_params_create (struct wl_client   *client,
                       struct wl_resource *resource,
                       uint32_t            id)
{
  ParametricCreator *creator = wl_resource_get_user_data (resource);
  g_autoptr (MetaWaylandColorDescription) description = NULL;

  if (!creator->tf || !creator->primaries)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
                              "a transfer function and primaries are required");
      return;
    }

  description = color_description_new (creator->tf, creator->primaries);

  if (creator->has_luminances)
    {
      description->min_lum = creator->min_lum;
      description->max_lum = creator->max_lum;
      description->reference_lum = creator->reference_lum;

      /* PQ always reaches 10000 cd/m² above its minimum */
      if (creator->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)
        description->max_lum = creator->min_lum + 10000.0;
    }

  if (creator->has_target_primaries)
    {
      description->has_target_primaries = TRUE;
      memcpy (description->target_primaries, creator->target_primaries,
              sizeof (description->target_primaries));
    }

  if (creator->has_target_luminance)
    {
      description->has_target_luminance = TRUE;
      description->target_min_lum = creator->target_min_lum;
      description->target_max_lum = creator->target_max_lum;
    }

  description->max_cll = creator->has_max_cll ? creator->max_cll : 0;
  description->max_fall = creator->has_max_fall ? creator->max_fall : 0;

  create_image_description (client, wl_resource_get_version (resource),
                            id, description, FALSE);
  wl_resource_destroy (resource);
}

static void
creator_params_set_tf_named (struct wl_client   *client,
                             struct wl_resource *resource,
                             uint32_t            tf)
{
  ParametricCreator *creator = wl_resource_get_user_data (resource);

  if (creator->tf)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                              "the transfer function is already set");
      return;
    }

  if (!is_supported_tf (tf))
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
                              "unsupported transfer function %u", tf);
      return;
    }

  creator->tf = tf;
}

static void
creator_params_set_tf_power (struct wl_client   *client,
                             struct wl_resource *resource,
                             uint32_t            eexp)
{
  wl_resource_post_error (resource,
                          WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
                          "set_tf_power is not supported");
}

static void
creator_params_set_primaries_named (struct wl_client   *client,
                                    struct wl_resource *resource,
                                    uint32_t            primaries)
{
  ParametricCreator *creator = wl_resource_get_user_data (resource);

  if (creator->primaries)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                              "the primaries are already set");
      return;
    }

  if (!get_named_primaries (primaries))
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
                              "unsupported primaries %u", primaries);
      return;
    }

  creator->primaries = primaries;
}

static void
creator_params_set_primaries (struct wl_client   *client,
                              struct wl_resource *resource,
                              int32_t             r_x,
                              int32_t             r_y,
                              int32_t             g_x,
                              int32_t             g_y,
                              int32_t             b_x,
                              int32_t             b_y,
                              int32_t             w_x,
                              int32_t             w_y)
{
  wl_resource_post_error (resource,
                          WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
                          "set_primaries is not supported");
}

static void
creator_params_set_luminances (struct wl_client   *client,
                               struct wl_resource *resource,
                               uint32_t            min_lum,
                               uint32_t            max_lum,
                               uint32_t            reference_lum)
{
  ParametricCreator *creator = wl_resource_get_user_data (resource);
  double min = min_lum / 10000.0;

  if (creator->has_luminances)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                              "the luminances are already set");
      return;
    }

  if (max_lum <= min || reference_lum <= min)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                              "max and reference luminance must exceed the "
                              "minimum");
      return;
    }

  creator->has_luminances = TRUE;
  creator->min_lum = min;
  creator->max_lum = max_lum;
  creator->reference_lum = reference_lum;
}

static void
creator_params_set_mastering_display_primaries (struct wl_client   *client,
                                                struct wl_resource *resource,
                                                int32_t             r_x,
                                                int32_t             r_y,
                                                int32_t             g_x,
                                                int32_t             g_y,
                                                int32_t             b_x,
                                                int32_t             b_y,
                                                int32_t             w_x,
                                                int32_t             w_y)
{
  ParametricCreator *creator = wl_resource_get_user_data (resource);

  if (creator->has_target_primaries)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                              "the mastering display primaries are already set");
      return;
    }

  creator->has_target_primaries = TRUE;
  creator->target_primaries[0] = r_x;
  creator->target_primaries[1] = r_y;
  creator->target_primaries[2] = g_x;
  creator->target_primaries[3] = g_y;
  creator->target_primaries[4] = b_x;
  creator->target_primaries[5] = b_y;
  creator->target_primaries[6] = w_x;
  creator->target_primaries[7] = w_y;
}

static void
creator_params_set_mastering_luminance (struct wl_client   *client,
                                        struct wl_resource *resource,
                                        uint32_t            min_lum,
                                        uint32_t            max_lum)
{
  ParametricCreator *creator = wl_resource_get_user_data (resource);
  double min = min_lum / 10000.0;

  if (creator->has_target_luminance)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                              "the mastering luminance is already set");
      return;
    }

  if (max_lum <= min)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                              "max luminance must exceed the minimum");
      return;
    }

  creator->has_target_luminance = TRUE;
  creator->target_min_lum = min;
  creator->target_max_lum = max_lum;
}

static void
creator_params_set_max_cll (struct wl_client   *client,
                            struct wl_resource *resource,
                            uint32_t            max_cll)
{
  ParametricCreator *creator = wl_resource_get_user_data (resource);

  if (creator->has_max_cll)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                              "max_cll is already set");
      return;
    }

  creator->has_max_cll = TRUE;
  creator->max_cll = max_cll;
}

static void
creator_params_set_max_fall (struct wl_client   *client,
                             struct wl_resource *resource,
                             uint32_t            max_fall)
{
  ParametricCreator *creator = wl_resource_get_user_data (resource);

  if (creator->has_max_fall)
    {
      wl_resource_post_error (resource,
                              WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                              "max_fall is already set");
      return;
    }

  creator->has_max_fall = TRUE;
  creator->max_fall = max_fall;
}

static const struct wp_image_description_creator_params_v1_interface creator_params_interface = {
  .create = creator_params_create,
  .set_tf_named = creator_params_set_tf_named,
  .set_tf_power = creator_params_set_tf_power,
  .set_primaries_named = creator_params_set_primaries_named,
  .set_primaries = creator_params_set_primaries,
  .set_luminances = creator_params_set_luminances,
  .set_mastering_display_primaries = creator_params_set_mastering_display_primaries,
  .set_mastering_luminance = creator_params_set_mastering_luminance,
  .set_max_cll = creator_params_set_max_cll,
  .set_max_fall = creator_params_set_max_fall,
};

/* wp_color_manager_v1 */

static void
color_manager_destroy (struct wl_client   *client,
                       struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
color_manager_get_output (struct wl_client   *client,
                          struct wl_resource *resource,
                          uint32_t            id,
                          struct wl_resource *output_resource)
{
  ColorManagementOutput *cm_output;

  cm_output = g_new0 (ColorManagementOutput, 1);
  cm_output->manager = color_manager;
  cm_output->resource = wl_resource_create (client,
                                            &wp_color_management_output_v1_interface,
                                            wl_resource_get_version (resource),
                                            id);
  wl_resource_set_implementation (cm_output->resource,
                                  &cm_output_interface,
                                  cm_output,
                                  cm_output_destructor);

  cm_output->output_resource = output_resource;
  cm_output->output_destroy_listener.notify = on_output_resource_destroyed;
  wl_resource_add_destroy_listener (output_resource,
                                    &cm_output->output_destroy_listener);

  color_manager->output_resources =
    g_list_prepend (color_manager->output_resources, cm_output->resource);
}

static void
color_manager_get_surface (struct wl_client   *client,
                           struct wl_resource *resource,
                           uint32_t            id,
                           struct wl_resource *surface_resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_resource *cm_surface_resource;

  if (surface->color_management.resource)
    {
      wl_resource_post_error (resource,
                              WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
                              "the surface already has a color management "
                              "object");
      return;
    }

  cm_surface_resource =
    wl_resource_create (client,
                        &wp_color_management_surface_v1_interface,
                        wl_resource_get_version (resource),
                        id);
  wl_resource_set_implementation (cm_surface_resource,
                                  &cm_surface_interface,
                                  surface,
                                  cm_surface_destructor);

  surface->color_management.resource = cm_surface_resource;
}

static void
color_manager_get_surface_feedback (struct wl_client   *client,
                                    struct wl_resource *resource,
                                    uint32_t            id,
                                    struct wl_resource *surface_resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_resource *feedback_resource;

  feedback_resource =
    wl_resource_create (client,
                        &wp_color_management_surface_feedback_v1_interface,
                        wl_resource_get_version (resource),
                        id);
  wl_resource_set_implementation (feedback_resource,
                                  &cm_feedback_interface,
                                  surface,
                                  cm_feedback_destructor);

  surface->color_management.feedback_resources =
    g_list_prepend (surface->color_management.feedback_resources,
                    feedback_resource);
  color_manager->feedback_resources =
    g_list_prepend (color_manager->feedback_resources, feedback_resource);

  if (!surface->color_management.preferred_id)
    surface->color_management.preferred_id =
      get_surface_preferred_description (surface)->id;
}

static void
color_manager_create_icc_creator (struct wl_client   *client,
                                  struct wl_resource *resource,
                                  uint32_t            id)
{
  wl_resource_post_error (resource,
                          WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
                          "ICC profiles are not supported");
}

static void
color_manager_create_parametric_creator (struct wl_client   *client,
                                         struct wl_resource *resource,
                                         uint32_t            id)
{
  struct wl_resource *creator_resource;

  creator_resource =
    wl_resource_create (client,
                        &wp_image_description_creator_params_v1_interface,
                        wl_resource_get_version (resource),
                        id);
  wl_resource_set_implementation (creator_resource,
                                  &creator_params_interface,
                                  g_new0 (ParametricCreator, 1),
                                  creator_params_destructor);
}

static void
color_manager_create_windows_scrgb (struct wl_client   *client,
                                    struct wl_resource *resource,
                                    uint32_t            id)
{
  wl_resource_post_error (resource,
                          WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
                          "windows_scrgb is not supported");
}

static void
color_manager_get_image_description (struct wl_client   *client,
                                     struct wl_resource *resource,
                                     uint32_t            id,
                                     struct wl_resource *reference)
{
  /* No interface Muffin implements hands out references */
  create_image_description (client, wl_resource_get_version (resource),
                            id, NULL, FALSE);
}

static const struct wp_color_manager_v1_interface color_manager_interface = {
  .destroy = color_manager_destroy,
  .get_output = color_manager_get_output,
  .get_surface = color_manager_get_surface,
  .get_surface_feedback = color_manager_get_surface_feedback,
  .create_icc_creator = color_manager_create_icc_creator,
  .create_parametric_creator = color_manager_create_parametric_creator,
  .create_windows_scrgb = color_manager_create_windows_scrgb,
  .get_image_description = color_manager_get_image_description,
};

static void
color_manager_bind (struct wl_client *client,
                    void             *data,
                    uint32_t          version,
                    uint32_t          id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_color_manager_v1_interface,
                                 version, id);
  wl_resource_set_implementation (resource, &color_manager_interface,
                                  data, NULL);

  wp_color_manager_v1_send_supported_intent (resource,
                                             WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);

  wp_color_manager_v1_send_supported_feature (resource,
                                              WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC);
  wp_color_manager_v1_send_supported_feature (resource,
                                              WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES);
  wp_color_manager_v1_send_supported_feature (resource,
                                              WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES);

  wp_color_manager_v1_send_supported_tf_named (resource,
                                               WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
  wp_color_manager_v1_send_supported_tf_named (resource,
                                               WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
  wp_color_manager_v1_send_supported_tf_named (resource,
                                               WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886);
  wp_color_manager_v1_send_supported_tf_named (resource,
                                               WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);

  wp_color_manager_v1_send_supported_primaries_named (resource,
                                                      WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
  wp_color_manager_v1_send_supported_primaries_named (resource,
                                                      WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3);
  wp_color_manager_v1_send_supported_primaries_named (resource,
                                                      WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);

  wp_color_manager_v1_send_done (resource);
}

static void
on_monitors_changed (MetaMonitorManager *monitor_manager,
                     gpointer            user_data)
{
  GList *l;

  for (l = color_manager->output_resources; l; l = l->next)
    {
      ColorManagementOutput *cm_output = wl_resource_get_user_data (l->data);
      MetaWaylandColorDescription *description;

      if (!cm_output->sent_id)
        continue;

      description =
        get_logical_monitor_description (get_cm_output_logical_monitor (cm_output));
      if (description->id == cm_output->sent_id)
        continue;

      cm_output->sent_id = description->id;
      wp_color_management_output_v1_send_image_description_changed (l->data);
    }

  for (l = color_manager->feedback_resources; l; l = l->next)
    {
      MetaWaylandSurface *surface = wl_resource_get_user_data (l->data);

      if (surface)
        meta_wayland_color_management_update_surface (surface);
    }
}

/* HDR passthrough */

/* Delays before switching, so a mode switch (which blanks the monitor) is not
 * started by a window only briefly fullscreen, or undone while the views are
 * rebuilt for the previous switch */
#define PASSTHROUGH_ON_DELAY_MS 300
#define PASSTHROUGH_OFF_DELAY_MS 1500

typedef struct
{
  gboolean enabled;
  /* connector -> whether a fullscreen window there shows HDR10 */
  GHashTable *wanted;
  /* Connectors Muffin switched to BT.2100 itself, and so may switch back */
  GHashTable *switched;
  /* Connectors where switching failed; not retried until wanted again */
  GHashTable *refused;
  guint timeout_id;
} Passthrough;

static Passthrough passthrough;

static gboolean
surface_tree_is_hdr10 (MetaWaylandSurface *surface)
{
  MetaWaylandSurface *subsurface;

  if (meta_wayland_color_description_is_hdr10 (surface->color_management.description))
    return TRUE;

  META_WAYLAND_SURFACE_FOREACH_SUBSURFACE (surface, subsurface)
    {
      if (surface_tree_is_hdr10 (subsurface))
        return TRUE;
    }

  return FALSE;
}

static gboolean
monitor_supports_bt2100 (MetaMonitor *monitor)
{
  return (meta_monitor_get_supported_color_modes (monitor) &
          (1u << META_OUTPUT_COLOR_MODE_BT2100)) != 0;
}

static MetaMonitorManager *
get_monitor_manager (void)
{
  return meta_backend_get_monitor_manager (meta_get_backend ());
}

static gboolean
apply_passthrough (gpointer user_data)
{
  MetaMonitorManager *monitor_manager = get_monitor_manager ();
  g_autoptr (GList) keys = NULL;
  GList *connectors, *l;

  passthrough.timeout_id = 0;

  /* Applying a configuration can re-enter through monitors-changed */
  keys = g_hash_table_get_keys (passthrough.wanted);
  connectors = g_list_copy_deep (keys, (GCopyFunc) g_strdup, NULL);

  for (l = connectors; l; l = l->next)
    {
      const char *connector = l->data;
      g_autoptr (GError) error = NULL;
      MetaMonitor *monitor;
      MetaOutputColorMode current, target;
      gboolean wanted;

      if (!g_hash_table_lookup_extended (passthrough.wanted, connector,
                                         NULL, (gpointer *) &wanted))
        continue;

      monitor = meta_monitor_manager_get_monitor_from_connector (monitor_manager,
                                                                 connector);
      if (!monitor)
        continue;

      current = meta_monitor_get_color_mode (monitor);

      /* Switched back by someone else: no longer ours to undo */
      if (current == META_OUTPUT_COLOR_MODE_DEFAULT)
        g_hash_table_remove (passthrough.switched, connector);

      if (wanted && current == META_OUTPUT_COLOR_MODE_DEFAULT &&
          !g_hash_table_contains (passthrough.refused, connector))
        target = META_OUTPUT_COLOR_MODE_BT2100;
      else if (!wanted && current == META_OUTPUT_COLOR_MODE_BT2100 &&
               g_hash_table_contains (passthrough.switched, connector))
        target = META_OUTPUT_COLOR_MODE_DEFAULT;
      else
        continue;

      g_message ("HDR passthrough: switching %s to %s", connector,
                 target == META_OUTPUT_COLOR_MODE_BT2100 ?
                 "BT.2100 for a fullscreen HDR10 window" : "SDR");

      if (!meta_monitor_manager_set_monitor_color_mode (monitor_manager,
                                                        monitor, target,
                                                        &error))
        {
          g_warning ("HDR passthrough: could not switch %s: %s",
                     connector, error->message);
          if (target == META_OUTPUT_COLOR_MODE_BT2100)
            g_hash_table_add (passthrough.refused, g_strdup (connector));
          continue;
        }

      if (target == META_OUTPUT_COLOR_MODE_BT2100)
        g_hash_table_add (passthrough.switched, g_strdup (connector));
      else
        g_hash_table_remove (passthrough.switched, connector);
    }

  g_list_free_full (connectors, g_free);

  return G_SOURCE_REMOVE;
}

void
meta_wayland_color_management_update_fullscreen (GHashTable *fullscreen_windows)
{
  MetaMonitorManager *monitor_manager;
  GList *logical_monitors, *l, *k;
  gboolean switch_on = FALSE, switch_off = FALSE;

  if (!passthrough.enabled)
    return;

  monitor_manager = get_monitor_manager ();
  g_hash_table_remove_all (passthrough.wanted);

  logical_monitors =
    meta_monitor_manager_get_logical_monitors (monitor_manager);
  for (l = logical_monitors; l; l = l->next)
    {
      MetaLogicalMonitor *logical_monitor = l->data;
      MetaWindow *window = g_hash_table_lookup (fullscreen_windows,
                                                logical_monitor);
      gboolean wanted;

      wanted = window && window->surface &&
               surface_tree_is_hdr10 (window->surface);

      for (k = meta_logical_monitor_get_monitors (logical_monitor); k; k = k->next)
        {
          MetaMonitor *monitor = k->data;
          const char *connector = meta_monitor_get_connector (monitor);
          MetaOutputColorMode current;

          if (!monitor_supports_bt2100 (monitor))
            continue;

          g_hash_table_insert (passthrough.wanted, g_strdup (connector),
                               GINT_TO_POINTER (wanted));
          if (!wanted)
            g_hash_table_remove (passthrough.refused, connector);

          current = meta_monitor_get_color_mode (monitor);
          if (wanted && current == META_OUTPUT_COLOR_MODE_DEFAULT &&
              !g_hash_table_contains (passthrough.refused, connector))
            switch_on = TRUE;
          else if (!wanted && current == META_OUTPUT_COLOR_MODE_BT2100 &&
                   g_hash_table_contains (passthrough.switched, connector))
            switch_off = TRUE;
        }
    }

  g_clear_handle_id (&passthrough.timeout_id, g_source_remove);

  if (switch_on || switch_off)
    {
      passthrough.timeout_id =
        g_timeout_add (switch_on ? PASSTHROUGH_ON_DELAY_MS :
                                   PASSTHROUGH_OFF_DELAY_MS,
                       apply_passthrough, NULL);
      g_source_set_name_by_id (passthrough.timeout_id,
                               "[muffin] HDR passthrough");
    }
}

void
meta_wayland_init_color_management (MetaWaylandCompositor *compositor)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);

  if (g_getenv ("MUFFIN_DEBUG_DISABLE_COLOR_MANAGEMENT"))
    {
      g_message ("wp_color_manager_v1 disabled by "
                 "MUFFIN_DEBUG_DISABLE_COLOR_MANAGEMENT");
      return;
    }

  color_manager = g_new0 (MetaWaylandColorManager, 1);
  color_manager->compositor = compositor;

  color_manager->sdr_output =
    color_description_new (WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
                           WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);

  /* Matches the HDR_OUTPUT_METADATA Muffin sends in the BT.2100 mode */
  color_manager->hdr10_output =
    color_description_new (WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
                           WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
  color_manager->hdr10_output->has_target_luminance = TRUE;
  color_manager->hdr10_output->target_min_lum = 0.005;
  color_manager->hdr10_output->target_max_lum = 1000.0;
  color_manager->hdr10_output->max_cll = 1000;
  color_manager->hdr10_output->max_fall = 400;

  passthrough.enabled = !g_getenv ("MUFFIN_DEBUG_DISABLE_HDR_PASSTHROUGH");
  passthrough.wanted = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, NULL);
  passthrough.switched = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, NULL);
  passthrough.refused = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, NULL);

  /* After the wl_outputs, which are updated on the same signal */
  g_signal_connect (monitor_manager, "monitors-changed-internal",
                    G_CALLBACK (on_monitors_changed), NULL);

  if (wl_global_create (compositor->wayland_display,
                        &wp_color_manager_v1_interface,
                        META_WP_COLOR_MANAGEMENT_V1_VERSION,
                        color_manager,
                        color_manager_bind) == NULL)
    g_error ("Failed to register a global wp_color_manager_v1 object");
}
