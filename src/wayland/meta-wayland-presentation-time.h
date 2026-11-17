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

#ifndef META_WAYLAND_PRESENTATION_TIME_H
#define META_WAYLAND_PRESENTATION_TIME_H

#include <wayland-server.h>

#include "clutter/clutter.h"
#include "wayland/meta-wayland-types.h"

void meta_wayland_init_presentation_time (MetaWaylandCompositor *compositor);

/* Discards (sends "discarded" for and frees) every feedback in @list */
void meta_wayland_presentation_time_discard_list (struct wl_list *list);

/* A commit of @surface was applied: @feedback_list holds the feedback
 * objects requested with it. Feedbacks of an earlier commit not drawn yet
 * are superseded. */
void meta_wayland_presentation_time_commit (MetaWaylandSurface *surface,
                                            struct wl_list     *feedback_list,
                                            gboolean            had_damage);

void meta_wayland_presentation_time_surface_destroyed (MetaWaylandSurface *surface);

/* After a redraw: the feedbacks of surfaces whose main view was drawn wait
 * for that view's frame */
void meta_wayland_presentation_time_after_paint (MetaWaylandCompositor *compositor);

/* A view's frame reached the screen */
void meta_wayland_presentation_time_present (MetaWaylandCompositor *compositor,
                                             ClutterFrameInfo      *frame_info);

#endif /* META_WAYLAND_PRESENTATION_TIME_H */
