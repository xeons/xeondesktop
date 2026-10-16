/* SPDX-License-Identifier: MIT
 * Screen savers: cairo animations shared by xd-lock (full screen) and
 * Display Properties (the preview monitor).
 */
#ifndef XD_SAVER_SAVERS_H
#define XD_SAVER_SAVERS_H

#include <cairo.h>
#include <stddef.h>

typedef struct xd_saver_info {
    const char *id;   /* stored in desktop.ini */
    const char *name; /* shown in Display Properties */
} xd_saver_info;

/* The savers, in display order. */
size_t xd_savers(const xd_saver_info **out);
/* nullptr if id isn't a saver. */
const xd_saver_info *xd_saver_find(const char *id);

typedef struct xd_saver xd_saver;

/* An animation for an area of w x h device pixels; scale sizes lines and
 * text (1.0 at 96 DPI). nullptr if id is unknown. */
[[nodiscard]] xd_saver *xd_saver_create(const char *id, int w, int h, double scale);
/* Advances by dt seconds and paints the whole area at (0, 0). */
void xd_saver_frame(xd_saver *s, cairo_t *cr, double dt);
void xd_saver_destroy(xd_saver *s);

#endif
