/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "style.h"

static const xd_style_ops *const styles[XD_STYLE_COUNT] = {
    [XD_STYLE_CLASSIC] = &xd_style_classic,
    [XD_STYLE_LUNA] = &xd_style_luna,
    [XD_STYLE_WHISTLER] = &xd_style_whistler,
    [XD_STYLE_AERO] = &xd_style_aero,
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
    const char *glass = getenv("XD_GLASS");
    t->glass = glass != nullptr && strcmp(glass, "1") == 0;
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
    if (!f->maximized && !f->dialog) {
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
void xd_theme_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge) {
    ops(t)->draw_taskbar(t, cr, r, edge);
}

xd_rect xd_style_rim_line(xd_rect r, xd_edge edge, int i, int px) {
    switch (edge) {
    case XD_EDGE_TOP:
        return (xd_rect){r.x, r.y + r.h - i - px, r.w, px};
    case XD_EDGE_LEFT:
        return (xd_rect){r.x + r.w - i - px, r.y, px, r.h};
    case XD_EDGE_RIGHT:
        return (xd_rect){r.x + i, r.y, px, r.h};
    case XD_EDGE_BOTTOM:
        break;
    }
    return (xd_rect){r.x, r.y + i, r.w, px};
}
int xd_theme_start_button_width(const xd_theme *t, cairo_t *cr) {
    return ops(t)->start_button_width(t, cr);
}
void xd_theme_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    ops(t)->draw_start_button(t, cr, r, state);
}
void xd_theme_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label, cairo_surface_t *icon) {
    ops(t)->draw_task_button(t, cr, r, state, label, icon);
}

void xd_theme_draw_app_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                              const xd_app_button *b) {
    if (ops(t)->draw_app_button != nullptr)
        ops(t)->draw_app_button(t, cr, r, state, b);
    else
        ops(t)->draw_task_button(t, cr, r, state, b->label != nullptr ? b->label : "", b->icon);
}

void xd_style_task_icon(const xd_theme *t, cairo_t *cr, cairo_surface_t *icon, int x, int y) {
    int size = xd_tpx(t, 16);
    if (icon != nullptr) {
        cairo_save(cr);
        cairo_rectangle(cr, x, y, size, size);
        cairo_clip(cr);
        cairo_set_source_surface(cr, icon, x, y);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }
    /* A generic window: white with a title bar. */
    xd_fill_rect(cr, (xd_rect){x, y, size, size}, xd_rgba_hex(0xFFFFFF));
    xd_fill_rect(cr, (xd_rect){x, y, size, xd_tpx(t, 4)}, xd_rgba_hex(0x0A246A));
    int p = xd_tpx(t, 1);
    xd_draw_bevel(cr, (xd_rect){x, y, size, size}, p, xd_rgba_hex(0x404040),
                  xd_rgba_hex(0x404040));
}
void xd_theme_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock) {
    ops(t)->draw_tray(t, cr, r, clock);
}
void xd_theme_tray_margins(const xd_theme *t, int *left, int *right) {
    /* What each style's draw_tray leaves around the clock. */
    switch (t->style) {
    case XD_STYLE_LUNA:
        *left = xd_tpx(t, 10);
        *right = xd_tpx(t, 13);
        return;
    case XD_STYLE_WHISTLER:
        *left = *right = xd_tpx(t, 10);
        return;
    case XD_STYLE_AERO:
        *left = *right = xd_tpx(t, 8);
        return;
    case XD_STYLE_CLASSIC:
    case XD_STYLE_COUNT:
        break;
    }
    *left = *right = xd_tpx(t, 1) + xd_tpx(t, 6);
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
    xd_theme_draw_menu_item_ex(t, cr, r, state, label, submenu, large, nullptr);
}

void xd_theme_draw_menu_item_ex(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                const char *label, bool submenu, bool large,
                                const xd_menu_item_extras *x) {
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
    xd_font_spec bold = t->scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style st = xd_text(t, XD_FONT_MENU, text, XD_ALIGN_START);
    if (x != nullptr && x->bold)
        st.font = &bold;
    if (x != nullptr && x->accel != nullptr) {
        xd_text_style ac = xd_text(t, XD_FONT_MENU, text, XD_ALIGN_END);
        xd_draw_text(cr, tr, x->accel, &ac);
        tr.w -= xd_text_width(cr, x->accel, &ac) + xd_tpx(t, 12);
    }
    xd_draw_text(cr, tr, label, &st);
    if (submenu)
        draw_submenu_arrow(t, cr, r, text);
    if (x != nullptr && (x->checked || x->glyph != XD_MENU_GLYPH_NONE)) {
        int box = xd_tpx(t, 16);
        xd_rect ic = {r.x + m.icon_x, r.y + (r.h - box) / 2, box, box};
        static const xd_glyph map[] = {
            [XD_MENU_GLYPH_RESTORE] = XD_GLYPH_RESTORE,
            [XD_MENU_GLYPH_MINIMIZE] = XD_GLYPH_MINIMIZE,
            [XD_MENU_GLYPH_MAXIMIZE] = XD_GLYPH_MAXIMIZE,
            [XD_MENU_GLYPH_CLOSE] = XD_GLYPH_CLOSE,
        };
        xd_glyph g = x->checked ? XD_GLYPH_CHECK : map[x->glyph];
        /* Window glyphs are drawn small, as in a window menu. */
        xd_rect gr = x->checked ? ic : xd_rect_inset(ic, xd_tpx(t, 3), xd_tpx(t, 3));
        xd_draw_glyph(t, cr, gr, g, text, 1.0);
    }
}

int xd_theme_menu_bold_text_width(const xd_theme *t, cairo_t *cr, const char *label) {
    xd_font_spec bold = t->scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_START);
    st.font = &bold;
    return xd_text_width(cr, label, &st);
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
    snprintf(bold.family, sizeof(bold.family), "%s", XD_FONT_FAMILY_TAHOMA);
    xd_font_spec light = bold;
    light.bold = false;
    xd_text_style st = {.font = &bold,
                        .scale = t->scale,
                        .color = p.banner_text,
                        .smoothing = t->scheme.font_smoothing};

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

void xd_theme_toolkit_colors(const xd_theme *t, xd_toolkit_colors *out) {
    *out = (xd_toolkit_colors){0};
    ops(t)->toolkit_colors(t, out);
}

/* ---- taskbar toolbars ---------------------------------------------------------- */

int xd_theme_toolbar_grip_width(const xd_theme *t) {
    return t->style == XD_STYLE_CLASSIC ? xd_tpx(t, 6) : xd_tpx(t, 8);
}

void xd_theme_draw_toolbar_grip(const xd_theme *t, cairo_t *cr, xd_rect r) {
    int px = xd_tpx(t, 1);
    if (t->style == XD_STYLE_CLASSIC) {
        /* Windows 2000: a thin raised bar, inset from top and bottom. */
        xd_rect bar = {r.x + xd_tpx(t, 2), r.y + xd_tpx(t, 2), xd_tpx(t, 3), r.h - xd_tpx(t, 4)};
        xd_draw_bevel(cr, bar, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(t, XD_COLOR_BUTTON_SHADOW));
        return;
    }
    /* XP: a column of dots with highlights. */
    int dot = XD_MAX(px * 2, 2);
    int x = r.x + (r.w - dot) / 2;
    for (int y = r.y + xd_tpx(t, 5); y + dot <= r.y + r.h - xd_tpx(t, 4); y += dot * 2) {
        xd_fill_rect(cr, (xd_rect){x + px, y + px, dot, dot}, xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.6));
        xd_fill_rect(cr, (xd_rect){x, y, dot, dot}, xd_rgba_alpha(xd_rgba_hex(0x000000), 0.45));
    }
}

void xd_theme_draw_toolbar_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    bool hot = state & XD_STATE_HOT;
    bool pressed = state & XD_STATE_PRESSED;
    if (!hot && !pressed)
        return;
    int px = xd_tpx(t, 1);
    if (t->style == XD_STYLE_CLASSIC) {
        /* Windows 2000 flat buttons: a 1px edge appears on hover. */
        if (pressed)
            xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                          xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
        else
            xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                          xd_sys(t, XD_COLOR_BUTTON_SHADOW));
        return;
    }
    double rad = xd_tpx(t, 2);
    xd_path_rounded(cr, r.x, r.y, r.w, r.h, rad, rad, rad, rad);
    xd_set_color(cr, pressed ? xd_rgba_alpha(xd_rgba_hex(0x000000), 0.25)
                             : xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.3));
    cairo_fill_preserve(cr);
    xd_set_color(cr, xd_rgba_alpha(xd_rgba_hex(pressed ? 0x000000 : 0xFFFFFF), 0.5));
    cairo_set_line_width(cr, px);
    cairo_stroke(cr);
}

/* ---- dialog controls -------------------------------------------------------- */

static bool classic(const xd_theme *t) { return t->style == XD_STYLE_CLASSIC; }
static bool watercolor(const xd_theme *t) { return t->style == XD_STYLE_WHISTLER; }

/* XP's flat control border colour per style. */
static xd_rgba flat_border(const xd_theme *t) {
    return xd_rgba_hex(watercolor(t) ? 0x646464 : 0x7F9DB9);
}

/* Whistler's flat face, tab and page colours. */
static const uint32_t WC_FACE = 0xEBEBE4, WC_TAB = 0xDBDBCE, WC_PAGE = 0xF3F3EF,
                      WC_THUMB_BORDER = 0xC1C1BB;

xd_field_colors xd_theme_field_colors(const xd_theme *t) {
    return (xd_field_colors){
        .text = xd_sys(t, XD_COLOR_WINDOW_TEXT),
        .disabled_text = xd_sys(t, XD_COLOR_GRAY_TEXT),
        .selection = watercolor(t) ? xd_rgba_hex(0x316AC5) : xd_sys(t, XD_COLOR_HILIGHT),
        .selection_text = watercolor(t) ? xd_rgba_hex(0xFFFFFF) : xd_sys(t, XD_COLOR_HILIGHT_TEXT),
    };
}

xd_rect xd_theme_draw_field(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled) {
    int px = xd_tpx(t, 1);
    xd_rgba fill = disabled ? xd_sys(t, XD_COLOR_BUTTON_FACE) : xd_sys(t, XD_COLOR_WINDOW);
    if (!classic(t)) {
        xd_fill_rect(cr, r, disabled ? xd_rgba_hex(WC_FACE) : xd_sys(t, XD_COLOR_WINDOW));
        return xd_draw_bevel(cr, r, px, flat_border(t), flat_border(t));
    }
    /* EDGE_SUNKEN */
    r = xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                      xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    r = xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW),
                      xd_sys(t, XD_COLOR_BUTTON_LIGHT));
    xd_fill_rect(cr, r, fill);
    return r;
}

int xd_theme_dropdown_button_width(const xd_theme *t) {
    return classic(t) ? xd_tpx(t, 16) : xd_tpx(t, 17);
}

void xd_theme_draw_dropdown_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    bool pressed = state & XD_STATE_PRESSED;
    bool disabled = state & XD_STATE_DISABLED;
    int px = xd_tpx(t, 1);
    xd_rgba arrow = xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT);

    if (watercolor(t)) {
        /* Just a black arrow on the face, inside the field's border. */
        xd_fill_rect(cr, r, xd_rgba_hex(pressed ? 0xD8DAD4 : WC_FACE));
        arrow = xd_rgba_hex(disabled ? 0xA0A0A0 : 0x000000);
    } else if (classic(t)) {
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
        if (pressed) {
            xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                          xd_sys(t, XD_COLOR_BUTTON_SHADOW));
            r = (xd_rect){r.x + px, r.y + px, r.w, r.h};
        } else {
            xd_rect in = xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_LIGHT),
                                       xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
            xd_draw_bevel(cr, in, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                          xd_sys(t, XD_COLOR_BUTTON_SHADOW));
        }
    } else {
        /* XP's pale blue rounded arrow button. */
        uint32_t top = pressed ? 0x6E8DED : (state & XD_STATE_HOT) ? 0xDDECFE : 0xC1D3FB;
        uint32_t bottom = pressed ? 0x8FADF5 : (state & XD_STATE_HOT) ? 0xB6CDFB : 0xA6C0F4;
        if (disabled) {
            top = 0xF0F0F0;
            bottom = 0xE0E0E0;
        }
        double rad = xd_tpx(t, 2);
        xd_rect b = xd_rect_inset(r, px, px);
        xd_path_rounded(cr, b.x, b.y, b.w, b.h, rad, rad, rad, rad);
        cairo_save(cr);
        cairo_clip(cr);
        xd_fill_gradient(cr, b, xd_rgba_hex(top), xd_rgba_hex(bottom), false);
        cairo_restore(cr);
        arrow = xd_rgba_hex(disabled ? 0xC9C7BA : 0x4D6185);
    }

    /* Down-pointing triangle, 7 logical px wide. */
    double w = xd_tpx(t, 7);
    double h = xd_tpx(t, 4);
    double x = r.x + (r.w - w) / 2;
    double y = r.y + (r.h - h) / 2;
    xd_set_color(cr, arrow);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + w, y);
    cairo_line_to(cr, x + w / 2, y + h);
    cairo_close_path(cr);
    cairo_fill(cr);
}

void xd_theme_draw_list_item(const xd_theme *t, cairo_t *cr, xd_rect r, bool selected,
                             const char *label) {
    xd_field_colors c = xd_theme_field_colors(t);
    if (selected)
        xd_fill_rect(cr, r, c.selection);
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE, selected ? c.selection_text : c.text,
                               XD_ALIGN_START);
    xd_draw_text(cr, xd_rect_inset4(r, xd_tpx(t, 2), 0, xd_tpx(t, 2), 0), label, &st);
}

void xd_theme_draw_focus_rect(const xd_theme *t, cairo_t *cr, xd_rect r) {
    double p = xd_tpx(t, 1);
    double dash[] = {p, p};
    cairo_save(cr);
    xd_set_color(cr, xd_sys(t, XD_COLOR_BUTTON_TEXT));
    cairo_set_line_width(cr, p);
    cairo_set_dash(cr, dash, 2, 0);
    cairo_rectangle(cr, r.x + p / 2, r.y + p / 2, r.w - p, r.h - p);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* Classic's 12x12 radio button, as DrawFrameControl draws it from Marlett's
 * pieces: an outer ring of Shadow over Hilight, an inner one of DkShadow
 * over Light (top-left over bottom-right), the well, and a 4x4 dot. */
static const char *const radio_rows[12] = {
    "....SSSS....", "..SSDDDDSS..", ".SDDWWWWLLH.", ".SDWWWWWWLH.",
    "SDWWWWWWWWLH", "SDWWWWWWWWLH", "SDWWWWWWWWLH", "SDWWWWWWWWLH",
    ".SDWWWWWWLH.", ".SLLWWWWLLH.", "..HHLLLLHH..", "....HHHH....",
};
static const char *const radio_dot[4] = {".##.", "####", "####", ".##."};

static void classic_radio(const xd_theme *t, cairo_t *cr, xd_rect b, uint8_t state) {
    bool disabled = state & XD_STATE_DISABLED;
    xd_rgba well = xd_sys(t, (disabled || (state & XD_STATE_PRESSED)) ? XD_COLOR_BUTTON_FACE
                                                                     : XD_COLOR_WINDOW);
    xd_rgba dot = xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT);
    double k = b.w / 12.0;
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            xd_rgba c;
            switch (radio_rows[y][x]) {
            case 'S':
                c = xd_sys(t, XD_COLOR_BUTTON_SHADOW);
                break;
            case 'D':
                c = xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW);
                break;
            case 'H':
                c = xd_sys(t, XD_COLOR_BUTTON_HILIGHT);
                break;
            case 'L':
                c = xd_sys(t, XD_COLOR_BUTTON_LIGHT);
                break;
            case 'W':
                c = well;
                if ((state & XD_STATE_CHECKED) && x >= 4 && x < 8 && y >= 4 && y < 8 &&
                    radio_dot[y - 4][x - 4] == '#')
                    c = dot;
                break;
            default:
                continue;
            }
            /* Each design pixel covers whole device pixels, edges shared. */
            int x0 = b.x + (int)lround(x * k);
            int y0 = b.y + (int)lround(y * k);
            int x1 = b.x + (int)lround((x + 1) * k);
            int y1 = b.y + (int)lround((y + 1) * k);
            xd_fill_rect(cr, (xd_rect){x0, y0, x1 - x0, y1 - y0}, c);
        }
    }
}

/* The flat styles' 13x13 round button: the check box's border and shading
 * (XP's light-grey to white), its hover ring, and a dot in the check mark's
 * colour. */
static void flat_radio(const xd_theme *t, cairo_t *cr, xd_rect b, uint8_t state) {
    xd_toolkit_colors c;
    xd_theme_toolkit_colors(t, &c);
    bool disabled = state & XD_STATE_DISABLED;
    double px = xd_tpx(t, 1);
    double cx = b.x + b.w / 2.0;
    double cy = b.y + b.h / 2.0;
    double r = b.w / 2.0;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r - px / 2, 0, 2 * XD_PI);
    if (disabled) {
        xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
        cairo_fill_preserve(cr);
    } else {
        xd_rgba top = xd_rgba_hex((state & XD_STATE_PRESSED) ? 0xB0B0A7 : 0xDCDCD7);
        cairo_pattern_t *g = cairo_pattern_create_linear(b.x, b.y, b.x + b.w, b.y + b.h);
        cairo_pattern_add_color_stop_rgba(g, 0, top.r, top.g, top.b, 1);
        cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 1);
        cairo_set_source(cr, g);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(g);
    }
    xd_set_color(cr, disabled ? c.disabled : c.check_border);
    cairo_set_line_width(cr, px);
    cairo_stroke(cr);
    if (!disabled && (state & XD_STATE_HOT)) {
        cairo_arc(cr, cx, cy, r - 2 * px, 0, 2 * XD_PI);
        xd_set_color(cr, c.button_hot);
        cairo_set_line_width(cr, 2 * px);
        cairo_stroke(cr);
    }
    if (state & XD_STATE_CHECKED) {
        cairo_arc(cr, cx, cy, 2.5 * px, 0, 2 * XD_PI);
        xd_set_color(cr, disabled ? c.disabled : c.check_mark);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

void xd_theme_draw_radio(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                         const char *label) {
    /* The button sits where a check box's box does, so their labels line up. */
    int slot = xd_tpx(t, 13);
    bool disabled = state & XD_STATE_DISABLED;
    if (classic(t)) {
        int box = xd_tpx(t, 12);
        classic_radio(t, cr, (xd_rect){r.x, r.y + (r.h - box) / 2, box, box}, state);
    } else {
        flat_radio(t, cr, (xd_rect){r.x, r.y + (r.h - slot) / 2, slot, slot}, state);
    }
    xd_rgba fg = xd_sys(t, XD_COLOR_WINDOW_TEXT);
    if (disabled) {
        xd_toolkit_colors c;
        xd_theme_toolkit_colors(t, &c);
        fg = classic(t) ? xd_sys(t, XD_COLOR_GRAY_TEXT) : c.disabled;
    }
    int gap = xd_tpx(t, 5);
    xd_rect text = {r.x + slot + gap, r.y, r.w - slot - gap, r.h};
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
    xd_draw_text(cr, text, label, &st);
    if (state & XD_STATE_FOCUSED) {
        int w = XD_MIN(xd_text_width(cr, label, &st), text.w);
        xd_theme_draw_focus_rect(t, cr,
                                 (xd_rect){text.x - xd_tpx(t, 1), r.y + xd_tpx(t, 2),
                                           w + xd_tpx(t, 3), r.h - xd_tpx(t, 4)});
    }
}

void xd_theme_draw_label(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text, bool wrap,
                         bool bold, bool disabled) {
    xd_font_spec font = t->scheme.fonts[XD_FONT_MESSAGE];
    font.bold = bold;
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    st.font = &font;
    st.wrap = wrap;
    st.ellipsize = !wrap;
    xd_draw_text(cr, r, text, &st);
}

void xd_theme_draw_tab(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                       const char *label) {
    int px = xd_tpx(t, 1);
    bool selected = state & XD_STATE_CHECKED;
    xd_rect tab = selected ? r : xd_rect_inset4(r, 0, xd_tpx(t, 2), 0, 0);
    if (classic(t)) {
        /* Raised edge on top/left/right, open at the bottom. */
        xd_fill_rect(cr, tab, xd_sys(t, XD_COLOR_BUTTON_FACE));
        xd_fill_rect(cr, (xd_rect){tab.x + 2 * px, tab.y, tab.w - 4 * px, px},
                     xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
        xd_fill_rect(cr, (xd_rect){tab.x + px, tab.y + px, px, px},
                     xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
        xd_fill_rect(cr, (xd_rect){tab.x, tab.y + 2 * px, px, tab.h - 2 * px},
                     xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
        xd_fill_rect(cr, (xd_rect){tab.x + tab.w - 2 * px, tab.y + px, px, tab.h - px},
                     xd_sys(t, XD_COLOR_BUTTON_SHADOW));
        xd_fill_rect(cr, (xd_rect){tab.x + tab.w - px, tab.y + 2 * px, px, tab.h - 2 * px},
                     xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
    } else if (watercolor(t)) {
        /* Flat: darker unselected tabs split by short white lines; the
         * selected one is lighter with a white edge, joining the page. */
        xd_fill_rect(cr, tab, xd_rgba_hex(selected ? WC_PAGE : WC_TAB));
        if (selected) {
            xd_rgba white = xd_rgba_hex(0xFFFFFF);
            xd_fill_rect(cr, (xd_rect){tab.x, tab.y, tab.w, px}, white);
            xd_fill_rect(cr, (xd_rect){tab.x, tab.y, px, tab.h}, white);
            xd_fill_rect(cr, (xd_rect){tab.x + tab.w - px, tab.y, px, tab.h}, white);
        } else {
            xd_fill_rect(cr, (xd_rect){tab.x + tab.w - px, tab.y + xd_tpx(t, 4), px,
                                       tab.h - xd_tpx(t, 8)},
                         xd_rgba_hex(0xFFFFFF));
        }
    } else {
        /* XP: white-to-beige tab, orange top line when hot or selected. */
        xd_rgba border = xd_rgba_hex(0x919B9C);
        xd_fill_gradient(cr, tab, xd_rgba_hex(0xFFFFFF),
                         selected ? xd_rgba_hex(0xFCFCFE) : xd_rgba_hex(0xECEBE6), false);
        xd_fill_rect(cr, (xd_rect){tab.x, tab.y, tab.w, px}, border);
        xd_fill_rect(cr, (xd_rect){tab.x, tab.y, px, tab.h}, border);
        xd_fill_rect(cr, (xd_rect){tab.x + tab.w - px, tab.y, px, tab.h}, border);
        if (selected || (state & XD_STATE_HOT)) {
            xd_fill_rect(cr, (xd_rect){tab.x, tab.y, tab.w, 2 * px}, xd_rgba_hex(0xFFC83C));
            xd_fill_rect(cr, (xd_rect){tab.x, tab.y + 2 * px, tab.w, px},
                         xd_rgba_hex(0xE6A000));
        }
    }
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE, xd_sys(t, XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_CENTER);
    xd_rect text = xd_rect_inset4(tab, xd_tpx(t, 4), xd_tpx(t, 1), xd_tpx(t, 4), 0);
    xd_draw_text(cr, text, label, &st);
    if (state & XD_STATE_FOCUSED) {
        int w = XD_MIN(xd_text_width(cr, label, &st) + xd_tpx(t, 4), text.w);
        xd_theme_draw_focus_rect(t, cr,
                                 (xd_rect){text.x + (text.w - w) / 2, text.y + xd_tpx(t, 2), w,
                                           text.h - xd_tpx(t, 4)});
    }
}

xd_rgba xd_theme_tab_page_color(const xd_theme *t) {
    if (watercolor(t))
        return xd_rgba_hex(WC_PAGE);
    return classic(t) ? xd_sys(t, XD_COLOR_BUTTON_FACE) : xd_rgba_hex(0xFCFCFE);
}

void xd_theme_draw_tab_panel(const xd_theme *t, cairo_t *cr, xd_rect r) {
    int px = xd_tpx(t, 1);
    if (classic(t)) {
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
        xd_rect in = xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                                   xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
        xd_draw_bevel(cr, in, px, xd_sys(t, XD_COLOR_BUTTON_FACE),
                      xd_sys(t, XD_COLOR_BUTTON_SHADOW));
        return;
    }
    if (watercolor(t)) {
        xd_fill_rect(cr, r, xd_rgba_hex(WC_PAGE));
        xd_draw_bevel(cr, r, px, xd_rgba_hex(0xFFFFFF), xd_rgba_hex(WC_THUMB_BORDER));
        return;
    }
    xd_rgba border = xd_rgba_hex(0x919B9C);
    xd_fill_gradient(cr, r, xd_rgba_hex(0xFCFCFE), xd_rgba_hex(0xF4F3EE), false);
    xd_draw_bevel(cr, r, px, border, border);
}

void xd_theme_draw_groupbox(const xd_theme *t, cairo_t *cr, xd_rect r, const char *label) {
    int px = xd_tpx(t, 1);
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               classic(t) || watercolor(t) ? xd_sys(t, XD_COLOR_BUTTON_TEXT)
                                                           : xd_rgba_hex(0x0046D5),
                               XD_ALIGN_START);
    int th = xd_tpx(t, 14);
    int top = r.y + th / 2;
    xd_rect box = {r.x, top, r.w, r.h - (top - r.y)};
    if (classic(t)) {
        /* Etched: shadow line with a highlight line inside it. */
        xd_rgba sh = xd_sys(t, XD_COLOR_BUTTON_SHADOW);
        xd_rgba hi = xd_sys(t, XD_COLOR_BUTTON_HILIGHT);
        xd_draw_bevel(cr, (xd_rect){box.x + px, box.y + px, box.w - px, box.h - px}, px, hi, hi);
        xd_draw_bevel(cr, (xd_rect){box.x, box.y, box.w - px, box.h - px}, px, sh, sh);
    } else if (watercolor(t)) {
        xd_rgba border = xd_rgba_hex(WC_THUMB_BORDER);
        xd_draw_bevel(cr, box, px, border, border);
    } else {
        xd_rgba border = xd_rgba_hex(0xD0D0BF);
        double rad = xd_tpx(t, 3);
        xd_path_rounded(cr, box.x + px / 2.0, box.y + px / 2.0, box.w - px, box.h - px, rad, rad,
                        rad, rad);
        xd_set_color(cr, border);
        cairo_set_line_width(cr, px);
        cairo_stroke(cr);
    }
    if (label != nullptr && label[0] != '\0') {
        int tw = xd_text_width(cr, label, &st);
        xd_rect gap = {r.x + xd_tpx(t, 7), r.y, tw + xd_tpx(t, 4), th};
        xd_fill_rect(cr, gap, classic(t) ? xd_sys(t, XD_COLOR_BUTTON_FACE)
                                         : xd_theme_tab_page_color(t));
        xd_draw_text(cr, xd_rect_inset4(gap, xd_tpx(t, 2), 0, 0, 0), label, &st);
    }
}

int xd_theme_scrollbar_width(const xd_theme *t) { return xd_metric_px(t, XD_METRIC_SCROLL_WIDTH); }

xd_rect xd_theme_vscrollbar_thumb(const xd_theme *t, xd_rect r, int first, int page, int total) {
    int arrow = XD_MIN(r.w, r.h / 2);
    xd_rect track = {r.x, r.y + arrow, r.w, r.h - 2 * arrow};
    if (total <= page || total <= 0 || track.h <= 0)
        return (xd_rect){track.x, track.y, track.w, 0};
    int min_thumb = xd_tpx(t, 8);
    int th = XD_MAX((int)((long)track.h * page / total), min_thumb);
    th = XD_MIN(th, track.h);
    int range = total - page;
    int y = track.y + (int)((long)(track.h - th) * XD_CLAMP(first, 0, range) / range);
    return (xd_rect){track.x, y, track.w, th};
}

static void scroll_arrow(const xd_theme *t, cairo_t *cr, xd_rect r, bool up, bool disabled) {
    double w = xd_tpx(t, 7);
    double h = xd_tpx(t, 4);
    double x = r.x + (r.w - w) / 2;
    double y = r.y + (r.h - h) / 2;
    xd_set_color(cr, classic(t)      ? xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT)
                     : watercolor(t) ? xd_rgba_hex(disabled ? 0xA0A0A0 : 0x000000)
                                     : xd_rgba_hex(disabled ? 0xC9C7BA : 0x4D6185));
    if (up) {
        cairo_move_to(cr, x + w / 2, y);
        cairo_line_to(cr, x + w, y + h);
        cairo_line_to(cr, x, y + h);
    } else {
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x + w, y);
        cairo_line_to(cr, x + w / 2, y + h);
    }
    cairo_close_path(cr);
    cairo_fill(cr);
}

/* Watercolor's arrows have no button; only the thumb is drawn: white with
 * a two-pixel grey border. */
static void scroll_button(const xd_theme *t, cairo_t *cr, xd_rect r, bool thumb) {
    int px = xd_tpx(t, 1);
    if (watercolor(t)) {
        if (thumb) {
            xd_fill_rect(cr, r, xd_rgba_hex(0xFFFFFF));
            xd_draw_bevel(cr, xd_rect_inset4(r, px, 0, px, 0), 2 * px,
                          xd_rgba_hex(WC_THUMB_BORDER), xd_rgba_hex(WC_THUMB_BORDER));
        }
        return;
    }
    if (classic(t)) {
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
        xd_rect in = xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_LIGHT),
                                   xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
        xd_draw_bevel(cr, in, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(t, XD_COLOR_BUTTON_SHADOW));
    } else {
        double rad = xd_tpx(t, 2);
        xd_rect b = xd_rect_inset(r, px, px);
        xd_path_rounded(cr, b.x, b.y, b.w, b.h, rad, rad, rad, rad);
        cairo_save(cr);
        cairo_clip(cr);
        xd_fill_gradient(cr, b, xd_rgba_hex(0xC8D6FB), xd_rgba_hex(0xB0C8F8), true);
        cairo_restore(cr);
        xd_path_rounded(cr, b.x + px / 2.0, b.y + px / 2.0, b.w - px, b.h - px, rad, rad, rad,
                        rad);
        xd_set_color(cr, xd_rgba_hex(0xFFFFFF));
        cairo_set_line_width(cr, px);
        cairo_stroke(cr);
    }
}

void xd_theme_draw_vscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, int first, int page,
                              int total) {
    int arrow = XD_MIN(r.w, r.h / 2);
    bool disabled = total <= page;
    /* Track: Classic dithers ButtonHilight over Scrollbar; XP is flat. */
    if (classic(t)) {
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_SCROLLBAR));
        xd_fill_rect(cr, r, xd_rgba_alpha(xd_sys(t, XD_COLOR_BUTTON_HILIGHT), 0.5));
    } else {
        xd_fill_rect(cr, r, xd_rgba_hex(watercolor(t) ? WC_FACE : 0xF3F1EC));
    }
    xd_rect up = {r.x, r.y, r.w, arrow};
    xd_rect down = {r.x, r.y + r.h - arrow, r.w, arrow};
    scroll_button(t, cr, up, false);
    scroll_arrow(t, cr, up, true, disabled);
    scroll_button(t, cr, down, false);
    scroll_arrow(t, cr, down, false, disabled);
    xd_rect thumb = xd_theme_vscrollbar_thumb(t, r, first, page, total);
    if (thumb.h > 0)
        scroll_button(t, cr, thumb, true);
}

void xd_theme_draw_dialog_banner(const xd_theme *t, cairo_t *cr, xd_rect r, const char *product,
                                 const char *edition) {
    xd_menu_palette p;
    ops(t)->menu_palette(t, &p);
    xd_fill_gradient(cr, r, p.banner_bottom, p.banner_top, true);

    int flag = XD_MIN(xd_tpx(t, 48), r.h - xd_tpx(t, 10));
    int fx = r.x + xd_tpx(t, 16);
    xd_draw_start_logo(cr, (xd_rect){fx, r.y + (r.h - flag) / 2, flag, flag});

    xd_font_spec big = {.size_pt = 18, .bold = true};
    snprintf(big.family, sizeof(big.family), "%s", XD_FONT_FAMILY_TAHOMA);
    xd_font_spec small = big;
    small.size_pt = 10;
    small.bold = false;
    xd_text_style st = {.font = &big,
                        .scale = t->scale,
                        .color = p.banner_text,
                        .smoothing = t->scheme.font_smoothing};
    int tx = fx + flag + xd_tpx(t, 12);
    int line = xd_tpx(t, 28);
    int top = r.y + (r.h - line - xd_tpx(t, 16)) / 2;
    xd_draw_text(cr, (xd_rect){tx, top, r.x + r.w - tx, line}, product, &st);
    st.font = &small;
    xd_draw_text(cr, (xd_rect){tx + xd_tpx(t, 2), top + line, r.x + r.w - tx, xd_tpx(t, 16)},
                 edition, &st);
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
        .smoothing = t->scheme.font_smoothing,
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

static void ellipse(cairo_t *cr, double cx, double cy, double rx, double ry) {
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0, 0, 1, 0, 2 * XD_PI);
    cairo_restore(cr);
}

void xd_draw_start_logo(cairo_t *cr, xd_rect r) {
    /* Tux, drawn from simple shapes so it stays readable at 16px. Shapes
     * are laid out on a unit square and scaled to fit r. */
    double s = XD_MIN(r.w, r.h);
    if (s <= 0)
        return;
    cairo_save(cr);
    cairo_translate(cr, r.x + (r.w - s) / 2, r.y + (r.h - s) / 2);
    cairo_scale(cr, s, s);

    const xd_rgba black = xd_rgba_hex(0x1A1A1A);
    const xd_rgba white = xd_rgba_hex(0xFFFFFF);
    const xd_rgba orange = xd_rgba_hex(0xF2B01E);
    const xd_rgba orange_dark = xd_rgba_hex(0xC88400);

    /* Feet behind the body. */
    xd_set_color(cr, orange);
    ellipse(cr, 0.33, 0.92, 0.15, 0.065);
    ellipse(cr, 0.67, 0.92, 0.15, 0.065);
    cairo_fill(cr);

    /* Body and head. */
    xd_set_color(cr, black);
    ellipse(cr, 0.5, 0.6, 0.32, 0.34);
    cairo_fill(cr);
    ellipse(cr, 0.5, 0.27, 0.2, 0.22);
    cairo_fill(cr);
    /* Flippers. */
    ellipse(cr, 0.21, 0.62, 0.08, 0.2);
    ellipse(cr, 0.79, 0.62, 0.08, 0.2);
    cairo_fill(cr);

    /* White belly and face. */
    xd_set_color(cr, white);
    ellipse(cr, 0.5, 0.65, 0.21, 0.26);
    cairo_fill(cr);
    ellipse(cr, 0.5, 0.31, 0.13, 0.11);
    cairo_fill(cr);

    /* Eyes. */
    ellipse(cr, 0.43, 0.22, 0.055, 0.07);
    ellipse(cr, 0.57, 0.22, 0.055, 0.07);
    cairo_fill(cr);
    xd_set_color(cr, black);
    ellipse(cr, 0.445, 0.235, 0.028, 0.035);
    ellipse(cr, 0.555, 0.235, 0.028, 0.035);
    cairo_fill(cr);

    /* Beak: two halves so it reads as a beak, not a blob. */
    xd_set_color(cr, orange);
    ellipse(cr, 0.5, 0.315, 0.11, 0.05);
    cairo_fill(cr);
    xd_set_color(cr, orange_dark);
    ellipse(cr, 0.5, 0.345, 0.075, 0.03);
    cairo_fill(cr);
    cairo_restore(cr);
}
