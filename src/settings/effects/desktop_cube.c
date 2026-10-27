/* SPDX-License-Identifier: MIT
 * Desktop Cube: the desktops across are the faces of a cube, which turns
 * from one to the next when they're switched (src/compositor/effects/
 * cube.c). Its options are Compiz's cube plugin's: the caps' colour, how
 * opaque the cube is while it turns (100: not see-through, Compiz's
 * default), and what's behind it.
 */
#include "../effects.h"

static const char *const background_names[] = {"desktop", "color", nullptr};
static const char *const background_labels[] = {"The desktop colour", "The colour below",
                                                nullptr};

static const xd_effect_option options[] = {
    {
        .key = "CapColor",
        .label = "Cube caps colour",
        .description = "The top and bottom of the cube.",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_COLOR,
        .default_value = "#FEFEFEFF",
    },
    {
        .key = "RotationOpacity",
        .label = "Opacity during rotation",
        .description = "How opaque the cube's faces are while it turns, in percent: below 100 the "
                       "faces behind show through.",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_INT,
        .min = 10,
        .max = 100,
        .step = 5,
        .default_value = "100",
    },
    {
        .key = "Background",
        .label = "Behind the cube",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_CHOICE,
        .choice_names = background_names,
        .choice_labels = background_labels,
        .default_value = "desktop",
    },
    {
        .key = "BackgroundColor",
        .label = "Background colour",
        .group = "Appearance",
        .type = XD_EFFECT_OPTION_COLOR,
        .default_value = "#000000FF",
    },
    {
        .key = "Zoom",
        .label = "Zoom",
        .description = "How far the cube moves away while it turns, in screen heights.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.0,
        .max = 2.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.5",
    },
    {
        .key = "RotationTime",
        .label = "Rotation time",
        .description = "How long, in seconds, the cube takes to turn to the next face.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 3.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.6",
    },
};

const xd_effect_info xd_effect_desktop_cube = {
    .id = "DesktopCube",
    .name = "Desktop Cube",
    .description = "The desktops side by side are the faces of a cube, which turns as you switch "
                   "between them.",
    .icon = "preferences-system-windows-effect-cubeslide",
    .category = XD_EFFECT_CATEGORY_DESKTOP,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
