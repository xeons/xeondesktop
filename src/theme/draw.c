/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "draw.h"

int xd_px(double scale, double logical) {
    XD_ASSERT(scale > 0 && scale <= 16);
    double pixels = round(logical * scale);
    if (logical > 0 && pixels < 1)
        return 1;
    XD_ASSERT(fabs(pixels) < (double)INT32_MAX);
    return (int)pixels;
}

xd_rect xd_rect_inset(xd_rect rect, int dx, int dy) { return xd_rect_inset4(rect, dx, dy, dx, dy); }

xd_rect xd_rect_inset4(xd_rect rect, int left, int top, int right, int bottom) {
    xd_rect inset = {rect.x + left, rect.y + top, rect.w - left - right, rect.h - top - bottom};
    if (inset.w < 0)
        inset.w = 0;
    if (inset.h < 0)
        inset.h = 0;
    return inset;
}

bool xd_rect_empty(xd_rect rect) { return rect.w <= 0 || rect.h <= 0; }

xd_rgba xd_rgba_from_rgb(xd_rgb color) {
    return (xd_rgba){color.r / 255.0, color.g / 255.0, color.b / 255.0, 1.0};
}

xd_rgba xd_rgba_hex(uint32_t rgb) {
    return (xd_rgba){((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                     (rgb & 0xFF) / 255.0, 1.0};
}

xd_rgba xd_rgba_mix(xd_rgba from, xd_rgba to, double amount) {
    return (xd_rgba){from.r + (to.r - from.r) * amount, from.g + (to.g - from.g) * amount,
                     from.b + (to.b - from.b) * amount, from.a + (to.a - from.a) * amount};
}

xd_rgba xd_rgba_alpha(xd_rgba color, double alpha) {
    color.a = alpha;
    return color;
}

xd_hsv xd_rgba_to_hsv(xd_rgba color) {
    double hi = fmax(color.r, fmax(color.g, color.b));
    double lo = fmin(color.r, fmin(color.g, color.b));
    double chroma = hi - lo;
    xd_hsv hsv = {0, hi > 0 ? chroma / hi : 0, hi};
    if (chroma <= 0)
        return hsv;
    if (hi == color.r)
        hsv.h = 60 * fmod((color.g - color.b) / chroma + 6, 6);
    else if (hi == color.g)
        hsv.h = 60 * ((color.b - color.r) / chroma + 2);
    else
        hsv.h = 60 * ((color.r - color.g) / chroma + 4);
    return hsv;
}

xd_rgba xd_rgba_from_hsv(xd_hsv hsv, double alpha) {
    double sector = fmod(fmod(hsv.h, 360) + 360, 360) / 60;
    double saturation = fmin(fmax(hsv.s, 0), 1), value = fmin(fmax(hsv.v, 0), 1);
    double chroma = value * saturation;
    double x = chroma * (1 - fabs(fmod(sector, 2) - 1));
    /* The sextant of the hue circle: which channel leads, which follows. */
    static const uint8_t lead[6] = {0, 1, 1, 2, 2, 0}, follow[6] = {1, 0, 2, 1, 0, 2};
    double rgb[3] = {0, 0, 0};
    int k = XD_CLAMP((int)sector, 0, 5);
    rgb[lead[k]] = chroma;
    rgb[follow[k]] = x;
    double offset = value - chroma;
    return (xd_rgba){rgb[0] + offset, rgb[1] + offset, rgb[2] + offset, alpha};
}

void xd_set_color(cairo_t *cairo, xd_rgba color) {
    cairo_set_source_rgba(cairo, color.r, color.g, color.b, color.a);
}

void xd_fill_rect(cairo_t *cairo, xd_rect rect, xd_rgba color) {
    if (xd_rect_empty(rect))
        return;
    xd_set_color(cairo, color);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_fill(cairo);
}

void xd_fill_gradient(cairo_t *cairo, xd_rect rect, xd_rgba from, xd_rgba to, bool horizontal) {
    if (xd_rect_empty(rect))
        return;
    cairo_pattern_t *pattern = horizontal
                                   ? cairo_pattern_create_linear(rect.x, 0, rect.x + rect.w, 0)
                                   : cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    cairo_pattern_add_color_stop_rgba(pattern, 0, from.r, from.g, from.b, from.a);
    cairo_pattern_add_color_stop_rgba(pattern, 1, to.r, to.g, to.b, to.a);
    cairo_set_source(cairo, pattern);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
}

void xd_fill_vstops(cairo_t *cairo, xd_rect rect, const double *offsets, const xd_rgba *colors,
                    size_t count) {
    if (xd_rect_empty(rect))
        return;
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    for (size_t i = 0; i < count; i++)
        cairo_pattern_add_color_stop_rgba(pattern, offsets[i], colors[i].r, colors[i].g,
                                          colors[i].b, colors[i].a);
    cairo_set_source(cairo, pattern);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
}

void xd_path_rounded(cairo_t *cairo, double x, double y, double width, double height, double rtl,
                     double rtr, double rbr, double rbl) {
    const double quarter = XD_PI / 2;
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, x + width - rtr, y + rtr, rtr, -quarter, 0);
    cairo_arc(cairo, x + width - rbr, y + height - rbr, rbr, 0, quarter);
    cairo_arc(cairo, x + rbl, y + height - rbl, rbl, quarter, 2 * quarter);
    cairo_arc(cairo, x + rtl, y + rtl, rtl, 2 * quarter, 3 * quarter);
    cairo_close_path(cairo);
}

xd_rect xd_draw_bevel(cairo_t *cairo, xd_rect rect, int thickness, xd_rgba tl, xd_rgba br) {
    if (xd_rect_empty(rect) || thickness <= 0)
        return rect;
    /* Bottom/right first; top/left overlaps the shared corner pixels the
     * way Win32 DrawEdge does. */
    xd_fill_rect(cairo, (xd_rect){rect.x, rect.y + rect.h - thickness, rect.w, thickness}, br);
    xd_fill_rect(cairo, (xd_rect){rect.x + rect.w - thickness, rect.y, thickness, rect.h}, br);
    xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, rect.w - thickness, thickness}, tl);
    xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, thickness, rect.h - thickness}, tl);
    return xd_rect_inset(rect, thickness, thickness);
}

static PangoLayout *make_layout(cairo_t *cairo, const char *utf8, const xd_text_style *st) {
    PangoLayout *layout = pango_cairo_create_layout(cairo);
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
    switch (st->smoothing.antialias) {
    case XD_SMOOTH_NONE:
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_NONE);
        break;
    case XD_SMOOTH_SUBPIXEL: {
        static const cairo_subpixel_order_t orders[] = {
            [XD_SUBPIXEL_RGB] = CAIRO_SUBPIXEL_ORDER_RGB,
            [XD_SUBPIXEL_BGR] = CAIRO_SUBPIXEL_ORDER_BGR,
            [XD_SUBPIXEL_VRGB] = CAIRO_SUBPIXEL_ORDER_VRGB,
            [XD_SUBPIXEL_VBGR] = CAIRO_SUBPIXEL_ORDER_VBGR,
        };
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
        cairo_font_options_set_subpixel_order(
            fo, orders[st->smoothing.order < XD_ARRAY_LEN(orders) ? st->smoothing.order : 0]);
        break;
    }
    case XD_SMOOTH_GRAYSCALE:
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
        break;
    }
    static const cairo_hint_style_t hints[] = {
        [XD_HINT_SLIGHT] = CAIRO_HINT_STYLE_SLIGHT,
        [XD_HINT_NONE] = CAIRO_HINT_STYLE_NONE,
        [XD_HINT_MEDIUM] = CAIRO_HINT_STYLE_MEDIUM,
        [XD_HINT_FULL] = CAIRO_HINT_STYLE_FULL,
    };
    cairo_font_options_set_hint_style(
        fo, hints[st->smoothing.hinting < XD_ARRAY_LEN(hints) ? st->smoothing.hinting : 0]);
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), fo);
    cairo_font_options_destroy(fo);
    pango_layout_context_changed(layout);
    pango_layout_set_text(layout, utf8, -1);
    pango_layout_set_single_paragraph_mode(layout, true);
    /* GDI never joins letters: no "fi" ligatures, as on Windows. */
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_font_features_new("liga 0, clig 0"));
    /* The access key's underline, as GDI draws it (Pango's own placing). */
    size_t len = strlen(utf8);
    if (st->underline != nullptr && st->underline >= utf8 && st->underline < utf8 + len) {
        PangoAttribute *underline = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
        underline->start_index = (guint)(st->underline - utf8);
        underline->end_index = (guint)(g_utf8_next_char(st->underline) - utf8);
        pango_attr_list_insert(attrs, underline);
    }
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
    return layout;
}

PangoLayout *xd_text_layout(cairo_t *cairo, const char *utf8, const xd_text_style *style) {
    return make_layout(cairo, utf8, style);
}

char *xd_text_strip_prefix(const char *label, int *key) {
    GString *out = g_string_sized_new(strlen(label));
    *key = -1;
    for (const char *cursor = label; *cursor != '\0'; cursor++) {
        if (*cursor != '&') {
            g_string_append_c(out, *cursor);
            continue;
        }
        if (cursor[1] == '&') {
            g_string_append_c(out, '&');
            cursor++;
        } else if (cursor[1] != '\0' && *key < 0) {
            *key = (int)out->len;
        }
    }
    return g_string_free(out, FALSE);
}

int xd_text_width(cairo_t *cairo, const char *utf8, const xd_text_style *style) {
    PangoLayout *layout = make_layout(cairo, utf8, style);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    g_object_unref(layout);
    return width;
}

/* Several lines, each centred on the widest, the block placed in r. */
static void draw_lines(cairo_t *cairo, xd_rect rect, PangoLayout *layout, const xd_text_style *st) {
    pango_layout_set_single_paragraph_mode(layout, false);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    int x = st->align == XD_ALIGN_CENTER ? rect.x + (rect.w - tw) / 2
            : st->align == XD_ALIGN_END  ? rect.x + rect.w - tw
                                         : rect.x;
    int y = rect.y + (rect.h - th) / 2;
    cairo_save(cairo);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_clip(cairo);
    if (st->shadow) {
        int off = xd_px(st->scale, 1);
        xd_set_color(cairo, st->shadow_color);
        cairo_move_to(cairo, x + off, y + off);
        pango_cairo_show_layout(cairo, layout);
    }
    xd_set_color(cairo, st->color);
    cairo_move_to(cairo, x, y);
    pango_cairo_show_layout(cairo, layout);
    cairo_restore(cairo);
}

void xd_draw_text(cairo_t *cairo, xd_rect rect, const char *utf8, const xd_text_style *st) {
    if (xd_rect_empty(rect) || utf8 == nullptr || utf8[0] == '\0')
        return;

    PangoLayout *layout = make_layout(cairo, utf8, st);
    if (st->lines && strchr(utf8, '\n') != nullptr) {
        draw_lines(cairo, rect, layout, st);
        g_object_unref(layout);
        return;
    }
    pango_layout_set_width(layout, rect.w * PANGO_SCALE);
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
    int y = st->wrap ? rect.y : rect.y + (rect.h - th) / 2;

    cairo_save(cairo);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_clip(cairo);
    if (st->shadow) {
        int off = xd_px(st->scale, 1);
        xd_set_color(cairo, st->shadow_color);
        cairo_move_to(cairo, rect.x + off, y + off);
        pango_cairo_show_layout(cairo, layout);
    }
    xd_set_color(cairo, st->color);
    cairo_move_to(cairo, rect.x, y);
    pango_cairo_show_layout(cairo, layout);
    cairo_restore(cairo);
    g_object_unref(layout);
}

void xd_draw_shortcut_arrow(cairo_t *cairo, double x, double y, double icon_px) {
    /* 9 of a 32px icon's pixels, drawn on a 9x9 grid. */
    double unit = icon_px / 32.0;
    double size = round(9 * unit);
    double bx = round(x), by = round(y + icon_px - size);
    double grid = size / 9.0;
    cairo_save(cairo);
    cairo_rectangle(cairo, bx, by, size, size);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_fill(cairo);
    /* The border on whole pixels, inside the box. */
    double lw = XD_MAX(1.0, round(unit));
    cairo_rectangle(cairo, bx + lw / 2, by + lw / 2, size - lw, size - lw);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_set_line_width(cairo, lw);
    cairo_stroke(cairo);
    /* The arrow: up from the bottom left, curving right, head at top right. */
    cairo_set_line_width(cairo, XD_MAX(1.0, 1.5 * unit));
    cairo_move_to(cairo, bx + 2.5 * grid, by + 7.5 * grid);
    cairo_curve_to(cairo, bx + 2.5 * grid, by + 4.5 * grid, bx + 4 * grid, by + 3.5 * grid,
                   bx + 6 * grid, by + 3.5 * grid);
    cairo_stroke(cairo);
    cairo_move_to(cairo, bx + 7.5 * grid, by + 3.5 * grid);
    cairo_line_to(cairo, bx + 5 * grid, by + 1.5 * grid);
    cairo_line_to(cairo, bx + 5 * grid, by + 5.5 * grid);
    cairo_close_path(cairo);
    cairo_fill(cairo);
    cairo_restore(cairo);
}
