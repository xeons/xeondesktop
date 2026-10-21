/* SPDX-License-Identifier: MIT
 * The controls beyond the dialog basics (theme.h's "more controls"):
 * horizontal scroll bars, size boxes, progress bars, sliders, spin
 * buttons, column headers, tree glyphs, status bars, tooltips, program
 * toolbars and menu bars. Each comes from the theme package where it
 * styles the control (style_pkg.c), else is drawn here as the style
 * draws it.
 */
#include <math.h>

#include "style.h"

static bool classic(const xd_theme *t) { return t->style == XD_STYLE_CLASSIC; }

static xd_rgba face(const xd_theme *t) { return xd_sys(t, XD_COLOR_BUTTON_FACE); }

/* A raised (or, pressed, sunken) classic button edge around r, filled
 * with ButtonFace; the inside. */
static xd_rect classic_button(const xd_theme *t, cairo_t *cr, xd_rect r, bool pressed) {
    int px = xd_tpx(t, 1);
    xd_fill_rect(cr, r, face(t));
    if (pressed) {
        xd_rgba sh = xd_sys(t, XD_COLOR_BUTTON_SHADOW);
        return xd_draw_bevel(cr, r, px, sh, sh);
    }
    xd_rect in = xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_LIGHT),
                               xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
    return xd_draw_bevel(cr, in, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                         xd_sys(t, XD_COLOR_BUTTON_SHADOW));
}

/* A small solid triangle pointing up or down, 7 by 4 logical px, centred. */
static void triangle(const xd_theme *t, cairo_t *cr, xd_rect r, bool up, xd_rgba c) {
    double w = xd_tpx(t, 7), h = xd_tpx(t, 4);
    double x = r.x + floor((r.w - w) / 2), y = r.y + floor((r.h - h) / 2);
    xd_set_color(cr, c);
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

/* The resize grip's three ridges in r's bottom-right corner (Marlett's
 * size grip): a highlight and two shadow lines each, 12 logical px. */
static void size_grip(const xd_theme *t, cairo_t *cr, xd_rect r) {
    int n = 12;
    int px = xd_tpx(t, 1);
    xd_rgba hi = xd_sys(t, XD_COLOR_BUTTON_HILIGHT), sh = xd_sys(t, XD_COLOR_BUTTON_SHADOW);
    int x0 = r.x + r.w - n * px, y0 = r.y + r.h - n * px;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int d = x + y - (n - 1); /* 0 on the anti-diagonal, growing to the corner */
            if (d < 0 || d >= 11)
                continue;
            int k = d % 4;
            if (k == 3)
                continue;
            xd_fill_rect(cr, (xd_rect){x0 + x * px, y0 + y * px, px, px}, k == 0 ? hi : sh);
        }
}

/* ---- scroll bars ------------------------------------------------------------------- */

/* Swaps x and y, so a vertical scroll bar's drawing lays out a horizontal
 * one: its up and down arrows become left and right. */
static void transpose(cairo_t *cr) {
    cairo_matrix_t m = {.xx = 0, .yx = 1, .xy = 1, .yy = 0, .x0 = 0, .y0 = 0};
    cairo_transform(cr, &m);
}

xd_rect xd_theme_hscrollbar_thumb(const xd_theme *t, xd_rect r, int first, int page, int total) {
    xd_rect v = xd_theme_vscrollbar_thumb(t, (xd_rect){r.y, r.x, r.h, r.w}, first, page, total);
    return (xd_rect){v.y, v.x, v.h, v.w};
}

void xd_theme_draw_hscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, int first, int page,
                              int total) {
    if (xd_pkgstyle_draw_hscrollbar(t, cr, r, xd_theme_hscrollbar_thumb(t, r, first, page, total),
                                    total <= page))
        return;
    cairo_save(cr);
    transpose(cr);
    xd_theme_draw_vscrollbar(t, cr, (xd_rect){r.y, r.x, r.h, r.w}, first, page, total);
    cairo_restore(cr);
}

void xd_theme_draw_sizebox(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (xd_pkgstyle_draw_sizebox(t, cr, r))
        return;
    xd_fill_rect(cr, r, face(t));
    size_grip(t, cr, r);
}

/* ---- progress bars ----------------------------------------------------------------- */

void xd_theme_draw_progress(const xd_theme *t, cairo_t *cr, xd_rect r, double fraction) {
    if (xd_pkgstyle_draw_progress(t, cr, r, fraction))
        return;
    int px = xd_tpx(t, 1);
    xd_rect in;
    xd_rgba chunk_color;
    int chunk, space;
    if (classic(t)) {
        /* Windows 2000: a thin sunken well, Hilight chunks two thirds as
         * wide as the bar is high, a pixel apart from the edge. */
        xd_fill_rect(cr, r, face(t));
        in = xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                           xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
        in = xd_rect_inset(in, px, px);
        chunk_color = xd_sys(t, XD_COLOR_HILIGHT);
        chunk = XD_MAX(in.h * 2 / 3, px);
        space = 2 * px;
    } else {
        /* XP: a white well with a grey border, green chunks. */
        xd_fill_rect(cr, r, xd_rgba_hex(0xFFFFFF));
        xd_rgba border = xd_rgba_hex(0x686868);
        in = xd_rect_inset(xd_draw_bevel(cr, r, px, border, border), 2 * px, 2 * px);
        chunk_color = xd_rgba_hex(0x2ED331);
        chunk = xd_tpx(t, 6);
        space = t->style == XD_STYLE_AERO ? 0 : 2 * px;
    }
    int done = (int)lround(in.w * XD_CLAMP(fraction, 0.0, 1.0));
    cairo_save(cr);
    cairo_rectangle(cr, in.x, in.y, done, in.h);
    cairo_clip(cr);
    for (int x = in.x; x < in.x + done; x += chunk + space)
        xd_fill_rect(cr, (xd_rect){x, in.y, chunk, in.h}, chunk_color);
    cairo_restore(cr);
}

/* ---- sliders ------------------------------------------------------------------------ */

/* A down-pointing trackbar thumb in device pixels. */
static void slider_thumb_path(cairo_t *cr, xd_rect r, double inset) {
    double tip = r.w / 2.0;
    double body = r.h - tip;
    cairo_new_path(cr);
    cairo_move_to(cr, r.x + inset, r.y + inset);
    cairo_line_to(cr, r.x + r.w - inset, r.y + inset);
    cairo_line_to(cr, r.x + r.w - inset, r.y + body);
    cairo_line_to(cr, r.x + r.w / 2.0, r.y + r.h - inset);
    cairo_line_to(cr, r.x + inset, r.y + body);
    cairo_close_path(cr);
}

void xd_theme_draw_trackbar(const xd_theme *t, cairo_t *cr, xd_rect channel, xd_rect thumb,
                            uint8_t state) {
    if (xd_pkgstyle_draw_trackbar(t, cr, channel, thumb, state))
        return;
    xd_toolkit_colors c;
    xd_theme_toolkit_colors(t, &c);
    int px = xd_tpx(t, 1);
    bool enabled = !(state & XD_STATE_DISABLED);

    /* Channel: a sunken groove the thumb rides along. */
    if (c.classic) {
        xd_rect in = xd_draw_bevel(cr, channel, px, c.shadow, c.hilight);
        xd_draw_bevel(cr, in, px, c.dkshadow, c.light);
    } else {
        xd_fill_rect(cr, channel, c.track);
        xd_draw_bevel(cr, channel, px, c.field_border, c.field_border);
    }

    slider_thumb_path(cr, thumb, 0);
    if (c.classic) {
        xd_set_color(cr, c.face);
        cairo_fill(cr);
        /* Light top and left, dark right and point. */
        double h = px / 2.0;
        double body = thumb.h - thumb.w / 2.0;
        cairo_set_line_width(cr, px);
        xd_set_color(cr, c.hilight);
        cairo_move_to(cr, thumb.x + h, thumb.y + body);
        cairo_line_to(cr, thumb.x + h, thumb.y + h);
        cairo_line_to(cr, thumb.x + thumb.w - px, thumb.y + h);
        cairo_stroke(cr);
        xd_set_color(cr, c.dkshadow);
        cairo_move_to(cr, thumb.x + thumb.w - h, thumb.y);
        cairo_line_to(cr, thumb.x + thumb.w - h, thumb.y + body);
        cairo_line_to(cr, thumb.x + thumb.w / 2.0, thumb.y + thumb.h - h);
        cairo_stroke(cr);
        xd_set_color(cr, c.shadow);
        cairo_move_to(cr, thumb.x + thumb.w - px - h, thumb.y + px);
        cairo_line_to(cr, thumb.x + thumb.w - px - h, thumb.y + body - px / 2.0);
        cairo_line_to(cr, thumb.x + thumb.w / 2.0, thumb.y + thumb.h - px - h);
        cairo_stroke(cr);
        xd_set_color(cr, c.hilight);
        cairo_move_to(cr, thumb.x + h, thumb.y + body);
        cairo_line_to(cr, thumb.x + thumb.w / 2.0, thumb.y + thumb.h - h);
        cairo_stroke(cr);
    } else {
        cairo_pattern_t *g = cairo_pattern_create_linear(0, thumb.y, 0, thumb.y + thumb.h);
        xd_rgba top = enabled ? c.thumb_top : c.face;
        xd_rgba bottom = enabled ? c.thumb_bottom : c.face;
        cairo_pattern_add_color_stop_rgba(g, 0, top.r, top.g, top.b, top.a);
        cairo_pattern_add_color_stop_rgba(g, 1, bottom.r, bottom.g, bottom.b, bottom.a);
        cairo_set_source(cr, g);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(g);
        xd_set_color(cr, enabled ? c.button_border : c.disabled);
        cairo_set_line_width(cr, px);
        slider_thumb_path(cr, thumb, px / 2.0);
        cairo_stroke(cr);
    }
}

xd_rgba xd_theme_trackbar_tics_color(const xd_theme *t, bool disabled) {
    xd_rgba c;
    if (!disabled && xd_pkgstyle_trackbar_tics_color(t, &c))
        return c;
    xd_toolkit_colors k;
    xd_theme_toolkit_colors(t, &k);
    return disabled ? k.disabled : k.fg;
}

/* ---- spin buttons -------------------------------------------------------------------- */

void xd_theme_draw_spin_button(const xd_theme *t, cairo_t *cr, xd_rect r, bool up,
                               uint8_t state) {
    if (xd_pkgstyle_draw_spin(t, cr, r, up, state))
        return;
    bool pressed = state & XD_STATE_PRESSED, disabled = state & XD_STATE_DISABLED;
    if (classic(t)) {
        xd_rect in = classic_button(t, cr, r, pressed);
        if (pressed)
            in = (xd_rect){in.x + xd_tpx(t, 1), in.y + xd_tpx(t, 1), in.w, in.h};
        triangle(t, cr, in, up, xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT));
        return;
    }
    /* XP's pale blue arrow buttons, as on drop-downs. */
    uint32_t top = pressed ? 0x6E8DED : (state & XD_STATE_HOT) ? 0xDDECFE : 0xC1D3FB;
    uint32_t bottom = pressed ? 0x8FADF5 : (state & XD_STATE_HOT) ? 0xB6CDFB : 0xA6C0F4;
    if (disabled) {
        top = 0xF0F0F0;
        bottom = 0xE0E0E0;
    }
    int px = xd_tpx(t, 1);
    double rad = xd_tpx(t, 2);
    xd_rect b = xd_rect_inset(r, px, 0);
    xd_path_rounded(cr, b.x, b.y, b.w, b.h, rad, rad, rad, rad);
    cairo_save(cr);
    cairo_clip(cr);
    xd_fill_gradient(cr, b, xd_rgba_hex(top), xd_rgba_hex(bottom), false);
    cairo_restore(cr);
    triangle(t, cr, b, up, xd_rgba_hex(disabled ? 0xC9C7BA : 0x4D6185));
}

/* ---- column headers and tree glyphs ------------------------------------------------- */

void xd_theme_draw_header_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label) {
    if (xd_pkgstyle_draw_header_item(t, cr, r, state, label))
        return;
    int px = xd_tpx(t, 1);
    bool pressed = state & XD_STATE_PRESSED;
    xd_rect text = xd_rect_inset4(r, xd_tpx(t, 6), 0, xd_tpx(t, 6), 0);
    if (classic(t)) {
        classic_button(t, cr, r, pressed);
        if (pressed)
            text.x += px;
    } else {
        /* XP: white to beige, three shaded lines along the bottom (orange
         * when hot), a divider at the right. */
        xd_fill_gradient(cr, r, xd_rgba_hex(pressed ? 0xDEDFD8 : 0xFFFFFF),
                         xd_rgba_hex(pressed ? 0xDEDFD8 : 0xEBEADB), false);
        bool hot = (state & XD_STATE_HOT) && !pressed;
        static const uint32_t bottom[2][3] = {{0xE2DECD, 0xD6D2C2, 0xCBC7B8},
                                              {0xF9B119, 0xF8A90E, 0xFFC83C}};
        for (int i = 0; i < 3; i++)
            xd_fill_rect(cr, (xd_rect){r.x, r.y + r.h - (3 - i) * px, r.w, px},
                         xd_rgba_hex(bottom[hot][i]));
        int inset = xd_tpx(t, 3);
        xd_fill_rect(cr, (xd_rect){r.x + r.w - 2 * px, r.y + inset, px, r.h - 2 * inset},
                     xd_rgba_hex(0xACA899));
        xd_fill_rect(cr, (xd_rect){r.x + r.w - px, r.y + inset, px, r.h - 2 * inset},
                     xd_rgba_hex(0xFFFFFF));
    }
    if (label != nullptr) {
        xd_text_style st = xd_text(t, XD_FONT_MESSAGE, xd_sys(t, XD_COLOR_BUTTON_TEXT),
                                   XD_ALIGN_START);
        xd_draw_text(cr, text, label, &st);
    }
}

void xd_theme_draw_tree_glyph(const xd_theme *t, cairo_t *cr, xd_rect r, bool open) {
    if (xd_pkgstyle_draw_tree_glyph(t, cr, r, open))
        return;
    /* A 9 px box with a plus or minus, as comctl32 draws without a theme. */
    int px = xd_tpx(t, 1), n = xd_tpx(t, 9);
    xd_rect b = {r.x + (r.w - n) / 2, r.y + (r.h - n) / 2, n, n};
    xd_rgba border = classic(t) ? xd_sys(t, XD_COLOR_BUTTON_SHADOW) : xd_rgba_hex(0x919B9C);
    xd_fill_rect(cr, b, xd_sys(t, XD_COLOR_WINDOW));
    xd_draw_bevel(cr, b, px, border, border);
    xd_rgba sign = xd_sys(t, XD_COLOR_WINDOW_TEXT);
    int arm = xd_tpx(t, 2);
    xd_fill_rect(cr, (xd_rect){b.x + arm, b.y + (n - px) / 2, n - 2 * arm, px}, sign);
    if (!open)
        xd_fill_rect(cr, (xd_rect){b.x + (n - px) / 2, b.y + arm, px, n - 2 * arm}, sign);
}

/* ---- status bars and tooltips --------------------------------------------------------- */

void xd_theme_draw_statusbar(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text,
                             bool grip) {
    if (xd_pkgstyle_draw_statusbar(t, cr, r, text, grip))
        return;
    int px = xd_tpx(t, 1);
    xd_fill_rect(cr, r, face(t));
    /* One sunken pane, a little below the top edge, left of the grip. */
    int gw = grip ? xd_tpx(t, 14) : 0;
    xd_rect pane = {r.x, r.y + xd_tpx(t, 2), r.w - gw, r.h - xd_tpx(t, 2)};
    xd_draw_bevel(cr, pane, px, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                  xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    if (grip)
        size_grip(t, cr, r);
    if (text != nullptr) {
        xd_text_style st = xd_text(t, XD_FONT_TOOLTIP, xd_sys(t, XD_COLOR_BUTTON_TEXT),
                                   XD_ALIGN_START);
        xd_draw_text(cr, xd_rect_inset4(pane, xd_tpx(t, 4), 0, xd_tpx(t, 4), 0), text, &st);
    }
}

void xd_theme_draw_tooltip(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text) {
    if (xd_pkgstyle_draw_tooltip(t, cr, r, text))
        return;
    /* The scheme's InfoWindow colours and a border in InfoText. */
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_INFO_WINDOW));
    xd_rgba edge = xd_sys(t, XD_COLOR_INFO_TEXT);
    xd_draw_bevel(cr, r, xd_tpx(t, 1), edge, edge);
    xd_text_style st = xd_text(t, XD_FONT_TOOLTIP, xd_sys(t, XD_COLOR_INFO_TEXT), XD_ALIGN_START);
    xd_draw_text(cr, xd_rect_inset(r, xd_tpx(t, 4), 0), text, &st);
}

/* ---- program toolbars ------------------------------------------------------------------ */

void xd_theme_draw_rebar(const xd_theme *t, cairo_t *cr, xd_rect r, bool gripper) {
    if (xd_pkgstyle_draw_rebar(t, cr, r, gripper))
        return;
    int px = xd_tpx(t, 1);
    xd_fill_rect(cr, r, face(t));
    /* An etched line along the top, as Windows 2000's bands have. */
    xd_fill_rect(cr, (xd_rect){r.x, r.y, r.w, px}, xd_sys(t, XD_COLOR_BUTTON_SHADOW));
    xd_fill_rect(cr, (xd_rect){r.x, r.y + px, r.w, px}, xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    if (gripper) {
        xd_rect bar = {r.x + xd_tpx(t, 2), r.y + xd_tpx(t, 4), xd_tpx(t, 3), r.h - xd_tpx(t, 6)};
        xd_draw_bevel(cr, bar, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(t, XD_COLOR_BUTTON_SHADOW));
    }
}

void xd_theme_draw_tool_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label) {
    if (xd_pkgstyle_draw_tool_button(t, cr, r, state, label))
        return;
    int px = xd_tpx(t, 1);
    bool down = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    if (down) {
        /* Sunken, and a checked button's face dithered lighter. */
        if ((state & XD_STATE_CHECKED) && !(state & XD_STATE_PRESSED))
            xd_fill_rect(cr, r, xd_rgba_mix(face(t), xd_sys(t, XD_COLOR_BUTTON_HILIGHT), 0.5));
        xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                      xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    } else if (state & XD_STATE_HOT) {
        xd_draw_bevel(cr, r, px, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(t, XD_COLOR_BUTTON_SHADOW));
    }
    if (label != nullptr) {
        xd_text_style st = xd_text(
            t, XD_FONT_MESSAGE,
            xd_sys(t, (state & XD_STATE_DISABLED) ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT),
            XD_ALIGN_CENTER);
        xd_rect tr = r;
        if (down) {
            tr.x += px;
            tr.y += px;
        }
        xd_draw_text(cr, tr, label, &st);
    }
}

/* ---- menu bars ---------------------------------------------------------------------- */

void xd_theme_draw_menubar(const xd_theme *t, cairo_t *cr, xd_rect r, bool active) {
    if (xd_pkgstyle_draw_menubar(t, cr, r, active))
        return;
    xd_menu_palette p;
    xd_theme_menu_palette(t, &p);
    /* XP's flat menus give the bar its own colour (MenuBar). */
    xd_fill_rect(cr, r, p.flat ? xd_sys(t, XD_COLOR_MENU_BAR) : xd_sys(t, XD_COLOR_MENU));
}

void xd_theme_draw_menubar_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                const char *label) {
    if (xd_pkgstyle_draw_menubar_item(t, cr, r, state, label))
        return;
    xd_menu_palette p;
    xd_theme_menu_palette(t, &p);
    int px = xd_tpx(t, 1);
    bool disabled = state & XD_STATE_DISABLED;
    bool lit = (state & (XD_STATE_HOT | XD_STATE_PRESSED)) && !disabled;
    xd_rgba text = disabled ? p.disabled_text : p.text;
    xd_rect tr = r;
    if (lit && p.flat) {
        /* XP: the highlight, framed. */
        xd_fill_rect(cr, r, p.hilight);
        if (p.hilight_border.a > 0)
            xd_draw_bevel(cr, r, px, p.hilight_border, p.hilight_border);
        text = p.hilight_text;
    } else if (lit) {
        /* Windows 2000: a thin raised edge when hot, sunken when open. */
        bool open = state & XD_STATE_PRESSED;
        xd_rgba hi = xd_sys(t, XD_COLOR_BUTTON_HILIGHT), sh = xd_sys(t, XD_COLOR_BUTTON_SHADOW);
        xd_draw_bevel(cr, r, px, open ? sh : hi, open ? hi : sh);
        if (open) {
            tr.x += px;
            tr.y += px;
        }
    }
    xd_text_style st = xd_text(t, XD_FONT_MENU, text, XD_ALIGN_CENTER);
    xd_draw_text(cr, tr, label, &st);
}
