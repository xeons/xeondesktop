/* SPDX-License-Identifier: MIT
 * Unit tests for the Animations Add-On: its effects' maths, its options
 * in effects.ini, and Animations falling back to Zoom without it.
 * Included by test_main.c, which has CHECK.
 */
#include "../src/compositor/effects/addon_model.h"
#include "../src/compositor/effects/animations_model.h"

static constexpr double ADDON_WIDTH = 400;
static constexpr double ADDON_HEIGHT = 300;

/* Every piece's corners where they are and opaque, facing the viewer. */
static bool addon_is_whole(const xd_addon *addon, double progress) {
    bool whole = true;
    for (size_t i = 0; i < addon->count; i++) {
        const xd_addon_piece *piece = &addon->pieces[i];
        xd_addon_pose pose;
        xd_addon_get_pose(addon, i, progress, &pose);
        for (int corner = 0; corner < piece->corner_count; corner++)
            whole = whole && fabs(pose.corners[corner][0] - piece->corners[corner][0]) < 1e-6 &&
                    fabs(pose.corners[corner][1] - piece->corners[corner][1]) < 1e-6 &&
                    fabs(pose.corners[corner][2]) < 1e-6 && pose.alpha[corner] == 1;
        whole = whole && fabs(pose.normal[2] - 1) < 1e-9;
    }
    return whole;
}

static bool addon_is_gone(const xd_addon *addon) {
    bool gone = true;
    for (size_t i = 0; i < addon->count; i++) {
        xd_addon_pose pose;
        xd_addon_get_pose(addon, i, 1.0, &pose);
        for (int corner = 0; corner < addon->pieces[i].corner_count; corner++)
            gone = gone && pose.alpha[corner] == 0;
    }
    return gone;
}

static double addon_get_total_area(const xd_addon *addon) {
    double total = 0;
    for (size_t i = 0; i < addon->count; i++) {
        const xd_addon_piece *piece = &addon->pieces[i];
        double twice = 0;
        for (int corner = 0; corner < piece->corner_count; corner++) {
            const double *current = piece->corners[corner];
            const double *next = piece->corners[(corner + 1) % piece->corner_count];
            twice += current[0] * next[1] - next[0] * current[1];
        }
        total += fabs(twice) / 2;
    }
    return total;
}

/* A piece's middle at `progress`. */
static void addon_get_piece_middle(const xd_addon *addon, size_t index, double progress,
                                   double middle[3]) {
    xd_addon_pose pose;
    xd_addon_get_pose(addon, index, progress, &pose);
    int count = addon->pieces[index].corner_count;
    middle[0] = middle[1] = middle[2] = 0;
    for (int corner = 0; corner < count; corner++) {
        for (int axis = 0; axis < 3; axis++)
            middle[axis] += pose.corners[corner][axis] / count;
    }
}

/* Every effect: the window whole at the start, covered by its pieces,
 * gone at the end, the same from the same seed. */
static void test_addon_model_common(void) {
    xd_addon_options options;
    xd_addon_get_default_options(&options);
    for (int kind = 0; kind < XD_ADDON_EFFECT_COUNT; kind++) {
        xd_addon_effect effect = (xd_addon_effect)kind;
        xd_addon first, second;
        CHECK(xd_addon_init(&first, effect, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 7));
        CHECK(xd_addon_init(&second, effect, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 7));
        CHECK(first.count > 0 && first.count == second.count);
        /* Airplane's folded corners are counted twice (the flap and what's
         * under it), the others tile the window. */
        double area = addon_get_total_area(&first);
        CHECK(effect == XD_ADDON_AIRPLANE ? area > ADDON_WIDTH * ADDON_HEIGHT - 1e-3
                                          : fabs(area - ADDON_WIDTH * ADDON_HEIGHT) < 1e-3);
        CHECK(addon_is_whole(&first, 0));
        CHECK(addon_is_gone(&first));
        bool same = true, moved = false;
        for (size_t i = 0; i < first.count; i++) {
            xd_addon_pose one, two;
            xd_addon_get_pose(&first, i, 0.5, &one);
            xd_addon_get_pose(&second, i, 0.5, &two);
            same = same && memcmp(&one, &two, sizeof(one)) == 0;
            for (int corner = 0; corner < first.pieces[i].corner_count; corner++)
                moved = moved || fabs(one.corners[corner][0] - first.pieces[i].corners[corner][0]) >
                                     1 ||
                        fabs(one.corners[corner][2]) > 1 || one.alpha[corner] < 1;
        }
        CHECK(same && moved);
        xd_addon_free(&first);
        xd_addon_free(&second);
    }
    xd_addon empty;
    CHECK(!xd_addon_init(&empty, XD_ADDON_DOMINO, 0, 300, &options, nullptr, 1));
    xd_addon_free(&empty);
}

static void test_addon_options(void) {
    xd_addon_options options;
    xd_addon_get_default_options(&options);
    options.airplane_path_length = 9;
    options.beam_spacing = 0;
    options.fold_columns = 50;
    options.fold_count = -3;
    options.leaf_spread = 0;
    options.skewer_across = 0;
    options.skewer_tessellation = XD_EXPLODE_GLASS;
    options.domino_direction = XD_ADDON_IN; /* not one of Domino's */
    xd_addon_clamp_options(&options);
    CHECK(options.airplane_path_length == 3.0 && options.beam_spacing == 1);
    CHECK(options.fold_columns == 10 && options.fold_count == 0);
    CHECK(options.leaf_spread == 0.1 && options.skewer_across == 1);
    CHECK(options.skewer_tessellation == XD_EXPLODE_RECTANGULAR);
    CHECK(options.domino_direction == XD_ADDON_DOWN);
    /* The eye as Compiz had it: a 60 degree field of view. */
    CHECK(fabs(xd_addon_get_eye_distance(1000) - 866.0254) < 1e-3);
}

static void test_addon_airplane(void) {
    xd_addon_options options;
    xd_addon_get_default_options(&options);
    xd_addon plane;
    CHECK(xd_addon_init(&plane, XD_ADDON_AIRPLANE, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 3));
    /* Folded (before it flies): its halves have gone back, a keel, and
     * its corners have folded onto it: inside the window's width, the
     * slanted wing folds dipping a little below it. */
    bool keel = false, inside = true;
    for (size_t i = 0; i < plane.count; i++) {
        xd_addon_pose pose;
        xd_addon_get_pose(&plane, i, 0.5, &pose);
        for (int corner = 0; corner < plane.pieces[i].corner_count; corner++) {
            keel = keel || pose.corners[corner][2] < -20;
            inside = inside && pose.corners[corner][0] > -1 &&
                     pose.corners[corner][0] < ADDON_WIDTH + 1 &&
                     pose.corners[corner][1] > -1 && pose.corners[corner][1] < ADDON_HEIGHT * 1.2;
        }
    }
    CHECK(keel && inside);
    /* Then up and away. */
    double middle[3];
    addon_get_piece_middle(&plane, 0, 0.95, middle);
    CHECK(middle[1] < 0 && middle[2] < 0);
    xd_addon_free(&plane);

    /* Minimizing, into its taskbar button below. */
    const xd_effect_rect button = {100, 700, 160, 24};
    CHECK(xd_addon_init(&plane, XD_ADDON_AIRPLANE, ADDON_WIDTH, ADDON_HEIGHT, &options, &button,
                        3));
    bool arrived = true;
    for (size_t i = 0; i < plane.count; i++) {
        addon_get_piece_middle(&plane, i, 0.999, middle);
        arrived = arrived && fabs(middle[0] - 180) < 80 && fabs(middle[1] - 712) < 80;
    }
    CHECK(arrived);
    xd_addon_free(&plane);
}

static void test_addon_beam_up(void) {
    xd_addon_options options;
    xd_addon_get_default_options(&options);
    xd_addon beam;
    CHECK(xd_addon_init(&beam, XD_ADDON_BEAM_UP, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 9));
    /* A beam each Width + Spacing across, twice over. */
    CHECK(beam.beam_count == 2 * (size_t)(ADDON_WIDTH / (8 + 5)));
    xd_addon_beam_state state;
    size_t lit_at_start = 0, lit = 0;
    for (size_t i = 0; i < beam.beam_count; i++) {
        lit_at_start += xd_addon_get_beam(&beam, i, 0, &state) ? 1 : 0;
        if (xd_addon_get_beam(&beam, i, 0.45, &state)) {
            lit++;
            CHECK(state.x >= 0 && state.x <= ADDON_WIDTH && state.width == 8 &&
                  state.brightness >= 0 && state.brightness <= 1);
        }
    }
    CHECK(lit_at_start == 0 && lit > beam.beam_count / 4);
    /* It goes from the bottom up, glowing, drawn in to a column. */
    xd_addon_pose top, bottom;
    xd_addon_get_pose(&beam, 0, 0.6, &top);
    xd_addon_get_pose(&beam, beam.count - 1, 0.6, &bottom);
    CHECK(top.alpha[0] > 0.9 && bottom.alpha[3] < 0.1 && top.glow > 0.5);
    xd_addon_get_pose(&beam, 0, 0.95, &top);
    CHECK(fabs(top.corners[0][0] - ADDON_WIDTH / 2) < ADDON_WIDTH * 0.15);
    /* Slowdown 0: beams rise steadily, and higher than slowed ones. */
    xd_addon steady;
    options.beam_slowdown = 0;
    CHECK(xd_addon_init(&steady, XD_ADDON_BEAM_UP, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr,
                        9));
    xd_addon_beam_state slowed;
    for (size_t i = 0; i < beam.beam_count; i++) {
        if (xd_addon_get_beam(&beam, i, 0.5, &slowed) &&
            xd_addon_get_beam(&steady, i, 0.5, &state)) {
            CHECK(state.top <= slowed.top + 1e-9);
        }
    }
    xd_addon_free(&steady);
    xd_addon_free(&beam);
}

static void test_addon_dominoes(void) {
    xd_addon_options options;
    xd_addon_get_default_options(&options);
    options.domino_piece_size = 100; /* 4 x 3 */
    xd_addon domino;
    CHECK(xd_addon_init(&domino, XD_ADDON_DOMINO, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 2));
    CHECK(domino.count == 12 && domino.direction == XD_ADDON_DOWN && domino.thickness > 0);
    /* Falling down: the top row first, toward the viewer, about its
     * bottom edge; the bottom row still stands. */
    xd_addon_pose first, last;
    xd_addon_get_pose(&domino, 0, 0.25, &first);
    xd_addon_get_pose(&domino, 11, 0.25, &last);
    CHECK(first.normal[2] < 0.99 && first.corners[0][2] > 1 && fabs(first.corners[3][2]) < 1e-6);
    CHECK(fabs(last.normal[2] - 1) < 1e-9);
    xd_addon_free(&domino);

    /* Random picks one of the four ways, the same from the same seed. */
    options.domino_direction = XD_ADDON_RANDOM;
    CHECK(xd_addon_init(&domino, XD_ADDON_DOMINO, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 5));
    xd_addon_direction picked = domino.direction;
    CHECK(picked <= XD_ADDON_RIGHT);
    xd_addon_free(&domino);
    CHECK(xd_addon_init(&domino, XD_ADDON_DOMINO, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 5));
    CHECK(domino.direction == picked);
    xd_addon_free(&domino);

    /* Razr: each piece flips right over, its back to the viewer. */
    options.razr_piece_size = 100;
    options.razr_direction = XD_ADDON_LEFT;
    xd_addon razr;
    CHECK(xd_addon_init(&razr, XD_ADDON_RAZR, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 4));
    const xd_addon_piece *piece = &razr.pieces[0];
    xd_addon_pose flipped;
    xd_addon_get_pose(&razr, 0, piece->hinges[0].end, &flipped);
    CHECK(fabs(flipped.normal[2] + 1) < 1e-6 && flipped.corners[1][0] < piece->corners[0][0]);
    xd_addon_free(&razr);
}

static void test_addon_fold(void) {
    xd_addon_options options;
    xd_addon_get_default_options(&options); /* 3 x 3, 4 folds: all of them */
    xd_addon fold;
    CHECK(xd_addon_init(&fold, XD_ADDON_FOLD, 300, 300, &options, nullptr, 1));
    CHECK(fold.count == 9);
    /* Folded up: every piece on the top left cell, still showing. */
    bool stacked = true;
    for (size_t i = 0; i < fold.count; i++) {
        xd_addon_pose pose;
        xd_addon_get_pose(&fold, i, 0.79, &pose);
        for (int corner = 0; corner < 4; corner++)
            stacked = stacked && pose.corners[corner][0] > -1 && pose.corners[corner][0] < 101 &&
                      pose.corners[corner][1] > -1 && pose.corners[corner][1] < 101 &&
                      fabs(pose.corners[corner][2]) < 10 && pose.alpha[corner] > 0.9;
    }
    CHECK(stacked);
    xd_addon_free(&fold);
    /* Two folds: the rows only, onto the top one, folding away from the
     * viewer. */
    options.fold_count = 2;
    CHECK(xd_addon_init(&fold, XD_ADDON_FOLD, 300, 300, &options, nullptr, 1));
    double middle[3];
    addon_get_piece_middle(&fold, 8, 0.79, middle);
    CHECK(middle[0] > 200 && middle[1] < 100);
    addon_get_piece_middle(&fold, 8, 0.2, middle);
    CHECK(middle[2] < -10);
    xd_addon_free(&fold);
    options.fold_out = true;
    CHECK(xd_addon_init(&fold, XD_ADDON_FOLD, 300, 300, &options, nullptr, 1));
    addon_get_piece_middle(&fold, 8, 0.2, middle);
    CHECK(middle[2] > 10);
    xd_addon_free(&fold);
}

static void test_addon_glide_leaf_skewer(void) {
    xd_addon_options options;
    xd_addon_get_default_options(&options);
    /* Glide 3: tips back about its middle (its top away) and goes back. */
    xd_addon glide;
    CHECK(xd_addon_init(&glide, XD_ADDON_GLIDE_3, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 1));
    CHECK(glide.count == 1);
    xd_addon_pose pose;
    xd_addon_get_pose(&glide, 0, 0.5, &pose);
    CHECK(pose.corners[0][2] < pose.corners[3][2] && pose.corners[3][2] < 0);
    CHECK(pose.alpha[0] > 0 && pose.alpha[0] < 1);
    xd_addon_free(&glide);

    /* Leaf Spread: farther apart midway, more so spread wider. */
    double spread[2];
    for (int wide = 0; wide < 2; wide++) {
        options.leaf_spread = wide ? 3.0 : 1.0;
        xd_addon leaf;
        CHECK(xd_addon_init(&leaf, XD_ADDON_LEAF_SPREAD, ADDON_WIDTH, ADDON_HEIGHT, &options,
                            nullptr, 11));
        double before = 0, after = 0;
        for (size_t i = 0; i < leaf.count; i++) {
            double middle[3];
            addon_get_piece_middle(&leaf, i, 0.6, middle);
            before += hypot(leaf.pieces[i].center_x - 200, leaf.pieces[i].center_y - 150);
            after += hypot(middle[0] - 200, middle[1] - 150);
        }
        CHECK(after > before * 1.2);
        spread[wide] = after;
        xd_addon_free(&leaf);
    }
    CHECK(spread[1] > spread[0]);

    /* Skewer, left: every piece goes left, turning about its way; In:
     * away from the viewer. */
    options.skewer_direction = XD_ADDON_LEFT;
    xd_addon skewer;
    CHECK(xd_addon_init(&skewer, XD_ADDON_SKEWER, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 6));
    CHECK(skewer.count == 24);
    bool left = true;
    for (size_t i = 0; i < skewer.count; i++) {
        double middle[3];
        addon_get_piece_middle(&skewer, i, 0.99, middle);
        left = left && middle[0] < skewer.pieces[i].center_x - 10;
    }
    CHECK(left);
    xd_addon_free(&skewer);
    options.skewer_direction = XD_ADDON_IN;
    options.skewer_depth = 0;
    options.skewer_tessellation = XD_EXPLODE_HEXAGONAL;
    CHECK(xd_addon_init(&skewer, XD_ADDON_SKEWER, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 6));
    bool away = true;
    for (size_t i = 0; i < skewer.count; i++) {
        double middle[3];
        addon_get_piece_middle(&skewer, i, 0.99, middle);
        away = away && middle[2] < -100;
    }
    CHECK(away);
    xd_addon_free(&skewer);
}

static void test_addon_projection(void) {
    xd_addon_options options;
    xd_addon_get_default_options(&options);
    xd_addon addon;
    CHECK(xd_addon_init(&addon, XD_ADDON_GLIDE_3, ADDON_WIDTH, ADDON_HEIGHT, &options, nullptr, 1));
    xd_addon_set_eye(&addon, 200, 150, 1000);
    double x = 0, y = 0, grow = 0;
    xd_addon_project(&addon, (const double[3]){300, 250, 0}, &x, &y, &grow);
    CHECK(x == 300 && y == 250 && grow == 1);
    xd_addon_project(&addon, (const double[3]){300, 250, 500}, &x, &y, &grow);
    CHECK(fabs(x - 400) < 1e-9 && fabs(y - 350) < 1e-9 && fabs(grow - 2) < 1e-9);
    /* Never at the eye. */
    xd_addon_project(&addon, (const double[3]){300, 250, 5000}, &x, &y, &grow);
    CHECK(isfinite(x) && grow <= 5 + 1e-9);
    xd_addon_free(&addon);
}

/* Animations: the Add-On's effects by name, drawn as Zoom without it. */
static void test_addon_fallback(void) {
    CHECK(xd_animation_find_effect("glide1") == XD_ANIMATION_GLIDE_1);
    CHECK(xd_animation_find_effect("leafspread") == XD_ANIMATION_LEAF_SPREAD);
    CHECK(xd_animation_find_effect("bogus") == XD_ANIMATION_NONE);
    CHECK(xd_animation_find_effect(nullptr) == XD_ANIMATION_NONE);
    CHECK(!xd_animation_is_addon(XD_ANIMATION_GLIDE_2) && xd_animation_is_addon(XD_ANIMATION_SKEWER));
    CHECK(xd_animation_get_fallback(XD_ANIMATION_AIRPLANE) == XD_ANIMATION_ZOOM);
    CHECK(xd_animation_get_fallback(XD_ANIMATION_FADE) == XD_ANIMATION_FADE);
    CHECK(xd_animation_get_fallback(XD_ANIMATION_NONE) == XD_ANIMATION_NONE);
    const xd_effect_rect window = {100, 50, 400, 300};
    xd_animation_frame zoom = xd_animation_get_frame(XD_ANIMATION_ZOOM, window, nullptr, 0.4);
    xd_animation_frame fold = xd_animation_get_frame(XD_ANIMATION_FOLD, window, nullptr, 0.4);
    CHECK(memcmp(&zoom, &fold, sizeof(zoom)) == 0);
}

/* The settings: each event lists the effects Compiz offered for it, the
 * Add-On's needing it on (and OpenGL), its options clamped. */
static void test_addon_settings(void) {
    xd_effects_settings *settings = xd_effects_create_settings();
    size_t animations = 0, addon = 0, option = 0;
    CHECK(xd_effects_find(settings, "Animations", &animations));
    CHECK(xd_effects_find(settings, "AnimationsAddOn", &addon));
    const xd_effect_info *info = xd_effects_get_info(settings, addon);
    CHECK(info->needs_opengl && info->category == XD_EFFECT_CATEGORY_EFFECTS);
    const xd_effect_info *animation_info = xd_effects_get_info(settings, animations);
    CHECK(strcmp(xd_effects_get_choice_name(settings, animations, "OpenEffect"), "zoom") == 0);
    /* Airplane: closing and minimizing, not opening. */
    xd_effect_value value = {0};
    CHECK(xd_effects_find_option(animation_info, "OpenEffect", &option));
    CHECK(!xd_effects_parse_value(&animation_info->options[option], "airplane", &value));
    CHECK(xd_effects_parse_value(&animation_info->options[option], "beamup", &value));
    int beam_up = value.choice;
    CHECK(strcmp(xd_effects_get_choice_requirement(&animation_info->options[option], beam_up),
                 "AnimationsAddOn") == 0);
    CHECK(xd_effects_get_choice_requirement(&animation_info->options[option], 2) == nullptr);
    /* Off, or no OpenGL: greyed; on with OpenGL: there. Zoom always. */
    CHECK(!xd_effects_is_choice_available(settings, animations, option, beam_up, true));
    xd_effects_set_enabled(settings, addon, true);
    CHECK(xd_effects_is_choice_available(settings, animations, option, beam_up, true));
    CHECK(!xd_effects_is_choice_available(settings, animations, option, beam_up, false));
    CHECK(xd_effects_is_choice_available(settings, animations, option, 2, false));
    size_t close = 0;
    CHECK(xd_effects_find_option(animation_info, "CloseEffect", &close));
    CHECK(xd_effects_parse_value(&animation_info->options[close], "airplane", &value));

    /* From the file: names per event, the Add-On's values clamped. */
    char *dir = g_dir_make_tmp("xd-addon-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "effects.ini", nullptr);
    CHECK(g_file_set_contents(path,
                              "[Animations]\nEnabled=true\nCloseEffect=skewer\n"
                              "OpenEffect=airplane\n"
                              "[AnimationsAddOn]\nEnabled=true\nFoldGridWidth=99\n"
                              "BeamLife=0.01\nSkewerDirection=InOut\nFoldDirection=Out\n",
                              -1, nullptr));
    xd_effects_load_file(settings, path);
    CHECK(strcmp(xd_effects_get_choice_name(settings, animations, "CloseEffect"), "skewer") == 0);
    CHECK(strcmp(xd_effects_get_choice_name(settings, animations, "OpenEffect"), "zoom") == 0);
    CHECK(xd_effects_get_int(settings, addon, "FoldGridWidth") == 10);
    CHECK(xd_effects_get_double(settings, addon, "BeamLife") == 0.1);
    CHECK(xd_effects_get_choice(settings, addon, "SkewerDirection") == 8);
    CHECK(xd_effects_get_choice(settings, addon, "FoldDirection") == 1);
    CHECK(xd_effects_is_default(settings, addon, 0));
    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
    xd_effects_destroy_settings(settings);
}

static void test_effects_addon(void) {
    test_addon_model_common();
    test_addon_options();
    test_addon_airplane();
    test_addon_beam_up();
    test_addon_dominoes();
    test_addon_fold();
    test_addon_glide_leaf_skewer();
    test_addon_projection();
    test_addon_fallback();
    test_addon_settings();
}
