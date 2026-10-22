/* SPDX-License-Identifier: MIT
 * Screen savers: cairo animations shared by xd-lock (full screen) and
 * Display Properties (the preview monitor).
 */
#ifndef XD_SAVER_SAVERS_H
#define XD_SAVER_SAVERS_H

#include <cairo.h>
#include <stddef.h>
#include <stdint.h>

#include "../settings/settings.h"

typedef struct xd_saver_info {
    const char *id;   /* stored in desktop.ini */
    const char *name; /* shown in Display Properties */
} xd_saver_info;

/* The savers, in display order. */
size_t xd_savers(const xd_saver_info **out);
/* nullptr if id isn't a saver. */
const xd_saver_info *xd_saver_find(const char *id);
/* False for a saver whose picture never changes (Blank Screen): paint it
 * once instead of every frame. */
bool xd_saver_animated(const char *id);
/* Whether the saver has a Settings dialog (all but Blank Screen). */
bool xd_saver_has_settings(const char *id);

typedef struct xd_saver xd_saver;

/* An animation for an area of w x h device pixels, with the saver's
 * settings from `saver_settings` (nullptr: the defaults); scale sizes lines and text
 * (1.0 at 96 DPI). nullptr if id is unknown. */
[[nodiscard]] xd_saver *xd_saver_create(const char *id, const xd_saver_settings *saver_settings,
                                        int width, int height, double scale);
/* The same, with its random numbers from `seed`: the same seed and the
 * same frame times draw the same pictures. */
[[nodiscard]] xd_saver *xd_saver_create_seeded(const char *id,
                                               const xd_saver_settings *saver_settings, int width,
                                               int height, double scale, uint64_t seed);
/* Advances by dt seconds and paints the whole area at (0, 0). */
void xd_saver_frame(xd_saver *saver, cairo_t *cairo, double dt);
void xd_saver_destroy(xd_saver *saver);

#endif
