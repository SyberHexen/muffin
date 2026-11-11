/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2013 Red Hat Inc.
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

/* This file is shared between muffin (src/core/meta-display-config-shared.h)
   and gnome-desktop (libgnome-desktop/meta-xrandr-shared.h).

   The canonical place for all changes is muffin.

   There should be no includes in this file.
*/

#ifndef META_DISPLAY_CONFIG_SHARED_H
#define META_DISPLAY_CONFIG_SHARED_H

typedef enum
{
  META_POWER_SAVE_UNSUPPORTED = -1,
  META_POWER_SAVE_ON = 0,
  META_POWER_SAVE_STANDBY,
  META_POWER_SAVE_SUSPEND,
  META_POWER_SAVE_OFF,
} MetaPowerSave;

/*
 * The quantization range of RGB output ("Broadcast RGB" in KMS).  The values
 * are the ones of the "rgb-range" property of the DisplayConfig interface.
 */
typedef enum
{
  META_OUTPUT_RGB_RANGE_UNKNOWN = 0,
  META_OUTPUT_RGB_RANGE_AUTO = 1,
  META_OUTPUT_RGB_RANGE_FULL = 2,
  META_OUTPUT_RGB_RANGE_LIMITED = 3,
} MetaOutputRGBRange;

#define META_OUTPUT_RGB_RANGE_LAST META_OUTPUT_RGB_RANGE_LIMITED

/*
 * The pixel encoding sent over the link ("color format" in KMS).  The values
 * are the ones of the "color-format" property of the DisplayConfig interface.
 */
typedef enum
{
  META_OUTPUT_COLOR_FORMAT_UNKNOWN = 0,
  META_OUTPUT_COLOR_FORMAT_AUTO = 1,
  META_OUTPUT_COLOR_FORMAT_RGB = 2,
  META_OUTPUT_COLOR_FORMAT_YCBCR444 = 3,
  META_OUTPUT_COLOR_FORMAT_YCBCR422 = 4,
  META_OUTPUT_COLOR_FORMAT_YCBCR420 = 5,
} MetaOutputColorFormat;

#define META_OUTPUT_COLOR_FORMAT_LAST META_OUTPUT_COLOR_FORMAT_YCBCR420

/*
 * The signal sent to the monitor.  BT2100 is HDR10: BT.2020 primaries and
 * the PQ (SMPTE ST 2084) transfer function, with HDR static metadata.  The
 * values are the ones of the "color-mode" property of the DisplayConfig
 * interface, as in Mutter.
 */
typedef enum
{
  META_OUTPUT_COLOR_MODE_DEFAULT = 0,
  META_OUTPUT_COLOR_MODE_BT2100 = 1,
} MetaOutputColorMode;

#define META_OUTPUT_COLOR_MODE_LAST META_OUTPUT_COLOR_MODE_BT2100

#endif /* META_DISPLAY_CONFIG_SHARED_H */
