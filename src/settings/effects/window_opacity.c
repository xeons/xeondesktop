/* SPDX-License-Identifier: MIT
 * Window Opacity: Compiz's Opacity, Brightness and Saturation
 * (src/compositor/effects/opacity.c). Each of the three has its tab, as
 * CCSM's page had: a step, a binding to raise it and one to lower it over
 * a window (Alt and the wheel for opacity), and rules giving matching
 * windows their own value; inactive windows can be dimmed besides.
 *
 * The scene graph draws a buffer with an opacity but neither brightness
 * nor saturation, so those two are drawn with OpenGL by xd-compositor-fx
 * (glfx.h): their options need it (needs_opengl), and Desktop Effects
 * greys them out without it. Opacity works in either compositor.
 */
#include "../effects.h"

#define PAINT_RULE(prefix, number, value_key, tab, noun, lowest, value, opengl)                \
    {                                                                                          \
        .key = prefix "Rule" number "Windows",                                                 \
        .label = "Rule " number ": windows",                                                   \
        .description = "Leave empty for no rule, or for example: app_id=org.gnome.Terminal",   \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_MATCH,                                                        \
        .needs_opengl = opengl,                                                                \
        .default_value = "",                                                                   \
    },                                                                                         \
    {                                                                                          \
        .key = prefix "Rule" number value_key,                                                 \
        .label = "Rule " number ": " noun " (percent)",                                        \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_INT,                                                          \
        .needs_opengl = opengl,                                                                \
        .min = lowest,                                                                         \
        .max = 100,                                                                            \
        .step = 1,                                                                             \
        .default_value = value,                                                                \
    }

/* The opacity rules' keys are the older "Rule1Windows" and "Rule1Opacity". */
#define OPACITY_RULE(number) PAINT_RULE("", number, "Opacity", "Opacity", "opacity", 1, "90", false)
#define BRIGHTNESS_RULE(number)                                                                \
    PAINT_RULE("Brightness", number, "Value", "Brightness", "brightness", 0, "75", true)
#define SATURATION_RULE(number)                                                                \
    PAINT_RULE("Saturation", number, "Value", "Saturation", "saturation", 0, "50", true)

#define PAINT_STEP(key_name, tab, noun, opengl)                                                \
    {                                                                                          \
        .key = key_name,                                                                       \
        .label = "Step (percent)",                                                             \
        .description = "How much each turn of the wheel changes a window's " noun ".",         \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_INT,                                                          \
        .needs_opengl = opengl,                                                                \
        .min = 1,                                                                              \
        .max = 50,                                                                             \
        .step = 1,                                                                             \
        .default_value = "5",                                                                  \
    }

#define PAINT_BUTTON(key_name, button_label, tab, button, opengl)                              \
    {                                                                                          \
        .key = key_name,                                                                       \
        .label = button_label,                                                                 \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_BUTTON,                                                       \
        .needs_opengl = opengl,                                                                \
        .default_value = button,                                                               \
    }

static const xd_effect_option options[] = {
    PAINT_STEP("Step", "Opacity", "opacity", false),
    PAINT_BUTTON("IncreaseButton", "More opaque", "Opacity", "<Alt>Button4", false),
    PAINT_BUTTON("DecreaseButton", "Less opaque", "Opacity", "<Alt>Button5", false),
    OPACITY_RULE("1"),
    OPACITY_RULE("2"),
    OPACITY_RULE("3"),
    PAINT_STEP("BrightnessStep", "Brightness", "brightness", true),
    PAINT_BUTTON("BrightnessIncreaseButton", "Brighter", "Brightness", "<Shift><Alt>Button4",
                 true),
    PAINT_BUTTON("BrightnessDecreaseButton", "Darker", "Brightness", "<Shift><Alt>Button5",
                 true),
    BRIGHTNESS_RULE("1"),
    BRIGHTNESS_RULE("2"),
    BRIGHTNESS_RULE("3"),
    PAINT_STEP("SaturationStep", "Saturation", "saturation", true),
    PAINT_BUTTON("SaturationIncreaseButton", "More saturated", "Saturation",
                 "<Control><Alt>Button4", true),
    PAINT_BUTTON("SaturationDecreaseButton", "Less saturated", "Saturation",
                 "<Control><Alt>Button5", true),
    SATURATION_RULE("1"),
    SATURATION_RULE("2"),
    SATURATION_RULE("3"),
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
    {
        .key = "InactiveBrightness",
        .label = "Inactive windows' brightness (percent)",
        .description = "100 leaves inactive windows as bright as they are.",
        .group = "Inactive windows",
        .type = XD_EFFECT_OPTION_INT,
        .needs_opengl = true,
        .min = 0,
        .max = 100,
        .step = 1,
        .default_value = "100",
    },
};

const xd_effect_info xd_effect_window_opacity = {
    .id = "WindowOpacity",
    .name = "Window Opacity",
    .description = "Changes how see-through, bright and saturated windows are: the mouse "
                   "wheel, rules, inactive windows.",
    .icon = "preferences-system-windows-effect-translucency",
    .category = XD_EFFECT_CATEGORY_ACCESSIBILITY,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
