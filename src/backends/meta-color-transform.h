/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

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

#ifndef META_COLOR_TRANSFORM_H
#define META_COLOR_TRANSFORM_H

#include "cogl/cogl.h"

/* The SDR white levels allowed, in cd/m² */
#define META_MIN_SDR_WHITE_LEVEL 80
#define META_MAX_SDR_WHITE_LEVEL 1000

/* The peak luminances that can be set for a display, in cd/m² */
#define META_MIN_HDR_PEAK_LUMINANCE 80
#define META_MAX_HDR_PEAK_LUMINANCE 10000

/* The luminance SDR white is shown at on outputs in the BT.2100 color mode,
 * in cd/m²: 203 unless MUFFIN_HDR_SDR_WHITE_NITS says otherwise */
float meta_color_transform_get_sdr_white_nits (void);

CoglSnippet * meta_color_transform_get_composite_to_pq_snippet (void);

CoglSnippet * meta_color_transform_get_pq_to_composite_snippet (void);

void meta_color_transform_set_sdr_white (CoglPipeline *pipeline,
                                         float         nits);

#endif /* META_COLOR_TRANSFORM_H */
