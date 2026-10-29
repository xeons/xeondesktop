/* SPDX-License-Identifier: MIT
 * Animations' maths (animations_model.h).
 */
#include "animations_model.h"

#include <string.h>

#include "../../core/common.h"

/* Zoom fades out over the last part of its way only. */
static constexpr double ZOOM_FADE_SHARE = 0.3;

/* effects.ini's names, by effect. */
static const char *const effect_names[XD_ANIMATION_EFFECT_COUNT] = {
    "none",   "fade", "zoom",   "glide1",     "glide2", "airplane", "beamup",
    "domino", "fold", "glide3", "leafspread", "razr",   "skewer", "burn", "explode",
    "digitalrain",
};

xd_animation_effect xd_animation_find_effect(const char *name) {
    for (size_t i = 0; name != nullptr && i < XD_ARRAY_LEN(effect_names); i++) {
        if (strcmp(effect_names[i], name) == 0)
            return (xd_animation_effect)i;
    }
    return XD_ANIMATION_NONE;
}

bool xd_animation_is_addon(xd_animation_effect effect) {
    return effect >= XD_ANIMATION_AIRPLANE && effect <= XD_ANIMATION_SKEWER;
}

xd_animation_effect xd_animation_get_fallback(xd_animation_effect effect) {
    return effect >= XD_ANIMATION_AIRPLANE ? XD_ANIMATION_ZOOM : effect;
}

bool xd_animation_is_appearing(xd_animation_event event) {
    return event == XD_ANIMATION_EVENT_OPEN || event == XD_ANIMATION_EVENT_UNMINIMIZE;
}

double xd_animation_step_progress(double progress, double step, bool appearing) {
    return XD_CLAMP(appearing ? progress - step : progress + step, 0.0, 1.0);
}

bool xd_animation_has_further(double progress, bool appearing) {
    return appearing ? progress > 0 : progress < 1;
}

double xd_animation_get_start_progress(bool appearing) {
    return appearing ? 1.0 : 0.0;
}

xd_animation_claim xd_animation_get_claim(xd_animation_effect own, xd_animation_event event,
                                          bool closing_claimed, bool own_match,
                                          xd_animation_effect chosen, bool event_match) {
    if (event == XD_ANIMATION_EVENT_CLOSE && closing_claimed && own_match)
        return XD_ANIMATION_CLAIM_OWN;
    if (event < XD_ANIMATION_EVENT_COUNT && chosen == own && event_match)
        return XD_ANIMATION_CLAIM_EVENT;
    return XD_ANIMATION_CLAIM_NONE;
}

double xd_animation_get_presence(bool appearing, double progress) {
    double time = XD_CLAMP(progress, 0.0, 1.0);
    /* Compiz's decelerating curve: quick at first, settling at the end. */
    if (appearing)
        return 1.0 - (1.0 - time) * (1.0 - time);
    return 1.0 - time * time;
}

/* The window squashed to scale_x by scale_y around the point `anchor_x`,
 * `anchor_y` of its box (0 to 1 across and down), moved down by `drop`. */
static xd_animation_frame animation_squash(xd_effect_rect window, double scale_x, double scale_y,
                                           double anchor_x, double anchor_y, double drop) {
    return (xd_animation_frame){
        .x = window.x + window.width * anchor_x * (1.0 - scale_x),
        .y = window.y + window.height * anchor_y * (1.0 - scale_y) + drop,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .opacity = 1.0,
    };
}

xd_animation_frame xd_animation_get_frame(xd_animation_effect effect, xd_effect_rect window,
                                          const xd_effect_rect *target, double presence) {
    double shown = XD_CLAMP(presence, 0.0, 1.0);
    double gone = 1.0 - shown;
    xd_animation_frame frame = {window.x, window.y, 1.0, 1.0, 1.0};
    switch (xd_animation_get_fallback(effect)) {
    case XD_ANIMATION_NONE:
    case XD_ANIMATION_AIRPLANE:
    case XD_ANIMATION_BEAM_UP:
    case XD_ANIMATION_DOMINO:
    case XD_ANIMATION_FOLD:
    case XD_ANIMATION_GLIDE_3:
    case XD_ANIMATION_LEAF_SPREAD:
    case XD_ANIMATION_RAZR:
    case XD_ANIMATION_SKEWER:
    case XD_ANIMATION_BURN:
    case XD_ANIMATION_EXPLODE:
    case XD_ANIMATION_DIGITAL_RAIN:
    case XD_ANIMATION_EFFECT_COUNT:
        return frame;
    case XD_ANIMATION_FADE:
        frame.opacity = shown;
        return frame;
    case XD_ANIMATION_ZOOM: {
        xd_effect_rect to = target != nullptr
                                ? *target
                                : (xd_effect_rect){window.x + window.width / 2,
                                                   window.y + window.height / 2, 0, 0};
        double width = to.width + (window.width - to.width) * shown;
        double height = to.height + (window.height - to.height) * shown;
        frame.x = to.x + (window.x - to.x) * shown;
        frame.y = to.y + (window.y - to.y) * shown;
        frame.scale_x = window.width > 0 ? width / window.width : 0;
        frame.scale_y = window.height > 0 ? height / window.height : 0;
        frame.opacity = XD_MIN(shown / ZOOM_FADE_SHARE, 1.0);
        return frame;
    }
    case XD_ANIMATION_GLIDE_1:
        /* Tipped back and away from its bottom edge, rising a little. */
        frame = animation_squash(window, 1.0 - 0.3 * gone, 1.0 - 0.6 * gone, 0.5, 1.0,
                                 -0.25 * window.height * gone);
        frame.opacity = shown;
        return frame;
    case XD_ANIMATION_GLIDE_2:
        /* Tipped forward from its top edge, falling away. */
        frame = animation_squash(window, 1.0 - 0.5 * gone, 1.0 - 0.8 * gone, 0.5, 0.0,
                                 0.3 * window.height * gone);
        frame.opacity = shown;
        return frame;
    }
    return frame;
}
