/* SPDX-License-Identifier: MIT
 * Enhanced Zoom Desktop's maths (zoom_model.h).
 */
#include "zoom_model.h"

#include <math.h>

#include "../../core/common.h"

/* Speed 25 (the default) closes 90% of the way in about a quarter of a
 * second. */
static constexpr double ZOOM_RATE_PER_SPEED = 0.37;
static constexpr double ZOOM_CLOSE_ENOUGH = 0.002;

xd_effect_rect xd_zoom_get_view(xd_effect_rect output, double pointer_x, double pointer_y,
                                double level) {
    double zoom = XD_MAX(level, 1.0);
    double x = XD_CLAMP(pointer_x, output.x, output.x + output.width);
    double y = XD_CLAMP(pointer_y, output.y, output.y + output.height);
    /* The pointer's point is the same share of the way across the view
     * as across the monitor. */
    return (xd_effect_rect){
        .x = x - (x - output.x) / zoom,
        .y = y - (y - output.y) / zoom,
        .width = output.width / zoom,
        .height = output.height / zoom,
    };
}

double xd_zoom_step_level(double level, double target, double seconds, double speed) {
    double kept = exp(-ZOOM_RATE_PER_SPEED * XD_MAX(speed, 0.01) * XD_MAX(seconds, 0.0));
    double next = target + (level - target) * kept;
    if (fabs(next - target) < ZOOM_CLOSE_ENOUGH * target)
        return target;
    return next;
}
