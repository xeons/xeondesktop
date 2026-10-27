/* SPDX-License-Identifier: MIT
 * Scale: Compiz's window picker. The windows glide into a grid of
 * thumbnails; one clicked (or Enter) glides back in front
 * (src/compositor/effects/scale.c). Its options and defaults are Compiz
 * Fusion 0.8's scale plugin's.
 */
#include "../effects.h"

static const char *const scale_output_names[] = {"current", "all", nullptr};
static const char *const scale_output_labels[] = {"The monitor under the pointer",
                                                  "Every monitor", nullptr};

static const xd_effect_option options[] = {
    {
        .key = "Spacing",
        .label = "Spacing",
        .description = "Space between the thumbnails, in pixels.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 0,
        .max = 250,
        .step = 1,
        .default_value = "10",
    },
    {
        .key = "Speed",
        .label = "Speed",
        .description = "How fast the windows glide.",
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
        .description = "How finely the glide is worked out: smaller is smoother.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 50.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "1.2",
    },
    {
        .key = "Opacity",
        .label = "Opacity of the other windows",
        .description = "How opaque the thumbnails not under the pointer are, in percent.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 0,
        .max = 100,
        .step = 1,
        .default_value = "75",
    },
    {
        .key = "DarkenBack",
        .label = "Darken the background",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "Outputs",
        .label = "Windows of",
        .type = XD_EFFECT_OPTION_CHOICE,
        .choice_names = scale_output_names,
        .choice_labels = scale_output_labels,
        .default_value = "current",
    },
    {
        .key = "Windows",
        .label = "Windows to show",
        .description = "Leave empty for every window, or for example: type=normal",
        .type = XD_EFFECT_OPTION_MATCH,
        .default_value = "",
    },
    {
        .key = "InitiateKey",
        .label = "Pick a window",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Shift><Alt>Up",
    },
    {
        .key = "InitiateEdge",
        .label = "Pick a window at",
        .description = "Screen corners or edges the pointer is pushed into.",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_EDGE,
        .default_value = "TopRight",
    },
};

const xd_effect_info xd_effect_scale = {
    .id = "Scale",
    .name = "Scale",
    .description = "Shows every window side by side, to pick one.",
    .icon = "preferences-system-windows-effect-presentwindows",
    .category = XD_EFFECT_CATEGORY_WINDOW_MANAGEMENT,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
