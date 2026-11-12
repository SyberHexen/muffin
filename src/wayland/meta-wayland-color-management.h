/*
 * Wayland Support
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

#ifndef META_WAYLAND_COLOR_MANAGEMENT_H
#define META_WAYLAND_COLOR_MANAGEMENT_H

#include <glib.h>
#include <stdint.h>

#include "wayland/meta-wayland-types.h"

void meta_wayland_init_color_management (MetaWaylandCompositor *compositor);

MetaWaylandColorDescription *
meta_wayland_color_description_ref (MetaWaylandColorDescription *description);

void meta_wayland_color_description_unref (MetaWaylandColorDescription *description);

/* PQ transfer function with BT.2020 primaries, which HDR10 carries as is */
gboolean meta_wayland_color_description_is_hdr10 (MetaWaylandColorDescription *description);

/* Re-evaluate the preferred image description after the surface moved */
void meta_wayland_color_management_update_surface (MetaWaylandSurface *surface);

/* Switch outputs to the BT.2100 color mode while a fullscreen window on them
 * shows HDR10 content, and back afterwards. @fullscreen_windows maps each
 * logical monitor in fullscreen to its window. */
void meta_wayland_color_management_update_fullscreen (GHashTable *fullscreen_windows);

/* Drop the color management objects of a surface being destroyed */
void meta_wayland_color_management_surface_destroyed (MetaWaylandSurface *surface);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MetaWaylandColorDescription,
                               meta_wayland_color_description_unref)

#endif /* META_WAYLAND_COLOR_MANAGEMENT_H */
