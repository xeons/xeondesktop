/* SPDX-License-Identifier: MIT
 * Ring Switcher's maths (ring_model.h).
 */
#include "ring_model.h"

#include <math.h>

#include "../../core/common.h"

void xd_ring_layout(size_t count, double rotation, xd_effect_rect ring, double min_scale,
                    double min_brightness, xd_ring_item *items) {
    double centre_x = ring.x + ring.width / 2, centre_y = ring.y + ring.height / 2;
    for (size_t i = 0; i < count; i++) {
        /* 0 at the front (the bottom), going round to the right. */
        double angle = 2.0 * XD_PI * ((double)i - rotation) / (double)count;
        double depth = (cos(angle) + 1.0) / 2.0;
        items[i] = (xd_ring_item){
            .x = centre_x + ring.width / 2 * sin(angle),
            .y = centre_y + ring.height / 2 * cos(angle),
            .scale = min_scale + (1.0 - min_scale) * depth,
            .brightness = min_brightness + (1.0 - min_brightness) * depth,
            .depth = depth,
        };
    }
}

size_t xd_ring_step_index(size_t index, int step, size_t count) {
    if (count == 0)
        return 0;
    long long next = ((long long)index + step) % (long long)count;
    if (next < 0)
        next += (long long)count;
    return (size_t)next;
}
