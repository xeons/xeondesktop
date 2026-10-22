/* SPDX-License-Identifier: MIT
 * The Snipping Tool's workings apart from Wayland and GTK, so the unit
 * tests can check them: what a selection covers, cutting it out of the
 * captured screen, the ink drawn on a snip, and the options.
 */
#ifndef XD_SNIP_SNIPPING_H
#define XD_SNIP_SNIPPING_H

#include <cairo.h>
#include <glib.h>
#include <stddef.h>

/* ---- selections ---------------------------------------------------------------------- */

typedef enum xd_snip_mode : uint8_t {
    XD_SNIP_FREE_FORM,
    XD_SNIP_RECTANGLE,
    XD_SNIP_WINDOW,
    XD_SNIP_FULL_SCREEN,
} xd_snip_mode;

/* The name used on the command line and in snip.ini ("free-form",
 * "rectangle", "window", "full-screen"), and back; false if unknown. */
const char *xd_snip_mode_name(xd_snip_mode mode);
bool xd_snip_mode_parse(const char *name, xd_snip_mode *mode);

/* A part of the desktop, in its logical coordinates. */
typedef struct xd_snip_box {
    double x, y, width, height;
} xd_snip_box;

typedef struct xd_snip_point {
    double x, y;
} xd_snip_point;

/* A window on the screen when it was captured, frontmost first. */
typedef struct xd_snip_window {
    int x, y, width, height;
    bool activated;
} xd_snip_window;

/* The box between two corners, either way round. */
xd_snip_box xd_snip_box_from_corners(xd_snip_point first, xd_snip_point second);
/* The box around a free-form line (empty for no points). */
xd_snip_box xd_snip_box_around(const xd_snip_point *points, size_t count);
/* Whether a selection is big enough to keep: a click alone isn't one. */
bool xd_snip_box_is_selection(xd_snip_box box);
/* The frontmost window at a point, or -1. */
int xd_snip_window_at(const xd_snip_window *windows, size_t count, double x, double y);
/* The window with the keyboard focus, or -1. */
int xd_snip_window_active(const xd_snip_window *windows, size_t count);

/* The screen as captured: the whole desktop in one picture, `scale`
 * pixels to a logical pixel, its top left corner at the desktop's
 * (origin_x, origin_y). */
typedef struct xd_snip_screen {
    cairo_surface_t *image;
    double origin_x, origin_y;
    double scale;
    xd_snip_window *windows;
    size_t window_count;
} xd_snip_screen;

void xd_snip_screen_free(xd_snip_screen *screen);

/* The pixels of `box` in the screen's picture, within it; false when
 * nothing of it is. */
bool xd_snip_screen_pixels(const xd_snip_screen *screen, xd_snip_box box,
                           cairo_rectangle_int_t *pixels);

/* Cuts `box` out of the screen as a new picture. With a free-form line
 * (`path`, desktop coordinates), what's outside it is white, as in
 * Windows; with `ink` (red, green, blue), the line or the box's edge is
 * drawn on it, 2 logical px wide (Options' "Show selection ink").
 * nullptr when the box is off the screen. */
cairo_surface_t *xd_snip_screen_cut(const xd_snip_screen *screen, xd_snip_box box,
                                    const xd_snip_point *path, size_t path_count,
                                    const double *ink);

/* ---- ink ------------------------------------------------------------------------------ */

typedef enum xd_ink_tool : uint8_t { XD_INK_PEN, XD_INK_HIGHLIGHTER } xd_ink_tool;

/* One line drawn with the pen or highlighter, in the snip's pixels. */
typedef struct xd_ink_stroke {
    xd_ink_tool tool;
    double red, green, blue;
    double width;
    GArray *points; /* xd_snip_point */
} xd_ink_stroke;

typedef struct xd_ink {
    GPtrArray *strokes; /* xd_ink_stroke, drawn in order */
} xd_ink;

void xd_ink_init(xd_ink *ink);
void xd_ink_finish(xd_ink *ink);
void xd_ink_clear(xd_ink *ink);
/* A new stroke starting at a point; extend it as the pointer moves. */
xd_ink_stroke *xd_ink_begin(xd_ink *ink, xd_ink_tool tool, const double color[3], double width,
                            xd_snip_point start);
void xd_ink_extend(xd_ink_stroke *stroke, xd_snip_point point);
/* The eraser: takes away every stroke within `radius` of the point, whole
 * strokes as Windows 7's does. True if any went. */
bool xd_ink_erase_at(xd_ink *ink, xd_snip_point point, double radius);
/* Draws the strokes; the highlighter's multiply what's under them, so the
 * text under it stays readable. */
void xd_ink_draw(const xd_ink *ink, cairo_t *cairo);

/* ---- options (~/.config/xeondesktop/snip.ini) ----------------------------------------- */

/* What a screen recording hears. */
typedef enum xd_record_sound : uint8_t {
    XD_RECORD_SILENT,
    XD_RECORD_MICROPHONE,
    XD_RECORD_COMPUTER,       /* what the speakers play */
    XD_RECORD_MICROPHONE_AND_COMPUTER,
} xd_record_sound;

/* "none", "microphone", "computer", "both", and back. */
const char *xd_record_sound_name(xd_record_sound sound);
bool xd_record_sound_parse(const char *name, xd_record_sound *sound);

typedef struct xd_snip_options {
    xd_snip_mode mode;
    int delay_seconds; /* 0..5 */
    bool hide_instructions;
    bool always_copy;
    bool prompt_to_save;
    bool show_overlay;
    bool show_selection_ink;
    double ink[3]; /* the selection's colour */
    xd_record_sound record_sound;
} xd_snip_options;

/* Windows 7's defaults. */
void xd_snip_options_init(xd_snip_options *options);
/* Keys missing or wrong keep what `options` has. */
void xd_snip_options_load_data(xd_snip_options *options, GKeyFile *file);
void xd_snip_options_save_data(const xd_snip_options *options, GKeyFile *file);

#endif
