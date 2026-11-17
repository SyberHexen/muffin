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

#ifndef META_WAYLAND_FIFO_H
#define META_WAYLAND_FIFO_H

#include <glib.h>

#include "wayland/meta-wayland-types.h"

void meta_wayland_init_fifo (MetaWaylandCompositor *compositor);

/* An update with set_barrier was applied; @had_damage: it changed what's
 * on screen, so the barrier waits for it to be drawn */
void meta_wayland_fifo_set_barrier (MetaWaylandSurface *surface,
                                    gboolean            had_damage);

/* After a redraw: clears the barriers whose latching deadline passed */
void meta_wayland_fifo_after_paint (MetaWaylandCompositor *compositor);

void meta_wayland_fifo_surface_destroyed (MetaWaylandSurface *surface);

#endif /* META_WAYLAND_FIFO_H */
