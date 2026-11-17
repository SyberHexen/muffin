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
 * wp_linux_drm_syncobj_v1: explicit synchronization of client buffers.
 *
 * A client gives each committed dma-buf an acquire point (the GPU has
 * finished drawing it once the point signals) and a release point (to
 * signal once the compositor is done with it) on DRM timeline syncobjs.
 *
 * Muffin reads client buffers with implicit synchronization: the GPU and KMS
 * wait for the fences in a dma-buf's reservation object. So the acquire
 * point's fence is put into the buffer's reservation object as a write fence
 * (DMA_BUF_IOCTL_IMPORT_SYNC_FILE) when the buffer is committed, and when
 * Muffin releases the buffer the fences of its reads are taken out of it
 * (DMA_BUF_IOCTL_EXPORT_SYNC_FILE) to signal the release point. Drivers
 * that don't track implicit fences for buffers (NVIDIA) then still get
 * waited for.
 */

#include "config.h"

#include "wayland/meta-wayland-linux-drm-syncobj.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

#include "backends/meta-backend-private.h"
#include "wayland/meta-wayland-buffer.h"
#include "wayland/meta-wayland-dma-buf.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-surface.h"

#ifdef HAVE_NATIVE_BACKEND
#include "backends/native/meta-backend-native.h"
#include "backends/native/meta-gpu-kms.h"
#include "backends/native/meta-renderer-native.h"
#endif

#include "linux-drm-syncobj-v1-server-protocol.h"

#define META_WP_LINUX_DRM_SYNCOBJ_V1_VERSION 1

/* How long a commit waits at most for its acquire point to get a fence (a
 * client may commit before submitting the work that signals it) */
#define ACQUIRE_AVAILABLE_TIMEOUT_NS (100 * 1000 * 1000)

typedef struct _MetaSyncobjTimeline
{
  grefcount ref_count;
  uint32_t handle;
} MetaSyncobjTimeline;

struct _MetaWaylandSyncPoint
{
  MetaSyncobjTimeline *timeline;
  uint64_t point;
};

struct _MetaWaylandSyncobjSurface
{
  struct wl_resource *resource;
  MetaWaylandSurface *surface;

  /* Pending (double-buffered) points */
  MetaWaylandSyncPoint *acquire;
  MetaWaylandSyncPoint *release;
};

/* The primary GPU's render node: syncobj handles are per DRM file */
static int drm_fd = -1;

static MetaSyncobjTimeline *
timeline_ref (MetaSyncobjTimeline *timeline)
{
  g_ref_count_inc (&timeline->ref_count);
  return timeline;
}

static void
timeline_unref (MetaSyncobjTimeline *timeline)
{
  if (g_ref_count_dec (&timeline->ref_count))
    {
      drmSyncobjDestroy (drm_fd, timeline->handle);
      g_free (timeline);
    }
}

static MetaWaylandSyncPoint *
sync_point_new (MetaSyncobjTimeline *timeline,
                uint32_t             point_hi,
                uint32_t             point_lo)
{
  MetaWaylandSyncPoint *sync_point = g_new0 (MetaWaylandSyncPoint, 1);

  sync_point->timeline = timeline_ref (timeline);
  sync_point->point = (uint64_t) point_hi << 32 | point_lo;

  return sync_point;
}

static void
sync_point_free (MetaWaylandSyncPoint *sync_point)
{
  if (!sync_point)
    return;

  timeline_unref (sync_point->timeline);
  g_free (sync_point);
}

static int64_t
monotonic_time_ns (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* A sync_file with the fence of a timeline point, waiting for the point to
 * get one if it hasn't yet; -1 if it doesn't in time */
static int
sync_point_export_sync_file (MetaWaylandSyncPoint *sync_point)
{
  uint32_t handle = sync_point->timeline->handle;
  uint64_t point = sync_point->point;
  uint32_t tmp = 0;
  int sync_fd = -1;

  if (drmSyncobjTimelineWait (drm_fd, &handle, &point, 1, 0,
                              DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                              NULL) != 0 &&
      drmSyncobjTimelineWait (drm_fd, &handle, &point, 1,
                              monotonic_time_ns () + ACQUIRE_AVAILABLE_TIMEOUT_NS,
                              DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                              NULL) != 0)
    return -1;

  if (drmSyncobjCreate (drm_fd, 0, &tmp) != 0)
    return -1;

  if (drmSyncobjTransfer (drm_fd, tmp, 0, handle, point, 0) != 0 ||
      drmSyncobjExportSyncFile (drm_fd, tmp, &sync_fd) != 0)
    sync_fd = -1;

  drmSyncobjDestroy (drm_fd, tmp);

  return sync_fd;
}

void
meta_wayland_sync_point_release (MetaWaylandSyncPoint *sync_point,
                                 MetaWaylandBuffer    *buffer)
{
  uint32_t handle = sync_point->timeline->handle;
  uint64_t point = sync_point->point;
  gboolean signaled = FALSE;
  int sync_fd = -1;

  if (buffer && buffer->type == META_WAYLAND_BUFFER_TYPE_DMA_BUF &&
      buffer->dma_buf.dma_buf)
    sync_fd = meta_wayland_dma_buf_export_sync_file (buffer->dma_buf.dma_buf);

  if (sync_fd >= 0)
    {
      uint32_t tmp = 0;

      if (drmSyncobjCreate (drm_fd, 0, &tmp) == 0)
        {
          signaled =
            drmSyncobjImportSyncFile (drm_fd, tmp, sync_fd) == 0 &&
            drmSyncobjTransfer (drm_fd, handle, point, tmp, 0, 0) == 0;
          drmSyncobjDestroy (drm_fd, tmp);
        }
      close (sync_fd);
    }

  /* No reads in flight (or no way to tell): released now */
  if (!signaled)
    drmSyncobjTimelineSignal (drm_fd, &handle, &point, 1);

  sync_point_free (sync_point);
}

static void
syncobj_surface_destructor (struct wl_resource *resource)
{
  MetaWaylandSyncobjSurface *syncobj_surface =
    wl_resource_get_user_data (resource);

  if (syncobj_surface->surface)
    syncobj_surface->surface->drm_syncobj = NULL;

  sync_point_free (syncobj_surface->acquire);
  sync_point_free (syncobj_surface->release);
  g_free (syncobj_surface);
}

static void
syncobj_surface_destroy (struct wl_client   *client,
                         struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
set_point (struct wl_resource    *resource,
           struct wl_resource    *timeline_resource,
           uint32_t               point_hi,
           uint32_t               point_lo,
           MetaWaylandSyncPoint **sync_point)
{
  MetaWaylandSyncobjSurface *syncobj_surface =
    wl_resource_get_user_data (resource);
  MetaSyncobjTimeline *timeline =
    wl_resource_get_user_data (timeline_resource);

  if (!syncobj_surface->surface)
    {
      wl_resource_post_error (resource,
                              WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
                              "The surface was destroyed");
      return;
    }

  sync_point_free (*sync_point);
  *sync_point = sync_point_new (timeline, point_hi, point_lo);
}

static void
syncobj_surface_set_acquire_point (struct wl_client   *client,
                                   struct wl_resource *resource,
                                   struct wl_resource *timeline_resource,
                                   uint32_t            point_hi,
                                   uint32_t            point_lo)
{
  MetaWaylandSyncobjSurface *syncobj_surface =
    wl_resource_get_user_data (resource);

  set_point (resource, timeline_resource, point_hi, point_lo,
             &syncobj_surface->acquire);
}

static void
syncobj_surface_set_release_point (struct wl_client   *client,
                                   struct wl_resource *resource,
                                   struct wl_resource *timeline_resource,
                                   uint32_t            point_hi,
                                   uint32_t            point_lo)
{
  MetaWaylandSyncobjSurface *syncobj_surface =
    wl_resource_get_user_data (resource);

  set_point (resource, timeline_resource, point_hi, point_lo,
             &syncobj_surface->release);
}

static const struct wp_linux_drm_syncobj_surface_v1_interface syncobj_surface_interface = {
  syncobj_surface_destroy,
  syncobj_surface_set_acquire_point,
  syncobj_surface_set_release_point,
};

static void
timeline_destructor (struct wl_resource *resource)
{
  timeline_unref (wl_resource_get_user_data (resource));
}

static void
timeline_destroy (struct wl_client   *client,
                  struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wp_linux_drm_syncobj_timeline_v1_interface timeline_interface = {
  timeline_destroy,
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
  MetaWaylandSyncobjSurface *syncobj_surface;
  struct wl_resource *syncobj_resource;

  if (surface->drm_syncobj)
    {
      wl_resource_post_error (resource,
                              WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
                              "The surface already has a syncobj surface");
      return;
    }

  syncobj_resource =
    wl_resource_create (client, &wp_linux_drm_syncobj_surface_v1_interface,
                        wl_resource_get_version (resource), id);
  if (!syncobj_resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  syncobj_surface = g_new0 (MetaWaylandSyncobjSurface, 1);
  syncobj_surface->resource = syncobj_resource;
  syncobj_surface->surface = surface;
  surface->drm_syncobj = syncobj_surface;

  wl_resource_set_implementation (syncobj_resource,
                                  &syncobj_surface_interface,
                                  syncobj_surface,
                                  syncobj_surface_destructor);
}

static void
manager_import_timeline (struct wl_client   *client,
                         struct wl_resource *resource,
                         uint32_t            id,
                         int32_t             fd)
{
  MetaSyncobjTimeline *timeline;
  struct wl_resource *timeline_resource;
  uint32_t handle = 0;

  if (drmSyncobjFDToHandle (drm_fd, fd, &handle) != 0)
    {
      close (fd);
      wl_resource_post_error (resource,
                              WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
                              "Not a DRM syncobj: %s", g_strerror (errno));
      return;
    }
  close (fd);

  timeline_resource =
    wl_resource_create (client, &wp_linux_drm_syncobj_timeline_v1_interface,
                        wl_resource_get_version (resource), id);
  if (!timeline_resource)
    {
      drmSyncobjDestroy (drm_fd, handle);
      wl_client_post_no_memory (client);
      return;
    }

  timeline = g_new0 (MetaSyncobjTimeline, 1);
  g_ref_count_init (&timeline->ref_count);
  timeline->handle = handle;

  wl_resource_set_implementation (timeline_resource,
                                  &timeline_interface,
                                  timeline,
                                  timeline_destructor);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface manager_interface = {
  manager_destroy,
  manager_get_surface,
  manager_import_timeline,
};

static void
manager_bind (struct wl_client *client,
              void             *data,
              uint32_t          version,
              uint32_t          id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
                                 &wp_linux_drm_syncobj_manager_v1_interface,
                                 version, id);
  wl_resource_set_implementation (resource, &manager_interface, NULL, NULL);
}

gboolean
meta_wayland_linux_drm_syncobj_commit (MetaWaylandSurface *surface,
                                       MetaWaylandBuffer  *buffer,
                                       gboolean            newly_attached)
{
  MetaWaylandSyncobjSurface *syncobj_surface = surface->drm_syncobj;
  MetaWaylandSyncPoint *acquire, *release;
  int sync_fd;

  if (!syncobj_surface)
    return TRUE;

  /* The points are for this commit only */
  acquire = g_steal_pointer (&syncobj_surface->acquire);
  release = g_steal_pointer (&syncobj_surface->release);

  if (!newly_attached || !buffer)
    {
      gboolean had_points = acquire || release;

      sync_point_free (acquire);
      sync_point_free (release);
      if (had_points)
        {
          wl_resource_post_error (syncobj_surface->resource,
                                  WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
                                  "Sync points were set without a buffer");
          return FALSE;
        }
      return TRUE;
    }

  if (!acquire || !release)
    {
      sync_point_free (acquire);
      sync_point_free (release);
      wl_resource_post_error (syncobj_surface->resource,
                              !acquire ?
                              WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT :
                              WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT,
                              "A buffer was attached without %s point",
                              !acquire ? "an acquire" : "a release");
      return FALSE;
    }

  if (buffer->type != META_WAYLAND_BUFFER_TYPE_DMA_BUF ||
      !buffer->dma_buf.dma_buf)
    {
      sync_point_free (acquire);
      sync_point_free (release);
      wl_resource_post_error (syncobj_surface->resource,
                              WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER,
                              "Explicit sync needs a dma-buf buffer");
      return FALSE;
    }

  if (acquire->timeline == release->timeline &&
      acquire->point >= release->point)
    {
      sync_point_free (acquire);
      sync_point_free (release);
      wl_resource_post_error (syncobj_surface->resource,
                              WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
                              "The release point must come after the acquire point");
      return FALSE;
    }

  /* Make every reader of the buffer wait for the acquire point; if that
   * isn't possible, wait for it here */
  sync_fd = sync_point_export_sync_file (acquire);
  if (sync_fd < 0 ||
      !meta_wayland_dma_buf_import_sync_file (buffer->dma_buf.dma_buf, sync_fd))
    {
      uint32_t handle = acquire->timeline->handle;
      uint64_t point = acquire->point;

      g_warning_once ("Explicit sync: waiting for a client buffer on the CPU");
      drmSyncobjTimelineWait (drm_fd, &handle, &point, 1,
                              monotonic_time_ns () + G_TIME_SPAN_SECOND * 1000,
                              DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
    }
  if (sync_fd >= 0)
    close (sync_fd);
  sync_point_free (acquire);

  meta_wayland_buffer_add_release_point (buffer, release);

  return TRUE;
}

void
meta_wayland_linux_drm_syncobj_surface_destroyed (MetaWaylandSurface *surface)
{
  if (surface->drm_syncobj)
    {
      surface->drm_syncobj->surface = NULL;
      surface->drm_syncobj = NULL;
    }
}

static int
open_render_node (void)
{
#ifdef HAVE_NATIVE_BACKEND
  MetaBackend *backend = meta_get_backend ();
  MetaRendererNative *renderer_native;
  MetaGpuKms *gpu_kms;
  char *render_node;
  int fd;

  if (!META_IS_BACKEND_NATIVE (backend))
    return -1;

  renderer_native = META_RENDERER_NATIVE (meta_backend_get_renderer (backend));
  gpu_kms = meta_renderer_native_get_primary_gpu (renderer_native);
  if (!gpu_kms)
    return -1;

  render_node = drmGetRenderDeviceNameFromFd (meta_gpu_kms_get_fd (gpu_kms));
  if (!render_node)
    return -1;

  fd = open (render_node, O_RDWR | O_CLOEXEC);
  free (render_node);

  return fd;
#else
  return -1;
#endif
}

void
meta_wayland_init_linux_drm_syncobj (MetaWaylandCompositor *compositor)
{
  uint64_t timeline_supported = 0;

  drm_fd = open_render_node ();
  if (drm_fd < 0)
    return;

  if (drmGetCap (drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline_supported) != 0 ||
      !timeline_supported)
    {
      close (drm_fd);
      drm_fd = -1;
      return;
    }

  if (!wl_global_create (compositor->wayland_display,
                         &wp_linux_drm_syncobj_manager_v1_interface,
                         META_WP_LINUX_DRM_SYNCOBJ_V1_VERSION,
                         NULL, manager_bind))
    g_warning ("Failed to create the wp_linux_drm_syncobj_manager_v1 global");
}
