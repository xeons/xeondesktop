/* SPDX-License-Identifier: MIT
 * Unit tests for Digital Rain (src/compositor/effects/rain_model.h, its
 * page and its choices in Animations' events): the front as a window
 * breaks up and is built, its progress played backwards for opening,
 * the drops and their colours, the glyph sets, and the options'
 * defaults, clamping and round trip. Included by test_main.c, which has
 * CHECK, after test_effects_events.c.
 */
#include "../src/compositor/effects/rain_model.h"

/* Every cell of a window `rows` high, with every column's front at
 * `progress` however ragged (jitter -1, 0 and 1), at least `reveal`
 * (all_at_least) or at most it. */
static bool rain_is_revealed(double progress, int rows, double reveal, bool all_at_least) {
    for (int jitter = -1; jitter <= 1; jitter++) {
        double front = xd_digital_rain_get_front(progress, rows, jitter);
        for (int row = 0; row < rows; row++) {
            double shown = xd_digital_rain_get_reveal(row, front);
            if (all_at_least ? shown < reveal : shown > reveal)
                return false;
        }
    }
    return true;
}

static void test_rain_front(void) {
    const int rows = 30;
    /* Unbuilding (closing): whole at 0 however ragged, gone at 1. */
    CHECK(rain_is_revealed(0, rows, 1, true));
    CHECK(rain_is_revealed(1, rows, 0, false));
    /* Midway its top has broken up and its bottom is still there. */
    double middle = xd_digital_rain_get_front(0.5, rows, 0);
    CHECK(fabs(middle - rows / 2.0) < 2);
    CHECK(xd_digital_rain_get_reveal(0, middle) == 0);
    CHECK(xd_digital_rain_get_reveal(rows / 4, middle) == 0);
    CHECK(xd_digital_rain_get_reveal(rows * 3 / 4, middle) == 1);
    CHECK(xd_digital_rain_get_reveal(rows - 1, middle) == 1);
    /* Just below the front glyphs and pixels mix. */
    double mixing = xd_digital_rain_get_reveal((int)middle, middle);
    CHECK(mixing > 0 && mixing < 1);
    /* Ragged: a column ahead has broken up further, within the reach. */
    double ahead = xd_digital_rain_get_front(0.5, rows, 1);
    double behind = xd_digital_rain_get_front(0.5, rows, -1);
    double reach = xd_digital_rain_get_reach(rows);
    CHECK(ahead > middle && behind < middle);
    CHECK(fabs(ahead - middle - reach) < 1e-9 && fabs(middle - behind - reach) < 1e-9);
    CHECK(reach >= 1 && reach < rows / 4.0);
    /* It only ever moves down as the progress grows. */
    bool down = true;
    for (int step = 1; step <= 20; step++)
        down = down && xd_digital_rain_get_front(step / 20.0, rows, 0) >
                           xd_digital_rain_get_front((step - 1) / 20.0, rows, 0);
    CHECK(down);
    /* Out of range: as at the ends. */
    CHECK(xd_digital_rain_get_front(-1, rows, 0) == xd_digital_rain_get_front(0, rows, 0));
    CHECK(xd_digital_rain_get_front(2, rows, 5) == xd_digital_rain_get_front(1, rows, 1));

    /* Building (opening): progress played from 1 back to 0, so it starts
     * as rain only, is half built from the bottom up halfway through,
     * and ends whole, exactly. */
    double progress = xd_animation_get_start_progress(true);
    CHECK(progress == 1 && rain_is_revealed(progress, rows, 0, false));
    progress = xd_animation_step_progress(progress, 0.5, true);
    CHECK(fabs(progress - 0.5) < 1e-12);
    double rising = xd_digital_rain_get_front(progress, rows, 0);
    CHECK(xd_digital_rain_get_reveal(rows - 1, rising) == 1); /* its bottom built */
    CHECK(xd_digital_rain_get_reveal(0, rising) == 0);        /* its top still rain */
    progress = xd_animation_step_progress(progress, 0.25, true);
    CHECK(xd_digital_rain_get_front(progress, rows, 0) < rising); /* the front rises */
    int frames = 0;
    while (xd_animation_has_further(progress, true) && frames < 1000) {
        progress = xd_animation_step_progress(progress, 0.017, true);
        frames++;
    }
    CHECK(progress == 0 && rain_is_revealed(progress, rows, 1, true));
    /* Closed while it opens: it breaks up again from where it had got
     * to. */
    double opening = xd_animation_step_progress(1, 0.7, true);
    CHECK(fabs(xd_animation_step_progress(opening, 0.1, false) - 0.4) < 1e-12);
    /* A window a single row high works the same. */
    CHECK(rain_is_revealed(0, 1, 1, true) && rain_is_revealed(1, 1, 0, false));
}

static void test_rain_drops(void) {
    const int rows = 30;
    /* Thickest just above the front, sparse further up, none at the
     * gone end. */
    double front = 20;
    double near = xd_digital_rain_get_density(19, front, rows, 0.5);
    double far = xd_digital_rain_get_density(0, front, rows, 0.5);
    CHECK(near > far && near <= 1 && far >= xd_digital_rain_get_sparse() - 1e-9);
    CHECK(far < 0.5);
    CHECK(xd_digital_rain_get_envelope(0.5) == 1);
    CHECK(xd_digital_rain_get_envelope(1) == 0);
    CHECK(xd_digital_rain_get_density(0, 40, rows, 1) == 0);
    double fading = xd_digital_rain_get_envelope(0.95);
    CHECK(fading > 0 && fading < 1);
    CHECK(xd_digital_rain_get_fall_speed(1) > 1 && xd_digital_rain_get_fall_speed(1000) < 1000);

    /* A drop every 10 rows, its head 25 rows down: row 25 is a head, 24
     * behind it by a row, 15 the next drop's head. */
    int64_t drop = 0, other = 0;
    CHECK(xd_digital_rain_get_behind(25.5, 10, 25, &drop) == 0.5);
    double one = xd_digital_rain_get_behind(25.5, 10, 24, &other);
    CHECK(fabs(one - 1.5) < 1e-12 && other == drop);
    double next = xd_digital_rain_get_behind(25.5, 10, 15, &other);
    CHECK(fabs(next - 0.5) < 1e-12 && other == drop + 1);
    /* Below the head: the drop before, not yet there. */
    double below = xd_digital_rain_get_behind(25.5, 10, 27, &other);
    CHECK(below >= 0 && below < 10 && other == drop - 1);
    /* Bright at the head, dimmer down the trail, nothing past it. */
    CHECK(xd_digital_rain_get_brightness(0.5, 6) == 1);
    double trailing = xd_digital_rain_get_brightness(3.2, 6);
    CHECK(trailing > 0 && trailing < 1);
    CHECK(xd_digital_rain_get_brightness(6.5, 6) == 0);
    CHECK(xd_digital_rain_get_brightness(-1, 6) == 0);

    /* The head near white, the trail green, dark at its end. */
    const double green[3] = {0.0, 1.0, 0.27};
    double head[3], middle[3], end[3];
    xd_digital_rain_get_glyph_color(1, green, head);
    xd_digital_rain_get_glyph_color(0.5, green, middle);
    xd_digital_rain_get_glyph_color(1.0 / 6, green, end);
    CHECK(head[0] >= 0.8 && head[1] >= 0.8 && head[2] >= 0.8);
    CHECK(middle[1] > middle[0] && middle[1] > middle[2] && middle[0] < 0.1);
    CHECK(end[1] < middle[1] && end[1] < 0.4);
    /* Fading out down the trail, the head and most of it opaque. */
    CHECK(xd_digital_rain_get_glyph_opacity(1) == 1);
    CHECK(xd_digital_rain_get_glyph_opacity(0.7) == 1);
    CHECK(xd_digital_rain_get_glyph_opacity(1.0 / 6) < 0.3);
    CHECK(xd_digital_rain_get_glyph_opacity(0) == 0);
}

static void test_rain_glyphs(void) {
    xd_rain_glyph glyphs[XD_RAIN_GLYPHS_MAX];
    /* The saver's sets: half-width katakana mirrored, then digits. */
    size_t count = xd_rain_list_glyphs(XD_RAIN_KATAKANA, glyphs);
    CHECK(count == 56 + 10);
    CHECK(glyphs[0].mirrored && strcmp(glyphs[0].text, "\xEF\xBD\xA6") == 0);
    CHECK(!glyphs[count - 1].mirrored && strcmp(glyphs[count - 1].text, "9") == 0);
    CHECK(xd_rain_list_glyphs(XD_RAIN_DIGITS, glyphs) == 10);
    CHECK(xd_rain_list_glyphs(XD_RAIN_BINARY, glyphs) == 2 && strcmp(glyphs[1].text, "1") == 0);
    CHECK(xd_rain_list_glyphs(XD_RAIN_LETTERS, glyphs) == 36 && strcmp(glyphs[0].text, "A") == 0);
    /* Cells a little narrower than high, never empty. */
    int width = 0, height = 0;
    xd_rain_get_cell_size(14, &width, &height);
    CHECK(width == 10 && height == 16);
    xd_rain_get_cell_size(0, &width, &height);
    CHECK(width >= 3 && height >= 4);
    /* Drawn: some ink in the cell, none outside it. */
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, 40, 32);
    cairo_t *cairo = cairo_create(surface);
    PangoLayout *layout = xd_rain_create_layout(cairo, XD_RAIN_LETTERS, 14);
    xd_rain_list_glyphs(XD_RAIN_LETTERS, glyphs);
    xd_rain_draw_glyph(cairo, layout, &glyphs[22], 10, 8, 10, 16); /* W, wide */
    g_object_unref(layout);
    cairo_destroy(cairo);
    cairo_surface_flush(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    int inside = 0, outside = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 40; x++) {
            bool in_cell = x >= 10 && x < 20 && y >= 8 && y < 24;
            if (data[y * stride + x] > 0)
                *(in_cell ? &inside : &outside) += 1;
        }
    }
    CHECK(inside > 5 && outside == 0);
    cairo_surface_destroy(surface);
    /* The colours are the saver's. */
    double rgb[3];
    xd_rain_get_base_color(XD_RAIN_AMBER, (xd_rgb){0}, rgb);
    CHECK(rgb[0] == 1.0 && rgb[1] == 0.62);
    xd_rain_get_base_color(XD_RAIN_CUSTOM, (xd_rgb){255, 0, 51}, rgb);
    CHECK(rgb[0] == 1.0 && rgb[1] == 0.0 && fabs(rgb[2] - 0.2) < 1e-12);
}

/* Each event lists Digital Rain, needing its page on (and OpenGL), Zoom
 * meanwhile; it takes only what Animations gives it. */
static void test_rain_choices(void) {
    xd_effects_settings *settings = xd_effects_create_settings();
    size_t animations = 0, rain = 0;
    CHECK(xd_effects_find(settings, "Animations", &animations));
    CHECK(xd_effects_find(settings, "DigitalRain", &rain));
    CHECK(xd_effects_get_info(settings, rain)->needs_opengl);
    CHECK(strcmp(xd_effects_get_info(settings, rain)->name, "Digital Rain") == 0);
    const xd_effect_info *info = xd_effects_get_info(settings, animations);
    static const char *const keys[] = {"OpenEffect", "CloseEffect", "MinimizeEffect",
                                       "UnminimizeEffect"};
    for (size_t i = 0; i < XD_ARRAY_LEN(keys); i++) {
        size_t option = 0;
        CHECK(xd_effects_find_option(info, keys[i], &option));
        const xd_effect_option *entry = &info->options[option];
        xd_effect_value value = {0};
        CHECK(xd_effects_parse_value(entry, "digitalrain", &value));
        CHECK(strcmp(entry->choice_names[value.choice], "digitalrain") == 0);
        CHECK(strcmp(entry->choice_labels[value.choice], "Digital Rain") == 0);
        /* Beside Burn, as CCSM lists effects by name after Compiz's own. */
        CHECK(value.choice > 0 && strcmp(entry->choice_names[value.choice - 1], "burn") == 0);
        CHECK(strcmp(xd_effects_get_choice_requirement(entry, value.choice), "DigitalRain") == 0);
        CHECK(strcmp(entry->choice_fallback, "zoom") == 0);
        CHECK(!xd_effects_is_choice_available(settings, animations, option, value.choice, true));
        xd_effects_set_enabled(settings, rain, true);
        CHECK(xd_effects_is_choice_available(settings, animations, option, value.choice, true));
        CHECK(!xd_effects_is_choice_available(settings, animations, option, value.choice, false));
        xd_effects_set_enabled(settings, rain, false);
    }
    /* Its page on doesn't make Burn's choice available. */
    size_t option = 0;
    CHECK(xd_effects_find_option(info, "CloseEffect", &option));
    xd_effect_value burn = {0};
    CHECK(xd_effects_parse_value(&info->options[option], "burn", &burn));
    xd_effects_set_enabled(settings, rain, true);
    CHECK(!xd_effects_is_choice_available(settings, animations, option, burn.choice, true));
    xd_effects_destroy_settings(settings);

    CHECK(xd_animation_find_effect("digitalrain") == XD_ANIMATION_DIGITAL_RAIN);
    CHECK(!xd_animation_is_addon(XD_ANIMATION_DIGITAL_RAIN));
    CHECK(xd_animation_get_fallback(XD_ANIMATION_DIGITAL_RAIN) == XD_ANIMATION_ZOOM);
    const xd_effect_rect window = {100, 50, 400, 300};
    xd_animation_frame zoom = xd_animation_get_frame(XD_ANIMATION_ZOOM, window, nullptr, 0.4);
    xd_animation_frame rain_frame =
        xd_animation_get_frame(XD_ANIMATION_DIGITAL_RAIN, window, nullptr, 0.4);
    CHECK(memcmp(&zoom, &rain_frame, sizeof(zoom)) == 0);
    /* No claim of its own: closing windows rain only where Animations
     * says, so an effects.ini from before it changes nothing. */
    for (int event = 0; event < XD_ANIMATION_EVENT_COUNT; event++) {
        CHECK(xd_animation_get_claim(XD_ANIMATION_DIGITAL_RAIN, (xd_animation_event)event, false,
                                     false, XD_ANIMATION_DIGITAL_RAIN,
                                     true) == XD_ANIMATION_CLAIM_EVENT);
        CHECK(xd_animation_get_claim(XD_ANIMATION_DIGITAL_RAIN, (xd_animation_event)event, false,
                                     false, XD_ANIMATION_ZOOM, true) == XD_ANIMATION_CLAIM_NONE);
        CHECK(xd_animation_get_claim(XD_ANIMATION_DIGITAL_RAIN, (xd_animation_event)event, false,
                                     false, XD_ANIMATION_DIGITAL_RAIN,
                                     false) == XD_ANIMATION_CLAIM_NONE);
    }
}

/* Its options: the defaults, clamped to the page's ranges, what can't
 * be read left at the default, and saved and read back the same. */
static void test_rain_settings(void) {
    xd_effects_settings *settings = xd_effects_create_settings();
    size_t rain = 0;
    CHECK(xd_effects_find(settings, "DigitalRain", &rain));
    CHECK(!xd_effects_is_enabled(settings, rain));
    CHECK(xd_effects_get_int(settings, rain, "Duration") == 1200);
    CHECK(xd_effects_get_int(settings, rain, "GlyphSize") == 14);
    CHECK(strcmp(xd_effects_get_choice_name(settings, rain, "Characters"), "katakana") == 0);
    CHECK(strcmp(xd_effects_get_choice_name(settings, rain, "Color"), "green") == 0);
    CHECK(xd_effects_get_color(settings, rain, "CustomColor") == 0x00FF46FFu);
    CHECK(xd_effects_get_bool(settings, rain, "Glow"));
    CHECK(strcmp(xd_effects_get_text(settings, rain, "Windows"), "") == 0);

    g_autofree char *dir = g_dir_make_tmp("xd-rain-XXXXXX", nullptr);
    g_autofree char *path = g_build_filename(dir, "effects.ini", nullptr);
    g_autofree char *again_path = g_build_filename(dir, "again.ini", nullptr);
    CHECK(g_file_set_contents(path,
                              "[DigitalRain]\nEnabled=true\nDuration=99999\nGlyphSize=2\n"
                              "Characters=runes\nColor=window\nGlow=perhaps\n",
                              -1, nullptr));
    xd_effects_load_file(settings, path);
    CHECK(xd_effects_is_enabled(settings, rain));
    CHECK(xd_effects_get_int(settings, rain, "Duration") == 5000);
    CHECK(xd_effects_get_int(settings, rain, "GlyphSize") == 8);
    CHECK(strcmp(xd_effects_get_choice_name(settings, rain, "Characters"), "katakana") == 0);
    CHECK(strcmp(xd_effects_get_choice_name(settings, rain, "Color"), "window") == 0);
    CHECK(xd_effects_get_bool(settings, rain, "Glow"));
    CHECK(g_file_set_contents(path,
                              "[DigitalRain]\nEnabled=true\nDuration=10\nGlyphSize=400\n"
                              "Characters=binary\nColor=custom\nCustomColor=#FF8000FF\n"
                              "Glow=false\nWindows=type=normal\n",
                              -1, nullptr));
    xd_effects_load_file(settings, path);
    CHECK(xd_effects_get_int(settings, rain, "Duration") == 200);
    CHECK(xd_effects_get_int(settings, rain, "GlyphSize") == 32);

    /* Saved and read back into fresh settings: the same. */
    CHECK(xd_effects_save_file(settings, again_path));
    xd_effects_settings *again = xd_effects_create_settings();
    xd_effects_load_file(again, again_path);
    CHECK(xd_effects_is_enabled(again, rain));
    CHECK(xd_effects_get_int(again, rain, "Duration") == 200);
    CHECK(xd_effects_get_int(again, rain, "GlyphSize") == 32);
    CHECK(strcmp(xd_effects_get_choice_name(again, rain, "Characters"), "binary") == 0);
    CHECK(strcmp(xd_effects_get_choice_name(again, rain, "Color"), "custom") == 0);
    CHECK(xd_effects_get_color(again, rain, "CustomColor") == 0xFF8000FFu);
    CHECK(!xd_effects_get_bool(again, rain, "Glow"));
    CHECK(strcmp(xd_effects_get_text(again, rain, "Windows"), "type=normal") == 0);
    xd_effects_destroy_settings(again);
    g_remove(path);
    g_remove(again_path);
    g_rmdir(dir);
    xd_effects_destroy_settings(settings);
}

static void test_effects_rain(void) {
    test_rain_front();
    test_rain_drops();
    test_rain_glyphs();
    test_rain_choices();
    test_rain_settings();
}
