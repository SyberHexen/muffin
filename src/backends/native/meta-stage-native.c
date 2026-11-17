/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2016 Red Hat Inc.
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
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#include "config.h"

#include "backends/native/meta-stage-native.h"

#include "backends/meta-backend-private.h"
#include "backends/native/meta-renderer-native.h"
#include "meta/meta-backend.h"
#include "meta/meta-cursor-tracker.h"
#include "meta/meta-monitor-manager.h"
#include "meta/util.h"

static GQuark quark_view_frame_closure  = 0;

struct _MetaStageNative
{
  ClutterStageCogl parent;

  CoglClosure *frame_closure;
};

static void
clutter_stage_window_iface_init (ClutterStageWindowInterface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaStageNative, meta_stage_native,
                         CLUTTER_TYPE_STAGE_COGL,
                         G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_STAGE_WINDOW,
                                                clutter_stage_window_iface_init))

/* A view's frame events: each monitor's frames are timed by its own */
typedef struct _ViewFrameData
{
  MetaStageNative *stage_native;
  ClutterStageView *view;

  int64_t presented_frame_counter_sync;
  int64_t presented_frame_counter_complete;
} ViewFrameData;

static void
view_frame_data_free (gpointer data)
{
  ViewFrameData *frame_data = data;

  g_clear_weak_pointer (&frame_data->view);
  g_free (frame_data);
}

static void
frame_cb (CoglOnscreen  *onscreen,
          CoglFrameEvent frame_event,
          CoglFrameInfo *frame_info,
          void          *user_data)

{
  ViewFrameData *frame_data = user_data;
  ClutterStageCogl *stage_cogl = CLUTTER_STAGE_COGL (frame_data->stage_native);
  int64_t global_frame_counter;
  int64_t presented_frame_counter;
  ClutterFrameInfo clutter_frame_info;

  if (!frame_data->view)
    return;

  global_frame_counter = cogl_frame_info_get_global_frame_counter (frame_info);

  switch (frame_event)
    {
    case COGL_FRAME_EVENT_SYNC:
      presented_frame_counter = frame_data->presented_frame_counter_sync;
      frame_data->presented_frame_counter_sync = global_frame_counter;
      break;
    case COGL_FRAME_EVENT_COMPLETE:
      presented_frame_counter = frame_data->presented_frame_counter_complete;
      frame_data->presented_frame_counter_complete = global_frame_counter;
      break;
    default:
      g_assert_not_reached ();
    }

  if (global_frame_counter <= presented_frame_counter)
    return;

  clutter_frame_info = (ClutterFrameInfo) {
    .frame_counter = global_frame_counter,
    .refresh_rate = cogl_frame_info_get_refresh_rate (frame_info),
    .presentation_time = cogl_frame_info_get_presentation_time (frame_info),
    .sequence = cogl_frame_info_get_sequence (frame_info),
    .hw_clock = cogl_frame_info_get_hw_clock (frame_info),
    .tearing = cogl_frame_info_get_tearing (frame_info),
  };

  _clutter_stage_cogl_view_presented (stage_cogl, frame_data->view,
                                      frame_event, &clutter_frame_info);
}

static void
ensure_frame_callback (MetaStageNative  *stage_native,
                       ClutterStageView *stage_view)
{
  CoglFramebuffer *framebuffer;
  CoglOnscreen *onscreen;
  CoglClosure *closure;
  ViewFrameData *frame_data;

  closure = g_object_get_qdata (G_OBJECT (stage_view),
                                quark_view_frame_closure);
  if (closure)
    return;

  frame_data = g_new0 (ViewFrameData, 1);
  frame_data->stage_native = stage_native;
  g_set_weak_pointer (&frame_data->view, stage_view);
  frame_data->presented_frame_counter_sync = -1;
  frame_data->presented_frame_counter_complete = -1;

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  onscreen = COGL_ONSCREEN (framebuffer);
  closure = cogl_onscreen_add_frame_callback (onscreen,
                                              frame_cb,
                                              frame_data,
                                              view_frame_data_free);
  g_object_set_qdata (G_OBJECT (stage_view),
                      quark_view_frame_closure,
                      closure);
}

static void
ensure_frame_callbacks (MetaStageNative *stage_native)
{
  MetaBackend *backend = meta_get_backend ();
  MetaRenderer *renderer = meta_backend_get_renderer (backend);
  GList *l;

  for (l = meta_renderer_get_views (renderer); l; l = l->next)
    {
      ClutterStageView *stage_view = l->data;

      ensure_frame_callback (stage_native, stage_view);
    }
}

void
meta_stage_native_rebuild_views (MetaStageNative *stage_native)
{
  MetaBackend *backend = meta_get_backend ();
  MetaRenderer *renderer = meta_backend_get_renderer (backend);
  ClutterActor *stage = meta_backend_get_stage (backend);

  meta_renderer_rebuild_views (renderer);
  clutter_stage_clear_stage_views (CLUTTER_STAGE (stage));
  ensure_frame_callbacks (stage_native);
}

static gboolean
meta_stage_native_can_clip_redraws (ClutterStageWindow *stage_window)
{
  return TRUE;
}

static void
meta_stage_native_get_geometry (ClutterStageWindow    *stage_window,
                                cairo_rectangle_int_t *geometry)
{
  MetaBackend *backend = meta_get_backend ();
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (backend);

  if (monitor_manager)
    {
      int width, height;

      meta_monitor_manager_get_screen_size (monitor_manager, &width, &height);
      *geometry = (cairo_rectangle_int_t) {
        .width = width,
        .height = height,
      };
    }
  else
    {
      *geometry = (cairo_rectangle_int_t) {
        .width = 1,
        .height = 1,
      };
    }
}

static GList *
meta_stage_native_get_views (ClutterStageWindow *stage_window)
{
  MetaBackend *backend = meta_get_backend ();
  MetaRenderer *renderer = meta_backend_get_renderer (backend);

  return meta_renderer_get_views (renderer);
}

static int64_t
meta_stage_native_get_frame_counter (ClutterStageWindow *stage_window)
{
  MetaBackend *backend = meta_get_backend ();
  MetaRenderer *renderer = meta_backend_get_renderer (backend);
  MetaRendererNative *renderer_native = META_RENDERER_NATIVE (renderer);

  return meta_renderer_native_get_frame_counter (renderer_native);
}

static void
meta_stage_native_finish_frame (ClutterStageWindow *stage_window)
{
  MetaBackend *backend = meta_get_backend ();
  MetaRenderer *renderer = meta_backend_get_renderer (backend);

  meta_renderer_native_finish_frame (META_RENDERER_NATIVE (renderer));
}

static void
meta_stage_native_set_cursor_visible (ClutterStageWindow *stage_window,
                                      gboolean            cursor_visible)
{
  MetaBackend *backend = meta_get_backend ();
  MetaCursorTracker *cursor_tracker = meta_backend_get_cursor_tracker (backend);

  meta_cursor_tracker_set_pointer_visible (cursor_tracker, cursor_visible);
}

static void
meta_stage_native_init (MetaStageNative *stage_native)
{
}

static void
meta_stage_native_class_init (MetaStageNativeClass *klass)
{
  quark_view_frame_closure =
    g_quark_from_static_string ("-meta-native-stage-view-frame-closure");
}

static void
clutter_stage_window_iface_init (ClutterStageWindowInterface *iface)
{
  iface->can_clip_redraws = meta_stage_native_can_clip_redraws;
  iface->get_geometry = meta_stage_native_get_geometry;
  iface->get_views = meta_stage_native_get_views;
  iface->get_frame_counter = meta_stage_native_get_frame_counter;
  iface->finish_frame = meta_stage_native_finish_frame;
  iface->set_cursor_visible = meta_stage_native_set_cursor_visible;
}
