/* SPDX-License-Identifier: MIT
 * A rectangle in layout units with fractions, for the effects' pure
 * maths (scale_model.h, ring_model.h, animations_model.h, zoom_model.h),
 * which the unit tests check without wlroots.
 */
#ifndef XD_COMPOSITOR_EFFECTS_GEOMETRY_H
#define XD_COMPOSITOR_EFFECTS_GEOMETRY_H

typedef struct xd_effect_rect {
    double x, y, width, height;
} xd_effect_rect;

#endif
