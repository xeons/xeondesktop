/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "style.h"

static const xd_style_ops *const styles[XD_STYLE_COUNT] = {
    [XD_STYLE_CLASSIC] = &xd_style_classic,
    [XD_STYLE_LUNA] = &xd_style_luna,
    [XD_STYLE_WHISTLER] = &xd_style_whistler,
};

static const xd_style_ops *ops(const xd_theme *t) {
    XD_ASSERT((unsigned)t->style < XD_STYLE_COUNT);
    return styles[t->style];
}

const char *xd_visual_style_name(xd_visual_style s) {
    XD_ASSERT((unsigned)s < XD_STYLE_COUNT);
    return styles[s]->name;
}

bool xd_visual_style_from_name(xd_str name, xd_visual_style *out) {
    for (size_t i = 0; i < XD_STYLE_COUNT; i++) {
        if (xd_str_eq_nocase(name, xd_str_from_cstr(styles[i]->name))) {
            *out = (xd_visual_style)i;
            return true;
        }
    }
    return false;
}

void xd_theme_init(xd_theme *t, xd_visual_style style, const xd_scheme *scheme, double scale) {
    XD_ASSERT((unsigned)style < XD_STYLE_COUNT);
    XD_ASSERT(scale >= 0.5 && scale <= 8.0);
    t->style = style;
    t->scheme = *scheme;
    t->scale = scale;
}

xd_insets xd_theme_frame_insets(const xd_theme *t, const xd_frame *f) {
    return ops(t)->frame_insets(t, f);
}
void xd_theme_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f) {
    ops(t)->draw_frame(t, cr, outer, f);
}
xd_rgba xd_theme_dialog_color(const xd_theme *t) { return ops(t)->dialog_color(t); }
void xd_theme_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                          const char *label) {
    ops(t)->draw_button(t, cr, r, state, label);
}
void xd_theme_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                            const char *label) {
    ops(t)->draw_checkbox(t, cr, r, state, label);
}
int xd_theme_taskbar_height(const xd_theme *t) { return ops(t)->taskbar_height(t); }
void xd_theme_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r) {
    ops(t)->draw_taskbar(t, cr, r);
}
int xd_theme_start_button_width(const xd_theme *t, cairo_t *cr) {
    return ops(t)->start_button_width(t, cr);
}
void xd_theme_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    ops(t)->draw_start_button(t, cr, r, state);
}
void xd_theme_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label) {
    ops(t)->draw_task_button(t, cr, r, state, label);
}
void xd_theme_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock) {
    ops(t)->draw_tray(t, cr, r, clock);
}

/* ---- shared helpers ---------------------------------------------------- */

xd_rgba xd_sys(const xd_theme *t, xd_syscolor c) {
    XD_ASSERT((unsigned)c < XD_COLOR_COUNT);
    return xd_rgba_from_rgb(t->scheme.colors[c]);
}

int xd_tpx(const xd_theme *t, double logical) { return xd_px(t->scale, logical); }

int xd_metric_px(const xd_theme *t, xd_metric m) {
    XD_ASSERT((unsigned)m < XD_METRIC_COUNT);
    return xd_px(t->scale, t->scheme.metrics[m]);
}

xd_text_style xd_text(const xd_theme *t, xd_font_role role, xd_rgba color, xd_halign align) {
    XD_ASSERT((unsigned)role < XD_FONT_COUNT);
    return (xd_text_style){
        .font = &t->scheme.fonts[role],
        .scale = t->scale,
        .color = color,
        .align = align,
        .ellipsize = true,
    };
}

void xd_draw_glyph(const xd_theme *t, cairo_t *cr, xd_rect r, xd_glyph g, xd_rgba color,
                   double weight) {
    /* Glyphs are designed on a 9x9 logical grid centred in r, so they scale
     * with DPI exactly like their strokes do. */
    int size = XD_MIN(xd_tpx(t, 9), XD_MIN(r.w, r.h));
    double x = r.x + (r.w - size) / 2;
    double y = r.y + (r.h - size) / 2;
    double s = size;
    double lw = fmax(1.0, round(weight * t->scale));
    double thick = fmax(1.0, round(weight * 1.6 * t->scale));

    cairo_save(cr);
    xd_set_color(cr, color);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
    switch (g) {
    case XD_GLYPH_CLOSE: {
        double inset = round(s * 0.1);
        cairo_set_line_width(cr, thick);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_move_to(cr, x + inset, y + inset);
        cairo_line_to(cr, x + s - inset, y + s - inset);
        cairo_move_to(cr, x + s - inset, y + inset);
        cairo_line_to(cr, x + inset, y + s - inset);
        cairo_stroke(cr);
        break;
    }
    case XD_GLYPH_MAXIMIZE: {
        double top = round(lw * 2);
        cairo_rectangle(cr, x, y, s, top);
        cairo_fill(cr);
        cairo_set_line_width(cr, lw);
        cairo_rectangle(cr, x + lw / 2, y + lw / 2, s - lw, s - lw);
        cairo_stroke(cr);
        break;
    }
    case XD_GLYPH_RESTORE: {
        double b = round(s * 0.65);
        double off = s - b;
        double top = round(lw * 2);
        cairo_set_line_width(cr, lw);
        /* Front window (bottom-left). */
        cairo_rectangle(cr, x, y + off, b, top);
        cairo_fill(cr);
        cairo_rectangle(cr, x + lw / 2, y + off + lw / 2, b - lw, b - lw);
        cairo_stroke(cr);
        /* Back window (top-right), clipped so it stays behind the front. */
        cairo_save(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(cr, x - 1, y - 1, s + 2, s + 2);
        cairo_rectangle(cr, x, y + off, b, b);
        cairo_clip(cr);
        cairo_rectangle(cr, x + off, y, b, top);
        cairo_fill(cr);
        cairo_rectangle(cr, x + off + lw / 2, y + lw / 2, b - lw, b - lw);
        cairo_stroke(cr);
        cairo_restore(cr);
        break;
    }
    case XD_GLYPH_MINIMIZE: {
        double h = round(lw * 2);
        cairo_rectangle(cr, x + round(s * 0.1), y + s - h, round(s * 0.65), h);
        cairo_fill(cr);
        break;
    }
    case XD_GLYPH_HELP: {
        cairo_set_line_width(cr, thick);
        cairo_arc(cr, x + s / 2, y + s * 0.3, s * 0.25, XD_PI, 2.2 * XD_PI);
        cairo_line_to(cr, x + s / 2, y + s * 0.68);
        cairo_stroke(cr);
        cairo_rectangle(cr, x + s / 2 - lw, y + s - 2 * lw, 2 * lw, 2 * lw);
        cairo_fill(cr);
        break;
    }
    case XD_GLYPH_CHECK: {
        cairo_set_line_width(cr, thick);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_move_to(cr, x + s * 0.12, y + s * 0.5);
        cairo_line_to(cr, x + s * 0.4, y + s * 0.78);
        cairo_line_to(cr, x + s * 0.9, y + s * 0.2);
        cairo_stroke(cr);
        break;
    }
    }
    cairo_restore(cr);
}

void xd_draw_start_flag(cairo_t *cr, xd_rect r) {
    /* A four-pane waving flag in the classic colours, drawn with skewed
     * quads so it scales cleanly. */
    static const uint32_t pane[4] = {0xE2431B, 0x5DB82D, 0x2D7AE0, 0xF5C21B};
    double s = XD_MIN(r.w, r.h);
    double x0 = r.x + (r.w - s) / 2;
    double y0 = r.y + (r.h - s) / 2;
    double half = s * 0.46;
    double gap = s * 0.08;
    double wave = s * 0.08;

    for (int i = 0; i < 4; i++) {
        double px = x0 + (i % 2) * (half + gap);
        double py = y0 + (i / 2) * (half + gap);
        double skew = (i % 2) ? -wave : 0;
        xd_set_color(cr, xd_rgba_hex(pane[i]));
        cairo_move_to(cr, px, py + skew + wave);
        cairo_curve_to(cr, px + half * 0.33, py + skew, px + half * 0.66, py + skew,
                       px + half, py + skew + wave * 0.2);
        cairo_line_to(cr, px + half, py + skew + half);
        cairo_curve_to(cr, px + half * 0.66, py + skew + half - wave * 0.8, px + half * 0.33,
                       py + skew + half - wave * 0.8, px, py + skew + half + wave * 0.5);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}
