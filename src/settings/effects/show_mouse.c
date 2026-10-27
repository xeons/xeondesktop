/* SPDX-License-Identifier: MIT
 * Show Mouse: Compiz's showmouse plugin, simplified
 * (src/compositor/effects/showmouse.c): a ring of glowing dots turns
 * round the pointer, to find it, while it's switched on with its key.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "Radius",
        .label = "Radius",
        .description = "How far from the pointer the dots turn, in pixels.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 10,
        .max = 300,
        .step = 1,
        .default_value = "40",
    },
    {
        .key = "Dots",
        .label = "Dots",
        .type = XD_EFFECT_OPTION_INT,
        .min = 2,
        .max = 32,
        .step = 1,
        .default_value = "8",
    },
    {
        .key = "RotationSpeed",
        .label = "Turns a second",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.0,
        .max = 5.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.5",
    },
    {
        .key = "Color",
        .label = "Colour",
        .type = XD_EFFECT_OPTION_COLOR,
        .default_value = "#FFDC00C0",
    },
    {
        .key = "ActivateAtStartup",
        .label = "Show it from the start",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "ToggleKey",
        .label = "Show or hide",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Super>k",
    },
};

const xd_effect_info xd_effect_show_mouse = {
    .id = "ShowMouse",
    .name = "Show Mouse",
    .description = "Draws a turning ring round the pointer, to find it.",
    .icon = "preferences-system-windows-effect-trackmouse",
    .category = XD_EFFECT_CATEGORY_ACCESSIBILITY,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
