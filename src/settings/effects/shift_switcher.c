/* SPDX-License-Identifier: MIT
 * Shift Switcher: the desktop's windows as live pictures in 3D, flicked
 * through as a cover flow (or a stack flipping over), the one chosen
 * activated when the keys are let go (src/compositor/effects/shift.c).
 * Its options are Compiz's shift plugin's, with its defaults for the
 * mode, size, background and reflection. Compiz's Super+Tab is left to
 * Ring Switcher: Super+Shift+Tab starts it here.
 */
#include "../effects.h"

static const char *const mode_names[] = {"cover", "flip", nullptr};
static const char *const mode_labels[] = {"Cover", "Flip", nullptr};

static const xd_effect_option options[] = {
    {
        .key = "Initiate",
        .label = "Initiate",
        .description = "Shows the windows; again, the next one. Let go of the keys to switch to "
                       "the one in front.",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Super><Shift>Tab",
    },
    {
        .key = "Mode",
        .label = "Switcher mode",
        .type = XD_EFFECT_OPTION_CHOICE,
        .choice_names = mode_names,
        .choice_labels = mode_labels,
        .default_value = "cover",
    },
    {
        .key = "Size",
        .label = "Maximum window size",
        .description = "How big the window in front may be, in percent of the screen.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 10,
        .max = 100,
        .step = 5,
        .default_value = "50",
    },
    {
        .key = "BackgroundBrightness",
        .label = "Background brightness",
        .description = "How bright the desktop behind stays, in percent.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 0,
        .max = 100,
        .step = 5,
        .default_value = "50",
    },
    {
        .key = "Reflection",
        .label = "Show reflection",
        .description = "Each window mirrored on the floor below it.",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "AnimationTime",
        .label = "Animation time",
        .description = "How long, in seconds, the windows take to move to their places.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.05,
        .max = 2.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.25",
    },
};

const xd_effect_info xd_effect_shift_switcher = {
    .id = "ShiftSwitcher",
    .name = "Shift Switcher",
    .description = "Flick through the windows as pictures in 3D, and switch to one.",
    .icon = "preferences-system-windows-effect-coverswitch",
    .category = XD_EFFECT_CATEGORY_WINDOW_MANAGEMENT,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
