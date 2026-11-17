/*
 * Copyright (C) 2016 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __CLUTTER_STAGE_VIEW_H__
#define __CLUTTER_STAGE_VIEW_H__

#if !defined(__CLUTTER_H_INSIDE__) && !defined(CLUTTER_COMPILATION)
#error "Only <clutter/clutter.h> can be included directly."
#endif

#include <cairo.h>
#include <glib-object.h>
#include <cogl/cogl.h>

#include "clutter-macros.h"

#define CLUTTER_TYPE_STAGE_VIEW (clutter_stage_view_get_type ())
CLUTTER_EXPORT
G_DECLARE_DERIVABLE_TYPE (ClutterStageView, clutter_stage_view,
                          CLUTTER, STAGE_VIEW,
                          GObject)

struct _ClutterStageViewClass
{
  GObjectClass parent_class;

  void (* setup_offscreen_blit_pipeline) (ClutterStageView *view,
                                          CoglPipeline     *pipeline);

  void (* get_offscreen_transformation_matrix) (ClutterStageView *view,
                                                CoglMatrix       *matrix);

  void (* transform_rect_to_onscreen) (ClutterStageView            *view,
                                       const cairo_rectangle_int_t *src_rect,
                                       int                          dst_width,
                                       int                          dst_height,
                                       cairo_rectangle_int_t       *dst_rect);
};

CLUTTER_EXPORT
void clutter_stage_view_get_layout (ClutterStageView      *view,
                                    cairo_rectangle_int_t *rect);

CLUTTER_EXPORT
CoglFramebuffer *clutter_stage_view_get_framebuffer (ClutterStageView *view);
CLUTTER_EXPORT
CoglFramebuffer *clutter_stage_view_get_onscreen (ClutterStageView *view);
CLUTTER_EXPORT
void             clutter_stage_view_invalidate_offscreen_blit_pipeline (ClutterStageView *view);

CLUTTER_EXPORT
float clutter_stage_view_get_scale (ClutterStageView *view);

CLUTTER_EXPORT
void clutter_stage_view_get_offscreen_transformation_matrix (ClutterStageView *view,
                                                             CoglMatrix       *matrix);

/* Whether the view was drawn (or scanned out) in the last stage redraw */
CLUTTER_EXPORT
gboolean clutter_stage_view_was_painted (ClutterStageView *view);

/* Whether the view still waits for its last frame to be shown */
CLUTTER_EXPORT
gboolean clutter_stage_view_has_pending_frame (ClutterStageView *view);

/* Whether part of the view is queued to be drawn */
CLUTTER_EXPORT
gboolean clutter_stage_view_has_queued_redraw (ClutterStageView *view);

/* When the next frame the view would draw from now on would be shown at the
 * earliest (microseconds, monotonic clock; 0 if unknown), and its refresh
 * interval */
CLUTTER_EXPORT
int64_t clutter_stage_view_get_next_presentation_time (ClutterStageView *view,
                                                       int64_t          *refresh_interval_out);

/* Whether the view's monitor varies its refresh rate now: it shows a frame
 * when one is ready instead of at a fixed rate */
CLUTTER_EXPORT
void clutter_stage_view_set_vrr_active (ClutterStageView *view,
                                        gboolean          vrr_active);

CLUTTER_EXPORT
gboolean clutter_stage_view_is_vrr_active (ClutterStageView *view);

#endif /* __CLUTTER_STAGE_VIEW_H__ */
