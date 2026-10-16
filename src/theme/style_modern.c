/* SPDX-License-Identifier: MIT
 * Rounded, gradient-based visual styles: Luna (XP) and Whistler.
 *
 * Both share one renderer driven by a palette, so a new style in this
 * family is just another palette. All artwork is drawn procedurally; no
 * bitmaps from any other product are used.
 */
#include "style.h"

typedef struct stops {
    double at[5];
    uint32_t rgb[5];
    size_t n;
} stops;

typedef struct modern_palette {
    const char *name;
    int caption_h;    /* logical px, includes the top frame edge */
    int border;       /* logical px, sides and bottom */
    double corner;    /* logical radius of the top window corners */
    stops caption_active, caption_inactive;
    uint32_t frame_active, frame_inactive, frame_outline;
    uint32_t title_active, title_inactive, title_shadow;
    xd_font_spec title_font;

    uint32_t dialog;
    stops button_face;
    uint32_t button_outline, button_hot, button_focus;

    stops capbtn, capbtn_close;
    uint32_t capbtn_border, capbtn_glyph;

    uint32_t check_border, check_mark;

    int taskbar_h;
    stops taskbar, tray, start, task, task_active;
    uint32_t tray_edge, taskbar_text, start_text;
    xd_font_spec start_font;
    double start_radius; /* rounded right side; 0 for a pill-less button */
    const char *start_label;

    uint32_t menu_bg, menu_border, menu_text, menu_hilight, menu_hilight_text,
        menu_hilight_border, menu_disabled, menu_separator, banner_top, banner_bottom;
} modern_palette;

static void fill_stops(cairo_t *cr, xd_rect r, const stops *s) {
    xd_rgba colors[5];
    for (size_t i = 0; i < s->n; i++)
        colors[i] = xd_rgba_hex(s->rgb[i]);
    xd_fill_vstops(cr, r, s->at, colors, s->n);
}

/* Fills the current path with a vertical stop gradient spanning r. */
static void fill_path_stops(cairo_t *cr, xd_rect r, const stops *s) {
    cairo_pattern_t *p = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
    for (size_t i = 0; i < s->n; i++) {
        xd_rgba c = xd_rgba_hex(s->rgb[i]);
        cairo_pattern_add_color_stop_rgb(p, s->at[i], c.r, c.g, c.b);
    }
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

static const modern_palette *pal(const xd_theme *t);
static void modern_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                                xd_frame_layout *l);

static xd_insets modern_frame_insets(const xd_theme *t, const xd_frame *f) {
    const modern_palette *m = pal(t);
    int b = f->maximized ? 0 : xd_tpx(t, m->border);
    return (xd_insets){b, xd_tpx(t, m->caption_h), b, b};
}

static void rounded_button(const xd_theme *t, cairo_t *cr, xd_rect r, const stops *face,
                           uint32_t border, bool pressed) {
    double rad = xd_tpx(t, 3);
    double lw = xd_tpx(t, 1);
    xd_path_rounded(cr, r.x + lw / 2, r.y + lw / 2, r.w - lw, r.h - lw, rad, rad, rad, rad);
    cairo_save(cr);
    fill_path_stops(cr, r, face);
    cairo_restore(cr);
    if (pressed) {
        xd_path_rounded(cr, r.x + lw / 2, r.y + lw / 2, r.w - lw, r.h - lw, rad, rad, rad,
                        rad);
        xd_set_color(cr, xd_rgba_alpha(xd_rgba_hex(0x000000), 0.18));
        cairo_fill(cr);
    }
    xd_path_rounded(cr, r.x + lw / 2, r.y + lw / 2, r.w - lw, r.h - lw, rad, rad, rad, rad);
    xd_set_color(cr, xd_rgba_hex(border));
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);
}

static void modern_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer,
                              const xd_frame *f) {
    const modern_palette *m = pal(t);
    xd_insets in = modern_frame_insets(t, f);
    double corner = f->maximized ? 0 : xd_tpx(t, m->corner);
    xd_rgba frame = xd_rgba_hex(f->active ? m->frame_active : m->frame_inactive);

    cairo_save(cr);
    xd_path_rounded(cr, outer.x, outer.y, outer.w, outer.h, corner, corner, 0, 0);
    cairo_clip(cr);

    /* Sides and bottom. */
    xd_fill_rect(cr, outer, frame);
    xd_rect cap = {outer.x, outer.y, outer.w, in.top};
    fill_stops(cr, cap, f->active ? &m->caption_active : &m->caption_inactive);

    /* Darker outline around the whole frame. */
    if (!f->maximized) {
        double lw = xd_tpx(t, 1);
        xd_path_rounded(cr, outer.x + lw / 2, outer.y + lw / 2, outer.w - lw, outer.h - lw,
                        corner, corner, 0, 0);
        xd_set_color(cr, xd_rgba_alpha(xd_rgba_hex(m->frame_outline), f->active ? 1 : 0.5));
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);
    }
    cairo_restore(cr);

    xd_frame_layout l;
    xd_theme_frame_layout(t, outer, f, &l);
    double alpha = f->active ? 1.0 : 0.6;

    for (size_t i = 0; i < l.button_count; i++) {
        xd_frame_part part = l.button_parts[i];
        xd_rect b = l.button_rects[i];
        bool hot = f->hot == part;
        bool pressed = hot && f->pressed == part;
        cairo_push_group(cr);
        rounded_button(t, cr, b, part == XD_PART_CLOSE ? &m->capbtn_close : &m->capbtn,
                       m->capbtn_border, pressed);
        if (hot && !pressed) {
            double rad = xd_tpx(t, 3);
            xd_path_rounded(cr, b.x, b.y, b.w, b.h, rad, rad, rad, rad);
            xd_set_color(cr, xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.25));
            cairo_fill(cr);
        }
        xd_draw_glyph(t, cr, xd_rect_inset(b, xd_tpx(t, 5), xd_tpx(t, 5)),
                      xd_glyph_for_part(part, f->maximized), xd_rgba_hex(m->capbtn_glyph), 1.4);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, alpha);
    }

    xd_rect ic = l.icon;
    xd_fill_rect(cr, ic, xd_rgba_hex(0xFFFFFF));
    xd_fill_rect(cr, (xd_rect){ic.x, ic.y, ic.w, xd_tpx(t, 4)}, xd_rgba_hex(0x2D63D8));

    xd_text_style st = {
        .font = &m->title_font,
        .scale = t->scale,
        .color = xd_rgba_hex(f->active ? m->title_active : m->title_inactive),
        .align = XD_ALIGN_START,
        .ellipsize = true,
        .shadow = f->active && m->title_shadow != m->title_active,
        .shadow_color = xd_rgba_hex(m->title_shadow),
        .smoothing = t->scheme.font_smoothing,
    };
    xd_draw_text(cr, l.title, f->title, &st);
}

static void modern_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                                xd_frame_layout *l) {
    xd_insets in = modern_frame_insets(t, f);
    l->caption = (xd_rect){outer.x, outer.y, outer.w, in.top};

    int bs = xd_tpx(t, 21);
    int by = outer.y + (in.top - bs) / 2 + xd_tpx(t, 1);
    int bx = outer.x + outer.w - in.right - xd_tpx(t, 2);
    int gap = xd_tpx(t, 2);
    static const struct {
        uint8_t flag;
        xd_frame_part part;
    } order[] = {
        {XD_CAPBTN_CLOSE, XD_PART_CLOSE},
        {XD_CAPBTN_MAXIMIZE, XD_PART_MAXIMIZE},
        {XD_CAPBTN_MINIMIZE, XD_PART_MINIMIZE},
        {XD_CAPBTN_HELP, XD_PART_HELP},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(order); i++) {
        if (!(f->buttons & order[i].flag))
            continue;
        bx -= bs;
        xd_layout_add_button(l, (xd_rect){bx, by, bs, bs}, order[i].part);
        bx -= gap;
    }

    int icon = xd_tpx(t, 16);
    l->icon = (xd_rect){outer.x + in.left + xd_tpx(t, 3),
                        outer.y + (in.top - icon) / 2 + xd_tpx(t, 1), icon, icon};
    int tx = l->icon.x + icon + xd_tpx(t, 4);
    l->title = (xd_rect){tx, outer.y + xd_tpx(t, 1), bx - tx, in.top};
}

static xd_rgba modern_dialog_color(const xd_theme *t) { return xd_rgba_hex(pal(t)->dialog); }

static void modern_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label) {
    const modern_palette *m = pal(t);
    bool disabled = state & XD_STATE_DISABLED;
    rounded_button(t, cr, r, &m->button_face, disabled ? 0xC9C7BA : m->button_outline,
                   state & XD_STATE_PRESSED);

    uint32_t ring = 0;
    if (!disabled && (state & XD_STATE_HOT))
        ring = m->button_hot;
    else if (!disabled && (state & (XD_STATE_FOCUSED | XD_STATE_DEFAULT)))
        ring = m->button_focus;
    if (ring != 0) {
        double lw = xd_tpx(t, 2);
        double o = xd_tpx(t, 1) + lw / 2;
        double rad = xd_tpx(t, 2);
        xd_path_rounded(cr, r.x + o, r.y + o, r.w - 2 * o, r.h - 2 * o, rad, rad, rad, rad);
        xd_set_color(cr, xd_rgba_hex(ring));
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);
    }

    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               disabled ? xd_rgba_hex(0xA1A192) : xd_sys(t, XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_CENTER);
    xd_draw_text(cr, r, label, &st);
}

static void modern_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                 const char *label) {
    const modern_palette *m = pal(t);
    int box = xd_tpx(t, 13);
    xd_rect b = {r.x, r.y + (r.h - box) / 2, box, box};
    bool disabled = state & XD_STATE_DISABLED;

    xd_rect well = xd_draw_bevel(cr, b, xd_tpx(t, 1),
                                 xd_rgba_hex(disabled ? 0xCAC8BB : m->check_border),
                                 xd_rgba_hex(disabled ? 0xCAC8BB : m->check_border));
    xd_fill_gradient(cr, well, xd_rgba_hex(disabled ? 0xFFFFFF : 0xDCDCD7),
                     xd_rgba_hex(0xFFFFFF), false);
    if (!disabled && (state & XD_STATE_HOT))
        xd_draw_bevel(cr, well, xd_tpx(t, 2), xd_rgba_hex(m->button_hot),
                      xd_rgba_hex(m->button_hot));
    if (state & XD_STATE_CHECKED)
        xd_draw_glyph(t, cr, xd_rect_inset(well, xd_tpx(t, 1), xd_tpx(t, 1)), XD_GLYPH_CHECK,
                      xd_rgba_hex(disabled ? 0xCAC8BB : m->check_mark), 1.2);

    int gap = xd_tpx(t, 5);
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               disabled ? xd_rgba_hex(0xA1A192) : xd_sys(t, XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    xd_draw_text(cr, (xd_rect){b.x + box + gap, r.y, r.w - box - gap, r.h}, label, &st);
}

static int modern_taskbar_height(const xd_theme *t) { return xd_tpx(t, pal(t)->taskbar_h); }

static void modern_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r) {
    fill_stops(cr, r, &pal(t)->taskbar);
}

static xd_text_style start_style(const xd_theme *t) {
    const modern_palette *m = pal(t);
    return (xd_text_style){
        .font = &m->start_font,
        .scale = t->scale,
        .color = xd_rgba_hex(m->start_text),
        .align = XD_ALIGN_START,
        .shadow = true,
        .shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.35),
        .smoothing = t->scheme.font_smoothing,
    };
}

static int modern_start_button_width(const xd_theme *t, cairo_t *cr) {
    xd_text_style st = start_style(t);
    return xd_tpx(t, 10 + 20 + 6 + 22) + xd_text_width(cr, pal(t)->start_label, &st);
}

static void modern_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                     uint8_t state) {
    const modern_palette *m = pal(t);
    double rad = XD_MIN(xd_tpx(t, m->start_radius), r.h / 2);
    xd_path_rounded(cr, r.x, r.y, r.w, r.h, 0, rad, rad, 0);
    cairo_save(cr);
    fill_path_stops(cr, r, &m->start);
    cairo_restore(cr);
    if (state & (XD_STATE_PRESSED | XD_STATE_HOT)) {
        xd_path_rounded(cr, r.x, r.y, r.w, r.h, 0, rad, rad, 0);
        xd_set_color(cr, (state & XD_STATE_PRESSED) ? xd_rgba_alpha(xd_rgba_hex(0x000000), 0.2)
                                                    : xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.15));
        cairo_fill(cr);
    }

    int icon = xd_tpx(t, 20);
    int ix = r.x + xd_tpx(t, 10);
    xd_draw_start_logo(cr, (xd_rect){ix, r.y + (r.h - icon) / 2, icon, icon});
    xd_text_style st = start_style(t);
    int tx = ix + icon + xd_tpx(t, 6);
    xd_draw_text(cr, (xd_rect){tx, r.y, r.x + r.w - tx, r.h}, m->start_label, &st);
}

static void modern_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                    uint8_t state, const char *label) {
    const modern_palette *m = pal(t);
    bool active = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    double rad = xd_tpx(t, 3);
    xd_path_rounded(cr, r.x, r.y, r.w, r.h, rad, rad, rad, rad);
    cairo_save(cr);
    fill_path_stops(cr, r, active ? &m->task_active : &m->task);
    cairo_restore(cr);

    int icon = xd_tpx(t, 16);
    int ix = r.x + xd_tpx(t, 6);
    xd_fill_rect(cr, (xd_rect){ix, r.y + (r.h - icon) / 2, icon, icon}, xd_rgba_hex(0xFFFFFF));
    xd_fill_rect(cr, (xd_rect){ix, r.y + (r.h - icon) / 2, icon, xd_tpx(t, 4)},
                 xd_rgba_hex(0x2D63D8));
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(m->taskbar_text), XD_ALIGN_START);
    int tx = ix + icon + xd_tpx(t, 5);
    xd_draw_text(cr, (xd_rect){tx, r.y, r.x + r.w - tx - xd_tpx(t, 4), r.h}, label, &st);
}

static void modern_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock) {
    const modern_palette *m = pal(t);
    fill_stops(cr, r, &m->tray);
    xd_fill_rect(cr, (xd_rect){r.x, r.y, xd_tpx(t, 1), r.h}, xd_rgba_hex(m->tray_edge));
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_rgba_hex(m->taskbar_text), XD_ALIGN_END);
    xd_draw_text(cr, xd_rect_inset(r, xd_tpx(t, 10), 0), clock, &st);
}

/* ---- palettes ----------------------------------------------------------- */

#define STOPS2(a, b) {.at = {0, 1}, .rgb = {a, b}, .n = 2}
#define STOPS3(a, oa, b, c) {.at = {0, oa, 1}, .rgb = {a, b, c}, .n = 3}
#define STOPS4(a, oa, b, ob, c, d) {.at = {0, oa, ob, 1}, .rgb = {a, b, c, d}, .n = 4}

static const modern_palette luna = {
    .name = "Luna",
    .caption_h = 30,
    .border = 4,
    .corner = 7,
    .caption_active = STOPS4(0x3D95FF, 0.1, 0x0A64F0, 0.55, 0x0055E5, 0x0047C8),
    .caption_inactive = STOPS4(0xA9C2F0, 0.1, 0x8FAEE8, 0.55, 0x7D9EE0, 0x7393D6),
    .frame_active = 0x0855DD,
    .frame_inactive = 0x7B9BE0,
    .frame_outline = 0x00138C,
    .title_active = 0xFFFFFF,
    .title_inactive = 0xD8E4F8,
    .title_shadow = 0x0A1883,
    .title_font = {.family = "Trebuchet MS, DejaVu Sans, sans-serif", .size_pt = 10, .bold = true},

    .dialog = 0xECE9D8,
    .button_face = STOPS3(0xFFFFFF, 0.85, 0xECEBE6, 0xD6D0C5),
    .button_outline = 0x003C74,
    .button_hot = 0xF8B330,
    .button_focus = 0x7DA2CE,

    .capbtn = STOPS3(0x6F9DF9, 0.5, 0x2E6BEB, 0x1F55D6),
    .capbtn_close = STOPS3(0xF0937A, 0.5, 0xE0532A, 0xC63A12),
    .capbtn_border = 0xFFFFFF,
    .capbtn_glyph = 0xFFFFFF,

    .check_border = 0x1C5180,
    .check_mark = 0x21A121,

    .taskbar_h = 30,
    .taskbar = STOPS4(0x3A80F3, 0.08, 0x5EA0F8, 0.2, 0x2E6FE5, 0x1F4FC2),
    .tray = STOPS3(0x1EA4F4, 0.2, 0x0F8DE6, 0x0C6CCE),
    .tray_edge = 0x1042AF,
    .start = STOPS4(0x6BC56B, 0.15, 0x3C9B3C, 0.8, 0x2F8A2F, 0x206F20),
    .task = STOPS3(0x5C97F7, 0.5, 0x3C81F3, 0x3274E8),
    .task_active = STOPS3(0x1A49AF, 0.5, 0x1E52B7, 0x2459C4),
    .taskbar_text = 0xFFFFFF,
    .start_text = 0xFFFFFF,
    .start_font = {.family = "Trebuchet MS, DejaVu Sans, sans-serif", .size_pt = 13, .bold = true,
                   .italic = true},
    .start_radius = 12,
    .start_label = "start",
    .menu_bg = 0xFFFFFF,
    .menu_border = 0xACA899,
    .menu_text = 0x000000,
    .menu_hilight = 0x316AC5,
    .menu_hilight_text = 0xFFFFFF,
    .menu_disabled = 0xACA899,
    .menu_separator = 0xACA899,
    .banner_top = 0x0A246A,
    .banner_bottom = 0x3A6EE8,
};

/* Whistler: the pale, silvery look of the pre-release builds. */
static const modern_palette whistler = {
    .name = "Whistler",
    .caption_h = 28,
    .border = 4,
    .corner = 5,
    .caption_active = STOPS4(0xF4F7FC, 0.12, 0xD9E3F2, 0.6, 0xC0CFE6, 0xA9BCDA),
    .caption_inactive = STOPS3(0xF4F4F4, 0.6, 0xE2E2E2, 0xD4D4D4),
    .frame_active = 0xB3C3DD,
    .frame_inactive = 0xD6D6D6,
    .frame_outline = 0x5F7598,
    .title_active = 0x10265A,
    .title_inactive = 0x8A8A8A,
    .title_shadow = 0x10265A,
    .title_font = {.family = "Tahoma, DejaVu Sans, sans-serif", .size_pt = 9, .bold = true},

    .dialog = 0xEFF1F5,
    .button_face = STOPS3(0xFFFFFF, 0.7, 0xEEF1F6, 0xD7DEEA),
    .button_outline = 0x5F7598,
    .button_hot = 0x8FB2E8,
    .button_focus = 0xA9BCDA,

    .capbtn = STOPS3(0xFFFFFF, 0.5, 0xE4EAF4, 0xC9D5E8),
    .capbtn_close = STOPS3(0xFFFFFF, 0.5, 0xF1DCDC, 0xE3B9B9),
    .capbtn_border = 0x7387A8,
    .capbtn_glyph = 0x24365E,

    .check_border = 0x5F7598,
    .check_mark = 0x24365E,

    .taskbar_h = 30,
    .taskbar = STOPS4(0xFFFFFF, 0.08, 0xE9EEF6, 0.5, 0xD5DEEC, 0xBCC9DE),
    .tray = STOPS2(0xE3E9F3, 0xC9D4E6),
    .tray_edge = 0x9AAAC6,
    .start = STOPS3(0xFFFFFF, 0.5, 0xE6ECF6, 0xBFCCE2),
    .task = STOPS2(0xFAFBFD, 0xDCE3EF),
    .task_active = STOPS2(0xC4D1E6, 0xDDE4F0),
    .taskbar_text = 0x10265A,
    .start_text = 0x10265A,
    .start_font = {.family = "Tahoma, DejaVu Sans, sans-serif", .size_pt = 10, .bold = true},
    .start_radius = 6,
    .start_label = "Start",
    .menu_bg = 0xFAFBFD,
    .menu_border = 0x8D9DBF,
    .menu_text = 0x10265A,
    .menu_hilight = 0xD6E0F1,
    .menu_hilight_text = 0x10265A,
    .menu_hilight_border = 0x8FA6CF,
    .menu_disabled = 0xA0A8B8,
    .menu_separator = 0xC3CCDD,
    .banner_top = 0x5F7598,
    .banner_bottom = 0xA9BCDA,
};

static const modern_palette *pal(const xd_theme *t) {
    return t->style == XD_STYLE_WHISTLER ? &whistler : &luna;
}

static void modern_menu_palette(const xd_theme *t, xd_menu_palette *p) {
    const modern_palette *m = pal(t);
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    *p = (xd_menu_palette){
        .flat = true,
        .background = xd_rgba_hex(m->menu_bg),
        .border = xd_rgba_hex(m->menu_border),
        .text = xd_rgba_hex(m->menu_text),
        .hilight = xd_rgba_hex(m->menu_hilight),
        .hilight_text = xd_rgba_hex(m->menu_hilight_text),
        .hilight_border = m->menu_hilight_border != 0 ? xd_rgba_hex(m->menu_hilight_border) : none,
        .disabled_text = xd_rgba_hex(m->menu_disabled),
        .disabled_emboss = none,
        .separator_dark = xd_rgba_hex(m->menu_separator),
        .separator_light = none,
        .banner_top = xd_rgba_hex(m->banner_top),
        .banner_bottom = xd_rgba_hex(m->banner_bottom),
        .banner_text = xd_rgba_hex(0xFFFFFF),
    };
}

#define MODERN_OPS(sym, label)                                                 \
    const xd_style_ops sym = {                                                 \
        .name = label,                                                         \
        .frame_insets = modern_frame_insets,                                   \
        .frame_layout = modern_frame_layout,                                   \
        .draw_frame = modern_draw_frame,                                       \
        .dialog_color = modern_dialog_color,                                   \
        .draw_button = modern_draw_button,                                     \
        .draw_checkbox = modern_draw_checkbox,                                 \
        .taskbar_height = modern_taskbar_height,                               \
        .draw_taskbar = modern_draw_taskbar,                                   \
        .start_button_width = modern_start_button_width,                       \
        .draw_start_button = modern_draw_start_button,                         \
        .draw_task_button = modern_draw_task_button,                           \
        .draw_tray = modern_draw_tray,                                         \
        .menu_palette = modern_menu_palette,                                   \
    }

MODERN_OPS(xd_style_luna, "Luna");
MODERN_OPS(xd_style_whistler, "Whistler");
