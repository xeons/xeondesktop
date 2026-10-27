/* SPDX-License-Identifier: MIT
 * Water Effect: the whole screen as the surface of water, rippling where
 * you click, in the rain, or behind the pointer
 * (src/compositor/effects/water.c). Compiz's Water Effect plugin, with
 * its options and its Shift+F9 for rain; its ripple binding is ours.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "OffsetScale",
        .label = "Offset scale",
        .description = "How strongly the ripples bend what's under them.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.0,
        .max = 10.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "1.0",
    },
    {
        .key = "RainDelay",
        .label = "Rain delay (ms)",
        .description = "The time between raindrops while it's raining.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 3600000,
        .step = 10,
        .default_value = "250",
    },
    {
        .key = "TitleWave",
        .label = "Title wave",
        .description = "A wave runs along a window's title bar when its title changes.",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "PointerWake",
        .label = "Leave a wake behind the pointer while Ctrl and the Windows key are held",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "Ripple",
        .label = "Ripple at the pointer",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_BUTTON,
        .default_value = "<Super><Shift>Button1",
    },
    {
        .key = "ToggleRain",
        .label = "Toggle rain",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Shift>F9",
    },
};

const xd_effect_info xd_effect_water = {
    .id = "Water",
    .name = "Water Effect",
    .description = "The screen ripples like the surface of water where you click, or in "
                   "the rain.",
    .icon = "weather-showers",
    .category = XD_EFFECT_CATEGORY_EFFECTS,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
