/* SPDX-License-Identifier: MIT
 * Animations' maths (animations_model.h).
 */
#include "animations_model.h"

#include "../../core/common.h"

/* Zoom fades out over the last part of its way only. */
static constexpr double ZOOM_FADE_SHARE = 0.3;

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
    switch (effect) {
    case XD_ANIMATION_NONE:
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
