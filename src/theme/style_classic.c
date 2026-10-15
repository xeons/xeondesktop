/* SPDX-License-Identifier: MIT
 * Windows 2000 "Classic" visual style. Every colour comes from the scheme,
 * so all classic colour schemes (Standard, Brick, High Contrast, ...) work.
 */
#include "style.h"

/* EDGE_RAISED as used by DrawFrameControl(DFCS_BUTTONPUSH). */
static xd_rect edge_button(const xd_theme *t, cairo_t *cr, xd_rect r, bool pressed) {
    int p = xd_tpx(t, 1);
    if (pressed) {
        r = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW),
                          xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
        return xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                             xd_sys(t, XD_COLOR_BUTTON_LIGHT));
    }
    r = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
    return xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_LIGHT),
                         xd_sys(t, XD_COLOR_BUTTON_SHADOW));
}

/* Window and caption-button edge: light outside, white inside. */
static xd_rect edge_window(const xd_theme *t, cairo_t *cr, xd_rect r, bool pressed) {
    int p = xd_tpx(t, 1);
    if (pressed) {
        r = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW),
                          xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
        return xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                             xd_sys(t, XD_COLOR_BUTTON_LIGHT));
    }
    r = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_LIGHT),
                      xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
    return xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                         xd_sys(t, XD_COLOR_BUTTON_SHADOW));
}

/* EDGE_SUNKEN (checkbox wells, text fields). */
static xd_rect edge_sunken(const xd_theme *t, cairo_t *cr, xd_rect r) {
    int p = xd_tpx(t, 1);
    r = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                      xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    return xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW),
                         xd_sys(t, XD_COLOR_BUTTON_LIGHT));
}

/* Win2000's 50% checkerboard of ButtonHilight over ButtonFace, used for
 * pressed task buttons and the like. */
static void fill_dither(const xd_theme *t, cairo_t *cr, xd_rect r) {
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    cairo_surface_t *pat_s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
    cairo_t *pc = cairo_create(pat_s);
    xd_rgba hi = xd_sys(t, XD_COLOR_BUTTON_HILIGHT);
    xd_fill_rect(pc, (xd_rect){0, 0, 1, 1}, hi);
    xd_fill_rect(pc, (xd_rect){1, 1, 1, 1}, hi);
    cairo_destroy(pc);

    cairo_pattern_t *pat = cairo_pattern_create_for_surface(pat_s);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
    cairo_save(cr);
    cairo_set_source(cr, pat);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(pat_s);
}

/* Dotted focus rectangle (DrawFocusRect). */
static void focus_rect(const xd_theme *t, cairo_t *cr, xd_rect r) {
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

static int frame_border(const xd_theme *t) {
    /* SM_CXSIZEFRAME = 2px edge + BorderWidth + 1px padding. */
    return xd_tpx(t, 2) + xd_metric_px(t, XD_METRIC_BORDER_WIDTH) + xd_tpx(t, 1);
}

static xd_insets classic_frame_insets(const xd_theme *t, const xd_frame *f) {
    int b = f->maximized ? 0 : frame_border(t);
    int cap = xd_metric_px(t, XD_METRIC_CAPTION_HEIGHT) + xd_tpx(t, 1);
    return (xd_insets){b, b + cap, b, b};
}

static void caption_button(const xd_theme *t, cairo_t *cr, xd_rect r, xd_glyph g,
                           bool disabled) {
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    xd_rect inner = edge_window(t, cr, r, false);
    xd_rect glyph = xd_rect_inset4(inner, 0, 0, xd_tpx(t, 1), xd_tpx(t, 1));
    if (disabled) {
        int p = xd_tpx(t, 1);
        xd_draw_glyph(t, cr, (xd_rect){glyph.x + p, glyph.y + p, glyph.w, glyph.h}, g,
                      xd_sys(t, XD_COLOR_BUTTON_HILIGHT), 1);
        xd_draw_glyph(t, cr, glyph, g, xd_sys(t, XD_COLOR_BUTTON_SHADOW), 1);
    } else {
        xd_draw_glyph(t, cr, glyph, g, xd_sys(t, XD_COLOR_BUTTON_TEXT), 1);
    }
}

static void app_icon(const xd_theme *t, cairo_t *cr, xd_rect r) {
    /* Generic window icon until real application icons are wired up. */
    int p = xd_tpx(t, 1);
    xd_fill_rect(cr, r, xd_rgba_hex(0xFFFFFF));
    xd_fill_rect(cr, (xd_rect){r.x, r.y, r.w, xd_tpx(t, 4)}, xd_rgba_hex(0x0A246A));
    xd_draw_bevel(cr, r, p, xd_rgba_hex(0x404040), xd_rgba_hex(0x404040));
}

static void classic_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer,
                               const xd_frame *f) {
    xd_rect r = outer;
    xd_syscolor border = f->active ? XD_COLOR_ACTIVE_BORDER : XD_COLOR_INACTIVE_BORDER;

    if (!f->maximized) {
        xd_fill_rect(cr, r, xd_sys(t, border));
        r = edge_window(t, cr, r, false);
        r = xd_rect_inset(r, xd_metric_px(t, XD_METRIC_BORDER_WIDTH),
                          xd_metric_px(t, XD_METRIC_BORDER_WIDTH));
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
        r = xd_rect_inset(r, xd_tpx(t, 1), xd_tpx(t, 1));
    }

    int cap_h = xd_metric_px(t, XD_METRIC_CAPTION_HEIGHT);
    xd_rect cap = {r.x, r.y, r.w, cap_h};
    xd_rgba c1 = xd_sys(t, f->active ? XD_COLOR_ACTIVE_TITLE : XD_COLOR_INACTIVE_TITLE);
    xd_rgba c2 = xd_sys(t, f->active ? XD_COLOR_GRADIENT_ACTIVE_TITLE
                                     : XD_COLOR_GRADIENT_INACTIVE_TITLE);
    if (t->scheme.gradient_captions)
        xd_fill_gradient(cr, cap, c1, c2, true);
    else
        xd_fill_rect(cr, cap, c1);
    /* 1px separator between caption and client area. */
    xd_fill_rect(cr, (xd_rect){r.x, r.y + cap_h, r.w, xd_tpx(t, 1)},
                 xd_sys(t, XD_COLOR_BUTTON_FACE));

    /* Caption buttons: 2px from the edges, close separated by a 2px gap. */
    int bw = xd_metric_px(t, XD_METRIC_CAPTION_BUTTON_WIDTH);
    int bh = cap_h - xd_tpx(t, 4);
    int by = cap.y + xd_tpx(t, 2);
    int bx = cap.x + cap.w - xd_tpx(t, 2);

    if (f->buttons & XD_CAPBTN_CLOSE) {
        bx -= bw;
        caption_button(t, cr, (xd_rect){bx, by, bw, bh}, XD_GLYPH_CLOSE, false);
        bx -= xd_tpx(t, 2);
    }
    if (f->buttons & XD_CAPBTN_MAXIMIZE) {
        bx -= bw;
        caption_button(t, cr, (xd_rect){bx, by, bw, bh},
                       f->maximized ? XD_GLYPH_RESTORE : XD_GLYPH_MAXIMIZE, false);
    }
    if (f->buttons & XD_CAPBTN_MINIMIZE) {
        bx -= bw;
        caption_button(t, cr, (xd_rect){bx, by, bw, bh}, XD_GLYPH_MINIMIZE, false);
    }
    if (f->buttons & XD_CAPBTN_HELP) {
        bx -= bw + xd_tpx(t, 2);
        caption_button(t, cr, (xd_rect){bx, by, bw, bh}, XD_GLYPH_HELP, false);
    }

    int icon = xd_tpx(t, 16);
    int ix = cap.x + xd_tpx(t, 2);
    app_icon(t, cr, (xd_rect){ix, cap.y + (cap_h - icon) / 2, icon, icon});

    int tx = ix + icon + xd_tpx(t, 3);
    xd_rect text = {tx, cap.y, bx - tx - xd_tpx(t, 2), cap_h};
    xd_text_style st = xd_text(t, XD_FONT_CAPTION,
                               xd_sys(t, f->active ? XD_COLOR_TITLE_TEXT
                                                   : XD_COLOR_INACTIVE_TITLE_TEXT),
                               XD_ALIGN_START);
    xd_draw_text(cr, text, f->title, &st);
}

static xd_rgba classic_dialog_color(const xd_theme *t) {
    return xd_sys(t, XD_COLOR_BUTTON_FACE);
}

static void classic_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                const char *label) {
    int p = xd_tpx(t, 1);
    bool pressed = state & XD_STATE_PRESSED;
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));

    if (state & XD_STATE_DEFAULT || state & XD_STATE_FOCUSED) {
        r = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_WINDOW_FRAME),
                          xd_sys(t, XD_COLOR_WINDOW_FRAME));
    }
    xd_rect inner;
    if (pressed) {
        /* Pressed push buttons are a flat shadow frame in Win2000. */
        inner = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                              xd_sys(t, XD_COLOR_BUTTON_SHADOW));
        inner = xd_rect_inset(inner, p, p);
    } else {
        inner = edge_button(t, cr, r, false);
    }

    xd_rect text = inner;
    if (pressed)
        text = (xd_rect){text.x + p, text.y + p, text.w, text.h};
    bool disabled = state & XD_STATE_DISABLED;
    if (disabled) {
        xd_text_style hi = xd_text(t, XD_FONT_MESSAGE, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                                   XD_ALIGN_CENTER);
        xd_draw_text(cr, (xd_rect){text.x + p, text.y + p, text.w, text.h}, label, &hi);
    }
    xd_text_style st =
        xd_text(t, XD_FONT_MESSAGE,
                xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT),
                XD_ALIGN_CENTER);
    xd_draw_text(cr, text, label, &st);

    if (state & XD_STATE_FOCUSED)
        focus_rect(t, cr, xd_rect_inset(inner, xd_tpx(t, 3), xd_tpx(t, 3)));
}

static void classic_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label) {
    int box = xd_tpx(t, 13);
    xd_rect b = {r.x, r.y + (r.h - box) / 2, box, box};
    bool disabled = state & XD_STATE_DISABLED;
    xd_rect well = edge_sunken(t, cr, b);
    xd_fill_rect(cr, well,
                 xd_sys(t, (disabled || (state & XD_STATE_PRESSED)) ? XD_COLOR_BUTTON_FACE
                                                                     : XD_COLOR_WINDOW));
    if (state & XD_STATE_CHECKED)
        xd_draw_glyph(t, cr, xd_rect_inset(well, xd_tpx(t, 1), xd_tpx(t, 1)), XD_GLYPH_CHECK,
                      xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT), 1);

    int gap = xd_tpx(t, 5);
    xd_rect text = {b.x + box + gap, r.y, r.w - box - gap, r.h};
    xd_text_style st =
        xd_text(t, XD_FONT_MESSAGE,
                xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT),
                XD_ALIGN_START);
    xd_draw_text(cr, text, label, &st);
    if (state & XD_STATE_FOCUSED) {
        int w = XD_MIN(xd_text_width(cr, label, &st), text.w);
        focus_rect(t, cr, (xd_rect){text.x - xd_tpx(t, 1), r.y + xd_tpx(t, 2),
                                    w + xd_tpx(t, 3), r.h - xd_tpx(t, 4)});
    }
}

static int classic_taskbar_height(const xd_theme *t) {
    return xd_metric_px(t, XD_METRIC_TASKBAR_HEIGHT);
}

static void classic_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r) {
    int p = xd_tpx(t, 1);
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    xd_fill_rect(cr, (xd_rect){r.x, r.y, r.w, p}, xd_sys(t, XD_COLOR_BUTTON_LIGHT));
    xd_fill_rect(cr, (xd_rect){r.x, r.y + p, r.w, p}, xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
}

static xd_text_style start_text(const xd_theme *t, xd_font_spec *bold) {
    *bold = t->scheme.fonts[XD_FONT_MENU];
    bold->bold = true;
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_sys(t, XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_START);
    st.font = bold;
    st.ellipsize = false;
    return st;
}

static int classic_start_button_width(const xd_theme *t, cairo_t *cr) {
    xd_font_spec bold;
    xd_text_style st = start_text(t, &bold);
    return xd_tpx(t, 4 + 16 + 4 + 6) + xd_text_width(cr, "Start", &st);
}

static void classic_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                      uint8_t state) {
    bool pressed = state & XD_STATE_PRESSED;
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    xd_rect inner = edge_button(t, cr, r, pressed);
    int off = pressed ? xd_tpx(t, 1) : 0;
    int icon = xd_tpx(t, 16);
    xd_draw_start_flag(cr, (xd_rect){inner.x + xd_tpx(t, 2) + off,
                                     inner.y + (inner.h - icon) / 2 + off, icon, icon});
    xd_font_spec bold;
    xd_text_style st = start_text(t, &bold);
    int tx = inner.x + xd_tpx(t, 2 + 16 + 3) + off;
    xd_draw_text(cr, (xd_rect){tx, inner.y + off, inner.x + inner.w - tx, inner.h}, "Start",
                 &st);
}

static void classic_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                     uint8_t state, const char *label) {
    bool active = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    xd_rect inner = edge_button(t, cr, r, active);
    if (active && !t->scheme.high_contrast)
        fill_dither(t, cr, inner);
    int off = active ? xd_tpx(t, 1) : 0;
    int icon = xd_tpx(t, 16);
    app_icon(t, cr, (xd_rect){inner.x + xd_tpx(t, 3) + off,
                              inner.y + (inner.h - icon) / 2 + off, icon, icon});
    xd_font_spec font = t->scheme.fonts[XD_FONT_MENU];
    font.bold = active;
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_sys(t, XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_START);
    st.font = &font;
    int tx = inner.x + xd_tpx(t, 3 + 16 + 4) + off;
    xd_draw_text(cr, (xd_rect){tx, inner.y + off, inner.x + inner.w - tx - xd_tpx(t, 2),
                               inner.h},
                 label, &st);
}

static void classic_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock) {
    int p = xd_tpx(t, 1);
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    xd_rect inner = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                                  xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    xd_text_style st = xd_text(t, XD_FONT_MENU, xd_sys(t, XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_END);
    xd_draw_text(cr, xd_rect_inset(inner, xd_tpx(t, 6), 0), clock, &st);
}

const xd_style_ops xd_style_classic = {
    .name = "Classic",
    .frame_insets = classic_frame_insets,
    .draw_frame = classic_draw_frame,
    .dialog_color = classic_dialog_color,
    .draw_button = classic_draw_button,
    .draw_checkbox = classic_draw_checkbox,
    .taskbar_height = classic_taskbar_height,
    .draw_taskbar = classic_draw_taskbar,
    .start_button_width = classic_start_button_width,
    .draw_start_button = classic_draw_start_button,
    .draw_task_button = classic_draw_task_button,
    .draw_tray = classic_draw_tray,
};
