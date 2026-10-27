/* SPDX-License-Identifier: MIT
 * Magic Lamp: a window being minimized is sucked into its taskbar button
 * like a genie into its lamp, and comes out of it when restored
 * (src/compositor/effects/magic_lamp.c). Compiz's Animation plugin's
 * Magic Lamp, with its options for the effect's length and grid.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "Duration",
        .label = "Duration (ms)",
        .description = "How long minimizing or restoring a window takes.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 50,
        .max = 4000,
        .step = 10,
        .default_value = "300",
    },
    {
        .key = "GridResolution",
        .label = "Grid resolution",
        .description = "How many strips the window bends in: more is smoother, fewer is "
                       "faster.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 4,
        .max = 200,
        .step = 1,
        .default_value = "100",
    },
    {
        .key = "OpenClose",
        .label = "Open and close windows with it too",
        .description = "Windows come out of their taskbar button (or the pointer) when they "
                       "open, and go into it when they close.",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "Windows",
        .label = "Windows that use it",
        .description = "Leave empty for every window, or for example: type=normal",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_MATCH,
        .default_value = "",
    },
};

const xd_effect_info xd_effect_magic_lamp = {
    .id = "MagicLamp",
    .name = "Magic Lamp",
    .description = "Minimized windows are sucked into their taskbar button like a genie into "
                   "its lamp.",
    .icon = "preferences-system-windows-effect-magiclamp",
    .category = XD_EFFECT_CATEGORY_EFFECTS,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
