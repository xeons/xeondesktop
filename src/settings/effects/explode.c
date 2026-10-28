/* SPDX-License-Identifier: MIT
 * Explode: a closing window shatters into pieces that fly out, spin and
 * fade (src/compositor/effects/explode.c). Compiz's Animation plugin's
 * Explode, with its tessellation options. It's also a choice in each of
 * Animations' events, as in CCSM (opening and restoring run backwards:
 * the pieces fly together), drawn while this page is on.
 */
#include "../effects.h"

static const char *const tessellation_names[] = {"Rectangular", "Hexagonal", "Glass", nullptr};
static const char *const tessellation_labels[] = {"Rectangular", "Hexagonal", "Glass", nullptr};

static const xd_effect_option options[] = {
    {
        .key = "Duration",
        .label = "Duration (ms)",
        .description = "How long the pieces of a closing window fly. Animations' events set "
                       "to Explode take their own durations.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 100,
        .max = 10000,
        .step = 10,
        .default_value = "1000",
    },
    {
        .key = "Tessellation",
        .label = "Tessellation type",
        .description = "The shape of the pieces: rectangles, hexagons, or shards of glass "
                       "round the window's middle.",
        .type = XD_EFFECT_OPTION_CHOICE,
        .choice_names = tessellation_names,
        .choice_labels = tessellation_labels,
        .default_value = "Rectangular",
    },
    {
        .key = "PiecesAcross",
        .label = "Pieces across",
        .description = "Rectangular and hexagonal pieces across the window.",
        .group = "Pieces",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 50,
        .step = 1,
        .default_value = "13",
    },
    {
        .key = "PiecesDown",
        .label = "Pieces down",
        .description = "Rectangular and hexagonal pieces down the window.",
        .group = "Pieces",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 50,
        .step = 1,
        .default_value = "10",
    },
    {
        .key = "Spokes",
        .label = "Glass spokes",
        .description = "How many cracks run out from the middle of shattering glass.",
        .group = "Pieces",
        .type = XD_EFFECT_OPTION_INT,
        .min = 3,
        .max = 64,
        .step = 1,
        .default_value = "10",
    },
    {
        .key = "Tiers",
        .label = "Glass tiers",
        .description = "How many rings of shards shattering glass has.",
        .group = "Pieces",
        .type = XD_EFFECT_OPTION_INT,
        .min = 1,
        .max = 32,
        .step = 1,
        .default_value = "4",
    },
    {
        .key = "Thickness",
        .label = "Thickness",
        .description = "How thick the pieces look as they turn.",
        .group = "Pieces",
        .type = XD_EFFECT_OPTION_INT,
        .min = 0,
        .max = 100,
        .step = 1,
        .default_value = "15",
    },
    {
        .key = "Closing",
        .label = "Explode every closing window",
        .description = "The windows below shatter as they close, whatever Animations' Close "
                       "effect is. Turned off, windows explode only on the events Animations "
                       "sets to Explode.",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "Windows",
        .label = "Windows that explode",
        .description = "The closing windows that explode. Leave empty for every window, or "
                       "for example: type=normal",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_MATCH,
        .default_value = "",
    },
};

const xd_effect_info xd_effect_explode = {
    .id = "Explode",
    .name = "Explode",
    .description = "Closing windows shatter into pieces that fly apart; in Animations, "
                   "windows can explode as they open and minimize too.",
    .icon = "preferences-system-windows-effect-fallapart",
    .category = XD_EFFECT_CATEGORY_EFFECTS,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
