/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Metacity X window decorations */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2003, 2004 Red Hat, Inc.
 * Copyright (C) 2005 Elijah Newren
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

#include <config.h>
#include "frame-private.h"
#include "bell.h"
#include "errors.h"
#include "keybindings.h"
#include "prefs.h"

#define EVENT_MASK (SubstructureRedirectMask |                     \
                    StructureNotifyMask | SubstructureNotifyMask | \
                    ExposureMask |                                 \
                    FocusChangeMask |                              \
                    ColormapChangeMask)

static gboolean update_shape (MetaFrame *frame);

static void
prefs_changed_callback (MetaPreference preference,
                        gpointer       data)
{
  MetaFrame *frame = (MetaFrame *) data;

  switch (preference)
    {
      case META_PREF_COMPOSITING_MANAGER:
        frame->need_reapply_frame_shape = TRUE;
        update_shape (frame);
        break;

      case META_PREF_MOUSE_BUTTON_MODS:
      case META_PREF_FOCUS_MODE:
      case META_PREF_FOCUS_NEW_WINDOWS:
      case META_PREF_ATTACH_MODAL_DIALOGS:
      case META_PREF_RAISE_ON_CLICK:
      case META_PREF_ACTION_DOUBLE_CLICK_TITLEBAR:
      case META_PREF_ACTION_MIDDLE_CLICK_TITLEBAR:
      case META_PREF_ACTION_RIGHT_CLICK_TITLEBAR:
      case META_PREF_AUTO_RAISE:
      case META_PREF_AUTO_RAISE_DELAY:
      case META_PREF_THEME:
      case META_PREF_TITLEBAR_FONT:
      case META_PREF_NUM_WORKSPACES:
      case META_PREF_KEYBINDINGS:
      case META_PREF_DISABLE_WORKAROUNDS:
      case META_PREF_BUTTON_LAYOUT:
      case META_PREF_WORKSPACE_NAMES:
      case META_PREF_VISUAL_BELL:
      case META_PREF_AUDIBLE_BELL:
      case META_PREF_VISUAL_BELL_TYPE:
      case META_PREF_REDUCED_RESOURCES:
      case META_PREF_GNOME_ACCESSIBILITY:
      case META_PREF_GNOME_ANIMATIONS:
      case META_PREF_CURSOR_THEME:
      case META_PREF_CURSOR_SIZE:
      case META_PREF_RESIZE_WITH_RIGHT_BUTTON:
      case META_PREF_EDGE_TILING:
      case META_PREF_FORCE_FULLSCREEN:
      case META_PREF_PLACEMENT_MODE:
      case META_PREF_ALT_TAB_THUMBNAILS:
        break;

      default:
        break;
    }
}

void
meta_window_ensure_frame (MetaWindow *window)
{
  MetaFrame *frame;
  MetaScreen *screen;
  XSetWindowAttributes attrs;
  XVisualInfo visual_info;
  Visual *visual;
  int status;

  if (window->frame)
    return;

  /* See comment below for why this is required. */
  meta_display_grab (window->display);

  frame = g_new (MetaFrame, 1);

  frame->window = window;
  frame->xwindow = None;

  frame->rect = window->rect;
  frame->child_x = 0;
  frame->child_y = 0;
  frame->bottom_height = 0;
  frame->right_width = 0;
  frame->current_cursor = 0;

  frame->mapped = TRUE;
  frame->need_reapply_frame_shape = TRUE;
  frame->is_flashing = FALSE;

  meta_verbose ("Framing window %s: visual %s default, depth %d default depth %d\n",
                window->desc,
                XVisualIDFromVisual (window->xvisual) ==
                XVisualIDFromVisual (window->screen->default_xvisual) ?
                "is" : "is not",
                window->depth, window->screen->default_depth);
  meta_verbose ("Frame geometry %d,%d  %dx%d\n",
                frame->rect.x, frame->rect.y,
                frame->rect.width, frame->rect.height);

  /* Default depth/visual handles clients with weird visuals; they can
   * always be children of the root depth/visual obviously, but
   * e.g. DRI games can't be children of a parent that has the same
   * visual as the client. NULL means default visual.
   *
   * We look for an ARGB visual if we can find one, otherwise use
   * the default of NULL.
   */

  screen = meta_window_get_screen (window);
  status = XMatchVisualInfo (window->display->xdisplay,
                             XScreenNumberOfScreen (screen->xscreen),
                             32, TrueColor,
                             &visual_info);

  if (!status)
    {
      /* Special case for depth 32 windows (assumed to be ARGB),
       * we use the window's visual. Otherwise we just use the system visual.
       */
      if (window->depth == 32)
        visual = window->xvisual;
      else
        visual = NULL;
    }
  else
    visual = visual_info.visual;

  frame->xwindow = meta_ui_create_frame_window (window->screen->ui,
                                                window->display->xdisplay,
                                                visual,
                                                frame->rect.x,
                                                frame->rect.y,
						frame->rect.width,
						frame->rect.height,
						frame->window->screen->number);

  meta_verbose ("Frame for %s is 0x%lx\n", frame->window->desc, frame->xwindow);
  attrs.event_mask = EVENT_MASK;
  XChangeWindowAttributes (window->display->xdisplay,
			   frame->xwindow, CWEventMask, &attrs);

  meta_display_register_x_window (window->display, &frame->xwindow, window);

  /* Reparent the client window; it may be destroyed,
   * thus the error trap. We'll get a destroy notify later
   * and free everything. Comment in FVWM source code says
   * we need a server grab or the child can get its MapNotify
   * before we've finished reparenting and getting the decoration
   * window onscreen, so ensure_frame must be called with
   * a grab.
   */
  meta_error_trap_push (window->display);
  if (window->mapped)
    {
      window->mapped = FALSE; /* the reparent will unmap the window,
                               * we don't want to take that as a withdraw
                               */
      meta_topic (META_DEBUG_WINDOW_STATE,
                  "Incrementing unmaps_pending on %s for reparent\n", window->desc);
      window->unmaps_pending += 1;
    }
  /* window was reparented to this position */
  window->rect.x = 0;
  window->rect.y = 0;

  XReparentWindow (window->display->xdisplay,
                   window->xwindow,
                   frame->xwindow,
                   window->rect.x,
                   window->rect.y);
  /* FIXME handle this error */
  meta_error_trap_pop (window->display, FALSE);

  /* stick frame to the window */
  window->frame = frame;

  meta_ui_map_frame (window->screen->ui, window->frame->xwindow);

  /* Now that frame->xwindow is registered with window, we can set its
   * style and background.
   */
  meta_ui_update_frame_style (window->screen->ui, frame->xwindow);

  if (window->title)
    meta_ui_set_frame_title (window->screen->ui,
                             window->frame->xwindow,
                             window->title);

  /* Move keybindings to frame instead of window */
  meta_window_grab_keys (window);

  /* Shape mask */
  meta_ui_apply_frame_shape (frame->window->screen->ui,
                             frame->xwindow,
                             frame->rect.width,
                             frame->rect.height,
                             frame->window->has_shape);
  frame->need_reapply_frame_shape = FALSE;

  meta_display_ungrab (window->display);

  {
      Display* xdisplay = window->display->xdisplay;

      unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
      XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };

      XISetMask (mask.mask, XI_ButtonPress);
      XISetMask (mask.mask, XI_ButtonRelease);
      XISetMask (mask.mask, XI_Motion);
      XISetMask (mask.mask, XI_Enter);
      XISetMask (mask.mask, XI_Leave);

      XISelectEvents (xdisplay, frame->xwindow, &mask, 1);
  }

  meta_prefs_add_listener (prefs_changed_callback, frame);
}

void
meta_window_destroy_frame (MetaWindow *window)
{
  MetaFrame *frame;
  MetaFrameBorders borders;

  if (window->frame == NULL)
    return;

  meta_verbose ("Unframing window %s\n", window->desc);

  frame = window->frame;

  meta_prefs_remove_listener (prefs_changed_callback, frame);

  meta_frame_calc_borders (frame, &borders);

  meta_bell_notify_frame_destroy (frame);

  /* Unparent the client window; it may be destroyed,
   * thus the error trap.
   */
  meta_error_trap_push (window->display);
  if (window->mapped)
    {
      window->mapped = FALSE; /* Keep track of unmapping it, so we
                               * can identify a withdraw initiated
                               * by the client.
                               */
      meta_topic (META_DEBUG_WINDOW_STATE,
                  "Incrementing unmaps_pending on %s for reparent back to root\n", window->desc);
      window->unmaps_pending += 1;
    }
  XReparentWindow (window->display->xdisplay,
                   window->xwindow,
                   window->screen->xroot,
                   /* Using anything other than meta_window_get_position()
                    * coordinates here means we'll need to ensure a configure
                    * notify event is sent; see bug 399552.
                    */
                   window->frame->rect.x + borders.invisible.left,
                   window->frame->rect.y + borders.invisible.top);
  meta_error_trap_pop (window->display, FALSE);

  meta_ui_destroy_frame_window (window->screen->ui, frame->xwindow);

  meta_display_unregister_x_window (window->display,
                                    frame->xwindow);

  window->frame = NULL;
  if (window->frame_bounds)
    {
      cairo_region_destroy (window->frame_bounds);
      window->frame_bounds = NULL;
    }

  /* Move keybindings to window instead of frame */
  meta_window_grab_keys (window);

  g_free (frame);

  /* Put our state back where it should be */
  meta_window_queue (window, META_QUEUE_CALC_SHOWING);
  meta_window_queue (window, META_QUEUE_MOVE_RESIZE);
}


MetaFrameFlags
meta_frame_get_flags (MetaFrame *frame)
{
  MetaFrameFlags flags;

  flags = 0;

  if (frame->window->border_only)
    {
      ; /* FIXME this may disable the _function_ as well as decor
         * in some cases, which is sort of wrong.
         */
    }
  else
    {
      flags |= META_FRAME_ALLOWS_MENU;

      if (frame->window->has_close_func)
        flags |= META_FRAME_ALLOWS_DELETE;

      if (frame->window->has_maximize_func)
        flags |= META_FRAME_ALLOWS_MAXIMIZE;

      if (frame->window->has_minimize_func)
        flags |= META_FRAME_ALLOWS_MINIMIZE;

      if (frame->window->has_shade_func)
        flags |= META_FRAME_ALLOWS_SHADE;
    }

  if (META_WINDOW_ALLOWS_MOVE (frame->window))
    flags |= META_FRAME_ALLOWS_MOVE;

  if (META_WINDOW_ALLOWS_HORIZONTAL_RESIZE (frame->window))
    flags |= META_FRAME_ALLOWS_HORIZONTAL_RESIZE;

  if (META_WINDOW_ALLOWS_VERTICAL_RESIZE (frame->window))
    flags |= META_FRAME_ALLOWS_VERTICAL_RESIZE;

  if (meta_window_appears_focused (frame->window))
    flags |= META_FRAME_HAS_FOCUS;

  if (frame->window->shaded)
    flags |= META_FRAME_SHADED;

  if (frame->window->on_all_workspaces)
    flags |= META_FRAME_STUCK;

  /* FIXME: Should we have some kind of UI for windows that are just vertically
   * maximized or just horizontally maximized?
   */
  if (META_WINDOW_MAXIMIZED (frame->window))
    flags |= META_FRAME_MAXIMIZED;

  if (META_WINDOW_TILED_LEFT (frame->window))
    flags |= META_FRAME_TILED_LEFT;

  if (META_WINDOW_TILED_RIGHT (frame->window))
    flags |= META_FRAME_TILED_RIGHT;

  if (frame->window->fullscreen)
    flags |= META_FRAME_FULLSCREEN;

  if (frame->is_flashing)
    flags |= META_FRAME_IS_FLASHING;

  if (frame->window->wm_state_above)
    flags |= META_FRAME_ABOVE;

  return flags;
}

void
meta_frame_borders_clear (MetaFrameBorders *self)
{
  self->visible.top = self->invisible.top = self->total.top = 0;
  self->visible.bottom = self->invisible.bottom = self->total.bottom = 0;
  self->visible.left = self->invisible.left = self->total.left = 0;
  self->visible.right = self->invisible.right = self->total.right = 0;
}

void
meta_frame_calc_borders (MetaFrame        *frame,
                         MetaFrameBorders *borders)
{
  meta_ui_get_frame_borders (frame->window->screen->ui,
                             frame->xwindow,
                             borders);
}

static gboolean
update_shape (MetaFrame *frame)
{
  if (frame->need_reapply_frame_shape)
    {
      meta_ui_apply_frame_shape (frame->window->screen->ui,
                                 frame->xwindow,
                                 frame->rect.width,
                                 frame->rect.height,
                                 frame->window->has_shape);
      frame->need_reapply_frame_shape = FALSE;

      return TRUE;
    }
  else
    return FALSE;
}

gboolean
meta_frame_sync_to_window (MetaFrame *frame,
                           int        resize_gravity,
                           gboolean   need_move,
                           gboolean   need_resize)
{
  if (!(need_move || need_resize))
    {
      return update_shape (frame);
    }

  meta_topic (META_DEBUG_GEOMETRY,
              "Syncing frame geometry %d,%d %dx%d (SE: %d,%d)\n",
              frame->rect.x, frame->rect.y,
              frame->rect.width, frame->rect.height,
              frame->rect.x + frame->rect.width,
              frame->rect.y + frame->rect.height);

  /* set bg to none to avoid flicker */
  if (need_resize)
    {
      /* we need new shape if we're resized */
      frame->need_reapply_frame_shape = TRUE;
    }

  /* Done before the window resize, because doing it before means
   * part of the window being resized becomes unshaped, which may
   * be sort of hard to see with bg = None. If we did it after
   * window resize, part of the window being resized would become
   * shaped, which might be more visible.
   */
  update_shape (frame);

  /* this may fail because of extra large size change, I dont have
   * a better solution now 
   * */
  meta_error_trap_push (frame->window->display);
  meta_ui_move_resize_frame (frame->window->screen->ui,
			     frame->xwindow,
			     frame->rect.x,
			     frame->rect.y,
			     frame->rect.width,
			     frame->rect.height);

  meta_error_trap_pop (frame->window->display, FALSE);

  if (need_resize)
    {
      /* If we're interactively resizing the frame, repaint
       * it immediately so we don't start to lag.
       */
      if (frame->window->display->grab_window ==
          frame->window)
        meta_ui_repaint_frame (frame->window->screen->ui,
                               frame->xwindow);
    }

  return need_resize;
}

cairo_region_t *
meta_frame_get_frame_bounds (MetaFrame *frame)
{
  return meta_ui_get_frame_bounds (frame->window->screen->ui,
                                   frame->xwindow,
                                   frame->rect.width,
                                   frame->rect.height);
}

void
meta_frame_get_mask (MetaFrame *frame,
                     cairo_t   *cr)
{
  meta_ui_get_frame_mask (frame->window->screen->ui, frame->xwindow,
                          frame->rect.width, frame->rect.height, cr);
}

void
meta_frame_queue_draw (MetaFrame *frame)
{
  meta_ui_queue_frame_draw (frame->window->screen->ui,
                            frame->xwindow);
}

void
meta_frame_set_screen_cursor (MetaFrame	*frame,
			      MetaCursor cursor)
{
  Cursor xcursor;
  if (cursor == frame->current_cursor)
    return;
  frame->current_cursor = cursor;
  if (cursor == META_CURSOR_DEFAULT)
    XUndefineCursor (frame->window->display->xdisplay, frame->xwindow);
  else
    {
      xcursor = meta_display_create_x_cursor (frame->window->display, cursor);
      XDefineCursor (frame->window->display->xdisplay, frame->xwindow, xcursor);
      XFlush (frame->window->display->xdisplay);
      XFreeCursor (frame->window->display->xdisplay, xcursor);
    }
}

Window
meta_frame_get_xwindow (MetaFrame *frame)
{
  return frame->xwindow;
}
