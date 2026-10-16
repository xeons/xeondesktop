/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "graph.h"

static constexpr double GRID = 12; /* grid square, px */
static constexpr double STEP = 2;  /* px per sample */

void xd_history_init(xd_history *h, int cap) {
    *h = (xd_history){
        .value = g_new0(double, (gsize)cap),
        .kernel = g_new0(double, (gsize)cap),
        .cap = cap,
    };
}

void xd_history_clear(xd_history *h) {
    g_free(h->value);
    g_free(h->kernel);
    *h = (xd_history){0};
}

void xd_history_push(xd_history *h, double value, double kernel) {
    if (h->cap == 0)
        return;
    h->value[h->head] = CLAMP(value, 0.0, 1.0);
    h->kernel[h->head] = CLAMP(kernel, 0.0, 1.0);
    h->head = (h->head + 1) % h->cap;
    if (h->len < h->cap)
        h->len++;
    h->pushed++;
}

/* i = 0 is the newest sample. */
static double hist_at(const xd_history *h, const double *data, int i) {
    int idx = ((h->head - 1 - i) % h->cap + h->cap) % h->cap;
    return data[idx];
}

/* (int)lround(x) trips -Wbad-function-cast. */
static int round_int(double v) {
    double r = round(v);
    return (int)r;
}

static void rgb(cairo_t *cr, int r, int g, int b) {
    cairo_set_source_rgb(cr, r / 255.0, g / 255.0, b / 255.0);
}

void xd_meter_draw(cairo_t *cr, double x, double y, double w, double h, double value,
                   double kernel, gboolean show_kernel, const char *text) {
    cairo_save(cr);
    rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);

    /* Caption at the bottom. */
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    double text_y = y + h - 6;
    rgb(cr, 0, 255, 0);
    cairo_move_to(cr, floor(x + (w - ext.x_advance) / 2), floor(text_y));
    cairo_show_text(cr, text);

    /* Two columns of 2px LED rows with 1px gaps. */
    double top = y + 6, bottom = text_y - ext.height - 5;
    double col_w = floor(MIN(w * 0.24, 16));
    double left = floor(x + w / 2 - col_w - 0.5);
    double row_space = floor((bottom - top) / 3);
    int rows = (int)row_space;
    if (rows <= 0) {
        cairo_restore(cr);
        return;
    }
    int lit = round_int(CLAMP(value, 0.0, 1.0) * rows);
    int red = show_kernel ? round_int(CLAMP(kernel, 0.0, 1.0) * rows) : 0;
    for (int r = 0; r < rows; r++) {
        double ry = bottom - (r + 1) * 3 + 1;
        for (int c = 0; c < 2; c++) {
            double cx = left + c * (col_w + 1);
            if (r < red) {
                rgb(cr, 255, 0, 0);
                cairo_rectangle(cr, cx, ry, col_w, 2);
                cairo_fill(cr);
            } else if (r < lit) {
                rgb(cr, 0, 255, 0);
                cairo_rectangle(cr, cx, ry, col_w, 2);
                cairo_fill(cr);
            } else {
                /* Unlit: a dotted dark green row. */
                rgb(cr, 0, 128, 0);
                for (double px = 0; px < col_w; px += 2)
                    cairo_rectangle(cr, cx + px + (r % 2), ry, 1, 1);
                cairo_fill(cr);
            }
        }
    }
    cairo_restore(cr);
}

static void plot(cairo_t *cr, double x, double y, double w, double h, const xd_history *hist,
                 const double *data) {
    int n = MIN(hist->len, (int)(w / STEP) + 2);
    if (n < 2)
        return;
    for (int i = 0; i < n; i++) {
        double px = x + w - 1 - i * STEP + 0.5;
        double py = y + (h - 1) * (1.0 - hist_at(hist, data, i)) + 0.5;
        if (i == 0)
            cairo_move_to(cr, px, py);
        else
            cairo_line_to(cr, px, py);
    }
    cairo_stroke(cr);
}

void xd_history_draw(cairo_t *cr, double x, double y, double w, double h, const xd_history *hist,
                     gboolean show_kernel) {
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    /* Grid; vertical lines scroll left with the data. */
    cairo_set_line_width(cr, 1);
    rgb(cr, 0, 128, 64);
    for (double gy = y + h - 1; gy >= y; gy -= GRID) {
        cairo_move_to(cr, x, floor(gy) + 0.5);
        cairo_line_to(cr, x + w, floor(gy) + 0.5);
    }
    double shift = fmod((double)hist->pushed * STEP, GRID);
    for (double gx = x + w - 1 - (GRID - shift); gx >= x - GRID; gx -= GRID) {
        cairo_move_to(cr, floor(gx) + 0.5, y);
        cairo_line_to(cr, floor(gx) + 0.5, y + h);
    }
    cairo_stroke(cr);

    if (hist->cap > 0) {
        if (show_kernel) {
            rgb(cr, 255, 0, 0);
            plot(cr, x, y, w, h, hist, hist->kernel);
        }
        rgb(cr, 0, 255, 0);
        plot(cr, x, y, w, h, hist, hist->value);
    }
    cairo_restore(cr);
}
