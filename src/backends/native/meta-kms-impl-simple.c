/*
 * Copyright (C) 2018-2019 Red Hat
 * Copyright (C) 2019-2020 DisplayLink (UK) Ltd.
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

#include "backends/native/meta-kms-impl-simple.h"

#include <errno.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backends/native/meta-kms-connector.h"
#include "backends/native/meta-kms-crtc.h"
#include "backends/native/meta-kms-device-private.h"
#include "backends/native/meta-kms-page-flip-private.h"
#include "backends/native/meta-kms-plane.h"
#include "backends/native/meta-kms-private.h"
#include "backends/native/meta-kms-update-private.h"
#include "backends/native/meta-kms-utils.h"

typedef struct _CachedModeSet
{
  GList *connectors;
  drmModeModeInfo *drm_mode;
} CachedModeSet;

struct _MetaKmsImplSimple
{
  MetaKmsImpl parent;

  GSource *mode_set_fallback_feedback_source;
  GList *mode_set_fallback_page_flip_datas;

  GList *pending_page_flip_retries;
  GSource *retry_page_flips_source;

  GList *postponed_page_flip_datas;
  GList *postponed_mode_set_fallback_datas;

  GHashTable *cached_mode_sets;

  /* MetaKmsDevice -> AtomicDevice, for every device seen with a cursor */
  GHashTable *atomic_devices;
};

G_DEFINE_TYPE (MetaKmsImplSimple, meta_kms_impl_simple,
               META_TYPE_KMS_IMPL)

static void
flush_postponed_page_flip_datas (MetaKmsImplSimple *impl_simple);

static int
atomic_page_flip (MetaKmsImplSimple   *impl_simple,
                  MetaKmsDevice       *device,
                  MetaKmsCrtc         *crtc,
                  uint32_t             fb_id,
                  MetaKmsPageFlipData *page_flip_data);

MetaKmsImplSimple *
meta_kms_impl_simple_new (MetaKms  *kms,
                          GError  **error)
{
  return g_object_new (META_TYPE_KMS_IMPL_SIMPLE,
                       "kms", kms,
                       NULL);
}

static gboolean
process_connector_property (MetaKmsImpl    *impl,
                            MetaKmsUpdate  *update,
                            gpointer        update_entry,
                            GError        **error)
{
  MetaKmsConnectorProperty *connector_property = update_entry;
  MetaKmsConnector *connector = connector_property->connector;
  MetaKmsDevice *device = meta_kms_connector_get_device (connector);
  MetaKmsImplDevice *impl_device = meta_kms_device_get_impl_device (device);
  int fd;
  int ret;

  fd = meta_kms_impl_device_get_fd (impl_device);

  ret = drmModeObjectSetProperty (fd,
                                  meta_kms_connector_get_id (connector),
                                  DRM_MODE_OBJECT_CONNECTOR,
                                  connector_property->prop_id,
                                  connector_property->value);
  if (ret != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   "Failed to set connector %u property %u: %s",
                   meta_kms_connector_get_id (connector),
                   connector_property->prop_id,
                   g_strerror (-ret));
      return FALSE;
    }

  return TRUE;
}

static gboolean
process_plane_property (MetaKmsImpl      *impl,
                        MetaKmsPlane     *plane,
                        MetaKmsProperty  *prop,
                        GError          **error)
{
  MetaKmsDevice *device = meta_kms_plane_get_device (plane);
  MetaKmsImplDevice *impl_device = meta_kms_device_get_impl_device (device);
  int fd;
  int ret;

  fd = meta_kms_impl_device_get_fd (impl_device);

  ret = drmModeObjectSetProperty (fd,
                                  meta_kms_plane_get_id (plane),
                                  DRM_MODE_OBJECT_PLANE,
                                  prop->prop_id,
                                  prop->value);
  if (ret != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   "Failed to set plane %u property %u: %s",
                   meta_kms_plane_get_id (plane),
                   prop->prop_id,
                   g_strerror (-ret));
      return FALSE;
    }

  return TRUE;
}

static CachedModeSet *
cached_mode_set_new (GList                 *connectors,
                     const drmModeModeInfo *drm_mode)
{
  CachedModeSet *cached_mode_set;

  cached_mode_set = g_new0 (CachedModeSet, 1);
  *cached_mode_set = (CachedModeSet) {
    .connectors = g_list_copy (connectors),
    .drm_mode = g_memdup2 (drm_mode, sizeof *drm_mode),
  };

  return cached_mode_set;
}

static void
cached_mode_set_free (CachedModeSet *cached_mode_set)
{
  g_list_free (cached_mode_set->connectors);
  g_free (cached_mode_set->drm_mode);
  g_free (cached_mode_set);
}

static void
fill_connector_ids_array (GList     *connectors,
                          uint32_t **out_connectors,
                          int       *out_n_connectors)
{
  GList *l;
  int i;

  *out_n_connectors = g_list_length (connectors);
  *out_connectors = g_new0 (uint32_t, *out_n_connectors);
  i = 0;
  for (l = connectors; l; l = l->next)
    {
      MetaKmsConnector *connector = l->data;

      (*out_connectors)[i++] = meta_kms_connector_get_id (connector);
    }
}

static gboolean
process_mode_set (MetaKmsImpl     *impl,
                  MetaKmsUpdate   *update,
                  gpointer         update_entry,
                  GError         **error)
{
  MetaKmsModeSet *mode_set = update_entry;
  MetaKmsImplSimple *impl_simple = META_KMS_IMPL_SIMPLE (impl);
  MetaKmsCrtc *crtc = mode_set->crtc;
  MetaKmsDevice *device = meta_kms_crtc_get_device (crtc);
  MetaKmsImplDevice *impl_device = meta_kms_device_get_impl_device (device);
  g_autofree uint32_t *connectors = NULL;
  int n_connectors;
  MetaKmsPlaneAssignment *plane_assignment;
  uint32_t x, y;
  uint32_t fb_id;
  int fd;
  int ret;

  crtc = mode_set->crtc;

  if (mode_set->drm_mode)
    {
      GList *l;

      fill_connector_ids_array (mode_set->connectors,
                                &connectors,
                                &n_connectors);

      plane_assignment = meta_kms_update_get_primary_plane_assignment (update,
                                                                       crtc);
      if (!plane_assignment)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Missing primary plane assignment for legacy mode set on CRTC %u",
                       meta_kms_crtc_get_id (crtc));
          return FALSE;
        }

      x = meta_fixed_16_to_int (plane_assignment->src_rect.x);
      y = meta_fixed_16_to_int (plane_assignment->src_rect.y);

      for (l = plane_assignment->plane_properties; l; l = l->next)
        {
          MetaKmsProperty *prop = l->data;

          if (!process_plane_property (impl, plane_assignment->plane,
                                       prop, error))
            return FALSE;
        }

      fb_id = plane_assignment->fb_id;
    }
  else
    {
      x = y = 0;
      n_connectors = 0;
      connectors = NULL;
      fb_id = 0;
    }

  fd = meta_kms_impl_device_get_fd (impl_device);
  ret = drmModeSetCrtc (fd,
                        meta_kms_crtc_get_id (crtc),
                        fb_id,
                        x, y,
                        connectors, n_connectors,
                        mode_set->drm_mode);
  if (ret != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   "Failed to set mode %s on CRTC %u: %s",
                   mode_set->drm_mode ? mode_set->drm_mode->name : "off",
                   meta_kms_crtc_get_id (crtc),
                   g_strerror (-ret));
      return FALSE;
    }

  if (mode_set->drm_mode)
    {
      g_hash_table_replace (impl_simple->cached_mode_sets,
                            crtc,
                            cached_mode_set_new (mode_set->connectors,
                                                 mode_set->drm_mode));
    }
  else
    {
      g_hash_table_remove (impl_simple->cached_mode_sets, crtc);
    }

  return TRUE;
}

static gboolean
process_crtc_gamma (MetaKmsImpl    *impl,
                    MetaKmsUpdate  *update,
                    gpointer        update_entry,
                    GError        **error)
{
  MetaKmsCrtcGamma *gamma = update_entry;
  MetaKmsCrtc *crtc = gamma->crtc;
  MetaKmsDevice *device = meta_kms_crtc_get_device (crtc);
  MetaKmsImplDevice *impl_device = meta_kms_device_get_impl_device (device);
  int fd;
  int ret;

  fd = meta_kms_impl_device_get_fd (impl_device);
  ret = drmModeCrtcSetGamma (fd, meta_kms_crtc_get_id (crtc),
                             gamma->size,
                             gamma->red,
                             gamma->green,
                             gamma->blue);
  if (ret != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   "drmModeCrtcSetGamma on CRTC %u failed: %s",
                   meta_kms_crtc_get_id (crtc),
                   g_strerror (-ret));
      return FALSE;
    }

  return TRUE;
}

static gboolean
is_timestamp_earlier_than (uint64_t ts1,
                           uint64_t ts2)
{
  if (ts1 == ts2)
    return FALSE;
  else
    return ts2 - ts1 < UINT64_MAX / 2;
}

typedef struct _RetryPageFlipData
{
  MetaKmsCrtc *crtc;
  uint32_t fb_id;
  MetaKmsPageFlipData *page_flip_data;
  float refresh_rate;
  uint64_t retry_time_us;
  MetaKmsCustomPageFlipFunc custom_page_flip_func;
  gpointer custom_page_flip_user_data;
} RetryPageFlipData;

static void
retry_page_flip_data_free (RetryPageFlipData *retry_page_flip_data)
{
  g_assert (!retry_page_flip_data->page_flip_data);
  g_free (retry_page_flip_data);
}

static CachedModeSet *
get_cached_mode_set (MetaKmsImplSimple *impl_simple,
                     MetaKmsCrtc       *crtc)
{
  return g_hash_table_lookup (impl_simple->cached_mode_sets, crtc);
}

static float
get_cached_crtc_refresh_rate (MetaKmsImplSimple *impl_simple,
                              MetaKmsCrtc       *crtc)
{
  CachedModeSet *cached_mode_set;

  cached_mode_set = g_hash_table_lookup (impl_simple->cached_mode_sets,
                                         crtc);
  g_assert (cached_mode_set);

  return meta_calculate_drm_mode_refresh_rate (cached_mode_set->drm_mode);
}

static gboolean
retry_page_flips (gpointer user_data)
{
  MetaKmsImplSimple *impl_simple = META_KMS_IMPL_SIMPLE (user_data);
  uint64_t now_us;
  GList *l;

  meta_assert_in_kms_impl (meta_kms_impl_get_kms (META_KMS_IMPL (impl_simple)));

  now_us = g_source_get_time (impl_simple->retry_page_flips_source);

  l = impl_simple->pending_page_flip_retries;
  while (l)
    {
      RetryPageFlipData *retry_page_flip_data = l->data;
      MetaKmsCrtc *crtc = retry_page_flip_data->crtc;
      MetaKmsDevice *device = meta_kms_crtc_get_device (crtc);
      MetaKmsImplDevice *impl_device = meta_kms_device_get_impl_device (device);
      GList *l_next = l->next;
      int fd;
      int ret;
      MetaKmsPageFlipData *page_flip_data;
      MetaKmsCustomPageFlipFunc custom_page_flip_func;

      if (is_timestamp_earlier_than (now_us,
                                     retry_page_flip_data->retry_time_us))
        {
          l = l_next;
          continue;
        }

      custom_page_flip_func = retry_page_flip_data->custom_page_flip_func;
      if (custom_page_flip_func)
        {
          ret = custom_page_flip_func (retry_page_flip_data->custom_page_flip_user_data,
                                       retry_page_flip_data->page_flip_data);
        }
      else
        {
          fd = meta_kms_impl_device_get_fd (impl_device);
          ret = drmModePageFlip (fd,
                                 meta_kms_crtc_get_id (crtc),
                                 retry_page_flip_data->fb_id,
                                 DRM_MODE_PAGE_FLIP_EVENT,
                                 retry_page_flip_data->page_flip_data);
        }

      if (ret == -EBUSY)
        {
          float refresh_rate;

          refresh_rate = get_cached_crtc_refresh_rate (impl_simple, crtc);
          retry_page_flip_data->retry_time_us +=
            (uint64_t) (G_USEC_PER_SEC / refresh_rate);
          l = l_next;
          continue;
        }

      impl_simple->pending_page_flip_retries =
        g_list_remove_link (impl_simple->pending_page_flip_retries, l);

      page_flip_data = g_steal_pointer (&retry_page_flip_data->page_flip_data);
      if (ret != 0)
        {
          g_autoptr (GError) error = NULL;

          g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (-ret),
                       "drmModePageFlip on CRTC %u failed: %s",
                       meta_kms_crtc_get_id (crtc),
                       g_strerror (-ret));
          if (!g_error_matches (error,
                                G_IO_ERROR,
                                G_IO_ERROR_PERMISSION_DENIED))
            g_critical ("Failed to page flip: %s", error->message);

          meta_kms_page_flip_data_discard_in_impl (page_flip_data, error);
        }

      retry_page_flip_data_free (retry_page_flip_data);

      l = l_next;
    }

  if (impl_simple->pending_page_flip_retries)
    {
      GList *l;
      uint64_t earliest_retry_time_us = 0;

      for (l = impl_simple->pending_page_flip_retries; l; l = l->next)
        {
          RetryPageFlipData *retry_page_flip_data = l->data;

          if (l == impl_simple->pending_page_flip_retries ||
              is_timestamp_earlier_than (retry_page_flip_data->retry_time_us,
                                         earliest_retry_time_us))
            earliest_retry_time_us = retry_page_flip_data->retry_time_us;
        }

      g_source_set_ready_time (impl_simple->retry_page_flips_source,
                               earliest_retry_time_us);
      return G_SOURCE_CONTINUE;
    }
  else
    {
      g_clear_pointer (&impl_simple->retry_page_flips_source,
                       g_source_unref);

      flush_postponed_page_flip_datas (impl_simple);

      return G_SOURCE_REMOVE;
    }
}

static void
schedule_retry_page_flip (MetaKmsImplSimple         *impl_simple,
                          MetaKmsCrtc               *crtc,
                          uint32_t                   fb_id,
                          float                      refresh_rate,
                          MetaKmsPageFlipData       *page_flip_data,
                          MetaKmsCustomPageFlipFunc  custom_page_flip_func,
                          gpointer                   custom_page_flip_user_data)
{
  RetryPageFlipData *retry_page_flip_data;
  uint64_t now_us;
  uint64_t retry_time_us;

  now_us = g_get_monotonic_time ();
  retry_time_us = now_us + (uint64_t) (G_USEC_PER_SEC / refresh_rate);

  retry_page_flip_data = g_new0 (RetryPageFlipData, 1);
  *retry_page_flip_data = (RetryPageFlipData) {
    .crtc = crtc,
    .fb_id = fb_id,
    .page_flip_data = meta_kms_page_flip_data_ref (page_flip_data),
    .refresh_rate = refresh_rate,
    .retry_time_us = retry_time_us,
    .custom_page_flip_func = custom_page_flip_func,
    .custom_page_flip_user_data = custom_page_flip_user_data,
  };

  if (!impl_simple->retry_page_flips_source)
    {
      MetaKms *kms = meta_kms_impl_get_kms (META_KMS_IMPL (impl_simple));
      GSource *source;

      source = meta_kms_add_source_in_impl (kms, retry_page_flips,
                                            impl_simple, NULL);
      g_source_set_ready_time (source, retry_time_us);

      impl_simple->retry_page_flips_source = source;
    }
  else
    {
      GList *l;

      for (l = impl_simple->pending_page_flip_retries; l; l = l->next)
        {
          RetryPageFlipData *pending_retry_page_flip_data = l->data;
          uint64_t pending_retry_time_us =
            pending_retry_page_flip_data->retry_time_us;

          if (is_timestamp_earlier_than (retry_time_us, pending_retry_time_us))
            {
              g_source_set_ready_time (impl_simple->retry_page_flips_source,
                                       retry_time_us);
              break;
            }
        }
    }

  impl_simple->pending_page_flip_retries =
    g_list_append (impl_simple->pending_page_flip_retries,
                   retry_page_flip_data);
}

static void
invoke_page_flip_datas (GList                        *page_flip_datas,
                        MetaPageFlipDataFeedbackFunc  func)
{
  g_list_foreach (page_flip_datas, (GFunc) func, NULL);
}

static void
clear_page_flip_datas (GList **page_flip_datas)
{
  g_list_free_full (*page_flip_datas,
                    (GDestroyNotify) meta_kms_page_flip_data_unref);
  *page_flip_datas = NULL;
}

static gboolean
mode_set_fallback_feedback_idle (gpointer user_data)
{
  MetaKmsImplSimple *impl_simple = user_data;

  g_clear_pointer (&impl_simple->mode_set_fallback_feedback_source,
                   g_source_unref);

  if (impl_simple->pending_page_flip_retries)
    {
      impl_simple->postponed_mode_set_fallback_datas =
        g_steal_pointer (&impl_simple->mode_set_fallback_page_flip_datas);
    }
  else
    {
      invoke_page_flip_datas (impl_simple->mode_set_fallback_page_flip_datas,
                              meta_kms_page_flip_data_mode_set_fallback_in_impl);
      clear_page_flip_datas (&impl_simple->mode_set_fallback_page_flip_datas);
    }

  return G_SOURCE_REMOVE;
}

static gboolean
mode_set_fallback (MetaKmsImplSimple       *impl_simple,
                   MetaKmsUpdate           *update,
                   MetaKmsPageFlip         *page_flip,
                   MetaKmsPlaneAssignment  *plane_assignment,
                   MetaKmsPageFlipData     *page_flip_data,
                   GError                 **error)
{
  MetaKms *kms = meta_kms_impl_get_kms (META_KMS_IMPL (impl_simple));
  MetaKmsCrtc *crtc = page_flip->crtc;
  MetaKmsDevice *device = meta_kms_crtc_get_device (crtc);
  MetaKmsImplDevice *impl_device = meta_kms_device_get_impl_device (device);
  CachedModeSet *cached_mode_set;
  g_autofree uint32_t *connectors = NULL;
  int n_connectors;
  uint32_t x, y;
  int fd;
  int ret;

  cached_mode_set = g_hash_table_lookup (impl_simple->cached_mode_sets,
                                         crtc);
  if (!cached_mode_set)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing mode set for page flip fallback");
      return FALSE;
    }

  fill_connector_ids_array (cached_mode_set->connectors,
                            &connectors,
                            &n_connectors);

  x = meta_fixed_16_to_int (plane_assignment->src_rect.x);
  y = meta_fixed_16_to_int (plane_assignment->src_rect.y);

  fd = meta_kms_impl_device_get_fd (impl_device);
  ret = drmModeSetCrtc (fd,
                        meta_kms_crtc_get_id (crtc),
                        plane_assignment->fb_id,
                        x, y,
                        connectors, n_connectors,
                        cached_mode_set->drm_mode);
  if (ret != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   "drmModeSetCrtc mode '%s' on CRTC %u failed: %s",
                   cached_mode_set->drm_mode->name,
                   meta_kms_crtc_get_id (crtc),
                   g_strerror (-ret));
      return FALSE;
    }

  if (!impl_simple->mode_set_fallback_feedback_source)
    {
      GSource *source;

      source = meta_kms_add_source_in_impl (kms,
                                            mode_set_fallback_feedback_idle,
                                            impl_simple,
                                            NULL);
      impl_simple->mode_set_fallback_feedback_source = source;
    }

  impl_simple->mode_set_fallback_page_flip_datas =
    g_list_prepend (impl_simple->mode_set_fallback_page_flip_datas,
                    meta_kms_page_flip_data_ref (page_flip_data));

  return TRUE;
}

static gboolean
process_page_flip (MetaKmsImpl    *impl,
                   MetaKmsUpdate  *update,
                   gpointer        update_entry,
                   GError        **error)
{
  MetaKmsPageFlip *page_flip = update_entry;
  MetaKmsImplSimple *impl_simple = META_KMS_IMPL_SIMPLE (impl);
  MetaKmsCrtc *crtc;
  MetaKmsDevice *device;
  MetaKmsImplDevice *impl_device;
  MetaKmsPlaneAssignment *plane_assignment;
  MetaKmsPageFlipData *page_flip_data;
  MetaKmsCustomPageFlipFunc custom_page_flip_func;
  int fd;
  int ret;

  crtc = page_flip->crtc;
  plane_assignment = meta_kms_update_get_primary_plane_assignment (update,
                                                                   crtc);

  page_flip_data = meta_kms_page_flip_data_new (impl,
                                                crtc,
                                                page_flip->feedback,
                                                page_flip->user_data);

  device = meta_kms_crtc_get_device (crtc);
  impl_device = meta_kms_device_get_impl_device (device);
  fd = meta_kms_impl_device_get_fd (impl_device);
  custom_page_flip_func = page_flip->custom_page_flip_func;
  if (custom_page_flip_func)
    {
      ret = custom_page_flip_func (page_flip->custom_page_flip_user_data,
                                   meta_kms_page_flip_data_ref (page_flip_data));
    }
  else
    {
      ret = atomic_page_flip (impl_simple, device, crtc,
                              plane_assignment ? plane_assignment->fb_id : 0,
                              page_flip_data);
      if (ret == -ENOTSUP)
        ret = drmModePageFlip (fd,
                               meta_kms_crtc_get_id (crtc),
                               plane_assignment->fb_id,
                               DRM_MODE_PAGE_FLIP_EVENT,
                               meta_kms_page_flip_data_ref (page_flip_data));
    }

  if (ret != 0)
    meta_kms_page_flip_data_unref (page_flip_data);

  if (ret == -EBUSY)
    {
      CachedModeSet *cached_mode_set;

      cached_mode_set = get_cached_mode_set (impl_simple, crtc);
      if (cached_mode_set)
        {
          drmModeModeInfo *drm_mode;
          float refresh_rate;

          drm_mode = cached_mode_set->drm_mode;
          refresh_rate = meta_calculate_drm_mode_refresh_rate (drm_mode);
          schedule_retry_page_flip (impl_simple,
                                    crtc,
                                    plane_assignment ?
                                        plane_assignment->fb_id : 0,
                                    refresh_rate,
                                    page_flip_data,
                                    custom_page_flip_func,
                                    page_flip->custom_page_flip_user_data);
        }
      else
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Page flip of %u failed, and no mode set available",
                       meta_kms_crtc_get_id (crtc));
          meta_kms_page_flip_data_unref (page_flip_data);
          return FALSE;
        }
    }
  else if (ret == -EINVAL)
    {
      if (!mode_set_fallback (impl_simple,
                              update,
                              page_flip,
                              plane_assignment,
                              page_flip_data,
                              error))
        {
          meta_kms_page_flip_data_unref (page_flip_data);
          return FALSE;
        }
    }
  else if (ret != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   "drmModePageFlip on CRTC %u failed: %s",
                   meta_kms_crtc_get_id (crtc),
                   g_strerror (-ret));
      meta_kms_page_flip_data_unref (page_flip_data);
      return FALSE;
    }

  meta_kms_page_flip_data_unref (page_flip_data);
  return TRUE;
}

static void
discard_page_flip (MetaKmsImpl     *impl,
                   MetaKmsUpdate   *update,
                   MetaKmsPageFlip *page_flip)
{
  MetaKmsCrtc *crtc;
  MetaKmsPageFlipData *page_flip_data;

  crtc = page_flip->crtc;
  page_flip_data = meta_kms_page_flip_data_new (impl,
                                                crtc,
                                                page_flip->feedback,
                                                page_flip->user_data);
  meta_kms_page_flip_data_discard_in_impl (page_flip_data, NULL);
  meta_kms_page_flip_data_unref (page_flip_data);
}

static gboolean
process_entries (MetaKmsImpl     *impl,
                 MetaKmsUpdate   *update,
                 GList           *entries,
                 gboolean      (* func) (MetaKmsImpl    *impl,
                                         MetaKmsUpdate  *update,
                                         gpointer        entry_data,
                                         GError        **error),
                 GError         **error)
{
  GList *l;

  for (l = entries; l; l = l->next)
    {
      if (!func (impl, update, l->data, error))
        return FALSE;
    }

  return TRUE;
}

/*
 * Atomic cursor updates.
 *
 * NVIDIA turns a commit that changes only the cursor image into a commit of
 * every plane of the CRTC (a workaround in nvidia-drm-modeset.c), so each
 * new cursor image costs a full flip.  Sent next to the page flips, as the
 * legacy cursor ioctls are, every image change takes a frame from the
 * monitor.
 *
 * On devices with slow cursor updates, keep the wanted cursor state per
 * CRTC instead, and send it as part of the next page flip, which then
 * becomes an atomic commit of the primary and cursor planes.  When the
 * monitor is not flipping (an idle desktop with a moving pointer), send it
 * in a non-blocking atomic commit of its own.  Page flips without a cursor
 * change stay legacy.
 *
 * Unexpected errors turn this off for the device: the legacy cursor ioctls
 * are used again, and the cursor renderer falls back to a software cursor.
 * MUFFIN_DEBUG_ATOMIC_CURSOR=0 disables it.
 */
typedef struct _AtomicDevice AtomicDevice;

typedef struct _AtomicCrtc
{
  AtomicDevice *adev;
  MetaKmsCrtc *crtc;
  uint32_t crtc_id;
  gboolean usable;

  uint32_t primary_plane_id;
  uint32_t prop_primary_fb_id;

  uint32_t plane_id;
  uint32_t prop_fb_id;
  uint32_t prop_crtc_id;
  uint32_t prop_src_x;
  uint32_t prop_src_y;
  uint32_t prop_src_w;
  uint32_t prop_src_h;
  uint32_t prop_crtc_x;
  uint32_t prop_crtc_y;
  uint32_t prop_crtc_w;
  uint32_t prop_crtc_h;

  /* the wanted state */
  gboolean dirty;
  gboolean visible;
  uint32_t fb_id;
  int width;
  int height;
  int x;
  int y;

  uint32_t committed_fb_id;
  int64_t last_flip_us;
  guint source_id;
} AtomicCrtc;

struct _AtomicDevice
{
  MetaKmsDevice *device;
  int fd;
  gboolean disabled;
  GHashTable *crtcs;            /* MetaKmsCrtc -> AtomicCrtc */
};

static void
atomic_crtc_free (gpointer data)
{
  AtomicCrtc *ac = data;

  g_clear_handle_id (&ac->source_id, g_source_remove);
  if (ac->fb_id && ac->fb_id != ac->committed_fb_id)
    drmModeRmFB (ac->adev->fd, ac->fb_id);
  g_free (ac);
}

static void
atomic_device_free (gpointer data)
{
  AtomicDevice *adev = data;

  if (adev->crtcs)
    g_hash_table_destroy (adev->crtcs);
  g_free (adev);
}

static void
atomic_device_disable (AtomicDevice *adev,
                       const char   *what,
                       int           ret)
{
  if (adev->disabled)
    return;

  g_warning ("Atomic cursor updates disabled: %s failed: %s",
             what, g_strerror (-ret));
  adev->disabled = TRUE;
  /* The cursor renderer then falls back to a software cursor */
  meta_kms_device_set_has_atomic_cursor_in_impl (adev->device, FALSE);
}

static AtomicDevice *
get_atomic_device (MetaKmsImplSimple *impl_simple,
                   MetaKmsDevice     *device,
                   int                fd)
{
  AtomicDevice *adev;

  adev = g_hash_table_lookup (impl_simple->atomic_devices, device);
  if (adev)
    return adev;

  adev = g_new0 (AtomicDevice, 1);
  adev->device = device;
  adev->fd = fd;
  adev->disabled =
    g_strcmp0 (g_getenv ("MUFFIN_DEBUG_ATOMIC_CURSOR"), "0") == 0 ||
    !meta_kms_device_has_slow_cursor_updates (device) ||
    drmSetClientCap (fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0;
  if (!adev->disabled)
    {
      adev->crtcs = g_hash_table_new_full (NULL, NULL, NULL, atomic_crtc_free);
      g_message ("Using atomic cursor updates on %s",
                 meta_kms_device_get_path (device));
      meta_kms_device_set_has_atomic_cursor_in_impl (device, TRUE);
    }
  g_hash_table_insert (impl_simple->atomic_devices, device, adev);

  return adev;
}

static uint32_t
find_property (int         fd,
               uint32_t    object_id,
               uint32_t    object_type,
               const char *name)
{
  drmModeObjectProperties *props;
  uint32_t prop_id = 0;
  uint32_t i;

  props = drmModeObjectGetProperties (fd, object_id, object_type);
  if (!props)
    return 0;

  for (i = 0; i < props->count_props && !prop_id; i++)
    {
      drmModePropertyRes *prop = drmModeGetProperty (fd, props->props[i]);

      if (!prop)
        continue;
      if (strcmp (prop->name, name) == 0)
        prop_id = prop->prop_id;
      drmModeFreeProperty (prop);
    }
  drmModeFreeObjectProperties (props);

  return prop_id;
}

static AtomicCrtc *
get_atomic_crtc (AtomicDevice  *adev,
                 MetaKmsDevice *device,
                 MetaKmsCrtc   *crtc)
{
  AtomicCrtc *ac;
  MetaKmsPlane *cursor_plane;
  MetaKmsPlane *primary_plane;
  int fd = adev->fd;

  ac = g_hash_table_lookup (adev->crtcs, crtc);
  if (ac)
    return ac->usable ? ac : NULL;

  ac = g_new0 (AtomicCrtc, 1);
  ac->adev = adev;
  ac->crtc = crtc;
  ac->crtc_id = meta_kms_crtc_get_id (crtc);
  g_hash_table_insert (adev->crtcs, crtc, ac);

  cursor_plane = meta_kms_device_get_cursor_plane_for (device, crtc);
  primary_plane = meta_kms_device_get_primary_plane_for (device, crtc);
  if (!cursor_plane || meta_kms_plane_is_fake (cursor_plane) ||
      !primary_plane || meta_kms_plane_is_fake (primary_plane))
    return NULL;

  ac->plane_id = meta_kms_plane_get_id (cursor_plane);
  ac->primary_plane_id = meta_kms_plane_get_id (primary_plane);

#define PLANE_PROP(field, id, name) \
  ((ac->field = find_property (fd, id, DRM_MODE_OBJECT_PLANE, name)) != 0)

  ac->usable =
    PLANE_PROP (prop_primary_fb_id, ac->primary_plane_id, "FB_ID") &&
    PLANE_PROP (prop_fb_id, ac->plane_id, "FB_ID") &&
    PLANE_PROP (prop_crtc_id, ac->plane_id, "CRTC_ID") &&
    PLANE_PROP (prop_src_x, ac->plane_id, "SRC_X") &&
    PLANE_PROP (prop_src_y, ac->plane_id, "SRC_Y") &&
    PLANE_PROP (prop_src_w, ac->plane_id, "SRC_W") &&
    PLANE_PROP (prop_src_h, ac->plane_id, "SRC_H") &&
    PLANE_PROP (prop_crtc_x, ac->plane_id, "CRTC_X") &&
    PLANE_PROP (prop_crtc_y, ac->plane_id, "CRTC_Y") &&
    PLANE_PROP (prop_crtc_w, ac->plane_id, "CRTC_W") &&
    PLANE_PROP (prop_crtc_h, ac->plane_id, "CRTC_H");

#undef PLANE_PROP

  return ac->usable ? ac : NULL;
}

/* Take the cursor state of 'plane_assignment' as the wanted one */
static gboolean
atomic_crtc_set (AtomicCrtc             *ac,
                 MetaKmsPlaneAssignment *plane_assignment)
{
  int fd = ac->adev->fd;

  if (!(plane_assignment->flags & META_KMS_ASSIGN_PLANE_FLAG_FB_UNCHANGED))
    {
      uint32_t new_fb_id = 0;

      if (plane_assignment->fb_id)
        {
          int width = meta_fixed_16_to_int (plane_assignment->dst_rect.width);
          int height = meta_fixed_16_to_int (plane_assignment->dst_rect.height);
          uint32_t handles[4] = { plane_assignment->fb_id };
          uint32_t pitches[4] = { plane_assignment->cursor_buffer.stride };
          uint32_t offsets[4] = { 0 };
          uint32_t format = plane_assignment->cursor_buffer.format;
          int ret;

          if (!format)
            format = DRM_FORMAT_ARGB8888;
          if (!pitches[0])
            pitches[0] = width * 4;

          ret = drmModeAddFB2 (fd, width, height, format, handles, pitches,
                               offsets, &new_fb_id, 0);
          if (ret != 0)
            {
              atomic_device_disable (ac->adev, "drmModeAddFB2", ret);
              return FALSE;
            }
          ac->width = width;
          ac->height = height;
        }

      /* A wanted image that was never committed is not shown anywhere */
      if (ac->fb_id && ac->fb_id != ac->committed_fb_id)
        drmModeRmFB (fd, ac->fb_id);
      ac->fb_id = new_fb_id;
      ac->visible = new_fb_id != 0;
    }

  ac->x = meta_fixed_16_to_int (plane_assignment->dst_rect.x);
  ac->y = meta_fixed_16_to_int (plane_assignment->dst_rect.y);
  ac->dirty = TRUE;

  return TRUE;
}

static gboolean
add_atomic_cursor_properties (AtomicCrtc       *ac,
                              drmModeAtomicReq *req)
{
  uint32_t plane = ac->plane_id;
  gboolean ok = TRUE;

#define ADD(prop, value) \
  ok = ok && drmModeAtomicAddProperty (req, plane, ac->prop, value) >= 0

  if (ac->visible)
    {
      ADD (prop_fb_id, ac->fb_id);
      ADD (prop_crtc_id, ac->crtc_id);
      ADD (prop_src_x, 0);
      ADD (prop_src_y, 0);
      ADD (prop_src_w, (uint64_t) ac->width << 16);
      ADD (prop_src_h, (uint64_t) ac->height << 16);
      ADD (prop_crtc_x, (uint64_t) (int64_t) ac->x);
      ADD (prop_crtc_y, (uint64_t) (int64_t) ac->y);
      ADD (prop_crtc_w, ac->width);
      ADD (prop_crtc_h, ac->height);
    }
  else
    {
      ADD (prop_fb_id, 0);
      ADD (prop_crtc_id, 0);
    }

#undef ADD

  return ok;
}

/* The wanted state is now the committed one */
static void
atomic_crtc_committed (AtomicCrtc *ac)
{
  uint32_t fb_id = ac->visible ? ac->fb_id : 0;

  /* The old framebuffer stays alive until the commit is done with it */
  if (ac->committed_fb_id && ac->committed_fb_id != fb_id)
    drmModeRmFB (ac->adev->fd, ac->committed_fb_id);
  ac->committed_fb_id = fb_id;
  ac->dirty = FALSE;
}

static int64_t
atomic_crtc_frame_us (MetaKmsImplSimple *impl_simple,
                      AtomicCrtc        *ac)
{
  CachedModeSet *cached_mode_set = get_cached_mode_set (impl_simple, ac->crtc);
  float refresh_rate = 0;

  if (cached_mode_set)
    refresh_rate = meta_calculate_drm_mode_refresh_rate (cached_mode_set->drm_mode);

  return refresh_rate > 1 ? (int64_t) (G_USEC_PER_SEC / refresh_rate) : 16667;
}

static void schedule_atomic_cursor_commit (MetaKmsImplSimple *impl_simple,
                                           AtomicCrtc        *ac);

typedef struct
{
  MetaKmsImplSimple *impl_simple;
  AtomicCrtc *ac;
} AtomicCommitData;

static gboolean
atomic_cursor_commit_timeout (gpointer user_data)
{
  AtomicCommitData *data = user_data;
  AtomicCrtc *ac = data->ac;
  drmModeAtomicReq *req;
  int ret;

  ac->source_id = 0;
  if (!ac->dirty || ac->adev->disabled)
    return G_SOURCE_REMOVE;

  req = drmModeAtomicAlloc ();
  if (!add_atomic_cursor_properties (ac, req))
    {
      drmModeAtomicFree (req);
      atomic_device_disable (ac->adev, "drmModeAtomicAddProperty", -ENOMEM);
      return G_SOURCE_REMOVE;
    }
  ret = drmModeAtomicCommit (ac->adev->fd, req, DRM_MODE_ATOMIC_NONBLOCK,
                             NULL);
  drmModeAtomicFree (req);

  if (ret == 0)
    {
      atomic_crtc_committed (ac);
    }
  else if (ret == -EBUSY)
    {
      schedule_atomic_cursor_commit (data->impl_simple, ac);
    }
  else if (ret == -EINVAL)
    {
      /* For example a CRTC that is turned off: drop this state, the next
       * cursor update or mode set brings a new one */
      g_warning_once ("Atomic cursor commit on CRTC %u refused: %s",
                      ac->crtc_id, g_strerror (-ret));
      ac->dirty = FALSE;
    }
  else if (ret != -EACCES && ret != -EPERM)
    {
      atomic_device_disable (ac->adev, "cursor drmModeAtomicCommit", ret);
    }

  return G_SOURCE_REMOVE;
}

/*
 * Send the wanted cursor state on its own, unless a page flip takes it
 * first: right away when the CRTC is idle, after two frames when it is
 * flipping (or busy), giving the flips a chance to carry it.
 */
static void
schedule_atomic_cursor_commit (MetaKmsImplSimple *impl_simple,
                               AtomicCrtc        *ac)
{
  int64_t frame_us = atomic_crtc_frame_us (impl_simple, ac);
  int64_t now_us = g_get_monotonic_time ();
  guint delay_ms = 0;
  AtomicCommitData *data;

  if (ac->source_id)
    return;

  if (ac->last_flip_us && now_us - ac->last_flip_us < 3 * frame_us)
    delay_ms = (guint) (2 * frame_us / 1000);

  data = g_new0 (AtomicCommitData, 1);
  data->impl_simple = impl_simple;
  data->ac = ac;
  ac->source_id = g_timeout_add_full (G_PRIORITY_DEFAULT, delay_ms,
                                      atomic_cursor_commit_timeout,
                                      data, g_free);
}

/* The CRTC of 'device' with cursor state waiting to be sent, or NULL */
static AtomicCrtc *
get_dirty_atomic_crtc (MetaKmsImplSimple *impl_simple,
                       MetaKmsDevice     *device,
                       MetaKmsCrtc       *crtc)
{
  AtomicDevice *adev = g_hash_table_lookup (impl_simple->atomic_devices,
                                            device);
  AtomicCrtc *ac;

  if (!adev || adev->disabled)
    return NULL;
  ac = g_hash_table_lookup (adev->crtcs, crtc);
  return ac && ac->usable && ac->dirty ? ac : NULL;
}

/*
 * Page flip 'crtc' to 'fb_id' together with its waiting cursor state, as
 * drmModePageFlip() would.  Returns -ENOTSUP when there is nothing to send
 * atomically, and turns atomic cursor updates off (also returning -ENOTSUP)
 * when the commit is refused, so the caller can do a legacy page flip.
 */
static int
atomic_page_flip (MetaKmsImplSimple   *impl_simple,
                  MetaKmsDevice       *device,
                  MetaKmsCrtc         *crtc,
                  uint32_t             fb_id,
                  MetaKmsPageFlipData *page_flip_data)
{
  AtomicCrtc *ac = get_dirty_atomic_crtc (impl_simple, device, crtc);
  drmModeAtomicReq *req;
  int ret;

  if (!ac || !fb_id)
    return -ENOTSUP;

  req = drmModeAtomicAlloc ();
  if (drmModeAtomicAddProperty (req, ac->primary_plane_id,
                                ac->prop_primary_fb_id, fb_id) < 0 ||
      !add_atomic_cursor_properties (ac, req))
    {
      drmModeAtomicFree (req);
      return -ENOTSUP;
    }

  ret = drmModeAtomicCommit (ac->adev->fd, req,
                             DRM_MODE_ATOMIC_NONBLOCK |
                             DRM_MODE_PAGE_FLIP_EVENT,
                             meta_kms_page_flip_data_ref (page_flip_data));
  drmModeAtomicFree (req);

  if (ret == 0)
    {
      atomic_crtc_committed (ac);
      return 0;
    }

  /* Like drmModePageFlip(): on failure the caller drops the reference that
   * went with the commit; on -ENOTSUP it does a legacy flip instead */
  if (ret == -EBUSY || ret == -EACCES || ret == -EPERM)
    return ret;

  meta_kms_page_flip_data_unref (page_flip_data);
  atomic_device_disable (ac->adev, "page flip drmModeAtomicCommit", ret);
  return -ENOTSUP;
}

static void
atomic_crtc_flipped (MetaKmsImplSimple *impl_simple,
                     MetaKmsCrtc       *crtc)
{
  MetaKmsDevice *device = meta_kms_crtc_get_device (crtc);
  AtomicDevice *adev = g_hash_table_lookup (impl_simple->atomic_devices,
                                            device);
  AtomicCrtc *ac;

  if (!adev || adev->disabled)
    return;
  ac = g_hash_table_lookup (adev->crtcs, crtc);
  if (!ac)
    return;

  ac->last_flip_us = g_get_monotonic_time ();
  if (ac->dirty)
    schedule_atomic_cursor_commit (impl_simple, ac);
}

static gboolean
process_cursor_plane_assignment (MetaKmsImpl             *impl,
                                 MetaKmsUpdate           *update,
                                 MetaKmsPlaneAssignment  *plane_assignment,
                                 GError                 **error)
{
  MetaKmsPlane *plane;
  MetaKmsDevice *device;
  MetaKmsImplDevice *impl_device;
  int fd;

  plane = plane_assignment->plane;
  device = meta_kms_plane_get_device (plane);
  impl_device = meta_kms_device_get_impl_device (device);
  fd = meta_kms_impl_device_get_fd (impl_device);

  {
    AtomicDevice *adev = get_atomic_device (META_KMS_IMPL_SIMPLE (impl),
                                            device, fd);
    AtomicCrtc *ac = NULL;

    if (!adev->disabled)
      ac = get_atomic_crtc (adev, device, plane_assignment->crtc);
    if (ac && atomic_crtc_set (ac, plane_assignment))
      {
        schedule_atomic_cursor_commit (META_KMS_IMPL_SIMPLE (impl), ac);
        return TRUE;
      }
  }


  if (!(plane_assignment->flags & META_KMS_ASSIGN_PLANE_FLAG_FB_UNCHANGED))
    {
      int width, height;
      int ret = -1;

      width = meta_fixed_16_to_int (plane_assignment->dst_rect.width);
      height = meta_fixed_16_to_int (plane_assignment->dst_rect.height);

      if (plane_assignment->cursor_hotspot.is_valid)
        {
          ret = drmModeSetCursor2 (fd, meta_kms_crtc_get_id (plane_assignment->crtc),
                                   plane_assignment->fb_id,
                                   width, height,
                                   plane_assignment->cursor_hotspot.x,
                                   plane_assignment->cursor_hotspot.y);
        }

      if (ret != 0)
        {
          ret = drmModeSetCursor (fd, meta_kms_crtc_get_id (plane_assignment->crtc),
                                  plane_assignment->fb_id,
                                  width, height);
        }

      if (ret != 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                       "drmModeSetCursor failed: %s", g_strerror (-ret));
          return FALSE;
        }
    }

  drmModeMoveCursor (fd,
                     meta_kms_crtc_get_id (plane_assignment->crtc),
                     meta_fixed_16_to_int (plane_assignment->dst_rect.x),
                     meta_fixed_16_to_int (plane_assignment->dst_rect.y));

  return TRUE;
}

static gboolean
process_plane_assignment (MetaKmsImpl             *impl,
                          MetaKmsUpdate           *update,
                          MetaKmsPlaneAssignment  *plane_assignment,
                          MetaKmsPlaneFeedback   **plane_feedback)
{
  MetaKmsPlane *plane;
  MetaKmsPlaneType plane_type;
  GError *error = NULL;

  plane = plane_assignment->plane;
  plane_type = meta_kms_plane_get_plane_type (plane);
  switch (plane_type)
    {
    case META_KMS_PLANE_TYPE_PRIMARY:
      /* Handled as part of the mode-set and page flip. */
      return TRUE;
    case META_KMS_PLANE_TYPE_CURSOR:
      if (!process_cursor_plane_assignment (impl, update,
                                            plane_assignment,
                                            &error))
        {
          *plane_feedback =
            meta_kms_plane_feedback_new_take_error (plane,
                                                    plane_assignment->crtc,
                                                    g_steal_pointer (&error));
          return FALSE;
        }
      else
        {
          return TRUE;
        }
    case META_KMS_PLANE_TYPE_OVERLAY:
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Overlay planes cannot be assigned");
      *plane_feedback =
        meta_kms_plane_feedback_new_take_error (plane,
                                                plane_assignment->crtc,
                                                g_steal_pointer (&error));
      return TRUE;
    }

  g_assert_not_reached ();
}

static GList *
process_plane_assignments (MetaKmsImpl   *impl,
                           MetaKmsUpdate *update)
{
  GList *failed_planes = NULL;
  GList *l;

  for (l = meta_kms_update_get_plane_assignments (update); l; l = l->next)
    {
      MetaKmsPlaneAssignment *plane_assignment = l->data;
      MetaKmsPlaneFeedback *plane_feedback;

      if (!process_plane_assignment (impl, update, plane_assignment,
                                     &plane_feedback))
        failed_planes = g_list_prepend (failed_planes, plane_feedback);
    }

  return failed_planes;
}

static GList *
generate_all_failed_feedbacks (MetaKmsUpdate *update)
{
  GList *failed_planes = NULL;
  GList *l;

  for (l = meta_kms_update_get_plane_assignments (update); l; l = l->next)
    {
      MetaKmsPlaneAssignment *plane_assignment = l->data;
      MetaKmsPlane *plane;
      MetaKmsPlaneType plane_type;
      MetaKmsPlaneFeedback *plane_feedback;

      plane = plane_assignment->plane;
      plane_type = meta_kms_plane_get_plane_type (plane);
      switch (plane_type)
        {
        case META_KMS_PLANE_TYPE_PRIMARY:
          continue;
        case META_KMS_PLANE_TYPE_CURSOR:
        case META_KMS_PLANE_TYPE_OVERLAY:
          break;
        }

      plane_feedback =
        meta_kms_plane_feedback_new_take_error (plane_assignment->plane,
                                                plane_assignment->crtc,
                                                g_error_new (G_IO_ERROR,
                                                             G_IO_ERROR_FAILED,
                                                             "Discarded"));
      failed_planes = g_list_prepend (failed_planes, plane_feedback);
    }

  return failed_planes;
}

static MetaKmsFeedback *
meta_kms_impl_simple_process_update (MetaKmsImpl   *impl,
                                     MetaKmsUpdate *update)
{
  GError *error = NULL;
  GList *failed_planes;
  GList *l;

  meta_assert_in_kms_impl (meta_kms_impl_get_kms (impl));

  if (!process_entries (impl,
                        update,
                        meta_kms_update_get_connector_properties (update),
                        process_connector_property,
                        &error))
    goto err_planes_not_assigned;

  if (!process_entries (impl,
                        update,
                        meta_kms_update_get_mode_sets (update),
                        process_mode_set,
                        &error))
    goto err_planes_not_assigned;

  if (!process_entries (impl,
                        update,
                        meta_kms_update_get_crtc_gammas (update),
                        process_crtc_gamma,
                        &error))
    goto err_planes_not_assigned;

  failed_planes = process_plane_assignments (impl, update);
  if (failed_planes)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to assign one or more planes");
      goto err_planes_assigned;
    }

  if (!process_entries (impl,
                        update,
                        meta_kms_update_get_page_flips (update),
                        process_page_flip,
                        &error))
    goto err_planes_assigned;

  return meta_kms_feedback_new_passed ();

err_planes_not_assigned:
  failed_planes = generate_all_failed_feedbacks (update);

err_planes_assigned:
  for (l = meta_kms_update_get_page_flips (update); l; l = l->next)
    {
      MetaKmsPageFlip *page_flip = l->data;

      discard_page_flip (impl, update, page_flip);
    }

  return meta_kms_feedback_new_failed (failed_planes, error);
}

static void
flush_postponed_page_flip_datas (MetaKmsImplSimple *impl_simple)
{
  invoke_page_flip_datas (impl_simple->postponed_page_flip_datas,
                          meta_kms_page_flip_data_flipped_in_impl);
  clear_page_flip_datas (&impl_simple->postponed_page_flip_datas);

  invoke_page_flip_datas (impl_simple->postponed_mode_set_fallback_datas,
                          meta_kms_page_flip_data_mode_set_fallback_in_impl);
  clear_page_flip_datas (&impl_simple->postponed_mode_set_fallback_datas);
}

static void
meta_kms_impl_simple_handle_page_flip_callback (MetaKmsImpl         *impl,
                                                MetaKmsPageFlipData *page_flip_data)
{
  MetaKmsImplSimple *impl_simple = META_KMS_IMPL_SIMPLE (impl);

  atomic_crtc_flipped (impl_simple,
                       meta_kms_page_flip_data_get_crtc (page_flip_data));

  if (impl_simple->pending_page_flip_retries)
    {
      impl_simple->postponed_page_flip_datas =
        g_list_append (impl_simple->postponed_page_flip_datas,
                       meta_kms_page_flip_data_ref (page_flip_data));
    }
  else
    {
      meta_kms_page_flip_data_flipped_in_impl (page_flip_data);
    }

  meta_kms_page_flip_data_unref (page_flip_data);
}

static void
meta_kms_impl_simple_discard_pending_page_flips (MetaKmsImpl *impl)
{
  MetaKmsImplSimple *impl_simple = META_KMS_IMPL_SIMPLE (impl);
  GList *l;

  if (!impl_simple->pending_page_flip_retries)
    return;

  for (l = impl_simple->pending_page_flip_retries; l; l = l->next)
    {
      RetryPageFlipData *retry_page_flip_data = l->data;
      MetaKmsPageFlipData *page_flip_data;

      page_flip_data = g_steal_pointer (&retry_page_flip_data->page_flip_data);
      meta_kms_page_flip_data_discard_in_impl (page_flip_data, NULL);
      retry_page_flip_data_free (retry_page_flip_data);
    }
  g_clear_pointer (&impl_simple->pending_page_flip_retries, g_list_free);

  g_clear_pointer (&impl_simple->retry_page_flips_source,
                   g_source_destroy);
}

static void
meta_kms_impl_simple_dispatch_idle (MetaKmsImpl *impl)
{
  MetaKmsImplSimple *impl_simple = META_KMS_IMPL_SIMPLE (impl);

  if (impl_simple->mode_set_fallback_feedback_source)
    mode_set_fallback_feedback_idle (impl_simple);
}

static void
meta_kms_impl_simple_notify_device_created (MetaKmsImpl   *impl,
                                            MetaKmsDevice *device)
{
  MetaKmsImplDevice *impl_device = meta_kms_device_get_impl_device (device);
  GList *l;

  /* Decide on atomic cursor updates now: the cursor renderer only uses the
   * hardware cursor of a device with slow cursor updates if they are atomic */
  get_atomic_device (META_KMS_IMPL_SIMPLE (impl), device,
                     meta_kms_impl_device_get_fd (impl_device));

  for (l = meta_kms_device_get_crtcs (device); l; l = l->next)
    {
      MetaKmsCrtc *crtc = l->data;
      MetaKmsPlane *plane;

      plane = meta_kms_device_get_cursor_plane_for (device, crtc);
      if (plane)
        continue;

      meta_kms_device_add_fake_plane_in_impl (device,
                                              META_KMS_PLANE_TYPE_CURSOR,
                                              crtc);
    }
}

static void
meta_kms_impl_simple_finalize (GObject *object)
{
  MetaKmsImplSimple *impl_simple = META_KMS_IMPL_SIMPLE (object);

  g_list_free_full (impl_simple->pending_page_flip_retries,
                    (GDestroyNotify) retry_page_flip_data_free);
  g_list_free_full (impl_simple->postponed_page_flip_datas,
                    (GDestroyNotify) meta_kms_page_flip_data_unref);
  g_list_free_full (impl_simple->postponed_mode_set_fallback_datas,
                    (GDestroyNotify) meta_kms_page_flip_data_unref);
  g_clear_pointer (&impl_simple->mode_set_fallback_feedback_source,
                   g_source_destroy);
  g_hash_table_destroy (impl_simple->cached_mode_sets);
  g_hash_table_destroy (impl_simple->atomic_devices);

  G_OBJECT_CLASS (meta_kms_impl_simple_parent_class)->finalize (object);
}

static void
meta_kms_impl_simple_init (MetaKmsImplSimple *impl_simple)
{
  impl_simple->cached_mode_sets =
    g_hash_table_new_full (NULL,
                           NULL,
                           NULL,
                           (GDestroyNotify) cached_mode_set_free);
  impl_simple->atomic_devices =
    g_hash_table_new_full (NULL, NULL, NULL, atomic_device_free);
}

static void
meta_kms_impl_simple_class_init (MetaKmsImplSimpleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaKmsImplClass *impl_class = META_KMS_IMPL_CLASS (klass);

  object_class->finalize = meta_kms_impl_simple_finalize;

  impl_class->process_update = meta_kms_impl_simple_process_update;
  impl_class->handle_page_flip_callback = meta_kms_impl_simple_handle_page_flip_callback;
  impl_class->discard_pending_page_flips = meta_kms_impl_simple_discard_pending_page_flips;
  impl_class->dispatch_idle = meta_kms_impl_simple_dispatch_idle;
  impl_class->notify_device_created = meta_kms_impl_simple_notify_device_created;
}
