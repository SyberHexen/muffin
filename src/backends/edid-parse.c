/*
 * Copyright 2007 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Author: Soren Sandmann <sandmann@redhat.com> */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>

#include "backends/edid.h"

static int
get_bit (int in, int bit)
{
  return (in & (1 << bit)) >> bit;
}

static int
get_bits (int in, int begin, int end)
{
  int mask = (1 << (end - begin + 1)) - 1;

  return (in >> begin) & mask;
}

static int
decode_header (const uchar *edid)
{
  if (memcmp (edid, "\x00\xff\xff\xff\xff\xff\xff\x00", 8) == 0)
    return TRUE;
  return FALSE;
}

static int
decode_vendor_and_product_identification (const uchar *edid, MonitorInfo *info)
{
  int is_model_year;

  /* Manufacturer Code */
  info->manufacturer_code[0]  = get_bits (edid[0x08], 2, 6);
  info->manufacturer_code[1]  = get_bits (edid[0x08], 0, 1) << 3;
  info->manufacturer_code[1] |= get_bits (edid[0x09], 5, 7);
  info->manufacturer_code[2]  = get_bits (edid[0x09], 0, 4);
  info->manufacturer_code[3]  = '\0';

  info->manufacturer_code[0] += 'A' - 1;
  info->manufacturer_code[1] += 'A' - 1;
  info->manufacturer_code[2] += 'A' - 1;

  /* Product Code */
  info->product_code = edid[0x0b] << 8 | edid[0x0a];

  /* Serial Number */
  info->serial_number =
    edid[0x0c] | edid[0x0d] << 8 | edid[0x0e] << 16 | edid[0x0f] << 24;

  /* Week and Year */
  is_model_year = FALSE;
  switch (edid[0x10])
    {
    case 0x00:
      info->production_week = -1;
      break;

    case 0xff:
      info->production_week = -1;
      is_model_year = TRUE;
      break;

    default:
      info->production_week = edid[0x10];
      break;
    }

  if (is_model_year)
    {
      info->production_year = -1;
      info->model_year = 1990 + edid[0x11];
    }
  else
    {
      info->production_year = 1990 + edid[0x11];
      info->model_year = -1;
    }

  return TRUE;
}

static int
decode_edid_version (const uchar *edid, MonitorInfo *info)
{
  info->major_version = edid[0x12];
  info->minor_version = edid[0x13];

  return TRUE;
}

static int
decode_display_parameters (const uchar *edid, MonitorInfo *info)
{
  /* Digital vs Analog */
  info->is_digital = get_bit (edid[0x14], 7);

  if (info->is_digital)
    {
      int bits;

      static const int bit_depth[8] =
        {
          -1, 6, 8, 10, 12, 14, 16, -1
        };

      static const Interface interfaces[6] =
        {
          UNDEFINED, DVI, HDMI_A, HDMI_B, MDDI, DISPLAY_PORT
        };

      bits = get_bits (edid[0x14], 4, 6);
      info->connector.digital.bits_per_primary = bit_depth[bits];

      bits = get_bits (edid[0x14], 0, 3);

      if (bits <= 5)
        info->connector.digital.interface = interfaces[bits];
      else
        info->connector.digital.interface = UNDEFINED;
    }
  else
    {
      int bits = get_bits (edid[0x14], 5, 6);

      static const double levels[][3] =
        {
          { 0.7,   0.3,    1.0 },
          { 0.714, 0.286,  1.0 },
          { 1.0,   0.4,    1.4 },
          { 0.7,   0.0,    0.7 },
        };

      info->connector.analog.video_signal_level = levels[bits][0];
      info->connector.analog.sync_signal_level = levels[bits][1];
      info->connector.analog.total_signal_level = levels[bits][2];

      info->connector.analog.blank_to_black = get_bit (edid[0x14], 4);

      info->connector.analog.separate_hv_sync = get_bit (edid[0x14], 3);
      info->connector.analog.composite_sync_on_h = get_bit (edid[0x14], 2);
      info->connector.analog.composite_sync_on_green = get_bit (edid[0x14], 1);

      info->connector.analog.serration_on_vsync = get_bit (edid[0x14], 0);
    }

  /* Screen Size / Aspect Ratio */
  if (edid[0x15] == 0 && edid[0x16] == 0)
    {
      info->width_mm = -1;
      info->height_mm = -1;
      info->aspect_ratio = -1.0;
    }
  else if (edid[0x16] == 0)
    {
      info->width_mm = -1;
      info->height_mm = -1;
      info->aspect_ratio = 100.0 / (edid[0x15] + 99);
    }
  else if (edid[0x15] == 0)
    {
      info->width_mm = -1;
      info->height_mm = -1;
      info->aspect_ratio = 100.0 / (edid[0x16] + 99);
      info->aspect_ratio = 1/info->aspect_ratio; /* portrait */
    }
  else
    {
      info->width_mm = 10 * edid[0x15];
      info->height_mm = 10 * edid[0x16];
    }

  /* Gamma */
  if (edid[0x17] == 0xFF)
    info->gamma = -1.0;
  else
    info->gamma = (edid[0x17] + 100.0) / 100.0;

  /* Features */
  info->standby = get_bit (edid[0x18], 7);
  info->suspend = get_bit (edid[0x18], 6);
  info->active_off = get_bit (edid[0x18], 5);

  if (info->is_digital)
    {
      info->connector.digital.rgb444 = TRUE;
      if (get_bit (edid[0x18], 3))
        info->connector.digital.ycrcb444 = 1;
      if (get_bit (edid[0x18], 4))
        info->connector.digital.ycrcb422 = 1;
    }
  else
    {
      int bits = get_bits (edid[0x18], 3, 4);
      ColorType color_type[4] =
        {
          MONOCHROME, RGB, OTHER_COLOR, UNDEFINED_COLOR
        };

      info->connector.analog.color_type = color_type[bits];
    }

  info->srgb_is_standard = get_bit (edid[0x18], 2);

  /* In 1.3 this is called "has preferred timing" */
  info->preferred_timing_includes_native = get_bit (edid[0x18], 1);

  /* FIXME: In 1.3 this indicates whether the monitor accepts GTF */
  info->continuous_frequency = get_bit (edid[0x18], 0);
  return TRUE;
}

static double
decode_fraction (int high, int low)
{
  double result = 0.0;
  int i;

  high = (high << 2) | low;

  for (i = 0; i < 10; ++i)
    result += get_bit (high, i) * pow (2, i - 10);

  return result;
}

static int
decode_color_characteristics (const uchar *edid, MonitorInfo *info)
{
  info->red_x = decode_fraction (edid[0x1b], get_bits (edid[0x19], 6, 7));
  info->red_y = decode_fraction (edid[0x1c], get_bits (edid[0x19], 4, 5));
  info->green_x = decode_fraction (edid[0x1d], get_bits (edid[0x19], 2, 3));
  info->green_y = decode_fraction (edid[0x1e], get_bits (edid[0x19], 0, 1));
  info->blue_x = decode_fraction (edid[0x1f], get_bits (edid[0x1a], 6, 7));
  info->blue_y = decode_fraction (edid[0x20], get_bits (edid[0x1a], 4, 5));
  info->white_x = decode_fraction (edid[0x21], get_bits (edid[0x1a], 2, 3));
  info->white_y = decode_fraction (edid[0x22], get_bits (edid[0x1a], 0, 1));

  return TRUE;
}

static int
decode_established_timings (const uchar *edid, MonitorInfo *info)
{
  static const Timing established[][8] =
    {
      {
        { 800, 600, 60 },
        { 800, 600, 56 },
        { 640, 480, 75 },
        { 640, 480, 72 },
        { 640, 480, 67 },
        { 640, 480, 60 },
        { 720, 400, 88 },
        { 720, 400, 70 }
      },
      {
        { 1280, 1024, 75 },
        { 1024, 768, 75 },
        { 1024, 768, 70 },
        { 1024, 768, 60 },
        { 1024, 768, 87 },
        { 832, 624, 75 },
        { 800, 600, 75 },
        { 800, 600, 72 }
	},
      {
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 0, 0, 0 },
        { 1152, 870, 75 }
      },
    };

  int i, j, idx;

  idx = 0;
  for (i = 0; i < 3; ++i)
    {
      for (j = 0; j < 8; ++j)
	{
          int byte = edid[0x23 + i];

          if (get_bit (byte, j) && established[i][j].frequency != 0)
            info->established[idx++] = established[i][j];
	}
    }
  return TRUE;
}

static int
decode_standard_timings (const uchar *edid, MonitorInfo *info)
{
  int i;

  for (i = 0; i < 8; i++)
    {
      int first = edid[0x26 + 2 * i];
      int second = edid[0x27 + 2 * i];

      if (first != 0x01 && second != 0x01)
	{
          int w = 8 * (first + 31);
          int h = 0;

          switch (get_bits (second, 6, 7))
            {
	    case 0x00: h = (w / 16) * 10; break;
	    case 0x01: h = (w / 4) * 3; break;
	    case 0x02: h = (w / 5) * 4; break;
	    case 0x03: h = (w / 16) * 9; break;
	    }

          info->standard[i].width = w;
          info->standard[i].height = h;
          info->standard[i].frequency = get_bits (second, 0, 5) + 60;
	}
    }

  return TRUE;
}

static void
decode_lf_string (const uchar *s, int n_chars, char *result)
{
  int i;
  for (i = 0; i < n_chars; ++i)
    {
      if (s[i] == 0x0a)
	{
          *result++ = '\0';
          break;
	}
      else if (s[i] == 0x00)
	{
          /* Convert embedded 0's to spaces */
          *result++ = ' ';
	}
      else
	{
          *result++ = s[i];
	}
    }
}

static void
decode_display_descriptor (const uchar *desc,
			   MonitorInfo *info)
{
  switch (desc[0x03])
    {
    case 0xFC:
      decode_lf_string (desc + 5, 13, info->dsc_product_name);
      break;
    case 0xFF:
      decode_lf_string (desc + 5, 13, info->dsc_serial_number);
      break;
    case 0xFE:
      decode_lf_string (desc + 5, 13, info->dsc_string);
      break;
    case 0xFD:
      /* Range Limits */
      break;
    case 0xFB:
      /* Color Point */
      break;
    case 0xFA:
      /* Timing Identifications */
      break;
    case 0xF9:
      /* Color Management */
      break;
    case 0xF8:
      /* Timing Codes */
      break;
    case 0xF7:
      /* Established Timings */
      break;
    case 0x10:
      break;
    }
}

static void
decode_detailed_timing (const uchar *timing,
			DetailedTiming *detailed)
{
  int bits;
  StereoType stereo[] =
    {
      NO_STEREO, NO_STEREO, FIELD_RIGHT, FIELD_LEFT,
      TWO_WAY_RIGHT_ON_EVEN, TWO_WAY_LEFT_ON_EVEN,
      FOUR_WAY_INTERLEAVED, SIDE_BY_SIDE
    };

  detailed->pixel_clock = (timing[0x00] | timing[0x01] << 8) * 10000;
  detailed->h_addr = timing[0x02] | ((timing[0x04] & 0xf0) << 4);
  detailed->h_blank = timing[0x03] | ((timing[0x04] & 0x0f) << 8);
  detailed->v_addr = timing[0x05] | ((timing[0x07] & 0xf0) << 4);
  detailed->v_blank = timing[0x06] | ((timing[0x07] & 0x0f) << 8);
  detailed->h_front_porch = timing[0x08] | get_bits (timing[0x0b], 6, 7) << 8;
  detailed->h_sync = timing[0x09] | get_bits (timing[0x0b], 4, 5) << 8;
  detailed->v_front_porch =
    get_bits (timing[0x0a], 4, 7) | get_bits (timing[0x0b], 2, 3) << 4;
  detailed->v_sync =
    get_bits (timing[0x0a], 0, 3) | get_bits (timing[0x0b], 0, 1) << 4;
  detailed->width_mm =  timing[0x0c] | get_bits (timing[0x0e], 4, 7) << 8;
  detailed->height_mm = timing[0x0d] | get_bits (timing[0x0e], 0, 3) << 8;
  detailed->right_border = timing[0x0f];
  detailed->top_border = timing[0x10];

  detailed->interlaced = get_bit (timing[0x11], 7);

  /* Stereo */
  bits = get_bits (timing[0x11], 5, 6) << 1 | get_bit (timing[0x11], 0);
  detailed->stereo = stereo[bits];

  /* Sync */
  bits = timing[0x11];

  detailed->digital_sync = get_bit (bits, 4);
  if (detailed->digital_sync)
    {
      detailed->connector.digital.composite = !get_bit (bits, 3);

      if (detailed->connector.digital.composite)
	{
          detailed->connector.digital.serrations = get_bit (bits, 2);
          detailed->connector.digital.negative_vsync = FALSE;
	}
      else
	{
          detailed->connector.digital.serrations = FALSE;
          detailed->connector.digital.negative_vsync = !get_bit (bits, 2);
	}

      detailed->connector.digital.negative_hsync = !get_bit (bits, 0);
    }
  else
    {
      detailed->connector.analog.bipolar = get_bit (bits, 3);
      detailed->connector.analog.serrations = get_bit (bits, 2);
      detailed->connector.analog.sync_on_green = !get_bit (bits, 1);
    }
}

static int
decode_descriptors (const uchar *edid, MonitorInfo *info)
{
  int i;
  int timing_idx;

  timing_idx = 0;

  for (i = 0; i < 4; ++i)
    {
      int index = 0x36 + i * 18;

      if (edid[index + 0] == 0x00 && edid[index + 1] == 0x00)
	{
          decode_display_descriptor (edid + index, info);
	}
      else
	{
          decode_detailed_timing (edid + index, &(info->detailed_timings[timing_idx++]));
	}
    }

  info->n_detailed_timings = timing_idx;

  return TRUE;
}

static void
decode_check_sum (const uchar *edid,
		  MonitorInfo *info)
{
  int i;
  uchar check = 0;

  for (i = 0; i < 128; ++i)
    check += edid[i];

  info->checksum = check;
}

MonitorInfo *
decode_edid (const uchar *edid)
{
  MonitorInfo *info = g_new0 (MonitorInfo, 1);

  decode_check_sum (edid, info);

  if (decode_header (edid)
      && decode_vendor_and_product_identification (edid, info)
      && decode_edid_version (edid, info)
      && decode_display_parameters (edid, info)
      && decode_color_characteristics (edid, info)
      && decode_established_timings (edid, info)
      && decode_standard_timings (edid, info)
      && decode_descriptors (edid, info))
    {
      return info;
    }
  else
    {
      g_free (info);
      return NULL;
    }
}
/* Every progressive CTA-861 video format (VIC) of at least 480 lines, with
 * its exact refresh rate in mHz (generated from libdisplay-info 0.3.0's
 * di_cta_video_format_from_vic (), CTA-861-I); 4:2:0 data blocks refer to
 * modes by these numbers. */
static const struct
{
  guint8 vic;
  guint16 width, height;
  guint32 mhz;
} cta_vics[] = {
    {   1,   640,  480,  59940 },    {   2,   720,  480,  59940 },    {   3,   720,  480,  59940 },
    {   4,  1280,  720,  60000 },    {  14,  1440,  480,  59940 },    {  15,  1440,  480,  59940 },
    {  16,  1920, 1080,  60000 },    {  17,   720,  576,  50000 },    {  18,   720,  576,  50000 },
    {  19,  1280,  720,  50000 },    {  29,  1440,  576,  50000 },    {  30,  1440,  576,  50000 },
    {  31,  1920, 1080,  50000 },    {  32,  1920, 1080,  24000 },    {  33,  1920, 1080,  25000 },
    {  34,  1920, 1080,  30000 },    {  35,  2880,  480,  59940 },    {  36,  2880,  480,  59940 },
    {  37,  2880,  576,  50000 },    {  38,  2880,  576,  50000 },    {  41,  1280,  720, 100000 },
    {  42,   720,  576, 100000 },    {  43,   720,  576, 100000 },    {  47,  1280,  720, 120000 },
    {  48,   720,  480, 119880 },    {  49,   720,  480, 119880 },    {  52,   720,  576, 200000 },
    {  53,   720,  576, 200000 },    {  56,   720,  480, 239760 },    {  57,   720,  480, 239760 },
    {  60,  1280,  720,  24000 },    {  61,  1280,  720,  25000 },    {  62,  1280,  720,  30000 },
    {  63,  1920, 1080, 120000 },    {  64,  1920, 1080, 100000 },    {  65,  1280,  720,  24000 },
    {  66,  1280,  720,  25000 },    {  67,  1280,  720,  30000 },    {  68,  1280,  720,  50000 },
    {  69,  1280,  720,  60000 },    {  70,  1280,  720, 100000 },    {  71,  1280,  720, 120000 },
    {  72,  1920, 1080,  24000 },    {  73,  1920, 1080,  25000 },    {  74,  1920, 1080,  30000 },
    {  75,  1920, 1080,  50000 },    {  76,  1920, 1080,  60000 },    {  77,  1920, 1080, 100000 },
    {  78,  1920, 1080, 120000 },    {  79,  1680,  720,  24000 },    {  80,  1680,  720,  25000 },
    {  81,  1680,  720,  30000 },    {  82,  1680,  720,  50000 },    {  83,  1680,  720,  60000 },
    {  84,  1680,  720, 100000 },    {  85,  1680,  720, 120000 },    {  86,  2560, 1080,  24000 },
    {  87,  2560, 1080,  25000 },    {  88,  2560, 1080,  30000 },    {  89,  2560, 1080,  50000 },
    {  90,  2560, 1080,  60000 },    {  91,  2560, 1080, 100000 },    {  92,  2560, 1080, 120000 },
    {  93,  3840, 2160,  24000 },    {  94,  3840, 2160,  25000 },    {  95,  3840, 2160,  30000 },
    {  96,  3840, 2160,  50000 },    {  97,  3840, 2160,  60000 },    {  98,  4096, 2160,  24000 },
    {  99,  4096, 2160,  25000 },    { 100,  4096, 2160,  30000 },    { 101,  4096, 2160,  50000 },
    { 102,  4096, 2160,  60000 },    { 103,  3840, 2160,  24000 },    { 104,  3840, 2160,  25000 },
    { 105,  3840, 2160,  30000 },    { 106,  3840, 2160,  50000 },    { 107,  3840, 2160,  60000 },
    { 108,  1280,  720,  48000 },    { 109,  1280,  720,  48000 },    { 110,  1680,  720,  48000 },
    { 111,  1920, 1080,  48000 },    { 112,  1920, 1080,  48000 },    { 113,  2560, 1080,  48000 },
    { 114,  3840, 2160,  48000 },    { 115,  4096, 2160,  48000 },    { 116,  3840, 2160,  48000 },
    { 117,  3840, 2160, 100000 },    { 118,  3840, 2160, 120000 },    { 119,  3840, 2160, 100000 },
    { 120,  3840, 2160, 120000 },    { 121,  5120, 2160,  24000 },    { 122,  5120, 2160,  25000 },
    { 123,  5120, 2160,  30000 },    { 124,  5120, 2160,  48000 },    { 125,  5120, 2160,  50000 },
    { 126,  5120, 2160,  60000 },    { 127,  5120, 2160, 100000 },    { 193,  5120, 2160, 120000 },
    { 194,  7680, 4320,  24000 },    { 195,  7680, 4320,  25000 },    { 196,  7680, 4320,  30000 },
    { 197,  7680, 4320,  48000 },    { 198,  7680, 4320,  50000 },    { 199,  7680, 4320,  60000 },
    { 200,  7680, 4320, 100000 },    { 201,  7680, 4320, 120000 },    { 202,  7680, 4320,  24000 },
    { 203,  7680, 4320,  25000 },    { 204,  7680, 4320,  30000 },    { 205,  7680, 4320,  48000 },
    { 206,  7680, 4320,  50000 },    { 207,  7680, 4320,  60000 },    { 208,  7680, 4320, 100000 },
    { 209,  7680, 4320, 120000 },    { 210, 10240, 4320,  24000 },    { 211, 10240, 4320,  25000 },
    { 212, 10240, 4320,  30000 },    { 213, 10240, 4320,  48000 },    { 214, 10240, 4320,  50000 },
    { 215, 10240, 4320,  60000 },    { 216, 10240, 4320, 100000 },    { 217, 10240, 4320, 120000 },
    { 218,  4096, 2160, 100000 },    { 219,  4096, 2160, 120000 },
};

static gboolean
vic_matches_mode (unsigned int vic,
                  int          width,
                  int          height,
                  float        refresh_rate)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (cta_vics); i++)
    {
      double hz;

      if (cta_vics[i].vic != vic)
        continue;
      if (width != cta_vics[i].width || height != cta_vics[i].height)
        return FALSE;

      /* A VIC covers both its integer rate and the 1000/1001 one */
      hz = cta_vics[i].mhz / 1000.0;
      return fabs (refresh_rate - hz) < 0.5 ||
             fabs (refresh_rate - hz * 1000.0 / 1001.0) < 0.5 ||
             fabs (refresh_rate - hz * 1001.0 / 1000.0) < 0.5;
    }

  return FALSE;
}

static gboolean
valid_vic (unsigned int vic)
{
  return (vic >= 1 && vic <= 127) || (vic >= 193 && vic <= 219);
}

/* A collection of CTA-861 data blocks: from a CTA extension, or embedded in a
 * DisplayID CTA data block */
static void
decode_cta_color_blocks (const uchar      *p,
                         unsigned int      len,
                         EdidColorFormats *out)
{
  unsigned int pos = 0;

  while (pos < len)
    {
      unsigned int tag = p[pos] >> 5;
      unsigned int length = p[pos] & 0x1f;
      const uchar *data = p + pos + 1;
      unsigned int i;

      if (pos + 1 + length > len)
        break;

      if (tag == 2)
        {
          /* video data block: the SVDs of all of them are numbered together,
           * skipping invalid VICs */
          for (i = 0; i < length; i++)
            {
              unsigned int vic = data[i];

              if (vic >= 129 && vic <= 192)    /* native flag */
                vic &= 0x7f;
              if (valid_vic (vic) && out->n_svds < G_N_ELEMENTS (out->svds))
                out->svds[out->n_svds++] = vic;
            }
        }
      else if (tag == 7 && length >= 1 && data[0] == 0x0e)
        {
          /* 4:2:0 video data block: modes sent only as 4:2:0 */
          for (i = 1; i < length; i++)
            {
              if (out->n_y420_vdb < G_N_ELEMENTS (out->y420_vdb))
                out->y420_vdb[out->n_y420_vdb++] = data[i];
            }
        }
      else if (tag == 7 && length >= 1 && data[0] == 0x0f)
        {
          /* 4:2:0 capability map over the SVDs; an empty one means all */
          if (length == 1)
            out->y420_cmdb_map = G_MAXUINT64;
          for (i = 1; i < length && i <= 8; i++)
            out->y420_cmdb_map |= (guint64) data[i] << (8 * (i - 1));
        }

      pos += 1 + length;
    }
}

EdidColorFormats *
decode_edid_color_formats (const uchar *edid,
                           gsize        len)
{
  EdidColorFormats *out;
  gsize n_blocks, b;

  if (!edid || len < 128)
    return NULL;

  out = g_new0 (EdidColorFormats, 1);

  /* EDID 1.4 digital displays declare YCbCr 4:4:4 and 4:2:2 in the base
   * block */
  if ((edid[20] & 0x80) && edid[18] == 1 && edid[19] >= 4)
    {
      unsigned int encodings = (edid[24] >> 3) & 3;

      out->ycbcr444 |= (encodings & 1) != 0;
      out->ycbcr422 |= (encodings & 2) != 0;
    }

  n_blocks = MIN ((gsize) edid[126], len / 128 - 1);
  for (b = 1; b <= n_blocks; b++)
    {
      const uchar *x = edid + 128 * b;

      if (x[0] == 0x02)                         /* CTA-861 extension */
        {
          out->ycbcr444 |= (x[3] & 0x20) != 0;
          out->ycbcr422 |= (x[3] & 0x10) != 0;
          if (x[2] >= 4)
            decode_cta_color_blocks (x + 4, MIN (x[2], 127) - 4, out);
        }
      else if (x[0] == 0x70)                    /* DisplayID extension */
        {
          unsigned int end = MIN (5u + x[2], 127u);
          unsigned int pos = 5;

          while (pos + 3 <= end)
            {
              unsigned int tag = x[pos], length = x[pos + 2];

              if (pos + 3 + length > end)
                break;
              if (tag == 0x81)                  /* CTA DisplayID block */
                decode_cta_color_blocks (x + pos + 3, length, out);
              pos += 3 + length;
            }
        }
    }

  return out;
}

gboolean
edid_color_formats_allow_ycbcr420 (const EdidColorFormats *formats,
                                   int                     width,
                                   int                     height,
                                   float                   refresh_rate,
                                   gboolean                interlaced)
{
  unsigned int i;

  /* 4:2:0 is defined for progressive modes only */
  if (interlaced)
    return FALSE;

  for (i = 0; i < formats->n_y420_vdb; i++)
    {
      if (vic_matches_mode (formats->y420_vdb[i], width, height, refresh_rate))
        return TRUE;
    }

  for (i = 0; i < formats->n_svds && i < 64; i++)
    {
      if ((formats->y420_cmdb_map & ((guint64) 1 << i)) &&
          vic_matches_mode (formats->svds[i], width, height, refresh_rate))
        return TRUE;
    }

  return FALSE;
}

