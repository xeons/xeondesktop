/* SPDX-License-Identifier: MIT
 * Animations: windows fade, zoom or glide as they open, close, minimize
 * and come back (src/compositor/effects/animations.c), Compiz's
 * animation plugin's events with an effect, a duration and the windows
 * of each. Turned on, it takes over XP's minimize animation for the
 * windows it animates.
 */
#include "../effects.h"

static const char *const animation_names[] = {"none",   "fade",   "zoom",
                                              "glide1", "glide2", nullptr};
static const char *const animation_labels[] = {"None",    "Fade",    "Zoom",
                                               "Glide 1", "Glide 2", nullptr};

#define ANIMATION_EVENT(prefix, tab, effect, duration)                                         \
    {                                                                                          \
        .key = prefix "Effect",                                                                \
        .label = "Effect",                                                                     \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_CHOICE,                                                       \
        .choice_names = animation_names,                                                       \
        .choice_labels = animation_labels,                                                     \
        .default_value = effect,                                                               \
    },                                                                                         \
    {                                                                                          \
        .key = prefix "Duration",                                                              \
        .label = "Duration (milliseconds)",                                                    \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_INT,                                                          \
        .min = 50,                                                                             \
        .max = 4000,                                                                           \
        .step = 10,                                                                            \
        .default_value = duration,                                                             \
    },                                                                                         \
    {                                                                                          \
        .key = prefix "Windows",                                                               \
        .label = "Windows",                                                                    \
        .description = "Leave empty for every window, or for example: type=normal",            \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_MATCH,                                                        \
        .default_value = "",                                                                   \
    }

static const xd_effect_option options[] = {
    ANIMATION_EVENT("Open", "Open", "zoom", "200"),
    ANIMATION_EVENT("Close", "Close", "zoom", "200"),
    ANIMATION_EVENT("Minimize", "Minimize", "zoom", "300"),
    ANIMATION_EVENT("Unminimize", "Unminimize", "zoom", "300"),
};

const xd_effect_info xd_effect_animations = {
    .id = "Animations",
    .name = "Animations",
    .description = "Windows fade, zoom or glide as they open, close and minimize.",
    .icon = "preferences-system-windows-effect-glide",
    .category = XD_EFFECT_CATEGORY_EFFECTS,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
