/**************************************************************************
* Copyright (C) 2017 tint2 authors
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License version 2
* as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**************************************************************************/

#ifndef SHORTCUTS_H
#define SHORTCUTS_H

#include <X11/Xlib.h>
#include <glib.h>

// When enabled, tint2 grabs a small set of global keyboard shortcuts:
//   Super+1 .. Super+9, Super+0   switch to desktop 1..10
//   Super+Tab                     switch to the most recently used desktop
//   Super+n                       activate the next task on the current desktop
//   Super+Shift+n                 activate the previous task on the current desktop
// Tasks are cycled in the order they are shown in the panel.
extern gboolean keyboard_shortcuts;

// Grabs the shortcut keys on the root window. No-op when keyboard_shortcuts is off.
void shortcuts_init();

// Releases the grabbed keys.
void shortcuts_cleanup();

// Handles a KeyPress event delivered for a grabbed shortcut.
// Returns TRUE if the event matched a shortcut and was acted upon.
gboolean shortcuts_handle_keypress(XKeyEvent *e);

// Records the desktop the user just left so Super+Tab can return to it.
void shortcuts_set_previous_desktop(int desktop);

#endif // SHORTCUTS_H
