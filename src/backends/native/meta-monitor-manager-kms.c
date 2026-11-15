/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2013 Red Hat Inc.
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
 *
 * Author: Giovanni Campagna <gcampagn@redhat.com>
 */

/**
 * SECTION:meta-monitor-manager-kms
 * @title: MetaMonitorManagerKms
 * @short_description: A subclass of #MetaMonitorManager using Linux DRM
 *
 * #MetaMonitorManagerKms is a subclass of #MetaMonitorManager which
 * implements its functionality "natively": it uses the appropriate
 * functions of the Linux DRM kernel module and using a udev client.
 *
 * See also #MetaMonitorManagerXrandr for an implementation using XRandR.
 */

#include "config.h"

#include "backends/native/meta-monitor-manager-kms.h"

#include <drm.h>
#include <errno.h>
#include <gudev/gudev.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "backends/meta-backend-private.h"
#include "backends/meta-crtc.h"
#include "backends/meta-monitor-config-manager.h"
#include "backends/meta-output.h"
#include "backends/meta-renderer-view.h"
#include "backends/native/meta-backend-native.h"
#include "backends/native/meta-crtc-kms.h"
#include "backends/native/meta-gpu-kms.h"
#include "backends/native/meta-kms-crtc.h"
#include "backends/native/meta-kms-update.h"
#include "backends/native/meta-kms.h"
#include "backends/native/meta-launcher.h"
#include "backends/native/meta-output-kms.h"
#include "backends/native/meta-renderer-native.h"
#include "clutter/clutter.h"
#include "meta/main.h"
#include "meta/meta-x11-errors.h"

typedef struct
{
  GSource source;

  gpointer fd_tag;
  MetaMonitorManagerKms *manager_kms;
} MetaKmsSource;

struct _MetaMonitorManagerKms
{
  MetaMonitorManager parent_instance;

  gulong kms_resources_changed_handler_id;
};

struct _MetaMonitorManagerKmsClass
{
  MetaMonitorManagerClass parent_class;
};

static void
initable_iface_init (GInitableIface *initable_iface);

G_DEFINE_TYPE_WITH_CODE (MetaMonitorManagerKms, meta_monitor_manager_kms,
                         META_TYPE_MONITOR_MANAGER,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                initable_iface_init))

static GBytes *
meta_monitor_manager_kms_read_edid (MetaMonitorManager *manager,
                                    MetaOutput         *output)
{
  return meta_output_kms_read_edid (output);
}

static void
meta_monitor_manager_kms_read_current_state (MetaMonitorManager *manager)
{
  MetaMonitorManagerClass *parent_class =
    META_MONITOR_MANAGER_CLASS (meta_monitor_manager_kms_parent_class);
  MetaPowerSave power_save_mode;

  power_save_mode = meta_monitor_manager_get_power_save_mode (manager);
  if (power_save_mode != META_POWER_SAVE_ON)
    meta_monitor_manager_power_save_mode_changed (manager,
                                                  META_POWER_SAVE_ON);

  parent_class->read_current_state (manager);
}

static void
meta_monitor_manager_kms_set_power_save_mode (MetaMonitorManager *manager,
                                              MetaPowerSave       mode)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaBackendNative *backend_native = META_BACKEND_NATIVE (backend);
  MetaKms *kms = meta_backend_native_get_kms (backend_native);
  MetaKmsUpdate *kms_update;
  uint64_t state;
  GList *l;
  g_autoptr (MetaKmsFeedback) kms_feedback = NULL;

  switch (mode) {
  case META_POWER_SAVE_ON:
    state = DRM_MODE_DPMS_ON;
    break;
  case META_POWER_SAVE_STANDBY:
    state = DRM_MODE_DPMS_STANDBY;
    break;
  case META_POWER_SAVE_SUSPEND:
    state = DRM_MODE_DPMS_SUSPEND;
    break;
  case META_POWER_SAVE_OFF:
    state = DRM_MODE_DPMS_OFF;
    break;
  default:
    return;
  }

  kms_update = meta_kms_ensure_pending_update (kms);
  for (l = meta_backend_get_gpus (backend); l; l = l->next)
    {
      MetaGpuKms *gpu_kms = l->data;

      meta_gpu_kms_set_power_save_mode (gpu_kms, state, kms_update);
    }

  kms_feedback = meta_kms_post_pending_update_sync (kms);
  if (meta_kms_feedback_get_result (kms_feedback) != META_KMS_FEEDBACK_PASSED)
    {
      g_warning ("Failed to set DPMS: %s",
                 meta_kms_feedback_get_error (kms_feedback)->message);
    }
}

static void
meta_monitor_manager_kms_ensure_initial_config (MetaMonitorManager *manager)
{
  MetaMonitorsConfig *config;

  config = meta_monitor_manager_ensure_configured (manager);

  meta_monitor_manager_update_logical_state (manager, config);
}

static void
apply_crtc_assignments (MetaMonitorManager *manager,
                        MetaCrtcInfo       **crtcs,
                        unsigned int         n_crtcs,
                        MetaOutputInfo     **outputs,
                        unsigned int         n_outputs)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  unsigned i;
  GList *gpus;
  GList *l;

  for (i = 0; i < n_crtcs; i++)
    {
      MetaCrtcInfo *crtc_info = crtcs[i];
      MetaCrtc *crtc = crtc_info->crtc;

      crtc->is_dirty = TRUE;

      if (crtc_info->mode == NULL)
        {
          meta_crtc_unset_config (crtc);
        }
      else
        {
          unsigned int j;

          meta_crtc_set_config (crtc,
                                &crtc_info->layout,
                                crtc_info->mode,
                                crtc_info->transform);

          for (j = 0; j < crtc_info->outputs->len; j++)
            {
              MetaOutput *output = g_ptr_array_index (crtc_info->outputs, j);

              output->is_dirty = TRUE;
              meta_output_assign_crtc (output, crtc);
            }
        }
    }
  /* Disable CRTCs not mentioned in the list (they have is_dirty == FALSE,
     because they weren't seen in the first loop) */
  gpus = meta_backend_get_gpus (backend);
  for (l = gpus; l; l = l->next)
    {
      MetaGpu *gpu = l->data;
      GList *k;

      for (k = meta_gpu_get_crtcs (gpu); k; k = k->next)
        {
          MetaCrtc *crtc = k->data;

          if (crtc->is_dirty)
            {
              crtc->is_dirty = FALSE;
              continue;
            }

          meta_crtc_unset_config (crtc);
        }
    }

  for (i = 0; i < n_outputs; i++)
    {
      MetaOutputInfo *output_info = outputs[i];
      MetaOutput *output = output_info->output;

      output->is_primary = output_info->is_primary;
      output->is_presentation = output_info->is_presentation;
      output->is_underscanning = output_info->is_underscanning;
      if (output->supported_rgb_ranges & (1 << output_info->rgb_range))
        output->rgb_range = output_info->rgb_range;
      if (output->supported_color_formats & (1 << output_info->color_format))
        output->color_format = output_info->color_format;
      if (output->supported_color_modes & (1 << output_info->color_mode))
        output->color_mode = output_info->color_mode;
      output->no_hdr_auto_switch = output_info->no_hdr_auto_switch;
      output->vrr_mode = output->vrr_capable ? output_info->vrr_mode
                                             : META_OUTPUT_VRR_MODE_OFF;
      output->sdr_white_level = output_info->sdr_white_level;
      output->hdr_peak_luminance = output_info->hdr_peak_luminance;
    }

  /* Disable outputs not mentioned in the list */
  for (l = gpus; l; l = l->next)
    {
      MetaGpu *gpu = l->data;
      GList *k;

      for (k = meta_gpu_get_outputs (gpu); k; k = k->next)
        {
          MetaOutput *output = k->data;

          if (output->is_dirty)
            {
              output->is_dirty = FALSE;
              continue;
            }

          meta_output_unassign_crtc (output);
          output->is_primary = FALSE;
        }
    }
}

static void
update_screen_size (MetaMonitorManager *manager,
                    MetaMonitorsConfig *config)
{
  GList *l;
  int screen_width = 0;
  int screen_height = 0;

  for (l = config->logical_monitor_configs; l; l = l->next)
    {
      MetaLogicalMonitorConfig *logical_monitor_config = l->data;
      int right_edge;
      int bottom_edge;

      right_edge = (logical_monitor_config->layout.width +
                    logical_monitor_config->layout.x);
      if (right_edge > screen_width)
        screen_width = right_edge;

      bottom_edge = (logical_monitor_config->layout.height +
                     logical_monitor_config->layout.y);
      if (bottom_edge > screen_height)
        screen_height = bottom_edge;
    }

  manager->screen_width = screen_width;
  manager->screen_height = screen_height;
}

static gboolean
meta_monitor_manager_kms_apply_monitors_config (MetaMonitorManager      *manager,
                                                MetaMonitorsConfig      *config,
                                                MetaMonitorsConfigMethod method,
                                                GError                 **error)
{
  GPtrArray *crtc_infos;
  GPtrArray *output_infos;

  if (!config)
    {
      if (!manager->in_init)
        {
          MetaBackend *backend = meta_get_backend ();
          MetaRenderer *renderer = meta_backend_get_renderer (backend);

          meta_renderer_native_reset_modes (META_RENDERER_NATIVE (renderer));
        }

      manager->screen_width = META_MONITOR_MANAGER_MIN_SCREEN_WIDTH;
      manager->screen_height = META_MONITOR_MANAGER_MIN_SCREEN_HEIGHT;
      meta_monitor_manager_rebuild (manager, NULL);
      return TRUE;
    }

  if (!meta_monitor_config_manager_assign (manager, config,
                                           &crtc_infos, &output_infos,
                                           error))
    return FALSE;

  if (method == META_MONITORS_CONFIG_METHOD_VERIFY)
    {
      g_ptr_array_free (crtc_infos, TRUE);
      g_ptr_array_free (output_infos, TRUE);
      return TRUE;
    }

  apply_crtc_assignments (manager,
                          (MetaCrtcInfo **) crtc_infos->pdata,
                          crtc_infos->len,
                          (MetaOutputInfo **) output_infos->pdata,
                          output_infos->len);

  g_ptr_array_free (crtc_infos, TRUE);
  g_ptr_array_free (output_infos, TRUE);

  update_screen_size (manager, config);
  meta_monitor_manager_rebuild (manager, config);

  return TRUE;
}

static void
meta_monitor_manager_kms_get_crtc_gamma (MetaMonitorManager  *manager,
                                         MetaCrtc            *crtc,
                                         gsize               *size,
                                         unsigned short     **red,
                                         unsigned short     **green,
                                         unsigned short     **blue)
{
  MetaKmsCrtc *kms_crtc;
  const MetaKmsCrtcState *crtc_state;
  const uint16_t *wanted_red, *wanted_green, *wanted_blue;
  int wanted_size;

  kms_crtc = meta_crtc_kms_get_kms_crtc (crtc);

  /* What was asked for, not the neutral ramp of an output in the BT.2100
   * color mode */
  if (meta_kms_crtc_get_wanted_gamma (kms_crtc, &wanted_size,
                                      &wanted_red, &wanted_green, &wanted_blue))
    {
      *size = wanted_size;
      *red = g_memdup2 (wanted_red, *size * sizeof **red);
      *green = g_memdup2 (wanted_green, *size * sizeof **green);
      *blue = g_memdup2 (wanted_blue, *size * sizeof **blue);
      return;
    }

  crtc_state = meta_kms_crtc_get_current_state (kms_crtc);

  *size = crtc_state->gamma.size;
  *red = g_memdup2 (crtc_state->gamma.red, *size * sizeof **red);
  *green = g_memdup2 (crtc_state->gamma.green, *size * sizeof **green);
  *blue = g_memdup2 (crtc_state->gamma.blue, *size * sizeof **blue);
}

static char *
generate_gamma_ramp_string (size_t          size,
                            unsigned short *red,
                            unsigned short *green,
                            unsigned short *blue)
{
  GString *string;
  int color;

  string = g_string_new ("[");
  for (color = 0; color < 3; color++)
    {
      unsigned short **color_ptr;
      char color_char;
      size_t i;

      switch (color)
        {
        case 0:
          color_ptr = &red;
          color_char = 'r';
          break;
        case 1:
          color_ptr = &green;
          color_char = 'g';
          break;
        case 2:
          color_ptr = &blue;
          color_char = 'b';
          break;
        }

      g_string_append_printf (string, " %c: ", color_char);
      for (i = 0; i < MIN (4, size); i++)
        {
          int j;

          if (size > 4)
            {
              if (i == 2)
                g_string_append (string, ",...");

              if (i >= 2)
                j = i + (size - 4);
              else
                j = i;
            }
          else
            {
              j = i;
            }
          g_string_append_printf (string, "%s%hu",
                                  j == 0 ? "" : ",",
                                  (*color_ptr)[i]);
        }
    }

  g_string_append (string, " ]");

  return g_string_free (string, FALSE);
}

/* Whether @crtc drives an output in the BT.2100 color mode */
static gboolean
crtc_is_bt2100 (MetaCrtc *crtc)
{
  GList *l;

  for (l = meta_gpu_get_outputs (meta_crtc_get_gpu (crtc)); l; l = l->next)
    {
      MetaOutput *output = l->data;

      if (meta_output_get_assigned_crtc (output) == crtc &&
          output->color_mode == META_OUTPUT_COLOR_MODE_BT2100)
        return TRUE;
    }

  return FALSE;
}

/* Hand @crtc's gamma ramp to the views it shows, which apply it in the
 * BT.2100 color mode */
static void
update_view_gamma (MetaMonitorManager *manager,
                   MetaCrtc           *crtc,
                   gsize               size,
                   unsigned short     *red,
                   unsigned short     *green,
                   unsigned short     *blue)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaRenderer *renderer = meta_backend_get_renderer (backend);
  GList *l;

  for (l = meta_renderer_get_views (renderer); l; l = l->next)
    {
      ClutterStageView *view = l->data;
      CoglFramebuffer *onscreen = clutter_stage_view_get_onscreen (view);

      if (!META_IS_RENDERER_VIEW (view) || !cogl_is_onscreen (onscreen) ||
          meta_onscreen_native_get_crtc (COGL_ONSCREEN (onscreen)) != crtc)
        continue;

      meta_renderer_view_set_gamma (META_RENDERER_VIEW (view),
                                    size, red, green, blue);
    }

  clutter_actor_queue_redraw (meta_backend_get_stage (backend));
}

static void
meta_monitor_manager_kms_set_crtc_gamma (MetaMonitorManager *manager,
                                         MetaCrtc           *crtc,
                                         gsize               size,
                                         unsigned short     *red,
                                         unsigned short     *green,
                                         unsigned short     *blue)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaBackendNative *backend_native = META_BACKEND_NATIVE (backend);
  MetaKms *kms = meta_backend_native_get_kms (backend_native);
  MetaKmsCrtc *kms_crtc;
  g_autofree char *gamma_ramp_string = NULL;
  MetaKmsUpdate *kms_update;
  g_autoptr (MetaKmsFeedback) kms_feedback = NULL;

  gamma_ramp_string = generate_gamma_ramp_string (size, red, green, blue);
  g_debug ("Setting CRTC (%ld) gamma to %s", crtc->crtc_id, gamma_ramp_string);

  kms_crtc = meta_crtc_kms_get_kms_crtc (crtc);
  meta_kms_crtc_set_wanted_gamma (kms_crtc, size, red, green, blue);

  /* The hardware ramp would bend the PQ signal: the view applies it */
  if (crtc_is_bt2100 (crtc))
    {
      update_view_gamma (manager, crtc, size, red, green, blue);
      return;
    }

  kms_update = meta_kms_ensure_pending_update (kms);

  meta_kms_crtc_set_gamma (kms_crtc, kms_update,
                           size, red, green, blue);

  kms_feedback = meta_kms_post_pending_update_sync (kms);
  if (meta_kms_feedback_get_result (kms_feedback) != META_KMS_FEEDBACK_PASSED)
    {
      g_warning ("Failed to set CRTC gamma: %s",
                 meta_kms_feedback_get_error (kms_feedback)->message);
    }
}

static void
handle_hotplug_event (MetaMonitorManager *manager)
{
  meta_monitor_manager_read_current_state (manager);
  meta_monitor_manager_on_hotplug (manager);
}

static void
on_kms_resources_changed (MetaKms            *kms,
                          MetaMonitorManager *manager)
{
  handle_hotplug_event (manager);
}

static void
meta_monitor_manager_kms_connect_hotplug_handler (MetaMonitorManagerKms *manager_kms)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_kms);
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaBackendNative *backend_native = META_BACKEND_NATIVE (backend);
  MetaKms *kms = meta_backend_native_get_kms (backend_native);

  manager_kms->kms_resources_changed_handler_id =
    g_signal_connect (kms, "resources-changed",
                      G_CALLBACK (on_kms_resources_changed), manager);
}

static void
meta_monitor_manager_kms_disconnect_hotplug_handler (MetaMonitorManagerKms *manager_kms)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_kms);
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaBackendNative *backend_native = META_BACKEND_NATIVE (backend);
  MetaKms *kms = meta_backend_native_get_kms (backend_native);

  g_clear_signal_handler (&manager_kms->kms_resources_changed_handler_id, kms);
}

void
meta_monitor_manager_kms_pause (MetaMonitorManagerKms *manager_kms)
{
  meta_monitor_manager_kms_disconnect_hotplug_handler (manager_kms);
}

void
meta_monitor_manager_kms_resume (MetaMonitorManagerKms *manager_kms)
{
  meta_monitor_manager_kms_connect_hotplug_handler (manager_kms);
  handle_hotplug_event (META_MONITOR_MANAGER (manager_kms));
}

static gboolean
meta_monitor_manager_kms_is_transform_handled (MetaMonitorManager  *manager,
                                               MetaCrtc            *crtc,
                                               MetaMonitorTransform transform)
{
  return meta_crtc_kms_is_transform_handled (crtc, transform);
}

static MetaMonitorScalesConstraint
get_monitor_scale_constraints_per_layout_mode (MetaLogicalMonitorLayoutMode layout_mode)
{
  MetaMonitorScalesConstraint constraints =
    META_MONITOR_SCALES_CONSTRAINT_NONE;

  switch (layout_mode)
    {
    case META_LOGICAL_MONITOR_LAYOUT_MODE_LOGICAL:
    case META_LOGICAL_MONITOR_LAYOUT_MODE_GLOBAL_UI_LOGICAL:
      break;
    case META_LOGICAL_MONITOR_LAYOUT_MODE_PHYSICAL:
      constraints |= META_MONITOR_SCALES_CONSTRAINT_NO_FRAC;
      break;
    }

  return constraints;
}

static float
meta_monitor_manager_kms_calculate_monitor_mode_scale (MetaMonitorManager           *manager,
                                                       MetaLogicalMonitorLayoutMode  layout_mode,
                                                       MetaMonitor                  *monitor,
                                                       MetaMonitorMode              *monitor_mode)
{
  MetaMonitorScalesConstraint constraints =
    get_monitor_scale_constraints_per_layout_mode (layout_mode);

  return meta_monitor_calculate_mode_scale (monitor, monitor_mode, constraints);
}

static float *
meta_monitor_manager_kms_calculate_supported_scales (MetaMonitorManager           *manager,
                                                     MetaLogicalMonitorLayoutMode  layout_mode,
                                                     MetaMonitor                  *monitor,
                                                     MetaMonitorMode              *monitor_mode,
                                                     int                          *n_supported_scales)
{
  MetaMonitorScalesConstraint constraints =
    get_monitor_scale_constraints_per_layout_mode (layout_mode);

  return meta_monitor_calculate_supported_scales (monitor, monitor_mode,
                                                  constraints,
                                                  n_supported_scales);
}

static MetaMonitorManagerCapability
meta_monitor_manager_kms_get_capabilities (MetaMonitorManager *manager)
{
  return META_MONITOR_MANAGER_CAPABILITY_TILING |
         META_MONITOR_MANAGER_CAPABILITY_LAYOUT_MODE;
}

static gboolean
meta_monitor_manager_kms_get_max_screen_size (MetaMonitorManager *manager,
                                              int                *max_width,
                                              int                *max_height)
{
  return FALSE;
}

static MetaLogicalMonitorLayoutMode
meta_monitor_manager_kms_get_default_layout_mode (MetaMonitorManager *manager)
{
  return META_LOGICAL_MONITOR_LAYOUT_MODE_LOGICAL;
}

static const char *
meta_monitor_manager_kms_get_config_file_basename (MetaMonitorManager *manager)
{
  return "cinnamon-monitors-wayland.xml";
}

static gboolean
meta_monitor_manager_kms_initable_init (GInitable    *initable,
                                        GCancellable *cancellable,
                                        GError      **error)
{
  MetaMonitorManagerKms *manager_kms = META_MONITOR_MANAGER_KMS (initable);
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_kms);
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  gboolean can_have_outputs;
  GList *l;

  meta_monitor_manager_kms_connect_hotplug_handler (manager_kms);

  can_have_outputs = FALSE;
  for (l = meta_backend_get_gpus (backend); l; l = l->next)
    {
      MetaGpuKms *gpu_kms = l->data;

      if (meta_gpu_kms_can_have_outputs (gpu_kms))
        {
          can_have_outputs = TRUE;
          break;
        }
    }
  if (!can_have_outputs)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No GPUs with outputs found");
      return FALSE;
    }

  return TRUE;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = meta_monitor_manager_kms_initable_init;
}

static void
meta_monitor_manager_kms_init (MetaMonitorManagerKms *manager_kms)
{
}

static void
meta_monitor_manager_kms_class_init (MetaMonitorManagerKmsClass *klass)
{
  MetaMonitorManagerClass *manager_class = META_MONITOR_MANAGER_CLASS (klass);

  manager_class->read_edid = meta_monitor_manager_kms_read_edid;
  manager_class->read_current_state = meta_monitor_manager_kms_read_current_state;
  manager_class->ensure_initial_config = meta_monitor_manager_kms_ensure_initial_config;
  manager_class->apply_monitors_config = meta_monitor_manager_kms_apply_monitors_config;
  manager_class->set_power_save_mode = meta_monitor_manager_kms_set_power_save_mode;
  manager_class->get_crtc_gamma = meta_monitor_manager_kms_get_crtc_gamma;
  manager_class->set_crtc_gamma = meta_monitor_manager_kms_set_crtc_gamma;
  manager_class->is_transform_handled = meta_monitor_manager_kms_is_transform_handled;
  manager_class->calculate_monitor_mode_scale = meta_monitor_manager_kms_calculate_monitor_mode_scale;
  manager_class->calculate_supported_scales = meta_monitor_manager_kms_calculate_supported_scales;
  manager_class->get_capabilities = meta_monitor_manager_kms_get_capabilities;
  manager_class->get_max_screen_size = meta_monitor_manager_kms_get_max_screen_size;
  manager_class->get_default_layout_mode = meta_monitor_manager_kms_get_default_layout_mode;
  manager_class->get_config_file_basename = meta_monitor_manager_kms_get_config_file_basename;
}
