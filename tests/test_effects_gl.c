/* SPDX-License-Identifier: MIT
 * Unit tests for the OpenGL window effects' maths (Magic Lamp, Burn,
 * Explode, Water): included by test_main.c, which has CHECK.
 */
#include "../src/compositor/effects/burn_model.h"
#include "../src/compositor/effects/explode_model.h"
#include "../src/compositor/effects/magic_lamp_model.h"
#include "../src/compositor/effects/water_model.h"

/* Magic Lamp: the window as it is at the start, narrower near the
 * button than at its top midway, all in the button at the end; and a
 * taskbar on any edge. */
static void test_magic_lamp_model(void) {
    xd_lamp_rect window = {100, 100, 400, 300};
    xd_lamp_rect icon = {300, 740, 160, 24}; /* a bottom taskbar's button */
    CHECK(xd_lamp_get_direction(window, icon) == XD_LAMP_DOWN);
    CHECK(xd_lamp_get_direction(window, (xd_lamp_rect){0, 0, 24, 24}) == XD_LAMP_UP);
    CHECK(xd_lamp_get_direction(window, (xd_lamp_rect){1000, 200, 30, 30}) == XD_LAMP_RIGHT);
    CHECK(xd_lamp_get_direction(window, (xd_lamp_rect){0, 250, 30, 30}) == XD_LAMP_LEFT);

    xd_lamp lamp;
    xd_lamp_init(&lamp, window, icon);
    double x = 0, y = 0;
    xd_lamp_get_point(&lamp, 0, 0, 0, &x, &y);
    CHECK(fabs(x - 100) < 1e-9 && fabs(y - 100) < 1e-9);
    xd_lamp_get_point(&lamp, 1, 1, 0, &x, &y);
    CHECK(fabs(x - 500) < 1e-9 && fabs(y - 400) < 1e-9);
    xd_lamp_get_point(&lamp, 0.5, 0.5, 0, &x, &y);
    CHECK(fabs(x - 300) < 1e-9 && fabs(y - 250) < 1e-9);

    /* Midway: the bottom row is narrower than the top one, and leans
     * toward the button. */
    double top_left = 0, top_right = 0, bottom_left = 0, bottom_right = 0, unused = 0;
    xd_lamp_get_point(&lamp, 0, 0, 0.5, &top_left, &unused);
    xd_lamp_get_point(&lamp, 1, 0, 0.5, &top_right, &unused);
    xd_lamp_get_point(&lamp, 0, 1, 0.5, &bottom_left, &unused);
    xd_lamp_get_point(&lamp, 1, 1, 0.5, &bottom_right, &unused);
    CHECK(bottom_right - bottom_left < (top_right - top_left) * 0.7);
    CHECK(bottom_left > top_left); /* toward the button's left edge (300) */
    /* Rows stay in order, top to bottom. */
    double previous = -1e9;
    bool ordered = true;
    for (int row = 0; row <= 10; row++) {
        xd_lamp_get_point(&lamp, 0.5, row / 10.0, 0.7, &x, &y);
        ordered = ordered && y >= previous;
        previous = y;
    }
    CHECK(ordered);

    /* The end: every point inside the button. */
    bool inside = true;
    for (int corner = 0; corner < 4; corner++) {
        xd_lamp_get_point(&lamp, corner & 1, corner >> 1, 1, &x, &y);
        inside = inside && x >= icon.x - 1e-6 && x <= icon.x + icon.width + 1e-6 &&
                 y >= icon.y - 1e-6 && y <= icon.y + icon.height + 1e-6;
    }
    CHECK(inside);

    /* A taskbar on the left: midway the window's left column is shorter
     * than its right one, and the far (right) edge hasn't moved yet. */
    xd_lamp_rect left_icon = {0, 300, 30, 30};
    xd_lamp_init(&lamp, window, left_icon);
    CHECK(lamp.direction == XD_LAMP_LEFT);
    double left_top = 0, left_bottom = 0, right_top = 0, right_bottom = 0, right_x = 0;
    xd_lamp_get_point(&lamp, 0, 0, 0.3, &unused, &left_top);
    xd_lamp_get_point(&lamp, 0, 1, 0.3, &unused, &left_bottom);
    xd_lamp_get_point(&lamp, 1, 0, 0.3, &right_x, &right_top);
    xd_lamp_get_point(&lamp, 1, 1, 0.3, &unused, &right_bottom);
    CHECK(left_bottom - left_top < right_bottom - right_top);
    CHECK(fabs(right_x - 500) < 1e-6);
    xd_lamp_get_point(&lamp, 1, 1, 1, &x, &y);
    CHECK(x <= 30 + 1e-6 && y >= 300 - 1e-6 && y <= 330 + 1e-6);
}

/* Burn: the front crosses the window from the side it starts on, what's
 * behind it goes, and sparks rise, slow and die. */
static void test_burn_model(void) {
    CHECK(xd_burn_get_front(0) <= 0);
    CHECK(xd_burn_get_char(0, xd_burn_get_front(0)) == 0); /* nothing burnt yet */
    CHECK(xd_burn_get_char(1, xd_burn_get_front(1)) > 1 - 1e-9); /* everything gone */
    /* Burning down: midway the top is gone, the bottom untouched. */
    double front = xd_burn_get_front(0.5);
    CHECK(xd_burn_get_char(xd_burn_get_along(XD_BURN_DOWN, 0.5, 0.1), front) == 1);
    CHECK(xd_burn_get_char(xd_burn_get_along(XD_BURN_DOWN, 0.5, 0.9), front) == 0);
    double charring = xd_burn_get_char(xd_burn_get_along(XD_BURN_DOWN, 0.5, front - 0.05), front);
    CHECK(charring > 0 && charring < 1);
    /* Burning left: it starts at the right edge. */
    CHECK(xd_burn_get_char(xd_burn_get_along(XD_BURN_LEFT, 0.9, 0.5), front) == 1);
    CHECK(xd_burn_get_char(xd_burn_get_along(XD_BURN_LEFT, 0.1, 0.5), front) == 0);
    CHECK(fabs(xd_burn_get_along(XD_BURN_UP, 0.3, 1.0)) < 1e-12);
    double across = 0, down = 0;
    xd_burn_get_front_point(XD_BURN_RIGHT, 0.25, 0.75, &across, &down);
    CHECK(fabs(across - 0.25) < 1e-12 && fabs(down - 0.75) < 1e-12);
    /* The front moves on as time does. */
    bool forward = true;
    for (int step = 1; step <= 10; step++)
        forward = forward && xd_burn_get_front(step / 10.0) > xd_burn_get_front((step - 1) / 10.0);
    CHECK(forward);

    xd_burn_particles particles;
    xd_burn_init_particles(&particles, 2, 42);
    uint64_t seed = 7;
    double first = xd_burn_get_random(&seed), second = xd_burn_get_random(&seed);
    CHECK(first >= 0 && first < 1 && second >= 0 && second < 1 && first != second);
    xd_burn_particle spark = {.x = 100, .y = 200, .speed_x = 10, .speed_y = 0, .life = 0.5,
                              .lifetime = 0.5};
    CHECK(xd_burn_add_particle(&particles, &spark));
    spark.life = spark.lifetime = 2.0;
    CHECK(xd_burn_add_particle(&particles, &spark));
    CHECK(!xd_burn_add_particle(&particles, &spark)); /* full */
    xd_burn_step_particles(&particles, 0.1, 0.5);
    CHECK(particles.count == 2);
    CHECK(particles.items[0].x > 100 && particles.items[0].speed_x < 10);
    CHECK(particles.items[0].speed_y < 0); /* rising */
    xd_burn_step_particles(&particles, 0.1, 0.5);
    CHECK(particles.items[1].y < 200);
    xd_burn_step_particles(&particles, 0.4, 0.5); /* the first's life is over */
    CHECK(particles.count == 1 && particles.items[0].lifetime == 2.0);
    xd_burn_free_particles(&particles);
}

/* The pieces' total area, which a tessellation of the whole window has as
 * the window's. */
static double explode_total_area(const xd_explode *explode) {
    double total = 0;
    for (size_t i = 0; i < explode->count; i++) {
        const xd_explode_piece *piece = &explode->pieces[i];
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

/* Explode: each tessellation covers the window exactly once with pieces
 * inside it; at the start they're where they were, midway they've flown
 * out (some past the window's box) and are fading. */
static void test_explode_model(void) {
    xd_explode explode;
    CHECK(xd_explode_init(&explode, XD_EXPLODE_RECTANGULAR, 400, 300, 13, 10, 10, 4, 1));
    CHECK(explode.count == 130 && explode.pieces[0].corner_count == 4);
    CHECK(fabs(explode_total_area(&explode) - 400 * 300) < 1e-6);
    /* At the start every corner is where it was. */
    bool still = true;
    for (size_t i = 0; i < explode.count; i++) {
        const xd_explode_piece *piece = &explode.pieces[i];
        for (int corner = 0; corner < piece->corner_count; corner++) {
            double x = 0, y = 0;
            xd_explode_get_corner(&explode, piece, corner, 0, &x, &y);
            still = still && fabs(x - piece->corners[corner][0]) < 1e-9 &&
                    fabs(y - piece->corners[corner][1]) < 1e-9;
        }
    }
    CHECK(still);
    /* Midway: pieces outside the box, each farther from the middle than
     * it was (on average), and fading but not gone. */
    size_t outside = 0;
    double before = 0, after = 0;
    for (size_t i = 0; i < explode.count; i++) {
        const xd_explode_piece *piece = &explode.pieces[i];
        double x = 0, y = 0;
        xd_explode_get_corner(&explode, piece, 0, 0.5, &x, &y);
        if (x < 0 || x > 400 || y < 0 || y > 300)
            outside++;
        before += hypot(piece->corners[0][0] - 200, piece->corners[0][1] - 150);
        after += hypot(x - 200, y - 150);
    }
    CHECK(outside > explode.count / 4);
    CHECK(after > before * 1.3);
    CHECK(xd_explode_get_alpha(0) == 1 && xd_explode_get_alpha(1) == 0);
    CHECK(xd_explode_get_alpha(0.7) > 0 && xd_explode_get_alpha(0.7) < 1);
    double normal[3];
    xd_explode_get_normal(&explode.pieces[0], 0, normal);
    CHECK(fabs(normal[2] - 1) < 1e-9);
    xd_explode_get_normal(&explode.pieces[0], 0.5, normal);
    CHECK(fabs(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] - 1) < 1e-9);
    xd_explode_free(&explode);

    /* Hexagons and glass: clipped to the window, covering it, convex
     * enough to fan. */
    const xd_explode_tessellation others[] = {XD_EXPLODE_HEXAGONAL, XD_EXPLODE_GLASS};
    for (size_t kind = 0; kind < 2; kind++) {
        CHECK(xd_explode_init(&explode, others[kind], 400, 300, 8, 6, 10, 4, 99));
        CHECK(fabs(explode_total_area(&explode) - 400 * 300) < 1e-3);
        bool inside = true;
        for (size_t i = 0; i < explode.count; i++) {
            const xd_explode_piece *piece = &explode.pieces[i];
            inside = inside && piece->corner_count >= 3 &&
                     piece->corner_count <= XD_EXPLODE_MAX_CORNERS;
            for (int corner = 0; corner < piece->corner_count; corner++)
                inside = inside && piece->corners[corner][0] >= -1e-9 &&
                         piece->corners[corner][0] <= 400 + 1e-9 &&
                         piece->corners[corner][1] >= -1e-9 &&
                         piece->corners[corner][1] <= 300 + 1e-9;
        }
        CHECK(inside);
        xd_explode_free(&explode);
    }
    CHECK(xd_explode_init(&explode, XD_EXPLODE_GLASS, 400, 300, 1, 1, 10, 4, 5));
    CHECK(explode.count == 40); /* every shard reaches into the window */
    xd_explode_free(&explode);
    CHECK(!xd_explode_init(&explode, XD_EXPLODE_RECTANGULAR, 0, 300, 4, 4, 3, 1, 1));
    xd_explode_free(&explode);
}

/* Water: a drop's ring spreads out step by step, symmetrically, and dies
 * down till the water is flat again; still water stays still. */
static void test_water_model(void) {
    xd_water water;
    CHECK(!xd_water_init(&water, 2, 40));
    CHECK(xd_water_init(&water, 64, 48));
    CHECK(!xd_water_step(&water)); /* flat */
    xd_water_add_drop(&water, 32, 24, 3, 1);
    CHECK(xd_water_get_height(&water, 32, 24) < -0.9f);
    CHECK(xd_water_get_height(&water, 40, 24) == 0);
    double across = 0, down = 0;
    xd_water_get_slope(&water, 33, 24, &across, &down);
    CHECK(across > 0 && fabs(down) < 1e-6); /* rising away from the dent */
    for (int step = 0; step < 10; step++)
        CHECK(xd_water_step(&water));
    /* The ring has reached cells the drop never touched, the same way
     * every way. */
    float east = xd_water_get_height(&water, 40, 24), west = xd_water_get_height(&water, 24, 24);
    float south = xd_water_get_height(&water, 32, 32), north = xd_water_get_height(&water, 32, 16);
    CHECK(fabsf(east) > 1e-4f);
    CHECK(fabsf(east - west) < 1e-5f && fabsf(east - south) < 1e-5f &&
          fabsf(east - north) < 1e-5f);
    int steps = 0;
    while (xd_water_step(&water) && steps < 5000)
        steps++;
    CHECK(steps > 20 && steps < 5000);
    bool flat = true;
    for (int row = 0; row < water.rows; row++) {
        for (int column = 0; column < water.columns; column++)
            flat = flat && xd_water_get_height(&water, column, row) == 0;
    }
    CHECK(flat);
    /* A line of drops: dents all along it. */
    xd_water_add_line(&water, 10, 10, 30, 10, 2, 0.5);
    CHECK(xd_water_get_height(&water, 10, 10) < 0 && xd_water_get_height(&water, 20, 10) < 0 &&
          xd_water_get_height(&water, 30, 10) < 0 && xd_water_get_height(&water, 20, 20) == 0);
    xd_water_free(&water);
}

/* Burn's smoke: puffs leave the front, rise, swell and fade, all gone
 * within their life once the fire is; the same seed gives the same
 * smoke. */
static void burn_run_smoke(uint64_t seed, xd_burn_particles *smoke) {
    const xd_burn_smoke_options options = {.count = 50, .size = 8, .life = 0.7};
    const double box[4] = {100, 100, 400, 300};
    xd_burn_init_particles(smoke, (size_t)options.count, seed);
    double owed = 0;
    for (int frame = 0; frame < 30; frame++) {
        xd_burn_step_smoke(smoke, 1.0 / 60, 0.5);
        xd_burn_spawn_smoke(smoke, &options, XD_BURN_DOWN, xd_burn_get_front(frame / 60.0), box,
                            1.0 / 60, &owed);
    }
}

static void test_burn_smoke_model(void) {
    xd_burn_particles smoke, again, other;
    burn_run_smoke(42, &smoke);
    burn_run_smoke(42, &again);
    burn_run_smoke(43, &other);
    /* Half a second at 50 / 0.7 s a second: about 35, none past room. */
    CHECK(smoke.count > 20 && smoke.count <= 50);
    CHECK(smoke.count == again.count &&
          memcmp(smoke.items, again.items, smoke.count * sizeof(*smoke.items)) == 0);
    CHECK(other.count > 0 && memcmp(smoke.items, other.items, sizeof(*smoke.items)) != 0);
    bool near_front = true, grey = true;
    for (size_t i = 0; i < smoke.count; i++) {
        const xd_burn_particle *puff = &smoke.items[i];
        /* Born along the front (from the top edge to a little past halfway
         * down), risen a little since. */
        near_front = near_front && puff->x > 80 && puff->x < 520 && puff->y > 0 &&
                     puff->y < 100 + 300 * 0.6;
        grey = grey && puff->red == puff->green && puff->green == puff->blue &&
               puff->red > 0.2f && puff->red < 0.6f && puff->lifetime <= 0.7;
    }
    CHECK(near_front && grey);
    /* Each rises, and swells as it ages; it fades in, then out. */
    xd_burn_particles alone;
    xd_burn_init_particles(&alone, 1, 1);
    CHECK(xd_burn_add_particle(&alone, &smoke.items[smoke.count - 1]));
    xd_burn_step_smoke(&alone, 0.05, 0.5);
    CHECK(alone.count == 1 && alone.items[0].y < smoke.items[smoke.count - 1].y);
    xd_burn_free_particles(&alone);
    xd_burn_particle puff = {.life = 1.0, .lifetime = 1.0};
    double radius = 0, opacity = 0;
    xd_burn_get_puff(&puff, 8, &radius, &opacity);
    CHECK(radius == 8 && opacity == 0);
    puff.life = 0.5;
    double half_radius = 0, half_opacity = 0;
    xd_burn_get_puff(&puff, 8, &half_radius, &half_opacity);
    CHECK(half_radius > 8 && half_radius < 24 && half_opacity > 0.4 && half_opacity <= 0.5);
    puff.life = 0;
    xd_burn_get_puff(&puff, 8, &radius, &opacity);
    CHECK(radius == 24 && opacity == 0);
    /* The fire out, the last puff is gone within its life. */
    for (int frame = 0; frame < 42; frame++)
        xd_burn_step_smoke(&smoke, 1.0 / 60, 0.5);
    CHECK(smoke.count == 0);
    /* No room, no puffs, and nothing owed for later. */
    xd_burn_particles none;
    xd_burn_init_particles(&none, 0, 1);
    double owed = 0;
    const xd_burn_smoke_options options = {.count = 100, .size = 8, .life = 0.7};
    xd_burn_spawn_smoke(&none, &options, XD_BURN_UP, 0.5, (const double[4]){0, 0, 10, 10}, 1,
                        &owed);
    CHECK(none.count == 0 && owed == 0);
    xd_burn_free_particles(&none);
    xd_burn_free_particles(&smoke);
    xd_burn_free_particles(&again);
    xd_burn_free_particles(&other);

    /* Its options: off by default as Compiz's, and clamped to the page's
     * ranges; what can't be read keeps the default. */
    xd_effects_settings *settings = xd_effects_create_settings();
    size_t burn = 0;
    CHECK(xd_effects_find(settings, "Burn", &burn));
    CHECK(!xd_effects_get_bool(settings, burn, "Smoke"));
    CHECK(xd_effects_get_int(settings, burn, "SmokeCount") == 100);
    CHECK(xd_effects_get_double(settings, burn, "SmokeSize") == 10.0);
    g_autofree char *dir = g_dir_make_tmp("xd-smoke-XXXXXX", nullptr);
    g_autofree char *path = g_build_filename(dir, "effects.ini", nullptr);
    CHECK(g_file_set_contents(path, "[Burn]\nSmoke=true\nSmokeCount=99999\nSmokeSize=-3\n", -1,
                              nullptr));
    xd_effects_load_file(settings, path);
    CHECK(xd_effects_get_bool(settings, burn, "Smoke"));
    CHECK(xd_effects_get_int(settings, burn, "SmokeCount") == 1000);
    CHECK(xd_effects_get_double(settings, burn, "SmokeSize") == 1.0);
    CHECK(g_file_set_contents(path, "[Burn]\nSmoke=perhaps\nSmokeCount=lots\n", -1, nullptr));
    xd_effects_load_file(settings, path);
    CHECK(!xd_effects_get_bool(settings, burn, "Smoke"));
    CHECK(xd_effects_get_int(settings, burn, "SmokeCount") == 100);
    g_remove(path);
    g_rmdir(dir);
    xd_effects_destroy_settings(settings);
}

static void test_effects_gl(void) {
    test_magic_lamp_model();
    test_burn_model();
    test_burn_smoke_model();
    test_explode_model();
    test_water_model();
}
