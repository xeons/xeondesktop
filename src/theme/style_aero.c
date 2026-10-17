/* SPDX-License-Identifier: MIT
 * Aero: the Windows 7 look. Translucent glass window frames (the
 * compositor blurs what's behind them), a grouped caption button strip
 * with a red Close, the title on a soft white glow, a glass taskbar with
 * a round Start orb, and Windows 7's light dialogs and menus. All drawn
 * procedurally.
 */
#include <math.h>
#include <pango/pangocairo.h>

#include "style.h"

/* Logical px at 96 dpi. */
enum {
    BORDER = 8,        /* a resizable window's glass border */
    DIALOG_BORDER = 6, /* a fixed-size window's */
    CAPTION = 22,      /* the caption band below the top border */
    RADIUS = 6,        /* frame corners */
    BTN_H = 20,
    BTN_MIN_W = 26,
    BTN_MAX_W = 27,
    BTN_CLOSE_W = 47,
    BTN_CLOSE_ONLY_W = 43,
    TASKBAR_H = 40,
    ORB_W = 54,
};

static const xd_font_spec title_font = {.family = XD_FONT_FAMILY_SEGOE, .size_pt = 9};

/* The default colourisation ("Sky"), active and inactive; opaque without
 * the compositor's blur (Windows 7 Basic). */
static xd_rgba glass(const xd_theme *t, bool active) {
    if (!t->glass)
        return active ? (xd_rgba){0.73, 0.82, 0.92, 1} : (xd_rgba){0.84, 0.88, 0.94, 1};
    return active ? (xd_rgba){0.45, 0.67, 0.89, 0.55} : (xd_rgba){0.80, 0.84, 0.89, 0.42};
}

static int border_of(const xd_theme *t, const xd_frame *f) {
    if (f->maximized)
        return 0;
    return xd_tpx(t, f->dialog ? DIALOG_BORDER : BORDER);
}

/* ---- frame ---------------------------------------------------------------------- */

static xd_insets aero_frame_insets(const xd_theme *t, const xd_frame *f) {
    int b = border_of(t, f);
    return (xd_insets){b, b + xd_tpx(t, CAPTION), b, b};
}

static void aero_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                              xd_frame_layout *l) {
    xd_insets in = aero_frame_insets(t, f);
    int b = border_of(t, f);
    l->caption = (xd_rect){outer.x, outer.y, outer.w, in.top};
    /* The strip hangs from the top edge, inside the right border. */
    int right = outer.x + outer.w - (f->maximized ? xd_tpx(t, 2) : b - xd_tpx(t, 2));
    int by = outer.y + (f->maximized ? 0 : xd_tpx(t, 1));
    int bh = xd_tpx(t, BTN_H);
    bool close_only = (f->buttons & ~XD_CAPBTN_CLOSE) == 0;
    static const struct {
        uint8_t flag;
        xd_frame_part part;
        int w;
    } order[] = {
        {XD_CAPBTN_CLOSE, XD_PART_CLOSE, BTN_CLOSE_W},
        {XD_CAPBTN_MAXIMIZE, XD_PART_MAXIMIZE, BTN_MAX_W},
        {XD_CAPBTN_MINIMIZE, XD_PART_MINIMIZE, BTN_MIN_W},
        {XD_CAPBTN_HELP, XD_PART_HELP, BTN_MIN_W},
    };
    int bx = right;
    for (size_t i = 0; i < XD_ARRAY_LEN(order); i++) {
        if (!(f->buttons & order[i].flag))
            continue;
        int w = xd_tpx(t, order[i].part == XD_PART_CLOSE && close_only ? BTN_CLOSE_ONLY_W
                                                                       : order[i].w);
        bx -= w;
        xd_layout_add_button(l, (xd_rect){bx, by, w, bh}, order[i].part);
    }
    int icon = xd_tpx(t, 16);
    int cy = outer.y + b + (xd_tpx(t, CAPTION) - icon) / 2;
    l->icon = (xd_rect){outer.x + b + xd_tpx(t, 2), cy, icon, icon};
    int tx = l->icon.x + icon + xd_tpx(t, 5);
    l->title = (xd_rect){tx, outer.y + b, bx - xd_tpx(t, 8) - tx, xd_tpx(t, CAPTION)};
}

/* The glass: the colour, a sheen fading down from the top, and faint
 * diagonal streaks, as light catches Windows 7's frames. */
static void paint_glass(const xd_theme *t, cairo_t *cr, xd_rect r, bool active, int sheen_h) {
    xd_rgba g = glass(t, active);
    cairo_set_source_rgba(cr, g.r, g.g, g.b, g.a);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_pattern_t *p = cairo_pattern_create_linear(0, r.y, 0, r.y + sheen_h);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, active ? 0.42 : 0.35);
    cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.14);
    cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0.0);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, r.x, r.y, r.w, sheen_h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    if (!t->glass)
        return; /* Basic: no reflections */
    /* Streaks every ~180px, leaning right. */
    double step = xd_tpx(t, 180), wide = xd_tpx(t, 36);
    for (double x = r.x - r.h; x < r.x + r.w; x += step) {
        cairo_move_to(cr, x, r.y + r.h);
        cairo_line_to(cr, x + r.h * 0.6, r.y);
        cairo_line_to(cr, x + r.h * 0.6 + wide, r.y);
        cairo_line_to(cr, x + wide, r.y + r.h);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.07 : 0.05);
        cairo_fill(cr);
    }
}

/* The title on Windows 7's soft white glow. */
static void draw_title(const xd_theme *t, cairo_t *cr, xd_rect r, const char *title,
                       bool active) {
    if (title == nullptr || *title == '\0' || r.w <= 0)
        return;
    xd_text_style st = {
        .font = &title_font,
        .scale = t->scale,
        .color = active ? xd_rgba_hex(0x000000) : xd_rgba_hex(0x4A4A4A),
        .align = XD_ALIGN_START,
        .ellipsize = true,
        .smoothing = t->scheme.font_smoothing,
    };
    PangoLayout *l = xd_text_layout(cr, title, &st);
    pango_layout_set_width(l, r.w * PANGO_SCALE);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(l, &tw, &th);
    double x = r.x, y = r.y + (r.h - th) / 2.0;
    /* The glow: a soft white pill under the text. */
    double pad = xd_tpx(t, 10);
    for (int i = 0; i < 6; i++) {
        double grow = pad * (1.0 - i / 6.0);
        xd_path_rounded(cr, x - grow, y + th * 0.15 - grow / 2, tw + 2 * grow,
                        th * 0.7 + grow, (th * 0.7 + grow) / 2, (th * 0.7 + grow) / 2,
                        (th * 0.7 + grow) / 2, (th * 0.7 + grow) / 2);
        cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.10 : 0.07);
        cairo_fill(cr);
    }
    xd_set_color(cr, st.color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

/* A glossy glass button face: a brighter top half over a softer bottom. */
static void gloss(cairo_t *cr, xd_rect r, xd_rgba top0, xd_rgba top1, xd_rgba bot0,
                  xd_rgba bot1) {
    double mid = r.y + r.h * 0.48;
    cairo_pattern_t *p = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    double split = (mid - r.y) / r.h;
    cairo_pattern_add_color_stop_rgba(p, 0, top0.r, top0.g, top0.b, top0.a);
    cairo_pattern_add_color_stop_rgba(p, split, top1.r, top1.g, top1.b, top1.a);
    cairo_pattern_add_color_stop_rgba(p, split, bot0.r, bot0.g, bot0.b, bot0.a);
    cairo_pattern_add_color_stop_rgba(p, 1, bot1.r, bot1.g, bot1.b, bot1.a);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
}

static void caption_button(const xd_theme *t, cairo_t *cr, xd_rect r, xd_frame_part part,
                           const xd_frame *f, bool first, bool last) {
    bool hot = f->hot == part && f->pressed != part;
    bool pressed = f->hot == part && f->pressed == part;
    bool close = part == XD_PART_CLOSE;
    double rad = xd_tpx(t, 4);
    xd_path_rounded(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1, 0, 0, last ? rad : 0,
                    first ? rad : 0);
    if (close && (f->active || hot || pressed)) {
        if (pressed)
            gloss(cr, r, xd_rgba_hex(0xC8705E), xd_rgba_hex(0xA83A28), xd_rgba_hex(0x8C1C10),
                  xd_rgba_hex(0xB0402C));
        else if (hot)
            gloss(cr, r, xd_rgba_hex(0xF4B0A0), xd_rgba_hex(0xE8604A), xd_rgba_hex(0xD8301A),
                  xd_rgba_hex(0xF07A50));
        else
            gloss(cr, r, xd_rgba_hex(0xE8A898), xd_rgba_hex(0xD06C5C), xd_rgba_hex(0xC04232),
                  xd_rgba_hex(0xD06450));
    } else if (pressed) {
        gloss(cr, r, (xd_rgba){0.55, 0.70, 0.85, 0.85}, (xd_rgba){0.25, 0.45, 0.65, 0.85},
              (xd_rgba){0.10, 0.30, 0.50, 0.85}, (xd_rgba){0.20, 0.45, 0.65, 0.85});
    } else if (hot) {
        gloss(cr, r, (xd_rgba){0.85, 0.93, 1.0, 0.75}, (xd_rgba){0.60, 0.80, 0.97, 0.60},
              (xd_rgba){0.40, 0.70, 0.95, 0.55}, (xd_rgba){0.62, 0.86, 1.0, 0.75});
    } else {
        double a = f->active ? 1.0 : 0.7;
        gloss(cr, r, (xd_rgba){1, 1, 1, 0.48 * a}, (xd_rgba){1, 1, 1, 0.26 * a},
              (xd_rgba){1, 1, 1, 0.10 * a}, (xd_rgba){1, 1, 1, 0.28 * a});
    }
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0, 0, 0, f->active ? 0.55 : 0.35);
    cairo_stroke(cr);
    xd_path_rounded(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, 0, 0, last ? rad - 1 : 0,
                    first ? rad - 1 : 0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
    cairo_stroke(cr);
    /* The glyph: white over a dark outline. */
    xd_glyph g = xd_glyph_for_part(part, f->maximized);
    int gs = xd_tpx(t, close ? 13 : 11);
    xd_rect gr = {r.x + (r.w - gs) / 2, r.y + (r.h - gs) / 2, gs, gs};
    xd_rgba shade = xd_rgba_alpha(xd_rgba_hex(0x000000), f->active ? 0.55 : 0.3);
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx != 0 || dy != 0)
                xd_draw_glyph(t, cr, (xd_rect){gr.x + dx, gr.y + dy, gr.w, gr.h}, g, shade,
                              close ? 1.6 : 1.3);
    xd_draw_glyph(t, cr, gr, g, xd_rgba_hex(0xFFFFFF), close ? 1.6 : 1.3);
}

static void aero_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f) {
    xd_insets in = aero_frame_insets(t, f);
    double rad = f->maximized ? 0 : xd_tpx(t, RADIUS);
    cairo_save(cr);
    xd_path_rounded(cr, outer.x, outer.y, outer.w, outer.h, rad, rad, rad, rad);
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    paint_glass(t, cr, outer, f->active, in.top + xd_tpx(t, 14));
    /* Around the client area: a dark line, and a light one outside it. */
    xd_rect client = {outer.x + in.left, outer.y + in.top, outer.w - in.left - in.right,
                      outer.h - in.top - in.bottom};
    /* Nothing under the client area: the compositor blurs only where the
     * frame is drawn. */
    if (client.w > 0 && client.h > 0) {
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_rectangle(cr, client.x, client.y, client.w, client.h);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    }
    if (!f->maximized && client.w > 0 && client.h > 0) {
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, client.x - 1.5, client.y - 1.5, client.w + 3, client.h + 3);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
        cairo_stroke(cr);
        cairo_rectangle(cr, client.x - 0.5, client.y - 0.5, client.w + 1, client.h + 1);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    if (!f->maximized) {
        /* The outline, and a highlight just inside it. */
        cairo_set_line_width(cr, 1);
        xd_path_rounded(cr, outer.x + 0.5, outer.y + 0.5, outer.w - 1, outer.h - 1, rad, rad,
                        rad, rad);
        cairo_set_source_rgba(cr, 0, 0, 0, f->active ? 0.65 : 0.45);
        cairo_stroke(cr);
        xd_path_rounded(cr, outer.x + 1.5, outer.y + 1.5, outer.w - 3, outer.h - 3, rad - 1,
                        rad - 1, rad - 1, rad - 1);
        cairo_set_source_rgba(cr, 1, 1, 1, f->active ? 0.55 : 0.4);
        cairo_stroke(cr);
    }
    xd_frame_layout l = {0};
    aero_frame_layout(t, outer, f, &l);
    for (size_t i = 0; i < l.button_count; i++) {
        /* Laid out right to left: the last is leftmost. */
        caption_button(t, cr, l.button_rects[i], l.button_parts[i], f, i + 1 == l.button_count,
                       i == 0);
    }
    draw_title(t, cr, l.title, f->title, f->active);
}

static xd_rgba aero_dialog_color([[maybe_unused]] const xd_theme *t) {
    return xd_rgba_hex(0xF0F0F0);
}

/* ---- push buttons and check boxes ------------------------------------------------- */

static void aero_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                             const char *label) {
    bool disabled = state & XD_STATE_DISABLED;
    bool pressed = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    bool hot = (state & XD_STATE_HOT) && !disabled;
    bool def = (state & (XD_STATE_DEFAULT | XD_STATE_FOCUSED)) && !disabled;
    double rad = xd_tpx(t, 3);
    xd_path_rounded(cr, r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1, rad, rad, rad, rad);
    if (disabled)
        gloss(cr, r, xd_rgba_hex(0xF4F4F4), xd_rgba_hex(0xF4F4F4), xd_rgba_hex(0xF4F4F4),
              xd_rgba_hex(0xF4F4F4));
    else if (pressed)
        gloss(cr, r, xd_rgba_hex(0xE5F4FC), xd_rgba_hex(0xC4E5F6), xd_rgba_hex(0x98D1EF),
              xd_rgba_hex(0x68B3DB));
    else if (hot)
        gloss(cr, r, xd_rgba_hex(0xEAF6FD), xd_rgba_hex(0xD9F0FC), xd_rgba_hex(0xBEE6FD),
              xd_rgba_hex(0xA7D9F5));
    else
        gloss(cr, r, xd_rgba_hex(0xF2F2F2), xd_rgba_hex(0xEBEBEB), xd_rgba_hex(0xDDDDDD),
              xd_rgba_hex(0xCFCFCF));
    cairo_set_line_width(cr, 1);
    xd_set_color(cr, xd_rgba_hex(disabled  ? 0xADB2B5
                                 : pressed ? 0x2C628B
                                 : hot     ? 0x3C7FB1
                                 : def     ? 0x3399FF
                                           : 0x707070));
    cairo_stroke(cr);
    xd_path_rounded(cr, r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, rad - 1, rad - 1, rad - 1,
                    rad - 1);
    cairo_set_source_rgba(cr, 1, 1, 1, disabled ? 0.0 : 0.7);
    cairo_stroke(cr);
    if (label != nullptr) {
        xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                                   xd_rgba_hex(disabled ? 0x838383 : 0x000000), XD_ALIGN_CENTER);
        xd_draw_text(cr, r, label, &st);
    }
}

static void aero_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label) {
    bool disabled = state & XD_STATE_DISABLED;
    bool hot = (state & XD_STATE_HOT) && !disabled;
    int box = xd_tpx(t, 13);
    xd_rect b = {r.x, r.y + (r.h - box) / 2, box, box};
    xd_fill_rect(cr, b, xd_rgba_hex(disabled ? 0xE6E6E6 : hot ? 0x3C7FB1 : 0x8E8F8F));
    xd_rect inner = xd_rect_inset(b, 1, 1);
    xd_fill_rect(cr, inner, xd_rgba_hex(0xF4F4F4));
    xd_fill_gradient(cr, xd_rect_inset(inner, 1, 1),
                     xd_rgba_hex(disabled ? 0xF0F0F0 : hot ? 0xDEF4FD : 0xD8D8D8),
                     xd_rgba_hex(disabled ? 0xF0F0F0 : hot ? 0xB9E1F6 : 0xF5F5F5), false);
    if (state & XD_STATE_CHECKED)
        xd_draw_glyph(t, cr, xd_rect_inset(b, 2, 2), XD_GLYPH_CHECK,
                      xd_rgba_hex(disabled ? 0x8F8F8F : 0x1F1F1F), 1.0);
    if (label != nullptr) {
        xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                                   xd_rgba_hex(disabled ? 0x838383 : 0x000000), XD_ALIGN_START);
        int tx = b.x + box + xd_tpx(t, 5);
        xd_draw_text(cr, (xd_rect){tx, r.y, r.x + r.w - tx, r.h}, label, &st);
    }
}

/* ---- taskbar ----------------------------------------------------------------------- */

static int aero_taskbar_height(const xd_theme *t) { return xd_tpx(t, TASKBAR_H); }

static void aero_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (t->glass)
        cairo_set_source_rgba(cr, 0.08, 0.16, 0.26, 0.62);
    else
        cairo_set_source_rgb(cr, 0.16, 0.27, 0.42); /* Basic: opaque */
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_restore(cr);
    /* A sheen over the half nearest the desktop, and Windows 7's rim. */
    bool vertical = edge == XD_EDGE_LEFT || edge == XD_EDGE_RIGHT;
    cairo_pattern_t *p =
        vertical ? cairo_pattern_create_linear(r.x, 0, r.x + r.w, 0)
                 : cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.16);
    cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.06);
    cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.0);
    cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0.04);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    int px = xd_tpx(t, 1);
    xd_fill_rect(cr, xd_style_rim_line(r, edge, 0, px), xd_rgba_alpha(xd_rgba_hex(0x000000), 0.5));
    xd_fill_rect(cr, xd_style_rim_line(r, edge, px, px),
                 xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.38));
}

static int aero_start_button_width(const xd_theme *t, [[maybe_unused]] cairo_t *cr) {
    return xd_tpx(t, ORB_W);
}

/* The Start orb: a glossy dark-blue sphere with the logo. */
static void aero_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    double d = XD_MIN(r.w, r.h) - xd_tpx(t, 2);
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, rad = d / 2;
    bool hot = state & XD_STATE_HOT, pressed = state & XD_STATE_PRESSED;
    if (hot || pressed) {
        cairo_pattern_t *glow = cairo_pattern_create_radial(cx, cy, rad * 0.6, cx, cy, rad * 1.3);
        cairo_pattern_add_color_stop_rgba(glow, 0, 0.55, 0.85, 1.0, pressed ? 0.3 : 0.55);
        cairo_pattern_add_color_stop_rgba(glow, 1, 0.55, 0.85, 1.0, 0);
        cairo_set_source(cr, glow);
        cairo_arc(cr, cx, cy, rad * 1.3, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);
    }
    cairo_pattern_t *body = cairo_pattern_create_radial(cx, cy + rad * 0.45, rad * 0.1, cx, cy,
                                                        rad);
    double dim = pressed ? 0.75 : 1.0;
    cairo_pattern_add_color_stop_rgb(body, 0, 0.30 * dim, 0.62 * dim, 0.95 * dim);
    cairo_pattern_add_color_stop_rgb(body, 0.55, 0.10 * dim, 0.30 * dim, 0.60 * dim);
    cairo_pattern_add_color_stop_rgb(body, 1, 0.03 * dim, 0.10 * dim, 0.24 * dim);
    cairo_set_source(cr, body);
    cairo_arc(cr, cx, cy, rad, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(body);
    /* The gloss over the top half. */
    cairo_save(cr);
    cairo_translate(cr, cx, cy - rad * 0.42);
    cairo_scale(cr, rad * 0.78, rad * 0.5);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, -1, 0, 1);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.75);
    cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0.08);
    cairo_set_source(cr, g);
    cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_restore(cr);
    cairo_set_line_width(cr, xd_tpx(t, 1));
    cairo_arc(cr, cx, cy, rad - 0.5, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, 0.75, 0.9, 1.0, 0.65);
    cairo_stroke(cr);
    int logo = (int)(d * 0.55);
    xd_draw_start_logo(cr, (xd_rect){(int)(cx - logo / 2.0), (int)(cy - logo / 2.0), logo, logo});
}

static void aero_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label, cairo_surface_t *icon) {
    bool active = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    bool hot = state & XD_STATE_HOT;
    bool flash = (state & XD_STATE_FLASH) && !active;
    xd_rect b = xd_rect_inset(r, xd_tpx(t, 1), xd_tpx(t, 2));
    double rad = xd_tpx(t, 3);
    xd_path_rounded(cr, b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, rad, rad, rad, rad);
    if (flash)
        gloss(cr, b, (xd_rgba){1, 0.80, 0.45, 0.85}, (xd_rgba){1, 0.62, 0.20, 0.80},
              (xd_rgba){0.95, 0.48, 0.08, 0.80}, (xd_rgba){1, 0.62, 0.20, 0.85});
    else
        gloss(cr, b, (xd_rgba){1, 1, 1, active ? 0.34 : hot ? 0.28 : 0.16},
              (xd_rgba){1, 1, 1, active ? 0.18 : hot ? 0.14 : 0.06},
              (xd_rgba){1, 1, 1, active ? 0.08 : hot ? 0.06 : 0.0},
              (xd_rgba){1, 1, 1, active ? 0.20 : hot ? 0.18 : 0.06});
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_stroke(cr);
    xd_path_rounded(cr, b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3, rad - 1, rad - 1, rad - 1,
                    rad - 1);
    cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.55 : 0.30);
    cairo_stroke(cr);
    int isz = xd_tpx(t, 16);
    int ix = b.x + xd_tpx(t, 8);
    xd_style_task_icon(t, cr, icon, ix, b.y + (b.h - isz) / 2);
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_START);
    st.shadow = true;
    st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    int tx = ix + isz + xd_tpx(t, 6);
    xd_draw_text(cr, (xd_rect){tx, b.y, b.x + b.w - tx - xd_tpx(t, 6), b.h}, label, &st);
}

static void aero_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock) {
    /* No band of its own in Windows 7: the clock sits on the glass. */
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0xFFFFFF), XD_ALIGN_END);
    st.shadow = true;
    st.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    xd_draw_text(cr, xd_rect_inset4(r, xd_tpx(t, 8), 0, xd_tpx(t, 8), 0), clock, &st);
}

/* ---- menus and the toolkit ----------------------------------------------------------- */

static void aero_menu_palette([[maybe_unused]] const xd_theme *t, xd_menu_palette *p) {
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    *p = (xd_menu_palette){
        .flat = true,
        .background = xd_rgba_hex(0xF0F0F0),
        .border = xd_rgba_hex(0x979797),
        .text = xd_rgba_hex(0x000000),
        .hilight = xd_rgba_hex(0xDCE9F7),
        .hilight_text = xd_rgba_hex(0x000000),
        .hilight_border = xd_rgba_hex(0xAECFF7),
        .disabled_text = xd_rgba_hex(0x6D6D6D),
        .disabled_emboss = none,
        .separator_dark = xd_rgba_hex(0xE0E0E0),
        .separator_light = xd_rgba_hex(0xFFFFFF),
        .banner_top = xd_rgba_hex(0x1C3D68),
        .banner_bottom = xd_rgba_hex(0x4C8BCF),
        .banner_text = xd_rgba_hex(0xFFFFFF),
    };
}

static void aero_toolkit_colors([[maybe_unused]] const xd_theme *t, xd_toolkit_colors *c) {
    xd_rgba white = xd_rgba_hex(0xFFFFFF);
    *c = (xd_toolkit_colors){
        .bg = xd_rgba_hex(0xF0F0F0),
        .fg = xd_rgba_hex(0x000000),
        .base = white,
        .text = xd_rgba_hex(0x000000),
        .disabled = xd_rgba_hex(0x838383),
        .selected_bg = xd_rgba_hex(0x3399FF),
        .selected_fg = white,
        .tooltip_bg = xd_rgba_hex(0xFFFFFF),
        .tooltip_fg = xd_rgba_hex(0x575757),
        .menu_bg = xd_rgba_hex(0xF0F0F0),
        .menu_fg = xd_rgba_hex(0x000000),
        .menu_border = xd_rgba_hex(0x979797),
        .menu_hot = xd_rgba_hex(0xDCE9F7),
        .menu_hot_fg = xd_rgba_hex(0x000000),
        .menubar_bg = xd_rgba_hex(0xF0F0F0),
        .title_bg = xd_rgba_hex(0x74A8E0),
        .title_bg2 = xd_rgba_hex(0xA8CCF0),
        .title_fg = xd_rgba_hex(0x000000),
        .face = xd_rgba_hex(0xF0F0F0),
        .field_border = xd_rgba_hex(0xABADB3),
        .button_top = xd_rgba_hex(0xF2F2F2),
        .button_bottom = xd_rgba_hex(0xCFCFCF),
        .button_border = xd_rgba_hex(0x707070),
        .button_hot = xd_rgba_hex(0x3C7FB1),
        .button_focus = xd_rgba_hex(0x3399FF),
        .check_border = xd_rgba_hex(0x8E8F8F),
        .check_mark = xd_rgba_hex(0x1F1F1F),
        .thumb_top = xd_rgba_hex(0xF0F0F0),
        .thumb_bottom = xd_rgba_hex(0xD6D6D6),
        .track = xd_rgba_hex(0xF0F0F0),
        .tab_border = xd_rgba_hex(0x898C95),
        .tab_hot = xd_rgba_hex(0xBEE6FD),
    };
}

const xd_style_ops xd_style_aero = {
    .name = "Aero",
    .frame_insets = aero_frame_insets,
    .frame_layout = aero_frame_layout,
    .draw_frame = aero_draw_frame,
    .dialog_color = aero_dialog_color,
    .draw_button = aero_draw_button,
    .draw_checkbox = aero_draw_checkbox,
    .taskbar_height = aero_taskbar_height,
    .draw_taskbar = aero_draw_taskbar,
    .start_button_width = aero_start_button_width,
    .draw_start_button = aero_draw_start_button,
    .draw_task_button = aero_draw_task_button,
    .draw_tray = aero_draw_tray,
    .menu_palette = aero_menu_palette,
    .toolkit_colors = aero_toolkit_colors,
};
