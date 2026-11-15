/*
 * Copyright (C) 2019 Red Hat
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

#include "backends/native/meta-kms-crtc.h"

#include <string.h>
#include <xf86drmMode.h>
#include "backends/native/meta-kms-crtc-private.h"

#include "backends/native/meta-kms-device-private.h"
#include "backends/native/meta-kms-impl-device.h"
#include "backends/native/meta-kms-update-private.h"

struct _MetaKmsCrtc
{
  GObject parent;

  MetaKmsDevice *device;

  uint32_t id;
  int idx;

  MetaKmsCrtcState current_state;

  uint32_t vrr_enabled_prop_id;

  /* The gamma ramp clients asked for: on outputs in the BT.2100 color mode
   * the compositor applies it before converting to PQ, and the hardware
   * ramp stays neutral */
  struct {
    int size;
    uint16_t *red;
    uint16_t *green;
    uint16_t *blue;
  } wanted_gamma;
};

G_DEFINE_TYPE (MetaKmsCrtc, meta_kms_crtc, G_TYPE_OBJECT)

void
meta_kms_crtc_set_gamma (MetaKmsCrtc    *crtc,
                         MetaKmsUpdate  *update,
                         int             size,
                         const uint16_t *red,
                         const uint16_t *green,
                         const uint16_t *blue)
{
  meta_kms_update_set_crtc_gamma (update, crtc, size, red, green, blue);
}

void
meta_kms_crtc_set_wanted_gamma (MetaKmsCrtc    *crtc,
                                int             size,
                                const uint16_t *red,
                                const uint16_t *green,
                                const uint16_t *blue)
{
  g_clear_pointer (&crtc->wanted_gamma.red, g_free);
  g_clear_pointer (&crtc->wanted_gamma.green, g_free);
  g_clear_pointer (&crtc->wanted_gamma.blue, g_free);
  crtc->wanted_gamma.size = size;
  if (size > 0)
    {
      crtc->wanted_gamma.red = g_memdup2 (red, size * sizeof (*red));
      crtc->wanted_gamma.green = g_memdup2 (green, size * sizeof (*green));
      crtc->wanted_gamma.blue = g_memdup2 (blue, size * sizeof (*blue));
    }
}

/* The gamma ramp clients asked for; FALSE if none */
gboolean
meta_kms_crtc_get_wanted_gamma (MetaKmsCrtc     *crtc,
                                int             *size,
                                const uint16_t **red,
                                const uint16_t **green,
                                const uint16_t **blue)
{
  if (crtc->wanted_gamma.size <= 0)
    return FALSE;

  *size = crtc->wanted_gamma.size;
  *red = crtc->wanted_gamma.red;
  *green = crtc->wanted_gamma.green;
  *blue = crtc->wanted_gamma.blue;
  return TRUE;
}

/* Sets a neutral ramp of the hardware's size, if it has one */
void
meta_kms_crtc_set_neutral_gamma (MetaKmsCrtc   *crtc,
                                 MetaKmsUpdate *update)
{
  int size = crtc->current_state.gamma.size;
  g_autofree uint16_t *ramp = NULL;
  int i;

  if (size <= 1)
    return;

  ramp = g_new (uint16_t, size);
  for (i = 0; i < size; i++)
    ramp[i] = (uint16_t) ((i * 65535) / (size - 1));
  meta_kms_update_set_crtc_gamma (update, crtc, size, ramp, ramp, ramp);
}

MetaKmsDevice *
meta_kms_crtc_get_device (MetaKmsCrtc *crtc)
{
  return crtc->device;
}

const MetaKmsCrtcState *
meta_kms_crtc_get_current_state (MetaKmsCrtc *crtc)
{
  return &crtc->current_state;
}

uint32_t
meta_kms_crtc_get_id (MetaKmsCrtc *crtc)
{
  return crtc->id;
}

int
meta_kms_crtc_get_idx (MetaKmsCrtc *crtc)
{
  return crtc->idx;
}

static void
read_gamma_state (MetaKmsCrtc       *crtc,
                  MetaKmsImplDevice *impl_device,
                  drmModeCrtc       *drm_crtc)
{
  MetaKmsCrtcState *current_state = &crtc->current_state;

  if (current_state->gamma.size != drm_crtc->gamma_size)
    {
      current_state->gamma.size = drm_crtc->gamma_size;

      current_state->gamma.red = g_realloc_n (current_state->gamma.red,
                                              drm_crtc->gamma_size,
                                              sizeof (uint16_t));
      current_state->gamma.green = g_realloc_n (current_state->gamma.green,
                                                drm_crtc->gamma_size,
                                                sizeof (uint16_t));
      current_state->gamma.blue = g_realloc_n (current_state->gamma.blue,
                                               drm_crtc->gamma_size,
                                               sizeof (uint16_t));
    }

  drmModeCrtcGetGamma (meta_kms_impl_device_get_fd (impl_device),
                       crtc->id,
                       current_state->gamma.size,
                       current_state->gamma.red,
                       current_state->gamma.green,
                       current_state->gamma.blue);
}

static void
meta_kms_crtc_read_state (MetaKmsCrtc       *crtc,
                          MetaKmsImplDevice *impl_device,
                          drmModeCrtc       *drm_crtc)
{
  crtc->current_state.rect = (MetaRectangle) {
    .x = drm_crtc->x,
    .y = drm_crtc->y,
    .width = drm_crtc->width,
    .height = drm_crtc->height,
  };

  crtc->current_state.is_drm_mode_valid = drm_crtc->mode_valid;
  crtc->current_state.drm_mode = drm_crtc->mode;

  read_gamma_state (crtc, impl_device, drm_crtc);
}

void
meta_kms_crtc_update_state (MetaKmsCrtc *crtc)
{
  MetaKmsImplDevice *impl_device;
  drmModeCrtc *drm_crtc;

  impl_device = meta_kms_device_get_impl_device (crtc->device);
  drm_crtc = drmModeGetCrtc (meta_kms_impl_device_get_fd (impl_device),
                             crtc->id);
  if (!drm_crtc)
    {
      crtc->current_state.rect = (MetaRectangle) { };
      crtc->current_state.is_drm_mode_valid = FALSE;
      return;
    }

  meta_kms_crtc_read_state (crtc, impl_device, drm_crtc);
  drmModeFreeCrtc (drm_crtc);
}

static void
clear_gamma_state (MetaKmsCrtc *crtc)
{
  crtc->current_state.gamma.size = 0;
  g_clear_pointer (&crtc->current_state.gamma.red, g_free);
  g_clear_pointer (&crtc->current_state.gamma.green, g_free);
  g_clear_pointer (&crtc->current_state.gamma.blue, g_free);
}

void
meta_kms_crtc_predict_state (MetaKmsCrtc   *crtc,
                             MetaKmsUpdate *update)
{
  GList *mode_sets;
  GList *crtc_gammas;
  GList *l;

  mode_sets = meta_kms_update_get_mode_sets (update);
  for (l = mode_sets; l; l = l->next)
    {
      MetaKmsModeSet *mode_set = l->data;

      if (mode_set->crtc != crtc)
        continue;

      if (mode_set->drm_mode)
        {
          MetaKmsPlaneAssignment *plane_assignment;

          plane_assignment =
            meta_kms_update_get_primary_plane_assignment (update, crtc);

          crtc->current_state.rect =
            meta_fixed_16_rectangle_to_rectangle (plane_assignment->src_rect);
          crtc->current_state.is_drm_mode_valid = TRUE;
          crtc->current_state.drm_mode = *mode_set->drm_mode;
        }
      else
        {
          crtc->current_state.rect = (MetaRectangle) { 0 };
          crtc->current_state.is_drm_mode_valid = FALSE;
          crtc->current_state.drm_mode = (drmModeModeInfo) { 0 };
        }

      break;
    }

  crtc_gammas = meta_kms_update_get_crtc_gammas (update);
  for (l = crtc_gammas; l; l = l->next)
    {
      MetaKmsCrtcGamma *gamma = l->data;

      if (gamma->crtc != crtc)
        continue;

      clear_gamma_state (crtc);
      crtc->current_state.gamma.size = gamma->size;
      crtc->current_state.gamma.red =
        g_memdup2 (gamma->red, gamma->size * sizeof (uint16_t));
      crtc->current_state.gamma.green =
        g_memdup2 (gamma->green, gamma->size * sizeof (uint16_t));
      crtc->current_state.gamma.blue =
        g_memdup2 (gamma->blue, gamma->size * sizeof (uint16_t));

      break;
    }
}

static void
find_vrr_enabled_prop_id (MetaKmsCrtc       *crtc,
                          MetaKmsImplDevice *impl_device)
{
  int fd = meta_kms_impl_device_get_fd (impl_device);
  drmModeObjectProperties *props;
  uint32_t i;

  props = drmModeObjectGetProperties (fd, crtc->id, DRM_MODE_OBJECT_CRTC);
  if (!props)
    return;

  for (i = 0; i < props->count_props; i++)
    {
      drmModePropertyRes *prop = drmModeGetProperty (fd, props->props[i]);

      if (prop && strcmp (prop->name, "VRR_ENABLED") == 0)
        crtc->vrr_enabled_prop_id = prop->prop_id;
      drmModeFreeProperty (prop);
    }

  drmModeFreeObjectProperties (props);
}

uint32_t
meta_kms_crtc_get_vrr_enabled_prop_id (MetaKmsCrtc *crtc)
{
  return crtc->vrr_enabled_prop_id;
}

MetaKmsCrtc *
meta_kms_crtc_new (MetaKmsImplDevice *impl_device,
                   drmModeCrtc       *drm_crtc,
                   int                idx)
{
  MetaKmsCrtc *crtc;

  crtc = g_object_new (META_TYPE_KMS_CRTC, NULL);
  crtc->device = meta_kms_impl_device_get_device (impl_device);
  crtc->id = drm_crtc->crtc_id;
  crtc->idx = idx;

  meta_kms_crtc_read_state (crtc, impl_device, drm_crtc);
  find_vrr_enabled_prop_id (crtc, impl_device);

  return crtc;
}

static void
meta_kms_crtc_finalize (GObject *object)
{
  MetaKmsCrtc *crtc = META_KMS_CRTC (object);

  clear_gamma_state (crtc);

  g_free (crtc->wanted_gamma.red);
  g_free (crtc->wanted_gamma.green);
  g_free (crtc->wanted_gamma.blue);

  G_OBJECT_CLASS (meta_kms_crtc_parent_class)->finalize (object);
}

static void
meta_kms_crtc_init (MetaKmsCrtc *crtc)
{
}

static void
meta_kms_crtc_class_init (MetaKmsCrtcClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_kms_crtc_finalize;
}
