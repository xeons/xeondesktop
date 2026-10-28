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

/* Animations' own effects, then the Animations Add-On's (addon.c draws
 * those, with OpenGL). */
typedef enum xd_animation_effect : uint8_t {
    XD_ANIMATION_NONE,
    XD_ANIMATION_FADE,
    XD_ANIMATION_ZOOM,
    XD_ANIMATION_GLIDE_1,
    XD_ANIMATION_GLIDE_2,
    XD_ANIMATION_AIRPLANE,
    XD_ANIMATION_BEAM_UP,
    XD_ANIMATION_DOMINO,
    XD_ANIMATION_FOLD,
    XD_ANIMATION_GLIDE_3,
    XD_ANIMATION_LEAF_SPREAD,
    XD_ANIMATION_RAZR,
    XD_ANIMATION_SKEWER,
    XD_ANIMATION_EFFECT_COUNT,
} xd_animation_effect;

/* The effect a name in effects.ini stands for ("glide1", "beamup");
 * None for one it doesn't know. */
xd_animation_effect xd_animation_find_effect(const char *name);
/* Whether the Animations Add-On draws it. */
bool xd_animation_is_addon(xd_animation_effect effect);
/* What Animations draws in its place when the Add-On doesn't draw it (it's
 * off, or there's no OpenGL): Zoom, as Compiz fell back to its default
 * when an effect's plugin was gone. The others are themselves. */
xd_animation_effect xd_animation_get_fallback(xd_animation_effect effect);

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
 * minimized) or, without one, to its own middle. An Add-On effect is
 * drawn as its fallback. */
xd_animation_frame xd_animation_get_frame(xd_animation_effect effect, xd_effect_rect window,
                                          const xd_effect_rect *target, double presence);

#endif
