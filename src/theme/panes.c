/* SPDX-License-Identifier: MIT
 * Explorer's panes (panes.h): from the theme package where it styles them
 * (style_pkg.c), else XP's task pane drawn in code.
 *
 * Without a package the task pane is laid out as XP's is (shellstyle.dll's
 * lengths: 12 px around the groups, 15 between them, a 25 px title bar,
 * 4 px between tasks). Luna's colour variants get the colours XP's
 * shellstyle.dll gives them; other styles and schemes a stand-in made from
 * the scheme: the title colours for the special group's title bar, the
 * selection colour washed out for the pane and the bodies.
 */
#include <glib.h>
#include <math.h>

#include "panes.h"

typedef struct palette {
    xd_rgba pane_top, pane_bottom;
    xd_rgba head_from[2], head_to[2]; /* [special] */
    xd_rgba body[2];
    xd_rgba text[2][2]; /* titles: [special][hot] */
    xd_rgba link, link_hot;
    xd_rgba arrow_face[2], arrow_mark[2];
} palette;

/* XP's shellstyle.dll for each Luna colour scheme (Energy Blue's is
 * Blue's): the pane's gradient, the title bars' ends, the bodies, the
 * titles' and links' colours. */
static const struct {
    xd_luna_color color;
    uint32_t pane[2], normal_head[2], special_head[2], body[2], text, hot, special_hot;
} luna_panes[] = {
    {XD_LUNA_BLUE, {0x7BA2E7, 0x6375D6}, {0xFFFFFF, 0xC7D4F7}, {0x0049B5, 0x295DCE},
     {0xD6DFF7, 0xEFF3FF}, 0x215DC6, 0x428EFF, 0x428EFF},
    {XD_LUNA_ENERGY, {0x7BA2E7, 0x6375D6}, {0xFFFFFF, 0xC7D4F7}, {0x0049B5, 0x295DCE},
     {0xD6DFF7, 0xEFF3FF}, 0x215DC6, 0x428EFF, 0x428EFF},
    {XD_LUNA_SILVER, {0xC4C8D4, 0xB1B3C8}, {0xFFFFFF, 0xD6D7E0}, {0x777792, 0xB3B5C6},
     {0xF0F1F5, 0xF0F1F5}, 0x3F3D3D, 0x7E7C7C, 0xE6E6E6},
    {XD_LUNA_OLIVE, {0xCCD9AD, 0xA5BD84}, {0xFFFCEC, 0xE0E7B8}, {0x778C40, 0x96A867},
     {0xF6F6EC, 0xF6F6EC}, 0x56662D, 0x72921D, 0xE0E7B8},
};

static void make_palette(const xd_theme *theme, palette *pane_colors) {
    xd_rgba white = xd_sys(theme, XD_COLOR_WINDOW);
    /* Luna's artwork is Blue's unless the scheme is another variant's. */
    xd_luna_color variant =
        theme->scheme.luna_color != XD_LUNA_NONE ? theme->scheme.luna_color : XD_LUNA_BLUE;
    if (theme->style == XD_STYLE_LUNA)
        for (size_t i = 0; i < G_N_ELEMENTS(luna_panes); i++) {
            if (luna_panes[i].color != variant)
                continue;
            *pane_colors = (palette){
                .pane_top = xd_rgba_hex(luna_panes[i].pane[0]),
                .pane_bottom = xd_rgba_hex(luna_panes[i].pane[1]),
                .head_from = {xd_rgba_hex(luna_panes[i].normal_head[0]),
                              xd_rgba_hex(luna_panes[i].special_head[0])},
                .head_to = {xd_rgba_hex(luna_panes[i].normal_head[1]),
                            xd_rgba_hex(luna_panes[i].special_head[1])},
                .body = {xd_rgba_hex(luna_panes[i].body[0]), xd_rgba_hex(luna_panes[i].body[1])},
                .text = {{xd_rgba_hex(luna_panes[i].text), xd_rgba_hex(luna_panes[i].hot)},
                         {xd_rgba_hex(0xFFFFFF), xd_rgba_hex(luna_panes[i].special_hot)}},
                .link = xd_rgba_hex(luna_panes[i].text),
                .link_hot = xd_rgba_hex(luna_panes[i].hot),
                .arrow_face = {xd_rgba_hex(0xFFFFFF), xd_rgba_hex(luna_panes[i].special_head[1])},
                .arrow_mark = {xd_rgba_hex(luna_panes[i].text), xd_rgba_hex(0xFFFFFF)},
            };
            return;
        }
    xd_rgba tint = xd_sys(theme, XD_COLOR_HILIGHT);
    xd_rgba dark = xd_rgba_mix(tint, (xd_rgba){0, 0, 0, 1}, 0.15);
    xd_rgba hot = xd_sys(theme, XD_COLOR_HOT_TRACKING);
    xd_rgba title = xd_sys(theme, XD_COLOR_TITLE_TEXT);
    *pane_colors = (palette){
        .pane_top = xd_rgba_mix(tint, white, 0.5),
        .pane_bottom = xd_rgba_mix(tint, white, 0.3),
        .head_from = {white, xd_sys(theme, XD_COLOR_ACTIVE_TITLE)},
        .head_to = {xd_rgba_mix(tint, white, 0.75), xd_sys(theme, XD_COLOR_GRADIENT_ACTIVE_TITLE)},
        .body = {xd_rgba_mix(tint, white, 0.82), xd_rgba_mix(tint, white, 0.94)},
        .text = {{dark, hot}, {title, xd_rgba_mix(title, hot, 0.5)}},
        .link = dark,
        .link_hot = hot,
        .arrow_face = {white, tint},
        .arrow_mark = {dark, xd_sys(theme, XD_COLOR_HILIGHT_TEXT)},
    };
}

static void font(const xd_theme *theme, xd_font_spec *spec, bool bold) {
    *spec = theme->scheme.fonts[XD_FONT_MESSAGE];
    spec->bold = bold;
}

void xd_theme_taskpane_style(const xd_theme *theme, xd_taskpane_style *taskpane_style) {
    palette pane_colors;
    make_palette(theme, &pane_colors);
    int p12 = xd_tpx(theme, 12);
    *taskpane_style = (xd_taskpane_style){
        .padding = {p12, p12, p12, p12},
        .group_spacing = xd_tpx(theme, 15),
        .header_height = xd_tpx(theme, 25),
        .body = {{xd_tpx(theme, 13), xd_tpx(theme, 10), xd_tpx(theme, 13), xd_tpx(theme, 11)},
                 {xd_tpx(theme, 13), xd_tpx(theme, 10), xd_tpx(theme, 13), xd_tpx(theme, 11)}},
        .link_spacing = {xd_tpx(theme, 4), xd_tpx(theme, 4)},
        .icon_gap = {xd_tpx(theme, 6), xd_tpx(theme, 6)},
        .icon_size = xd_tpx(theme, 16),
        .details_spacing = xd_tpx(theme, 5),
        .details_color = xd_sys(theme, XD_COLOR_WINDOW_TEXT),
    };
    for (int sp = 0; sp < 2; sp++) {
        font(theme, &taskpane_style->header_font[sp], true);
        for (int hot = 0; hot < 2; hot++) {
            taskpane_style->header_color[sp][hot] = pane_colors.text[sp][hot];
            font(theme, &taskpane_style->link_font[sp][hot], false);
            taskpane_style->link_underline[sp][hot] = hot;
            taskpane_style->link_color[sp][hot] = hot ? pane_colors.link_hot : pane_colors.link;
        }
    }
    font(theme, &taskpane_style->details_font, false);
    font(theme, &taskpane_style->details_name_font, true);
    xd_pkgstyle_taskpane_style(theme, taskpane_style);
}

void xd_theme_draw_taskpane(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (xd_pkgstyle_draw_taskpane(theme, cairo, rect))
        return;
    palette pane_colors;
    make_palette(theme, &pane_colors);
    xd_fill_gradient(cairo, rect, pane_colors.pane_top, pane_colors.pane_bottom, false);
}

/* The Expand or Collapse button without a package: a disc with two
 * chevrons, as XP's. */
static void draw_arrow(const xd_theme *theme, const palette *pane_colors, cairo_t *cairo,
                       xd_rect rect, bool special, bool expanded, bool hot) {
    double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h / 2.0;
    double rad = xd_tpx(theme, 8);
    xd_rgba face = pane_colors->arrow_face[special], mark = pane_colors->arrow_mark[special];
    xd_rgba ring = xd_rgba_mix(mark, face, 0.65);
    if (hot)
        face = xd_rgba_mix(face, xd_sys(theme, XD_COLOR_WINDOW), 0.3);
    cairo_save(cairo);
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, cy, rad, 0, 2 * G_PI);
    xd_set_color(cairo, face);
    cairo_fill_preserve(cairo);
    cairo_set_line_width(cairo, xd_tpx(theme, 1));
    xd_set_color(cairo, ring);
    cairo_stroke(cairo);
    /* Two chevrons, pointing up (Collapse) or down (Expand). */
    double width = xd_tpx(theme, 3), height = xd_tpx(theme, 3), dir = expanded ? -1 : 1;
    cairo_set_line_width(cairo, MAX(1.0, round(1.5 * theme->scale)));
    xd_set_color(cairo, mark);
    for (int i = 0; i < 2; i++) {
        double y = cy + (i == 0 ? -xd_tpx(theme, 2) : xd_tpx(theme, 2)) - dir * height / 2;
        cairo_move_to(cairo, cx - width, y);
        cairo_line_to(cairo, cx, y + dir * height);
        cairo_line_to(cairo, cx + width, y);
        cairo_stroke(cairo);
    }
    cairo_restore(cairo);
}

void xd_theme_draw_taskpane_header(const xd_theme *theme, cairo_t *cairo,
                                   const xd_taskpane_style *taskpane_style, xd_rect rect,
                                   bool special, bool expanded, uint8_t state, const char *title) {
    if (xd_pkgstyle_draw_taskpane_header(theme, cairo, taskpane_style, rect, special, expanded,
                                         state, title))
        return;
    palette pane_colors;
    make_palette(theme, &pane_colors);
    bool hot = state & (XD_STATE_HOT | XD_STATE_PRESSED);
    /* Rounded top corners, as XP's title bars have. */
    double rad = xd_tpx(theme, 3);
    cairo_save(cairo);
    xd_path_rounded(cairo, rect.x, rect.y, rect.w, rect.h, rad, rad, 0, 0);
    cairo_clip(cairo);
    xd_fill_gradient(cairo, rect, pane_colors.head_from[special], pane_colors.head_to[special],
                     true);
    cairo_restore(cairo);
    int aw = xd_tpx(theme, 21), ah = xd_tpx(theme, 23);
    xd_rect arrow = {rect.x + rect.w - aw - xd_tpx(theme, 2), rect.y + (rect.h - ah) / 2, aw, ah};
    draw_arrow(theme, &pane_colors, cairo, arrow, special, expanded, hot);
    xd_rect title_rect = {rect.x + xd_tpx(theme, 12), rect.y + xd_tpx(theme, 2),
                          arrow.x - rect.x - xd_tpx(theme, 14), rect.h - xd_tpx(theme, 2)};
    if (title != nullptr && title_rect.w > 0) {
        xd_text_style ts = xd_text(theme, XD_FONT_MESSAGE,
                                   taskpane_style->header_color[special][hot], XD_ALIGN_START);
        ts.font = &taskpane_style->header_font[special];
        xd_draw_text(cairo, title_rect, title, &ts);
    }
    if (state & XD_STATE_FOCUSED)
        xd_theme_draw_focus_rect(theme, cairo, xd_rect_inset(title_rect, 0, xd_tpx(theme, 2)));
}

void xd_theme_draw_taskpane_body(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                 bool special) {
    if (xd_pkgstyle_draw_taskpane_body(theme, cairo, rect, special))
        return;
    palette pane_colors;
    make_palette(theme, &pane_colors);
    xd_fill_rect(cairo, rect, pane_colors.body[special]);
    /* White edges left, right and bottom. */
    int border = xd_tpx(theme, 1);
    xd_rgba edge = xd_sys(theme, XD_COLOR_WINDOW);
    xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, border, rect.h}, edge);
    xd_fill_rect(cairo, (xd_rect){rect.x + rect.w - border, rect.y, border, rect.h}, edge);
    xd_fill_rect(cairo, (xd_rect){rect.x, rect.y + rect.h - border, rect.w, border}, edge);
}

bool xd_theme_draw_explorer_part(const xd_theme *theme, cairo_t *cairo, xd_explorer_part part,
                                 uint8_t state, xd_rect rect) {
    return xd_pkgstyle_draw_explorer_part(theme, cairo, part, state, rect);
}

bool xd_theme_explorer_text_color(const xd_theme *theme, xd_explorer_part part, uint8_t state,
                                  xd_rgba *out) {
    return xd_pkgstyle_explorer_text_color(theme, part, state, out);
}
