/* SPDX-License-Identifier: MIT
 * The Task Manager's Performance graphics: the green LED usage meter and
 * the scrolling history graph on a black, gridded background.
 */
#ifndef XD_TASKMGR_GRAPH_H
#define XD_TASKMGR_GRAPH_H

#include <cairo.h>
#include <glib.h>

/* Recent values, newest last. */
typedef struct xd_history {
    double *value, *kernel; /* 0..1 */
    int cap, len, head;     /* ring buffer */
    guint64 pushed;         /* samples ever pushed (scrolls the grid) */
} xd_history;

void xd_history_init(xd_history *history, int cap);
void xd_history_clear(xd_history *history);
void xd_history_push(xd_history *history, double value, double kernel);

/* LED meter in the rectangle: bars for `value` (0..1), the kernel part in
 * red when show_kernel, and `text` underneath. */
void xd_meter_draw(cairo_t *cairo, double x, double y, double width, double height, double value,
                   double kernel, gboolean show_kernel, const char *text);

/* History graph filling the rectangle; with kernel, a red line too. */
void xd_history_draw(cairo_t *cairo, double x, double y, double width, double height,
                     const xd_history *hist, gboolean show_kernel);
/* The same with `full` (0..1) at the top: Networking's graphs zoom in on
 * low utilization. */
void xd_history_draw_scaled(cairo_t *cairo, double x, double y, double width, double height,
                            const xd_history *hist, gboolean show_kernel, double full);

#endif
