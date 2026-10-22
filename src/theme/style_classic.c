/* SPDX-License-Identifier: MIT
 * Windows 2000 "Classic" visual style. Every colour comes from the scheme,
 * so all classic colour schemes (Standard, Brick, High Contrast, ...) work.
 */
#include "style.h"

/* EDGE_RAISED as used by DrawFrameControl(DFCS_BUTTONPUSH). */
static xd_rect edge_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool pressed) {
    int pixel = xd_tpx(theme, 1);
    if (pressed) {
        rect = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW),
                             xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
        return xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                             xd_sys(theme, XD_COLOR_BUTTON_LIGHT));
    }
    rect = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                         xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
    return xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
                         xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
}

/* Window and caption-button edge: light outside, white inside. */
static xd_rect edge_window(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool pressed) {
    int pixel = xd_tpx(theme, 1);
    if (pressed) {
        rect = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW),
                             xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
        return xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                             xd_sys(theme, XD_COLOR_BUTTON_LIGHT));
    }
    rect = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
                         xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
    return xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                         xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
}

/* EDGE_SUNKEN (checkbox wells, text fields). */
static xd_rect edge_sunken(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    int pixel = xd_tpx(theme, 1);
    rect = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                         xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    return xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW),
                         xd_sys(theme, XD_COLOR_BUTTON_LIGHT));
}

/* Win2000's 50% checkerboard of ButtonHilight over ButtonFace, used for
 * pressed task buttons and the like. */
static void fill_dither(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    cairo_surface_t *pat_s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
    cairo_t *pc = cairo_create(pat_s);
    xd_rgba hi = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT);
    xd_fill_rect(pc, (xd_rect){0, 0, 1, 1}, hi);
    xd_fill_rect(pc, (xd_rect){1, 1, 1, 1}, hi);
    cairo_destroy(pc);

    cairo_pattern_t *pat = cairo_pattern_create_for_surface(pat_s);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
    cairo_save(cairo);
    cairo_set_source(cairo, pat);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_fill(cairo);
    cairo_restore(cairo);
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(pat_s);
}

/* Dotted focus rectangle (DrawFocusRect). */
static void focus_rect(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    double pixel = xd_tpx(theme, 1);
    double dash[] = {pixel, pixel};
    cairo_save(cairo);
    xd_set_color(cairo, xd_sys(theme, XD_COLOR_BUTTON_TEXT));
    cairo_set_line_width(cairo, pixel);
    cairo_set_dash(cairo, dash, 2, 0);
    cairo_rectangle(cairo, rect.x + pixel / 2, rect.y + pixel / 2, rect.w - pixel, rect.h - pixel);
    cairo_stroke(cairo);
    cairo_restore(cairo);
}

static int frame_border(const xd_theme *theme) {
    /* SM_CXSIZEFRAME = 2px edge + BorderWidth + 1px padding. */
    return xd_tpx(theme, 2) + xd_metric_px(theme, XD_METRIC_BORDER_WIDTH) + xd_tpx(theme, 1);
}

/* SM_CXDLGFRAME: dialogs have the 2px edge and 1px padding but no sizing
 * border. */
static int dialog_border(const xd_theme *theme) { return xd_tpx(theme, 2) + xd_tpx(theme, 1); }

static int border_for(const xd_theme *theme, const xd_frame *frame) {
    return frame->maximized ? 0 : frame->dialog ? dialog_border(theme) : frame_border(theme);
}

static xd_insets classic_frame_insets(const xd_theme *theme, const xd_frame *frame) {
    int border = border_for(theme, frame);
    int cap = xd_metric_px(theme, XD_METRIC_CAPTION_HEIGHT) + xd_tpx(theme, 1);
    return (xd_insets){border, border + cap, border, border};
}

static void caption_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_glyph glyph_kind,
                           bool pressed) {
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    xd_rect inner = edge_window(theme, cairo, rect, pressed);
    xd_rect glyph = xd_rect_inset4(inner, 0, 0, xd_tpx(theme, 1), xd_tpx(theme, 1));
    if (pressed) {
        int pixel = xd_tpx(theme, 1);
        glyph = (xd_rect){glyph.x + pixel, glyph.y + pixel, glyph.w, glyph.h};
    }
    xd_draw_glyph(theme, cairo, glyph, glyph_kind, xd_sys(theme, XD_COLOR_BUTTON_TEXT), 1);
}

static void app_icon(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    /* A generic window, for one without an icon. */
    int pixel = xd_tpx(theme, 1);
    xd_fill_rect(cairo, rect, xd_rgba_hex(0xFFFFFF));
    xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, rect.w, xd_tpx(theme, 4)}, xd_rgba_hex(0x0A246A));
    xd_draw_bevel(cairo, rect, pixel, xd_rgba_hex(0x404040), xd_rgba_hex(0x404040));
}

static void classic_frame_layout(const xd_theme *theme, xd_rect outer, const xd_frame *frame,
                                 xd_frame_layout *frame_layout) {
    int border = border_for(theme, frame);
    xd_rect rect = xd_rect_inset(outer, border, border);
    int cap_h = xd_metric_px(theme, XD_METRIC_CAPTION_HEIGHT);
    frame_layout->caption = (xd_rect){rect.x, rect.y, rect.w, cap_h};

    /* Caption buttons: 2px from the edges, close separated by a 2px gap. */
    int bw = xd_metric_px(theme, XD_METRIC_CAPTION_BUTTON_WIDTH);
    int bh = cap_h - xd_tpx(theme, 4);
    int by = rect.y + xd_tpx(theme, 2);
    int bx = rect.x + rect.w - xd_tpx(theme, 2);
    if (frame->buttons & XD_CAPBTN_CLOSE) {
        bx -= bw;
        xd_layout_add_button(frame_layout, (xd_rect){bx, by, bw, bh}, XD_PART_CLOSE);
        bx -= xd_tpx(theme, 2);
    }
    if (frame->buttons & XD_CAPBTN_MAXIMIZE) {
        bx -= bw;
        xd_layout_add_button(frame_layout, (xd_rect){bx, by, bw, bh}, XD_PART_MAXIMIZE);
    }
    if (frame->buttons & XD_CAPBTN_MINIMIZE) {
        bx -= bw;
        xd_layout_add_button(frame_layout, (xd_rect){bx, by, bw, bh}, XD_PART_MINIMIZE);
    }
    if (frame->buttons & XD_CAPBTN_HELP) {
        bx -= bw + xd_tpx(theme, 2);
        xd_layout_add_button(frame_layout, (xd_rect){bx, by, bw, bh}, XD_PART_HELP);
    }

    int icon = xd_tpx(theme, 16);
    frame_layout->icon =
        (xd_rect){rect.x + xd_tpx(theme, 2), rect.y + (cap_h - icon) / 2, icon, icon};
    int tx = frame_layout->icon.x + icon + xd_tpx(theme, 3);
    frame_layout->title = (xd_rect){tx, rect.y, bx - tx - xd_tpx(theme, 2), cap_h};
}

static void classic_draw_frame(const xd_theme *theme, cairo_t *cairo, xd_rect outer,
                               const xd_frame *frame) {
    xd_rect rect = outer;
    xd_syscolor border = frame->active ? XD_COLOR_ACTIVE_BORDER : XD_COLOR_INACTIVE_BORDER;

    if (!frame->maximized) {
        xd_fill_rect(cairo, rect, xd_sys(theme, border));
        rect = edge_window(theme, cairo, rect, false);
        if (!frame->dialog)
            rect = xd_rect_inset(rect, xd_metric_px(theme, XD_METRIC_BORDER_WIDTH),
                                 xd_metric_px(theme, XD_METRIC_BORDER_WIDTH));
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    } else {
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    }

    xd_frame_layout frame_layout;
    xd_theme_frame_layout(theme, outer, frame, &frame_layout);

    xd_rgba c1 = xd_sys(theme, frame->active ? XD_COLOR_ACTIVE_TITLE : XD_COLOR_INACTIVE_TITLE);
    xd_rgba c2 = xd_sys(theme, frame->active ? XD_COLOR_GRADIENT_ACTIVE_TITLE
                                             : XD_COLOR_GRADIENT_INACTIVE_TITLE);
    if (theme->scheme.gradient_captions)
        xd_fill_gradient(cairo, frame_layout.caption, c1, c2, true);
    else
        xd_fill_rect(cairo, frame_layout.caption, c1);

    for (size_t i = 0; i < frame_layout.button_count; i++) {
        xd_frame_part part = frame_layout.button_parts[i];
        bool pressed = frame->pressed == part && frame->hot == part;
        caption_button(theme, cairo, frame_layout.button_rects[i],
                       xd_glyph_for_part(part, frame->maximized), pressed);
    }

    if (frame->icon != nullptr)
        xd_style_task_icon(theme, cairo, frame->icon, frame_layout.icon.x, frame_layout.icon.y);
    else
        app_icon(theme, cairo, frame_layout.icon);
    xd_text_style st =
        xd_text(theme, XD_FONT_CAPTION,
                xd_sys(theme, frame->active ? XD_COLOR_TITLE_TEXT : XD_COLOR_INACTIVE_TITLE_TEXT),
                XD_ALIGN_START);
    xd_draw_text(cairo, frame_layout.title, frame->title, &st);
}

static xd_rgba classic_dialog_color(const xd_theme *theme) {
    return xd_sys(theme, XD_COLOR_BUTTON_FACE);
}

static void classic_draw_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                const char *label) {
    int pixel = xd_tpx(theme, 1);
    bool pressed = state & XD_STATE_PRESSED;
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));

    if (state & XD_STATE_DEFAULT || state & XD_STATE_FOCUSED) {
        rect = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_WINDOW_FRAME),
                             xd_sys(theme, XD_COLOR_WINDOW_FRAME));
    }
    xd_rect inner;
    if (pressed) {
        /* Pressed push buttons are a flat shadow frame in Win2000. */
        inner = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                              xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
        inner = xd_rect_inset(inner, pixel, pixel);
    } else {
        inner = edge_button(theme, cairo, rect, false);
    }

    xd_rect text = inner;
    if (pressed)
        text = (xd_rect){text.x + pixel, text.y + pixel, text.w, text.h};
    bool disabled = state & XD_STATE_DISABLED;
    if (disabled) {
        xd_text_style hi = xd_text(theme, XD_FONT_MESSAGE, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                                   XD_ALIGN_CENTER);
        xd_draw_text(cairo, (xd_rect){text.x + pixel, text.y + pixel, text.w, text.h}, label, &hi);
    }
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE,
                               xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_CENTER);
    xd_draw_text(cairo, text, label, &st);

    if (state & XD_STATE_FOCUSED)
        focus_rect(theme, cairo, xd_rect_inset(inner, xd_tpx(theme, 3), xd_tpx(theme, 3)));
}

static void classic_draw_checkbox(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state, const char *label) {
    int box = xd_tpx(theme, 13);
    xd_rect box_rect = {rect.x, rect.y + (rect.h - box) / 2, box, box};
    bool disabled = state & XD_STATE_DISABLED;
    xd_rect well = edge_sunken(theme, cairo, box_rect);
    xd_fill_rect(cairo, well,
                 xd_sys(theme, (disabled || (state & XD_STATE_PRESSED)) ? XD_COLOR_BUTTON_FACE
                                                                        : XD_COLOR_WINDOW));
    if (state & XD_STATE_CHECKED)
        xd_draw_glyph(theme, cairo, xd_rect_inset(well, xd_tpx(theme, 1), xd_tpx(theme, 1)),
                      XD_GLYPH_CHECK,
                      xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT), 1);

    int gap = xd_tpx(theme, 5);
    xd_rect text = {box_rect.x + box + gap, rect.y, rect.w - box - gap, rect.h};
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE,
                               xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    xd_draw_text(cairo, text, label, &st);
    if (state & XD_STATE_FOCUSED) {
        int width = XD_MIN(xd_text_width(cairo, label, &st), text.w);
        focus_rect(theme, cairo,
                   (xd_rect){text.x - xd_tpx(theme, 1), rect.y + xd_tpx(theme, 2),
                             width + xd_tpx(theme, 3), rect.h - xd_tpx(theme, 4)});
    }
}

static int classic_taskbar_height(const xd_theme *theme) {
    return xd_metric_px(theme, XD_METRIC_TASKBAR_HEIGHT);
}

static void classic_draw_taskbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                 xd_edge edge) {
    int pixel = xd_tpx(theme, 1);
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    xd_fill_rect(cairo, xd_style_rim_line(rect, edge, 0, pixel),
                 xd_sys(theme, XD_COLOR_BUTTON_LIGHT));
    xd_fill_rect(cairo, xd_style_rim_line(rect, edge, pixel, pixel),
                 xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
}

static xd_text_style start_text(const xd_theme *theme, xd_font_spec *bold) {
    *bold = theme->scheme.fonts[XD_FONT_MENU];
    bold->bold = true;
    xd_text_style st =
        xd_text(theme, XD_FONT_MENU, xd_sys(theme, XD_COLOR_BUTTON_TEXT), XD_ALIGN_START);
    st.font = bold;
    st.ellipsize = false;
    return st;
}

static int classic_start_button_width(const xd_theme *theme, cairo_t *cairo) {
    xd_font_spec bold;
    xd_text_style st = start_text(theme, &bold);
    return xd_tpx(theme, 4 + 16 + 4 + 6) + xd_text_width(cairo, "Start", &st);
}

static void classic_draw_start_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                      uint8_t state) {
    bool pressed = state & XD_STATE_PRESSED;
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    xd_rect inner = edge_button(theme, cairo, rect, pressed);
    int off = pressed ? xd_tpx(theme, 1) : 0;
    int icon = xd_tpx(theme, 16);
    xd_draw_start_logo(cairo, (xd_rect){inner.x + xd_tpx(theme, 2) + off,
                                        inner.y + (inner.h - icon) / 2 + off, icon, icon});
    xd_font_spec bold;
    xd_text_style st = start_text(theme, &bold);
    int tx = inner.x + xd_tpx(theme, 2 + 16 + 3) + off;
    xd_draw_text(cairo, (xd_rect){tx, inner.y + off, inner.x + inner.w - tx, inner.h}, "Start",
                 &st);
}

static void classic_draw_task_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                     uint8_t state, const char *label,
                                     cairo_surface_t *icon_surface) {
    bool active = state & (XD_STATE_PRESSED | XD_STATE_CHECKED);
    /* Flashing: the selection colour, as FlashWindow paints it. */
    bool flash = (state & XD_STATE_FLASH) && !active;
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    xd_rect inner = edge_button(theme, cairo, rect, active);
    if (flash)
        xd_fill_rect(cairo, inner, xd_sys(theme, XD_COLOR_HILIGHT));
    else if (active && !theme->scheme.high_contrast)
        fill_dither(theme, cairo, inner);
    int off = active ? xd_tpx(theme, 1) : 0;
    int icon = xd_tpx(theme, 16);
    xd_style_task_icon(theme, cairo, icon_surface, inner.x + xd_tpx(theme, 3) + off,
                       inner.y + (inner.h - icon) / 2 + off);
    xd_font_spec font = theme->scheme.fonts[XD_FONT_MENU];
    font.bold = active;
    xd_text_style st = xd_text(theme, XD_FONT_MENU,
                               xd_sys(theme, flash ? XD_COLOR_HILIGHT_TEXT : XD_COLOR_BUTTON_TEXT),
                               XD_ALIGN_START);
    st.font = &font;
    int tx = inner.x + xd_tpx(theme, 3 + 16 + 4) + off;
    xd_draw_text(cairo,
                 (xd_rect){tx, inner.y + off, inner.x + inner.w - tx - xd_tpx(theme, 2), inner.h},
                 label, &st);
}

static void classic_draw_tray(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                              const char *clock) {
    int pixel = xd_tpx(theme, 1);
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    xd_rect inner = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                                  xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    xd_text_style st =
        xd_text(theme, XD_FONT_MENU, xd_sys(theme, XD_COLOR_BUTTON_TEXT), XD_ALIGN_END);
    st.lines = true; /* a tall taskbar's day and date under the time */
    xd_draw_text(cairo, xd_rect_inset(inner, xd_tpx(theme, 6), 0), clock, &st);
}

static void classic_menu_palette(const xd_theme *theme, xd_menu_palette *menu_palette) {
    bool flat = theme->scheme.flat_menus;
    *menu_palette = (xd_menu_palette){
        .flat = flat,
        .background = xd_sys(theme, XD_COLOR_MENU),
        .border = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
        .text = xd_sys(theme, XD_COLOR_MENU_TEXT),
        .hilight = xd_sys(theme, flat ? XD_COLOR_MENU_HILIGHT : XD_COLOR_HILIGHT),
        .hilight_text = xd_sys(theme, XD_COLOR_HILIGHT_TEXT),
        .disabled_text = xd_sys(theme, XD_COLOR_GRAY_TEXT),
        /* Windows 2000 etches disabled menu text with a white shadow. */
        .disabled_emboss = theme->scheme.high_contrast ? xd_rgba_alpha(xd_rgba_hex(0), 0)
                                                       : xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
        .separator_dark = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
        .separator_light = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
        .banner_top = xd_rgba_hex(0x000000),
        .banner_bottom = xd_sys(theme, XD_COLOR_ACTIVE_TITLE),
        .banner_text = xd_sys(theme, XD_COLOR_TITLE_TEXT),
    };
}

static void classic_toolkit_colors(const xd_theme *theme, xd_toolkit_colors *toolkit_colors) {
    *toolkit_colors = (xd_toolkit_colors){
        .classic = true,
        .bg = xd_sys(theme, XD_COLOR_BUTTON_FACE),
        .fg = xd_sys(theme, XD_COLOR_BUTTON_TEXT),
        .base = xd_sys(theme, XD_COLOR_WINDOW),
        .text = xd_sys(theme, XD_COLOR_WINDOW_TEXT),
        .disabled = xd_sys(theme, XD_COLOR_GRAY_TEXT),
        .selected_bg = xd_sys(theme, XD_COLOR_HILIGHT),
        .selected_fg = xd_sys(theme, XD_COLOR_HILIGHT_TEXT),
        .tooltip_bg = xd_sys(theme, XD_COLOR_INFO_WINDOW),
        .tooltip_fg = xd_sys(theme, XD_COLOR_INFO_TEXT),
        .menu_bg = xd_sys(theme, XD_COLOR_MENU),
        .menu_fg = xd_sys(theme, XD_COLOR_MENU_TEXT),
        .menu_border = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
        .menu_hot =
            xd_sys(theme, theme->scheme.flat_menus ? XD_COLOR_MENU_HILIGHT : XD_COLOR_HILIGHT),
        .menu_hot_fg = xd_sys(theme, XD_COLOR_HILIGHT_TEXT),
        .menubar_bg = xd_sys(theme, XD_COLOR_MENU_BAR),
        .title_bg = xd_sys(theme, XD_COLOR_ACTIVE_TITLE),
        .title_bg2 = xd_sys(theme, XD_COLOR_GRADIENT_ACTIVE_TITLE),
        .title_fg = xd_sys(theme, XD_COLOR_TITLE_TEXT),
        .face = xd_sys(theme, XD_COLOR_BUTTON_FACE),
        .hilight = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
        .light = xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
        .shadow = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
        .dkshadow = xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW),
        .frame = xd_sys(theme, XD_COLOR_WINDOW_FRAME),
        .scrollbar = xd_sys(theme, XD_COLOR_SCROLLBAR),
    };
}

const xd_style_ops xd_style_classic = {
    .name = "Classic",
    .frame_insets = classic_frame_insets,
    .frame_layout = classic_frame_layout,
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
    .menu_palette = classic_menu_palette,
    .toolkit_colors = classic_toolkit_colors,
};
