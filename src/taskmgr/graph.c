/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "graph.h"

static constexpr double GRID = 12; /* grid square, px */
static constexpr double STEP = 2;  /* px per sample */

void xd_history_init(xd_history *history, int cap) {
    *history = (xd_history){
        .value = g_new0(double, (gsize)cap),
        .kernel = g_new0(double, (gsize)cap),
        .cap = cap,
    };
}

void xd_history_clear(xd_history *history) {
    g_free(history->value);
    g_free(history->kernel);
    *history = (xd_history){0};
}

void xd_history_push(xd_history *history, double value, double kernel) {
    if (history->cap == 0)
        return;
    history->value[history->head] = CLAMP(value, 0.0, 1.0);
    history->kernel[history->head] = CLAMP(kernel, 0.0, 1.0);
    history->head = (history->head + 1) % history->cap;
    if (history->len < history->cap)
        history->len++;
    history->pushed++;
}

/* i = 0 is the newest sample. */
static double hist_at(const xd_history *history, const double *data, int i) {
    int idx = ((history->head - 1 - i) % history->cap + history->cap) % history->cap;
    return data[idx];
}

/* (int)lround(x) trips -Wbad-function-cast. */
static int round_int(double value) {
    double rounded = round(value);
    return (int)rounded;
}

static void rgb(cairo_t *cairo, int red, int green, int blue) {
    cairo_set_source_rgb(cairo, red / 255.0, green / 255.0, blue / 255.0);
}

void xd_meter_draw(cairo_t *cairo, double x, double y, double width, double height, double value,
                   double kernel, gboolean show_kernel, const char *text) {
    cairo_save(cairo);
    rgb(cairo, 0, 0, 0);
    cairo_rectangle(cairo, x, y, width, height);
    cairo_fill(cairo);

    /* Caption at the bottom. */
    cairo_select_font_face(cairo, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cairo, 11);
    cairo_text_extents_t ext;
    cairo_text_extents(cairo, text, &ext);
    double text_y = y + height - 6;
    rgb(cairo, 0, 255, 0);
    cairo_move_to(cairo, floor(x + (width - ext.x_advance) / 2), floor(text_y));
    cairo_show_text(cairo, text);

    /* Two columns of 2px LED rows with 1px gaps. */
    double top = y + 6, bottom = text_y - ext.height - 5;
    double col_w = floor(MIN(width * 0.24, 16));
    double left = floor(x + width / 2 - col_w - 0.5);
    double row_space = floor((bottom - top) / 3);
    int rows = (int)row_space;
    if (rows <= 0) {
        cairo_restore(cairo);
        return;
    }
    int lit = round_int(CLAMP(value, 0.0, 1.0) * rows);
    int red = show_kernel ? round_int(CLAMP(kernel, 0.0, 1.0) * rows) : 0;
    for (int led_row = 0; led_row < rows; led_row++) {
        double ry = bottom - (led_row + 1) * 3 + 1;
        for (int led_column = 0; led_column < 2; led_column++) {
            double cx = left + led_column * (col_w + 1);
            if (led_row < red) {
                rgb(cairo, 255, 0, 0);
                cairo_rectangle(cairo, cx, ry, col_w, 2);
                cairo_fill(cairo);
            } else if (led_row < lit) {
                rgb(cairo, 0, 255, 0);
                cairo_rectangle(cairo, cx, ry, col_w, 2);
                cairo_fill(cairo);
            } else {
                /* Unlit: a dotted dark green row. */
                rgb(cairo, 0, 128, 0);
                for (double px = 0; px < col_w; px += 2)
                    cairo_rectangle(cairo, cx + px + (led_row % 2), ry, 1, 1);
                cairo_fill(cairo);
            }
        }
    }
    cairo_restore(cairo);
}

static void plot(cairo_t *cairo, double x, double y, double width, double height,
                 const xd_history *hist, const double *data, double full) {
    int count = MIN(hist->len, (int)(width / STEP) + 2);
    if (count < 2)
        return;
    for (int i = 0; i < count; i++) {
        double px = x + width - 1 - i * STEP + 0.5;
        double fraction = MIN(hist_at(hist, data, i) / full, 1.0);
        double py = y + (height - 1) * (1.0 - fraction) + 0.5;
        if (i == 0)
            cairo_move_to(cairo, px, py);
        else
            cairo_line_to(cairo, px, py);
    }
    cairo_stroke(cairo);
}

void xd_history_draw(cairo_t *cairo, double x, double y, double width, double height,
                     const xd_history *hist, gboolean show_kernel) {
    xd_history_draw_scaled(cairo, x, y, width, height, hist, show_kernel, 1.0);
}

void xd_history_draw_scaled(cairo_t *cairo, double x, double y, double width, double height,
                            const xd_history *hist, gboolean show_kernel, double full) {
    cairo_save(cairo);
    cairo_rectangle(cairo, x, y, width, height);
    cairo_clip(cairo);
    rgb(cairo, 0, 0, 0);
    cairo_paint(cairo);

    /* Grid; vertical lines scroll left with the data. */
    cairo_set_line_width(cairo, 1);
    rgb(cairo, 0, 128, 64);
    for (double gy = y + height - 1; gy >= y; gy -= GRID) {
        cairo_move_to(cairo, x, floor(gy) + 0.5);
        cairo_line_to(cairo, x + width, floor(gy) + 0.5);
    }
    double shift = fmod((double)hist->pushed * STEP, GRID);
    for (double gx = x + width - 1 - (GRID - shift); gx >= x - GRID; gx -= GRID) {
        cairo_move_to(cairo, floor(gx) + 0.5, y);
        cairo_line_to(cairo, floor(gx) + 0.5, y + height);
    }
    cairo_stroke(cairo);

    if (hist->cap > 0) {
        if (show_kernel) {
            rgb(cairo, 255, 0, 0);
            plot(cairo, x, y, width, height, hist, hist->kernel, full);
        }
        rgb(cairo, 0, 255, 0);
        plot(cairo, x, y, width, height, hist, hist->value, full);
    }
    cairo_restore(cairo);
}
