/* SPDX-License-Identifier: MIT
 * General Options: CCSM's settings that aren't an effect, here how many
 * desktops there are (Compiz's Desktop Size: horizontal and vertical
 * virtual size) and the keys that switch between them and carry the
 * active window along (Desktop Wall's in Compiz; here they work with no
 * effect on). Always in use; one desktop by default, as Windows has.
 * src/compositor/desktops.c is the compositor's side.
 */
#include "../effects.h"

static const xd_effect_option options[] = {
    {
        .key = "HorizontalSize",
        .label = "Desktops across",
        .description = "How many desktops there are side by side.",
        .group = "Desktop Size",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 16,
        .step = 1,
        .default_value = "1",
    },
    {
        .key = "VerticalSize",
        .label = "Desktops down",
        .description = "How many rows of desktops there are.",
        .group = "Desktop Size",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 16,
        .step = 1,
        .default_value = "1",
    },
    {
        .key = "WrapAround",
        .label = "Wrap around at the edges",
        .description = "Going past the last desktop comes back to the first.",
        .group = "Desktop Size",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "SwitchLeft",
        .label = "Switch to the desktop on the left",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Control><Alt>Left",
    },
    {
        .key = "SwitchRight",
        .label = "Switch to the desktop on the right",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Control><Alt>Right",
    },
    {
        .key = "SwitchUp",
        .label = "Switch to the desktop above",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Control><Alt>Up",
    },
    {
        .key = "SwitchDown",
        .label = "Switch to the desktop below",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Control><Alt>Down",
    },
    {
        .key = "MoveLeft",
        .label = "Move the window to the desktop on the left",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Control><Alt><Shift>Left",
    },
    {
        .key = "MoveRight",
        .label = "Move the window to the desktop on the right",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Control><Alt><Shift>Right",
    },
    {
        .key = "MoveUp",
        .label = "Move the window to the desktop above",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Control><Alt><Shift>Up",
    },
    {
        .key = "MoveDown",
        .label = "Move the window to the desktop below",
        .group = "Bindings",
        .type = XD_EFFECT_OPTION_KEY,
        .default_value = "<Control><Alt><Shift>Down",
    },
};

const xd_effect_info xd_effect_general_options = {
    .id = "GeneralOptions",
    .name = "General Options",
    .description = "How many desktops there are, and the keys that switch between them.",
    .icon = "preferences-desktop-virtual",
    .category = XD_EFFECT_CATEGORY_DESKTOP,
    .always_on = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
