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

void xd_history_init(xd_history *h, int cap);
void xd_history_clear(xd_history *h);
void xd_history_push(xd_history *h, double value, double kernel);

/* LED meter in the rectangle: bars for `value` (0..1), the kernel part in
 * red when show_kernel, and `text` underneath. */
void xd_meter_draw(cairo_t *cr, double x, double y, double w, double h, double value,
                   double kernel, gboolean show_kernel, const char *text);

/* History graph filling the rectangle; with kernel, a red line too. */
void xd_history_draw(cairo_t *cr, double x, double y, double w, double h, const xd_history *hist,
                     gboolean show_kernel);

#endif
