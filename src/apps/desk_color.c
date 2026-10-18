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

void color_rgb_to_hls(xd_rgb c, int *hue, int *lum, int *sat) {
    int r = c.r, g = c.g, b = c.b;
    int mx = XD_MAX(XD_MAX(r, g), b);
    int mn = XD_MIN(XD_MIN(r, g), b);
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
    int rd = ((mx - r) * (HLS_MAX / 6) + diff / 2) / diff;
    int gd = ((mx - g) * (HLS_MAX / 6) + diff / 2) / diff;
    int bd = ((mx - b) * (HLS_MAX / 6) + diff / 2) / diff;
    int h;
    if (r == mx)
        h = bd - gd;
    else if (g == mx)
        h = HLS_MAX / 3 + rd - bd;
    else
        h = 2 * HLS_MAX / 3 + gd - rd;
    *hue = ((h % HLS_MAX) + HLS_MAX) % HLS_MAX;
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

static uint8_t channel(double v) {
    return (uint8_t)XD_CLAMP(lround(v * RGB_MAX / HLS_MAX), 0, RGB_MAX);
}

xd_rgb color_rgb_from_hls(int hue, int lum, int sat) {
    if (sat == 0) {
        uint8_t v = channel(lum);
        return (xd_rgb){v, v, v};
    }
    double l = lum, s = sat;
    double m2 = l <= HLS_MAX / 2.0 ? l * (HLS_MAX + s) / HLS_MAX : l + s - l * s / HLS_MAX;
    double m1 = 2 * l - m2;
    return (xd_rgb){channel(hue_to_rgb(m1, m2, hue + HLS_MAX / 3.0)),
                    channel(hue_to_rgb(m1, m2, hue)),
                    channel(hue_to_rgb(m1, m2, hue - HLS_MAX / 3.0))};
}

static bool same_rgb(xd_rgb a, xd_rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

/* A sunken 2px well around a colour, as Windows' colour swatches. */
static void draw_well(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rgb c) {
    int p = xd_tpx(t, 1);
    xd_rect in = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    in = xd_draw_bevel(cr, in, p, xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW),
                       xd_sys(t, XD_COLOR_BUTTON_LIGHT));
    xd_fill_rect(cr, in, xd_rgba_from_rgb(c));
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
static void draw_swatches(const xd_theme *t, cairo_t *cr, xd_rect r, const xd_rgb *colors,
                          int rows, int sel) {
    xd_fill_rect(cr, r, xd_theme_dialog_color(t));
    for (int i = 0; i < SWATCH_COLS * rows; i++) {
        xd_rect cell;
        swatch_rect(r, rows, i, &cell);
        xd_rect well = xd_rect_inset(cell, xd_tpx(t, 3), xd_tpx(t, 3));
        if (i == sel) {
            /* Windows frames the selection in black. */
            xd_rect f = xd_rect_inset(cell, xd_tpx(t, 1), xd_tpx(t, 1));
            xd_rgba black = xd_rgba_hex(0x000000);
            xd_draw_bevel(cr, f, xd_tpx(t, 1), black, black);
        }
        draw_well(t, cr, well, colors[i]);
    }
}

static void draw_basic([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                       void *user) {
    color_dialog *d = user;
    draw_swatches(t, cr, r, basic_colors, BASIC_ROWS, d->basic_sel);
}

static void draw_custom([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                        void *user) {
    color_dialog *d = user;
    draw_swatches(t, cr, r, custom_colors, CUSTOM_ROWS, d->custom_sel);
}

/* The spectrum: hue across, saturation from full at the top to none at the
 * bottom, at middle luminosity; a crosshair on the current hue and
 * saturation. */
static void draw_spectrum([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r,
                          const xd_theme *t, void *user) {
    color_dialog *d = user;
    int p = xd_tpx(t, 1);
    xd_rect in = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    if (d->spectrum_cache == nullptr || cairo_image_surface_get_width(d->spectrum_cache) != in.w ||
        cairo_image_surface_get_height(d->spectrum_cache) != in.h) {
        if (d->spectrum_cache != nullptr)
            cairo_surface_destroy(d->spectrum_cache);
        d->spectrum_cache = cairo_image_surface_create(CAIRO_FORMAT_RGB24, in.w, in.h);
        uint8_t *px = cairo_image_surface_get_data(d->spectrum_cache);
        int stride = cairo_image_surface_get_stride(d->spectrum_cache);
        for (int y = 0; y < in.h; y++) {
            int sat = HLS_MAX - y * HLS_MAX / XD_MAX(in.h - 1, 1);
            uint32_t *row = (uint32_t *)(void *)(px + (size_t)y * (size_t)stride);
            for (int x = 0; x < in.w; x++) {
                int hue = x * (HLS_MAX - 1) / XD_MAX(in.w - 1, 1);
                xd_rgb c = color_rgb_from_hls(hue, HLS_MAX / 2, sat);
                row[x] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
            }
        }
        cairo_surface_mark_dirty(d->spectrum_cache);
    }
    cairo_save(cr);
    cairo_rectangle(cr, in.x, in.y, in.w, in.h);
    cairo_clip(cr);
    cairo_set_source_surface(cr, d->spectrum_cache, in.x, in.y);
    cairo_paint(cr);
    /* The crosshair: four arms around a gap, as Windows draws it. */
    double cx = in.x + d->hue * (in.w - 1) / (double)(HLS_MAX - 1) + 0.5;
    double cy = in.y + (HLS_MAX - d->sat) * (in.h - 1) / (double)HLS_MAX + 0.5;
    double gap = xd_tpx(t, 3), arm = xd_tpx(t, 8);
    cairo_set_line_width(cr, xd_tpx(t, 2));
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_move_to(cr, cx - gap - arm, cy);
    cairo_line_to(cr, cx - gap, cy);
    cairo_move_to(cr, cx + gap, cy);
    cairo_line_to(cr, cx + gap + arm, cy);
    cairo_move_to(cr, cx, cy - gap - arm);
    cairo_line_to(cr, cx, cy - gap);
    cairo_move_to(cr, cx, cy + gap);
    cairo_line_to(cr, cx, cy + gap + arm);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static xd_rect lum_bar(const xd_theme *t, xd_rect r) {
    return (xd_rect){r.x, r.y, xd_tpx(t, LUM_BAR_W), r.h};
}

/* The luminosity bar for the current hue and saturation, white at the top
 * to black, with an arrow at the current luminosity. */
static void draw_lum([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                     void *user) {
    color_dialog *d = user;
    xd_fill_rect(cr, r, xd_theme_dialog_color(t));
    xd_rect bar = lum_bar(t, r);
    int p = xd_tpx(t, 1);
    xd_rect in = xd_draw_bevel(cr, bar, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    for (int y = 0; y < in.h; y++) {
        int lum = HLS_MAX - y * HLS_MAX / XD_MAX(in.h - 1, 1);
        xd_fill_rect(cr, (xd_rect){in.x, in.y + y, in.w, 1},
                     xd_rgba_from_rgb(color_rgb_from_hls(d->hue, lum, d->sat)));
    }
    double ay = in.y + (HLS_MAX - d->lumv) * (in.h - 1) / (double)HLS_MAX + 0.5;
    double ax = bar.x + bar.w + xd_tpx(t, 1);
    double s = xd_tpx(t, 5);
    cairo_move_to(cr, ax, ay);
    cairo_line_to(cr, ax + s, ay - s);
    cairo_line_to(cr, ax + s, ay + s);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_fill(cr);
}

/* Color|Solid: on a true-colour screen the nearest solid colour is the
 * colour itself, so both halves show it. */
static void draw_solid([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                       void *user) {
    color_dialog *d = user;
    int p = xd_tpx(t, 1);
    xd_rect in = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_SHADOW),
                               xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    xd_fill_rect(cr, in, xd_rgba_from_rgb(d->rgb));
}

/* Puts the current colour into the fields; `except` (being typed in) is
 * left alone. */
static void show_values(color_dialog *d, int except) {
    int v[E_COUNT] = {d->hue, d->sat, d->lumv, d->rgb.r, d->rgb.g, d->rgb.b};
    for (int i = 0; i < E_COUNT; i++) {
        if (i == except)
            continue;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", v[i]);
        xd_widget_set_text(d->edits[i], buf);
    }
    xd_widget_redraw(d->spectrum);
    xd_widget_redraw(d->lum);
    xd_widget_redraw(d->solid);
    xd_widget_redraw(d->basic);
    xd_widget_redraw(d->custom);
}

static void set_rgb(color_dialog *d, xd_rgb c, int except) {
    d->rgb = c;
    color_rgb_to_hls(c, &d->hue, &d->lumv, &d->sat);
    show_values(d, except);
}

static void set_hls(color_dialog *d, int hue, int lum, int sat, int except) {
    d->hue = XD_CLAMP(hue, 0, HLS_MAX - 1);
    d->lumv = XD_CLAMP(lum, 0, HLS_MAX);
    d->sat = XD_CLAMP(sat, 0, HLS_MAX);
    d->rgb = color_rgb_from_hls(d->hue, d->lumv, d->sat);
    show_values(d, except);
}

static int swatch_at(xd_rect area, int rows, double x, double y) {
    if (x < 0 || y < 0 || x >= area.w || y >= area.h)
        return -1;
    int col = (int)(x / (area.w / SWATCH_COLS)), row = (int)(y / (area.h / rows));
    if (col >= SWATCH_COLS || row >= rows)
        return -1;
    return row * SWATCH_COLS + col;
}

static void on_basic_pointer([[maybe_unused]] xd_widget *w, xd_pointer_event ev, double x,
                             double y, void *user) {
    color_dialog *d = user;
    int i = swatch_at(BASIC_R, BASIC_ROWS, x, y);
    if (ev != XD_POINTER_PRESS || i < 0)
        return;
    d->basic_sel = i;
    d->custom_sel = -1;
    set_rgb(d, basic_colors[i], -1);
}

static void on_custom_pointer([[maybe_unused]] xd_widget *w, xd_pointer_event ev, double x,
                              double y, void *user) {
    color_dialog *d = user;
    int i = swatch_at(CUSTOM_R, CUSTOM_ROWS, x, y);
    if (ev != XD_POINTER_PRESS || i < 0)
        return;
    d->custom_sel = i;
    d->basic_sel = -1;
    set_rgb(d, custom_colors[i], -1);
}

static void on_spectrum_pointer([[maybe_unused]] xd_widget *w, xd_pointer_event ev, double x,
                                double y, void *user) {
    color_dialog *d = user;
    if (ev == XD_POINTER_RELEASE)
        return;
    /* Inside the 1px edge. */
    double fx = XD_CLAMP((x - 1) / (SPECTRUM_R.w - 3), 0.0, 1.0);
    double fy = XD_CLAMP((y - 1) / (SPECTRUM_R.h - 3), 0.0, 1.0);
    d->basic_sel = d->custom_sel = -1;
    /* The luminosity stays: a white or black colour stays so, as in
     * Windows. */
    set_hls(d, (int)lround(fx * (HLS_MAX - 1)), d->lumv, (int)lround((1 - fy) * HLS_MAX), -1);
}

static void on_lum_pointer([[maybe_unused]] xd_widget *w, xd_pointer_event ev,
                           [[maybe_unused]] double x, double y, void *user) {
    color_dialog *d = user;
    if (ev == XD_POINTER_RELEASE)
        return;
    double fy = XD_CLAMP((y - 1) / (LUM_R.h - 3), 0.0, 1.0);
    d->basic_sel = d->custom_sel = -1;
    set_hls(d, d->hue, (int)lround((1 - fy) * HLS_MAX), d->sat, -1);
}

static void on_edit(xd_widget *w, void *user) {
    color_dialog *d = user;
    int which = 0;
    while (which < E_COUNT && d->edits[which] != w)
        which++;
    int64_t v;
    int max = which == E_HUE ? HLS_MAX - 1 : which < E_RED ? HLS_MAX : RGB_MAX;
    if (which == E_COUNT || !xd_str_parse_i64(xd_str_from_cstr(xd_widget_text(w)), 0, max, &v))
        return; /* typing on: it's read once it's a number in range */
    d->basic_sel = d->custom_sel = -1;
    int n = (int)v;
    switch (which) {
    case E_HUE:
        set_hls(d, n, d->lumv, d->sat, which);
        break;
    case E_SAT:
        set_hls(d, d->hue, d->lumv, n, which);
        break;
    case E_LUM:
        set_hls(d, d->hue, n, d->sat, which);
        break;
    case E_RED:
        set_rgb(d, (xd_rgb){(uint8_t)n, d->rgb.g, d->rgb.b}, which);
        break;
    case E_GREEN:
        set_rgb(d, (xd_rgb){d->rgb.r, (uint8_t)n, d->rgb.b}, which);
        break;
    default:
        set_rgb(d, (xd_rgb){d->rgb.r, d->rgb.g, (uint8_t)n}, which);
        break;
    }
}

/* Into the selected custom box (the first if none is), then on to the
 * next, as Windows does. */
static void on_add_custom([[maybe_unused]] xd_widget *w, void *user) {
    color_dialog *d = user;
    int i = d->custom_sel >= 0 ? d->custom_sel : 0;
    custom_colors[i] = d->rgb;
    d->custom_sel = (i + 1) % CUSTOM_COUNT;
    xd_widget_redraw(d->custom);
}

static void destroy(color_dialog *d) {
    if (d->spectrum_cache != nullptr)
        cairo_surface_destroy(d->spectrum_cache);
    xd_window_close(d->win);
    xd_free(d);
}

static void finish(color_dialog *d, bool ok) {
    color_dialog_done done = d->done;
    void *user = d->user;
    xd_rgb rgb = d->rgb;
    destroy(d);
    done(ok, rgb, user);
}

void color_dialog_cancel(color_dialog *d) { finish(d, false); }

static void on_ok([[maybe_unused]] xd_widget *w, void *user) { finish(user, true); }
static void on_cancel([[maybe_unused]] xd_widget *w, void *user) { finish(user, false); }

color_dialog *color_dialog_open(xd_ui *ui, xd_rgb initial, color_dialog_done done, void *user) {
    color_dialog *d = XD_NEW(color_dialog);
    d->ui = ui;
    d->done = done;
    d->user = user;
    d->basic_sel = d->custom_sel = -1;
    for (int i = 0; i < (int)XD_ARRAY_LEN(basic_colors) && d->basic_sel < 0; i++) {
        if (same_rgb(basic_colors[i], initial))
            d->basic_sel = i;
    }
    xd_window *win = d->win = xd_window_create(ui, "Color", "xd-desk", DLG_W, DLG_H);
    xd_label(win, (xd_rect){6, 6, 210, 16}, "Basic colors:", 0);
    d->basic = xd_custom(win, BASIC_R, draw_basic, d);
    xd_custom_set_pointer(d->basic, on_basic_pointer);
    xd_label(win, (xd_rect){6, 171, 210, 16}, "Custom colors:", 0);
    d->custom = xd_custom(win, CUSTOM_R, draw_custom, d);
    xd_custom_set_pointer(d->custom, on_custom_pointer);
    xd_widget_set_enabled(xd_button(win, (xd_rect){6, 244, 213, 23}, "Define Custom Colors >>",
                                    nullptr, nullptr),
                          false);
    xd_widget *ok = xd_button(win, (xd_rect){6, 271, 66, 23}, "OK", on_ok, d);
    xd_widget *cancel = xd_button(win, (xd_rect){78, 271, 66, 23}, "Cancel", on_cancel, d);

    d->spectrum = xd_custom(win, SPECTRUM_R, draw_spectrum, d);
    xd_custom_set_pointer(d->spectrum, on_spectrum_pointer);
    d->lum = xd_custom(win, LUM_R, draw_lum, d);
    xd_custom_set_pointer(d->lum, on_lum_pointer);
    d->solid = xd_custom(win, (xd_rect){228, 202, 60, 40}, draw_solid, d);
    xd_label(win, (xd_rect){226, 246, 70, 16}, "Color|Solid", 0);
    static const char *const names[E_COUNT] = {"Hue:", "Sat:", "Lum:", "Red:", "Green:", "Blue:"};
    for (int i = 0; i < E_COUNT; i++) {
        int col = i / 3, row = i % 3;
        int lx = col == 0 ? 300 : 370, ex = col == 0 ? 330 : 410;
        int y = 202 + row * 22;
        xd_label(win, (xd_rect){lx, y + 3, ex - lx - 2, 16}, names[i], 0);
        d->edits[i] = xd_edit(win, (xd_rect){ex, y, 32, 20}, "");
        xd_edit_set_on_change(d->edits[i], on_edit, d);
    }
    xd_button(win, (xd_rect){228, 271, 213, 23}, "Add to Custom Colors", on_add_custom, d);
    set_rgb(d, initial, -1);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, ok);
    xd_window_show(win);
    return d;
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

static void draw_button([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                        void *user) {
    color_button *b = user;
    uint8_t state = 0;
    if (!b->enabled)
        state |= XD_STATE_DISABLED;
    if (b->held)
        state |= XD_STATE_PRESSED;
    xd_theme_draw_button(t, cr, r, state, "");
    int p = xd_tpx(t, 1);
    int shift = b->held ? p : 0;
    /* The swatch, a separator and the drop-down arrow. */
    int arrow_w = xd_tpx(t, 13);
    xd_rect sw = {r.x + xd_tpx(t, 5) + shift, r.y + xd_tpx(t, 5) + shift,
                  r.w - xd_tpx(t, 10) - arrow_w, r.h - xd_tpx(t, 10)};
    if (b->enabled) {
        xd_rgba black = xd_sys(t, XD_COLOR_BUTTON_TEXT);
        xd_rect in = xd_draw_bevel(cr, sw, p, black, black);
        xd_fill_rect(cr, in, xd_rgba_from_rgb(b->rgb));
    }
    int sx = sw.x + sw.w + xd_tpx(t, 3);
    xd_fill_rect(cr, (xd_rect){sx, sw.y, p, sw.h}, xd_sys(t, XD_COLOR_BUTTON_SHADOW));
    xd_fill_rect(cr, (xd_rect){sx + p, sw.y, p, sw.h}, xd_sys(t, XD_COLOR_BUTTON_HILIGHT));
    double ax = sx + xd_tpx(t, 4), ay = r.y + r.h / 2.0 - xd_tpx(t, 1) + shift;
    double a = xd_tpx(t, 3);
    cairo_move_to(cr, ax, ay);
    cairo_line_to(cr, ax + 2 * a + 1, ay);
    cairo_line_to(cr, ax + a + 0.5, ay + a + 0.5);
    cairo_close_path(cr);
    xd_set_color(cr, xd_sys(t, b->enabled ? XD_COLOR_BUTTON_TEXT : XD_COLOR_GRAY_TEXT));
    cairo_fill(cr);
}

static xd_rect pick_cell(int i) {
    return (xd_rect){PICK_PAD + (i % PICK_COLS) * PICK_CELL, PICK_PAD + (i / PICK_COLS) * PICK_CELL,
                     PICK_CELL, PICK_CELL};
}

static const xd_rect PICK_OTHER = {PICK_PAD, PICK_OTHER_Y, PICK_W - 2 * PICK_PAD, PICK_BUTTON_H};

static xd_rect scaled(const xd_theme *t, xd_rect r) {
    return (xd_rect){xd_tpx(t, r.x), xd_tpx(t, r.y), xd_tpx(t, r.w), xd_tpx(t, r.h)};
}

static void draw_picker([[maybe_unused]] xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t,
                        void *user) {
    color_button *b = user;
    int p = xd_tpx(t, 1);
    xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    /* A raised edge, as a menu's. */
    xd_rect in = xd_draw_bevel(cr, r, p, xd_sys(t, XD_COLOR_BUTTON_LIGHT),
                               xd_sys(t, XD_COLOR_BUTTON_DK_SHADOW));
    xd_draw_bevel(cr, in, p, xd_sys(t, XD_COLOR_BUTTON_HILIGHT),
                  xd_sys(t, XD_COLOR_BUTTON_SHADOW));
    int sel = -1;
    for (int i = 0; i < (int)XD_ARRAY_LEN(palette) && sel < 0; i++) {
        if (same_rgb(palette[i], b->rgb))
            sel = i;
    }
    xd_rgba frame = xd_sys(t, XD_COLOR_BUTTON_TEXT);
    for (int i = 0; i < (int)XD_ARRAY_LEN(palette); i++) {
        xd_rect cell = scaled(t, pick_cell(i));
        if (i == b->hot || i == sel)
            xd_draw_bevel(cr, xd_rect_inset(cell, p, p), p, frame, frame);
        draw_well(t, cr, xd_rect_inset(cell, 2 * p, 2 * p), palette[i]);
    }
    uint8_t state = b->other_held && b->over_other ? XD_STATE_PRESSED : 0;
    xd_theme_draw_button(t, cr, scaled(t, PICK_OTHER), state, "Other...");
}

static bool inside(xd_rect r, double x, double y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void dialog_done(bool ok, xd_rgb rgb, void *user) {
    color_button *b = user;
    b->dialog = nullptr;
    if (!ok)
        return;
    b->rgb = rgb;
    xd_widget_redraw(b->w);
    b->cb(b, rgb, b->user);
}

static void on_picker_pointer(xd_widget *w, xd_pointer_event ev, double x, double y, void *user) {
    color_button *b = user;
    int hot = -1;
    for (int i = 0; i < (int)XD_ARRAY_LEN(palette); i++) {
        if (inside(pick_cell(i), x, y))
            hot = i;
    }
    b->hot = hot;
    b->over_other = inside(PICK_OTHER, x, y);
    if (ev == XD_POINTER_PRESS) {
        b->other_held = b->over_other;
        return;
    }
    if (ev != XD_POINTER_RELEASE)
        return;
    bool other = b->other_held && b->over_other;
    b->other_held = false;
    b->held = false; /* a drag from the button, released over the list */
    xd_widget_redraw(b->w);
    if (hot >= 0) {
        xd_popup_dismiss(xd_widget_window(w));
        b->rgb = palette[hot];
        xd_widget_redraw(b->w);
        b->cb(b, b->rgb, b->user);
    } else if (other) {
        xd_popup_dismiss(xd_widget_window(w));
        xd_ui *ui = xd_window_ui(xd_widget_window(w));
        if (b->dialog == nullptr)
            b->dialog = color_dialog_open(ui, b->rgb, dialog_done, b);
    }
}

static void on_button_pointer(xd_widget *w, xd_pointer_event ev, [[maybe_unused]] double x,
                              [[maybe_unused]] double y, void *user) {
    color_button *b = user;
    if (ev == XD_POINTER_PRESS) {
        b->held = true;
        b->hot = -1;
        b->other_held = false;
        xd_widget_redraw(w);
        /* Down on the press, as Windows' does. */
        xd_popup_custom(w, PICK_W, PICK_H, draw_picker, on_picker_pointer, b);
    } else if (ev == XD_POINTER_RELEASE) {
        b->held = false;
        xd_widget_redraw(w);
    }
}

color_button *color_button_create(xd_window *win, xd_rect r, color_button_cb on_change,
                                  void *user) {
    color_button *b = XD_NEW(color_button);
    b->enabled = true;
    b->hot = -1;
    b->cb = on_change;
    b->user = user;
    b->w = xd_custom(win, r, draw_button, b);
    xd_custom_set_pointer(b->w, on_button_pointer);
    return b;
}

void color_button_set(color_button *b, xd_rgb rgb) {
    b->rgb = rgb;
    xd_widget_redraw(b->w);
}

xd_rgb color_button_value(const color_button *b) { return b->rgb; }

void color_button_set_enabled(color_button *b, bool enabled) {
    b->enabled = enabled;
    xd_widget_set_enabled(b->w, enabled);
    xd_widget_redraw(b->w);
}

void color_button_free(color_button *b) {
    if (b == nullptr)
        return;
    if (b->dialog != nullptr)
        destroy(b->dialog); /* without telling b, which goes too */
    xd_free(b);
}
