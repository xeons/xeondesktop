/* SPDX-License-Identifier: MIT
 * Rotate Cube: Desktop Cube turned by hand, dragging with Ctrl+Alt and the
 * left button held (Compiz's rotate plugin's Initiate), the cube zoomed
 * out while it turns (Desktop Cube's Zoom); let go, it settles on the face
 * nearest the front and that desktop shows (src/compositor/effects/
 * cube.c). Needs Desktop Cube on.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "Initiate",
        .label = "Initiate",
        .description = "Hold to turn the cube by dragging.",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_BUTTON,
        .default_value = "<Control><Alt>Button1",
    },
    {
        .key = "Sensitivity",
        .label = "Sensitivity",
        .description = "How far the cube turns for the pointer's movement.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 10.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "1.0",
    },
};

const xd_effect_info xd_effect_rotate_cube = {
    .id = "RotateCube",
    .name = "Rotate Cube",
    .description = "Turn Desktop Cube by dragging with Ctrl+Alt held.",
    .icon = "object-rotate-left",
    .category = XD_EFFECT_CATEGORY_DESKTOP,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
