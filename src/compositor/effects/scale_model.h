/* SPDX-License-Identifier: MIT
 * Scale's maths, apart from wlroots so the unit tests can check it: where
 * each window's thumbnail goes (Compiz's scale plugin's slots), and the
 * glide there and back (its velocity model, which Ring Switcher shares).
 */
#ifndef XD_COMPOSITOR_EFFECTS_SCALE_MODEL_H
#define XD_COMPOSITOR_EFFECTS_SCALE_MODEL_H

#include <stddef.h>

#include "geometry.h"

/* Where a window's outer box goes: its top left, and its size times
 * `scale` (never above 1). */
typedef struct xd_scale_slot {
    double x, y, scale;
} xd_scale_slot;

/* Thumbnails for `count` windows (their outer boxes, top window first)
 * in rows across `area`, `spacing` apart and from its edges: about the
 * square root of count rows, each window in the free slot nearest to it,
 * centred there. */
void xd_scale_layout(const xd_effect_rect *windows, size_t count, xd_effect_rect area,
                     double spacing, xd_scale_slot *slots);

/* A thumbnail on its way: where it is, and how fast it's going. */
typedef struct xd_scale_motion {
    double x, y, scale;
    double velocity_x, velocity_y, velocity_scale;
} xd_scale_motion;

/* Moves `motion` towards `target` for `milliseconds` as Compiz did (speed
 * and timestep as its options); false once it has arrived, where it then
 * is exactly. */
bool xd_scale_step_motion(xd_scale_motion *motion, xd_scale_slot target, double milliseconds,
                          double speed, double timestep);

/* The thumbnail under (x, y), of `count` windows of those sizes placed
 * as `motions` say, the last (drawn on top) first; -1 for none. */
int xd_scale_find_thumbnail_at(const xd_effect_rect *windows, const xd_scale_motion *motions,
                               size_t count, double x, double y);

/* From slot `from`, the nearest one in a direction (dx, dy: one of them
 * -1 or 1, the other 0), or `from` if there's none that way. */
size_t xd_scale_find_neighbour(const xd_effect_rect *windows, const xd_scale_slot *slots,
                               size_t count, size_t from, int dx, int dy);

#endif
