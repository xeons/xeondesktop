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
 * those, with OpenGL), then Burn and Explode (burn.c, explode.c, with
 * OpenGL too), whose options are on their own pages. */
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
    XD_ANIMATION_BURN,
    XD_ANIMATION_EXPLODE,
    XD_ANIMATION_EFFECT_COUNT,
} xd_animation_effect;

/* Animations' events, in the order of its pages' tabs. */
typedef enum xd_animation_event : uint8_t {
    XD_ANIMATION_EVENT_OPEN,
    XD_ANIMATION_EVENT_CLOSE,
    XD_ANIMATION_EVENT_MINIMIZE,
    XD_ANIMATION_EVENT_UNMINIMIZE,
    XD_ANIMATION_EVENT_COUNT,
} xd_animation_event;

/* The effect a name in effects.ini stands for ("glide1", "beamup");
 * None for one it doesn't know. */
xd_animation_effect xd_animation_find_effect(const char *name);
/* Whether the Animations Add-On draws it. */
bool xd_animation_is_addon(xd_animation_effect effect);
/* What Animations draws in its place when the Add-On, Burn or Explode
 * doesn't draw it (its page is off, or there's no OpenGL): Zoom, as
 * Compiz fell back to its default when an effect's plugin was gone. The
 * others are themselves. */
xd_animation_effect xd_animation_get_fallback(xd_animation_effect effect);

/* Whether the event shows the window (opening, restoring): Compiz played
 * an effect's closing backwards for it. */
bool xd_animation_is_appearing(xd_animation_event event);
/* An effect's progress as its closing has it (0 the window whole, 1
 * gone), moved on by `step` (a share of its duration): forwards when
 * it's going, backwards when it's `appearing`; within 0..1. */
double xd_animation_step_progress(double progress, double step, bool appearing);
/* Whether that progress has further to go: an appearing window is whole
 * again at 0, a going one gone at 1. */
bool xd_animation_has_further(double progress, bool appearing);
/* Where it starts: from the window whole when going, gone when
 * appearing. */
double xd_animation_get_start_progress(bool appearing);

/* Who animates an event for Burn or Explode (`own`, the effect itself):
 * its page's own claim on closing windows (`closing_claimed`, Burn every
 * closing window, the windows its page matches: `own_match`), which
 * comes first as it did before these were Animations' choices; else
 * Animations' choice for the event (`chosen`, while Animations is on,
 * for the windows that event matches: `event_match`). */
typedef enum xd_animation_claim : uint8_t {
    XD_ANIMATION_CLAIM_NONE,
    XD_ANIMATION_CLAIM_OWN,   /* the page's: its own duration */
    XD_ANIMATION_CLAIM_EVENT, /* Animations' event's: that event's duration */
} xd_animation_claim;

xd_animation_claim xd_animation_get_claim(xd_animation_effect own, xd_animation_event event,
                                          bool closing_claimed, bool own_match,
                                          xd_animation_effect chosen, bool event_match);

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
