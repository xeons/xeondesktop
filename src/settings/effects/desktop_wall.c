/* SPDX-License-Identifier: MIT
 * Desktop Wall: switching virtual desktops slides the old one out and the
 * new one in, the way the view moves, with a small map of the desktops
 * showing which one is current (src/compositor/effects/wall.c). Its
 * options are Compiz's wall plugin's, with its defaults for the slide
 * and the map; edge flipping is off by default.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "SlideDuration",
        .label = "Wall sliding duration",
        .description = "How long, in seconds, a switch takes to slide.",
        .group = "Viewport Switching",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.0,
        .max = 5.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.3",
    },
    {
        .key = "ShowSwitcher",
        .label = "Show viewport switcher preview",
        .description = "A small map of the desktops while switching, the new one lit.",
        .group = "Viewport Switching",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "PreviewTimeout",
        .label = "Preview timeout",
        .description = "How long, in seconds, the map stays once the slide is over.",
        .group = "Viewport Switching",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.0,
        .max = 2.0,
        .step = 0.05,
        .digits = 2,
        .default_value = "0.4",
    },
    {
        .key = "EdgeFlipMove",
        .label = "Edge flip move",
        .description = "Dragging a window against the edge of the screen takes it to the next "
                       "desktop that way.",
        .group = "Edge Flipping",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "EdgeFlipDelay",
        .label = "Edge flip delay",
        .description = "How long, in milliseconds, the window has to be held at the edge.",
        .group = "Edge Flipping",
        .type = XD_EFFECT_OPTION_INT,
        .min = 0,
        .max = 5000,
        .step = 50,
        .default_value = "500",
    },
};

const xd_effect_info xd_effect_desktop_wall = {
    .id = "DesktopWall",
    .name = "Desktop Wall",
    .description = "Desktops side by side as a wall: switching slides from one to the next.",
    .icon = "preferences-system-windows-effect-slide",
    .category = XD_EFFECT_CATEGORY_DESKTOP,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
