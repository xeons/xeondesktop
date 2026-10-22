/* SPDX-License-Identifier: MIT
 * Windows' colour button and the Color dialog (ChooseColor), for Display
 * Properties' Advanced Appearance.
 *
 * The button drops down the 20 colours of Windows' default system palette
 * (the 16 VGA colours and the four "static" ones: money green, sky blue,
 * cream and medium grey), four to a row, with Other... underneath.
 *
 * The Color dialog opens with its custom colour part shown, as Windows'
 * opens for desk.cpl's Other... (CC_FULLOPEN, so Define Custom Colors >>
 * stays greyed): the 48 basic colours, 16 custom colours (kept while
 * Display Properties is open, as Windows kept them for the process), the
 * hue/saturation spectrum, the luminosity bar, Hue/Sat/Lum on Windows'
 * 0-240 scale and Red/Green/Blue. Not done: Windows' keyboard navigation
 * of the swatches (they take clicks only) and Help.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/alloc.h"
#include "../theme/theme.h"
#include "desk_color.h"

/* ---- Windows' HLS (ColorRGBToHLS / ColorHLSToRGB) ---------------------------------- */

static constexpr int HLS_MAX = 240;
static constexpr int RGB_MAX = 255;
static constexpr int HUE_UNDEFINED = HLS_MAX * 2 / 3;

void color_rgb_to_hls(xd_rgb color, int *hue, int *lum, int *sat) {
    int red = color.r, green = color.g, blue = color.b;
    int mx = XD_MAX(XD_MAX(red, green), blue);
    int mn = XD_MIN(XD_MIN(red, green), blue);
    *lum = ((mx + mn) * HLS_MAX + RGB_MAX) / (2 * RGB_MAX);
    if (mx == mn) {
        /* A grey: no saturation, and a hue Windows leaves at two thirds. */
        *sat = 0;
        *hue = HUE_UNDEFINED;
        return;
    }
    int sum = mx + mn, diff = mx - mn;
    if (*lum <= HLS_MAX / 2)
        *sat = (diff * HLS_MAX + sum / 2) / sum;
    else
        *sat = (diff * HLS_MAX + (2 * RGB_MAX - sum) / 2) / (2 * RGB_MAX - sum);
    int rd = ((mx - red) * (HLS_MAX / 6) + diff / 2) / diff;
    int gd = ((mx - green) * (HLS_MAX / 6) + diff / 2) / diff;
    int bd = ((mx - blue) * (HLS_MAX / 6) + diff / 2) / diff;
    int raw_hue;
    if (red == mx)
        raw_hue = bd - gd;
    else if (green == mx)
        raw_hue = HLS_MAX / 3 + rd - bd;
    else
        raw_hue = 2 * HLS_MAX / 3 + gd - rd;
    *hue = ((raw_hue % HLS_MAX) + HLS_MAX) % HLS_MAX;
}

static double hue_to_rgb(double n1, double n2, double hue) {
    hue = fmod(fmod(hue, HLS_MAX) + HLS_MAX, HLS_MAX);
    if (hue < HLS_MAX / 6.0)
        return n1 + (n2 - n1) * hue / (HLS_MAX / 6.0);
    if (hue < HLS_MAX / 2.0)
        return n2;
    if (hue < HLS_MAX * 2 / 3.0)
        return n1 + (n2 - n1) * (HLS_MAX * 2 / 3.0 - hue) / (HLS_MAX / 6.0);
    return n1;
}

static uint8_t channel(double value) {
    return (uint8_t)XD_CLAMP(lround(value * RGB_MAX / HLS_MAX), 0, RGB_MAX);
}

xd_rgb color_rgb_from_hls(int hue, int lum, int sat) {
    if (sat == 0) {
        uint8_t gray = channel(lum);
        return (xd_rgb){gray, gray, gray};
    }
    double lightness = lum, saturation = sat;
    double m2 = lightness <= HLS_MAX / 2.0
                    ? lightness * (HLS_MAX + saturation) / HLS_MAX
                    : lightness + saturation - lightness * saturation / HLS_MAX;
    double m1 = 2 * lightness - m2;
    return (xd_rgb){channel(hue_to_rgb(m1, m2, hue + HLS_MAX / 3.0)),
                    channel(hue_to_rgb(m1, m2, hue)),
                    channel(hue_to_rgb(m1, m2, hue - HLS_MAX / 3.0))};
}

static bool same_rgb(xd_rgb left, xd_rgb right) {
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

/* A sunken 2px well around a colour, as Windows' colour swatches. */
static void draw_well(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_rgb color) {
    int pixel = xd_tpx(theme, 1);
    xd_rect in = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    in = xd_draw_bevel(cairo, in, pixel, xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW),
                       xd_sys(theme, XD_COLOR_BUTTON_LIGHT));
    xd_fill_rect(cairo, in, xd_rgba_from_rgb(color));
}

/* ---- the Color dialog -------------------------------------------------------------- */

static const xd_rgb basic_colors[48] = {
    {255, 128, 128}, {255, 255, 128}, {128, 255, 128}, {0, 255, 128},
    {128, 255, 255}, {0, 128, 255},   {255, 128, 192}, {255, 128, 255},
    {255, 0, 0},     {255, 255, 0},   {128, 255, 0},   {0, 255, 64},
    {0, 255, 255},   {0, 128, 192},   {128, 128, 192}, {255, 0, 255},
    {128, 64, 64},   {255, 128, 64},  {0, 255, 0},     {0, 128, 128},
    {0, 64, 128},    {128, 128, 255}, {128, 0, 64},    {255, 0, 128},
    {128, 0, 0},     {255, 128, 0},   {0, 128, 0},     {0, 128, 64},
    {0, 0, 255},     {0, 0, 160},     {128, 0, 128},   {128, 0, 255},
    {64, 0, 0},      {128, 64, 0},    {0, 64, 0},      {0, 64, 64},
    {0, 0, 128},     {0, 0, 64},      {64, 0, 64},     {64, 0, 128},
    {0, 0, 0},       {128, 128, 0},   {128, 128, 64},  {128, 128, 128},
    {64, 128, 128},  {192, 192, 192}, {64, 0, 64},     {255, 255, 255},
};

enum { SWATCH_COLS = 8, BASIC_ROWS = 6, CUSTOM_ROWS = 2, CUSTOM_COUNT = 16 };

/* Kept for as long as the program runs, as Windows kept them per process
 * (desk.cpl's own array); they start white. */
static xd_rgb custom_colors[CUSTOM_COUNT] = {
    {255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255},
    {255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255},
    {255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255},
    {255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255},
};

/* Layout, in logical px: Windows' dialog units at 8pt Tahoma. */
static constexpr int DLG_W = 450;
static constexpr int DLG_H = 302;
static const xd_rect BASIC_R = {6, 23, 210, 140};
static const xd_rect CUSTOM_R = {6, 188, 210, 46};
static const xd_rect SPECTRUM_R = {228, 6, 177, 189};
static const xd_rect LUM_R = {414, 6, 24, 189}; /* the bar and its arrow */
static constexpr int LUM_BAR_W = 12;

enum { E_HUE, E_SAT, E_LUM, E_RED, E_GREEN, E_BLUE, E_COUNT };

struct color_dialog {
    xd_ui *ui;
    xd_window *win;
    xd_widget *basic, *custom, *spectrum, *lum, *solid;
    xd_widget *edits[E_COUNT];
    int hue, sat, lumv;
    xd_rgb rgb;
    int basic_sel, custom_sel; /* -1: none */
    bool dragging_spectrum;
    cairo_surface_t *spectrum_cache;
    color_dialog_done done;
    void *user;
};

static void swatch_rect(xd_rect area, int rows, int i, xd_rect *out) {
    int cw = area.w / SWATCH_COLS, ch = area.h / rows;
    *out = (xd_rect){area.x + (i % SWATCH_COLS) * cw, area.y + (i / SWATCH_COLS) * ch, cw, ch};
}

/* Swatches in a grid, the selected one framed. r is the widget's device
 * rect; `cols x rows` cells. */
static void draw_swatches(const xd_theme *theme, cairo_t *cairo, xd_rect bounds,
                          const xd_rgb *colors, int rows, int sel) {
    xd_fill_rect(cairo, bounds, xd_theme_dialog_color(theme));
    for (int i = 0; i < SWATCH_COLS * rows; i++) {
        xd_rect cell;
        swatch_rect(bounds, rows, i, &cell);
        xd_rect well = xd_rect_inset(cell, xd_tpx(theme, 3), xd_tpx(theme, 3));
        if (i == sel) {
            /* Windows frames the selection in black. */
            xd_rect frame_rect = xd_rect_inset(cell, xd_tpx(theme, 1), xd_tpx(theme, 1));
            xd_rgba black = xd_rgba_hex(0x000000);
            xd_draw_bevel(cairo, frame_rect, xd_tpx(theme, 1), black, black);
        }
        draw_well(theme, cairo, well, colors[i]);
    }
}

static void draw_basic([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                       const xd_theme *theme, void *user) {
    color_dialog *color_window = user;
    draw_swatches(theme, cairo, rect, basic_colors, BASIC_ROWS, color_window->basic_sel);
}

static void draw_custom([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                        const xd_theme *theme, void *user) {
    color_dialog *color_window = user;
    draw_swatches(theme, cairo, rect, custom_colors, CUSTOM_ROWS, color_window->custom_sel);
}

/* The spectrum: hue across, saturation from full at the top to none at the
 * bottom, at middle luminosity; a crosshair on the current hue and
 * saturation. */
static void draw_spectrum([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                          const xd_theme *theme, void *user) {
    color_dialog *color_window = user;
    int pixel = xd_tpx(theme, 1);
    xd_rect in = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    if (color_window->spectrum_cache == nullptr ||
        cairo_image_surface_get_width(color_window->spectrum_cache) != in.w ||
        cairo_image_surface_get_height(color_window->spectrum_cache) != in.h) {
        if (color_window->spectrum_cache != nullptr)
            cairo_surface_destroy(color_window->spectrum_cache);
        color_window->spectrum_cache = cairo_image_surface_create(CAIRO_FORMAT_RGB24, in.w, in.h);
        uint8_t *px = cairo_image_surface_get_data(color_window->spectrum_cache);
        int stride = cairo_image_surface_get_stride(color_window->spectrum_cache);
        for (int y = 0; y < in.h; y++) {
            int sat = HLS_MAX - y * HLS_MAX / XD_MAX(in.h - 1, 1);
            uint32_t *row = (uint32_t *)(void *)(px + (size_t)y * (size_t)stride);
            for (int x = 0; x < in.w; x++) {
                int hue = x * (HLS_MAX - 1) / XD_MAX(in.w - 1, 1);
                xd_rgb color = color_rgb_from_hls(hue, HLS_MAX / 2, sat);
                row[x] = ((uint32_t)color.r << 16) | ((uint32_t)color.g << 8) | color.b;
            }
        }
        cairo_surface_mark_dirty(color_window->spectrum_cache);
    }
    cairo_save(cairo);
    cairo_rectangle(cairo, in.x, in.y, in.w, in.h);
    cairo_clip(cairo);
    cairo_set_source_surface(cairo, color_window->spectrum_cache, in.x, in.y);
    cairo_paint(cairo);
    /* The crosshair: four arms around a gap, as Windows draws it. */
    double cx = in.x + color_window->hue * (in.w - 1) / (double)(HLS_MAX - 1) + 0.5;
    double cy = in.y + (HLS_MAX - color_window->sat) * (in.h - 1) / (double)HLS_MAX + 0.5;
    double gap = xd_tpx(theme, 3), arm = xd_tpx(theme, 8);
    cairo_set_line_width(cairo, xd_tpx(theme, 2));
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_move_to(cairo, cx - gap - arm, cy);
    cairo_line_to(cairo, cx - gap, cy);
    cairo_move_to(cairo, cx + gap, cy);
    cairo_line_to(cairo, cx + gap + arm, cy);
    cairo_move_to(cairo, cx, cy - gap - arm);
    cairo_line_to(cairo, cx, cy - gap);
    cairo_move_to(cairo, cx, cy + gap);
    cairo_line_to(cairo, cx, cy + gap + arm);
    cairo_stroke(cairo);
    cairo_restore(cairo);
}

static xd_rect lum_bar(const xd_theme *theme, xd_rect rect) {
    return (xd_rect){rect.x, rect.y, xd_tpx(theme, LUM_BAR_W), rect.h};
}

/* The luminosity bar for the current hue and saturation, white at the top
 * to black, with an arrow at the current luminosity. */
static void draw_lum([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                     const xd_theme *theme, void *user) {
    color_dialog *color_window = user;
    xd_fill_rect(cairo, rect, xd_theme_dialog_color(theme));
    xd_rect bar = lum_bar(theme, rect);
    int pixel = xd_tpx(theme, 1);
    xd_rect in = xd_draw_bevel(cairo, bar, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    for (int y = 0; y < in.h; y++) {
        int lum = HLS_MAX - y * HLS_MAX / XD_MAX(in.h - 1, 1);
        xd_fill_rect(
            cairo, (xd_rect){in.x, in.y + y, in.w, 1},
            xd_rgba_from_rgb(color_rgb_from_hls(color_window->hue, lum, color_window->sat)));
    }
    double ay = in.y + (HLS_MAX - color_window->lumv) * (in.h - 1) / (double)HLS_MAX + 0.5;
    double ax = bar.x + bar.w + xd_tpx(theme, 1);
    double arrow_size = xd_tpx(theme, 5);
    cairo_move_to(cairo, ax, ay);
    cairo_line_to(cairo, ax + arrow_size, ay - arrow_size);
    cairo_line_to(cairo, ax + arrow_size, ay + arrow_size);
    cairo_close_path(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_fill(cairo);
}

/* Color|Solid: on a true-colour screen the nearest solid colour is the
 * colour itself, so both halves show it. */
static void draw_solid([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                       const xd_theme *theme, void *user) {
    color_dialog *color_window = user;
    int pixel = xd_tpx(theme, 1);
    xd_rect in = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    xd_fill_rect(cairo, in, xd_rgba_from_rgb(color_window->rgb));
}

/* Puts the current colour into the fields; `except` (being typed in) is
 * left alone. */
static void show_values(color_dialog *color_window, int except) {
    int values[E_COUNT] = {color_window->hue,   color_window->sat,   color_window->lumv,
                           color_window->rgb.r, color_window->rgb.g, color_window->rgb.b};
    for (int i = 0; i < E_COUNT; i++) {
        if (i == except)
            continue;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", values[i]);
        xd_widget_set_text(color_window->edits[i], buf);
    }
    xd_widget_redraw(color_window->spectrum);
    xd_widget_redraw(color_window->lum);
    xd_widget_redraw(color_window->solid);
    xd_widget_redraw(color_window->basic);
    xd_widget_redraw(color_window->custom);
}

static void set_rgb(color_dialog *color_window, xd_rgb color, int except) {
    color_window->rgb = color;
    color_rgb_to_hls(color, &color_window->hue, &color_window->lumv, &color_window->sat);
    show_values(color_window, except);
}

static void set_hls(color_dialog *color_window, int hue, int lum, int sat, int except) {
    color_window->hue = XD_CLAMP(hue, 0, HLS_MAX - 1);
    color_window->lumv = XD_CLAMP(lum, 0, HLS_MAX);
    color_window->sat = XD_CLAMP(sat, 0, HLS_MAX);
    color_window->rgb =
        color_rgb_from_hls(color_window->hue, color_window->lumv, color_window->sat);
    show_values(color_window, except);
}

static int swatch_at(xd_rect area, int rows, double x, double y) {
    if (x < 0 || y < 0 || x >= area.w || y >= area.h)
        return -1;
    int col = (int)(x / (area.w / SWATCH_COLS)), row = (int)(y / (area.h / rows));
    if (col >= SWATCH_COLS || row >= rows)
        return -1;
    return row * SWATCH_COLS + col;
}

static void on_basic_pointer([[maybe_unused]] xd_widget *widget, xd_pointer_event ev, double x,
                             double y, void *user) {
    color_dialog *color_window = user;
    int i = swatch_at(BASIC_R, BASIC_ROWS, x, y);
    if (ev != XD_POINTER_PRESS || i < 0)
        return;
    color_window->basic_sel = i;
    color_window->custom_sel = -1;
    set_rgb(color_window, basic_colors[i], -1);
}

static void on_custom_pointer([[maybe_unused]] xd_widget *widget, xd_pointer_event ev, double x,
                              double y, void *user) {
    color_dialog *color_window = user;
    int i = swatch_at(CUSTOM_R, CUSTOM_ROWS, x, y);
    if (ev != XD_POINTER_PRESS || i < 0)
        return;
    color_window->custom_sel = i;
    color_window->basic_sel = -1;
    set_rgb(color_window, custom_colors[i], -1);
}

static void on_spectrum_pointer([[maybe_unused]] xd_widget *widget, xd_pointer_event ev, double x,
                                double y, void *user) {
    color_dialog *color_window = user;
    if (ev == XD_POINTER_RELEASE)
        return;
    /* Inside the 1px edge. */
    double fx = XD_CLAMP((x - 1) / (SPECTRUM_R.w - 3), 0.0, 1.0);
    double fy = XD_CLAMP((y - 1) / (SPECTRUM_R.h - 3), 0.0, 1.0);
    color_window->basic_sel = color_window->custom_sel = -1;
    /* The luminosity stays: a white or black colour stays so, as in
     * Windows. */
    set_hls(color_window, (int)lround(fx * (HLS_MAX - 1)), color_window->lumv,
            (int)lround((1 - fy) * HLS_MAX), -1);
}

static void on_lum_pointer([[maybe_unused]] xd_widget *widget, xd_pointer_event ev,
                           [[maybe_unused]] double x, double y, void *user) {
    color_dialog *color_window = user;
    if (ev == XD_POINTER_RELEASE)
        return;
    double fy = XD_CLAMP((y - 1) / (LUM_R.h - 3), 0.0, 1.0);
    color_window->basic_sel = color_window->custom_sel = -1;
    set_hls(color_window, color_window->hue, (int)lround((1 - fy) * HLS_MAX), color_window->sat,
            -1);
}

static void on_edit(xd_widget *widget, void *user) {
    color_dialog *color_window = user;
    int which = 0;
    while (which < E_COUNT && color_window->edits[which] != widget)
        which++;
    int64_t parsed;
    int max = which == E_HUE ? HLS_MAX - 1 : which < E_RED ? HLS_MAX : RGB_MAX;
    if (which == E_COUNT ||
        !xd_str_parse_i64(xd_str_from_cstr(xd_widget_text(widget)), 0, max, &parsed))
        return; /* typing on: it's read once it's a number in range */
    color_window->basic_sel = color_window->custom_sel = -1;
    int value = (int)parsed;
    switch (which) {
    case E_HUE:
        set_hls(color_window, value, color_window->lumv, color_window->sat, which);
        break;
    case E_SAT:
        set_hls(color_window, color_window->hue, color_window->lumv, value, which);
        break;
    case E_LUM:
        set_hls(color_window, color_window->hue, value, color_window->sat, which);
        break;
    case E_RED:
        set_rgb(color_window, (xd_rgb){(uint8_t)value, color_window->rgb.g, color_window->rgb.b},
                which);
        break;
    case E_GREEN:
        set_rgb(color_window, (xd_rgb){color_window->rgb.r, (uint8_t)value, color_window->rgb.b},
                which);
        break;
    default:
        set_rgb(color_window, (xd_rgb){color_window->rgb.r, color_window->rgb.g, (uint8_t)value},
                which);
        break;
    }
}

/* Into the selected custom box (the first if none is), then on to the
 * next, as Windows does. */
static void on_add_custom([[maybe_unused]] xd_widget *widget, void *user) {
    color_dialog *color_window = user;
    int i = color_window->custom_sel >= 0 ? color_window->custom_sel : 0;
    custom_colors[i] = color_window->rgb;
    color_window->custom_sel = (i + 1) % CUSTOM_COUNT;
    xd_widget_redraw(color_window->custom);
}

static void destroy(color_dialog *color_window) {
    if (color_window->spectrum_cache != nullptr)
        cairo_surface_destroy(color_window->spectrum_cache);
    xd_window_close(color_window->win);
    xd_free(color_window);
}

static void finish(color_dialog *color_window, bool ok) {
    color_dialog_done done = color_window->done;
    void *user = color_window->user;
    xd_rgb rgb = color_window->rgb;
    destroy(color_window);
    done(ok, rgb, user);
}

void color_dialog_cancel(color_dialog *color_window) { finish(color_window, false); }

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) { finish(user, true); }
static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) { finish(user, false); }

color_dialog *color_dialog_open(xd_ui *ui, xd_rgb initial, color_dialog_done done, void *user) {
    color_dialog *color_window = XD_NEW(color_dialog);
    color_window->ui = ui;
    color_window->done = done;
    color_window->user = user;
    color_window->basic_sel = color_window->custom_sel = -1;
    for (int i = 0; i < (int)XD_ARRAY_LEN(basic_colors) && color_window->basic_sel < 0; i++) {
        if (same_rgb(basic_colors[i], initial))
            color_window->basic_sel = i;
    }
    xd_window *win = color_window->win = xd_window_create(ui, "Color", "xd-desk", DLG_W, DLG_H);
    xd_label(win, (xd_rect){6, 6, 210, 16}, "Basic colors:", 0);
    color_window->basic = xd_custom(win, BASIC_R, draw_basic, color_window);
    xd_custom_set_pointer(color_window->basic, on_basic_pointer);
    xd_label(win, (xd_rect){6, 171, 210, 16}, "Custom colors:", 0);
    color_window->custom = xd_custom(win, CUSTOM_R, draw_custom, color_window);
    xd_custom_set_pointer(color_window->custom, on_custom_pointer);
    xd_widget_set_enabled(xd_button(win, (xd_rect){6, 244, 213, 23}, "Define Custom Colors >>",
                                    nullptr, nullptr),
                          false);
    xd_widget *ok = xd_button(win, (xd_rect){6, 271, 66, 23}, "OK", on_ok, color_window);
    xd_widget *cancel =
        xd_button(win, (xd_rect){78, 271, 66, 23}, "Cancel", on_cancel, color_window);

    color_window->spectrum = xd_custom(win, SPECTRUM_R, draw_spectrum, color_window);
    xd_custom_set_pointer(color_window->spectrum, on_spectrum_pointer);
    color_window->lum = xd_custom(win, LUM_R, draw_lum, color_window);
    xd_custom_set_pointer(color_window->lum, on_lum_pointer);
    color_window->solid = xd_custom(win, (xd_rect){228, 202, 60, 40}, draw_solid, color_window);
    xd_label(win, (xd_rect){226, 246, 70, 16}, "Color|Solid", 0);
    static const char *const names[E_COUNT] = {"Hue:", "Sat:", "Lum:", "Red:", "Green:", "Blue:"};
    for (int i = 0; i < E_COUNT; i++) {
        int col = i / 3, row = i % 3;
        int lx = col == 0 ? 300 : 370, ex = col == 0 ? 330 : 410;
        int y = 202 + row * 22;
        xd_label(win, (xd_rect){lx, y + 3, ex - lx - 2, 16}, names[i], 0);
        color_window->edits[i] = xd_edit(win, (xd_rect){ex, y, 32, 20}, "");
        xd_edit_set_on_change(color_window->edits[i], on_edit, color_window);
    }
    xd_button(win, (xd_rect){228, 271, 213, 23}, "Add to Custom Colors", on_add_custom,
              color_window);
    set_rgb(color_window, initial, -1);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, ok);
    xd_window_show(win);
    return color_window;
}

/* ---- the colour button ---------------------------------------------------------------- */

/* Windows' default palette: the 20 colours the button drops down. */
static const xd_rgb palette[20] = {
    {0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},   {0, 0, 128},
    {128, 0, 128},   {0, 128, 128},   {192, 192, 192}, {192, 220, 192}, {166, 202, 240},
    {255, 251, 240}, {160, 160, 164}, {128, 128, 128}, {255, 0, 0},     {0, 255, 0},
    {255, 255, 0},   {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
};

enum {
    PICK_COLS = 4,
    PICK_ROWS = 5,
    PICK_CELL = 18,
    PICK_PAD = 4,
    PICK_BUTTON_H = 23,
    PICK_W = 2 * PICK_PAD + PICK_COLS * PICK_CELL,
    PICK_OTHER_Y = PICK_PAD + PICK_ROWS * PICK_CELL + PICK_PAD,
    PICK_H = PICK_OTHER_Y + PICK_BUTTON_H + PICK_PAD,
};

struct color_button {
    xd_widget *w;
    xd_rgb rgb;
    bool enabled;
    bool held;       /* the button, while the mouse is down on it */
    int hot;         /* the list's cell under the pointer; -1 none */
    bool other_held; /* Other..., while the mouse is down on it */
    bool over_other;
    color_dialog *dialog;
    color_button_cb cb;
    void *user;
};

static void draw_button([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                        const xd_theme *theme, void *user) {
    color_button *button = user;
    uint8_t state = 0;
    if (!button->enabled)
        state |= XD_STATE_DISABLED;
    if (button->held)
        state |= XD_STATE_PRESSED;
    xd_theme_draw_button(theme, cairo, rect, state, "");
    int pixel = xd_tpx(theme, 1);
    int shift = button->held ? pixel : 0;
    /* The swatch, a separator and the drop-down arrow. */
    int arrow_w = xd_tpx(theme, 13);
    xd_rect sw = {rect.x + xd_tpx(theme, 5) + shift, rect.y + xd_tpx(theme, 5) + shift,
                  rect.w - xd_tpx(theme, 10) - arrow_w, rect.h - xd_tpx(theme, 10)};
    if (button->enabled) {
        xd_rgba black = xd_sys(theme, XD_COLOR_BUTTON_TEXT);
        xd_rect in = xd_draw_bevel(cairo, sw, pixel, black, black);
        xd_fill_rect(cairo, in, xd_rgba_from_rgb(button->rgb));
    }
    int sx = sw.x + sw.w + xd_tpx(theme, 3);
    xd_fill_rect(cairo, (xd_rect){sx, sw.y, pixel, sw.h}, xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
    xd_fill_rect(cairo, (xd_rect){sx + pixel, sw.y, pixel, sw.h},
                 xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    double ax = sx + xd_tpx(theme, 4), ay = rect.y + rect.h / 2.0 - xd_tpx(theme, 1) + shift;
    double arrow_size = xd_tpx(theme, 3);
    cairo_move_to(cairo, ax, ay);
    cairo_line_to(cairo, ax + 2 * arrow_size + 1, ay);
    cairo_line_to(cairo, ax + arrow_size + 0.5, ay + arrow_size + 0.5);
    cairo_close_path(cairo);
    xd_set_color(cairo, xd_sys(theme, button->enabled ? XD_COLOR_BUTTON_TEXT : XD_COLOR_GRAY_TEXT));
    cairo_fill(cairo);
}

static xd_rect pick_cell(int i) {
    return (xd_rect){PICK_PAD + (i % PICK_COLS) * PICK_CELL, PICK_PAD + (i / PICK_COLS) * PICK_CELL,
                     PICK_CELL, PICK_CELL};
}

static const xd_rect PICK_OTHER = {PICK_PAD, PICK_OTHER_Y, PICK_W - 2 * PICK_PAD, PICK_BUTTON_H};

static xd_rect scaled(const xd_theme *theme, xd_rect rect) {
    return (xd_rect){xd_tpx(theme, rect.x), xd_tpx(theme, rect.y), xd_tpx(theme, rect.w),
                     xd_tpx(theme, rect.h)};
}

static void draw_picker([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                        const xd_theme *theme, void *user) {
    color_button *button = user;
    int pixel = xd_tpx(theme, 1);
    xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    /* A raised edge, as a menu's. */
    xd_rect in = xd_draw_bevel(cairo, rect, pixel, xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
                               xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
    xd_draw_bevel(cairo, in, pixel, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                  xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
    int sel = -1;
    for (int i = 0; i < (int)XD_ARRAY_LEN(palette) && sel < 0; i++) {
        if (same_rgb(palette[i], button->rgb))
            sel = i;
    }
    xd_rgba frame = xd_sys(theme, XD_COLOR_BUTTON_TEXT);
    for (int i = 0; i < (int)XD_ARRAY_LEN(palette); i++) {
        xd_rect cell = scaled(theme, pick_cell(i));
        if (i == button->hot || i == sel)
            xd_draw_bevel(cairo, xd_rect_inset(cell, pixel, pixel), pixel, frame, frame);
        draw_well(theme, cairo, xd_rect_inset(cell, 2 * pixel, 2 * pixel), palette[i]);
    }
    uint8_t state = button->other_held && button->over_other ? XD_STATE_PRESSED : 0;
    xd_theme_draw_button(theme, cairo, scaled(theme, PICK_OTHER), state, "Other...");
}

static bool inside(xd_rect rect, double x, double y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

static void dialog_done(bool ok, xd_rgb rgb, void *user) {
    color_button *button = user;
    button->dialog = nullptr;
    if (!ok)
        return;
    button->rgb = rgb;
    xd_widget_redraw(button->w);
    button->cb(button, rgb, button->user);
}

static void on_picker_pointer(xd_widget *widget, xd_pointer_event ev, double x, double y,
                              void *user) {
    color_button *button = user;
    int hot = -1;
    for (int i = 0; i < (int)XD_ARRAY_LEN(palette); i++) {
        if (inside(pick_cell(i), x, y))
            hot = i;
    }
    button->hot = hot;
    button->over_other = inside(PICK_OTHER, x, y);
    if (ev == XD_POINTER_PRESS) {
        button->other_held = button->over_other;
        return;
    }
    if (ev != XD_POINTER_RELEASE)
        return;
    bool other = button->other_held && button->over_other;
    button->other_held = false;
    button->held = false; /* a drag from the button, released over the list */
    xd_widget_redraw(button->w);
    if (hot >= 0) {
        xd_popup_dismiss(xd_widget_window(widget));
        button->rgb = palette[hot];
        xd_widget_redraw(button->w);
        button->cb(button, button->rgb, button->user);
    } else if (other) {
        xd_popup_dismiss(xd_widget_window(widget));
        xd_ui *ui = xd_window_ui(xd_widget_window(widget));
        if (button->dialog == nullptr)
            button->dialog = color_dialog_open(ui, button->rgb, dialog_done, button);
    }
}

static void on_button_pointer(xd_widget *widget, xd_pointer_event ev, [[maybe_unused]] double x,
                              [[maybe_unused]] double y, void *user) {
    color_button *button = user;
    if (ev == XD_POINTER_PRESS) {
        button->held = true;
        button->hot = -1;
        button->other_held = false;
        xd_widget_redraw(widget);
        /* Down on the press, as Windows' does. */
        xd_popup_custom(widget, PICK_W, PICK_H, draw_picker, on_picker_pointer, button);
    } else if (ev == XD_POINTER_RELEASE) {
        button->held = false;
        xd_widget_redraw(widget);
    }
}

color_button *color_button_create(xd_window *win, xd_rect rect, color_button_cb on_change,
                                  void *user) {
    color_button *button = XD_NEW(color_button);
    button->enabled = true;
    button->hot = -1;
    button->cb = on_change;
    button->user = user;
    button->w = xd_custom(win, rect, draw_button, button);
    xd_custom_set_pointer(button->w, on_button_pointer);
    return button;
}

void color_button_set(color_button *button, xd_rgb rgb) {
    button->rgb = rgb;
    xd_widget_redraw(button->w);
}

xd_rgb color_button_value(const color_button *button) { return button->rgb; }

void color_button_set_enabled(color_button *button, bool enabled) {
    button->enabled = enabled;
    xd_widget_set_enabled(button->w, enabled);
    xd_widget_redraw(button->w);
}

void color_button_free(color_button *button) {
    if (button == nullptr)
        return;
    if (button->dialog != nullptr)
        destroy(button->dialog); /* without telling b, which goes too */
    xd_free(button);
}
