/* SPDX-License-Identifier: MIT
 * Animations' maths, apart from wlroots so the unit tests can check it:
 * how far through its animation a window is, and where and how it's drawn
 * then by each effect (Compiz's animation plugin's Fade, Zoom, Glide 1
 * and Glide 2, the glides' tilt drawn as a flat squash).
 */
#ifndef XD_COMPOSITOR_EFFECTS_ANIMATIONS_MODEL_H
#define XD_COMPOSITOR_EFFECTS_ANIMATIONS_MODEL_H

#include <stdint.h>

#include "geometry.h"

/* In the order of the option's choices. */
typedef enum xd_animation_effect : uint8_t {
    XD_ANIMATION_NONE,
    XD_ANIMATION_FADE,
    XD_ANIMATION_ZOOM,
    XD_ANIMATION_GLIDE_1,
    XD_ANIMATION_GLIDE_2,
} xd_animation_effect;

/* The window's picture: its outer box's top left, its size times
 * scale_x and scale_y, and its opacity. */
typedef struct xd_animation_frame {
    double x, y, scale_x, scale_y, opacity;
} xd_animation_frame;

/* How much of the window shows `progress` (0 to 1) of the way through:
 * from 0 to 1 when it's appearing (opened, unminimized), slowing down as
 * it arrives; from 1 to 0 when it's going, speeding up as it leaves. */
double xd_animation_get_presence(bool appearing, double progress);

/* The window (its outer box) drawn when `presence` of it shows: 1 is the
 * window as it is, 0 gone. Zoom goes to `target` (its taskbar button,
 * minimized) or, without one, to its own middle. */
xd_animation_frame xd_animation_get_frame(xd_animation_effect effect, xd_effect_rect window,
                                          const xd_effect_rect *target, double presence);

#endif
