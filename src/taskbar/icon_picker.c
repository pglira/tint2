/**************************************************************************
* icon_picker
*
* Lets the user replace a window's taskbar icon with a short label of up to
* three characters (letters, digits or a single emoji), rendered to fit a
* rounded square of a chosen color. A popup offers a text field, a row of
* color swatches, and a history of recently used icons (label plus color)
* for quick reuse. Text is typed directly; emojis are pasted from the
* clipboard (Ctrl+V) or the primary selection.
*
* Overrides are keyed by the X11 window id and live only for the running
* session; the history persists in the user cache directory.
**************************************************************************/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <pango/pangocairo.h>
#include <Imlib2.h>
#include <glib.h>

#include "icon_picker.h"
#include "panel.h"
#include "server.h"
#include "task.h"
#include "taskbar.h"

#define HISTORY_MAX 10  // number of remembered icons
#define LABEL_CHARS 3   // maximum grapheme clusters per label
#define GRID_COLS 5     // history cells per row
#define SWATCH_COLS 9   // color swatches per row
#define SWATCH_ROWS 2
#define NUM_SWATCHES (SWATCH_COLS * SWATCH_ROWS)

typedef enum HitType {
    HZ_NONE,
    HZ_SWATCH,
    HZ_GRADIENT,
    HZ_HISTORY,
    HZ_RESET,
} HitType;

// Swatch palette: a row of bright hues over a row of darker variants.
static const char *swatch_palette[NUM_SWATCHES] = {
    "e53935", "fb8c00", "fdd835", "43a047", "00897b", "1e88e5", "3949ab", "8e24aa", "d81b60",
    "b71c1c", "e65100", "f9a825", "2e7d32", "00695c", "1565c0", "283593", "6a1b9a", "424242",
};

// History of icons, most-recent first.
static GList *history = NULL;
static gboolean history_loading = FALSE;

// Window id -> IconSpec. Session only.
static GHashTable *win_overrides = NULL;

// Popup state.
static Window picker_win = None;
static Window picker_target = None;     // window whose icon the popup edits
static int picker_cell;                 // cell side in pixels
static int picker_hist_rows;            // rows occupied by history cells
static int picker_w, picker_h;          // popup size in pixels
static int picker_n;                    // history entries shown
static HitType hover_type = HZ_NONE;    // hovered region
static int hover_index = -1;            // index within the hovered region
static char input_text[64];             // current edit buffer (UTF-8)
static Color current_color;             // color applied to the edited icon
static cairo_surface_t *grad_surface = NULL;  // cached gradient picker image
static gboolean tried_primary = FALSE;

static Atom a_clipboard = None;
static Atom a_utf8 = None;
static Atom a_paste_prop = None;
static XIM xim = NULL;
static XIC xic = NULL;

Color icon_badge_bg_color = { {0.23, 0.23, 0.25}, 1.0 };

// ------------------------------------------------------------------ colors ---

static gboolean hex_to_color(const char *hex, Color *c)
{
    if (*hex == '#')
        hex++;
    size_t len = strlen(hex);
    if (len != 6 && len != 8)
        return FALSE;
    char byte[3] = {0};
    for (int i = 0; i < 3; i++) {
        byte[0] = hex[i * 2];
        byte[1] = hex[i * 2 + 1];
        c->rgb[i] = (int)strtol(byte, NULL, 16) / 255.0;
    }
    if (len == 8) {
        byte[0] = hex[6];
        byte[1] = hex[7];
        c->alpha = (int)strtol(byte, NULL, 16) / 255.0;
    } else {
        c->alpha = 1.0;
    }
    return TRUE;
}

static void color_to_hex(const Color *c, char *out /* >= 9 bytes */)
{
    g_snprintf(out, 9, "%02x%02x%02x%02x", (int)(c->rgb[0] * 255 + 0.5), (int)(c->rgb[1] * 255 + 0.5),
               (int)(c->rgb[2] * 255 + 0.5), (int)(c->alpha * 255 + 0.5));
}

static gboolean color_equal(const Color *a, const Color *b)
{
    return fabs(a->rgb[0] - b->rgb[0]) < 0.004 && fabs(a->rgb[1] - b->rgb[1]) < 0.004 &&
           fabs(a->rgb[2] - b->rgb[2]) < 0.004 && fabs(a->alpha - b->alpha) < 0.004;
}

static Color swatch_color(int i)
{
    Color c = icon_badge_bg_color;
    if (i >= 0 && i < NUM_SWATCHES)
        hex_to_color(swatch_palette[i], &c);
    return c;
}

static void hsv_to_rgb(double h, double s, double v, double *r, double *g, double *b)
{
    h = fmod(h, 360);
    if (h < 0)
        h += 360;
    double cc = v * s;
    double x = cc * (1 - fabs(fmod(h / 60.0, 2) - 1));
    double m = v - cc;
    double R = 0, G = 0, B = 0;
    if (h < 60)        { R = cc; G = x; }
    else if (h < 120)  { R = x; G = cc; }
    else if (h < 180)  { G = cc; B = x; }
    else if (h < 240)  { G = x; B = cc; }
    else if (h < 300)  { R = x; B = cc; }
    else               { R = cc; B = x; }
    *r = R + m;
    *g = G + m;
    *b = B + m;
}

// Maps a position in the gradient picker (fx, fy in [0,1]) to a color:
// x selects hue, y goes white (top) -> saturated (middle) -> black (bottom).
static Color gradient_color(double fx, double fy)
{
    double sat, val;
    if (fy < 0.5) {
        sat = fy * 2;
        val = 1;
    } else {
        sat = 1;
        val = 1 - (fy - 0.5) * 2;
    }
    Color c = { {0, 0, 0}, 1.0 };
    hsv_to_rgb(fx * 360.0, sat, val, &c.rgb[0], &c.rgb[1], &c.rgb[2]);
    return c;
}

// Renders the gradient picker into an opaque image surface.
static cairo_surface_t *build_gradient(int w, int h)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(data + y * stride);
        double fy = h > 1 ? (double)y / (h - 1) : 0;
        for (int x = 0; x < w; x++) {
            Color c = gradient_color(w > 1 ? (double)x / (w - 1) : 0, fy);
            row[x] = ((int)(c.rgb[0] * 255 + 0.5) << 16) | ((int)(c.rgb[1] * 255 + 0.5) << 8) |
                     (int)(c.rgb[2] * 255 + 0.5);
        }
    }
    cairo_surface_mark_dirty(s);
    return s;
}

// ---------------------------------------------------------------- history ---

static void spec_free(gpointer p)
{
    IconSpec *s = p;
    if (s) {
        g_free(s->label);
        g_free(s);
    }
}

static char *history_path()
{
    return g_build_filename(g_get_user_cache_dir(), "tint2", "recent_icons", NULL);
}

static void history_save()
{
    if (history_loading)
        return;
    char *path = history_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GString *s = g_string_new("");
    for (GList *l = history; l; l = l->next) {
        IconSpec *spec = l->data;
        char hex[9];
        color_to_hex(&spec->bg, hex);
        g_string_append_printf(s, "%s\t%s\n", spec->label, hex);
    }
    g_file_set_contents(path, s->str, s->len, NULL);
    g_string_free(s, TRUE);
    g_free(path);
}

// Moves a (label, color) icon to the front of the history, de-duplicating.
static void history_push(const char *label, Color bg)
{
    if (!label || !*label)
        return;

    for (GList *l = history; l; l = l->next) {
        IconSpec *s = l->data;
        if (strcmp(s->label, label) == 0 && color_equal(&s->bg, &bg)) {
            spec_free(s);
            history = g_list_delete_link(history, l);
            break;
        }
    }
    IconSpec *spec = g_new0(IconSpec, 1);
    spec->label = g_strdup(label);
    spec->bg = bg;
    history = g_list_prepend(history, spec);

    while (g_list_length(history) > HISTORY_MAX) {
        GList *last = g_list_last(history);
        spec_free(last->data);
        history = g_list_delete_link(history, last);
    }
    history_save();
}

// Starter icons shown until the user adds their own. In display order.
static const char *default_history[] = {
    "💻", "🖥️", "🌐", "📡", "🛰️", "🤖", "⚙️", "📊", "🧠", "🔌",
};

static void history_load()
{
    char *path = history_path();
    char *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        char **lines = g_strsplit(content, "\n", -1);
        int count = 0;
        while (lines[count])
            count++;
        // The file stores most-recent first; prepend in reverse to keep order.
        for (int i = count - 1; i >= 0; --i) {
            char *line = lines[i];
            Color bg = icon_badge_bg_color;
            char *tab = strchr(line, '\t');
            if (tab) {
                *tab = '\0';
                hex_to_color(g_strstrip(tab + 1), &bg);
            }
            char *label = g_strstrip(line);
            if (*label)
                history_push(label, bg);
        }
        g_strfreev(lines);
        g_free(content);
    }
    g_free(path);

    // First run (no saved file): seed a usable set so the grid is not empty.
    if (!history)
        for (int i = (int)(sizeof(default_history) / sizeof(default_history[0])) - 1; i >= 0; --i)
            history_push(default_history[i], icon_badge_bg_color);
}

// --------------------------------------------------------------- overrides ---

const IconSpec *icon_override_get(Window win)
{
    return win_overrides ? g_hash_table_lookup(win_overrides, (gpointer)(uintptr_t)win) : NULL;
}

static void icon_override_set(Window win, const char *label, Color bg)
{
    if (!win_overrides)
        return;
    IconSpec *spec = g_new0(IconSpec, 1);
    spec->label = g_strdup(label);
    spec->bg = bg;
    g_hash_table_insert(win_overrides, (gpointer)(uintptr_t)win, spec);
}

void icon_override_forget(Window win)
{
    if (win_overrides)
        g_hash_table_remove(win_overrides, (gpointer)(uintptr_t)win);
}

// Rebuilds the cached icons of every task button for win and redraws.
static void refresh_window_icon(Window win)
{
    GPtrArray *task_buttons = get_task_buttons(win);
    if (task_buttons && task_buttons->len > 0) {
        task_update_icon(g_ptr_array_index(task_buttons, 0));
        schedule_panel_redraw();
    }
}

// --------------------------------------------------------- label rendering ---

// Draws text scaled to fill the box minus pad pixels on every side, centered.
// The caller sets the source color; color emoji ignore it.
static void draw_label_rect(cairo_t *c, const char *text, double x, double y, double w, double h, double pad)
{
    double aw = w - 2 * pad;
    double ah = h - 2 * pad;
    if (aw < 1)
        aw = 1;
    if (ah < 1)
        ah = 1;

    PangoLayout *layout = pango_cairo_create_layout(c);
    PangoFontDescription *desc = pango_font_description_from_string(get_default_font());
    pango_font_description_set_absolute_size(desc, ah * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);

    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    if (ink.width > 0 && ink.height > 0) {
        double s = MIN(aw / ink.width, ah / ink.height);
        pango_font_description_set_absolute_size(desc, ah * s * PANGO_SCALE);
        pango_layout_set_font_description(layout, desc);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        cairo_move_to(c, x + (w - ink.width) / 2.0 - ink.x, y + (h - ink.height) / 2.0 - ink.y);
        pango_cairo_show_layout(c, layout);
    }

    pango_font_description_free(desc);
    g_object_unref(layout);
}

// Rounded-square path filling the box inset by the given pixel margin.
static void rounded_square_path(cairo_t *c, double x, double y, double size, double inset)
{
    double bx = x + inset, by = y + inset, bs = size - 2 * inset;
    double r = bs * 0.23;
    cairo_new_sub_path(c);
    cairo_arc(c, bx + bs - r, by + r, r, -M_PI / 2, 0);
    cairo_arc(c, bx + bs - r, by + bs - r, r, 0, M_PI / 2);
    cairo_arc(c, bx + r, by + bs - r, r, M_PI / 2, M_PI);
    cairo_arc(c, bx + r, by + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(c);
}

// Draws the label on a rounded square of color bg, the label in white with a
// 1px gap to the square edge (color emoji keep their own colors). inset is the
// transparent margin between the square and the box edge in pixels.
static void draw_badge(cairo_t *c, const char *text, double x, double y, double size, Color bg, double inset)
{
    rounded_square_path(c, x, y, size, inset);
    cairo_set_source_rgba(c, bg.rgb[0], bg.rgb[1], bg.rgb[2], bg.alpha);
    cairo_fill(c);

    cairo_set_source_rgb(c, 0.97, 0.97, 0.97);
    draw_label_rect(c, text, x + inset, y + inset, size - 2 * inset, size - 2 * inset, 1.0);
}

Imlib_Image render_icon_image(const char *label, Color bg, int size)
{
    if (!label || !*label || size <= 0)
        return NULL;

    cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *c = cairo_create(cs);
    draw_badge(c, label, 0, 0, size, bg, 0);    // full-bleed, like an app icon
    cairo_destroy(c);
    cairo_surface_flush(cs);

    Imlib_Image img = imlib_create_image(size, size);
    if (img) {
        int stride = cairo_image_surface_get_stride(cs);
        unsigned char *pix = cairo_image_surface_get_data(cs);
        imlib_context_set_image(img);
        imlib_image_set_has_alpha(1);
        DATA32 *out = imlib_image_get_data();
        for (int y = 0; y < size; y++) {
            uint32_t *row = (uint32_t *)(pix + y * stride);
            for (int x = 0; x < size; x++) {
                uint32_t p = row[x];    // cairo: premultiplied native-endian ARGB
                unsigned a = (p >> 24) & 0xff;
                unsigned r = (p >> 16) & 0xff;
                unsigned g = (p >> 8) & 0xff;
                unsigned b = p & 0xff;
                if (a != 0 && a != 0xff) {
                    r = (r * 255 + a / 2) / a;
                    g = (g * 255 + a / 2) / a;
                    b = (b * 255 + a / 2) / a;
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                }
                out[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        imlib_image_put_back_data(out);
    }
    cairo_surface_destroy(cs);
    return img;
}

// --------------------------------------------------------- text edit logic ---

// Truncates s in place to at most max grapheme clusters (an emoji counts once).
static void truncate_graphemes(char *s, int max)
{
    glong n = g_utf8_strlen(s, -1);
    PangoLogAttr *attrs = g_new0(PangoLogAttr, n + 1);
    pango_get_log_attrs(s, -1, -1, NULL, attrs, n + 1);
    int before = 0;
    for (int i = 0; i <= n; i++) {
        if (attrs[i].is_cursor_position) {
            if (before == max) {
                *g_utf8_offset_to_pointer(s, i) = '\0';
                break;
            }
            before++;
        }
    }
    g_free(attrs);
}

// Appends valid UTF-8 to the edit buffer, keeping at most LABEL_CHARS graphemes.
static void input_append(const char *bytes)
{
    if (!bytes || !*bytes || !g_utf8_validate(bytes, -1, NULL))
        return;
    char *cand = g_strconcat(input_text, bytes, NULL);
    truncate_graphemes(cand, LABEL_CHARS);
    g_strlcpy(input_text, cand, sizeof(input_text));
    g_free(cand);
}

// Removes the last grapheme cluster from the edit buffer.
static void input_backspace()
{
    if (!*input_text)
        return;
    glong n = g_utf8_strlen(input_text, -1);
    PangoLogAttr *attrs = g_new0(PangoLogAttr, n + 1);
    pango_get_log_attrs(input_text, -1, -1, NULL, attrs, n + 1);
    int cut = 0;
    for (int i = (int)n - 1; i >= 0; i--) {
        if (attrs[i].is_cursor_position) {
            cut = i;
            break;
        }
    }
    *g_utf8_offset_to_pointer(input_text, cut) = '\0';
    g_free(attrs);
}

// ------------------------------------------------------------------- popup ---

static void picker_destroy()
{
    if (xic) {
        XDestroyIC(xic);
        xic = NULL;
    }
    if (picker_win != None) {
        XDestroyWindow(server.display, picker_win);
        picker_win = None;
    }
    if (grad_surface) {
        cairo_surface_destroy(grad_surface);
        grad_surface = NULL;
    }
    picker_target = None;
    hover_type = HZ_NONE;
    hover_index = -1;
    input_text[0] = '\0';
}

static void picker_close()
{
    if (picker_win == None)
        return;
    XUngrabPointer(server.display, CurrentTime);
    XUngrabKeyboard(server.display, CurrentTime);
    picker_destroy();
    XFlush(server.display);
}

// Applies a (label, color) icon to the target window and remembers it.
static void apply_label(const char *label, Color bg)
{
    char *copy = g_strdup(label);
    history_push(copy, bg);
    icon_override_set(picker_target, copy, bg);
    refresh_window_icon(picker_target);
    g_free(copy);
    picker_close();
}

// Closes the popup, applying the typed label first if there is one.
static void picker_commit()
{
    if (picker_win == None)
        return;
    if (*input_text)
        apply_label(input_text, current_color);
    else
        picker_close();
}

// Identifies the region and index under a pixel position.
// Rows top to bottom: input, swatch grid, gradient picker, history, footer.
static HitType picker_hit(int x, int y, int *index)
{
    *index = -1;
    if (x < 0 || y < 0 || x >= picker_w || y >= picker_h)
        return HZ_NONE;
    if (y < picker_cell)
        return HZ_NONE;    // input field
    if (y < 2 * picker_cell) {    // swatch grid
        int row = (y - picker_cell) * SWATCH_ROWS / picker_cell;
        if (row >= SWATCH_ROWS)
            row = SWATCH_ROWS - 1;
        int col = x * SWATCH_COLS / picker_w;
        if (col >= SWATCH_COLS)
            col = SWATCH_COLS - 1;
        *index = row * SWATCH_COLS + col;
        return HZ_SWATCH;
    }
    if (y < 3 * picker_cell)
        return HZ_GRADIENT;
    int hist_bottom = 3 * picker_cell + picker_hist_rows * picker_cell;
    if (y < hist_bottom) {
        int col = x / picker_cell;
        if (col >= GRID_COLS)
            return HZ_NONE;
        int row = (y - 3 * picker_cell) / picker_cell;
        int idx = row * GRID_COLS + col;
        if (idx < picker_n) {
            *index = idx;
            return HZ_HISTORY;
        }
        return HZ_NONE;
    }
    int rx = (picker_w - picker_cell) / 2;
    return (x >= rx && x < rx + picker_cell) ? HZ_RESET : HZ_NONE;
}

static void cell_highlight(cairo_t *c, double x, double y, double w, double h)
{
    cairo_set_source_rgb(c, 0.30, 0.32, 0.40);
    cairo_rectangle(c, x + 1, y + 1, w - 2, h - 2);
    cairo_fill(c);
}

static void picker_draw()
{
    if (picker_win == None)
        return;

    cairo_surface_t *cs =
        cairo_xlib_surface_create(server.display, picker_win, server.visual, picker_w, picker_h);
    cairo_t *c = cairo_create(cs);

    cairo_set_source_rgb(c, 0.15, 0.15, 0.17);
    cairo_paint(c);

    // Input field with a live preview of the edited icon.
    cairo_set_source_rgb(c, 0.10, 0.10, 0.12);
    cairo_rectangle(c, 0, 0, picker_w, picker_cell);
    cairo_fill(c);
    if (*input_text) {
        draw_badge(c, input_text, (picker_w - picker_cell) / 2.0, 0, picker_cell, current_color,
                   picker_cell * 0.06);
    } else {
        cairo_set_source_rgb(c, 0.5, 0.5, 0.55);
        draw_label_rect(c, "type 1-3 chars (Ctrl+V to paste)", picker_cell * 0.2, picker_cell * 0.3,
                        picker_w - picker_cell * 0.4, picker_cell * 0.4, 0);
    }

    // Color swatches (a grid of presets).
    double sh = (double)picker_cell / SWATCH_ROWS;
    for (int i = 0; i < NUM_SWATCHES; i++) {
        int col = i % SWATCH_COLS, row = i / SWATCH_COLS;
        double sx = (double)col * picker_w / SWATCH_COLS;
        double sxr = (double)(col + 1) * picker_w / SWATCH_COLS;
        double sy = picker_cell + row * sh;
        Color cc = swatch_color(i);
        cairo_set_source_rgb(c, cc.rgb[0], cc.rgb[1], cc.rgb[2]);
        cairo_rectangle(c, sx, sy, sxr - sx, sh);
        cairo_fill(c);
        if (color_equal(&cc, &current_color)) {
            cairo_set_source_rgb(c, 1, 1, 1);
            cairo_set_line_width(c, 2);
            cairo_rectangle(c, sx + 1.5, sy + 1.5, sxr - sx - 3, sh - 3);
            cairo_stroke(c);
        } else if (hover_type == HZ_SWATCH && hover_index == i) {
            cairo_set_source_rgba(c, 1, 1, 1, 0.5);
            cairo_set_line_width(c, 1);
            cairo_rectangle(c, sx + 1, sy + 1, sxr - sx - 2, sh - 2);
            cairo_stroke(c);
        }
    }

    // Gradient color picker.
    if (grad_surface) {
        cairo_set_source_surface(c, grad_surface, 0, 2 * picker_cell);
        cairo_rectangle(c, 0, 2 * picker_cell, picker_w, picker_cell);
        cairo_fill(c);
    }

    // History grid, each cell previewing the resulting icon with its color.
    for (int idx = 0; idx < picker_n; idx++) {
        IconSpec *spec = g_list_nth_data(history, idx);
        if (!spec)
            continue;
        int col = idx % GRID_COLS;
        int row = idx / GRID_COLS;
        double cx = col * picker_cell;
        double cy = 3 * picker_cell + row * picker_cell;
        if (hover_type == HZ_HISTORY && hover_index == idx)
            cell_highlight(c, cx, cy, picker_cell, picker_cell);
        draw_badge(c, spec->label, cx, cy, picker_cell, spec->bg, picker_cell * 0.08);
    }

    // Footer: a centered reset button to restore the window's own icon.
    double fy = 3 * picker_cell + picker_hist_rows * picker_cell;
    double rx = (picker_w - picker_cell) / 2.0;
    if (hover_type == HZ_RESET)
        cell_highlight(c, rx, fy, picker_cell, picker_cell);
    cairo_set_source_rgb(c, 0.95, 0.6, 0.6);
    draw_label_rect(c, "\xc3\x97", rx, fy, picker_cell, picker_cell, picker_cell * 0.25);  // '×' restore

    cairo_destroy(c);
    cairo_surface_destroy(cs);
}

// Grabs the pointer, retrying briefly while the just-mapped window settles.
static gboolean grab_pointer()
{
    for (int i = 0; i < 100; i++) {
        if (XGrabPointer(server.display, picker_win, False, ButtonPressMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess)
            return TRUE;
        nanosleep(&(struct timespec){0, 1000000}, NULL);
    }
    return FALSE;
}

void icon_picker_open(Task *task)
{
    if (!task)
        return;
    if (picker_win != None)
        picker_close();

    picker_target = task->win;
    picker_n = g_list_length(history);
    input_text[0] = '\0';
    current_color = icon_badge_bg_color;
    tried_primary = FALSE;

    Panel *panel = task->area.panel;
    double scale = panel ? panel->scale : 1.0;
    picker_cell = (int)(44 * scale + 0.5);
    if (picker_cell < 28)
        picker_cell = 28;

    picker_hist_rows = (picker_n + GRID_COLS - 1) / GRID_COLS;
    picker_w = GRID_COLS * picker_cell;
    // input + swatches + gradient + history + footer
    picker_h = picker_cell * (3 + picker_hist_rows + 1);

    if (grad_surface)
        cairo_surface_destroy(grad_surface);
    grad_surface = build_gradient(picker_w, picker_cell);

    // Position centered above the pointer, clamped to the panel's monitor.
    Window r, child;
    int rx, ry, wx, wy;
    unsigned m;
    XQueryPointer(server.display, server.root_win, &r, &child, &rx, &ry, &wx, &wy, &m);
    Monitor *mon = &server.monitors[panel ? panel->monitor : 0];
    int px = rx - picker_w / 2;
    int py = ry - picker_h - 8;
    if (px < mon->x)
        px = mon->x;
    if (py < mon->y)
        py = mon->y;
    if (px + picker_w > mon->x + mon->width)
        px = mon->x + mon->width - picker_w;
    if (py + picker_h > mon->y + mon->height)
        py = mon->y + mon->height - picker_h;

    XSetWindowAttributes attr;
    attr.override_redirect = True;
    attr.event_mask = ButtonPressMask | ExposureMask | KeyPressMask | PointerMotionMask | LeaveWindowMask;
    attr.colormap = server.colormap;
    attr.background_pixel = 0;
    attr.border_pixel = 0;
    unsigned long mask = CWEventMask | CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect;
    picker_win = XCreateWindow(server.display, server.root_win, px, py, picker_w, picker_h, 0,
                               server.depth, InputOutput, server.visual, mask, &attr);
    hover_type = HZ_NONE;
    hover_index = -1;

    if (xim)
        xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                        picker_win, XNFocusWindow, picker_win, NULL);

    XMapRaised(server.display, picker_win);
    XSync(server.display, False);
    if (!grab_pointer()) {
        // Cannot confine input to the popup; abandon it rather than leave a
        // window the user cannot dismiss.
        picker_destroy();
        return;
    }
    XGrabKeyboard(server.display, picker_win, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    if (xic)
        XSetICFocus(xic);
    XFlush(server.display);
}

// -------------------------------------------------------------- clipboard ---

static void request_selection(Atom selection)
{
    XConvertSelection(server.display, selection, a_utf8, a_paste_prop, picker_win, CurrentTime);
    XFlush(server.display);
}

static void icon_picker_selection_notify(XSelectionEvent *e)
{
    if (picker_win == None || e->requestor != picker_win)
        return;

    char *text = NULL;
    if (e->property != None) {
        Atom type;
        int fmt;
        unsigned long nitems, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(server.display, picker_win, a_paste_prop, 0, 256, True, AnyPropertyType,
                               &type, &fmt, &nitems, &after, &data) == Success) {
            if (data && nitems > 0)
                text = g_strndup((char *)data, nitems);
            if (data)
                XFree(data);
        }
    }

    if (text) {
        g_strstrip(text);
        for (char *p = text; *p; ++p)
            if (*p == '\n' || *p == '\r' || *p == '\t') {
                *p = '\0';
                break;
            }
    }

    if (text && *text) {
        input_append(text);
        picker_draw();
    } else if (!tried_primary) {
        // Nothing useful on the clipboard; fall back to the primary selection.
        tried_primary = TRUE;
        request_selection(XA_PRIMARY);
    }
    g_free(text);
}

// ---------------------------------------------------------------- dispatch ---

gboolean icon_picker_handles_window(Window win)
{
    return picker_win != None && win == picker_win;
}

static int lookup_text(XKeyEvent *ev, char *buf, int size, KeySym *ks)
{
    int len;
    if (xic) {
        Status st;
        len = Xutf8LookupString(xic, ev, buf, size - 1, ks, &st);
        if (st == XBufferOverflow)
            len = 0;
    } else {
        len = XLookupString(ev, buf, size - 1, ks, NULL);
    }
    buf[len] = '\0';
    return len;
}

void icon_picker_handle_event(XEvent *e)
{
    switch (e->type) {
    case Expose:
        picker_draw();
        break;
    case MotionNotify: {
        int idx;
        HitType t = picker_hit(e->xmotion.x, e->xmotion.y, &idx);
        if (t == HZ_GRADIENT && (e->xmotion.state & Button1Mask)) {
            // Drag across the gradient to pick a color live.
            current_color = gradient_color(CLAMP((double)e->xmotion.x / picker_w, 0, 1),
                                           CLAMP((double)(e->xmotion.y - 2 * picker_cell) / picker_cell, 0, 1));
            hover_type = t;
            hover_index = idx;
            picker_draw();
        } else if (t != hover_type || idx != hover_index) {
            hover_type = t;
            hover_index = idx;
            picker_draw();
        }
        break;
    }
    case LeaveNotify:
        if (hover_type != HZ_NONE) {
            hover_type = HZ_NONE;
            hover_index = -1;
            picker_draw();
        }
        break;
    case KeyPress: {
        char buf[32];
        KeySym ks = NoSymbol;
        int len = lookup_text(&e->xkey, buf, sizeof(buf), &ks);
        if (ks == XK_Escape) {
            picker_commit();
        } else if (ks == XK_BackSpace) {
            input_backspace();
            picker_draw();
        } else if ((e->xkey.state & ControlMask) && (ks == XK_v || ks == XK_V)) {
            tried_primary = FALSE;
            request_selection(a_clipboard);
        } else if (len > 0 && (unsigned char)buf[0] >= 0x20) {
            input_append(buf);
            picker_draw();
        }
        break;
    }
    case ButtonPress: {
        int idx;
        HitType t = picker_hit(e->xbutton.x, e->xbutton.y, &idx);
        if (t == HZ_SWATCH) {
            current_color = swatch_color(idx);
            picker_draw();
        } else if (t == HZ_GRADIENT) {
            current_color = gradient_color(CLAMP((double)e->xbutton.x / picker_w, 0, 1),
                                           CLAMP((double)(e->xbutton.y - 2 * picker_cell) / picker_cell, 0, 1));
            picker_draw();
        } else if (t == HZ_HISTORY) {
            IconSpec *spec = g_list_nth_data(history, idx);
            char *label = g_strdup(spec->label);
            Color bg = spec->bg;
            apply_label(label, bg);
            g_free(label);
        } else if (t == HZ_RESET) {
            icon_override_forget(picker_target);
            refresh_window_icon(picker_target);
            picker_close();
        } else if (e->xbutton.x < 0 || e->xbutton.y < 0 || e->xbutton.x >= picker_w ||
                   e->xbutton.y >= picker_h) {
            picker_commit();    // leaving the popup applies the typed label
        }
        break;
    }
    case SelectionNotify:
        icon_picker_selection_notify(&e->xselection);
        break;
    }
}

// ----------------------------------------------------------- init/cleanup ---

void init_icon_picker()
{
    if (!win_overrides)
        win_overrides = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, spec_free);

    a_clipboard = XInternAtom(server.display, "CLIPBOARD", False);
    a_utf8 = server.atom[UTF8_STRING];
    a_paste_prop = XInternAtom(server.display, "_TINT2_ICON_PASTE", False);

    XSetLocaleModifiers("");
    xim = XOpenIM(server.display, NULL, NULL, NULL);

    history_loading = TRUE;
    history_load();
    history_loading = FALSE;
}

void cleanup_icon_picker()
{
    if (picker_win != None) {
        XUngrabPointer(server.display, CurrentTime);
        XUngrabKeyboard(server.display, CurrentTime);
        picker_destroy();
    }
    if (xim) {
        XCloseIM(xim);
        xim = NULL;
    }
    if (win_overrides) {
        g_hash_table_destroy(win_overrides);
        win_overrides = NULL;
    }
    for (GList *l = history; l; l = l->next)
        spec_free(l->data);
    g_list_free(history);
    history = NULL;
}
