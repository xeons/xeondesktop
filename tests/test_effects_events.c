/* SPDX-License-Identifier: MIT
 * Unit tests for Burn and Explode as Animations' choices: the events'
 * lists, the pages they need, who takes an event (old effects.ini files
 * keep burning and exploding closing windows), and their progress run
 * backwards for opening and restoring. Included by test_main.c, which
 * has CHECK, after test_effects_gl.c and test_effects_addon.c.
 */

/* Each event lists Burn and Explode, each needing its own page on (and
 * OpenGL), Zoom meanwhile. */
static void test_events_choices(void) {
    xd_effects_settings *settings = xd_effects_create_settings();
    size_t animations = 0, burn = 0, explode = 0;
    CHECK(xd_effects_find(settings, "Animations", &animations));
    CHECK(xd_effects_find(settings, "Burn", &burn));
    CHECK(xd_effects_find(settings, "Explode", &explode));
    const xd_effect_info *info = xd_effects_get_info(settings, animations);
    static const char *const keys[] = {"OpenEffect", "CloseEffect", "MinimizeEffect",
                                       "UnminimizeEffect"};
    for (size_t i = 0; i < XD_ARRAY_LEN(keys); i++) {
        size_t option = 0;
        CHECK(xd_effects_find_option(info, keys[i], &option));
        const xd_effect_option *entry = &info->options[option];
        xd_effect_value value = {0};
        CHECK(xd_effects_parse_value(entry, "burn", &value));
        int burn_choice = value.choice;
        CHECK(xd_effects_parse_value(entry, "explode", &value));
        int explode_choice = value.choice;
        CHECK(strcmp(entry->choice_labels[burn_choice], "Burn") == 0);
        CHECK(strcmp(entry->choice_labels[explode_choice], "Explode") == 0);
        CHECK(strcmp(xd_effects_get_choice_requirement(entry, burn_choice), "Burn") == 0);
        CHECK(strcmp(xd_effects_get_choice_requirement(entry, explode_choice), "Explode") == 0);
        CHECK(strcmp(entry->choice_fallback, "zoom") == 0);
        CHECK(!xd_effects_is_choice_available(settings, animations, option, burn_choice, true));
        CHECK(!xd_effects_is_choice_available(settings, animations, option, explode_choice,
                                              true));
    }
    size_t option = 0;
    CHECK(xd_effects_find_option(info, "MinimizeEffect", &option));
    xd_effect_value value = {0};
    CHECK(xd_effects_parse_value(&info->options[option], "burn", &value));
    xd_effects_set_enabled(settings, burn, true);
    CHECK(xd_effects_is_choice_available(settings, animations, option, value.choice, true));
    CHECK(!xd_effects_is_choice_available(settings, animations, option, value.choice, false));
    /* Burn's page on doesn't make Explode's choice available. */
    xd_effect_value other = {0};
    CHECK(xd_effects_parse_value(&info->options[option], "explode", &other));
    CHECK(!xd_effects_is_choice_available(settings, animations, option, other.choice, true));
    xd_effects_destroy_settings(settings);

    /* By name, theirs and not the Add-On's, drawn as Zoom without them. */
    CHECK(xd_animation_find_effect("burn") == XD_ANIMATION_BURN);
    CHECK(xd_animation_find_effect("explode") == XD_ANIMATION_EXPLODE);
    CHECK(!xd_animation_is_addon(XD_ANIMATION_BURN));
    CHECK(!xd_animation_is_addon(XD_ANIMATION_EXPLODE));
    CHECK(xd_animation_get_fallback(XD_ANIMATION_BURN) == XD_ANIMATION_ZOOM);
    CHECK(xd_animation_get_fallback(XD_ANIMATION_EXPLODE) == XD_ANIMATION_ZOOM);
    const xd_effect_rect window = {100, 50, 400, 300};
    xd_animation_frame zoom = xd_animation_get_frame(XD_ANIMATION_ZOOM, window, nullptr, 0.4);
    xd_animation_frame burnt = xd_animation_get_frame(XD_ANIMATION_BURN, window, nullptr, 0.4);
    CHECK(memcmp(&zoom, &burnt, sizeof(zoom)) == 0);
}

/* Who takes an event: Burn's own claim on closing windows first, then
 * Animations' choice of it; from an effects.ini written before Burn was a
 * choice, closing windows still burn whatever Animations says. */
static void test_events_precedence(void) {
    /* Its own claim: closing windows it matches, nothing else. */
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_CLOSE, true, true,
                                 XD_ANIMATION_FADE, true) == XD_ANIMATION_CLAIM_OWN);
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_CLOSE, true, false,
                                 XD_ANIMATION_FADE, true) == XD_ANIMATION_CLAIM_NONE);
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_MINIMIZE, true, true,
                                 XD_ANIMATION_ZOOM, true) == XD_ANIMATION_CLAIM_NONE);
    /* Animations' choice, for the windows that event matches. */
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_MINIMIZE, true, true,
                                 XD_ANIMATION_BURN, true) == XD_ANIMATION_CLAIM_EVENT);
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_MINIMIZE, true, true,
                                 XD_ANIMATION_BURN, false) == XD_ANIMATION_CLAIM_NONE);
    CHECK(xd_animation_get_claim(XD_ANIMATION_EXPLODE, XD_ANIMATION_EVENT_OPEN, false, false,
                                 XD_ANIMATION_EXPLODE, true) == XD_ANIMATION_CLAIM_EVENT);
    CHECK(xd_animation_get_claim(XD_ANIMATION_EXPLODE, XD_ANIMATION_EVENT_OPEN, false, false,
                                 XD_ANIMATION_BURN, true) == XD_ANIMATION_CLAIM_NONE);
    /* Its own claim off: closing only where Animations chose it. */
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_CLOSE, false, true,
                                 XD_ANIMATION_ZOOM, true) == XD_ANIMATION_CLAIM_NONE);
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_CLOSE, false, true,
                                 XD_ANIMATION_BURN, true) == XD_ANIMATION_CLAIM_EVENT);
    /* Both: the page's (and its duration). */
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_CLOSE, true, true,
                                 XD_ANIMATION_BURN, true) == XD_ANIMATION_CLAIM_OWN);

    /* An old effects.ini: Burn and Explode on, Animations untouched or
     * set to fade: their own claims are on, as before. */
    xd_effects_settings *settings = xd_effects_create_settings();
    size_t animations = 0, burn = 0, explode = 0;
    CHECK(xd_effects_find(settings, "Animations", &animations));
    CHECK(xd_effects_find(settings, "Burn", &burn));
    CHECK(xd_effects_find(settings, "Explode", &explode));
    char *dir = g_dir_make_tmp("xd-events-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "effects.ini", nullptr);
    CHECK(g_file_set_contents(path,
                              "[Burn]\nEnabled=true\nDuration=4000\n"
                              "[Explode]\nEnabled=true\nWindows=type=dialog\n",
                              -1, nullptr));
    xd_effects_load_file(settings, path);
    CHECK(xd_effects_is_enabled(settings, burn) && !xd_effects_is_enabled(settings, animations));
    CHECK(xd_effects_get_bool(settings, burn, "Closing"));
    CHECK(xd_effects_get_bool(settings, explode, "Closing"));
    CHECK(xd_effects_get_int(settings, burn, "Duration") == 4000);
    CHECK(strcmp(xd_effects_get_choice_name(settings, animations, "CloseEffect"), "zoom") == 0);
    /* Burn's claim on, its match empty (every window): it takes closing,
     * Animations' choice (off: None) none of the others. */
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_CLOSE,
                                 xd_effects_get_bool(settings, burn, "Closing"), true,
                                 XD_ANIMATION_NONE, false) == XD_ANIMATION_CLAIM_OWN);

    /* A new one: Burn only on minimize, by Animations; closing zooms. */
    CHECK(g_file_set_contents(path,
                              "[Burn]\nEnabled=true\nClosing=false\n"
                              "[Animations]\nEnabled=true\nMinimizeEffect=burn\n"
                              "UnminimizeEffect=explode\n",
                              -1, nullptr));
    xd_effects_load_file(settings, path);
    bool closing = xd_effects_get_bool(settings, burn, "Closing");
    CHECK(!closing);
    xd_animation_effect minimize = xd_animation_find_effect(
        xd_effects_get_choice_name(settings, animations, "MinimizeEffect"));
    xd_animation_effect unminimize = xd_animation_find_effect(
        xd_effects_get_choice_name(settings, animations, "UnminimizeEffect"));
    xd_animation_effect close =
        xd_animation_find_effect(xd_effects_get_choice_name(settings, animations, "CloseEffect"));
    CHECK(minimize == XD_ANIMATION_BURN && unminimize == XD_ANIMATION_EXPLODE);
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_MINIMIZE, closing, true,
                                 minimize, true) == XD_ANIMATION_CLAIM_EVENT);
    CHECK(xd_animation_get_claim(XD_ANIMATION_BURN, XD_ANIMATION_EVENT_CLOSE, closing, true, close,
                                 true) == XD_ANIMATION_CLAIM_NONE);
    CHECK(xd_animation_get_fallback(close) == XD_ANIMATION_ZOOM);
    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
    xd_effects_destroy_settings(settings);
}

/* Opening and restoring run the closing backwards: from gone to whole,
 * ending exactly whole; turned round midway, from where it had got to. */
static void test_events_reversed(void) {
    CHECK(xd_animation_is_appearing(XD_ANIMATION_EVENT_OPEN));
    CHECK(xd_animation_is_appearing(XD_ANIMATION_EVENT_UNMINIMIZE));
    CHECK(!xd_animation_is_appearing(XD_ANIMATION_EVENT_CLOSE));
    CHECK(!xd_animation_is_appearing(XD_ANIMATION_EVENT_MINIMIZE));
    CHECK(xd_animation_get_start_progress(true) == 1);
    CHECK(xd_animation_get_start_progress(false) == 0);
    /* Frames of uneven lengths: it lands on 0 exactly, never past it. */
    double progress = xd_animation_get_start_progress(true);
    int frames = 0;
    bool falling = true;
    while (xd_animation_has_further(progress, true) && frames < 1000) {
        double next = xd_animation_step_progress(progress, 0.017 + 0.003 * (frames % 3), true);
        falling = falling && next < progress;
        progress = next;
        frames++;
    }
    CHECK(falling && progress == 0 && frames > 40 && frames < 70);
    /* Going: to 1 exactly. */
    CHECK(xd_animation_step_progress(0.95, 0.2, false) == 1);
    CHECK(!xd_animation_has_further(1, false) && xd_animation_has_further(0.99, false));
    /* Closed at 0.4 while opening: the close goes on from there. */
    double opening = xd_animation_step_progress(1, 0.6, true);
    CHECK(fabs(opening - 0.4) < 1e-12);
    CHECK(fabs(xd_animation_step_progress(opening, 0.1, false) - 0.5) < 1e-12);

    /* Burn, ended: nothing charred however ragged the front, and gone
     * at its start, however ragged. */
    double reach = xd_burn_get_ragged_reach();
    CHECK(reach > 0);
    CHECK(xd_burn_get_char(-reach, xd_burn_get_front(0)) == 0);
    CHECK(xd_burn_get_char(1 + reach, xd_burn_get_front(1)) == 1);
    /* Explode, ended: every corner where it was, opaque, flat. */
    xd_explode explode;
    CHECK(xd_explode_init(&explode, XD_EXPLODE_GLASS, 400, 300, 13, 10, 10, 4, 7));
    bool whole = xd_explode_get_alpha(0) == 1;
    for (size_t i = 0; i < explode.count; i++) {
        const xd_explode_piece *piece = &explode.pieces[i];
        double normal[3];
        xd_explode_get_normal(piece, 0, normal);
        whole = whole && fabs(normal[2] - 1) < 1e-12;
        for (int corner = 0; corner < piece->corner_count; corner++) {
            double x = 0, y = 0;
            xd_explode_get_corner(&explode, piece, corner, 0, &x, &y);
            whole = whole && fabs(x - piece->corners[corner][0]) < 1e-9 &&
                    fabs(y - piece->corners[corner][1]) < 1e-9;
        }
    }
    CHECK(whole);
    /* The same seed cuts the same pieces (turned round, they carry on). */
    xd_explode again;
    CHECK(xd_explode_init(&again, XD_EXPLODE_GLASS, 400, 300, 13, 10, 10, 4, 7));
    bool same = again.count == explode.count;
    for (size_t i = 0; same && i < explode.count; i++) {
        const xd_explode_piece *first = &explode.pieces[i], *second = &again.pieces[i];
        same = first->corner_count == second->corner_count &&
               first->center_x == second->center_x && first->center_y == second->center_y &&
               first->direction_x == second->direction_x && first->speed == second->speed &&
               first->spin == second->spin && first->rise == second->rise;
    }
    CHECK(same);
    xd_explode_free(&again);
    xd_explode_free(&explode);
}

static void test_effects_events(void) {
    test_events_choices();
    test_events_precedence();
    test_events_reversed();
}
