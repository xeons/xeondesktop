/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "draw.h"

int xd_px(double scale, double logical) {
    XD_ASSERT(scale > 0 && scale <= 16);
    double v = round(logical * scale);
    if (logical > 0 && v < 1)
        return 1;
    XD_ASSERT(fabs(v) < (double)INT32_MAX);
    return (int)v;
}

xd_rect xd_rect_inset(xd_rect r, int dx, int dy) {
    return xd_rect_inset4(r, dx, dy, dx, dy);
}

xd_rect xd_rect_inset4(xd_rect r, int left, int top, int right, int bottom) {
    xd_rect o = {r.x + left, r.y + top, r.w - left - right, r.h - top - bottom};
    if (o.w < 0)
        o.w = 0;
    if (o.h < 0)
        o.h = 0;
    return o;
}

bool xd_rect_empty(xd_rect r) { return r.w <= 0 || r.h <= 0; }

xd_rgba xd_rgba_from_rgb(xd_rgb c) {
    return (xd_rgba){c.r / 255.0, c.g / 255.0, c.b / 255.0, 1.0};
}

xd_rgba xd_rgba_hex(uint32_t rgb) {
    return (xd_rgba){((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                     (rgb & 0xFF) / 255.0, 1.0};
}

xd_rgba xd_rgba_mix(xd_rgba a, xd_rgba b, double t) {
    return (xd_rgba){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                     a.a + (b.a - a.a) * t};
}

xd_rgba xd_rgba_alpha(xd_rgba c, double a) {
    c.a = a;
    return c;
}

void xd_set_color(cairo_t *cr, xd_rgba c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

void xd_fill_rect(cairo_t *cr, xd_rect r, xd_rgba c) {
    if (xd_rect_empty(r))
        return;
    xd_set_color(cr, c);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
}

void xd_fill_gradient(cairo_t *cr, xd_rect r, xd_rgba from, xd_rgba to, bool horizontal) {
    if (xd_rect_empty(r))
        return;
    cairo_pattern_t *p = horizontal ? cairo_pattern_create_linear(r.x, 0, r.x + r.w, 0)
                                    : cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    cairo_pattern_add_color_stop_rgba(p, 0, from.r, from.g, from.b, from.a);
    cairo_pattern_add_color_stop_rgba(p, 1, to.r, to.g, to.b, to.a);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

void xd_fill_vstops(cairo_t *cr, xd_rect r, const double *offsets, const xd_rgba *colors,
                    size_t count) {
    if (xd_rect_empty(r))
        return;
    cairo_pattern_t *p = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    for (size_t i = 0; i < count; i++)
        cairo_pattern_add_color_stop_rgba(p, offsets[i], colors[i].r, colors[i].g,
                                          colors[i].b, colors[i].a);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

void xd_path_rounded(cairo_t *cr, double x, double y, double w, double h, double rtl,
                     double rtr, double rbr, double rbl) {
    const double q = XD_PI / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rtr, y + rtr, rtr, -q, 0);
    cairo_arc(cr, x + w - rbr, y + h - rbr, rbr, 0, q);
    cairo_arc(cr, x + rbl, y + h - rbl, rbl, q, 2 * q);
    cairo_arc(cr, x + rtl, y + rtl, rtl, 2 * q, 3 * q);
    cairo_close_path(cr);
}

xd_rect xd_draw_bevel(cairo_t *cr, xd_rect r, int t, xd_rgba tl, xd_rgba br) {
    if (xd_rect_empty(r) || t <= 0)
        return r;
    /* Bottom/right first; top/left overlaps the shared corner pixels the
     * way Win32 DrawEdge does. */
    xd_fill_rect(cr, (xd_rect){r.x, r.y + r.h - t, r.w, t}, br);
    xd_fill_rect(cr, (xd_rect){r.x + r.w - t, r.y, t, r.h}, br);
    xd_fill_rect(cr, (xd_rect){r.x, r.y, r.w - t, t}, tl);
    xd_fill_rect(cr, (xd_rect){r.x, r.y, t, r.h - t}, tl);
    return xd_rect_inset(r, t, t);
}

static PangoLayout *make_layout(cairo_t *cr, const char *utf8, const xd_text_style *st) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(st->font->family);
    /* Points -> whole device pixels at 96 DPI times the scale, rounded as
     * GDI does (MulDiv(pt, dpi, 72)): 8pt Tahoma is 11px, not 10.67. */
    double px = round(st->font->size_pt * 96.0 * st->scale / 72.0);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_font_description_set_weight(fd, st->font->bold ? PANGO_WEIGHT_BOLD
                                                         : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(fd, st->font->italic ? PANGO_STYLE_ITALIC
                                                          : PANGO_STYLE_NORMAL);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    cairo_font_options_t *fo = cairo_font_options_create();
    switch (st->smoothing) {
    case XD_SMOOTH_NONE:
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_NONE);
        break;
    case XD_SMOOTH_CLEARTYPE:
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
        cairo_font_options_set_subpixel_order(fo, CAIRO_SUBPIXEL_ORDER_RGB);
        break;
    case XD_SMOOTH_STANDARD:
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
        break;
    }
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), fo);
    cairo_font_options_destroy(fo);
    pango_layout_context_changed(layout);
    pango_layout_set_text(layout, utf8, -1);
    pango_layout_set_single_paragraph_mode(layout, true);
    /* GDI never joins letters: no "fi" ligatures, as on Windows. */
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_font_features_new("liga 0, clig 0"));
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
    return layout;
}

PangoLayout *xd_text_layout(cairo_t *cr, const char *utf8, const xd_text_style *style) {
    return make_layout(cr, utf8, style);
}

int xd_text_width(cairo_t *cr, const char *utf8, const xd_text_style *style) {
    PangoLayout *layout = make_layout(cr, utf8, style);
    int w = 0;
    int h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    return w;
}

void xd_draw_text(cairo_t *cr, xd_rect r, const char *utf8, const xd_text_style *st) {
    if (xd_rect_empty(r) || utf8 == nullptr || utf8[0] == '\0')
        return;

    PangoLayout *layout = make_layout(cr, utf8, st);
    pango_layout_set_width(layout, r.w * PANGO_SCALE);
    if (st->wrap) {
        pango_layout_set_single_paragraph_mode(layout, false);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    } else if (st->ellipsize) {
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    }
    PangoAlignment align = st->align == XD_ALIGN_CENTER ? PANGO_ALIGN_CENTER
                           : st->align == XD_ALIGN_END  ? PANGO_ALIGN_RIGHT
                                                        : PANGO_ALIGN_LEFT;
    pango_layout_set_alignment(layout, align);

    int tw = 0;
    int th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    int y = st->wrap ? r.y : r.y + (r.h - th) / 2;

    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    if (st->shadow) {
        int off = xd_px(st->scale, 1);
        xd_set_color(cr, st->shadow_color);
        cairo_move_to(cr, r.x + off, y + off);
        pango_cairo_show_layout(cr, layout);
    }
    xd_set_color(cr, st->color);
    cairo_move_to(cr, r.x, y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);
}

void xd_draw_shortcut_arrow(cairo_t *cr, double x, double y, double icon_px) {
    /* 9 of a 32px icon's pixels, drawn on a 9x9 grid. */
    double u = icon_px / 32.0;
    double size = round(9 * u);
    double bx = round(x), by = round(y + icon_px - size);
    double g = size / 9.0;
    cairo_save(cr);
    cairo_rectangle(cr, bx, by, size, size);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    /* The border on whole pixels, inside the box. */
    double lw = XD_MAX(1.0, round(u));
    cairo_rectangle(cr, bx + lw / 2, by + lw / 2, size - lw, size - lw);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);
    /* The arrow: up from the bottom left, curving right, head at top right. */
    cairo_set_line_width(cr, XD_MAX(1.0, 1.5 * u));
    cairo_move_to(cr, bx + 2.5 * g, by + 7.5 * g);
    cairo_curve_to(cr, bx + 2.5 * g, by + 4.5 * g, bx + 4 * g, by + 3.5 * g, bx + 6 * g,
                   by + 3.5 * g);
    cairo_stroke(cr);
    cairo_move_to(cr, bx + 7.5 * g, by + 3.5 * g);
    cairo_line_to(cr, bx + 5 * g, by + 1.5 * g);
    cairo_line_to(cr, bx + 5 * g, by + 5.5 * g);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}
