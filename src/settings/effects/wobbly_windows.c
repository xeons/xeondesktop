/* SPDX-License-Identifier: MIT
 * Wobbly Windows: a window is a sheet of springs, which lags behind and
 * wobbles when it's dragged, and shivers when it snaps to a new size
 * (src/compositor/effects/wobbly.c). Its options are Compiz's wobbly
 * plugin's, with its defaults for the spring constant and friction.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "SpringConstant",
        .label = "Spring constant",
        .description = "How stiff the window is: higher springs back faster.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 10.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "8.0",
    },
    {
        .key = "Friction",
        .label = "Friction",
        .description = "How quickly the wobbling dies down.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 10.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "3.0",
    },
    {
        .key = "GridResolution",
        .label = "Grid resolution",
        .description = "How many pieces each side of a window bends in: more is smoother, "
                       "fewer is faster.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 2,
        .max = 64,
        .step = 1,
        .default_value = "16",
    },
    {
        .key = "WobbleOnMove",
        .label = "Wobble when moved",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "WobbleOnMaximize",
        .label = "Wobble when maximized, restored or snapped",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "WobbleOnMap",
        .label = "Wobble when opened",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "WobbleOnFocus",
        .label = "Wobble when activated",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "Windows",
        .label = "Windows that wobble",
        .description = "Leave empty for every window, or for example: type=normal & "
                       "!app_id=org.mozilla.firefox",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_MATCH,
        .default_value = "",
    },
};

const xd_effect_info xd_effect_wobbly_windows = {
    .id = "WobblyWindows",
    .name = "Wobbly Windows",
    .description = "Windows bend and wobble like jelly as you drag them.",
    .icon = "preferences-system-windows-effect-wobblywindows",
    .category = XD_EFFECT_CATEGORY_EFFECTS,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
