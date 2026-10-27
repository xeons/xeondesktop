/* SPDX-License-Identifier: MIT
 * Enhanced Zoom Desktop: Compiz's ezoom plugin
 * (src/compositor/effects/zoom.c). The Windows key and the wheel zoom
 * the whole screen in and out around the pointer, smoothly, and the
 * view follows the pointer.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "ZoomFactor",
        .label = "Zoom factor",
        .description = "How much each turn of the wheel zooms in or out.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 1.01,
        .max = 3.0,
        .step = 0.01,
        .digits = 2,
        .default_value = "1.5",
    },
    {
        .key = "MaximumZoom",
        .label = "Most magnification",
        .type = XD_EFFECT_OPTION_INT,
        .min = 2,
        .max = 32,
        .step = 1,
        .default_value = "16",
    },
    {
        .key = "Speed",
        .label = "Speed",
        .description = "How quickly the zoom follows the wheel.",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 100.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "25.0",
    },
    {
        .key = "ZoomInButton",
        .label = "Zoom in",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_BUTTON,
        .default_value = "<Super>Button4",
    },
    {
        .key = "ZoomOutButton",
        .label = "Zoom out",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_BUTTON,
        .default_value = "<Super>Button5",
    },
};

const xd_effect_info xd_effect_enhanced_zoom = {
    .id = "EnhancedZoom",
    .name = "Enhanced Zoom Desktop",
    .description = "Zooms the screen in around the pointer with the Windows key and the wheel.",
    .icon = "preferences-system-windows-effect-zoom",
    .category = XD_EFFECT_CATEGORY_ACCESSIBILITY,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
