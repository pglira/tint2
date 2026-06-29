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

#include <stdint.h>
#include <stdio.h>
#include <X11/keysym.h>

#include "shortcuts.h"
#include "panel.h"
#include "server.h"
#include "task.h"
#include "taskbar.h"
#include "window.h"

gboolean keyboard_shortcuts = TRUE;

// Number of digit keys mapped to desktops: Super+1..9 and Super+0.
#define NUM_DESKTOP_KEYS 10

// Digit row keysyms in desktop order; the trailing 0 selects the tenth desktop.
static const KeySym desktop_keysyms[NUM_DESKTOP_KEYS] = {
    XK_1, XK_2, XK_3, XK_4, XK_5, XK_6, XK_7, XK_8, XK_9, XK_0
};

// Modifier mask carrying the Super (a.k.a. Meta/Win) key.
static unsigned int super_mask = Mod4Mask;
// Lock modifiers ignored when matching shortcuts (CapsLock is always LockMask).
static unsigned int numlock_mask;
static unsigned int scrolllock_mask;

// Desktop shown before the current one; -1 until a desktop change is observed.
static int previous_desktop = -1;

// Returns the modifier mask (Mod1Mask..Mod5Mask, ShiftMask, ...) that the given
// keysym is bound to, or 0 if the keysym is not a modifier.
static unsigned int modifier_mask_for_keysym(KeySym keysym)
{
    unsigned int mask = 0;
    KeyCode target = XKeysymToKeycode(server.display, keysym);
    if (!target)
        return 0;

    XModifierKeymap *modmap = XGetModifierMapping(server.display);
    if (!modmap)
        return 0;
    for (int mod = 0; mod < 8; mod++)
        for (int k = 0; k < modmap->max_keypermod; k++)
            if (modmap->modifiermap[mod * modmap->max_keypermod + k] == target)
                mask |= (1 << mod);
    XFreeModifiermap(modmap);
    return mask;
}

// Counts BadAccess errors seen while a grab error handler is installed; a grab
// fails with BadAccess when another client already owns the key combination.
static int num_grab_conflicts;

static int grab_error_handler(Display *display, XErrorEvent *e)
{
    if (e->error_code == BadAccess)
        num_grab_conflicts++;
    else
        server_catch_error(display, e);
    return 0;
}

// Grabs a key with every combination of the lock modifiers, so the shortcut
// fires regardless of CapsLock/NumLock/ScrollLock state.
static void grab_key(KeySym keysym, unsigned int modifiers)
{
    KeyCode keycode = XKeysymToKeycode(server.display, keysym);
    if (!keycode)
        return;

    unsigned int locks[3];
    int num_locks = 0;
    locks[num_locks++] = LockMask;
    if (numlock_mask)
        locks[num_locks++] = numlock_mask;
    if (scrolllock_mask)
        locks[num_locks++] = scrolllock_mask;

    for (int combo = 0; combo < (1 << num_locks); combo++) {
        unsigned int extra = 0;
        for (int b = 0; b < num_locks; b++)
            if (combo & (1 << b))
                extra |= locks[b];
        XGrabKey(server.display, keycode, modifiers | extra, server.root_win,
                 False, GrabModeAsync, GrabModeAsync);
    }
}

// Grabs a shortcut and records its name in failed if another client owns it.
static void try_grab(KeySym keysym, unsigned int modifiers, const char *name, GString *failed)
{
    int before = num_grab_conflicts;
    grab_key(keysym, modifiers);
    XSync(server.display, False); // Flush so the error handler observes any conflict.
    if (num_grab_conflicts != before)
        g_string_append_printf(failed, " %s", name);
}

void shortcuts_init()
{
    if (!keyboard_shortcuts)
        return;

    super_mask = modifier_mask_for_keysym(XK_Super_L);
    if (!super_mask)
        super_mask = Mod4Mask;
    numlock_mask = modifier_mask_for_keysym(XK_Num_Lock);
    scrolllock_mask = modifier_mask_for_keysym(XK_Scroll_Lock);

    XSync(server.display, False);
    XErrorHandler prev_handler = XSetErrorHandler(grab_error_handler);

    GString *failed = g_string_new(NULL);
    for (int i = 0; i < NUM_DESKTOP_KEYS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Super+%d", (i + 1) % 10);
        try_grab(desktop_keysyms[i], super_mask, name, failed);
    }
    try_grab(XK_n, super_mask, "Super+n", failed);
    try_grab(XK_n, super_mask | ShiftMask, "Super+Shift+n", failed);
    try_grab(XK_Tab, super_mask, "Super+Tab", failed);

    XSetErrorHandler(prev_handler);

    if (failed->len)
        fprintf(stderr,
                "tint2: could not grab keyboard shortcut(s), already used by another "
                "program (e.g. the window manager):%s\n",
                failed->str);
    g_string_free(failed, TRUE);
}

void shortcuts_cleanup()
{
    if (server.display && server.root_win)
        XUngrabKey(server.display, AnyKey, AnyModifier, server.root_win);
}

void shortcuts_set_previous_desktop(int desktop)
{
    previous_desktop = desktop;
}

// Switches back to the desktop the user came from.
static void goto_previous_desktop()
{
    if (previous_desktop >= 0 && previous_desktop < server.num_desktops &&
        previous_desktop != server.desktop)
        change_desktop(previous_desktop);
}

// Activates the next (direction +1) or previous (direction -1) task on the
// current desktop, following the order shown in the panel.
static void cycle_task(int direction)
{
    if (!taskbar_enabled || server.desktop < 0 || server.desktop >= server.num_desktops)
        return;

    // Windows on the current desktop in display order, de-duplicated across panels.
    GList *windows = NULL;
    for (int i = 0; i < num_panels; i++) {
        Panel *panel = &panels[i];
        if (!panel->taskbar)
            continue;
        Taskbar *taskbar = &panel->taskbar[server.desktop];
        for_taskbar_tasks(taskbar, l) {
            Task *task = l->data;
            gpointer win = (gpointer)(uintptr_t)task->win;
            if (!g_list_find(windows, win))
                windows = g_list_append(windows, win);
        }
    }

    int count = g_list_length(windows);
    if (count > 0) {
        Window active = active_task ? active_task->win : get_active_window();
        int index = g_list_index(windows, (gpointer)(uintptr_t)active);
        int next = (index < 0) ? (direction > 0 ? 0 : count - 1)
                               : (index + direction + count) % count;
        activate_window((Window)(uintptr_t)g_list_nth_data(windows, next));
    }
    g_list_free(windows);
}

gboolean shortcuts_handle_keypress(XKeyEvent *e)
{
    if (!keyboard_shortcuts)
        return FALSE;

    // Match independently of the lock modifiers.
    unsigned int state = e->state & ~(LockMask | numlock_mask | scrolllock_mask);
    // Compare by keycode, symmetrically with how the keys were grabbed, so that
    // matching is independent of keyboard layout and shift level.
    KeyCode keycode = e->keycode;

    if (state == super_mask && keycode == XKeysymToKeycode(server.display, XK_Tab)) {
        goto_previous_desktop();
        return TRUE;
    }

    if (keycode == XKeysymToKeycode(server.display, XK_n)) {
        if (state == super_mask) {
            cycle_task(+1);
            return TRUE;
        }
        if (state == (super_mask | ShiftMask)) {
            cycle_task(-1);
            return TRUE;
        }
    }

    if (state == super_mask)
        for (int i = 0; i < NUM_DESKTOP_KEYS; i++)
            if (keycode == XKeysymToKeycode(server.display, desktop_keysyms[i])) {
                if (i < server.num_desktops && i != server.desktop)
                    change_desktop(i);
                return TRUE;
            }

    return FALSE;
}
