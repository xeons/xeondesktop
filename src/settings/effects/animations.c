/* SPDX-License-Identifier: MIT
 * Animations: windows fade, zoom or glide as they open, close, minimize
 * and come back (src/compositor/effects/animations.c), Compiz's
 * animation plugin's events with an effect, a duration and the windows
 * of each. Turned on, it takes over XP's minimize animation for the
 * windows it animates. The Animations Add-On's effects are choices here
 * too; they're drawn while that page is on (OpenGL), Zoom otherwise.
 */
#include "../effects.h"

/* Compiz's own effects, then the Animations Add-On's (animationaddon's,
 * as CCSM listed them once that plugin was on), each event offering
 * those Compiz offered for it: Airplane only for closing and minimizing
 * (and so restoring, minimizing's played back). */
#define ADDON "AnimationsAddOn"

static const char *const open_names[] = {
    "none",   "fade",  "zoom", "glide1", "glide2",     "beamup", "domino",
    "fold",   "glide3", "leafspread", "razr", "skewer", nullptr};
static const char *const open_labels[] = {
    "None",    "Fade",   "Zoom", "Glide 1", "Glide 2",     "Beam Up", "Domino",
    "Fold",    "Glide 3", "Leaf Spread", "Razr", "Skewer", nullptr};
static const char *const open_requires[] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, ADDON, ADDON,
    ADDON,   ADDON,   ADDON,   ADDON,   ADDON,   nullptr};

static const char *const close_names[] = {
    "none",   "fade",   "zoom",   "glide1",   "glide2", "airplane", "beamup",
    "domino", "fold",   "glide3", "leafspread", "razr", "skewer",   nullptr};
static const char *const close_labels[] = {
    "None",   "Fade", "Zoom",    "Glide 1",     "Glide 2", "Airplane", "Beam Up",
    "Domino", "Fold", "Glide 3", "Leaf Spread", "Razr",    "Skewer",   nullptr};
static const char *const close_requires[] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, ADDON, ADDON,
    ADDON,   ADDON,   ADDON,   ADDON,   ADDON,   ADDON, nullptr};

#define ANIMATION_EVENT(prefix, tab, list, effect, duration)                                   \
    {                                                                                          \
        .key = prefix "Effect",                                                                \
        .label = "Effect",                                                                     \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_CHOICE,                                                       \
        .choice_names = list##_names,                                                          \
        .choice_labels = list##_labels,                                                        \
        .choice_requires = list##_requires,                                                    \
        .choice_fallback = "zoom",                                                             \
        .default_value = effect,                                                               \
    },                                                                                         \
    {                                                                                          \
        .key = prefix "Duration",                                                              \
        .label = "Duration (milliseconds)",                                                    \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_INT,                                                          \
        .min = 50,                                                                             \
        .max = 4000,                                                                           \
        .step = 10,                                                                            \
        .default_value = duration,                                                             \
    },                                                                                         \
    {                                                                                          \
        .key = prefix "Windows",                                                               \
        .label = "Windows",                                                                    \
        .description = "Leave empty for every window, or for example: type=normal",            \
        .group = tab,                                                                          \
        .type = XD_EFFECT_OPTION_MATCH,                                                        \
        .default_value = "",                                                                   \
    }

static const xd_effect_option options[] = {
    ANIMATION_EVENT("Open", "Open", open, "zoom", "200"),
    ANIMATION_EVENT("Close", "Close", close, "zoom", "200"),
    ANIMATION_EVENT("Minimize", "Minimize", close, "zoom", "300"),
    ANIMATION_EVENT("Unminimize", "Unminimize", close, "zoom", "300"),
};

const xd_effect_info xd_effect_animations = {
    .id = "Animations",
    .name = "Animations",
    .description = "Windows fade, zoom or glide as they open, close and minimize; with the "
                   "Animations Add-On, more effects.",
    .icon = "preferences-system-windows-effect-glide",
    .category = XD_EFFECT_CATEGORY_EFFECTS,
    .options = options,
    .option_count = sizeof(options) / sizeof(options[0]),
};
