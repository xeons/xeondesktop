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

static bool classic(const xd_theme *theme) { return theme->style == XD_STYLE_CLASSIC; }

static xd_rgba face(const xd_theme *theme) { return xd_sys(theme, XD_COLOR_BUTTON_FACE); }

/* A raised (or, pressed, sunken) classic button edge around r, filled
 * with ButtonFace; the inside. */
static xd_rect classic_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool pressed) {
    int px = xd_tpx(theme, 1);
    xd_fill_rect(cairo, rect, face(theme));
    if (pressed) {
        xd_rgba sh = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
        return xd_draw_bevel(cairo, rect, px, sh, sh);
    }
    xd_rect in = xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
                               xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
    return xd_draw_bevel(cairo, in, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                         xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
}

/* A small solid triangle pointing up or down, 7 by 4 logical px, centred. */
static void triangle(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool up, xd_rgba color) {
    double width = xd_tpx(theme, 7), height = xd_tpx(theme, 4);
    double x = rect.x + floor((rect.w - width) / 2), y = rect.y + floor((rect.h - height) / 2);
    xd_set_color(cairo, color);
    if (up) {
        cairo_move_to(cairo, x + width / 2, y);
        cairo_line_to(cairo, x + width, y + height);
        cairo_line_to(cairo, x, y + height);
    } else {
        cairo_move_to(cairo, x, y);
        cairo_line_to(cairo, x + width, y);
        cairo_line_to(cairo, x + width / 2, y + height);
    }
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

/* The resize grip's three ridges in r's bottom-right corner (Marlett's
 * size grip): a highlight and two shadow lines each, 12 logical px. */
static void size_grip(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    int grid = 12;
    int px = xd_tpx(theme, 1);
    xd_rgba hi = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT), sh = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
    int x0 = rect.x + rect.w - grid * px, y0 = rect.y + rect.h - grid * px;
    for (int y = 0; y < grid; y++)
        for (int x = 0; x < grid; x++) {
            int diagonal = x + y - (grid - 1); /* 0 on the anti-diagonal, growing to the corner */
            if (diagonal < 0 || diagonal >= 11)
                continue;
            int k = diagonal % 4;
            if (k == 3)
                continue;
            xd_fill_rect(cairo, (xd_rect){x0 + x * px, y0 + y * px, px, px}, k == 0 ? hi : sh);
        }
}

/* ---- scroll bars ------------------------------------------------------------------- */

/* Swaps x and y, so a vertical scroll bar's drawing lays out a horizontal
 * one: its up and down arrows become left and right. */
static void transpose(cairo_t *cairo) {
    cairo_matrix_t matrix = {.xx = 0, .yx = 1, .xy = 1, .yy = 0, .x0 = 0, .y0 = 0};
    cairo_transform(cairo, &matrix);
}

xd_rect xd_theme_hscrollbar_thumb(const xd_theme *theme, xd_rect rect, int first, int page,
                                  int total) {
    xd_rect vertical = xd_theme_vscrollbar_thumb(theme, (xd_rect){rect.y, rect.x, rect.h, rect.w},
                                                 first, page, total);
    return (xd_rect){vertical.y, vertical.x, vertical.h, vertical.w};
}

void xd_theme_draw_hscrollbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, int first,
                              int page, int total) {
    if (xd_pkgstyle_draw_hscrollbar(theme, cairo, rect,
                                    xd_theme_hscrollbar_thumb(theme, rect, first, page, total),
                                    total <= page))
        return;
    cairo_save(cairo);
    transpose(cairo);
    xd_theme_draw_vscrollbar(theme, cairo, (xd_rect){rect.y, rect.x, rect.h, rect.w}, first, page,
                             total);
    cairo_restore(cairo);
}

void xd_theme_draw_sizebox(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (xd_pkgstyle_draw_sizebox(theme, cairo, rect))
        return;
    xd_fill_rect(cairo, rect, face(theme));
    size_grip(theme, cairo, rect);
}

/* ---- progress bars ----------------------------------------------------------------- */

void xd_theme_draw_progress(const xd_theme *theme, cairo_t *cairo, xd_rect rect, double fraction) {
    if (xd_pkgstyle_draw_progress(theme, cairo, rect, fraction))
        return;
    int px = xd_tpx(theme, 1);
    xd_rect in;
    xd_rgba chunk_color;
    int chunk, space;
    if (classic(theme)) {
        /* Windows 2000: a thin sunken well, Hilight chunks two thirds as
         * wide as the bar is high, a pixel apart from the edge. */
        xd_fill_rect(cairo, rect, face(theme));
        in = xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                           xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
        in = xd_rect_inset(in, px, px);
        chunk_color = xd_sys(theme, XD_COLOR_HILIGHT);
        chunk = XD_MAX(in.h * 2 / 3, px);
        space = 2 * px;
    } else {
        /* XP: a white well with a grey border, green chunks. */
        xd_fill_rect(cairo, rect, xd_rgba_hex(0xFFFFFF));
        xd_rgba border = xd_rgba_hex(0x686868);
        in = xd_rect_inset(xd_draw_bevel(cairo, rect, px, border, border), 2 * px, 2 * px);
        chunk_color = xd_rgba_hex(0x2ED331);
        chunk = xd_tpx(theme, 6);
        space = theme->style == XD_STYLE_AERO ? 0 : 2 * px;
    }
    int done = (int)lround(in.w * XD_CLAMP(fraction, 0.0, 1.0));
    cairo_save(cairo);
    cairo_rectangle(cairo, in.x, in.y, done, in.h);
    cairo_clip(cairo);
    for (int x = in.x; x < in.x + done; x += chunk + space)
        xd_fill_rect(cairo, (xd_rect){x, in.y, chunk, in.h}, chunk_color);
    cairo_restore(cairo);
}

/* ---- sliders ------------------------------------------------------------------------ */

/* A down-pointing trackbar thumb in device pixels. */
static void slider_thumb_path(cairo_t *cairo, xd_rect rect, double inset) {
    double tip = rect.w / 2.0;
    double body = rect.h - tip;
    cairo_new_path(cairo);
    cairo_move_to(cairo, rect.x + inset, rect.y + inset);
    cairo_line_to(cairo, rect.x + rect.w - inset, rect.y + inset);
    cairo_line_to(cairo, rect.x + rect.w - inset, rect.y + body);
    cairo_line_to(cairo, rect.x + rect.w / 2.0, rect.y + rect.h - inset);
    cairo_line_to(cairo, rect.x + inset, rect.y + body);
    cairo_close_path(cairo);
}

void xd_theme_draw_trackbar(const xd_theme *theme, cairo_t *cairo, xd_rect channel, xd_rect thumb,
                            uint8_t state) {
    if (xd_pkgstyle_draw_trackbar(theme, cairo, channel, thumb, state))
        return;
    xd_toolkit_colors toolkit_colors;
    xd_theme_toolkit_colors(theme, &toolkit_colors);
    int px = xd_tpx(theme, 1);
    bool enabled = !(state & XD_STATE_DISABLED);

    /* Channel: a sunken groove the thumb rides along. */
    if (toolkit_colors.classic) {
        xd_rect in =
            xd_draw_bevel(cairo, channel, px, toolkit_colors.shadow, toolkit_colors.hilight);
        xd_draw_bevel(cairo, in, px, toolkit_colors.dkshadow, toolkit_colors.light);
    } else {
        xd_fill_rect(cairo, channel, toolkit_colors.track);
        xd_draw_bevel(cairo, channel, px, toolkit_colors.field_border, toolkit_colors.field_border);
    }

    slider_thumb_path(cairo, thumb, 0);
    if (toolkit_colors.classic) {
        xd_set_color(cairo, toolkit_colors.face);
        cairo_fill(cairo);
        /* Light top and left, dark right and point. */
        double half_pixel = px / 2.0;
        double body = thumb.h - thumb.w / 2.0;
        cairo_set_line_width(cairo, px);
        xd_set_color(cairo, toolkit_colors.hilight);
        cairo_move_to(cairo, thumb.x + half_pixel, thumb.y + body);
        cairo_line_to(cairo, thumb.x + half_pixel, thumb.y + half_pixel);
        cairo_line_to(cairo, thumb.x + thumb.w - px, thumb.y + half_pixel);
        cairo_stroke(cairo);
        xd_set_color(cairo, toolkit_colors.dkshadow);
        cairo_move_to(cairo, thumb.x + thumb.w - half_pixel, thumb.y);
        cairo_line_to(cairo, thumb.x + thumb.w - half_pixel, thumb.y + body);
        cairo_line_to(cairo, thumb.x + thumb.w / 2.0, thumb.y + thumb.h - half_pixel);
        cairo_stroke(cairo);
        xd_set_color(cairo, toolkit_colors.shadow);
        cairo_move_to(cairo, thumb.x + thumb.w - px - half_pixel, thumb.y + px);
        cairo_line_to(cairo, thumb.x + thumb.w - px - half_pixel, thumb.y + body - px / 2.0);
        cairo_line_to(cairo, thumb.x + thumb.w / 2.0, thumb.y + thumb.h - px - half_pixel);
        cairo_stroke(cairo);
        xd_set_color(cairo, toolkit_colors.hilight);
        cairo_move_to(cairo, thumb.x + half_pixel, thumb.y + body);
        cairo_line_to(cairo, thumb.x + thumb.w / 2.0, thumb.y + thumb.h - half_pixel);
        cairo_stroke(cairo);
    } else {
        cairo_pattern_t *pattern = cairo_pattern_create_linear(0, thumb.y, 0, thumb.y + thumb.h);
        xd_rgba top = enabled ? toolkit_colors.thumb_top : toolkit_colors.face;
        xd_rgba bottom = enabled ? toolkit_colors.thumb_bottom : toolkit_colors.face;
        cairo_pattern_add_color_stop_rgba(pattern, 0, top.r, top.g, top.b, top.a);
        cairo_pattern_add_color_stop_rgba(pattern, 1, bottom.r, bottom.g, bottom.b, bottom.a);
        cairo_set_source(cairo, pattern);
        cairo_fill_preserve(cairo);
        cairo_pattern_destroy(pattern);
        xd_set_color(cairo, enabled ? toolkit_colors.button_border : toolkit_colors.disabled);
        cairo_set_line_width(cairo, px);
        slider_thumb_path(cairo, thumb, px / 2.0);
        cairo_stroke(cairo);
    }
}

xd_rgba xd_theme_trackbar_tics_color(const xd_theme *theme, bool disabled) {
    xd_rgba color;
    if (!disabled && xd_pkgstyle_trackbar_tics_color(theme, &color))
        return color;
    xd_toolkit_colors toolkit_colors;
    xd_theme_toolkit_colors(theme, &toolkit_colors);
    return disabled ? toolkit_colors.disabled : toolkit_colors.fg;
}

/* ---- spin buttons -------------------------------------------------------------------- */

void xd_theme_draw_spin_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool up,
                               uint8_t state) {
    if (xd_pkgstyle_draw_spin(theme, cairo, rect, up, state))
        return;
    bool pressed = state & XD_STATE_PRESSED, disabled = state & XD_STATE_DISABLED;
    if (classic(theme)) {
        xd_rect in = classic_button(theme, cairo, rect, pressed);
        if (pressed)
            in = (xd_rect){in.x + xd_tpx(theme, 1), in.y + xd_tpx(theme, 1), in.w, in.h};
        triangle(theme, cairo, in, up,
                 xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT));
        return;
    }
    /* XP's pale blue arrow buttons, as on drop-downs. */
    uint32_t top = pressed ? 0x6E8DED : (state & XD_STATE_HOT) ? 0xDDECFE : 0xC1D3FB;
    uint32_t bottom = pressed ? 0x8FADF5 : (state & XD_STATE_HOT) ? 0xB6CDFB : 0xA6C0F4;
    if (disabled) {
        top = 0xF0F0F0;
        bottom = 0xE0E0E0;
    }
    int px = xd_tpx(theme, 1);
    double rad = xd_tpx(theme, 2);
    xd_rect button = xd_rect_inset(rect, px, 0);
    xd_path_rounded(cairo, button.x, button.y, button.w, button.h, rad, rad, rad, rad);
    cairo_save(cairo);
    cairo_clip(cairo);
    xd_fill_gradient(cairo, button, xd_rgba_hex(top), xd_rgba_hex(bottom), false);
    cairo_restore(cairo);
    triangle(theme, cairo, button, up, xd_rgba_hex(disabled ? 0xC9C7BA : 0x4D6185));
}

/* ---- column headers and tree glyphs ------------------------------------------------- */

void xd_theme_draw_header_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                               const char *label) {
    if (xd_pkgstyle_draw_header_item(theme, cairo, rect, state, label))
        return;
    int px = xd_tpx(theme, 1);
    bool pressed = state & XD_STATE_PRESSED;
    xd_rect text = xd_rect_inset4(rect, xd_tpx(theme, 6), 0, xd_tpx(theme, 6), 0);
    if (classic(theme)) {
        classic_button(theme, cairo, rect, pressed);
        if (pressed)
            text.x += px;
    } else {
        /* XP: white to beige, three shaded lines along the bottom (orange
         * when hot), a divider at the right. */
        xd_fill_gradient(cairo, rect, xd_rgba_hex(pressed ? 0xDEDFD8 : 0xFFFFFF),
                         xd_rgba_hex(pressed ? 0xDEDFD8 : 0xEBEADB), false);
        bool hot = (state & XD_STATE_HOT) && !pressed;
        static const uint32_t bottom[2][3] = {{0xE2DECD, 0xD6D2C2, 0xCBC7B8},
                                              {0xF9B119, 0xF8A90E, 0xFFC83C}};
        for (int i = 0; i < 3; i++)
            xd_fill_rect(cairo, (xd_rect){rect.x, rect.y + rect.h - (3 - i) * px, rect.w, px},
                         xd_rgba_hex(bottom[hot][i]));
        int inset = xd_tpx(theme, 3);
        xd_fill_rect(cairo,
                     (xd_rect){rect.x + rect.w - 2 * px, rect.y + inset, px, rect.h - 2 * inset},
                     xd_rgba_hex(0xACA899));
        xd_fill_rect(cairo, (xd_rect){rect.x + rect.w - px, rect.y + inset, px, rect.h - 2 * inset},
                     xd_rgba_hex(0xFFFFFF));
    }
    if (label != nullptr) {
        xd_text_style st =
            xd_text(theme, XD_FONT_MESSAGE, xd_sys(theme, XD_COLOR_BUTTON_TEXT), XD_ALIGN_START);
        xd_draw_text(cairo, text, label, &st);
    }
}

void xd_theme_draw_tree_glyph(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool open) {
    if (xd_pkgstyle_draw_tree_glyph(theme, cairo, rect, open))
        return;
    /* A 9 px box with a plus or minus, as comctl32 draws without a theme. */
    int px = xd_tpx(theme, 1), box_size = xd_tpx(theme, 9);
    xd_rect box = {rect.x + (rect.w - box_size) / 2, rect.y + (rect.h - box_size) / 2, box_size,
                   box_size};
    xd_rgba border = classic(theme) ? xd_sys(theme, XD_COLOR_BUTTON_SHADOW) : xd_rgba_hex(0x919B9C);
    xd_fill_rect(cairo, box, xd_sys(theme, XD_COLOR_WINDOW));
    xd_draw_bevel(cairo, box, px, border, border);
    xd_rgba sign = xd_sys(theme, XD_COLOR_WINDOW_TEXT);
    int arm = xd_tpx(theme, 2);
    xd_fill_rect(cairo, (xd_rect){box.x + arm, box.y + (box_size - px) / 2, box_size - 2 * arm, px},
                 sign);
    if (!open)
        xd_fill_rect(cairo,
                     (xd_rect){box.x + (box_size - px) / 2, box.y + arm, px, box_size - 2 * arm},
                     sign);
}

/* ---- status bars and tooltips --------------------------------------------------------- */

void xd_theme_draw_statusbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *text,
                             bool grip) {
    if (xd_pkgstyle_draw_statusbar(theme, cairo, rect, text, grip))
        return;
    int px = xd_tpx(theme, 1);
    xd_fill_rect(cairo, rect, face(theme));
    /* One sunken pane, a little below the top edge, left of the grip. */
    int gw = grip ? xd_tpx(theme, 14) : 0;
    xd_rect pane = {rect.x, rect.y + xd_tpx(theme, 2), rect.w - gw, rect.h - xd_tpx(theme, 2)};
    xd_draw_bevel(cairo, pane, px, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                  xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    if (grip)
        size_grip(theme, cairo, rect);
    if (text != nullptr) {
        xd_text_style st =
            xd_text(theme, XD_FONT_TOOLTIP, xd_sys(theme, XD_COLOR_BUTTON_TEXT), XD_ALIGN_START);
        xd_draw_text(cairo, xd_rect_inset4(pane, xd_tpx(theme, 4), 0, xd_tpx(theme, 4), 0), text,
                     &st);
    }
}

void xd_theme_draw_tooltip(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *text) {
    if (xd_pkgstyle_draw_tooltip(theme, cairo, rect, text))
        return;
    /* The scheme's InfoWindow colours and a border in InfoText. */
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_INFO_WINDOW));
    xd_rgba edge = xd_sys(theme, XD_COLOR_INFO_TEXT);
    xd_draw_bevel(cairo, rect, xd_tpx(theme, 1), edge, edge);
    xd_text_style st =
        xd_text(theme, XD_FONT_TOOLTIP, xd_sys(theme, XD_COLOR_INFO_TEXT), XD_ALIGN_START);
    xd_draw_text(cairo, xd_rect_inset(rect, xd_tpx(theme, 4), 0), text, &st);
}

/* ---- program toolbars ------------------------------------------------------------------ */

void xd_theme_draw_rebar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool gripper) {
    if (xd_pkgstyle_draw_rebar(theme, cairo, rect, gripper))
        return;
    int px = xd_tpx(theme, 1);
    xd_fill_rect(cairo, rect, face(theme));
    /* An etched line along the top, as Windows 2000's bands have. */
    xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, rect.w, px},
                 xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
    xd_fill_rect(cairo, (xd_rect){rect.x, rect.y + px, rect.w, px},
                 xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    if (gripper) {
        xd_rect bar = {rect.x + xd_tpx(theme, 2), rect.y + xd_tpx(theme, 4), xd_tpx(theme, 3),
                       rect.h - xd_tpx(theme, 6)};
        xd_draw_bevel(cairo, bar, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
    }
}

void xd_theme_draw_tool_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                               const char *label) {
    if (xd_pkgstyle_draw_tool_button(theme, cairo, rect, state, label))
        return;
    int px = xd_tpx(theme, 1);
    bool down = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    if (down) {
        /* Sunken, and a checked button's face dithered lighter. */
        if ((state & XD_STATE_CHECKED) && !(state & XD_STATE_PRESSED))
            xd_fill_rect(cairo, rect,
                         xd_rgba_mix(face(theme), xd_sys(theme, XD_COLOR_BUTTON_HILIGHT), 0.5));
        xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                      xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    } else if (state & XD_STATE_HOT) {
        xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
    }
    if (label != nullptr) {
        xd_text_style st = xd_text(
            theme, XD_FONT_MESSAGE,
            xd_sys(theme, (state & XD_STATE_DISABLED) ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT),
            XD_ALIGN_CENTER);
        xd_rect tr = rect;
        if (down) {
            tr.x += px;
            tr.y += px;
        }
        xd_draw_text(cairo, tr, label, &st);
    }
}

/* ---- menu bars ---------------------------------------------------------------------- */

void xd_theme_draw_menubar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool active) {
    if (xd_pkgstyle_draw_menubar(theme, cairo, rect, active))
        return;
    xd_menu_palette menu_palette;
    xd_theme_menu_palette(theme, &menu_palette);
    /* XP's flat menus give the bar its own colour (MenuBar). */
    xd_fill_rect(cairo, rect,
                 menu_palette.flat ? xd_sys(theme, XD_COLOR_MENU_BAR)
                                   : xd_sys(theme, XD_COLOR_MENU));
}

void xd_theme_draw_menubar_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                const char *label) {
    if (xd_pkgstyle_draw_menubar_item(theme, cairo, rect, state, label))
        return;
    xd_menu_palette menu_palette;
    xd_theme_menu_palette(theme, &menu_palette);
    int px = xd_tpx(theme, 1);
    bool disabled = state & XD_STATE_DISABLED;
    bool lit = (state & (XD_STATE_HOT | XD_STATE_PRESSED)) && !disabled;
    xd_rgba text = disabled ? menu_palette.disabled_text : menu_palette.text;
    xd_rect tr = rect;
    if (lit && menu_palette.flat) {
        /* XP: the highlight, framed. */
        xd_fill_rect(cairo, rect, menu_palette.hilight);
        if (menu_palette.hilight_border.a > 0)
            xd_draw_bevel(cairo, rect, px, menu_palette.hilight_border,
                          menu_palette.hilight_border);
        text = menu_palette.hilight_text;
    } else if (lit) {
        /* Windows 2000: a thin raised edge when hot, sunken when open. */
        bool open = state & XD_STATE_PRESSED;
        xd_rgba hi = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                sh = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
        xd_draw_bevel(cairo, rect, px, open ? sh : hi, open ? hi : sh);
        if (open) {
            tr.x += px;
            tr.y += px;
        }
    }
    xd_text_style st = xd_text(theme, XD_FONT_MENU, text, XD_ALIGN_CENTER);
    xd_draw_text(cairo, tr, label, &st);
}
