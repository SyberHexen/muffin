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

typedef struct _MetaWaylandSyncobjSurface MetaWaylandSyncobjSurface;
typedef struct _MetaWaylandSyncPoint MetaWaylandSyncPoint;

void meta_wayland_init_linux_drm_syncobj (MetaWaylandCompositor *compositor);

/* At wl_surface.commit, before the state is applied or cached: checks the
 * surface's explicit sync points against the committed buffer, makes the
 * buffer wait for the acquire point and gives it the release point.
 * Returns FALSE after posting a protocol error. */
gboolean meta_wayland_linux_drm_syncobj_commit (MetaWaylandSurface *surface,
                                                MetaWaylandBuffer  *buffer,
                                                gboolean            newly_attached);

void meta_wayland_linux_drm_syncobj_surface_destroyed (MetaWaylandSurface *surface);

/* Signals a release point once the compositor's use of the buffer it was
 * committed with has finished (the buffer's fences), and frees it. */
void meta_wayland_sync_point_release (MetaWaylandSyncPoint *sync_point,
                                      MetaWaylandBuffer    *buffer);
