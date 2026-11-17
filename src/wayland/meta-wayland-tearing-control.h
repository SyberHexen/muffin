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

#ifndef META_WAYLAND_TEARING_CONTROL_H
#define META_WAYLAND_TEARING_CONTROL_H

#include <glib.h>

#include "wayland/meta-wayland-types.h"

void meta_wayland_init_tearing_control (MetaWaylandCompositor *compositor);

/* Whether @surface asks for its frames to be shown at once (it may tear) */
gboolean meta_wayland_surface_wants_tearing (MetaWaylandSurface *surface);

void meta_wayland_tearing_control_surface_destroyed (MetaWaylandSurface *surface);

#endif /* META_WAYLAND_TEARING_CONTROL_H */
