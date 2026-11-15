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

/*
 * Color correction per display, kept apart from the gamma ramp clients set
 * with SetCrtcGamma (Night Light) and applied on top of it, so setting one
 * does not undo the other.  It belongs to the display, not to a monitor
 * layout, so it is stored per display (vendor, product, serial) in its own
 * key file, ~/.config/cinnamon-monitor-color-correction.ini.
 */

#include "config.h"

#include "backends/meta-color-correction.h"

#include <math.h>

#define BRIGHTNESS_MIN -1.0
#define BRIGHTNESS_MAX 1.0
#define CONTRAST_MIN -1.0
#define CONTRAST_MAX 1.0
#define GAMMA_MIN 0.1
#define GAMMA_MAX 10.0

/* The correction ramp is computed at this size and interpolated */
#define RAMP_SIZE 1024

static const char * const channel_names[] = { "red", "green", "blue" };

void
meta_color_correction_reset (MetaColorCorrection *cc)
{
  int ch;

  for (ch = 0; ch < 3; ch++)
    {
      cc->brightness[ch] = 0.0;
      cc->contrast[ch] = 0.0;
      cc->gamma[ch] = 1.0;
    }
}

gboolean
meta_color_correction_is_neutral (const MetaColorCorrection *cc)
{
  int ch;

  for (ch = 0; ch < 3; ch++)
    {
      if (cc->brightness[ch] != 0.0 || cc->contrast[ch] != 0.0 ||
          cc->gamma[ch] != 1.0)
        return FALSE;
    }

  return TRUE;
}

gboolean
meta_color_correction_from_values (MetaColorCorrection *cc,
                                   const double        *values,
                                   size_t               n_values)
{
  int ch;

  if (n_values != META_COLOR_CORRECTION_N_VALUES)
    return FALSE;

  for (ch = 0; ch < 3; ch++)
    {
      if (!(values[ch] >= BRIGHTNESS_MIN && values[ch] <= BRIGHTNESS_MAX) ||
          !(values[3 + ch] >= CONTRAST_MIN && values[3 + ch] <= CONTRAST_MAX) ||
          !(values[6 + ch] >= GAMMA_MIN && values[6 + ch] <= GAMMA_MAX))
        return FALSE;
    }

  for (ch = 0; ch < 3; ch++)
    {
      cc->brightness[ch] = values[ch];
      cc->contrast[ch] = values[3 + ch];
      cc->gamma[ch] = values[6 + ch];
    }

  return TRUE;
}

void
meta_color_correction_to_values (const MetaColorCorrection *cc,
                                 double                    *values)
{
  int ch;

  for (ch = 0; ch < 3; ch++)
    {
      values[ch] = cc->brightness[ch];
      values[3 + ch] = cc->contrast[ch];
      values[6 + ch] = cc->gamma[ch];
    }
}

static char *
get_path (void)
{
  return g_build_filename (g_get_user_config_dir (),
                           "cinnamon-monitor-color-correction.ini", NULL);
}

static char *
get_group (const char *vendor,
           const char *product,
           const char *serial)
{
  return g_strdup_printf ("%s:%s:%s", vendor ? vendor : "",
                          product ? product : "", serial ? serial : "");
}

void
meta_color_correction_load (const char          *vendor,
                            const char          *product,
                            const char          *serial,
                            MetaColorCorrection *cc)
{
  g_autoptr (GKeyFile) key_file = g_key_file_new ();
  g_autofree char *path = get_path ();
  g_autofree char *group = get_group (vendor, product, serial);
  double values[META_COLOR_CORRECTION_N_VALUES];
  int ch;

  meta_color_correction_reset (cc);

  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, NULL) ||
      !g_key_file_has_group (key_file, group))
    return;

  for (ch = 0; ch < 3; ch++)
    {
      g_autofree char *b = g_strdup_printf ("%s-brightness", channel_names[ch]);
      g_autofree char *c = g_strdup_printf ("%s-contrast", channel_names[ch]);
      g_autofree char *g = g_strdup_printf ("%s-gamma", channel_names[ch]);

      values[ch] = g_key_file_has_key (key_file, group, b, NULL) ?
        g_key_file_get_double (key_file, group, b, NULL) : 0.0;
      values[3 + ch] = g_key_file_has_key (key_file, group, c, NULL) ?
        g_key_file_get_double (key_file, group, c, NULL) : 0.0;
      values[6 + ch] = g_key_file_has_key (key_file, group, g, NULL) ?
        g_key_file_get_double (key_file, group, g, NULL) : 1.0;
    }

  if (!meta_color_correction_from_values (cc, values,
                                          META_COLOR_CORRECTION_N_VALUES))
    {
      g_warning ("Ignoring invalid color correction for %s in %s",
                 group, path);
      meta_color_correction_reset (cc);
    }
}

void
meta_color_correction_store (const char                *vendor,
                             const char                *product,
                             const char                *serial,
                             const MetaColorCorrection *cc)
{
  g_autoptr (GKeyFile) key_file = g_key_file_new ();
  g_autoptr (GError) error = NULL;
  g_autofree char *path = get_path ();
  g_autofree char *group = get_group (vendor, product, serial);
  int ch;

  g_key_file_load_from_file (key_file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

  if (meta_color_correction_is_neutral (cc))
    {
      g_key_file_remove_group (key_file, group, NULL);
    }
  else
    {
      for (ch = 0; ch < 3; ch++)
        {
          g_autofree char *b = g_strdup_printf ("%s-brightness", channel_names[ch]);
          g_autofree char *c = g_strdup_printf ("%s-contrast", channel_names[ch]);
          g_autofree char *g = g_strdup_printf ("%s-gamma", channel_names[ch]);

          g_key_file_set_double (key_file, group, b, cc->brightness[ch]);
          g_key_file_set_double (key_file, group, c, cc->contrast[ch]);
          g_key_file_set_double (key_file, group, g, cc->gamma[ch]);
        }
    }

  if (!g_key_file_save_to_file (key_file, path, &error))
    g_warning ("Failed to save color correction to %s: %s",
               path, error->message);
}

/* NV-CONTROL's ComputeGammaRampVal(), for a ramp of RAMP_SIZE entries */
static double
correction_ramp_value (int    i,
                       double contrast,
                       double brightness,
                       double gamma)
{
  double num = RAMP_SIZE - 1;
  double scale = num / 3.0;
  double half;
  double j = i;

  contrast *= scale;
  if (contrast > 0.0)
    {
      half = num / 2.0 - 1.0;
      j = (j - half) * (half / (half - contrast)) + half;
    }
  else
    {
      half = num / 2.0;
      j = (j - half) * ((half + contrast) / half) + half;
    }

  j = CLAMP (j + brightness * scale, 0.0, num);

  if (gamma != 1.0)
    j = pow (j / num, 1.0 / gamma) * num;

  return j / num;
}

void
meta_color_correction_apply (const MetaColorCorrection *cc,
                             int                        size,
                             uint16_t                  *red,
                             uint16_t                  *green,
                             uint16_t                  *blue)
{
  uint16_t *ramps[3] = { red, green, blue };
  int ch, i;

  if (meta_color_correction_is_neutral (cc))
    return;

  for (ch = 0; ch < 3; ch++)
    {
      double table[RAMP_SIZE];

      for (i = 0; i < RAMP_SIZE; i++)
        table[i] = correction_ramp_value (i, cc->contrast[ch],
                                          cc->brightness[ch], cc->gamma[ch]);

      for (i = 0; i < size; i++)
        {
          double pos = ramps[ch][i] / 65535.0 * (RAMP_SIZE - 1);
          int k = MIN ((int) pos, RAMP_SIZE - 1);
          int l = MIN (k + 1, RAMP_SIZE - 1);
          double v = table[k] + (table[l] - table[k]) * (pos - k);

          ramps[ch][i] = (uint16_t) CLAMP (round (v * 65535.0), 0, 65535);
        }
    }
}
