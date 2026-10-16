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
void xd_theme_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                           xd_frame_layout *out) {
    *out = (xd_frame_layout){0};
    ops(t)->frame_layout(t, outer, f, out);
}
void xd_theme_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f) {
    ops(t)->draw_frame(t, cr, outer, f);
}

bool xd_frame_part_is_button(xd_frame_part p) {
    return p == XD_PART_CLOSE || p == XD_PART_MAXIMIZE || p == XD_PART_MINIMIZE ||
           p == XD_PART_HELP;
}

bool xd_frame_part_is_border(xd_frame_part p) { return p >= XD_PART_TOP; }

static bool rect_contains(xd_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h;
}

xd_frame_part xd_theme_frame_hit(const xd_theme *t, xd_rect outer, const xd_frame *f, int x,
                                 int y) {
    if (!rect_contains(outer, x, y))
        return XD_PART_NONE;

    xd_frame_layout l;
    xd_theme_frame_layout(t, outer, f, &l);
    for (size_t i = 0; i < l.button_count; i++) {
        if (rect_contains(l.button_rects[i], x, y))
            return l.button_parts[i];
    }

    xd_insets in = xd_theme_frame_insets(t, f);
    if (!f->maximized) {
        /* Edges are at least 4 logical px wide even with thin borders, and
         * the corner zones extend along each edge like Windows' do. */
        int edge = XD_MAX(in.left, xd_tpx(t, 4));
        int corner = xd_tpx(t, 16);
        bool left = x < outer.x + edge;
        bool right = x >= outer.x + outer.w - edge;
        bool top = y < outer.y + edge;
        bool bottom = y >= outer.y + outer.h - edge;
        bool near_left = x < outer.x + corner;
        bool near_right = x >= outer.x + outer.w - corner;
        bool near_top = y < outer.y + corner;
        bool near_bottom = y >= outer.y + outer.h - corner;

        if ((top && near_left) || (left && near_top))
            return XD_PART_TOP_LEFT;
        if ((top && near_right) || (right && near_top))
            return XD_PART_TOP_RIGHT;
        if ((bottom && near_left) || (left && near_bottom))
            return XD_PART_BOTTOM_LEFT;
        if ((bottom && near_right) || (right && near_bottom))
            return XD_PART_BOTTOM_RIGHT;
        if (top)
            return XD_PART_TOP;
        if (bottom)
            return XD_PART_BOTTOM;
        if (left)
            return XD_PART_LEFT;
        if (right)
            return XD_PART_RIGHT;
    }
    if (rect_contains(l.icon, x, y))
        return XD_PART_SYSMENU;
    if (y < outer.y + in.top)
        return XD_PART_CAPTION;
    return XD_PART_CLIENT;
}

void xd_layout_add_button(xd_frame_layout *l, xd_rect r, xd_frame_part part) {
    XD_ASSERT(l->button_count < XD_FRAME_MAX_BUTTONS);
    l->button_rects[l->button_count] = r;
    l->button_parts[l->button_count] = part;
    l->button_count++;
}

xd_glyph xd_glyph_for_part(xd_frame_part part, bool maximized) {
    switch (part) {
    case XD_PART_CLOSE:
        return XD_GLYPH_CLOSE;
    case XD_PART_MAXIMIZE:
        return maximized ? XD_GLYPH_RESTORE : XD_GLYPH_MAXIMIZE;
    case XD_PART_MINIMIZE:
        return XD_GLYPH_MINIMIZE;
    case XD_PART_HELP:
        return XD_GLYPH_HELP;
    case XD_PART_NONE:
    case XD_PART_CLIENT:
    case XD_PART_CAPTION:
    case XD_PART_SYSMENU:
    case XD_PART_TOP:
    case XD_PART_BOTTOM:
    case XD_PART_LEFT:
    case XD_PART_RIGHT:
    case XD_PART_TOP_LEFT:
    case XD_PART_TOP_RIGHT:
    case XD_PART_BOTTOM_LEFT:
    case XD_PART_BOTTOM_RIGHT:
        break;
    }
    XD_PANIC("frame part has no glyph");
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

/* ---- menus ----------------------------------------------------------------- */

xd_menu_metrics xd_theme_menu_metrics(const xd_theme *t) {
    xd_menu_palette p;
    ops(t)->menu_palette(t, &p);
    return (xd_menu_metrics){
        .border = xd_tpx(t, 3),
        .item_h = xd_tpx(t, 22),
        .large_item_h = xd_tpx(t, 36),
        .icon = xd_tpx(t, 16),
        .large_icon = xd_tpx(t, 32),
        .icon_x = xd_tpx(t, p.flat ? 4 : 3),
        .text_x = xd_tpx(t, 28),
        .large_text_x = xd_tpx(t, 44),
        .arrow_w = xd_tpx(t, 20),
        .separator_h = xd_tpx(t, 9),
        .banner_w = xd_tpx(t, 21),
    };
}

void xd_theme_draw_menu_frame(const xd_theme *t, cairo_t *cr, xd_rect r) {
    xd_menu_palette p;
    ops(t)->menu_palette(t, &p);
    xd_fill_rect(cr, r, p.background);
    int px = xd_tpx(t, 1);
    if (p.flat) {
        xd_draw_bevel(cr, r, px, p.border, p.border);
        return;
    }
    /* EDGE_RAISED, as Windows 2000 draws menus. */
    r = xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_LIGHT),
                      xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
    xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                  xd_sys(t, XD_COLOR_BUTTON_SHADOW));
}

static void draw_submenu_arrow(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rgba color) {
    /* Solid right-pointing triangle, 4 logical px wide, 7 tall. */
    double w = xd_tpx(t, 4);
    double h = xd_tpx(t, 7);
    double x = r.x + r.w - xd_tpx(t, 12);
    double y = r.y + (r.h - h) / 2;
    xd_set_color(cr, color);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + w, y + h / 2);
    cairo_line_to(cr, x, y + h);
    cairo_close_path(cr);
    cairo_fill(cr);
}

void xd_theme_draw_menu_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                             const char *label, bool submenu, bool large) {
    xd_menu_palette p;
    ops(t)->menu_palette(t, &p);
    xd_menu_metrics m = xd_theme_menu_metrics(t);
    bool disabled = state & XD_STATE_DISABLED;
    bool hot = state & XD_STATE_HOT;

    xd_rgba text = p.text;
    if (hot) {
        xd_fill_rect(cr, r, p.hilight);
        if (p.hilight_border.a > 0)
            xd_draw_bevel(cr, r, xd_tpx(t, 1), p.hilight_border, p.hilight_border);
        text = disabled ? p.disabled_text : p.hilight_text;
    } else if (disabled) {
        text = p.disabled_text;
    }

    int tx = r.x + (large ? m.large_text_x : m.text_x);
    xd_rect tr = {tx, r.y, r.x + r.w - tx - m.arrow_w, r.h};
    if (disabled && !hot && p.disabled_emboss.a > 0) {
        int px = xd_tpx(t, 1);
        xd_text_style emboss = xd_text(t, XD_FONT_MENU, p.disabled_emboss, XD_ALIGN_START);
        xd_draw_text(cr, (xd_rect){tr.x + px, tr.y + px, tr.w, tr.h}, label, &emboss);
    }
    xd_text_style st = xd_text(t, XD_FONT_MENU, text, XD_ALIGN_START);
    xd_draw_text(cr, tr, label, &st);
    if (submenu)
        draw_submenu_arrow(t, cr, r, text);
}

void xd_theme_draw_menu_separator(const xd_theme *t, cairo_t *cr, xd_rect r) {
    xd_menu_palette p;
    ops(t)->menu_palette(t, &p);
    int px = xd_tpx(t, 1);
    int inset = p.flat ? xd_tpx(t, 2) : xd_tpx(t, 1);
    int y = r.y + (r.h - (p.separator_light.a > 0 ? 2 * px : px)) / 2;
    xd_fill_rect(cr, (xd_rect){r.x + inset, y, r.w - 2 * inset, px}, p.separator_dark);
    if (p.separator_light.a > 0)
        xd_fill_rect(cr, (xd_rect){r.x + inset, y + px, r.w - 2 * inset, px},
                     p.separator_light);
}

void xd_theme_draw_start_banner(const xd_theme *t, cairo_t *cr, xd_rect r, const char *product,
                                const char *edition) {
    xd_menu_palette p;
    ops(t)->menu_palette(t, &p);
    xd_fill_gradient(cr, r, p.banner_top, p.banner_bottom, false);

    /* Rotate so text runs bottom to top, starting a few px above the
     * banner's foot. */
    xd_font_spec bold = {.size_pt = 14, .bold = true};
    snprintf(bold.family, sizeof(bold.family), "%s", "Tahoma, DejaVu Sans, sans-serif");
    xd_font_spec light = bold;
    light.bold = false;
    xd_text_style st = {.font = &bold, .scale = t->scale, .color = p.banner_text};

    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    cairo_translate(cr, r.x, r.y + r.h);
    cairo_rotate(cr, -XD_PI / 2);
    int x = xd_tpx(t, 6);
    int w1 = xd_text_width(cr, product, &st);
    xd_draw_text(cr, (xd_rect){x, 0, r.h - x, r.w}, product, &st);
    st.font = &light;
    xd_draw_text(cr, (xd_rect){x + w1 + xd_tpx(t, 4), 0, r.h - x - w1, r.w}, edition, &st);
    cairo_restore(cr);
}

int xd_theme_menu_text_width(const xd_theme *t, cairo_t *cr, const char *label) {
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_START);
    return xd_text_width(cr, label, &st);
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
