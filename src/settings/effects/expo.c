/* SPDX-License-Identifier: MIT
 * Expo: every desktop at once, side by side as a wall, zoomed out from
 * the current one (src/compositor/effects/expo.c); click one to go there,
 * or drag a window from one desktop to another. Its options are Compiz's
 * expo plugin's, with its zoom time and corner; the key is Win+W, not
 * Compiz's Win+E, which Windows gives to Explorer. Its deformations are
 * Compiz's, Tilt by default; Compiz's reflection (a mirror floor) is left
 * out.
 */
#include "../effects.h"

static const char *const deformation_names[] = {"none", "tilt", "curve", nullptr};
static const char *const deformation_labels[] = {"None", "Tilt", "Curve", nullptr};

static const xd_effect_option options[] = {
    {
        .key = "ExpoKey",
        .label = "Expo key",
        .description = "Shows every desktop, or goes back.",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Super>w",
    },
    {
        .key = "ExpoEdge",
        .label = "Expo edge",
        .description = "Moving the pointer here shows every desktop.",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_EDGE,
        .default_value = "TopLeft",
    },
    {
        .key = "ZoomTime",
        .label = "Zoom time",
        .description = "How long, in seconds, zooming out and in takes.",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 5.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.3",
    },
    {
        .key = "Spacing",
        .label = "Space between desktops",
        .description = "In pixels, between the desktops and round the wall.",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_INT,
        .min = 0,
        .max = 100,
        .step = 1,
        .default_value = "10",
    },
    {
        .key = "Deformation",
        .label = "Deformation",
        .description = "Flat on the screen, leaned back, or curved round you.",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_CHOICE,
        .choice_names = deformation_names,
        .choice_labels = deformation_labels,
        .default_value = "tilt",
    },
    {
        .key = "Curve",
        .label = "Curve strength",
        .description = "How far Curve bends the desktops round you.",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.0,
        .max = 1.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.5",
    },
};

const xd_effect_info xd_effect_expo = {
    .id = "Expo",
    .name = "Expo",
    .description = "See every desktop at once, go to one, or move windows between them.",
    .icon = "preferences-system-windows-effect-desktopgrid",
    .category = XD_EFFECT_CATEGORY_DESKTOP,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
