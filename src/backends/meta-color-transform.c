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

#include <math.h>
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
  /* ITU-R BT.2100 HLG: signal -> display light as a fraction of 10000
   * cd/m², for a display peaking at @peak cd/m² (inverse OETF, then the
   * OOTF on BT.2020 luminance, with the system gamma for that peak from
   * ITU-R BT.2390: 1.2 at 1000 cd/m²).  HLG is made to be shown this way at
   * any peak, so it is not tone mapped. */
  "vec3 meta_cc_hlg_to_linear (vec3 e, float peak)\n"
  "{\n"
  "  const float a = 0.17883277;\n"
  "  const float b = 0.28466892;\n"
  "  const float c = 0.55991073;\n"
  "  vec3 v = clamp (e, 0.0, 1.0);\n"
  "  vec3 s = mix (v * v / 3.0, (exp ((v - c) / a) + b) / 12.0,\n"
  "                step (vec3 (0.5), v));\n"
  "  float y = dot (s, vec3 (0.2627, 0.6780, 0.0593));\n"
  "  float gamma = 1.2 * pow (1.111, log2 (peak / 1000.0));\n"
  "  return s * (pow (max (y, 1e-6), gamma - 1.0) * (peak / 10000.0));\n"
  "}\n"
  "\n"
  /* ITU-R BT.2390 EETF: tone map luminance from a source peak to a display
   * peak (cd/m², 0 for unknown: no tone mapping), in the PQ domain, on the
   * largest component so hues are kept.  Values are fractions of 10000
   * cd/m². */
  "vec3 meta_cc_tone_map (vec3 l, float src_peak, float dst_peak)\n"
  "{\n"
  "  float m = max (max (l.r, l.g), l.b);\n"
  "  if (dst_peak <= 0.0 || src_peak <= dst_peak || m <= 0.0)\n"
  "    return l;\n"
  "  float src = meta_cc_linear_to_pq (vec3 (src_peak / 10000.0)).x;\n"
  "  float max_lum = meta_cc_linear_to_pq (vec3 (dst_peak / 10000.0)).x / src;\n"
  "  float ks = max (1.5 * max_lum - 0.5, 0.0);\n"
  "  float e = min (meta_cc_linear_to_pq (vec3 (m)).x / src, 1.0);\n"
  "  if (e <= ks)\n"
  "    return l;\n"
  "  float t = (e - ks) / (1.0 - ks);\n"
  "  float t2 = t * t;\n"
  "  float t3 = t2 * t;\n"
  "  float e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks +\n"
  "             (t3 - 2.0 * t2 + t) * (1.0 - ks) +\n"
  "             (-2.0 * t3 + 3.0 * t2) * max_lum;\n"
  "  return l * (meta_cc_pq_to_linear (vec3 (e2 * src)).x / m);\n"
  "}\n"
  "\n"
  /* Brings linear BT.709 colors (1.0 at SDR white) into what an SDR output
   * shows, [0, 1], by moving them toward the gray of their luminance just
   * as far as needed: luminance and hue are kept, only saturation is lost,
   * where clipping each component would shift hues */
  "vec3 meta_cc_clip_to_sdr (vec3 l)\n"
  "{\n"
  "  float y = dot (l, vec3 (0.2126, 0.7152, 0.0722));\n"
  "  if (y <= 0.0)\n"
  "    return vec3 (0.0);\n"
  "  if (y >= 1.0)\n"
  "    return vec3 (1.0);\n"
  "  vec3 d = l - vec3 (y);\n"
  "  vec3 lo = mix (vec3 (1.0), vec3 (y) / max (-d, 1e-6), step (l, vec3 (0.0)));\n"
  "  vec3 hi = mix (vec3 (1.0), vec3 (1.0 - y) / max (d, 1e-6),\n"
  "                 step (vec3 (1.0), l));\n"
  "  vec3 t = min (lo, hi);\n"
  "  return vec3 (y) + min (min (t.r, t.g), t.b) * d;\n"
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

/* A gamma ramp (Night Light, color correction) given for the SDR signal,
 * applied to the SDR-encoded composite before it is converted to PQ: 256
 * entries per channel, interpolated.  Values past SDR white are scaled by
 * the ramp at white, so they are tinted like it. */
#define LUT_SIZE META_COLOR_TRANSFORM_LUT_SIZE

static const char lut_functions[] =
  "uniform vec3 meta_cc_lut[256];\n"
  "\n"
  /* GLSL 1.20: no integer min(), no indexing vectors by a variable */
  "float meta_cc_lut_channel (float v, vec3 sel)\n"
  "{\n"
  "  if (v >= 1.0)\n"
  "    return dot (meta_cc_lut[255], sel) * v;\n"
  "  if (v <= 0.0)\n"
  "    return dot (meta_cc_lut[0], sel) + v;\n"
  "  float p = v * 255.0;\n"
  "  float f = floor (p);\n"
  "  int i = int (f);\n"
  "  int j = int (min (f + 1.0, 255.0));\n"
  "  return mix (dot (meta_cc_lut[i], sel), dot (meta_cc_lut[j], sel),\n"
  "              p - f);\n"
  "}\n"
  "\n"
  "vec3 meta_cc_apply_lut (vec3 c)\n"
  "{\n"
  "  return vec3 (meta_cc_lut_channel (c.r, vec3 (1.0, 0.0, 0.0)),\n"
  "               meta_cc_lut_channel (c.g, vec3 (0.0, 1.0, 0.0)),\n"
  "               meta_cc_lut_channel (c.b, vec3 (0.0, 0.0, 1.0)));\n"
  "}\n";

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

/* The CPU side of the SDR decode in the snippets (see get_sdr_transfer) */
static float
sdr_to_linear (float e)
{
  if (g_strcmp0 (get_sdr_transfer (), "srgb") == 0)
    return e <= 0.04045f ? e / 12.92f : powf ((e + 0.055f) / 1.055f, 2.4f);

  return powf (e, 2.2f);
}

static float
linear_to_pq (float l)
{
  const float m1 = 0.1593017578125f, m2 = 78.84375f;
  const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
  float y = powf (CLAMP (l, 0.0f, 1.0f), m1);

  return powf ((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

/**
 * meta_color_transform_encode_sdr_pixels_pq: (skip)
 * @pixels: premultiplied ARGB8888 (B, G, R, A in memory) SDR pixels
 * @sdr_white: the luminance SDR white is shown at, in cd/m²
 *
 * Converts the pixels, in place, to what an output in the BT.2100 color
 * mode takes: BT.2020 primaries, PQ, SDR white at @sdr_white, the same
 * conversion the composited views get.  For images that skip compositing,
 * as a hardware cursor.  Coverage stays premultiplied, now in PQ.
 */
void
meta_color_transform_encode_sdr_pixels_pq (uint8_t *pixels,
                                           int      width,
                                           int      height,
                                           int      rowstride,
                                           float    sdr_white)
{
  /* BT.709 to BT.2020, linear (ITU-R BT.2087) */
  static const float m[3][3] = {
    { 0.6274f, 0.3293f, 0.0433f },
    { 0.0691f, 0.9195f, 0.0114f },
    { 0.0164f, 0.0880f, 0.8956f },
  };
  float scale = sdr_white / 10000.0f;
  float decode[256];
  int x, y, i;

  for (i = 0; i < 256; i++)
    decode[i] = sdr_to_linear (i / 255.0f);

  for (y = 0; y < height; y++)
    {
      uint8_t *p = pixels + y * rowstride;

      for (x = 0; x < width; x++, p += 4)
        {
          float a = p[3] / 255.0f;
          float rgb[3], lin[3];
          int c;

          if (p[3] == 0)
            continue;

          /* Unpremultiplied R, G, B */
          for (c = 0; c < 3; c++)
            {
              int v = (int) roundf (p[2 - c] / a);

              lin[c] = decode[CLAMP (v, 0, 255)];
            }

          for (c = 0; c < 3; c++)
            rgb[c] = linear_to_pq ((m[c][0] * lin[0] + m[c][1] * lin[1] +
                                    m[c][2] * lin[2]) * scale);

          for (c = 0; c < 3; c++)
            p[2 - c] = (uint8_t) CLAMP (roundf (rgb[c] * a * 255.0f), 0, 255);
        }
    }
}

/* Run @body on the un-premultiplied color in "c" */
static CoglSnippet *
create_snippet (const char *extra_declarations,
                const char *body)
{
  g_autofree char *declarations = NULL;
  g_autofree char *post = NULL;

  /* SDR white depends on the output; see
   * meta_color_transform_set_sdr_white() */
  declarations =
    g_strdup_printf ("%s\n"
                     "uniform float meta_cc_sdr_white;\n"
                     "uniform float meta_cc_src_peak;\n"
                     "uniform float meta_cc_dst_peak;\n"
                     "vec3 meta_cc_sdr_to_linear (vec3 e)\n"
                     "{\n"
                     "  return meta_cc_%s_to_linear (e);\n"
                     "}\n"
                     "vec3 meta_cc_linear_to_sdr (vec3 l)\n"
                     "{\n"
                     "  return meta_cc_linear_to_%s (l);\n"
                     "}\n"
                     "%s",
                     color_functions,
                     get_sdr_transfer (), get_sdr_transfer (),
                     extra_declarations ? extra_declarations : "");
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
    snippet = create_snippet (NULL,
      "  vec3 l = meta_cc_sdr_to_linear (c) *\n"
      "           (meta_cc_sdr_white / 10000.0);\n"
      "  c = meta_cc_linear_to_pq (max (meta_cc_bt709_to_bt2020 * l, 0.0));\n");

  return snippet;
}

/**
 * meta_color_transform_get_composite_to_pq_lut_snippet: (skip)
 *
 * Returns: (transfer none): like
 *   meta_color_transform_get_composite_to_pq_snippet(), applying the gamma
 *   ramp set with meta_color_transform_set_lut() first
 */
CoglSnippet *
meta_color_transform_get_composite_to_pq_lut_snippet (void)
{
  static CoglSnippet *snippet = NULL;

  if (!snippet)
    snippet = create_snippet (lut_functions,
      "  vec3 l = meta_cc_sdr_to_linear (meta_cc_apply_lut (c)) *\n"
      "           (meta_cc_sdr_white / 10000.0);\n"
      "  c = meta_cc_linear_to_pq (max (meta_cc_bt709_to_bt2020 * l, 0.0));\n");

  return snippet;
}

/**
 * meta_color_transform_get_composite_to_display_snippet: (skip)
 *
 * Returns: (transfer none): a fragment snippet converting the composited
 *   colors (BT.709, the SDR encoding) to the primaries of an SDR display
 *   (see meta_color_transform_set_to_display()), for the final blit of a view
 *   of a wide gamut display that shows SDR signals in its own primaries
 */
CoglSnippet *
meta_color_transform_get_composite_to_display_snippet (void)
{
  static CoglSnippet *snippet = NULL;

  if (!snippet)
    /* In the light the display gives for the signal (a 2.2 power, as SDR
     * displays decode it), not in MUFFIN_HDR_SDR_TRANSFER, which is how SDR
     * content is shown in HDR */
    snippet = create_snippet ("uniform mat3 meta_cc_to_display;\n",
      "  vec3 l = meta_cc_g22_to_linear (c);\n"
      "  l = clamp (meta_cc_to_display * l, 0.0, 1.0);\n"
      "  c = meta_cc_linear_to_g22 (l);\n");

  return snippet;
}

/**
 * meta_color_transform_set_to_display: (skip)
 * @to_display: linear BT.709 to the display's linear RGB, row major
 */
void
meta_color_transform_set_to_display (CoglPipeline *pipeline,
                                     const float   to_display[9])
{
  cogl_pipeline_set_uniform_matrix (pipeline,
                                    cogl_pipeline_get_uniform_location (pipeline,
                                                                        "meta_cc_to_display"),
                                    3, 1, TRUE, to_display);
}

/* How the content of each MetaColorTransformDecode becomes linear light, as
 * a fraction of 10000 cd/m²: SDR content has SDR white at the white level
 * the view composites with, linear content its 1.0 at meta_cc_content_unit
 * cd/m², PQ content is absolute, HLG content is shown for the display's
 * peak (1000 cd/m² if unknown) */
static const char * const decode_bodies[] = {
  [META_COLOR_TRANSFORM_DECODE_SDR] =
    "  vec3 l = meta_cc_sdr_to_linear (c) * (meta_cc_sdr_white / 10000.0);\n",
  [META_COLOR_TRANSFORM_DECODE_BT1886] =
    "  vec3 l = sign (c) * pow (abs (c), vec3 (2.4)) *\n"
    "           (meta_cc_sdr_white / 10000.0);\n",
  [META_COLOR_TRANSFORM_DECODE_LINEAR] =
    "  vec3 l = c * (meta_cc_content_unit / 10000.0);\n",
  [META_COLOR_TRANSFORM_DECODE_PQ] =
    "  vec3 l = meta_cc_pq_to_linear (c);\n",
  [META_COLOR_TRANSFORM_DECODE_HLG] =
    "  vec3 l = meta_cc_hlg_to_linear (c, meta_cc_dst_peak > 0.0 ?\n"
    "                                     meta_cc_dst_peak : 1000.0);\n",
};

/**
 * meta_color_transform_get_to_composite_snippet: (skip)
 * @decode: how the content is encoded
 *
 * The snippet tone maps the content (see meta_color_transform_set_peaks()),
 * converts its primaries to BT.709 (see meta_color_transform_set_content())
 * and brings it into range on SDR outputs (see
 * meta_color_transform_set_gamut_clip()).
 *
 * Returns: (transfer none): a fragment snippet converting the content of a
 *   surface to the SDR encoding views are composited in
 */
CoglSnippet *
meta_color_transform_get_to_composite_snippet (MetaColorTransformDecode decode)
{
  static CoglSnippet *snippets[META_COLOR_TRANSFORM_N_DECODES] = { NULL };

  g_return_val_if_fail (decode < META_COLOR_TRANSFORM_N_DECODES, NULL);

  if (!snippets[decode])
    {
      g_autofree char *body =
        g_strconcat (decode_bodies[decode],
                     "  l = meta_cc_tone_map (l, meta_cc_src_peak,\n"
                     "                        meta_cc_dst_peak) *\n"
                     "      (10000.0 / meta_cc_sdr_white);\n"
                     "  l = meta_cc_content_to_bt709 * l;\n"
                     "  if (meta_cc_gamut_clip > 0.5)\n"
                     "    l = meta_cc_clip_to_sdr (l);\n"
                     "  c = meta_cc_linear_to_sdr (l);\n",
                     NULL);

      snippets[decode] =
        create_snippet ("uniform float meta_cc_gamut_clip;\n"
                        "uniform float meta_cc_content_unit;\n"
                        "uniform mat3 meta_cc_content_to_bt709;\n",
                        body);
    }

  return snippets[decode];
}

/**
 * meta_color_transform_set_sdr_white: (skip)
 * @pipeline: a pipeline with one of the snippets above
 * @nits: the luminance of SDR white, in cd/m²
 *
 * Sets the SDR white level the pipeline's color snippet converts with.
 * Every pipeline using the snippets needs it set: the shaders read 0
 * otherwise.
 */
void
meta_color_transform_set_sdr_white (CoglPipeline *pipeline,
                                    float         nits)
{
  int location;

  location = cogl_pipeline_get_uniform_location (pipeline,
                                                 "meta_cc_sdr_white");
  cogl_pipeline_set_uniform_1f (pipeline, location, nits);
}

/**
 * meta_color_transform_set_peaks: (skip)
 * @pipeline: a pipeline with the PQ to composite snippet
 * @src_peak: the peak luminance of the content, in cd/m²
 * @dst_peak: the peak luminance of the display, in cd/m², 0 if unknown
 *
 * Sets the luminances HDR content is tone mapped between; with an unknown
 * display peak, or content within it, it is not tone mapped.
 */
void
meta_color_transform_set_peaks (CoglPipeline *pipeline,
                                float         src_peak,
                                float         dst_peak)
{
  cogl_pipeline_set_uniform_1f (pipeline,
                                cogl_pipeline_get_uniform_location (pipeline,
                                                                    "meta_cc_src_peak"),
                                src_peak);
  cogl_pipeline_set_uniform_1f (pipeline,
                                cogl_pipeline_get_uniform_location (pipeline,
                                                                    "meta_cc_dst_peak"),
                                dst_peak);
}

/**
 * meta_color_transform_set_content: (skip)
 * @pipeline: a pipeline with a to composite snippet
 * @to_bt709: the content's linear RGB to linear BT.709, row major
 * @unit_nits: what 1.0 of linear content is, in cd/m²
 *
 * Describes the content the snippet converts.  Every pipeline using the
 * snippet needs it set.
 */
void
meta_color_transform_set_content (CoglPipeline *pipeline,
                                  const float   to_bt709[9],
                                  float         unit_nits)
{
  cogl_pipeline_set_uniform_matrix (pipeline,
                                    cogl_pipeline_get_uniform_location (pipeline,
                                                                        "meta_cc_content_to_bt709"),
                                    3, 1, TRUE, to_bt709);
  cogl_pipeline_set_uniform_1f (pipeline,
                                cogl_pipeline_get_uniform_location (pipeline,
                                                                    "meta_cc_content_unit"),
                                unit_nits);
}

/* RGB to XYZ for primaries given as x, y of red, green, blue and white */
static void
rgb_to_xyz (const double p[8],
            double       m[3][3])
{
  double xyz[3][3], w[3], s[3], inv[3][3], det;
  int i, j;

  for (i = 0; i < 3; i++)
    {
      xyz[0][i] = p[2 * i] / p[2 * i + 1];
      xyz[1][i] = 1.0;
      xyz[2][i] = (1.0 - p[2 * i] - p[2 * i + 1]) / p[2 * i + 1];
    }
  w[0] = p[6] / p[7];
  w[1] = 1.0;
  w[2] = (1.0 - p[6] - p[7]) / p[7];

  det = xyz[0][0] * (xyz[1][1] * xyz[2][2] - xyz[1][2] * xyz[2][1]) -
        xyz[0][1] * (xyz[1][0] * xyz[2][2] - xyz[1][2] * xyz[2][0]) +
        xyz[0][2] * (xyz[1][0] * xyz[2][1] - xyz[1][1] * xyz[2][0]);
  inv[0][0] = (xyz[1][1] * xyz[2][2] - xyz[1][2] * xyz[2][1]) / det;
  inv[0][1] = (xyz[0][2] * xyz[2][1] - xyz[0][1] * xyz[2][2]) / det;
  inv[0][2] = (xyz[0][1] * xyz[1][2] - xyz[0][2] * xyz[1][1]) / det;
  inv[1][0] = (xyz[1][2] * xyz[2][0] - xyz[1][0] * xyz[2][2]) / det;
  inv[1][1] = (xyz[0][0] * xyz[2][2] - xyz[0][2] * xyz[2][0]) / det;
  inv[1][2] = (xyz[0][2] * xyz[1][0] - xyz[0][0] * xyz[1][2]) / det;
  inv[2][0] = (xyz[1][0] * xyz[2][1] - xyz[1][1] * xyz[2][0]) / det;
  inv[2][1] = (xyz[0][1] * xyz[2][0] - xyz[0][0] * xyz[2][1]) / det;
  inv[2][2] = (xyz[0][0] * xyz[1][1] - xyz[0][1] * xyz[1][0]) / det;

  for (i = 0; i < 3; i++)
    s[i] = inv[i][0] * w[0] + inv[i][1] * w[1] + inv[i][2] * w[2];

  for (i = 0; i < 3; i++)
    for (j = 0; j < 3; j++)
      m[i][j] = xyz[i][j] * s[j];
}

static const double bt709_primaries[8] = {
  0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290,
};

/* Linear RGB in the primaries @src to linear RGB in @dst, row major */
static void
rgb_to_rgb (const double src_primaries[8],
            const double dst_primaries[8],
            float        out[9])
{
  double src[3][3], dst[3][3], inv[3][3], det;
  int i, j, k;

  rgb_to_xyz (src_primaries, src);
  rgb_to_xyz (dst_primaries, dst);

  det = dst[0][0] * (dst[1][1] * dst[2][2] - dst[1][2] * dst[2][1]) -
        dst[0][1] * (dst[1][0] * dst[2][2] - dst[1][2] * dst[2][0]) +
        dst[0][2] * (dst[1][0] * dst[2][1] - dst[1][1] * dst[2][0]);
  inv[0][0] = (dst[1][1] * dst[2][2] - dst[1][2] * dst[2][1]) / det;
  inv[0][1] = (dst[0][2] * dst[2][1] - dst[0][1] * dst[2][2]) / det;
  inv[0][2] = (dst[0][1] * dst[1][2] - dst[0][2] * dst[1][1]) / det;
  inv[1][0] = (dst[1][2] * dst[2][0] - dst[1][0] * dst[2][2]) / det;
  inv[1][1] = (dst[0][0] * dst[2][2] - dst[0][2] * dst[2][0]) / det;
  inv[1][2] = (dst[0][2] * dst[1][0] - dst[0][0] * dst[1][2]) / det;
  inv[2][0] = (dst[1][0] * dst[2][1] - dst[1][1] * dst[2][0]) / det;
  inv[2][1] = (dst[0][1] * dst[2][0] - dst[0][0] * dst[2][1]) / det;
  inv[2][2] = (dst[0][0] * dst[1][1] - dst[0][1] * dst[1][0]) / det;

  for (i = 0; i < 3; i++)
    for (j = 0; j < 3; j++)
      {
        double v = 0.0;

        for (k = 0; k < 3; k++)
          v += inv[i][k] * src[k][j];
        out[i * 3 + j] = v;
      }
}

/**
 * meta_color_transform_get_to_bt709: (skip)
 * @primaries: x, y of red, green, blue and white, times 1000000
 * @to_bt709: (out): linear RGB in @primaries to linear BT.709, row major
 *
 * The white points are taken to be the same (D65), as they are for every
 * named set of primaries clients can use.
 */
void
meta_color_transform_get_to_bt709 (const int32_t primaries[8],
                                   float         to_bt709[9])
{
  double p[8];
  int i;

  for (i = 0; i < 8; i++)
    p[i] = primaries[i] / 1000000.0;

  rgb_to_rgb (p, bt709_primaries, to_bt709);
}

/**
 * meta_color_transform_get_bt709_to_display: (skip)
 * @display: x, y of the display's red, green, blue and white
 * @to_display: (out): linear BT.709 to the display's linear RGB, row major
 *
 * BT.709 white is taken to be the display's white, so white and greys stay
 * neutral on it whatever white point its EDID gives.
 */
void
meta_color_transform_get_bt709_to_display (const double display[8],
                                           float        to_display[9])
{
  double src[8];
  int i;

  for (i = 0; i < 6; i++)
    src[i] = bt709_primaries[i];
  src[6] = display[6];
  src[7] = display[7];

  rgb_to_rgb (src, display, to_display);
}

/**
 * meta_color_transform_set_gamut_clip: (skip)
 * @pipeline: a pipeline with the PQ to composite snippet
 * @clip: whether the content is painted for an SDR output
 *
 * Content painted for an SDR output is brought into its range (see
 * meta_cc_clip_to_sdr) instead of being clipped per component; for an
 * output in the BT.2100 color mode it is left past it.
 */
void
meta_color_transform_set_gamut_clip (CoglPipeline *pipeline,
                                     gboolean      clip)
{
  cogl_pipeline_set_uniform_1f (pipeline,
                                cogl_pipeline_get_uniform_location (pipeline,
                                                                    "meta_cc_gamut_clip"),
                                clip ? 1.0f : 0.0f);
}

/**
 * meta_color_transform_set_lut: (skip)
 * @pipeline: a pipeline with the composite to PQ LUT snippet
 * @size: the number of entries of the ramps
 * @red: (array length=size): the red ramp, 0 to 65535
 * @green: (array length=size): the green ramp
 * @blue: (array length=size): the blue ramp
 *
 * Sets the gamma ramp the snippet applies, resampled to its size.
 */
void
meta_color_transform_set_lut (CoglPipeline         *pipeline,
                              size_t                size,
                              const unsigned short *red,
                              const unsigned short *green,
                              const unsigned short *blue)
{
  float values[LUT_SIZE * 3];
  int i;

  for (i = 0; i < LUT_SIZE; i++)
    {
      /* the ramp entry at i / (LUT_SIZE - 1) of its range, interpolated */
      double pos = (double) i * (size - 1) / (LUT_SIZE - 1);
      size_t j = MIN ((size_t) pos, size - 1);
      size_t k = MIN (j + 1, size - 1);
      double f = pos - j;

      values[i * 3 + 0] = (red[j] + (red[k] - (double) red[j]) * f) / 65535.0;
      values[i * 3 + 1] = (green[j] + (green[k] - (double) green[j]) * f) / 65535.0;
      values[i * 3 + 2] = (blue[j] + (blue[k] - (double) blue[j]) * f) / 65535.0;
    }

  cogl_pipeline_set_uniform_float (pipeline,
                                   cogl_pipeline_get_uniform_location (pipeline,
                                                                       "meta_cc_lut"),
                                   3, LUT_SIZE, values);
}
