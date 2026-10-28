/* SPDX-License-Identifier: MIT
 * Burn: a closing window burns away, a flame front crossing it and
 * giving off sparks and, if asked, smoke (src/compositor/effects/burn.c).
 * Compiz's Burn, with its fire and smoke options. It's also a choice in
 * each of Animations' events, as in CCSM (opening and restoring burn backwards:
 * the window comes out of the fire), drawn while this page is on.
 */
#include "../effects.h"

static const char *const direction_names[] = {"Down", "Up", "Left", "Right", "Random", nullptr};
static const char *const direction_labels[] = {"Down", "Up", "Left", "Right", "Random", nullptr};

static const xd_effect_option options[] = {
    {
        .key = "Duration",
        .label = "Duration (ms)",
        .description = "How long a closing window takes to burn away. Animations' events "
                       "set to Burn take their own durations.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 100,
        .max = 10000,
        .step = 10,
        .default_value = "1000",
    },
    {
        .key = "Direction",
        .label = "Direction",
        .description = "The way the flames move across the window.",
        .type = XD_EFFECT_OPTION_CHOICE,
        .choice_names = direction_names,
        .choice_labels = direction_labels,
        .default_value = "Down",
    },
    {
        .key = "FireColor",
        .label = "Fire color",
        .group = "Fire",
        .type = XD_EFFECT_OPTION_COLOR,
        .default_value = "#FF3305FF",
    },
    {
        .key = "MysticalFire",
        .label = "Mystical fire",
        .description = "Each spark a colour of its own, picked at random.",
        .group = "Fire",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "ParticleCount",
        .label = "Number of fire particles",
        .description = "Fewer are faster.",
        .group = "Fire",
        .type = XD_EFFECT_OPTION_INT,
        .min = 100,
        .max = 5000,
        .step = 100,
        .default_value = "1000",
    },
    {
        .key = "ParticleSize",
        .label = "Fire particle size",
        .group = "Fire",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 20.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "5.0",
    },
    {
        .key = "ParticleLife",
        .label = "Fire particle life",
        .description = "How long a spark lasts, in seconds.",
        .group = "Fire",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 1.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "0.7",
    },
    {
        .key = "Slowdown",
        .label = "Fire particle slowdown",
        .description = "How quickly sparks lose the speed they start with.",
        .group = "Fire",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 0.1,
        .max = 10.0,
        .step = 0.1,
        .digits = 1,
        .default_value = "0.5",
    },
    {
        .key = "Smoke",
        .label = "Smoke",
        .description = "Grey smoke rises from the flames and drifts away. Windows burning "
                       "backwards, as they open or are restored, give off none.",
        .group = "Smoke",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "false",
    },
    {
        .key = "SmokeCount",
        .label = "Number of smoke particles",
        .description = "How thick the smoke is. Fewer are faster.",
        .group = "Smoke",
        .type = XD_EFFECT_OPTION_INT,
        .min = 10,
        .max = 1000,
        .step = 10,
        .default_value = "100",
    },
    {
        .key = "SmokeSize",
        .label = "Smoke particle size",
        .description = "How big a puff of smoke is when it leaves the flames; it swells "
                       "to three times that. Puffs last as long as sparks do.",
        .group = "Smoke",
        .type = XD_EFFECT_OPTION_DOUBLE,
        .min = 1.0,
        .max = 20.0,
        .step = 0.5,
        .digits = 1,
        .default_value = "10.0",
    },
    {
        .key = "Closing",
        .label = "Burn every closing window",
        .description = "The windows below burn as they close, whatever Animations' Close "
                       "effect is. Turned off, windows burn only on the events Animations "
                       "sets to Burn.",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "Windows",
        .label = "Windows that burn",
        .description = "The closing windows that burn. Leave empty for every window, or for "
                       "example: type=normal",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_MATCH,
        .default_value = "",
    },
};

const xd_effect_info xd_effect_burn = {
    .id = "Burn",
    .name = "Burn",
    .description = "Closing windows burn away; in Animations, windows can burn as they open "
                   "and minimize too.",
    /* No fire in the icon theme: a closing window's. */
    .icon = "preferences-system-windows-effect-fade",
    .category = XD_EFFECT_CATEGORY_EFFECTS,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
