/* SPDX-License-Identifier: MIT
 * Window Opacity: Compiz's Opacity, Brightness and Saturation, opacity
 * only (src/compositor/effects/opacity.c): Alt and the wheel over a
 * window make it more or less opaque, windows matching a rule have their
 * own opacity, and inactive windows can be dimmed. The scene graph draws
 * a buffer with an opacity but neither brightness nor saturation, in
 * either compositor, so those two aren't offered.
 */
#include "../effects.h"

#define OPACITY_RULE(number)                                                                   \
    {                                                                                          \
        .key = "Rule" number "Windows",                                                        \
        .label = "Rule " number ": windows",                                                   \
        .description = "Leave empty for no rule, or for example: app_id=org.gnome.Terminal",   \
        .group = "Rules",                                                                      \
        .type = XD_EFFECT_OPTION_MATCH,                                                        \
        .default_value = "",                                                                   \
    },                                                                                         \
    {                                                                                          \
        .key = "Rule" number "Opacity",                                                        \
        .label = "Rule " number ": opacity (percent)",                                         \
        .group = "Rules",                                                                      \
        .type = XD_EFFECT_OPTION_INT,                                                          \
        .min = 1,                                                                              \
        .max = 100,                                                                            \
        .step = 1,                                                                             \
        .default_value = "90",                                                                 \
    }

static const xd_effect_option options[] = {
    {
        .key = "Step",
        .label = "Step (percent)",
        .description = "How much each turn of the wheel changes a window's opacity.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 50,
        .step = 1,
        .default_value = "5",
    },
    {
        .key = "IncreaseButton",
        .label = "More opaque",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_BUTTON,
        .default_value = "<Alt>Button4",
    },
    {
        .key = "DecreaseButton",
        .label = "Less opaque",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_BUTTON,
        .default_value = "<Alt>Button5",
    },
    {
        .key = "DimInactive",
        .label = "Dim inactive windows",
        .group = "Inactive windows",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "InactiveOpacity",
        .label = "Inactive windows' opacity (percent)",
        .group = "Inactive windows",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 100,
        .step = 1,
        .default_value = "75",
    },
    OPACITY_RULE("1"),
    OPACITY_RULE("2"),
    OPACITY_RULE("3"),
};

const xd_effect_info xd_effect_window_opacity = {
    .id = "WindowOpacity",
    .name = "Window Opacity",
    .description = "Makes windows see-through: Alt and the mouse wheel, rules, inactive windows.",
    .icon = "preferences-system-windows-effect-translucency",
    .category = XD_EFFECT_CATEGORY_ACCESSIBILITY,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
