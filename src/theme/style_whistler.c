/* SPDX-License-Identifier: MIT
 * Whistler ("Watercolor"): the flat look of the Windows XP pre-release
 * builds. Square corners, a flat blue caption that steps into a darker blue
 * behind the caption buttons through a pixel checkerboard, outlined caption
 * buttons, a three-pixel blue frame, flat grey-bordered controls on a pale
 * beige face, and a flat blue taskbar with a logo-only Start button.
 * Drawn procedurally; colours were matched against screenshots.
 */
#include "style.h"

/* Caption: a light step on the left, a darker one behind the buttons. */
static const uint32_t CAP_LIGHT = 0x5297F9, CAP_DARK = 0x3573D6, CAP_TOP = 0x7DB1FB,
                      CAP_BOTTOM = 0x4E91F0;
static const uint32_t CAP_LIGHT_INACTIVE = 0x9DB9E8, CAP_DARK_INACTIVE = 0x88A6DA,
                      CAP_TOP_INACTIVE = 0xB8CDF0, CAP_BOTTOM_INACTIVE = 0x97B3E3;
static const uint32_t TITLE_TEXT = 0xFFFFFF, TITLE_TEXT_INACTIVE = 0xE3ECF9;

/* The frame's three lines, outside to inside, per edge. */
static const uint32_t EDGE_LEFT[3] = {0x7DB0FA, 0x5196F8, 0x4E91EF};
static const uint32_t EDGE_TOP[3] = {0x7DB1FB, 0x5297F9, 0x5297F9};
static const uint32_t EDGE_RIGHT[3] = {0x3972CB, 0x3E7CDB, 0x71A2EC};
static const uint32_t EDGE_BOTTOM[3] = {0x6896E0, 0x3573D6, 0x2F67BF};

static const uint32_t FACE = 0xEBEBE4, BUTTON_FACE = 0xE5E7E1, BUTTON_BORDER = 0x808080,
                      BUTTON_SHADE = 0xBCBEB8, FIELD_BORDER = 0x646464, DEFAULT_BORDER = 0x6F89B0,
                      DEFAULT_RING_IN = 0xC7DCF6, DEFAULT_RING = 0xA8C8EB,
                      DISABLED_TEXT = 0xA0A0A0, DISABLED_BORDER = 0xC1C1BB;

static constexpr int CAPTION_H = 26; /* logical, including the top frame line */
static constexpr int BORDER = 3;
static constexpr int CAPBTN = 21;
static constexpr int TASKBAR_H = 30;
static constexpr double PI = 3.14159265358979323846;

static xd_rgba hex(uint32_t v) { return xd_rgba_hex(v); }

/* Inactive frames fade towards grey-blue. */
static xd_rgba edge(uint32_t v, bool active) {
    return active ? hex(v) : xd_rgba_mix(hex(v), hex(0xB4C3DD), 0.55);
}

static xd_insets whistler_frame_insets(const xd_theme *t, const xd_frame *f) {
    int b = f->maximized ? 0 : xd_tpx(t, BORDER);
    return (xd_insets){b, xd_tpx(t, CAPTION_H), b, b};
}

static void whistler_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                                  xd_frame_layout *l) {
    xd_insets in = whistler_frame_insets(t, f);
    l->caption = (xd_rect){outer.x, outer.y, outer.w, in.top};
    int bs = xd_tpx(t, CAPBTN);
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

/* The light-to-dark step: a mosaic of 4x5 tiles with 1px grout, brighter
 * tiles on the light side and light tiles straying into the dark side,
 * repeating every four rows (traced from Watercolor's caption). */
static void draw_checker_step(const xd_theme *t, cairo_t *cr, xd_rect band, int x0, bool active) {
    static const char *const rows[4] = {
        "+LLDLDD",
        "L+LLDDD",
        "LLLDLDD",
        "L+LLDLD",
    };
    int tw = XD_MAX(xd_tpx(t, 5), 2); /* tile pitch */
    int th = XD_MAX(xd_tpx(t, 6), 2);
    int g = XD_MAX(xd_tpx(t, 1), 1); /* grout */
    xd_rgba light = hex(active ? CAP_LIGHT : CAP_LIGHT_INACTIVE);
    xd_rgba dark = hex(active ? CAP_DARK : CAP_DARK_INACTIVE);
    xd_rgba bright = active ? hex(0x78A9FF) : xd_rgba_mix(light, hex(0xFFFFFF), 0.25);
    xd_rgba grout = xd_rgba_mix(light, dark, 0.5);
    int left = x0 - 3 * tw;
    cairo_save(cr);
    cairo_rectangle(cr, band.x, band.y, band.w, band.h);
    cairo_clip(cr);
    xd_fill_rect(cr, (xd_rect){left, band.y, 7 * tw, band.h}, grout);
    for (int r = 0, y = band.y + g; y < band.y + band.h; r++, y += th) {
        const char *row = rows[r % 4];
        for (int c = 0; c < 7; c++) {
            xd_rgba col = row[c] == '+' ? bright : row[c] == 'L' ? light : dark;
            xd_fill_rect(cr, (xd_rect){left + c * tw, y, tw - g, th - g}, col);
        }
    }
    cairo_restore(cr);
}

static void whistler_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer,
                                const xd_frame *f) {
    bool act = f->active;
    xd_insets in = whistler_frame_insets(t, f);
    int px = xd_tpx(t, 1);

    /* Frame: three lines per edge (none when maximized). */
    xd_fill_rect(cr, outer, edge(EDGE_LEFT[2], act));
    if (!f->maximized) {
        for (int i = 0; i < 3; i++) {
            int o = i * px;
            xd_rect r = {outer.x + o, outer.y + o, outer.w - 2 * o, outer.h - 2 * o};
            xd_fill_rect(cr, (xd_rect){r.x, r.y, r.w, px}, edge(EDGE_TOP[i], act));
            xd_fill_rect(cr, (xd_rect){r.x, r.y, px, r.h}, edge(EDGE_LEFT[i], act));
            xd_fill_rect(cr, (xd_rect){r.x + r.w - px, r.y, px, r.h}, edge(EDGE_RIGHT[i], act));
            xd_fill_rect(cr, (xd_rect){r.x, r.y + r.h - px, r.w, px}, edge(EDGE_BOTTOM[i], act));
        }
    }

    /* Caption. */
    xd_frame_layout l;
    xd_theme_frame_layout(t, outer, f, &l);
    xd_rect cap = {outer.x + in.left, outer.y + (f->maximized ? 0 : px), outer.w - in.left - in.right,
                   in.top - (f->maximized ? 0 : px)};
    xd_rgba light = hex(act ? CAP_LIGHT : CAP_LIGHT_INACTIVE);
    xd_rgba dark = hex(act ? CAP_DARK : CAP_DARK_INACTIVE);
    /* The dark step starts a little left of the caption buttons. */
    int buttons_left = cap.x + cap.w;
    for (size_t i = 0; i < l.button_count; i++)
        buttons_left = XD_MIN(buttons_left, l.button_rects[i].x);
    int step = XD_MAX(buttons_left - xd_tpx(t, 70), cap.x + cap.w / 2);
    xd_fill_rect(cr, (xd_rect){cap.x, cap.y, step - cap.x, cap.h}, light);
    xd_fill_rect(cr, (xd_rect){step, cap.y, cap.x + cap.w - step, cap.h}, dark);
    draw_checker_step(t, cr, cap, step, act);
    xd_fill_rect(cr, (xd_rect){cap.x, cap.y, cap.w, px}, hex(act ? CAP_TOP : CAP_TOP_INACTIVE));
    xd_fill_rect(cr, (xd_rect){cap.x, cap.y + cap.h - px, cap.w, px},
                 hex(act ? CAP_BOTTOM : CAP_BOTTOM_INACTIVE));

    /* Caption buttons: a light outline and glyph on the caption colour; the
     * help button is round. */
    xd_rgba outline = xd_rgba_alpha(hex(0xFFFFFF), act ? 0.5 : 0.4);
    xd_rgba glyph = xd_rgba_alpha(hex(0xFFFFFF), act ? 0.85 : 0.7);
    for (size_t i = 0; i < l.button_count; i++) {
        xd_frame_part part = l.button_parts[i];
        xd_rect b = l.button_rects[i];
        bool hot = f->hot == part;
        bool pressed = hot && f->pressed == part;
        double lw = px;
        if (part == XD_PART_HELP)
            cairo_arc(cr, b.x + b.w / 2.0, b.y + b.h / 2.0, b.w / 2.0 - lw / 2, 0, 2 * PI);
        else
            cairo_rectangle(cr, b.x + lw / 2, b.y + lw / 2, b.w - lw, b.h - lw);
        if (pressed || hot) {
            xd_set_color(cr, pressed ? xd_rgba_alpha(hex(0x000000), 0.18)
                                     : xd_rgba_alpha(hex(0xFFFFFF), 0.2));
            cairo_fill_preserve(cr);
        }
        xd_set_color(cr, outline);
        cairo_set_line_width(cr, lw);
        cairo_stroke(cr);
        xd_draw_glyph(t, cr, xd_rect_inset(b, xd_tpx(t, 5), xd_tpx(t, 5)),
                      xd_glyph_for_part(part, f->maximized), glyph, 1.4);
    }

    xd_rect ic = l.icon;
    xd_fill_rect(cr, ic, hex(0xFFFFFF));
    xd_fill_rect(cr, (xd_rect){ic.x, ic.y, ic.w, xd_tpx(t, 4)}, dark);

    static const xd_font_spec title_font = {
        .family = "Tahoma, DejaVu Sans, sans-serif", .size_pt = 9, .bold = true};
    xd_text_style st = {
        .font = &title_font,
        .scale = t->scale,
        .color = hex(act ? TITLE_TEXT : TITLE_TEXT_INACTIVE),
        .align = XD_ALIGN_START,
        .ellipsize = true,
        .smoothing = t->scheme.font_smoothing,
    };
    xd_draw_text(cr, l.title, f->title, &st);
}

void xd_theme_draw_whistler_mosaic(const xd_theme *t, cairo_t *cr, xd_rect band, int x) {
    draw_checker_step(t, cr, band, x, true);
}

static xd_rgba whistler_dialog_color([[maybe_unused]] const xd_theme *t) { return hex(FACE); }

/* Flat button: grey border, white top/left inside, a shaded bottom. The
 * default (or focused, or hot) button gets a light blue ring. */
static void whistler_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                 const char *label) {
    int px = xd_tpx(t, 1);
    bool disabled = state & XD_STATE_DISABLED;
    bool pressed = state & XD_STATE_PRESSED;
    xd_fill_rect(cr, r, hex(pressed ? 0xD8DAD4 : BUTTON_FACE));
    bool ring = !disabled && !pressed &&
                (state & (XD_STATE_HOT | XD_STATE_FOCUSED | XD_STATE_DEFAULT));
    xd_rgba border = hex(disabled ? DISABLED_BORDER : ring ? DEFAULT_BORDER : BUTTON_BORDER);
    xd_rect in = xd_draw_bevel(cr, r, px, border, border);
    if (ring) {
        in = xd_draw_bevel(cr, in, px, hex(DEFAULT_RING_IN), hex(DEFAULT_RING));
        xd_draw_bevel(cr, in, 2 * px, hex(DEFAULT_RING), hex(0x9BBCE5));
    } else if (!pressed) {
        xd_fill_rect(cr, (xd_rect){in.x, in.y, in.w, px}, hex(0xFFFFFF));
        xd_fill_rect(cr, (xd_rect){in.x, in.y, px, in.h}, hex(0xFFFFFF));
        xd_fill_rect(cr, (xd_rect){in.x, in.y + in.h - px, in.w, px}, hex(BUTTON_SHADE));
        xd_fill_rect(cr, (xd_rect){in.x, in.y + in.h - 2 * px, in.w, px}, hex(0xDEE0DA));
        xd_fill_rect(cr, (xd_rect){in.x + in.w - px, in.y, px, in.h}, hex(BUTTON_SHADE));
    }
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               disabled ? hex(DISABLED_TEXT) : xd_sys(t, XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_CENTER);
    xd_rect text = pressed ? (xd_rect){r.x + px, r.y + px, r.w, r.h} : r;
    xd_draw_text(cr, text, label, &st);
}

static void whistler_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                   const char *label) {
    int box = xd_tpx(t, 13);
    xd_rect b = {r.x, r.y + (r.h - box) / 2, box, box};
    bool disabled = state & XD_STATE_DISABLED;
    xd_fill_rect(cr, b, hex(disabled ? FACE : 0xFFFFFF));
    xd_rect well = xd_draw_bevel(cr, b, xd_tpx(t, 1), hex(disabled ? DISABLED_BORDER : FIELD_BORDER),
                                 hex(disabled ? DISABLED_BORDER : FIELD_BORDER));
    if (!disabled && (state & XD_STATE_HOT))
        xd_draw_bevel(cr, well, xd_tpx(t, 1), hex(DEFAULT_RING), hex(DEFAULT_RING));
    if (state & XD_STATE_CHECKED)
        xd_draw_glyph(t, cr, xd_rect_inset(well, xd_tpx(t, 1), xd_tpx(t, 1)), XD_GLYPH_CHECK,
                      disabled ? hex(DISABLED_TEXT) : hex(0x000000), 1.2);
    int gap = xd_tpx(t, 5);
    xd_text_style st = xd_text(t, XD_FONT_MESSAGE,
                               disabled ? hex(DISABLED_TEXT) : xd_sys(t, XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    xd_draw_text(cr, (xd_rect){b.x + box + gap, r.y, r.w - box - gap, r.h}, label, &st);
}

/* ---- taskbar ------------------------------------------------------------------------ */

static const uint32_t BAR = 0x5297F9, BAR_TOP = 0x7DB1FB, BAR_EDGE = 0x3573D6,
                      START = 0x2E68CF, TRAY = 0x3F88E4, TASK_ACTIVE = 0x3A7BE0,
                      TASK_HOT = 0x66A6FB;

static int whistler_taskbar_height(const xd_theme *t) { return xd_tpx(t, TASKBAR_H); }

static void whistler_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge) {
    int px = xd_tpx(t, 1);
    xd_fill_rect(cr, r, hex(BAR));
    xd_fill_rect(cr, xd_style_rim_line(r, edge, 0, px), hex(BAR_EDGE));
    xd_fill_rect(cr, xd_style_rim_line(r, edge, px, px), hex(BAR_TOP));
}

/* Logo only, on a darker square, as in the pre-release builds. */
static int whistler_start_button_width(const xd_theme *t, [[maybe_unused]] cairo_t *cr) {
    return xd_tpx(t, 40);
}

static void whistler_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                       uint8_t state) {
    int px = xd_tpx(t, 1);
    /* Flat on the bar at rest, lighter under the pointer, dark blue while
     * pressed or while the Start menu is open. */
    bool pressed = state & XD_STATE_PRESSED;
    xd_rect b = {r.x, r.y + 2 * px, r.w, r.h - 2 * px};
    if (pressed) {
        xd_fill_rect(cr, b, hex(START));
        xd_fill_rect(cr, (xd_rect){b.x, b.y, b.w, px}, hex(0x2459B8));
        xd_fill_rect(cr, (xd_rect){b.x, b.y, px, b.h}, hex(0x2459B8));
    } else if (state & XD_STATE_HOT) {
        xd_fill_rect(cr, b, hex(TASK_HOT));
    }
    xd_fill_rect(cr, (xd_rect){b.x + b.w - px, b.y, px, b.h}, hex(BAR_TOP));
    xd_fill_rect(cr, (xd_rect){b.x + b.w - 2 * px, b.y, px, b.h}, hex(BAR_EDGE));
    int icon = xd_tpx(t, 22);
    int dy = pressed ? px : 0;
    xd_draw_start_logo(cr, (xd_rect){b.x + (b.w - icon) / 2, b.y + (b.h - icon) / 2 + dy, icon,
                                     icon});
}

/* Flat task buttons separated by a dark and a light line. */
static void whistler_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                      const char *label, cairo_surface_t *window_icon) {
    int px = xd_tpx(t, 1);
    bool active = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    if (active)
        xd_fill_rect(cr, r, hex(TASK_ACTIVE));
    else if (state & XD_STATE_FLASH)
        xd_fill_rect(cr, r, hex(0xE8801E));
    else if (state & XD_STATE_HOT)
        xd_fill_rect(cr, r, hex(TASK_HOT));
    if (active) {
        xd_fill_rect(cr, (xd_rect){r.x, r.y, r.w, px}, hex(0x2459B8));
        xd_fill_rect(cr, (xd_rect){r.x, r.y, px, r.h}, hex(0x2459B8));
    }
    xd_fill_rect(cr, (xd_rect){r.x + r.w - 2 * px, r.y + 2 * px, px, r.h - 4 * px}, hex(BAR_EDGE));
    xd_fill_rect(cr, (xd_rect){r.x + r.w - px, r.y + 2 * px, px, r.h - 4 * px}, hex(BAR_TOP));

    int icon = xd_tpx(t, 16);
    int ix = r.x + xd_tpx(t, 6);
    int iy = r.y + (r.h - icon) / 2;
    xd_style_task_icon(t, cr, window_icon, ix, iy);
    xd_text_style st = xd_text(t, XD_FONT_MENU, hex(0xFFFFFF), XD_ALIGN_START);
    int tx = ix + icon + xd_tpx(t, 5);
    xd_draw_text(cr, (xd_rect){tx, r.y, r.x + r.w - tx - xd_tpx(t, 6), r.h}, label, &st);
}

static void whistler_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock) {
    int px = xd_tpx(t, 1);
    xd_rect b = {r.x, r.y + 2 * px, r.w, r.h - 2 * px};
    xd_fill_rect(cr, b, hex(TRAY));
    xd_fill_rect(cr, (xd_rect){b.x, b.y, px, b.h}, hex(BAR_EDGE));
    xd_fill_rect(cr, (xd_rect){b.x + px, b.y, px, b.h}, hex(BAR_TOP));
    xd_text_style st = xd_text(t, XD_FONT_MENU, hex(0xFFFFFF), XD_ALIGN_END);
    xd_draw_text(cr, xd_rect_inset(b, xd_tpx(t, 10), 0), clock, &st);
}

/* ---- menus and toolkit colours ------------------------------------------------------ */

static void whistler_menu_palette([[maybe_unused]] const xd_theme *t, xd_menu_palette *p) {
    xd_rgba none = xd_rgba_alpha(hex(0), 0);
    *p = (xd_menu_palette){
        .flat = true,
        .background = hex(0xFFFFFF),
        .border = hex(BUTTON_BORDER),
        .text = hex(0x000000),
        .hilight = hex(0x316AC5),
        .hilight_text = hex(0xFFFFFF),
        .hilight_border = none,
        .disabled_text = hex(DISABLED_TEXT),
        .disabled_emboss = none,
        .separator_dark = hex(DISABLED_BORDER),
        .separator_light = none,
        .banner_top = hex(CAP_DARK),
        .banner_bottom = hex(CAP_LIGHT),
        .banner_text = hex(0xFFFFFF),
    };
}

static void whistler_toolkit_colors([[maybe_unused]] const xd_theme *t, xd_toolkit_colors *c) {
    xd_rgba white = hex(0xFFFFFF);
    *c = (xd_toolkit_colors){
        .bg = hex(FACE),
        .fg = hex(0x000000),
        .base = white,
        .text = hex(0x000000),
        .disabled = hex(DISABLED_TEXT),
        .selected_bg = hex(0x316AC5),
        .selected_fg = white,
        .tooltip_bg = hex(0xFFFFE1),
        .tooltip_fg = hex(0x000000),
        .menu_bg = white,
        .menu_fg = hex(0x000000),
        .menu_border = hex(BUTTON_BORDER),
        .menu_hot = hex(0x316AC5),
        .menu_hot_fg = white,
        .menubar_bg = hex(FACE),
        .title_bg = hex(CAP_LIGHT),
        .title_bg2 = hex(CAP_DARK),
        .title_fg = white,
        .face = hex(FACE),
        .field_border = hex(FIELD_BORDER),
        .button_top = hex(BUTTON_FACE),
        .button_bottom = hex(BUTTON_FACE),
        .button_border = hex(BUTTON_BORDER),
        .button_hot = hex(DEFAULT_RING),
        .button_focus = hex(DEFAULT_RING),
        .check_border = hex(FIELD_BORDER),
        .check_mark = hex(0x000000),
        .thumb_top = white,
        .thumb_bottom = white,
        .track = hex(FACE),
        .tab_border = white,
        .tab_hot = hex(DEFAULT_RING),
    };
}

const xd_style_ops xd_style_whistler = {
    .name = "Whistler",
    .frame_insets = whistler_frame_insets,
    .frame_layout = whistler_frame_layout,
    .draw_frame = whistler_draw_frame,
    .dialog_color = whistler_dialog_color,
    .draw_button = whistler_draw_button,
    .draw_checkbox = whistler_draw_checkbox,
    .taskbar_height = whistler_taskbar_height,
    .draw_taskbar = whistler_draw_taskbar,
    .start_button_width = whistler_start_button_width,
    .draw_start_button = whistler_draw_start_button,
    .draw_task_button = whistler_draw_task_button,
    .draw_tray = whistler_draw_tray,
    .menu_palette = whistler_menu_palette,
    .toolkit_colors = whistler_toolkit_colors,
};
