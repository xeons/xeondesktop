/* SPDX-License-Identifier: MIT
 * Unit tests of Window Opacity (Compiz's Opacity, Brightness and
 * Saturation): its options as the file gives them, the rules, the
 * bindings' steps and what brightness and saturation do to a colour,
 * included by test_main.c for its CHECK.
 */
#include "../src/compositor/effects/opacity_model.h"

static bool opacity_is_near(float value, float expected) {
    return fabsf(value - expected) < 1e-4f;
}

/* The options: brightness and saturation need OpenGL, opacity doesn't;
 * values from the file are clamped to their ranges; the defaults change
 * nothing. */
static void test_opacity_options(void) {
    xd_effects_settings *settings = xd_effects_create_settings();
    size_t effect = 0;
    CHECK(xd_effects_find(settings, "WindowOpacity", &effect));
    const xd_effect_info *info = xd_effects_get_info(settings, effect);
    CHECK(!info->needs_opengl);
    static const struct {
        const char *key;
        bool opengl;
    } expected[] = {
        {"Step", false},
        {"IncreaseButton", false},
        {"Rule1Opacity", false},
        {"BrightnessStep", true},
        {"BrightnessIncreaseButton", true},
        {"BrightnessRule3Windows", true},
        {"SaturationDecreaseButton", true},
        {"SaturationRule2Value", true},
        {"InactiveBrightness", true},
        {"DimInactive", false},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(expected); i++) {
        size_t option = 0;
        CHECK(xd_effects_find_option(info, expected[i].key, &option));
        CHECK(info->options[option].needs_opengl == expected[i].opengl);
    }
    CHECK(xd_effects_get_int(settings, effect, "BrightnessStep") == 5);
    CHECK(xd_effects_get_int(settings, effect, "SaturationStep") == 5);
    CHECK(xd_effects_get_int(settings, effect, "InactiveBrightness") == 100);
    CHECK(strcmp(xd_effects_get_text(settings, effect, "BrightnessIncreaseButton"),
                 "<Shift><Alt>Button4") == 0);
    CHECK(strcmp(xd_effects_get_text(settings, effect, "SaturationDecreaseButton"),
                 "<Control><Alt>Button5") == 0);
    CHECK(strcmp(xd_effects_get_text(settings, effect, "SaturationRule1Windows"), "") == 0);

    g_autofree char *dir = g_dir_make_tmp("xd-opacity-XXXXXX", nullptr);
    g_autofree char *path = g_build_filename(dir, "effects.ini", nullptr);
    const char *text = "[WindowOpacity]\nEnabled=true\nBrightnessStep=500\n"
                       "BrightnessRule1Value=-20\nSaturationRule1Value=150\n"
                       "SaturationRule1Windows=app_id=foot\nInactiveBrightness=40\n"
                       "SaturationStep=nonsense\nRule2Opacity=0\n";
    CHECK(g_file_set_contents(path, text, -1, nullptr));
    xd_effects_load_file(settings, path);
    CHECK(xd_effects_is_enabled(settings, effect));
    CHECK(xd_effects_get_int(settings, effect, "BrightnessStep") == 50);
    CHECK(xd_effects_get_int(settings, effect, "BrightnessRule1Value") == 0);
    CHECK(xd_effects_get_int(settings, effect, "SaturationRule1Value") == 100);
    CHECK(strcmp(xd_effects_get_text(settings, effect, "SaturationRule1Windows"),
                 "app_id=foot") == 0);
    CHECK(xd_effects_get_int(settings, effect, "InactiveBrightness") == 40);
    CHECK(xd_effects_get_int(settings, effect, "SaturationStep") == 5);
    /* Opacity's rules still stop short of invisible. */
    CHECK(xd_effects_get_int(settings, effect, "Rule2Opacity") == 1);
    g_unlink(path);
    g_rmdir(dir);
    xd_effects_destroy_settings(settings);
}

/* The rules: the first whose windows match, in order; empty ones don't
 * count; no match is unchanged. */
static void test_opacity_rules(void) {
    xd_opacity_rule rules[XD_OPACITY_RULES] = {
        {.windows = "", .value = 0.1f},
        {.windows = "app_id=foot", .value = 0.4f},
        {.windows = "type=dialog | app_id=foo*", .value = 0.7f},
    };
    CHECK(opacity_is_near(xd_opacity_choose_rule(rules, 3, "foot", "Terminal", false), 0.4f));
    CHECK(opacity_is_near(xd_opacity_choose_rule(rules, 3, "footclient", "", false), 0.7f));
    CHECK(opacity_is_near(xd_opacity_choose_rule(rules, 3, "gedit", "Open", true), 0.7f));
    CHECK(opacity_is_near(xd_opacity_choose_rule(rules, 3, "gedit", "Untitled", false), 1.0f));
    CHECK(opacity_is_near(xd_opacity_choose_rule(rules, 1, "foot", "", false), 1.0f));
    CHECK(opacity_is_near(xd_opacity_choose_rule(rules, 3, nullptr, nullptr, false), 1.0f));
    /* Only an empty rule matches nothing; "any" is every window. */
    snprintf(rules[0].windows, sizeof(rules[0].windows), "%s", "any");
    CHECK(opacity_is_near(xd_opacity_choose_rule(rules, 3, "foot", "", false), 0.1f));
}

/* The bindings' steps, clamped: opacity never below 5%, brightness and
 * saturation down to 0; none above 1. */
static void test_opacity_steps(void) {
    CHECK(opacity_is_near(xd_opacity_step_value(XD_OPACITY_PROPERTY_BRIGHTNESS, 1.0f, -0.05f, 3),
                          0.85f));
    CHECK(opacity_is_near(xd_opacity_step_value(XD_OPACITY_PROPERTY_BRIGHTNESS, 0.1f, -0.05f, 5),
                          0.0f));
    CHECK(opacity_is_near(xd_opacity_step_value(XD_OPACITY_PROPERTY_SATURATION, 0.2f, -0.5f, 1),
                          0.0f));
    CHECK(opacity_is_near(xd_opacity_step_value(XD_OPACITY_PROPERTY_SATURATION, 0.9f, 0.05f, 4),
                          1.0f));
    CHECK(opacity_is_near(xd_opacity_step_value(XD_OPACITY_PROPERTY_OPACITY, 0.2f, -0.05f, 10),
                          0.05f));
    CHECK(opacity_is_near(xd_opacity_get_lowest(XD_OPACITY_PROPERTY_OPACITY), 0.05f));
    CHECK(opacity_is_near(xd_opacity_get_lowest(XD_OPACITY_PROPERTY_BRIGHTNESS), 0.0f));
    CHECK(!xd_opacity_is_colored(1.0f, 1.0f));
    CHECK(xd_opacity_is_colored(0.95f, 1.0f));
    CHECK(xd_opacity_is_colored(1.0f, 0.0f));
}

/* Colours: unchanged at 1 and 1; saturation 0 is a grey of the colour's
 * luminance; brightness scales; a premultiplied colour's alpha never
 * matters (the same scale of each channel). */
static void test_opacity_colors(void) {
    float red[3] = {0.8f, 0.2f, 0.1f};
    xd_opacity_adjust_color(red, 1.0f, 1.0f);
    CHECK(opacity_is_near(red[0], 0.8f) && opacity_is_near(red[1], 0.2f) &&
          opacity_is_near(red[2], 0.1f));
    float grey[3] = {0.8f, 0.2f, 0.1f};
    xd_opacity_adjust_color(grey, 1.0f, 0.0f);
    float luminance = 0.8f * 0.30f + 0.2f * 0.59f + 0.1f * 0.11f;
    CHECK(opacity_is_near(grey[0], luminance) && opacity_is_near(grey[1], luminance) &&
          opacity_is_near(grey[2], luminance));
    float white[3] = {1.0f, 1.0f, 1.0f};
    xd_opacity_adjust_color(white, 1.0f, 0.0f);
    CHECK(opacity_is_near(white[0], 1.0f) && opacity_is_near(white[2], 1.0f));
    float dark[3] = {0.8f, 0.2f, 0.1f};
    xd_opacity_adjust_color(dark, 0.5f, 1.0f);
    CHECK(opacity_is_near(dark[0], 0.4f) && opacity_is_near(dark[1], 0.1f) &&
          opacity_is_near(dark[2], 0.05f));
    /* Half saturated: halfway to grey; then halved. */
    float half[3] = {0.8f, 0.2f, 0.1f};
    xd_opacity_adjust_color(half, 0.5f, 0.5f);
    CHECK(opacity_is_near(half[0], (luminance + 0.8f) / 4.0f));
    /* Premultiplied at alpha 0.5: the same as the opaque colour, halved. */
    float translucent[3] = {0.4f, 0.1f, 0.05f};
    xd_opacity_adjust_color(translucent, 1.0f, 0.0f);
    CHECK(opacity_is_near(translucent[1], luminance / 2.0f));
}

static void test_effects_opacity(void) {
    test_opacity_options();
    test_opacity_rules();
    test_opacity_steps();
    test_opacity_colors();
}
