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

/**
 * SECTION:meta-renderer-view
 * @title: MetaRendererView
 * @short_description: Renders (a part of) the global stage.
 *
 * A MetaRendererView object is responsible for rendering (a part of) the
 * global stage, or more precisely: the part that matches what can be seen on a
 * #MetaLogicalMonitor. By splitting up the rendering into different parts and
 * attaching it to a #MetaLogicalMonitor, we can do the rendering so that each
 * renderer view is responsible for applying the right #MetaMonitorTransform
 * and the right scaling.
 */

#include "config.h"

#include "backends/meta-renderer-view.h"

#include <string.h>

#include "backends/meta-color-transform.h"
#include "backends/meta-renderer.h"
#include "clutter/clutter-muffin.h"
#include "compositor/region-utils.h"

enum
{
  PROP_0,

  PROP_TRANSFORM,
  PROP_COLOR_MODE,
  PROP_SDR_WHITE_LEVEL,
  PROP_HDR_PEAK_LUMINANCE,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

struct _MetaRendererView
{
  ClutterStageViewCogl parent;

  MetaMonitorTransform transform;
  MetaOutputColorMode color_mode;
  float sdr_white_level;
  float hdr_peak_luminance;

  /* The gamma ramp of the CRTC, applied in the blit in the BT.2100 color
   * mode; size 0 for none */
  size_t gamma_size;
  unsigned short *gamma_red;
  unsigned short *gamma_green;
  unsigned short *gamma_blue;

  /* An SDR view of a wide gamut display that shows SDR signals in its own
   * primaries: the blit converts the composited BT.709 colors to them */
  gboolean has_to_display;
  float to_display[9];
};

G_DEFINE_TYPE (MetaRendererView, meta_renderer_view,
               CLUTTER_TYPE_STAGE_VIEW_COGL)

MetaMonitorTransform
meta_renderer_view_get_transform (MetaRendererView *view)
{
  return view->transform;
}

MetaOutputColorMode
meta_renderer_view_get_color_mode (MetaRendererView *view)
{
  return view->color_mode;
}

float
meta_renderer_view_get_sdr_white_level (MetaRendererView *view)
{
  return view->sdr_white_level;
}

float
meta_renderer_view_get_hdr_peak_luminance (MetaRendererView *view)
{
  return view->hdr_peak_luminance;
}

static void
clear_gamma (MetaRendererView *view)
{
  g_clear_pointer (&view->gamma_red, g_free);
  g_clear_pointer (&view->gamma_green, g_free);
  g_clear_pointer (&view->gamma_blue, g_free);
  view->gamma_size = 0;
}

/**
 * meta_renderer_view_set_gamma: (skip)
 * @size: entries of the ramps, 0 for none
 *
 * Sets the gamma ramp of the view's CRTC, which a view in the BT.2100
 * color mode applies before converting to PQ.
 */
void
meta_renderer_view_set_gamma (MetaRendererView     *view,
                              size_t                size,
                              const unsigned short *red,
                              const unsigned short *green,
                              const unsigned short *blue)
{
  clear_gamma (view);
  if (size > 1)
    {
      view->gamma_size = size;
      view->gamma_red = g_memdup2 (red, size * sizeof (*red));
      view->gamma_green = g_memdup2 (green, size * sizeof (*green));
      view->gamma_blue = g_memdup2 (blue, size * sizeof (*blue));
    }
  clutter_stage_view_invalidate_offscreen_blit_pipeline (CLUTTER_STAGE_VIEW (view));
}

/**
 * meta_renderer_view_set_to_display: (skip)
 * @to_display: (nullable): linear BT.709 to the display's linear RGB, row
 *   major, or %NULL for none
 *
 * Makes an SDR view convert its colors to the display's primaries in the
 * blit from its offscreen.
 */
void
meta_renderer_view_set_to_display (MetaRendererView *view,
                                   const float       to_display[9])
{
  view->has_to_display = to_display != NULL;
  if (to_display)
    memcpy (view->to_display, to_display, sizeof (view->to_display));
  clutter_stage_view_invalidate_offscreen_blit_pipeline (CLUTTER_STAGE_VIEW (view));
}

gboolean
meta_renderer_view_has_to_display (MetaRendererView *view)
{
  return view->has_to_display;
}

static void
meta_renderer_view_get_offscreen_transformation_matrix (ClutterStageView *view,
                                                        CoglMatrix       *matrix)
{
  MetaRendererView *renderer_view = META_RENDERER_VIEW (view);

  cogl_matrix_init_identity (matrix);

  switch (renderer_view->transform)
    {
    case META_MONITOR_TRANSFORM_NORMAL:
      break;
    case META_MONITOR_TRANSFORM_90:
      cogl_matrix_rotate (matrix, 90, 0, 0, 1);
      cogl_matrix_translate (matrix, 0, -1, 0);
      break;
    case META_MONITOR_TRANSFORM_180:
      cogl_matrix_rotate (matrix, 180, 0, 0, 1);
      cogl_matrix_translate (matrix, -1, -1, 0);
      break;
    case META_MONITOR_TRANSFORM_270:
      cogl_matrix_rotate (matrix, 270, 0, 0, 1);
      cogl_matrix_translate (matrix, -1, 0, 0);
      break;
    case META_MONITOR_TRANSFORM_FLIPPED:
      cogl_matrix_scale (matrix, -1, 1, 1);
      cogl_matrix_translate (matrix, -1, 0, 0);
      break;
    case META_MONITOR_TRANSFORM_FLIPPED_90:
      cogl_matrix_scale (matrix, -1, 1, 1);
      cogl_matrix_rotate (matrix, 90, 0, 0, 1);
      break;
    case META_MONITOR_TRANSFORM_FLIPPED_180:
      cogl_matrix_scale (matrix, -1, 1, 1);
      cogl_matrix_rotate (matrix, 180, 0, 0, 1);
      cogl_matrix_translate (matrix, 0, -1, 0);
      break;
    case META_MONITOR_TRANSFORM_FLIPPED_270:
      cogl_matrix_scale (matrix, -1, 1, 1);
      cogl_matrix_rotate (matrix, 270, 0, 0, 1);
      cogl_matrix_translate (matrix, -1, -1, 0);
      break;
    }
}

static void
meta_renderer_view_setup_offscreen_blit_pipeline (ClutterStageView *view,
                                                  CoglPipeline     *pipeline)
{
  CoglMatrix matrix;

  meta_renderer_view_get_offscreen_transformation_matrix (view, &matrix);
  cogl_pipeline_set_layer_matrix (pipeline, 0, &matrix);

  /* The view was composited in the SDR encoding; the output takes PQ */
  if (META_RENDERER_VIEW (view)->color_mode == META_OUTPUT_COLOR_MODE_BT2100)
    {
      MetaRendererView *renderer_view = META_RENDERER_VIEW (view);

      /* A gamma ramp (Night Light, color correction) is for the SDR signal:
       * apply it before converting to PQ instead of on the hardware */
      if (renderer_view->gamma_size > 1)
        {
          cogl_pipeline_add_snippet (pipeline,
                                     meta_color_transform_get_composite_to_pq_lut_snippet ());
          meta_color_transform_set_lut (pipeline,
                                        renderer_view->gamma_size,
                                        renderer_view->gamma_red,
                                        renderer_view->gamma_green,
                                        renderer_view->gamma_blue);
        }
      else
        {
          cogl_pipeline_add_snippet (pipeline,
                                     meta_color_transform_get_composite_to_pq_snippet ());
        }
      meta_color_transform_set_sdr_white (pipeline,
                                          renderer_view->sdr_white_level);
    }
  else if (META_RENDERER_VIEW (view)->has_to_display)
    {
      MetaRendererView *renderer_view = META_RENDERER_VIEW (view);

      cogl_pipeline_add_snippet (pipeline,
                                 meta_color_transform_get_composite_to_display_snippet ());
      meta_color_transform_set_to_display (pipeline, renderer_view->to_display);
    }
}

static void
meta_renderer_view_transform_rect_to_onscreen (ClutterStageView            *view,
                                               const cairo_rectangle_int_t *src_rect,
                                               int                          dst_width,
                                               int                          dst_height,
                                               cairo_rectangle_int_t       *dst_rect)
{
  MetaRendererView *renderer_view = META_RENDERER_VIEW (view);
  MetaMonitorTransform inverted_transform;

  inverted_transform =
  meta_monitor_transform_invert (renderer_view->transform);
  return meta_rectangle_transform (src_rect,
                                   inverted_transform,
                                   dst_width,
                                   dst_height,
                                   dst_rect);
}

static void
meta_renderer_view_set_transform (MetaRendererView     *view,
                                  MetaMonitorTransform  transform)
{
  if (view->transform == transform)
    return;

  view->transform = transform;
  clutter_stage_view_invalidate_offscreen_blit_pipeline (CLUTTER_STAGE_VIEW (view));
}

static void
meta_renderer_view_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  MetaRendererView *view = META_RENDERER_VIEW (object);

  switch (prop_id)
    {
    case PROP_TRANSFORM:
      g_value_set_uint (value, view->transform);
      break;
    case PROP_COLOR_MODE:
      g_value_set_uint (value, view->color_mode);
      break;
    case PROP_SDR_WHITE_LEVEL:
      g_value_set_float (value, view->sdr_white_level);
      break;
    case PROP_HDR_PEAK_LUMINANCE:
      g_value_set_float (value, view->hdr_peak_luminance);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_renderer_view_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  MetaRendererView *view = META_RENDERER_VIEW (object);

  switch (prop_id)
    {
    case PROP_TRANSFORM:
      meta_renderer_view_set_transform (view, g_value_get_uint (value));
      break;
    case PROP_COLOR_MODE:
      view->color_mode = g_value_get_uint (value);
      clutter_stage_view_invalidate_offscreen_blit_pipeline (CLUTTER_STAGE_VIEW (view));
      break;
    case PROP_SDR_WHITE_LEVEL:
      view->sdr_white_level = g_value_get_float (value);
      clutter_stage_view_invalidate_offscreen_blit_pipeline (CLUTTER_STAGE_VIEW (view));
      break;
    case PROP_HDR_PEAK_LUMINANCE:
      view->hdr_peak_luminance = g_value_get_float (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_renderer_view_init (MetaRendererView *view)
{
}

static void
meta_renderer_view_finalize (GObject *object)
{
  clear_gamma (META_RENDERER_VIEW (object));

  G_OBJECT_CLASS (meta_renderer_view_parent_class)->finalize (object);
}

static void
meta_renderer_view_class_init (MetaRendererViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterStageViewClass *view_class = CLUTTER_STAGE_VIEW_CLASS (klass);

  view_class->setup_offscreen_blit_pipeline =
    meta_renderer_view_setup_offscreen_blit_pipeline;
  view_class->get_offscreen_transformation_matrix =
    meta_renderer_view_get_offscreen_transformation_matrix;
  view_class->transform_rect_to_onscreen =
    meta_renderer_view_transform_rect_to_onscreen;

  object_class->finalize = meta_renderer_view_finalize;
  object_class->get_property = meta_renderer_view_get_property;
  object_class->set_property = meta_renderer_view_set_property;

  obj_props[PROP_TRANSFORM] =
    g_param_spec_uint ("transform",
                       "Transform",
                       "Transform to apply to the view",
                       META_MONITOR_TRANSFORM_NORMAL,
                       META_MONITOR_TRANSFORM_FLIPPED_270,
                       META_MONITOR_TRANSFORM_NORMAL,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  obj_props[PROP_COLOR_MODE] =
    g_param_spec_uint ("color-mode",
                       "Color mode",
                       "Color mode of the view's output; in the BT.2100 mode "
                       "the view is composited offscreen and converted",
                       META_OUTPUT_COLOR_MODE_DEFAULT,
                       META_OUTPUT_COLOR_MODE_LAST,
                       META_OUTPUT_COLOR_MODE_DEFAULT,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  obj_props[PROP_SDR_WHITE_LEVEL] =
    g_param_spec_float ("sdr-white-level",
                        "SDR white level",
                        "Luminance of SDR white in the BT.2100 color mode, "
                        "in cd/m²",
                        META_MIN_SDR_WHITE_LEVEL,
                        META_MAX_SDR_WHITE_LEVEL,
                        203.0f,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  obj_props[PROP_HDR_PEAK_LUMINANCE] =
    g_param_spec_float ("hdr-peak-luminance",
                        "HDR peak luminance",
                        "Peak luminance of the view's display in cd/m², "
                        "which HDR content is tone mapped to; 0 if unknown",
                        0.0f,
                        META_MAX_HDR_PEAK_LUMINANCE,
                        0.0f,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST, obj_props);
}
