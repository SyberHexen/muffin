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

#include "backends/native/meta-kms-connector.h"
#include "backends/native/meta-kms-connector-private.h"

#include <errno.h>

#include "backends/native/meta-gpu-kms.h"
#include "backends/native/meta-kms-crtc.h"
#include "backends/native/meta-kms-device-private.h"
#include "backends/native/meta-kms-impl-device.h"
#include "backends/native/meta-kms-update-private.h"

struct _MetaKmsConnector
{
  GObject parent;

  MetaKmsDevice *device;

  uint32_t id;
  MetaConnectorType type;
  uint32_t type_id;
  char *name;

  drmModeConnection connection;
  MetaKmsConnectorState *current_state;

  uint32_t dpms_prop_id;
  uint32_t underscan_prop_id;
  uint32_t underscan_hborder_prop_id;
  uint32_t underscan_vborder_prop_id;

  /* "Broadcast RGB" and "color format": the KMS enum value for each
   * MetaOutputRGBRange / MetaOutputColorFormat, and which ones exist */
  uint32_t broadcast_rgb_prop_id;
  uint64_t broadcast_rgb_values[META_OUTPUT_RGB_RANGE_LAST + 1];
  unsigned int supported_rgb_ranges;
  /* HDR: "Colorspace" (Default / BT2020_RGB) and "HDR_OUTPUT_METADATA" */
  uint32_t colorspace_prop_id;
  uint64_t colorspace_default_value;
  uint64_t colorspace_bt2020_value;
  gboolean have_colorspace_bt2020;
  uint32_t hdr_metadata_prop_id;
  uint32_t hdr_metadata_blob_id;

  uint32_t color_format_prop_id;
  uint64_t color_format_values[META_OUTPUT_COLOR_FORMAT_LAST + 1];
  unsigned int supported_color_formats;

  /* the values of both after the last successful mode set */
  gboolean have_good_color_properties;
  uint64_t good_broadcast_rgb_value;
  uint64_t good_color_format_value;

  uint32_t edid_blob_id;
  uint32_t tile_blob_id;
};

G_DEFINE_TYPE (MetaKmsConnector, meta_kms_connector, G_TYPE_OBJECT)

MetaKmsDevice *
meta_kms_connector_get_device (MetaKmsConnector *connector)
{
  return connector->device;
}

void
meta_kms_connector_update_set_dpms_state (MetaKmsConnector *connector,
                                          MetaKmsUpdate    *update,
                                          uint64_t          state)
{
  meta_kms_update_set_connector_property (update,
                                          connector,
                                          connector->dpms_prop_id,
                                          state);
}

void
meta_kms_connector_set_underscanning (MetaKmsConnector *connector,
                                      MetaKmsUpdate    *update,
                                      uint64_t          hborder,
                                      uint64_t          vborder)
{
  meta_kms_update_set_connector_property (update,
                                          connector,
                                          connector->underscan_prop_id,
                                          1);
  meta_kms_update_set_connector_property (update,
                                          connector,
                                          connector->underscan_hborder_prop_id,
                                          hborder);
  meta_kms_update_set_connector_property (update,
                                          connector,
                                          connector->underscan_vborder_prop_id,
                                          vborder);
}

void
meta_kms_connector_unset_underscanning (MetaKmsConnector *connector,
                                        MetaKmsUpdate    *update)
{
  meta_kms_update_set_connector_property (update,
                                          connector,
                                          connector->underscan_prop_id,
                                          0);
}

unsigned int
meta_kms_connector_get_supported_rgb_ranges (MetaKmsConnector *connector)
{
  return connector->supported_rgb_ranges;
}

void
meta_kms_connector_set_rgb_range (MetaKmsConnector   *connector,
                                  MetaKmsUpdate      *update,
                                  MetaOutputRGBRange  rgb_range)
{
  if (!(connector->supported_rgb_ranges & (1 << rgb_range)))
    return;

  meta_kms_update_set_connector_property (update,
                                          connector,
                                          connector->broadcast_rgb_prop_id,
                                          connector->broadcast_rgb_values[rgb_range]);
}

gboolean
meta_kms_connector_is_color_property (MetaKmsConnector *connector,
                                      uint32_t          prop_id)
{
  return prop_id &&
         (prop_id == connector->broadcast_rgb_prop_id ||
          prop_id == connector->color_format_prop_id ||
          prop_id == connector->colorspace_prop_id ||
          prop_id == connector->hdr_metadata_prop_id);
}

/* Remember the current RGB range and color format of the connector as the
 * last ones a mode set succeeded with */
void
meta_kms_connector_remember_color_properties_in_impl (MetaKmsConnector *connector,
                                                      int               fd)
{
  drmModeObjectProperties *props;
  gboolean have_rgb = !connector->broadcast_rgb_prop_id;
  gboolean have_format = !connector->color_format_prop_id;
  uint32_t i;

  if (have_rgb && have_format)
    return;

  props = drmModeObjectGetProperties (fd, connector->id,
                                      DRM_MODE_OBJECT_CONNECTOR);
  if (!props)
    return;

  for (i = 0; i < props->count_props; i++)
    {
      if (props->props[i] == connector->broadcast_rgb_prop_id)
        {
          connector->good_broadcast_rgb_value = props->prop_values[i];
          have_rgb = TRUE;
        }
      else if (props->props[i] == connector->color_format_prop_id)
        {
          connector->good_color_format_value = props->prop_values[i];
          have_format = TRUE;
        }
    }
  drmModeFreeObjectProperties (props);

  connector->have_good_color_properties = have_rgb && have_format;
}

/* Set the RGB range and color format right away: to the ones of the last
 * successful mode set, or with 'automatic' (or when there are none yet) to
 * automatic.  Returns TRUE if the connector has either property. */
gboolean
meta_kms_connector_restore_color_properties_in_impl (MetaKmsConnector *connector,
                                                     int               fd,
                                                     gboolean          automatic)
{
  gboolean good = !automatic && connector->have_good_color_properties;
  gboolean restored = FALSE;

  if (connector->supported_rgb_ranges & (1 << META_OUTPUT_RGB_RANGE_AUTO))
    {
      drmModeObjectSetProperty (fd, connector->id, DRM_MODE_OBJECT_CONNECTOR,
                                connector->broadcast_rgb_prop_id,
                                good ? connector->good_broadcast_rgb_value :
                                connector->broadcast_rgb_values[META_OUTPUT_RGB_RANGE_AUTO]);
      restored = TRUE;
    }

  if (connector->supported_color_formats & (1 << META_OUTPUT_COLOR_FORMAT_AUTO))
    {
      drmModeObjectSetProperty (fd, connector->id, DRM_MODE_OBJECT_CONNECTOR,
                                connector->color_format_prop_id,
                                good ? connector->good_color_format_value :
                                connector->color_format_values[META_OUTPUT_COLOR_FORMAT_AUTO]);
      restored = TRUE;
    }

  /* A refused HDR signal goes back to SDR either way */
  if (connector->colorspace_prop_id)
    {
      drmModeObjectSetProperty (fd, connector->id, DRM_MODE_OBJECT_CONNECTOR,
                                connector->colorspace_prop_id,
                                connector->colorspace_default_value);
      restored = TRUE;
    }
  if (connector->hdr_metadata_prop_id)
    {
      drmModeObjectSetProperty (fd, connector->id, DRM_MODE_OBJECT_CONNECTOR,
                                connector->hdr_metadata_prop_id, 0);
      restored = TRUE;
    }

  return restored;
}

unsigned int
meta_kms_connector_get_supported_color_modes (MetaKmsConnector *connector)
{
  if (!connector->colorspace_prop_id)
    return 0;

  if (connector->hdr_metadata_prop_id && connector->have_colorspace_bt2020)
    return (1 << META_OUTPUT_COLOR_MODE_DEFAULT) |
           (1 << META_OUTPUT_COLOR_MODE_BT2100);

  return 1 << META_OUTPUT_COLOR_MODE_DEFAULT;
}

/* HDR10 static metadata.  Without content metadata, describe a 1000 nit
 * BT.2020 mastering display, the most common HDR10 grading target. */
static uint32_t
ensure_hdr_metadata_blob (MetaKmsConnector *connector,
                          int               fd)
{
  struct hdr_output_metadata metadata = { 0 };
  struct hdr_metadata_infoframe *info = &metadata.hdmi_metadata_type1;
  int ret;

  if (connector->hdr_metadata_blob_id)
    return connector->hdr_metadata_blob_id;

  metadata.metadata_type = 0;           /* HDMI_STATIC_METADATA_TYPE1 */
  info->eotf = 2;                       /* HDMI_EOTF_SMPTE_ST2084 */
  info->metadata_type = 0;
  /* chromaticities in units of 0.00002: BT.2020 red, green, blue, D65 */
  info->display_primaries[0].x = 35400;
  info->display_primaries[0].y = 14600;
  info->display_primaries[1].x = 8500;
  info->display_primaries[1].y = 39850;
  info->display_primaries[2].x = 6550;
  info->display_primaries[2].y = 2300;
  info->white_point.x = 15635;
  info->white_point.y = 16450;
  info->max_display_mastering_luminance = 1000;   /* cd/m² */
  info->min_display_mastering_luminance = 50;     /* 0.0001 cd/m² */
  info->max_cll = 1000;
  info->max_fall = 400;

  ret = drmModeCreatePropertyBlob (fd, &metadata, sizeof (metadata),
                                   &connector->hdr_metadata_blob_id);
  if (ret != 0)
    {
      g_warning ("Failed to create HDR metadata for connector %s: %s",
                 connector->name, g_strerror (-ret));
      connector->hdr_metadata_blob_id = 0;
    }

  return connector->hdr_metadata_blob_id;
}

/* 'fd' is the device's fd: the HDR metadata blob is created on it */
void
meta_kms_connector_set_color_mode (MetaKmsConnector    *connector,
                                   MetaKmsUpdate       *update,
                                   int                  fd,
                                   MetaOutputColorMode  color_mode)
{
  if (!(meta_kms_connector_get_supported_color_modes (connector) &
        (1 << color_mode)))
    return;

  if (color_mode == META_OUTPUT_COLOR_MODE_BT2100)
    {
      uint32_t blob_id = ensure_hdr_metadata_blob (connector, fd);

      if (!blob_id)
        return;
      meta_kms_update_set_connector_property (update, connector,
                                              connector->colorspace_prop_id,
                                              connector->colorspace_bt2020_value);
      meta_kms_update_set_connector_property (update, connector,
                                              connector->hdr_metadata_prop_id,
                                              blob_id);
    }
  else
    {
      meta_kms_update_set_connector_property (update, connector,
                                              connector->colorspace_prop_id,
                                              connector->colorspace_default_value);
      if (connector->hdr_metadata_prop_id)
        meta_kms_update_set_connector_property (update, connector,
                                                connector->hdr_metadata_prop_id,
                                                0);
    }
}

unsigned int
meta_kms_connector_get_supported_color_formats (MetaKmsConnector *connector)
{
  return connector->supported_color_formats;
}

void
meta_kms_connector_set_color_format (MetaKmsConnector      *connector,
                                     MetaKmsUpdate         *update,
                                     MetaOutputColorFormat  color_format)
{
  if (!(connector->supported_color_formats & (1 << color_format)))
    return;

  meta_kms_update_set_connector_property (update,
                                          connector,
                                          connector->color_format_prop_id,
                                          connector->color_format_values[color_format]);
}

MetaConnectorType
meta_kms_connector_get_connector_type (MetaKmsConnector *connector)
{
  return connector->type;
}

uint32_t
meta_kms_connector_get_id (MetaKmsConnector *connector)
{
  return connector->id;
}

const char *
meta_kms_connector_get_name (MetaKmsConnector *connector)
{
  return connector->name;
}

gboolean
meta_kms_connector_can_clone (MetaKmsConnector *connector,
                              MetaKmsConnector *other_connector)
{
  MetaKmsConnectorState *state = connector->current_state;
  MetaKmsConnectorState *other_state = other_connector->current_state;

  if (state->common_possible_clones == 0 ||
      other_state->common_possible_clones == 0)
    return FALSE;

  if (state->encoder_device_idxs != other_state->encoder_device_idxs)
    return FALSE;

  return TRUE;
}

const MetaKmsConnectorState *
meta_kms_connector_get_current_state (MetaKmsConnector *connector)
{
  return connector->current_state;
}

gboolean
meta_kms_connector_is_underscanning_supported (MetaKmsConnector *connector)
{
  return connector->underscan_prop_id != 0;
}

static void
set_panel_orientation (MetaKmsConnectorState *state,
                       drmModePropertyPtr     prop,
                       uint64_t               orientation)
{
  const char *name;

  name = prop->enums[orientation].name;
  if (strcmp (name, "Upside Down") == 0)
    {
      state->panel_orientation_transform = META_MONITOR_TRANSFORM_180;
    }
  else if (strcmp (name, "Left Side Up") == 0)
    {
      /* Left side up, rotate 90 degrees counter clockwise to correct */
      state->panel_orientation_transform = META_MONITOR_TRANSFORM_90;
    }
  else if (strcmp (name, "Right Side Up") == 0)
    {
      /* Right side up, rotate 270 degrees counter clockwise to correct */
      state->panel_orientation_transform = META_MONITOR_TRANSFORM_270;
    }
  else
    {
      state->panel_orientation_transform = META_MONITOR_TRANSFORM_NORMAL;
    }
}

static void
state_set_properties (MetaKmsConnectorState *state,
                      MetaKmsImplDevice     *impl_device,
                      drmModeConnector      *drm_connector)
{
  int fd;
  int i;

  fd = meta_kms_impl_device_get_fd (impl_device);

  for (i = 0; i < drm_connector->count_props; i++)
    {
      drmModePropertyPtr prop;

      prop = drmModeGetProperty (fd, drm_connector->props[i]);
      if (!prop)
        continue;

      if ((prop->flags & DRM_MODE_PROP_RANGE) &&
          strcmp (prop->name, "suggested X") == 0)
        state->suggested_x = drm_connector->prop_values[i];
      else if ((prop->flags & DRM_MODE_PROP_RANGE) &&
               strcmp (prop->name, "suggested Y") == 0)
        state->suggested_y = drm_connector->prop_values[i];
      else if ((prop->flags & DRM_MODE_PROP_RANGE) &&
               strcmp (prop->name, "hotplug_mode_update") == 0)
        state->hotplug_mode_update = drm_connector->prop_values[i];
      else if (strcmp (prop->name, "scaling mode") == 0)
        state->has_scaling = TRUE;
      else if ((prop->flags & DRM_MODE_PROP_ENUM) &&
               strcmp (prop->name, "panel orientation") == 0)
        set_panel_orientation (state, prop, drm_connector->prop_values[i]);

      drmModeFreeProperty (prop);
    }
}

static CoglSubpixelOrder
drm_subpixel_order_to_cogl_subpixel_order (drmModeSubPixel subpixel)
{
  switch (subpixel)
    {
    case DRM_MODE_SUBPIXEL_NONE:
      return COGL_SUBPIXEL_ORDER_NONE;
      break;
    case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB:
      return COGL_SUBPIXEL_ORDER_HORIZONTAL_RGB;
      break;
    case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR:
      return COGL_SUBPIXEL_ORDER_HORIZONTAL_BGR;
      break;
    case DRM_MODE_SUBPIXEL_VERTICAL_RGB:
      return COGL_SUBPIXEL_ORDER_VERTICAL_RGB;
      break;
    case DRM_MODE_SUBPIXEL_VERTICAL_BGR:
      return COGL_SUBPIXEL_ORDER_VERTICAL_BGR;
      break;
    case DRM_MODE_SUBPIXEL_UNKNOWN:
      return COGL_SUBPIXEL_ORDER_UNKNOWN;
    }
  return COGL_SUBPIXEL_ORDER_UNKNOWN;
}

static void
state_set_edid (MetaKmsConnectorState *state,
                MetaKmsConnector      *connector,
                MetaKmsImplDevice     *impl_device,
                uint32_t               blob_id)
{
  int fd;
  drmModePropertyBlobPtr edid_blob;
  GBytes *edid_data;

  fd = meta_kms_impl_device_get_fd (impl_device);
  edid_blob = drmModeGetPropertyBlob (fd, blob_id);
  if (!edid_blob)
    {
      g_warning ("Failed to read EDID of connector %s: %s",
                 connector->name, g_strerror (errno));
      return;
    }

   edid_data = g_bytes_new (edid_blob->data, edid_blob->length);
   drmModeFreePropertyBlob (edid_blob);

   state->edid_data = edid_data;
}

static void
state_set_tile_info (MetaKmsConnectorState *state,
                     MetaKmsConnector      *connector,
                     MetaKmsImplDevice     *impl_device,
                     uint32_t               blob_id)
{
  int fd;
  drmModePropertyBlobPtr tile_blob;

  state->tile_info = (MetaTileInfo) { 0 };

  fd = meta_kms_impl_device_get_fd (impl_device);
  tile_blob = drmModeGetPropertyBlob (fd, blob_id);
  if (!tile_blob)
    {
      g_warning ("Failed to read TILE of connector %s: %s",
                 connector->name, strerror (errno));
      return;
    }

  if (tile_blob->length > 0)
    {
      if (sscanf ((char *) tile_blob->data, "%d:%d:%d:%d:%d:%d:%d:%d",
                  &state->tile_info.group_id,
                  &state->tile_info.flags,
                  &state->tile_info.max_h_tiles,
                  &state->tile_info.max_v_tiles,
                  &state->tile_info.loc_h_tile,
                  &state->tile_info.loc_v_tile,
                  &state->tile_info.tile_w,
                  &state->tile_info.tile_h) != 8)
        {
          g_warning ("Couldn't understand TILE property blob of connector %s",
                     connector->name);
          state->tile_info = (MetaTileInfo) { 0 };
        }
    }

  drmModeFreePropertyBlob (tile_blob);
}

static void
state_set_blobs (MetaKmsConnectorState *state,
                 MetaKmsConnector      *connector,
                 MetaKmsImplDevice     *impl_device,
                 drmModeConnector      *drm_connector)
{
  int fd;
  int i;

  fd = meta_kms_impl_device_get_fd (impl_device);

  for (i = 0; i < drm_connector->count_props; i++)
    {
      drmModePropertyPtr prop;

      prop = drmModeGetProperty (fd, drm_connector->props[i]);
      if (!prop)
        continue;

      if (prop->flags & DRM_MODE_PROP_BLOB)
        {
          uint32_t blob_id;

          blob_id = drm_connector->prop_values[i];

          if (blob_id)
            {
              if (strcmp (prop->name, "EDID") == 0)
                state_set_edid (state, connector, impl_device, blob_id);
              else if (strcmp (prop->name, "TILE") == 0)
                state_set_tile_info (state, connector, impl_device, blob_id);
            }
        }

      drmModeFreeProperty (prop);
    }
}

static void
state_set_physical_dimensions (MetaKmsConnectorState *state,
                               drmModeConnector      *drm_connector)
{
  state->width_mm = drm_connector->mmWidth;
  state->height_mm = drm_connector->mmHeight;
}

static void
state_set_modes (MetaKmsConnectorState *state,
                 drmModeConnector      *drm_connector)
{
  state->modes =
    g_memdup2 (drm_connector->modes,
              drm_connector->count_modes * sizeof (drmModeModeInfo));
  state->n_modes = drm_connector->count_modes;
}

static void
set_encoder_device_idx_bit (uint32_t          *encoder_device_idxs,
                            uint32_t           encoder_id,
                            MetaKmsImplDevice *impl_device,
                            drmModeRes        *drm_resources)
{
  int fd;
  int i;

  fd = meta_kms_impl_device_get_fd (impl_device);

  for (i = 0; i < drm_resources->count_encoders; i++)
    {
      drmModeEncoder *drm_encoder;

      drm_encoder = drmModeGetEncoder (fd, drm_resources->encoders[i]);
      if (!drm_encoder)
        continue;

      if (drm_encoder->encoder_id == encoder_id)
        {
          *encoder_device_idxs |= (1 << i);
          drmModeFreeEncoder (drm_encoder);
          break;
        }

      drmModeFreeEncoder (drm_encoder);
    }
}

static void
state_set_crtc_state (MetaKmsConnectorState *state,
                      drmModeConnector      *drm_connector,
                      MetaKmsImplDevice     *impl_device,
                      drmModeRes            *drm_resources)
{
  int fd;
  int i;
  uint32_t common_possible_crtcs;
  uint32_t common_possible_clones;
  uint32_t encoder_device_idxs;

  fd = meta_kms_impl_device_get_fd (impl_device);

  common_possible_crtcs = UINT32_MAX;
  common_possible_clones = UINT32_MAX;
  encoder_device_idxs = 0;
  for (i = 0; i < drm_connector->count_encoders; i++)
    {
      drmModeEncoder *drm_encoder;

      drm_encoder = drmModeGetEncoder (fd, drm_connector->encoders[i]);
      if (!drm_encoder)
        continue;

      common_possible_crtcs &= drm_encoder->possible_crtcs;
      common_possible_clones &= drm_encoder->possible_clones;

      set_encoder_device_idx_bit (&encoder_device_idxs,
                                  drm_encoder->encoder_id,
                                  impl_device,
                                  drm_resources);

      if (drm_connector->encoder_id == drm_encoder->encoder_id)
        state->current_crtc_id = drm_encoder->crtc_id;

      drmModeFreeEncoder (drm_encoder);
    }

  state->common_possible_crtcs = common_possible_crtcs;
  state->common_possible_clones = common_possible_clones;
  state->encoder_device_idxs = encoder_device_idxs;
}

static MetaKmsConnectorState *
meta_kms_connector_state_new (void)
{
  MetaKmsConnectorState *state;

  state = g_new0 (MetaKmsConnectorState, 1);
  state->suggested_x = -1;
  state->suggested_y = -1;

  return state;
}

static void
meta_kms_connector_state_free (MetaKmsConnectorState *state)
{
  g_clear_pointer (&state->edid_data, g_bytes_unref);
  g_free (state->modes);
  g_free (state);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MetaKmsConnectorState,
                               meta_kms_connector_state_free);

static gboolean
edid_equal (GBytes *edid,
            GBytes *other_edid)
{
  if (!edid || !other_edid)
    return edid == other_edid;

  return g_bytes_equal (edid, other_edid);
}

static gboolean
modes_equal (MetaKmsConnectorState *state,
             MetaKmsConnectorState *other_state)
{
  int i;

  if (state->n_modes != other_state->n_modes)
    return FALSE;

  for (i = 0; i < state->n_modes; i++)
    {
      if (!meta_drm_mode_equal (&state->modes[i], &other_state->modes[i]))
        return FALSE;
    }

  return TRUE;
}

/* The CRTC assignment is deliberately left out: it only changes through our
 * own mode sets, and a mismatch between the predicted and the read back
 * assignment must not turn the uevent our mode set triggers into a hotplug. */
static gboolean
monitor_state_equal (MetaKmsConnectorState *state,
                     MetaKmsConnectorState *other_state)
{
  return (state->common_possible_crtcs == other_state->common_possible_crtcs &&
          state->common_possible_clones == other_state->common_possible_clones &&
          state->encoder_device_idxs == other_state->encoder_device_idxs &&
          state->width_mm == other_state->width_mm &&
          state->height_mm == other_state->height_mm &&
          state->has_scaling == other_state->has_scaling &&
          state->subpixel_order == other_state->subpixel_order &&
          state->suggested_x == other_state->suggested_x &&
          state->suggested_y == other_state->suggested_y &&
          state->hotplug_mode_update == other_state->hotplug_mode_update &&
          state->panel_orientation_transform ==
            other_state->panel_orientation_transform &&
          meta_tile_info_equal (&state->tile_info, &other_state->tile_info) &&
          edid_equal (state->edid_data, other_state->edid_data) &&
          modes_equal (state, other_state));
}

static MetaKmsResourceChanges
meta_kms_connector_read_state (MetaKmsConnector  *connector,
                               MetaKmsImplDevice *impl_device,
                               drmModeConnector  *drm_connector,
                               drmModeRes        *drm_resources)
{
  g_autoptr (MetaKmsConnectorState) state = NULL;
  g_autoptr (MetaKmsConnectorState) current_state = NULL;
  MetaKmsResourceChanges changes = META_KMS_RESOURCE_CHANGE_NONE;

  current_state = g_steal_pointer (&connector->current_state);

  if (drm_connector->connection != connector->connection)
    {
      connector->connection = drm_connector->connection;
      changes = META_KMS_RESOURCE_CHANGE_FULL;
    }

  if (drm_connector->connection != DRM_MODE_CONNECTED)
    return changes;

  state = meta_kms_connector_state_new ();

  state_set_blobs (state, connector, impl_device, drm_connector);

  state_set_properties (state, impl_device, drm_connector);

  state->subpixel_order =
    drm_subpixel_order_to_cogl_subpixel_order (drm_connector->subpixel);

  state_set_physical_dimensions (state, drm_connector);

  state_set_modes (state, drm_connector);

  state_set_crtc_state (state, drm_connector, impl_device, drm_resources);

  if (!current_state || !monitor_state_equal (current_state, state))
    changes = META_KMS_RESOURCE_CHANGE_FULL;

  connector->current_state = g_steal_pointer (&state);

  return changes;
}

MetaKmsResourceChanges
meta_kms_connector_update_state (MetaKmsConnector *connector,
                                 drmModeRes       *drm_resources)
{
  MetaKmsImplDevice *impl_device;
  drmModeConnector *drm_connector;
  MetaKmsResourceChanges changes;

  impl_device = meta_kms_device_get_impl_device (connector->device);
  drm_connector = drmModeGetConnector (meta_kms_impl_device_get_fd (impl_device),
                                       connector->id);
  if (!drm_connector)
    {
      g_clear_pointer (&connector->current_state,
                       meta_kms_connector_state_free);
      connector->connection = DRM_MODE_UNKNOWNCONNECTION;
      return META_KMS_RESOURCE_CHANGE_FULL;
    }

  changes = meta_kms_connector_read_state (connector, impl_device,
                                           drm_connector,
                                           drm_resources);
  drmModeFreeConnector (drm_connector);

  return changes;
}

void
meta_kms_connector_predict_state (MetaKmsConnector *connector,
                                  MetaKmsUpdate    *update)
{
  GList *mode_sets;
  GList *l;

  if (!connector->current_state)
    return;

  mode_sets = meta_kms_update_get_mode_sets (update);
  for (l = mode_sets; l; l = l->next)
    {
      MetaKmsModeSet *mode_set = l->data;
      MetaKmsCrtc *crtc;

      if (!g_list_find (mode_set->connectors, connector))
        continue;

      crtc = mode_set->crtc;
      if (crtc)
        connector->current_state->current_crtc_id = meta_kms_crtc_get_id (crtc);
      else
        connector->current_state->current_crtc_id = 0;

      break;
    }
}

static MetaOutputRGBRange
rgb_range_from_kms_name (const char *name)
{
  if (g_ascii_strcasecmp (name, "Automatic") == 0)
    return META_OUTPUT_RGB_RANGE_AUTO;
  else if (g_ascii_strcasecmp (name, "Full") == 0)
    return META_OUTPUT_RGB_RANGE_FULL;
  else if (g_ascii_strncasecmp (name, "Limited", strlen ("Limited")) == 0)
    return META_OUTPUT_RGB_RANGE_LIMITED;
  else
    return META_OUTPUT_RGB_RANGE_UNKNOWN;
}

static MetaOutputColorFormat
color_format_from_kms_name (const char *name)
{
  if (g_ascii_strcasecmp (name, "AUTO") == 0)
    return META_OUTPUT_COLOR_FORMAT_AUTO;
  else if (g_ascii_strcasecmp (name, "RGB") == 0)
    return META_OUTPUT_COLOR_FORMAT_RGB;
  else if (g_ascii_strcasecmp (name, "YUV 4:4:4") == 0 ||
           g_ascii_strcasecmp (name, "YCbCr 4:4:4") == 0)
    return META_OUTPUT_COLOR_FORMAT_YCBCR444;
  else if (g_ascii_strcasecmp (name, "YUV 4:2:2") == 0 ||
           g_ascii_strcasecmp (name, "YCbCr 4:2:2") == 0)
    return META_OUTPUT_COLOR_FORMAT_YCBCR422;
  else if (g_ascii_strcasecmp (name, "YUV 4:2:0") == 0 ||
           g_ascii_strcasecmp (name, "YCbCr 4:2:0") == 0)
    return META_OUTPUT_COLOR_FORMAT_YCBCR420;
  else
    return META_OUTPUT_COLOR_FORMAT_UNKNOWN;
}

static void
find_property_ids (MetaKmsConnector  *connector,
                   MetaKmsImplDevice *impl_device,
                   drmModeConnector  *drm_connector)
{
  int fd;
  int i;

  fd = meta_kms_impl_device_get_fd (impl_device);

  for (i = 0; i < drm_connector->count_props; i++)
    {
      drmModePropertyPtr prop;

      prop = drmModeGetProperty (fd, drm_connector->props[i]);
      if (!prop)
        continue;

      if ((prop->flags & DRM_MODE_PROP_ENUM) &&
          strcmp (prop->name, "DPMS") == 0)
        connector->dpms_prop_id = prop->prop_id;
      else if ((prop->flags & DRM_MODE_PROP_ENUM) &&
               strcmp (prop->name, "underscan") == 0)
        connector->underscan_prop_id = prop->prop_id;
      else if ((prop->flags & DRM_MODE_PROP_RANGE) &&
               strcmp (prop->name, "underscan hborder") == 0)
        connector->underscan_hborder_prop_id = prop->prop_id;
      else if ((prop->flags & DRM_MODE_PROP_RANGE) &&
               strcmp (prop->name, "underscan vborder") == 0)
        connector->underscan_vborder_prop_id = prop->prop_id;
      else if ((prop->flags & DRM_MODE_PROP_ENUM) &&
               strcmp (prop->name, "Broadcast RGB") == 0)
        {
          int j;

          connector->broadcast_rgb_prop_id = prop->prop_id;
          connector->supported_rgb_ranges = 0;
          for (j = 0; j < prop->count_enums; j++)
            {
              MetaOutputRGBRange range =
                rgb_range_from_kms_name (prop->enums[j].name);

              if (range == META_OUTPUT_RGB_RANGE_UNKNOWN)
                continue;
              connector->broadcast_rgb_values[range] = prop->enums[j].value;
              connector->supported_rgb_ranges |= 1 << range;
            }
        }
      else if ((prop->flags & DRM_MODE_PROP_ENUM) &&
               strcmp (prop->name, "Colorspace") == 0)
        {
          int j;

          connector->colorspace_prop_id = prop->prop_id;
          for (j = 0; j < prop->count_enums; j++)
            {
              if (strcmp (prop->enums[j].name, "Default") == 0)
                {
                  connector->colorspace_default_value = prop->enums[j].value;
                }
              else if (strcmp (prop->enums[j].name, "BT2020_RGB") == 0)
                {
                  connector->colorspace_bt2020_value = prop->enums[j].value;
                  connector->have_colorspace_bt2020 = TRUE;
                }
            }
        }
      else if ((prop->flags & DRM_MODE_PROP_BLOB) &&
               strcmp (prop->name, "HDR_OUTPUT_METADATA") == 0)
        {
          connector->hdr_metadata_prop_id = prop->prop_id;
        }
      else if ((prop->flags & DRM_MODE_PROP_ENUM) &&
               strcmp (prop->name, "color format") == 0)
        {
          int j;

          connector->color_format_prop_id = prop->prop_id;
          connector->supported_color_formats = 0;
          for (j = 0; j < prop->count_enums; j++)
            {
              MetaOutputColorFormat format =
                color_format_from_kms_name (prop->enums[j].name);

              if (format == META_OUTPUT_COLOR_FORMAT_UNKNOWN)
                continue;
              connector->color_format_values[format] = prop->enums[j].value;
              connector->supported_color_formats |= 1 << format;
            }
        }

      drmModeFreeProperty (prop);
    }
}

static char *
make_connector_name (drmModeConnector *drm_connector)
{
  static const char * const connector_type_names[] = {
    "None",
    "VGA",
    "DVI-I",
    "DVI-D",
    "DVI-A",
    "Composite",
    "SVIDEO",
    "LVDS",
    "Component",
    "DIN",
    "DP",
    "HDMI",
    "HDMI-B",
    "TV",
    "eDP",
    "Virtual",
    "DSI",
  };

  if (drm_connector->connector_type < G_N_ELEMENTS (connector_type_names))
    return g_strdup_printf ("%s-%d",
                            connector_type_names[drm_connector->connector_type],
                            drm_connector->connector_type_id);
  else
    return g_strdup_printf ("Unknown%d-%d",
                            drm_connector->connector_type,
                            drm_connector->connector_type_id);
}

gboolean
meta_kms_connector_is_same_as (MetaKmsConnector *connector,
                               drmModeConnector *drm_connector)
{
  return (connector->id == drm_connector->connector_id &&
          connector->type == drm_connector->connector_type &&
          connector->type_id == drm_connector->connector_type_id);
}

MetaKmsConnector *
meta_kms_connector_new (MetaKmsImplDevice *impl_device,
                        drmModeConnector  *drm_connector,
                        drmModeRes        *drm_resources)
{
  MetaKmsConnector *connector;

  connector = g_object_new (META_TYPE_KMS_CONNECTOR, NULL);
  connector->device = meta_kms_impl_device_get_device (impl_device);
  connector->id = drm_connector->connector_id;
  connector->type = (MetaConnectorType) drm_connector->connector_type;
  connector->type_id = drm_connector->connector_type_id;
  connector->name = make_connector_name (drm_connector);

  find_property_ids (connector, impl_device, drm_connector);

  meta_kms_connector_read_state (connector, impl_device,
                                 drm_connector,
                                 drm_resources);

  return connector;
}

static void
meta_kms_connector_finalize (GObject *object)
{
  MetaKmsConnector *connector = META_KMS_CONNECTOR (object);

  g_clear_pointer (&connector->current_state, meta_kms_connector_state_free);
  g_free (connector->name);

  G_OBJECT_CLASS (meta_kms_connector_parent_class)->finalize (object);
}

static void
meta_kms_connector_init (MetaKmsConnector *connector)
{
}

static void
meta_kms_connector_class_init (MetaKmsConnectorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_kms_connector_finalize;
}
