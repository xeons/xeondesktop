/* SPDX-License-Identifier: MIT
 * Ring Switcher's maths, apart from wlroots so the unit tests can check
 * it: the windows on an ellipse, the one picked at the front (the bottom
 * of the ellipse), larger and brighter than those behind, as Compiz's
 * ring plugin drew them.
 */
#ifndef XD_COMPOSITOR_EFFECTS_RING_MODEL_H
#define XD_COMPOSITOR_EFFECTS_RING_MODEL_H

#include <stddef.h>

#include "geometry.h"

typedef struct xd_ring_item {
    double x, y;       /* the thumbnail's centre */
    double scale;      /* min_scale at the back to 1 at the front */
    double brightness; /* min_brightness at the back to 1 at the front */
    double depth;      /* 0 at the back to 1 at the front: draw in this order */
} xd_ring_item;

/* `count` windows evenly round the ellipse inside `ring`, turned so that
 * window `rotation` (it may be fractional, between two) is at the front. */
void xd_ring_layout(size_t count, double rotation, xd_effect_rect ring, double min_scale,
                    double min_brightness, xd_ring_item *items);

/* The window `step` places on from `index` (backwards when negative), of
 * `count`, round the end. */
size_t xd_ring_step_index(size_t index, int step, size_t count);

#endif
