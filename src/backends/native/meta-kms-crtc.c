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

#include <math.h>
#include <string.h>
#include <drm_mode.h>
#include <xf86drm.h>
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

  /* The CRTC's color pipeline: DEGAMMA_LUT, CTM, GAMMA_LUT (0 if missing) */
  struct {
    uint32_t degamma_lut_prop_id;
    uint64_t degamma_lut_size;
    uint32_t ctm_prop_id;
    uint32_t gamma_lut_prop_id;
    uint64_t gamma_lut_size;

    /* SDR colors are converted to the display's primaries in it: a 2.2
     * decode, this matrix in linear light, a 2.2 encode and the gamma ramp */
    gboolean sdr_to_display;
    float to_display[9];

    /* Our DEGAMMA_LUT and CTM are set */
    gboolean programmed;
    gboolean warned;

    /* The output configuration said whether to map colors: until then a
     * pipeline another Muffin (the login window) set is left alone */
    gboolean decided;
  } color_pipeline;

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
find_crtc_props (MetaKmsCrtc       *crtc,
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

      if (!prop)
        continue;

      if (strcmp (prop->name, "DEGAMMA_LUT") == 0)
        crtc->color_pipeline.degamma_lut_prop_id = prop->prop_id;
      else if (strcmp (prop->name, "DEGAMMA_LUT_SIZE") == 0)
        crtc->color_pipeline.degamma_lut_size = props->prop_values[i];
      else if (strcmp (prop->name, "CTM") == 0)
        crtc->color_pipeline.ctm_prop_id = prop->prop_id;
      else if (strcmp (prop->name, "GAMMA_LUT") == 0)
        crtc->color_pipeline.gamma_lut_prop_id = prop->prop_id;
      else if (strcmp (prop->name, "GAMMA_LUT_SIZE") == 0)
        crtc->color_pipeline.gamma_lut_size = props->prop_values[i];
      drmModeFreeProperty (prop);
    }

  drmModeFreeObjectProperties (props);
}

gboolean
meta_kms_crtc_supports_color_pipeline (MetaKmsCrtc *crtc)
{
  return crtc->color_pipeline.degamma_lut_prop_id &&
         crtc->color_pipeline.degamma_lut_size >= 2 &&
         crtc->color_pipeline.ctm_prop_id &&
         crtc->color_pipeline.gamma_lut_prop_id &&
         crtc->color_pipeline.gamma_lut_size >= 2;
}

/**
 * meta_kms_crtc_set_sdr_to_display:
 * @to_display: (nullable): linear BT.709 to the display's linear RGB, row
 *   major, or %NULL to stop
 *
 * Has the CRTC's color pipeline show SDR colors in the display's primaries
 * from the next gamma update on: see meta_kms_crtc_program_color_pipeline().
 */
void
meta_kms_crtc_set_sdr_to_display (MetaKmsCrtc *crtc,
                                  const float  to_display[9])
{
  crtc->color_pipeline.sdr_to_display = to_display != NULL;
  crtc->color_pipeline.decided = TRUE;
  if (to_display)
    memcpy (crtc->color_pipeline.to_display, to_display,
            sizeof (crtc->color_pipeline.to_display));
}

gboolean
meta_kms_crtc_has_sdr_to_display (MetaKmsCrtc *crtc)
{
  return crtc->color_pipeline.sdr_to_display;
}

gboolean
meta_kms_crtc_is_color_pipeline_programmed (MetaKmsCrtc *crtc)
{
  return crtc->color_pipeline.programmed;
}

/* The same 2.2 power as the view's to-display conversion (see
 * meta_color_transform_get_composite_to_display_snippet()) */
#define SDR_GAMMA 2.2

static uint64_t
get_crtc_prop_value (int      fd,
                     uint32_t crtc_id,
                     uint32_t prop_id)
{
  drmModeObjectProperties *props;
  uint64_t value = 0;
  uint32_t i;

  props = drmModeObjectGetProperties (fd, crtc_id, DRM_MODE_OBJECT_CRTC);
  if (!props)
    return 0;

  for (i = 0; i < props->count_props; i++)
    {
      if (props->props[i] == prop_id)
        value = props->prop_values[i];
    }
  drmModeFreeObjectProperties (props);

  return value;
}

/* Sets a blob property of the CRTC to @data (NULL: no blob), unless it is
 * that already: re-programming the pipeline with the same values makes
 * amdgpu show a frame without it */
static gboolean
set_crtc_blob_prop (int           fd,
                    uint32_t      crtc_id,
                    uint32_t      prop_id,
                    const void   *data,
                    size_t        length,
                    GError      **error)
{
  uint64_t current_blob_id;
  uint32_t blob_id = 0;
  int ret;

  current_blob_id = get_crtc_prop_value (fd, crtc_id, prop_id);
  if (!data && current_blob_id == 0)
    return TRUE;

  if (data && current_blob_id != 0)
    {
      drmModePropertyBlobRes *blob = drmModeGetPropertyBlob (fd, current_blob_id);
      gboolean same = blob && blob->length == length &&
                      memcmp (blob->data, data, length) == 0;

      drmModeFreePropertyBlob (blob);
      if (same)
        return TRUE;
    }

  if (data)
    {
      ret = drmModeCreatePropertyBlob (fd, data, length, &blob_id);
      if (ret != 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                       "Creating a color pipeline blob for CRTC %u failed: %s",
                       crtc_id, g_strerror (-ret));
          return FALSE;
        }
    }

  ret = drmModeObjectSetProperty (fd, crtc_id, DRM_MODE_OBJECT_CRTC,
                                  prop_id, blob_id);
  if (blob_id)
    drmModeDestroyPropertyBlob (fd, blob_id);

  if (ret != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   "Setting the color pipeline of CRTC %u failed: %s",
                   crtc_id, g_strerror (-ret));
      return FALSE;
    }

  return TRUE;
}

/* Whether the CRTC's blob property @prop_id already holds @data */
static gboolean
crtc_blob_prop_is (int         fd,
                   uint32_t    crtc_id,
                   uint32_t    prop_id,
                   const void *data,
                   size_t      length)
{
  uint64_t blob_id = get_crtc_prop_value (fd, crtc_id, prop_id);
  drmModePropertyBlobRes *blob;
  gboolean same;

  if (blob_id == 0)
    return FALSE;

  blob = drmModeGetPropertyBlob (fd, blob_id);
  same = blob && blob->length == length && memcmp (blob->data, data, length) == 0;
  drmModeFreePropertyBlob (blob);

  return same;
}

/* Sets the blob properties that change in one atomic commit, so the display
 * takes them on the same frame (legacy property sets are each a commit of
 * their own, waiting for a vertical blank). FALSE if the device has no
 * atomic support on this file (the caller sets them one by one). */
static gboolean
set_crtc_blob_props_atomic (int          fd,
                            uint32_t     crtc_id,
                            int          n_props,
                            uint32_t    *prop_ids,
                            const void **data,
                            size_t      *lengths)
{
  drmModeAtomicReq *req;
  uint32_t blob_ids[3] = { 0 };
  gboolean any = FALSE;
  int i;
  int ret;

  req = drmModeAtomicAlloc ();
  for (i = 0; i < n_props; i++)
    {
      if (crtc_blob_prop_is (fd, crtc_id, prop_ids[i], data[i], lengths[i]))
        continue;

      if (drmModeCreatePropertyBlob (fd, data[i], lengths[i], &blob_ids[i]) != 0 ||
          drmModeAtomicAddProperty (req, crtc_id, prop_ids[i], blob_ids[i]) < 0)
        {
          ret = -1;
          goto out;
        }
      any = TRUE;
    }

  ret = any ? drmModeAtomicCommit (fd, req, 0, NULL) : 0;

out:
  drmModeAtomicFree (req);
  for (i = 0; i < n_props; i++)
    {
      if (blob_ids[i])
        drmModeDestroyPropertyBlob (fd, blob_ids[i]);
    }

  return ret == 0;
}

/* The gamma ramp's value at @x (0..1), interpolated */
static uint16_t
sample_ramp (int             size,
             const uint16_t *ramp,
             double          x)
{
  double position;
  int i;
  double frac;

  if (size < 2 || !ramp)
    return (uint16_t) (CLAMP (x, 0.0, 1.0) * 65535.0 + 0.5);

  position = CLAMP (x, 0.0, 1.0) * (size - 1);
  i = MIN ((int) position, size - 2);
  frac = position - i;

  return (uint16_t) (ramp[i] * (1.0 - frac) + ramp[i + 1] * frac + 0.5);
}

/**
 * meta_kms_crtc_program_color_pipeline:
 *
 * Shows SDR colors in the display's primaries (see
 * meta_kms_crtc_set_sdr_to_display()), with the gamma ramp @red, @green,
 * @blue applied on top as it would be to the signal: DEGAMMA_LUT decodes
 * the signal (2.2 power), CTM converts its linear BT.709 colors to the
 * display's primaries, GAMMA_LUT encodes them again and applies the ramp.
 * The display engine does that for everything shown, so the output keeps
 * direct scanout.
 */
gboolean
meta_kms_crtc_program_color_pipeline (MetaKmsCrtc     *crtc,
                                      int              fd,
                                      int              size,
                                      const uint16_t  *red,
                                      const uint16_t  *green,
                                      const uint16_t  *blue,
                                      GError         **error)
{
  int degamma_size = (int) crtc->color_pipeline.degamma_lut_size;
  int gamma_size = (int) crtc->color_pipeline.gamma_lut_size;
  g_autofree struct drm_color_lut *degamma = NULL;
  g_autofree struct drm_color_lut *gamma = NULL;
  struct drm_color_ctm ctm;
  int i;

  degamma = g_new0 (struct drm_color_lut, degamma_size);
  for (i = 0; i < degamma_size; i++)
    {
      double l = pow ((double) i / (degamma_size - 1), SDR_GAMMA);
      uint16_t v = (uint16_t) (l * 65535.0 + 0.5);

      degamma[i] = (struct drm_color_lut) { .red = v, .green = v, .blue = v };
    }

  /* S31.32 sign-magnitude, row major */
  for (i = 0; i < 9; i++)
    {
      double v = crtc->color_pipeline.to_display[i];

      ctm.matrix[i] = (v < 0.0 ? (UINT64_C (1) << 63) : 0) |
                      (uint64_t) (fabs (v) * 4294967296.0 + 0.5);
    }

  gamma = g_new0 (struct drm_color_lut, gamma_size);
  for (i = 0; i < gamma_size; i++)
    {
      double e = pow ((double) i / (gamma_size - 1), 1.0 / SDR_GAMMA);

      gamma[i] = (struct drm_color_lut) {
        .red = sample_ramp (size, red, e),
        .green = sample_ramp (size, green, e),
        .blue = sample_ramp (size, blue, e),
      };
    }

  {
    uint32_t prop_ids[] = {
      crtc->color_pipeline.degamma_lut_prop_id,
      crtc->color_pipeline.ctm_prop_id,
      crtc->color_pipeline.gamma_lut_prop_id,
    };
    const void *data[] = { degamma, &ctm, gamma };
    size_t lengths[] = {
      degamma_size * sizeof (*degamma),
      sizeof (ctm),
      gamma_size * sizeof (*gamma),
    };

    if (set_crtc_blob_props_atomic (fd, crtc->id, 3, prop_ids, data, lengths))
      {
        crtc->color_pipeline.programmed = TRUE;
        return TRUE;
      }
  }

  if (!set_crtc_blob_prop (fd, crtc->id,
                           crtc->color_pipeline.degamma_lut_prop_id,
                           degamma, degamma_size * sizeof (*degamma),
                           error) ||
      !set_crtc_blob_prop (fd, crtc->id,
                           crtc->color_pipeline.ctm_prop_id,
                           &ctm, sizeof (ctm), error) ||
      !set_crtc_blob_prop (fd, crtc->id,
                           crtc->color_pipeline.gamma_lut_prop_id,
                           gamma, gamma_size * sizeof (*gamma), error))
    {
      if (!crtc->color_pipeline.warned)
        {
          crtc->color_pipeline.warned = TRUE;
          g_warning ("Could not map SDR colors to the display's primaries "
                     "in CRTC %u's color pipeline: %s", crtc->id,
                     (*error)->message);
        }
      return FALSE;
    }

  crtc->color_pipeline.programmed = TRUE;
  return TRUE;
}

/**
 * meta_kms_crtc_set_gamma_without_mapping:
 *
 * For a gamma update of a CRTC that doesn't map SDR colors (see
 * meta_kms_crtc_set_sdr_to_display()), deals with a decode and matrix in
 * its color pipeline, which we or another display server (the login
 * window's Muffin) set:
 *
 * - Before the output configuration decided whether to map colors, the
 *   pipeline is left as it is and the update is skipped: a Muffin that
 *   starts would otherwise take out the login window's pipeline only to
 *   set it again once configured, showing the display's own colors in
 *   between.
 * - Once decided against mapping, the decode, the matrix and the new ramp
 *   are set in one atomic commit: taking out the decode alone shows a
 *   frame with the matrix and the encode applied to the signal, much too
 *   bright.
 *
 * Returns: %TRUE if the update was dealt with, %FALSE if the caller sets
 *   the gamma ramp the usual way
 */
gboolean
meta_kms_crtc_set_gamma_without_mapping (MetaKmsCrtc    *crtc,
                                         int             fd,
                                         int             size,
                                         const uint16_t *red,
                                         const uint16_t *green,
                                         const uint16_t *blue)
{
  int gamma_size = (int) crtc->color_pipeline.gamma_lut_size;
  g_autofree struct drm_color_lut *gamma = NULL;
  drmModeAtomicReq *req;
  uint32_t gamma_blob_id = 0;
  int ret;
  int i;

  if (!meta_kms_crtc_supports_color_pipeline (crtc))
    return FALSE;

  if (!get_crtc_prop_value (fd, crtc->id,
                            crtc->color_pipeline.degamma_lut_prop_id) &&
      !get_crtc_prop_value (fd, crtc->id,
                            crtc->color_pipeline.ctm_prop_id))
    {
      crtc->color_pipeline.programmed = FALSE;
      return FALSE;
    }

  if (!crtc->color_pipeline.decided)
    return TRUE;

  gamma = g_new0 (struct drm_color_lut, gamma_size);
  for (i = 0; i < gamma_size; i++)
    {
      double x = (double) i / (gamma_size - 1);

      gamma[i] = (struct drm_color_lut) {
        .red = sample_ramp (size, red, x),
        .green = sample_ramp (size, green, x),
        .blue = sample_ramp (size, blue, x),
      };
    }

  req = drmModeAtomicAlloc ();
  ret = drmModeCreatePropertyBlob (fd, gamma, gamma_size * sizeof (*gamma),
                                   &gamma_blob_id);
  if (ret == 0 &&
      drmModeAtomicAddProperty (req, crtc->id,
                                crtc->color_pipeline.degamma_lut_prop_id, 0) >= 0 &&
      drmModeAtomicAddProperty (req, crtc->id,
                                crtc->color_pipeline.ctm_prop_id, 0) >= 0 &&
      drmModeAtomicAddProperty (req, crtc->id,
                                crtc->color_pipeline.gamma_lut_prop_id,
                                gamma_blob_id) >= 0)
    ret = drmModeAtomicCommit (fd, req, 0, NULL);
  else
    ret = -1;
  drmModeAtomicFree (req);
  if (gamma_blob_id)
    drmModeDestroyPropertyBlob (fd, gamma_blob_id);

  if (ret == 0)
    {
      crtc->color_pipeline.programmed = FALSE;
      return TRUE;
    }

  /* Without atomic support on this file: one by one */
  meta_kms_crtc_clear_color_pipeline (crtc, fd);
  return FALSE;
}

/**
 * meta_kms_crtc_clear_color_pipeline:
 *
 * Takes the decode and the matrix out of the CRTC's color pipeline, if we
 * or another display server put them there; the gamma ramp is set the
 * usual way.
 */
void
meta_kms_crtc_clear_color_pipeline (MetaKmsCrtc *crtc,
                                    int          fd)
{
  g_autoptr (GError) error = NULL;

  if (!meta_kms_crtc_supports_color_pipeline (crtc))
    return;

  if (!set_crtc_blob_prop (fd, crtc->id,
                           crtc->color_pipeline.degamma_lut_prop_id,
                           NULL, 0, &error) ||
      !set_crtc_blob_prop (fd, crtc->id,
                           crtc->color_pipeline.ctm_prop_id,
                           NULL, 0, &error))
    g_warning ("Clearing CRTC %u's color pipeline failed: %s", crtc->id,
               error->message);

  crtc->color_pipeline.programmed = FALSE;
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
  find_crtc_props (crtc, impl_device);

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
