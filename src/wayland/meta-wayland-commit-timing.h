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

#ifndef META_WAYLAND_COMMIT_TIMING_H
#define META_WAYLAND_COMMIT_TIMING_H

#include <glib.h>

#include "wayland/meta-wayland-types.h"

void meta_wayland_init_commit_timing (MetaWaylandCompositor *compositor);

/* Whether an update meant to be shown at @target_us (monotonic clock) can
 * be applied now; if not, @wake_up_us is lowered to when to check again */
gboolean meta_wayland_commit_timing_is_ready (MetaWaylandSurface *surface,
                                              int64_t             target_us,
                                              int64_t             now_us,
                                              int64_t            *wake_up_us);

void meta_wayland_commit_timing_surface_destroyed (MetaWaylandSurface *surface);

#endif /* META_WAYLAND_COMMIT_TIMING_H */
