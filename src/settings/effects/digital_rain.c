/* SPDX-License-Identifier: MIT
 * Digital Rain: windows build out of falling glyphs as they open and
 * come back, and break up into them as they close and minimize
 * (src/compositor/effects/rain.c). A Linux addition after the screen
 * saver of that name, with its characters and colours; no Windows or
 * Compiz original. It has no events of its own: it's one of Animations'
 * choices for each event, drawn while this page is on, so an
 * effects.ini from before it changes nothing.
 */
#include "../effects.h"

/* The screen saver's sets and colours, by the same names. */
static const char *const character_names[] = {"katakana", "digits", "binary", "letters", nullptr};
static const char *const character_labels[] = {"Katakana and digits", "Digits only",
                                               "Binary (0 and 1)", "Letters and digits", nullptr};
static const char *const color_names[] = {"green",  "amber",  "blue", "white",
                                          "custom", "window", nullptr};
static const char *const color_labels[] = {"Green",  "Amber",           "Blue", "White",
                                           "Custom", "Window's colors", nullptr};

static const xd_effect_option options[] = {
    {
        .key = "Duration",
        .label = "Duration (ms)",
        .description = "How long a window takes to build or break up. Animations' own "
                       "durations don't apply: the rain needs longer to be seen.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 200,
        .max = 5000,
        .step = 10,
        .default_value = "1200",
    },
    {
        .key = "GlyphSize",
        .label = "Glyph size (pixels)",
        .description = "How high a glyph is, at 100% scaling; larger on monitors scaled up.",
        .type = XD_EFFECT_OPTION_INT,
        .min = 8,
        .max = 32,
        .step = 1,
        .default_value = "14",
    },
    {
        .key = "Characters",
        .label = "Characters",
        .description = "Without a font that has katakana, letters and digits are drawn.",
        .type = XD_EFFECT_OPTION_CHOICE,
        .choice_names = character_names,
        .choice_labels = character_labels,
        .default_value = "katakana",
    },
    {
        .key = "Color",
        .label = "Color",
        .description = "Window's colors: each glyph takes the color of the part of the window "
                       "it replaces.",
        .group = "Color",
        .type = XD_EFFECT_OPTION_CHOICE,
        .choice_names = color_names,
        .choice_labels = color_labels,
        .default_value = "green",
    },
    {
        .key = "CustomColor",
        .label = "Custom color",
        .description = "The glyphs' color when Color is Custom.",
        .group = "Color",
        .type = XD_EFFECT_OPTION_COLOR,
        .default_value = "#00FF46FF",
    },
    {
        .key = "Glow",
        .label = "Glow behind the leading glyphs",
        .group = "Color",
        .type = XD_EFFECT_OPTION_BOOL,
        .default_value = "true",
    },
    {
        .key = "Windows",
        .label = "Windows",
        .description = "Of the windows Animations sets to Digital Rain, those that rain. Leave "
                       "empty for every one, or for example: type=normal",
        .group = "Windows",
        .type = XD_EFFECT_OPTION_MATCH,
        .default_value = "",
    },
};

const xd_effect_info xd_effect_digital_rain = {
    .id = "DigitalRain",
    .name = "Digital Rain",
    .description = "In Animations, windows build out of falling glyphs as they open, and break "
                   "up into them as they close or minimize.",
    .icon = "preferences-desktop-screensaver",
    .category = XD_EFFECT_CATEGORY_EFFECTS,
    .needs_opengl = true,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
