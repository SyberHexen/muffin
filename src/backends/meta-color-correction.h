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

#ifndef META_COLOR_CORRECTION_H
#define META_COLOR_CORRECTION_H

#include <glib.h>
#include <stdint.h>

/* The color correction of a display, per channel (red, green, blue), as
 * the NV-CONTROL color correction defines it: brightness and contrast from
 * -1 to 1 (0 neutral), gamma from 0.1 to 10 (1 neutral).  It is applied on
 * top of the gamma ramp clients set (Night Light), so both stay in effect. */
typedef struct _MetaColorCorrection
{
  double brightness[3];
  double contrast[3];
  double gamma[3];
} MetaColorCorrection;

#define META_COLOR_CORRECTION_N_VALUES 9

void meta_color_correction_reset (MetaColorCorrection *cc);

gboolean meta_color_correction_is_neutral (const MetaColorCorrection *cc);

/* From / to brightness RGB, contrast RGB, gamma RGB; FALSE if out of range */
gboolean meta_color_correction_from_values (MetaColorCorrection *cc,
                                            const double        *values,
                                            size_t               n_values);

void meta_color_correction_to_values (const MetaColorCorrection *cc,
                                      double                    *values);

/* The correction stored for a display (vendor, product, serial), neutral
 * if none */
void meta_color_correction_load (const char          *vendor,
                                 const char          *product,
                                 const char          *serial,
                                 MetaColorCorrection *cc);

void meta_color_correction_store (const char                *vendor,
                                  const char                *product,
                                  const char                *serial,
                                  const MetaColorCorrection *cc);

/* Applies @cc to the ramps @red, @green, @blue of @size entries, in place */
void meta_color_correction_apply (const MetaColorCorrection *cc,
                                  int                        size,
                                  uint16_t                  *red,
                                  uint16_t                  *green,
                                  uint16_t                  *blue);

#endif /* META_COLOR_CORRECTION_H */
