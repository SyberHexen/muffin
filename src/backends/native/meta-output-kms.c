/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2013-2017 Red Hat
 * Copyright (C) 2018 DisplayLink (UK) Ltd.
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

#include "config.h"

#include "backends/native/meta-output-kms.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "backends/meta-color-transform.h"
#include "backends/meta-crtc.h"
#include "backends/native/meta-kms-connector.h"
#include "backends/native/meta-kms-crtc.h"
#include "backends/native/meta-kms-device.h"
#include "backends/native/meta-kms-utils.h"
#include "backends/native/meta-crtc-kms.h"

#include "meta-default-modes.h"

#define SYNC_TOLERANCE 0.01    /* 1 percent */

typedef struct _MetaOutputKms
{
  MetaOutput parent;

  MetaKmsConnector *kms_connector;
} MetaOutputKms;

/**
 * meta_output_kms_get_gamma: (skip)
 *
 * The gamma ramp for @output's CRTC: the one clients set (Night Light), or a
 * neutral one, with the output's color correction applied on top.  FALSE if
 * there is nothing to apply.  The ramps are newly allocated.
 */
gboolean
meta_output_kms_get_gamma (MetaOutput  *output,
                           int         *size_out,
                           uint16_t   **red_out,
                           uint16_t   **green_out,
                           uint16_t   **blue_out)
{
  MetaCrtc *crtc = meta_output_get_assigned_crtc (output);
  MetaKmsCrtc *kms_crtc;
  const uint16_t *red, *green, *blue;
  gboolean have_wanted;
  int size, i;

  if (!crtc)
    return FALSE;

  kms_crtc = meta_crtc_kms_get_kms_crtc (crtc);
  have_wanted = meta_kms_crtc_get_wanted_gamma (kms_crtc, &size,
                                                &red, &green, &blue);
  if (!have_wanted)
    {
      if (meta_color_correction_is_neutral (&output->color_correction))
        return FALSE;
      size = meta_kms_crtc_get_current_state (kms_crtc)->gamma.size;
      if (size < 2)
        size = 256;
    }

  *size_out = size;
  *red_out = g_new (uint16_t, size);
  *green_out = g_new (uint16_t, size);
  *blue_out = g_new (uint16_t, size);
  for (i = 0; i < size; i++)
    {
      if (have_wanted)
        {
          (*red_out)[i] = red[i];
          (*green_out)[i] = green[i];
          (*blue_out)[i] = blue[i];
        }
      else
        {
          (*red_out)[i] = (*green_out)[i] = (*blue_out)[i] =
            (uint16_t) ((i * 65535) / (size - 1));
        }
    }

  meta_color_correction_apply (&output->color_correction, size,
                               *red_out, *green_out, *blue_out);
  return TRUE;
}

MetaKmsConnector *
meta_output_kms_get_kms_connector (MetaOutput *output)
{
  MetaOutputKms *output_kms = output->driver_private;

  return output_kms->kms_connector;
}

void
meta_output_kms_set_underscan (MetaOutput    *output,
                               MetaKmsUpdate *kms_update)
{
  MetaOutputKms *output_kms = output->driver_private;

  if (!output->supports_underscanning)
    return;

  if (output->is_underscanning)
    {
      MetaCrtc *crtc;
      MetaCrtcConfig *crtc_config;
      uint64_t hborder, vborder;

      crtc = meta_output_get_assigned_crtc (output);
      crtc_config = crtc->config;
      hborder = MIN (128, (uint64_t) round (crtc_config->mode->width * 0.05));
      vborder = MIN (128, (uint64_t) round (crtc_config->mode->height * 0.05));

      g_debug ("Setting underscan of connector %s to %" G_GUINT64_FORMAT " x %" G_GUINT64_FORMAT,
               meta_kms_connector_get_name (output_kms->kms_connector),
               hborder, vborder);

      meta_kms_connector_set_underscanning (output_kms->kms_connector,
                                            kms_update,
                                            hborder,
                                            vborder);
    }
  else
    {
      g_debug ("Unsetting underscan of connector %s",
               meta_kms_connector_get_name (output_kms->kms_connector));

      meta_kms_connector_unset_underscanning (output_kms->kms_connector,
                                              kms_update);
    }
}

/* Test switch: MUFFIN_SDR_EDID_COLORS=1 shows SDR colors on a wide gamut
 * display in their own (BT.709) colors, converted to the display's primaries
 * from its EDID, instead of letting the display stretch them over its whole
 * gamut */
gboolean
meta_output_kms_wants_sdr_colors_mapped (MetaOutput *output)
{
  static const double bt709[6] = { 0.64, 0.33, 0.30, 0.60, 0.15, 0.06 };
  static int enabled = -1;
  int i;

  if (enabled < 0)
    enabled = g_strcmp0 (g_getenv ("MUFFIN_SDR_EDID_COLORS"), "1") == 0;

  if (!enabled ||
      output->color_mode == META_OUTPUT_COLOR_MODE_BT2100 ||
      !output->has_edid_primaries)
    return FALSE;

  /* A display with (about) BT.709 primaries needs no conversion */
  for (i = 0; i < 6; i++)
    {
      if (fabs (output->edid_primaries[i] - bt709[i]) > 0.01)
        return TRUE;
    }

  return FALSE;
}

/* Whether SDR colors are converted to the display's primaries by its CRTC's
 * color pipeline, so the view needs no offscreen framebuffer and can be
 * scanned out directly; MUFFIN_DEBUG_SDR_COLORS_SHADER=1 converts them in
 * the view as before, for comparisons */
gboolean
meta_output_kms_maps_sdr_colors_in_hardware (MetaOutput *output)
{
  static int use_shader = -1;
  MetaCrtc *crtc;

  if (use_shader < 0)
    use_shader = g_strcmp0 (g_getenv ("MUFFIN_DEBUG_SDR_COLORS_SHADER"), "1") == 0;

  if (use_shader || !meta_output_kms_wants_sdr_colors_mapped (output))
    return FALSE;

  crtc = meta_output_get_assigned_crtc (output);
  return crtc &&
         meta_kms_crtc_supports_color_pipeline (meta_crtc_kms_get_kms_crtc (crtc));
}

void
meta_output_kms_set_color_properties (MetaOutput    *output,
                                      MetaKmsUpdate *kms_update)
{
  MetaOutputKms *output_kms = output->driver_private;

  if (output->supported_rgb_ranges & (1 << output->rgb_range))
    {
      g_debug ("Setting RGB range of connector %s to %d",
               meta_kms_connector_get_name (output_kms->kms_connector),
               output->rgb_range);
      meta_kms_connector_set_rgb_range (output_kms->kms_connector,
                                        kms_update,
                                        output->rgb_range);
    }

  if (output->supported_color_modes & (1 << output->color_mode))
    {
      /* The EDID's frame average and minimum describe the display only
       * along with its peak; a peak the user set comes alone */
      float peak = meta_output_get_hdr_peak_luminance (output);
      gboolean from_edid = !output->hdr_peak_luminance;

      meta_kms_connector_set_hdr_primaries (output_kms->kms_connector,
                                            output->has_edid_primaries ?
                                            output->edid_primaries : NULL);
      meta_kms_connector_set_hdr_luminance (output_kms->kms_connector,
                                            peak,
                                            from_edid ? output->edid_max_frame_avg : 0,
                                            from_edid ? output->edid_min_luminance : 0);
      g_debug ("Setting color mode of connector %s to %d",
               meta_kms_connector_get_name (output_kms->kms_connector),
               output->color_mode);
      meta_kms_connector_set_color_mode (output_kms->kms_connector,
                                         kms_update,
                                         meta_gpu_kms_get_fd (META_GPU_KMS (output->gpu)),
                                         output->color_mode);

    }

  /* In the BT.2100 color mode the view applies the gamma ramp before
   * converting to PQ, so the hardware's is neutral; in SDR the hardware
   * applies it, with the color correction */
  if (meta_output_get_assigned_crtc (output))
    {
      MetaKmsCrtc *kms_crtc =
        meta_crtc_kms_get_kms_crtc (meta_output_get_assigned_crtc (output));
      g_autofree uint16_t *red = NULL;
      g_autofree uint16_t *green = NULL;
      g_autofree uint16_t *blue = NULL;
      int size;

      gboolean sdr_in_hardware =
        meta_output_kms_maps_sdr_colors_in_hardware (output);

      if (sdr_in_hardware)
        {
          float to_display[9];

          meta_color_transform_get_bt709_to_display (output->edid_primaries,
                                                     to_display);
          meta_kms_crtc_set_sdr_to_display (kms_crtc, to_display);
        }
      else
        {
          meta_kms_crtc_set_sdr_to_display (kms_crtc, NULL);
        }

      /* A gamma update also programs (or clears) the color pipeline */
      if (output->color_mode == META_OUTPUT_COLOR_MODE_BT2100)
        meta_kms_crtc_set_neutral_gamma (kms_crtc, kms_update);
      else if (meta_output_kms_get_gamma (output, &size, &red, &green, &blue))
        meta_kms_crtc_set_gamma (kms_crtc, kms_update, size, red, green, blue);
      else if (sdr_in_hardware ||
               meta_kms_crtc_is_color_pipeline_programmed (kms_crtc))
        meta_kms_crtc_set_neutral_gamma (kms_crtc, kms_update);
    }

  if (output->supported_color_formats & (1 << output->color_format))
    {
      g_debug ("Setting color format of connector %s to %d",
               meta_kms_connector_get_name (output_kms->kms_connector),
               output->color_format);
      meta_kms_connector_set_color_format (output_kms->kms_connector,
                                           kms_update,
                                           output->color_format);
    }
}

uint32_t
meta_output_kms_get_connector_id (MetaOutput *output)
{
  MetaOutputKms *output_kms = output->driver_private;

  return meta_kms_connector_get_id (output_kms->kms_connector);
}

void
meta_output_kms_set_power_save_mode (MetaOutput    *output,
                                     uint64_t       dpms_state,
                                     MetaKmsUpdate *kms_update)
{
  MetaOutputKms *output_kms = output->driver_private;

  g_debug ("Setting DPMS state of connector %s to %" G_GUINT64_FORMAT,
           meta_kms_connector_get_name (output_kms->kms_connector),
           dpms_state);

  meta_kms_connector_update_set_dpms_state (output_kms->kms_connector,
                                            kms_update,
                                            dpms_state);
}

gboolean
meta_output_kms_can_clone (MetaOutput *output,
                           MetaOutput *other_output)
{
  MetaOutputKms *output_kms = output->driver_private;
  MetaOutputKms *other_output_kms = other_output->driver_private;

  return meta_kms_connector_can_clone (output_kms->kms_connector,
                                       other_output_kms->kms_connector);
}

GBytes *
meta_output_kms_read_edid (MetaOutput *output)
{
  MetaOutputKms *output_kms = output->driver_private;
  const MetaKmsConnectorState *connector_state;
  GBytes *edid_data;

  connector_state =
    meta_kms_connector_get_current_state (output_kms->kms_connector);
  edid_data = connector_state->edid_data;
  if (!edid_data)
    return NULL;

  return g_bytes_new_from_bytes (edid_data, 0, g_bytes_get_size (edid_data));
}

static void
meta_output_destroy_notify (MetaOutput *output)
{
  MetaOutputKms *output_kms;

  output_kms = output->driver_private;

  g_slice_free (MetaOutputKms, output_kms);
}

static void
add_common_modes (MetaOutput *output,
                  MetaGpuKms *gpu_kms)
{
  const drmModeModeInfo *drm_mode;
  MetaCrtcMode *crtc_mode;
  GPtrArray *array;
  float refresh_rate;
  unsigned i;
  unsigned max_hdisplay = 0;
  unsigned max_vdisplay = 0;
  float max_refresh_rate = 0.0;

  for (i = 0; i < output->n_modes; i++)
    {
      drm_mode = output->modes[i]->driver_private;
      refresh_rate = meta_calculate_drm_mode_refresh_rate (drm_mode);
      max_hdisplay = MAX (max_hdisplay, drm_mode->hdisplay);
      max_vdisplay = MAX (max_vdisplay, drm_mode->vdisplay);
      max_refresh_rate = MAX (max_refresh_rate, refresh_rate);
    }

  max_refresh_rate = MAX (max_refresh_rate, 60.0);
  max_refresh_rate *= (1 + SYNC_TOLERANCE);

  array = g_ptr_array_new ();
  if (max_hdisplay > max_vdisplay)
    {
      for (i = 0; i < G_N_ELEMENTS (meta_default_landscape_drm_mode_infos); i++)
        {
          drm_mode = &meta_default_landscape_drm_mode_infos[i];
          refresh_rate = meta_calculate_drm_mode_refresh_rate (drm_mode);
          if (drm_mode->hdisplay > max_hdisplay ||
              drm_mode->vdisplay > max_vdisplay ||
              refresh_rate > max_refresh_rate)
            continue;

          crtc_mode = meta_gpu_kms_get_mode_from_drm_mode (gpu_kms,
                                                           drm_mode);
          g_ptr_array_add (array, crtc_mode);
        }
    }
  else
    {
      for (i = 0; i < G_N_ELEMENTS (meta_default_portrait_drm_mode_infos); i++)
        {
          drm_mode = &meta_default_portrait_drm_mode_infos[i];
          refresh_rate = meta_calculate_drm_mode_refresh_rate (drm_mode);
          if (drm_mode->hdisplay > max_hdisplay ||
              drm_mode->vdisplay > max_vdisplay ||
              refresh_rate > max_refresh_rate)
            continue;

          crtc_mode = meta_gpu_kms_get_mode_from_drm_mode (gpu_kms,
                                                           drm_mode);
          g_ptr_array_add (array, crtc_mode);
        }
    }

  output->modes = g_renew (MetaCrtcMode *, output->modes,
                           output->n_modes + array->len);
  memcpy (output->modes + output->n_modes, array->pdata,
          array->len * sizeof (MetaCrtcMode *));
  output->n_modes += array->len;

  g_ptr_array_free (array, TRUE);
}

static int
compare_modes (const void *one,
               const void *two)
{
  MetaCrtcMode *a = *(MetaCrtcMode **) one;
  MetaCrtcMode *b = *(MetaCrtcMode **) two;

  if (a->width != b->width)
    return a->width > b->width ? -1 : 1;
  if (a->height != b->height)
    return a->height > b->height ? -1 : 1;
  if (a->refresh_rate != b->refresh_rate)
    return a->refresh_rate > b->refresh_rate ? -1 : 1;

  return g_strcmp0 (b->name, a->name);
}

static gboolean
init_output_modes (MetaOutput  *output,
                   MetaGpuKms  *gpu_kms,
                   GError     **error)
{
  MetaOutputKms *output_kms = output->driver_private;
  const MetaKmsConnectorState *connector_state;
  int i;

  connector_state =
    meta_kms_connector_get_current_state (output_kms->kms_connector);

  output->preferred_mode = NULL;

  output->n_modes = connector_state->n_modes;
  output->modes = g_new0 (MetaCrtcMode *, output->n_modes);
  for (i = 0; i < connector_state->n_modes; i++)
    {
      drmModeModeInfo *drm_mode = &connector_state->modes[i];
      MetaCrtcMode *crtc_mode;

      crtc_mode = meta_gpu_kms_get_mode_from_drm_mode (gpu_kms, drm_mode);
      output->modes[i] = crtc_mode;
      if (drm_mode->type & DRM_MODE_TYPE_PREFERRED)
        output->preferred_mode = output->modes[i];
    }

  /* FIXME: MSC feature bit? */
  /* Presume that if the output supports scaling, then we have
   * a panel fitter capable of adjusting any mode to suit.
   */
  if (connector_state->has_scaling)
    add_common_modes (output, gpu_kms);

  if (!output->modes)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No modes available");
      return FALSE;
    }

  qsort (output->modes, output->n_modes,
         sizeof (MetaCrtcMode *), compare_modes);

  if (!output->preferred_mode)
    output->preferred_mode = output->modes[0];

  return TRUE;
}

/* Whether the connector's device has CRTCs that can enable variable refresh */
static gboolean
device_can_vary_refresh (MetaKmsConnector *kms_connector)
{
  MetaKmsDevice *device = meta_kms_connector_get_device (kms_connector);
  GList *l;

  for (l = meta_kms_device_get_crtcs (device); l; l = l->next)
    {
      if (meta_kms_crtc_get_vrr_enabled_prop_id (l->data))
        return TRUE;
    }

  return FALSE;
}

MetaOutput *
meta_create_kms_output (MetaGpuKms        *gpu_kms,
                        MetaKmsConnector  *kms_connector,
                        MetaOutput        *old_output,
                        GError           **error)
{
  MetaGpu *gpu = META_GPU (gpu_kms);
  MetaOutput *output;
  MetaOutputKms *output_kms;
  const MetaKmsConnectorState *connector_state;
  uint32_t connector_id;
  GArray *crtcs;
  GList *l;
  uint32_t gpu_id;

  output = g_object_new (META_TYPE_OUTPUT, NULL);

  output_kms = g_slice_new0 (MetaOutputKms);
  output->driver_private = output_kms;
  output->driver_notify = (GDestroyNotify) meta_output_destroy_notify;

  output->gpu = gpu;
  output->name = g_strdup (meta_kms_connector_get_name (kms_connector));

  gpu_id = meta_gpu_kms_get_id (gpu_kms);
  connector_id = meta_kms_connector_get_id (kms_connector);
  output->winsys_id = ((uint64_t) gpu_id << 32) | connector_id;

  output_kms->kms_connector = kms_connector;

  connector_state = meta_kms_connector_get_current_state (kms_connector);

  output->panel_orientation_transform =
    connector_state->panel_orientation_transform;
  if (meta_monitor_transform_is_rotated (output->panel_orientation_transform))
    {
      output->width_mm = connector_state->height_mm;
      output->height_mm = connector_state->width_mm;
    }
  else
    {
      output->width_mm = connector_state->width_mm;
      output->height_mm = connector_state->height_mm;
    }

  if (!init_output_modes (output, gpu_kms, error))
    {
      g_object_unref (output);
      return NULL;
    }

  crtcs = g_array_new (FALSE, FALSE, sizeof (MetaCrtc *));

  for (l = meta_gpu_get_crtcs (gpu); l; l = l->next)
    {
      MetaCrtc *crtc = l->data;
      MetaKmsCrtc *kms_crtc = meta_crtc_kms_get_kms_crtc (crtc);
      uint32_t crtc_idx;

      crtc_idx = meta_kms_crtc_get_idx (kms_crtc);
      if (connector_state->common_possible_crtcs & (1 << crtc_idx))
        g_array_append_val (crtcs, crtc);
    }

  output->n_possible_crtcs = crtcs->len;
  output->possible_crtcs = (MetaCrtc **) g_array_free (crtcs, FALSE);

  if (connector_state->current_crtc_id)
    {
      for (l = meta_gpu_get_crtcs (gpu); l; l = l->next)
        {
          MetaCrtc *crtc = l->data;

          if (crtc->crtc_id == connector_state->current_crtc_id)
            {
              meta_output_assign_crtc (output, crtc);
              break;
            }
        }
    }
  else
    {
      meta_output_unassign_crtc (output);
    }

  if (old_output)
    {
      output->is_primary = old_output->is_primary;
      output->is_presentation = old_output->is_presentation;
    }
  else
    {
      output->is_primary = FALSE;
      output->is_presentation = FALSE;
    }

  output->suggested_x = connector_state->suggested_x;
  output->suggested_y = connector_state->suggested_y;
  output->hotplug_mode_update = connector_state->hotplug_mode_update;
  output->supports_underscanning =
    meta_kms_connector_is_underscanning_supported (kms_connector);

  {
    const MetaKmsConnectorState *connector_state =
      meta_kms_connector_get_current_state (kms_connector);

    output->vrr_capable = connector_state && connector_state->vrr_capable &&
                          device_can_vary_refresh (kms_connector);
  }

  output->supported_rgb_ranges =
    meta_kms_connector_get_supported_rgb_ranges (kms_connector);
  output->supported_color_formats =
    meta_kms_connector_get_supported_color_formats (kms_connector);
  if (old_output &&
      (output->supported_rgb_ranges & (1 << old_output->rgb_range)))
    output->rgb_range = old_output->rgb_range;
  else
    output->rgb_range = META_OUTPUT_RGB_RANGE_AUTO;
  if (old_output &&
      (output->supported_color_formats & (1 << old_output->color_format)))
    output->color_format = old_output->color_format;
  else
    output->color_format = META_OUTPUT_COLOR_FORMAT_AUTO;

  output->supported_color_modes =
    meta_kms_connector_get_supported_color_modes (kms_connector);
  if (old_output &&
      (output->supported_color_modes & (1 << old_output->color_mode)))
    output->color_mode = old_output->color_mode;
  else
    output->color_mode = META_OUTPUT_COLOR_MODE_DEFAULT;
  if (old_output)
    {
      output->no_hdr_auto_switch = old_output->no_hdr_auto_switch;
      output->vrr_mode = old_output->vrr_mode;
      output->sdr_white_level = old_output->sdr_white_level;
      output->hdr_peak_luminance = old_output->hdr_peak_luminance;
    }

  meta_output_parse_edid (output, connector_state->edid_data);
  meta_color_correction_load (output->vendor, output->product, output->serial,
                              &output->color_correction);

  output->connector_type = meta_kms_connector_get_connector_type (kms_connector);

  output->tile_info = connector_state->tile_info;

  /* FIXME: backlight is a very driver specific thing unfortunately,
     every DDX does its own thing, and the dumb KMS API does not include it.

     For example, xf86-video-intel has a list of paths to probe in /sys/class/backlight
     (one for each major HW maker, and then some).
     We can't do the same because we're not root.
     It might be best to leave backlight out of the story and rely on the setuid
     helper in gnome-settings-daemon.
     */
  output->backlight_min = 0;
  output->backlight_max = 0;
  output->backlight = -1;

  return output;
}
