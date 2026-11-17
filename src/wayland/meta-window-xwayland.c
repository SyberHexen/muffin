/*
 * Copyright (C) 2017 Red Hat
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
 *
 */

#include "config.h"

#include <limits.h>
#include <math.h>
#include <X11/extensions/Xdamage.h>

#include "core/frame.h"
#include "core/window-private.h"
#include "meta/meta-x11-errors.h"
#include "x11/window-x11.h"
#include "x11/window-x11-private.h"
#include "x11/xprops.h"
#include "wayland/meta-window-xwayland.h"
#include "wayland/meta-wayland.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-xwayland.h"

enum
{
  PROP_0,

  PROP_XWAYLAND_MAY_GRAB_KEYBOARD,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

struct _MetaWindowXwayland
{
  MetaWindowX11 parent;

  gboolean xwayland_may_grab_keyboard;
  int freeze_count;

  /* Commits are frozen while the window is minimized, and after it's
   * unminimized until the client draws (restore_damage) or a timeout */
  gboolean frozen_for_minimize;
  Damage restore_damage;
  guint restore_timeout_id;
};

/* How long an unminimized window keeps showing its last frame at most,
 * waiting for its client to draw: Chromium-based clients take ~20 ms */
#define RESTORE_DRAW_TIMEOUT_MS 300

struct _MetaWindowXwaylandClass
{
  MetaWindowX11Class parent_class;
};

G_DEFINE_TYPE (MetaWindowXwayland, meta_window_xwayland, META_TYPE_WINDOW_X11)

static void meta_window_xwayland_freeze_commits (MetaWindow *window);
static void meta_window_xwayland_thaw_commits (MetaWindow *window);

static void
stop_waiting_for_restore_draw (MetaWindowXwayland *xwayland_window)
{
  MetaWindow *window = META_WINDOW (xwayland_window);

  g_clear_handle_id (&xwayland_window->restore_timeout_id, g_source_remove);

  if (xwayland_window->restore_damage)
    {
      MetaX11Display *x11_display = window->display->x11_display;

      if (x11_display)
        {
          meta_x11_error_trap_push (x11_display);
          XDamageDestroy (x11_display->xdisplay, xwayland_window->restore_damage);
          meta_x11_error_trap_pop (x11_display);
        }
      xwayland_window->restore_damage = None;
    }
}

static void
end_minimize_freeze (MetaWindowXwayland *xwayland_window)
{
  stop_waiting_for_restore_draw (xwayland_window);

  if (xwayland_window->frozen_for_minimize)
    {
      xwayland_window->frozen_for_minimize = FALSE;
      meta_window_xwayland_thaw_commits (META_WINDOW (xwayland_window));
    }
}

static gboolean
restore_draw_timeout (gpointer user_data)
{
  MetaWindowXwayland *xwayland_window = user_data;

  xwayland_window->restore_timeout_id = 0;
  end_minimize_freeze (xwayland_window);

  return G_SOURCE_REMOVE;
}

/* Watches for the client's first drawing after the window is unminimized,
 * which replaces what it drew while it was hidden */
static void
wait_for_restore_draw (MetaWindowXwayland *xwayland_window)
{
  MetaWindow *window = META_WINDOW (xwayland_window);
  MetaX11Display *x11_display = window->display->x11_display;

  if (x11_display && META_X11_DISPLAY_HAS_DAMAGE (x11_display) &&
      window->xwindow)
    {
      meta_x11_error_trap_push (x11_display);
      xwayland_window->restore_damage =
        XDamageCreate (x11_display->xdisplay, window->xwindow,
                       XDamageReportNonEmpty);
      if (meta_x11_error_trap_pop_with_return (x11_display) != Success)
        xwayland_window->restore_damage = None;
    }

  if (!xwayland_window->restore_damage)
    {
      end_minimize_freeze (xwayland_window);
      return;
    }

  xwayland_window->restore_timeout_id =
    g_timeout_add (RESTORE_DRAW_TIMEOUT_MS, restore_draw_timeout, xwayland_window);
  g_source_set_name_by_id (xwayland_window->restore_timeout_id,
                           "[muffin] restore_draw_timeout");
}

gboolean
meta_window_xwayland_handle_damage_notify (MetaX11Display *x11_display,
                                           XEvent         *xevent)
{
  XDamageNotifyEvent *event = (XDamageNotifyEvent *) xevent;
  MetaWindow *window;
  MetaWindowXwayland *xwayland_window;

  window = meta_x11_display_lookup_x_window (x11_display, event->drawable);
  if (!window || !META_IS_WINDOW_XWAYLAND (window))
    return FALSE;

  xwayland_window = META_WINDOW_XWAYLAND (window);
  if (xwayland_window->restore_damage == None ||
      xwayland_window->restore_damage != event->damage)
    return FALSE;

  end_minimize_freeze (xwayland_window);
  return TRUE;
}

/* A minimized window keeps the last frame it showed: its preview (window
 * list thumbnails, the minimize and unminimize animations) shows that
 * frame and not what the client draws while it knows it's hidden (some
 * clear their window then, Chromium-based ones paint it white), and it
 * doesn't commit frames nobody sees. Once unminimized, it keeps that frame
 * until the client draws again (Chromium-based clients take ~20 ms),
 * instead of showing what it drew while hidden. */
static void
on_minimized_changed (MetaWindow *window,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  MetaWindowXwayland *xwayland_window = META_WINDOW_XWAYLAND (window);

  if (window->minimized)
    {
      stop_waiting_for_restore_draw (xwayland_window);

      if (!xwayland_window->frozen_for_minimize)
        {
          xwayland_window->frozen_for_minimize = TRUE;
          meta_window_xwayland_freeze_commits (window);
        }
    }
  else if (xwayland_window->frozen_for_minimize &&
           !xwayland_window->restore_damage)
    {
      wait_for_restore_draw (xwayland_window);
    }
}

static void
meta_window_xwayland_dispose (GObject *object)
{
  stop_waiting_for_restore_draw (META_WINDOW_XWAYLAND (object));

  G_OBJECT_CLASS (meta_window_xwayland_parent_class)->dispose (object);
}

static void
meta_window_xwayland_init (MetaWindowXwayland *window_xwayland)
{
  g_signal_connect (window_xwayland, "notify::minimized",
                    G_CALLBACK (on_minimized_changed), NULL);
}

static int
scale_and_handle_overflow (int                  input,
                           float                scale,
                           MetaRoundingStrategy rounding_strategy)
{
  float value;

  switch (rounding_strategy)
    {
    case META_ROUNDING_STRATEGY_SHRINK:
      value = floorf (input * scale);
      break;
    case META_ROUNDING_STRATEGY_GROW:
      value = ceilf (input * scale);
      break;
    case META_ROUNDING_STRATEGY_ROUND:
      value = roundf (input * scale);
      break;
    default:
      g_return_val_if_reached (0);
    }

  if (value >= (float) INT_MAX)
    return INT_MAX;
  else if (value <= (float) INT_MIN)
    return INT_MIN;

  return (int) value;
}

/*
 * Xwayland's randr resolution emulation gives the surface a destination
 * viewport, so its buffer no longer maps to the window one-to-one and the
 * emulated ratio has to come back out of the scale.
 */
static float
get_viewport_scale_x (MetaWaylandSurface *surface)
{
  int buffer_width, buffer_height;

  meta_wayland_surface_get_buffer_size (surface, &buffer_width, &buffer_height);

  if (buffer_width <= 0 || surface->viewport.dst_width <= 0)
    return 1.0f;

  return (float) buffer_width / surface->viewport.dst_width;
}

static float
get_viewport_scale_y (MetaWaylandSurface *surface)
{
  int buffer_width, buffer_height;

  meta_wayland_surface_get_buffer_size (surface, &buffer_width, &buffer_height);

  if (buffer_height <= 0 || surface->viewport.dst_height <= 0)
    return 1.0f;

  return (float) buffer_height / surface->viewport.dst_height;
}

static void
meta_window_xwayland_stage_to_protocol_point (MetaWindow           *window,
                                              int                   stage_x,
                                              int                   stage_y,
                                              int                  *protocol_x,
                                              int                  *protocol_y,
                                              MetaRoundingStrategy  rounding_strategy)
{
  float scale = meta_xwayland_get_effective_scale ();

  if (protocol_x)
    *protocol_x = scale_and_handle_overflow (stage_x, scale, rounding_strategy);
  if (protocol_y)
    *protocol_y = scale_and_handle_overflow (stage_y, scale, rounding_strategy);
}

static void
meta_window_xwayland_stage_to_protocol_size (MetaWindow *window,
                                             int         stage_w,
                                             int         stage_h,
                                             int        *protocol_w,
                                             int        *protocol_h)
{
  MetaWaylandSurface *surface = window->surface;
  float scale_w, scale_h;

  scale_w = scale_h = meta_xwayland_get_effective_scale ();

  if (surface && surface->viewport.has_dst_size)
    {
      scale_w /= get_viewport_scale_x (surface);
      scale_h /= get_viewport_scale_y (surface);
    }

  if (protocol_w)
    *protocol_w = scale_and_handle_overflow (stage_w, scale_w,
                                             META_ROUNDING_STRATEGY_GROW);
  if (protocol_h)
    *protocol_h = scale_and_handle_overflow (stage_h, scale_h,
                                             META_ROUNDING_STRATEGY_GROW);
}

static void
meta_window_xwayland_protocol_to_stage_point (MetaWindow           *window,
                                              int                   protocol_x,
                                              int                   protocol_y,
                                              int                  *stage_x,
                                              int                  *stage_y,
                                              MetaRoundingStrategy  rounding_strategy)
{
  float scale = 1.0f / meta_xwayland_get_effective_scale ();

  if (stage_x)
    *stage_x = scale_and_handle_overflow (protocol_x, scale, rounding_strategy);
  if (stage_y)
    *stage_y = scale_and_handle_overflow (protocol_y, scale, rounding_strategy);
}

static void
meta_window_xwayland_protocol_to_stage_size (MetaWindow *window,
                                             int         protocol_w,
                                             int         protocol_h,
                                             int        *stage_w,
                                             int        *stage_h)
{
  MetaWaylandSurface *surface = window->surface;
  float scale_w, scale_h;

  scale_w = scale_h = 1.0f / meta_xwayland_get_effective_scale ();

  if (surface && surface->viewport.has_dst_size)
    {
      scale_w *= get_viewport_scale_x (surface);
      scale_h *= get_viewport_scale_y (surface);
    }

  if (stage_w)
    *stage_w = scale_and_handle_overflow (protocol_w, scale_w,
                                          META_ROUNDING_STRATEGY_GROW);
  if (stage_h)
    *stage_h = scale_and_handle_overflow (protocol_h, scale_h,
                                          META_ROUNDING_STRATEGY_GROW);
}

/**
 * meta_window_xwayland_adjust_fullscreen_monitor_rect:
 *
 * This function implements a workaround for X11 apps which use randr to change the
 * the monitor resolution, followed by setting _NET_WM_FULLSCREEN to make the
 * window-manager fullscreen them.
 *
 * Newer versions of Xwayland support the randr part of this by supporting randr
 * resolution change emulation in combination with using WPviewport to scale the
 * app's window (at the emulated resolution) to fill the entire monitor.
 *
 * Apps using randr in combination with NET_WM_STATE_FULLSCREEN expect the
 * fullscreen window to have the size of the emulated randr resolution since
 * when running on regular Xorg the resolution will actually be changed and
 * after that going fullscreen through NET_WM_STATE_FULLSCREEN will size
 * the window to be equal to the new resolution.
 *
 * We need to emulate this behavior for these apps to work correctly.
 *
 * Xwayland's emulated resolution is a per X11 client setting and Xwayland
 * will set a special _XWAYLAND_RANDR_EMU_MONITOR_RECTS property on the
 * toplevel windows of a client (and only those of that client), which has
 * changed the (emulated) resolution through a randr call.
 *
 * Here we check for that property and if it is set we adjust the fullscreen
 * monitor rect for this window to match the emulated resolution.
 *
 * Here is a step-by-step of such an app going fullscreen:
 * 1. App changes monitor resolution with randr.
 * 2. Xwayland sets the _XWAYLAND_RANDR_EMU_MONITOR_RECTS property on all the
 *    apps current and future windows. This property contains the origin of the
 *    monitor for which the emulated resolution is set and the emulated
 *    resolution.
 * 3. App sets _NET_WM_FULLSCREEN.
 * 4. We check the property and adjust the app's fullscreen size to match
 *    the emulated resolution.
 * 5. Xwayland sees a Window at monitor origin fully covering the emulated
 *    monitor resolution. Xwayland sets a viewport making the emulated
 *    resolution sized window cover the full actual monitor resolution.
 */
static void
meta_window_xwayland_adjust_fullscreen_monitor_rect (MetaWindow    *window,
                                                     MetaRectangle *fs_monitor_rect)
{
  MetaX11Display *x11_display = window->display->x11_display;
  MetaRectangle win_monitor_rect;
  cairo_rectangle_int_t *rects;
  uint32_t *list = NULL;
  int i, n_items = 0;

  if (!window->monitor)
    {
      g_warning ("MetaWindow does not have a monitor");
      return;
    }

  win_monitor_rect = meta_logical_monitor_get_layout (window->monitor);

  if (!meta_prop_get_cardinal_list (x11_display,
                                    window->xwindow,
                                    x11_display->atom__XWAYLAND_RANDR_EMU_MONITOR_RECTS,
                                    &list, &n_items))
    return;

  if (n_items % 4)
    {
      meta_verbose ("_XWAYLAND_RANDR_EMU_MONITOR_RECTS on %s has %d values which is not a multiple of 4",
                    window->desc, n_items);
      g_free (list);
      return;
    }

  rects = (cairo_rectangle_int_t *) list;
  n_items = n_items / 4;
  for (i = 0; i < n_items; i++)
    {
      if (rects[i].x == win_monitor_rect.x && rects[i].y == win_monitor_rect.y)
        {
          fs_monitor_rect->width = rects[i].width;
          fs_monitor_rect->height = rects[i].height;
          break;
        }
    }

  g_free (list);
}

static void
meta_window_xwayland_force_restore_shortcuts (MetaWindow         *window,
                                              ClutterInputDevice *source)
{
  MetaWaylandCompositor *compositor = meta_wayland_compositor_get_default ();

  meta_wayland_compositor_restore_shortcuts (compositor, source);
}

static gboolean
meta_window_xwayland_shortcuts_inhibited (MetaWindow         *window,
                                          ClutterInputDevice *source)
{
  MetaWaylandCompositor *compositor = meta_wayland_compositor_get_default ();

  return meta_wayland_compositor_is_shortcuts_inhibited (compositor, source);
}

static void
apply_allow_commits_x11_property (MetaWindowXwayland *xwayland_window,
                                  gboolean            allow_commits)
{
  MetaWindow *window = META_WINDOW (xwayland_window);
  MetaDisplay *display = window->display;
  MetaX11Display *x11_display = display->x11_display;
  Display *xdisplay = x11_display->xdisplay;
  MetaFrame *frame;
  Window xwin;
  guint32 property[1];

  frame = meta_window_get_frame (window);
  if (!frame)
    xwin = window->xwindow;
  else
    xwin = meta_frame_get_xwindow (frame);

  if (!xwin)
    return;

  property[0] = !!allow_commits;

  meta_x11_error_trap_push (x11_display);
  XChangeProperty (xdisplay, xwin,
                   x11_display->atom__XWAYLAND_ALLOW_COMMITS,
                   XA_CARDINAL, 32, PropModeReplace,
                   (guchar*) &property, 1);
  meta_x11_error_trap_pop (x11_display);
  XFlush (xdisplay);
}

static void
meta_window_xwayland_freeze_commits (MetaWindow *window)
{
  MetaWindowXwayland *xwayland_window = META_WINDOW_XWAYLAND (window);

  if (xwayland_window->freeze_count == 0)
    apply_allow_commits_x11_property (xwayland_window, FALSE);

  xwayland_window->freeze_count++;
}

static void
meta_window_xwayland_thaw_commits (MetaWindow *window)
{
  MetaWindowXwayland *xwayland_window = META_WINDOW_XWAYLAND (window);

  g_return_if_fail (xwayland_window->freeze_count > 0);

  xwayland_window->freeze_count--;
  if (xwayland_window->freeze_count > 0)
    return;

  apply_allow_commits_x11_property (xwayland_window, TRUE);
}

static gboolean
meta_window_xwayland_always_update_shape (MetaWindow *window)
{
  /*
   * On Xwayland, resizing a window will clear the corresponding Wayland
   * buffer to plain solid black.
   *
   * Therefore, to address the black shadows which sometimes show during
   * resize with Xwayland, we need to always update the window shape
   * regardless of the actual frozen state of the window actor.
   */

  return TRUE;
}

static void
meta_window_xwayland_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  MetaWindowXwayland *window = META_WINDOW_XWAYLAND (object);

  switch (prop_id)
    {
    case PROP_XWAYLAND_MAY_GRAB_KEYBOARD:
      g_value_set_boolean (value, window->xwayland_may_grab_keyboard);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_window_xwayland_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  MetaWindowXwayland *window = META_WINDOW_XWAYLAND (object);

  switch (prop_id)
    {
    case PROP_XWAYLAND_MAY_GRAB_KEYBOARD:
      window->xwayland_may_grab_keyboard = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_window_xwayland_class_init (MetaWindowXwaylandClass *klass)
{
  MetaWindowClass *window_class = META_WINDOW_CLASS (klass);
  MetaWindowX11Class *window_x11_class = META_WINDOW_X11_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  window_class->adjust_fullscreen_monitor_rect = meta_window_xwayland_adjust_fullscreen_monitor_rect;
  window_class->stage_to_protocol_point = meta_window_xwayland_stage_to_protocol_point;
  window_class->stage_to_protocol_size = meta_window_xwayland_stage_to_protocol_size;
  window_class->protocol_to_stage_point = meta_window_xwayland_protocol_to_stage_point;
  window_class->protocol_to_stage_size = meta_window_xwayland_protocol_to_stage_size;
  window_class->force_restore_shortcuts = meta_window_xwayland_force_restore_shortcuts;
  window_class->shortcuts_inhibited = meta_window_xwayland_shortcuts_inhibited;

  window_x11_class->freeze_commits = meta_window_xwayland_freeze_commits;
  window_x11_class->thaw_commits = meta_window_xwayland_thaw_commits;
  window_x11_class->always_update_shape = meta_window_xwayland_always_update_shape;

  gobject_class->get_property = meta_window_xwayland_get_property;
  gobject_class->set_property = meta_window_xwayland_set_property;
  gobject_class->dispose = meta_window_xwayland_dispose;

  obj_props[PROP_XWAYLAND_MAY_GRAB_KEYBOARD] =
    g_param_spec_boolean ("xwayland-may-grab-keyboard",
                          "Xwayland may use keyboard grabs",
                          "Whether the client may use Xwayland keyboard grabs on this window",
                          FALSE,
                          G_PARAM_READWRITE);

  g_object_class_install_properties (gobject_class, PROP_LAST, obj_props);
}
