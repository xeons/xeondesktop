/* SPDX-License-Identifier: MIT
 * Unit tests for the OpenGL window effects' maths (Magic Lamp, Burn,
 * Explode, Water): included by test_main.c, which has CHECK.
 */
#include "../src/compositor/effects/burn_model.h"
#include "../src/compositor/effects/magic_lamp_model.h"

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

static void test_effects_gl(void) {
    test_magic_lamp_model();
    test_burn_model();
}
