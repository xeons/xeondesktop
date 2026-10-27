/* SPDX-License-Identifier: MIT
 * Annotate: Compiz's annotate plugin (src/compositor/effects/annotate.c).
 * Draw on the screen, above every window, with the Windows key, Alt and
 * the left button; rub out with the right one; clear it all with
 * Windows+Alt+K. The windows keep the keyboard. Its bindings and colours
 * are Compiz Fusion 0.8's.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "FillColor",
        .label = "Colour",
        .description = "The colour lines are drawn in.",
        .type = XD_EFFECT_OPTION_COLOR,
        .default_value = "#FF0000FF",
    },
    {
        .key = "StrokeColor",
        .label = "Outline colour",
        .description = "The colour round the lines.",
        .type = XD_EFFECT_OPTION_COLOR,
        .default_value = "#00FF00FF",
    },
    {
        .key = "LineWidth",
        .label = "Line width",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 100.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "3.0",
    },
    {
        .key = "StrokeWidth",
        .label = "Outline width",
        .description = "0 draws the lines without an outline.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.0,
        .max = 20.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "1.0",
    },
    {
        .key = "EraseWidth",
        .label = "Eraser width",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 1.0,
        .max = 100.0,
        .step = 1.0,
        .digits = 0,
        .default_value = "20",
    },
    {
        .key = "DrawButton",
        .label = "Draw",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_BUTTON,
        .default_value = "<Super><Alt>Button1",
    },
    {
        .key = "EraseButton",
        .label = "Erase",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_BUTTON,
        .default_value = "<Super><Alt>Button3",
    },
    {
        .key = "ClearKey",
        .label = "Clear",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Super><Alt>k",
    },
};

const xd_effect_info xd_effect_annotate = {
    .id = "Annotate",
    .name = "Annotate",
    .description = "Draws on the screen with the mouse, above every window.",
    .icon = "preferences-system-windows-effect-mousemark",
    .category = XD_EFFECT_CATEGORY_UTILITY,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
