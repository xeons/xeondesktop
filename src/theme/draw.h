/* SPDX-License-Identifier: MIT
 * DPI-aware drawing primitives on top of cairo + pango.
 *
 * Geometry passed to these functions is in device pixels. Logical sizes
 * (96-DPI units) are converted with xd_px(), which rounds to whole pixels so
 * 1px bevels stay crisp at any fractional scale.
 */
#ifndef XD_THEME_DRAW_H
#define XD_THEME_DRAW_H

#include <cairo.h>

#include "scheme.h"

typedef struct xd_rect {
    int x, y, w, h;
} xd_rect;

typedef struct xd_rgba {
    double r, g, b, a;
} xd_rgba;

/* Logical units -> device pixels, never less than 1 for a positive input. */
int xd_px(double scale, double logical);

xd_rect xd_rect_inset(xd_rect r, int dx, int dy);
xd_rect xd_rect_inset4(xd_rect r, int left, int top, int right, int bottom);
bool xd_rect_empty(xd_rect r);

xd_rgba xd_rgba_from_rgb(xd_rgb c);
xd_rgba xd_rgba_hex(uint32_t rgb);
xd_rgba xd_rgba_mix(xd_rgba a, xd_rgba b, double t);
xd_rgba xd_rgba_alpha(xd_rgba c, double a);
/* Hue (0-360), saturation and value (0-1), as Windows 7's colour mixer
 * shows a colour; the alpha is kept. */
typedef struct xd_hsv {
    double h, s, v;
} xd_hsv;
xd_hsv xd_rgba_to_hsv(xd_rgba c);
xd_rgba xd_rgba_from_hsv(xd_hsv hsv, double alpha);

void xd_set_color(cairo_t *cr, xd_rgba c);
void xd_fill_rect(cairo_t *cr, xd_rect r, xd_rgba c);
/* Gradient fill; horizontal when `horizontal` is true, else top to bottom. */
void xd_fill_gradient(cairo_t *cr, xd_rect r, xd_rgba from, xd_rgba to, bool horizontal);
/* Vertical multi-stop gradient. offsets are 0..1 and ascending. */
void xd_fill_vstops(cairo_t *cr, xd_rect r, const double *offsets, const xd_rgba *colors,
                    size_t count);
void xd_path_rounded(cairo_t *cr, double x, double y, double w, double h, double rtl,
                     double rtr, double rbr, double rbl);

/* A `t`-pixel bevel: top/left in `tl`, bottom/right in `br`. Returns the
 * rectangle inside it. */
xd_rect xd_draw_bevel(cairo_t *cr, xd_rect r, int t, xd_rgba tl, xd_rgba br);

typedef enum xd_halign { XD_ALIGN_START, XD_ALIGN_CENTER, XD_ALIGN_END } xd_halign;

typedef struct xd_text_style {
    const xd_font_spec *font;
    double scale; /* device pixels per logical unit */
    xd_rgba color;
    xd_halign align;
    bool ellipsize;
    bool shadow; /* 1px drop shadow (Luna caption text) */
    xd_rgba shadow_color;
    bool wrap; /* wrap at word boundaries and align to the top of the rect */
    /* '\n' starts another line; the lines are centred on each other and
     * the block aligned as `align` says (the clock's day and date). */
    bool lines;
    xd_font_smoothing smoothing;
} xd_text_style;

/* Draws one line of text vertically centred in r and clipped to it. */
/* Windows' shortcut overlay: a white box with a curved black arrow, at
 * the bottom left of an icon of `icon_px` whose top left is (x, y). */
void xd_draw_shortcut_arrow(cairo_t *cr, double x, double y, double icon_px);
/* The Start button / banner logo: Tux, the Linux penguin. */
void xd_draw_start_logo(cairo_t *cr, xd_rect r);
void xd_draw_text(cairo_t *cr, xd_rect r, const char *utf8, const xd_text_style *style);
/* Width in device pixels of one line of text. */
int xd_text_width(cairo_t *cr, const char *utf8, const xd_text_style *style);

/* The Pango layout xd_draw_text would use, for callers that need caret
 * positions or hit testing (edit controls). Free with g_object_unref. */
typedef struct _PangoLayout PangoLayout;
PangoLayout *xd_text_layout(cairo_t *cr, const char *utf8, const xd_text_style *style);

#endif
