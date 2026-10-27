/* SPDX-License-Identifier: MIT
 * Ring Switcher: Compiz's ring plugin, another way to switch windows
 * than Alt+Tab (src/compositor/effects/ring.c). The windows' live
 * thumbnails stand on an ellipse that turns to bring the one picked to
 * the front, its title below; let go of the keys to switch to it. Its
 * options and defaults are Compiz Fusion 0.8's.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "Speed",
        .label = "Speed",
        .description = "How fast the ring turns.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 50.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "1.5",
    },
    {
        .key = "Timestep",
        .label = "Timestep",
        .description = "How finely the turning is worked out: smaller is smoother.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 50.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "1.2",
    },
    {
        .key = "Windows",
        .label = "Windows to show",
        .description = "Leave empty for every window, or for example: type=normal",
        .type = XD_EFFECT_OPTION_MATCH,
        .default_value = "",
    },
    {
        .key = "DarkenBack",
        .label = "Darken the background",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "ShowTitle",
        .label = "Show the window's title",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "RingWidth",
        .label = "Ring width (percent of the screen)",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 100,
        .step = 1,
        .default_value = "70",
    },
    {
        .key = "RingHeight",
        .label = "Ring height (percent of the screen)",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 100,
        .step = 1,
        .default_value = "60",
    },
    {
        .key = "ThumbWidth",
        .label = "Thumbnail width",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_INT,
        .min = 10,
        .max = 1024,
        .step = 1,
        .default_value = "350",
    },
    {
        .key = "ThumbHeight",
        .label = "Thumbnail height",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_INT,
        .min = 10,
        .max = 1024,
        .step = 1,
        .default_value = "250",
    },
    {
        .key = "MinScale",
        .label = "Smallest size, at the back",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 1.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.4",
    },
    {
        .key = "MinBrightness",
        .label = "Least brightness, at the back",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 1.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.5",
    },
    {
        .key = "NextKey",
        .label = "Next window",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Super>Tab",
    },
    {
        .key = "PreviousKey",
        .label = "Previous window",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Super><Shift>Tab",
    },
};

const xd_effect_info xd_effect_ring_switcher = {
    .id = "RingSwitcher",
    .name = "Ring Switcher",
    .description = "Switches windows on a turning ring of thumbnails.",
    .icon = "preferences-system-windows-effect-coverswitch",
    .category = XD_EFFECT_CATEGORY_WINDOW_MANAGEMENT,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
