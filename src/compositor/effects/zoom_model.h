/* SPDX-License-Identifier: MIT
 * Enhanced Zoom Desktop's maths, apart from wlroots so the unit tests can
 * check it: the part of a monitor that fills it at a zoom level, around
 * the pointer, and the level easing towards the one asked for.
 */
#ifndef XD_COMPOSITOR_EFFECTS_ZOOM_MODEL_H
#define XD_COMPOSITOR_EFFECTS_ZOOM_MODEL_H

#include "geometry.h"

/* The part of `output` (layout units) shown over the whole of it at
 * `level` (1: none, 2: twice the size), zoomed around (pointer_x,
 * pointer_y): what's under the pointer stays under it, so the pointer
 * still points at what it shows, and the view follows the pointer to
 * the edges. A pointer off the monitor counts as on its nearest edge. */
xd_effect_rect xd_zoom_get_view(xd_effect_rect output, double pointer_x, double pointer_y,
                                double level);

/* `level` eased towards `target` over `seconds`, faster at a higher
 * `speed` (Compiz's ezoom's option); exactly the target once close. */
double xd_zoom_step_level(double level, double target, double seconds, double speed);

#endif
