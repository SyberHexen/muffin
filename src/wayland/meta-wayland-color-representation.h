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

#pragma once

#include <glib.h>

#include "wayland/meta-wayland-types.h"

void meta_wayland_init_color_representation (MetaWaylandCompositor *compositor);

/* At wl_surface.commit, before the buffer is attached: applies the
 * committed coefficients and range, and hands them to a YUV buffer's
 * conversion. Returns FALSE after posting a protocol error (coefficients
 * set for an RGB buffer). */
gboolean meta_wayland_color_representation_commit (MetaWaylandSurface      *surface,
                                                   MetaWaylandSurfaceState *state);

void meta_wayland_color_representation_surface_destroyed (MetaWaylandSurface *surface);
