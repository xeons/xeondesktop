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

static void make_palette(const xd_theme *t, palette *p) {
    xd_rgba white = xd_sys(t, XD_COLOR_WINDOW);
    /* Luna's artwork is Blue's unless the scheme is another variant's. */
    xd_luna_color variant = t->scheme.luna_color != XD_LUNA_NONE ? t->scheme.luna_color
                                                                 : XD_LUNA_BLUE;
    if (t->style == XD_STYLE_LUNA)
        for (size_t i = 0; i < G_N_ELEMENTS(luna_panes); i++) {
            if (luna_panes[i].color != variant)
                continue;
            *p = (palette){
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
    xd_rgba tint = xd_sys(t, XD_COLOR_HILIGHT);
    xd_rgba dark = xd_rgba_mix(tint, (xd_rgba){0, 0, 0, 1}, 0.15);
    xd_rgba hot = xd_sys(t, XD_COLOR_HOT_TRACKING);
    xd_rgba title = xd_sys(t, XD_COLOR_TITLE_TEXT);
    *p = (palette){
        .pane_top = xd_rgba_mix(tint, white, 0.5),
        .pane_bottom = xd_rgba_mix(tint, white, 0.3),
        .head_from = {white, xd_sys(t, XD_COLOR_ACTIVE_TITLE)},
        .head_to = {xd_rgba_mix(tint, white, 0.75), xd_sys(t, XD_COLOR_GRADIENT_ACTIVE_TITLE)},
        .body = {xd_rgba_mix(tint, white, 0.82), xd_rgba_mix(tint, white, 0.94)},
        .text = {{dark, hot}, {title, xd_rgba_mix(title, hot, 0.5)}},
        .link = dark,
        .link_hot = hot,
        .arrow_face = {white, tint},
        .arrow_mark = {dark, xd_sys(t, XD_COLOR_HILIGHT_TEXT)},
    };
}

static void font(const xd_theme *t, xd_font_spec *f, bool bold) {
    *f = t->scheme.fonts[XD_FONT_MESSAGE];
    f->bold = bold;
}

void xd_theme_taskpane_style(const xd_theme *t, xd_taskpane_style *s) {
    palette p;
    make_palette(t, &p);
    int p12 = xd_tpx(t, 12);
    *s = (xd_taskpane_style){
        .padding = {p12, p12, p12, p12},
        .group_spacing = xd_tpx(t, 15),
        .header_height = xd_tpx(t, 25),
        .body = {{xd_tpx(t, 13), xd_tpx(t, 10), xd_tpx(t, 13), xd_tpx(t, 11)},
                 {xd_tpx(t, 13), xd_tpx(t, 10), xd_tpx(t, 13), xd_tpx(t, 11)}},
        .link_spacing = {xd_tpx(t, 4), xd_tpx(t, 4)},
        .icon_gap = {xd_tpx(t, 6), xd_tpx(t, 6)},
        .icon_size = xd_tpx(t, 16),
        .details_spacing = xd_tpx(t, 5),
        .details_color = xd_sys(t, XD_COLOR_WINDOW_TEXT),
    };
    for (int sp = 0; sp < 2; sp++) {
        font(t, &s->header_font[sp], true);
        for (int h = 0; h < 2; h++) {
            s->header_color[sp][h] = p.text[sp][h];
            font(t, &s->link_font[sp][h], false);
            s->link_underline[sp][h] = h;
            s->link_color[sp][h] = h ? p.link_hot : p.link;
        }
    }
    font(t, &s->details_font, false);
    font(t, &s->details_name_font, true);
    xd_pkgstyle_taskpane_style(t, s);
}

void xd_theme_draw_taskpane(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (xd_pkgstyle_draw_taskpane(t, cr, r))
        return;
    palette p;
    make_palette(t, &p);
    xd_fill_gradient(cr, r, p.pane_top, p.pane_bottom, false);
}

/* The Expand or Collapse button without a package: a disc with two
 * chevrons, as XP's. */
static void draw_arrow(const xd_theme *t, const palette *p, cairo_t *cr, xd_rect r, bool special,
                       bool expanded, bool hot) {
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double rad = xd_tpx(t, 8);
    xd_rgba face = p->arrow_face[special], mark = p->arrow_mark[special];
    xd_rgba ring = xd_rgba_mix(mark, face, 0.65);
    if (hot)
        face = xd_rgba_mix(face, xd_sys(t, XD_COLOR_WINDOW), 0.3);
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, rad, 0, 2 * G_PI);
    xd_set_color(cr, face);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, xd_tpx(t, 1));
    xd_set_color(cr, ring);
    cairo_stroke(cr);
    /* Two chevrons, pointing up (Collapse) or down (Expand). */
    double w = xd_tpx(t, 3), h = xd_tpx(t, 3), dir = expanded ? -1 : 1;
    cairo_set_line_width(cr, MAX(1.0, round(1.5 * t->scale)));
    xd_set_color(cr, mark);
    for (int i = 0; i < 2; i++) {
        double y = cy + (i == 0 ? -xd_tpx(t, 2) : xd_tpx(t, 2)) - dir * h / 2;
        cairo_move_to(cr, cx - w, y);
        cairo_line_to(cr, cx, y + dir * h);
        cairo_line_to(cr, cx + w, y);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void xd_theme_draw_taskpane_header(const xd_theme *t, cairo_t *cr, const xd_taskpane_style *s,
                                   xd_rect r, bool special, bool expanded, uint8_t state,
                                   const char *title) {
    if (xd_pkgstyle_draw_taskpane_header(t, cr, s, r, special, expanded, state, title))
        return;
    palette p;
    make_palette(t, &p);
    bool hot = state & (XD_STATE_HOT | XD_STATE_PRESSED);
    /* Rounded top corners, as XP's title bars have. */
    double rad = xd_tpx(t, 3);
    cairo_save(cr);
    xd_path_rounded(cr, r.x, r.y, r.w, r.h, rad, rad, 0, 0);
    cairo_clip(cr);
    xd_fill_gradient(cr, r, p.head_from[special], p.head_to[special], true);
    cairo_restore(cr);
    int aw = xd_tpx(t, 21), ah = xd_tpx(t, 23);
    xd_rect a = {r.x + r.w - aw - xd_tpx(t, 2), r.y + (r.h - ah) / 2, aw, ah};
    draw_arrow(t, &p, cr, a, special, expanded, hot);
    xd_rect c = {r.x + xd_tpx(t, 12), r.y + xd_tpx(t, 2), a.x - r.x - xd_tpx(t, 14),
                 r.h - xd_tpx(t, 2)};
    if (title != nullptr && c.w > 0) {
        xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, s->header_color[special][hot],
                                   XD_ALIGN_START);
        ts.font = &s->header_font[special];
        xd_draw_text(cr, c, title, &ts);
    }
    if (state & XD_STATE_FOCUSED)
        xd_theme_draw_focus_rect(t, cr, xd_rect_inset(c, 0, xd_tpx(t, 2)));
}

void xd_theme_draw_taskpane_body(const xd_theme *t, cairo_t *cr, xd_rect r, bool special) {
    if (xd_pkgstyle_draw_taskpane_body(t, cr, r, special))
        return;
    palette p;
    make_palette(t, &p);
    xd_fill_rect(cr, r, p.body[special]);
    /* White edges left, right and bottom. */
    int b = xd_tpx(t, 1);
    xd_rgba edge = xd_sys(t, XD_COLOR_WINDOW);
    xd_fill_rect(cr, (xd_rect){r.x, r.y, b, r.h}, edge);
    xd_fill_rect(cr, (xd_rect){r.x + r.w - b, r.y, b, r.h}, edge);
    xd_fill_rect(cr, (xd_rect){r.x, r.y + r.h - b, r.w, b}, edge);
}

bool xd_theme_draw_explorer_part(const xd_theme *t, cairo_t *cr, xd_explorer_part part,
                                 uint8_t state, xd_rect r) {
    return xd_pkgstyle_draw_explorer_part(t, cr, part, state, r);
}

bool xd_theme_explorer_text_color(const xd_theme *t, xd_explorer_part part, uint8_t state,
                                  xd_rgba *out) {
    return xd_pkgstyle_explorer_text_color(t, part, state, out);
}
