/**************************************************************************
* icon_picker:
* Per-window custom icons drawn from a short text label (or emoji) on a
* colored rounded square, plus a popup to pick the label, its background
* color, and reuse recent ones.
**************************************************************************/

#ifndef ICON_PICKER_H
#define ICON_PICKER_H

#include <X11/Xlib.h>
#include <Imlib2.h>
#include <glib.h>

#include "color.h"
#include "task.h"

// A custom icon: a short label rendered on a rounded square of color bg.
typedef struct IconSpec {
    char *label;
    Color bg;
} IconSpec;

// Default background color for new labels, also offered as the first swatch.
// Configurable via the "icon_badge_background_color" option.
extern Color icon_badge_bg_color;

// Loads the persistent history and prepares the override table.
void init_icon_picker();

// Frees all state and destroys the popup if open.
void cleanup_icon_picker();

// Returns the custom icon overriding window win, or NULL if none.
const IconSpec *icon_override_get(Window win);

// Drops the override for win. Called when the window leaves the taskbar.
void icon_override_forget(Window win);

// Renders a label on a colored rounded square into a square Imlib_Image.
Imlib_Image render_icon_image(const char *label, Color bg, int size);

// Opens the icon picker popup acting on the given task's window.
void icon_picker_open(Task *task);

// True if win is the picker popup window (or its lingering selection owner).
gboolean icon_picker_handles_window(Window win);

// Dispatches an X event that belongs to the picker popup window.
void icon_picker_handle_event(XEvent *e);

#endif
