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
 * Color transforms for compositing SDR and HDR10 content together.
 *
 * Views of outputs in the BT.2100 color mode are painted into a half float
 * offscreen with BT.709 primaries, the SDR transfer function and 1.0 at SDR
 * white.  SDR content needs no conversion to get there, so it blends exactly
 * as it does on SDR outputs; HDR10 surfaces are converted into it, reaching
 * past 1.0 (and below 0.0 for colors outside BT.709).  The view's final blit
 * converts the result to PQ with BT.2020 primaries, with SDR white at the
 * configured luminance.
 *
 * The SDR transfer function is a pure 2.2 power by default, which is how SDR
 * displays show content and what it is made on.  The piecewise sRGB curve
 * (MUFFIN_HDR_SDR_TRANSFER=srgb) lifts the shadows and looks washed out.
 */

#include "config.h"

#include "backends/meta-color-transform.h"

#include <stdlib.h>

#define DEFAULT_SDR_WHITE_NITS 203.0f  /* ITU-R BT.2408 reference white */

static const char color_functions[] =
  /* sRGB, extended to values past [0, 1] by mirroring and continuing */
  "vec3 meta_cc_srgb_to_linear (vec3 e)\n"
  "{\n"
  "  vec3 a = abs (e);\n"
  "  vec3 l = mix (a / 12.92,\n"
  "                pow ((a + 0.055) / 1.055, vec3 (2.4)),\n"
  "                step (vec3 (0.04045), a));\n"
  "  return sign (e) * l;\n"
  "}\n"
  "\n"
  "vec3 meta_cc_linear_to_srgb (vec3 l)\n"
  "{\n"
  "  vec3 a = abs (l);\n"
  "  vec3 e = mix (a * 12.92,\n"
  "                1.055 * pow (a, vec3 (1.0 / 2.4)) - 0.055,\n"
  "                step (vec3 (0.0031308), a));\n"
  "  return sign (l) * e;\n"
  "}\n"
  "\n"
  /* A pure 2.2 power, extended the same way */
  "vec3 meta_cc_g22_to_linear (vec3 e)\n"
  "{\n"
  "  return sign (e) * pow (abs (e), vec3 (2.2));\n"
  "}\n"
  "\n"
  "vec3 meta_cc_linear_to_g22 (vec3 l)\n"
  "{\n"
  "  return sign (l) * pow (abs (l), vec3 (1.0 / 2.2));\n"
  "}\n"
  "\n"
  /* SMPTE ST 2084: signal <-> luminance as a fraction of 10000 cd/m² */
  "vec3 meta_cc_pq_to_linear (vec3 e)\n"
  "{\n"
  "  const float m1 = 0.1593017578125;\n"
  "  const float m2 = 78.84375;\n"
  "  const float c1 = 0.8359375;\n"
  "  const float c2 = 18.8515625;\n"
  "  const float c3 = 18.6875;\n"
  "  vec3 p = pow (clamp (e, 0.0, 1.0), vec3 (1.0 / m2));\n"
  "  return pow (max (p - c1, 0.0) / (c2 - c3 * p), vec3 (1.0 / m1));\n"
  "}\n"
  "\n"
  "vec3 meta_cc_linear_to_pq (vec3 l)\n"
  "{\n"
  "  const float m1 = 0.1593017578125;\n"
  "  const float m2 = 78.84375;\n"
  "  const float c1 = 0.8359375;\n"
  "  const float c2 = 18.8515625;\n"
  "  const float c3 = 18.6875;\n"
  "  vec3 p = pow (clamp (l, 0.0, 1.0), vec3 (m1));\n"
  "  return pow ((c1 + c2 * p) / (1.0 + c3 * p), vec3 (m2));\n"
  "}\n"
  "\n"
  /* Linear BT.709 <-> BT.2020 primaries, derived from their chromaticities
   * and D65 (as in ITU-R BT.2087) and exact inverses of each other, so HDR10
   * content survives the round trip; column major */
  "const mat3 meta_cc_bt709_to_bt2020 =\n"
  "  mat3 (0.627403896, 0.069097289, 0.016391439,\n"
  "        0.329283038, 0.919540395, 0.088013308,\n"
  "        0.043313066, 0.011362316, 0.895595253);\n"
  "\n"
  "const mat3 meta_cc_bt2020_to_bt709 =\n"
  "  mat3 ( 1.660491002, -0.124550475, -0.018150763,\n"
  "        -0.587641139,  1.132899897, -0.100578898,\n"
  "        -0.072849863, -0.008349423,  1.118729661);\n";

float
meta_color_transform_get_sdr_white_nits (void)
{
  static float nits = -1.0f;

  if (nits < 0.0f)
    {
      const char *env = g_getenv ("MUFFIN_HDR_SDR_WHITE_NITS");

      nits = DEFAULT_SDR_WHITE_NITS;
      if (env)
        {
          float value = g_ascii_strtod (env, NULL);

          if (value >= META_MIN_SDR_WHITE_LEVEL && value <= META_MAX_SDR_WHITE_LEVEL)
            nits = value;
          else
            g_warning ("Ignoring MUFFIN_HDR_SDR_WHITE_NITS=%s: "
                       "not between %d and %d",
                       env, META_MIN_SDR_WHITE_LEVEL, META_MAX_SDR_WHITE_LEVEL);
        }
    }

  return nits;
}

/* "srgb" or "g22", the name the GLSL functions use for the SDR transfer */
static const char *
get_sdr_transfer (void)
{
  static const char *transfer = NULL;

  if (!transfer)
    {
      const char *env = g_getenv ("MUFFIN_HDR_SDR_TRANSFER");

      transfer = "g22";
      if (g_strcmp0 (env, "srgb") == 0)
        transfer = "srgb";
      else if (env && g_strcmp0 (env, "gamma22") != 0)
        g_warning ("Ignoring MUFFIN_HDR_SDR_TRANSFER=%s: "
                   "not \"srgb\" or \"gamma22\"", env);
    }

  return transfer;
}

/* Run @body on the un-premultiplied color in "c" */
static CoglSnippet *
create_snippet (const char *body)
{
  g_autofree char *declarations = NULL;
  g_autofree char *post = NULL;
  char white[G_ASCII_DTOSTR_BUF_SIZE];

  /* Not printf's %f, which would write a decimal comma in some locales */
  g_ascii_formatd (white, sizeof (white), "%.4f",
                   meta_color_transform_get_sdr_white_nits ());
  declarations =
    g_strdup_printf ("%s\n"
                     "const float meta_cc_sdr_white = %s;\n"
                     "vec3 meta_cc_sdr_to_linear (vec3 e)\n"
                     "{\n"
                     "  return meta_cc_%s_to_linear (e);\n"
                     "}\n"
                     "vec3 meta_cc_linear_to_sdr (vec3 l)\n"
                     "{\n"
                     "  return meta_cc_linear_to_%s (l);\n"
                     "}\n",
                     color_functions, white,
                     get_sdr_transfer (), get_sdr_transfer ());
  post = g_strdup_printf ("{\n"
                          "  float a = cogl_color_out.a;\n"
                          "  vec3 c = a > 0.0 ? cogl_color_out.rgb / a\n"
                          "                   : cogl_color_out.rgb;\n"
                          "%s"
                          "  cogl_color_out.rgb = c * a;\n"
                          "}\n",
                          body);

  return cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT, declarations, post);
}

/**
 * meta_color_transform_get_composite_to_pq_snippet: (skip)
 *
 * Returns: (transfer none): a fragment snippet converting composited
 *   SDR-encoded colors to PQ with BT.2020 primaries, for the final blit of a view
 *   in the BT.2100 color mode
 */
CoglSnippet *
meta_color_transform_get_composite_to_pq_snippet (void)
{
  static CoglSnippet *snippet = NULL;

  if (!snippet)
    snippet = create_snippet (
      "  vec3 l = meta_cc_sdr_to_linear (c) *\n"
      "           (meta_cc_sdr_white / 10000.0);\n"
      "  c = meta_cc_linear_to_pq (max (meta_cc_bt709_to_bt2020 * l, 0.0));\n");

  return snippet;
}

/**
 * meta_color_transform_get_pq_to_composite_snippet: (skip)
 *
 * Returns: (transfer none): a fragment snippet converting the PQ / BT.2020
 *   content of an HDR10 surface to the SDR encoding views are composited in
 */
CoglSnippet *
meta_color_transform_get_pq_to_composite_snippet (void)
{
  static CoglSnippet *snippet = NULL;

  if (!snippet)
    snippet = create_snippet (
      "  vec3 l = meta_cc_pq_to_linear (c) *\n"
      "           (10000.0 / meta_cc_sdr_white);\n"
      "  c = meta_cc_linear_to_sdr (meta_cc_bt2020_to_bt709 * l);\n");

  return snippet;
}
