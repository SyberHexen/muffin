/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2017 Red Hat
 * Copyright (C) 2018 DisplayLink (UK) Ltd.
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
 */

#ifndef META_OUTPUT_KMS_H
#define META_OUTPUT_KMS_H

#include "backends/meta-output.h"
#include "backends/native/meta-gpu-kms.h"
#include "backends/native/meta-kms-types.h"

void meta_output_kms_set_power_save_mode (MetaOutput    *output,
                                          uint64_t       dpms_state,
                                          MetaKmsUpdate *kms_update);

void meta_output_kms_set_color_properties (MetaOutput    *output,
                                           MetaKmsUpdate *kms_update);

void meta_output_kms_set_underscan (MetaOutput    *output,
                                    MetaKmsUpdate *kms_update);

gboolean meta_output_kms_can_clone (MetaOutput *output,
                                    MetaOutput *other_output);

MetaKmsConnector * meta_output_kms_get_kms_connector (MetaOutput *output);

gboolean meta_output_kms_get_gamma (MetaOutput  *output,
                                    int         *size_out,
                                    uint16_t   **red_out,
                                    uint16_t   **green_out,
                                    uint16_t   **blue_out);

/* MUFFIN_SDR_EDID_COLORS=1: SDR colors shown in their own (BT.709) colors
 * on a wide gamut display */
gboolean meta_output_kms_wants_sdr_colors_mapped (MetaOutput *output);

gboolean meta_output_kms_maps_sdr_colors_in_hardware (MetaOutput *output);

uint32_t meta_output_kms_get_connector_id (MetaOutput *output);

GBytes * meta_output_kms_read_edid (MetaOutput *output);

MetaOutput * meta_create_kms_output (MetaGpuKms        *gpu_kms,
                                     MetaKmsConnector  *kms_connector,
                                     MetaOutput        *old_output,
                                     GError           **error);

#endif /* META_OUTPUT_KMS_H */
