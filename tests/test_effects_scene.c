/* SPDX-License-Identifier: MIT
 * Unit tests of the scene-graph Desktop Effects' maths (Scale, Ring
 * Switcher, Animations, Enhanced Zoom), included by test_main.c for its
 * CHECK.
 */
#include "../src/compositor/effects/scale_model.h"

static bool rects_overlap(xd_effect_rect left, xd_effect_rect right) {
    return left.x < right.x + right.width && right.x < left.x + left.width &&
           left.y < right.y + right.height && right.y < left.y + left.height;
}

/* Scale: every window gets a thumbnail inside the area, none above its
 * size, none overlapping, the spacing kept. */
static void test_scale_layout(void) {
    const xd_effect_rect area = {0, 0, 1024, 728};
    const xd_effect_rect windows[] = {
        {10, 10, 600, 400}, {300, 200, 640, 480}, {50, 300, 300, 200},
        {500, 50, 200, 600}, {0, 0, 1024, 728},
    };
    for (size_t count = 1; count <= XD_ARRAY_LEN(windows); count++) {
        xd_scale_slot slots[XD_ARRAY_LEN(windows)];
        xd_scale_layout(windows, count, area, 10, slots);
        xd_effect_rect placed[XD_ARRAY_LEN(windows)];
        for (size_t i = 0; i < count; i++) {
            placed[i] = (xd_effect_rect){slots[i].x, slots[i].y, windows[i].width * slots[i].scale,
                                         windows[i].height * slots[i].scale};
            CHECK(slots[i].scale > 0 && slots[i].scale <= 1.0);
            CHECK(placed[i].x >= area.x + 10 - 1e-9 && placed[i].y >= area.y + 10 - 1e-9);
            CHECK(placed[i].x + placed[i].width <= area.x + area.width - 10 + 1e-9);
            CHECK(placed[i].y + placed[i].height <= area.y + area.height - 10 + 1e-9);
        }
        for (size_t i = 0; i < count; i++)
            for (size_t j = i + 1; j < count; j++)
                CHECK(!rects_overlap(placed[i], placed[j]));
    }
    /* One small window stays its own size, centred. */
    xd_scale_slot slot;
    xd_scale_layout(&windows[2], 1, area, 10, &slot);
    CHECK(slot.scale == 1.0);
    CHECK(fabs(slot.x - (1024 - 300) / 2.0) < 1e-9 && fabs(slot.y - (728 - 200) / 2.0) < 1e-9);
    /* Two windows share one row; the one on the left takes the left slot. */
    const xd_effect_rect pair[] = {{600, 100, 300, 200}, {20, 100, 300, 200}};
    xd_scale_slot pair_slots[2];
    xd_scale_layout(pair, 2, area, 10, pair_slots);
    CHECK(pair_slots[1].x < pair_slots[0].x);
    CHECK(fabs(pair_slots[0].y - pair_slots[1].y) < 1e-9);
    /* Three need two rows. */
    xd_scale_slot three[3];
    xd_scale_layout(windows, 3, area, 10, three);
    CHECK(fabs(three[0].y - three[1].y) > 50 || fabs(three[0].y - three[2].y) > 50);

    /* What's under the pointer, and the arrow keys' neighbours. */
    xd_scale_motion motions[2] = {{.x = pair_slots[0].x, .y = pair_slots[0].y, .scale = 1},
                                  {.x = pair_slots[1].x, .y = pair_slots[1].y, .scale = 1}};
    motions[0].scale = pair_slots[0].scale;
    motions[1].scale = pair_slots[1].scale;
    CHECK(xd_scale_find_thumbnail_at(pair, motions, 2, pair_slots[0].x + 5,
                                     pair_slots[0].y + 5) == 0);
    CHECK(xd_scale_find_thumbnail_at(pair, motions, 2, pair_slots[1].x + 5,
                                     pair_slots[1].y + 5) == 1);
    CHECK(xd_scale_find_thumbnail_at(pair, motions, 2, 2, 2) == -1);
    CHECK(xd_scale_find_neighbour(pair, pair_slots, 2, 1, 1, 0) == 0);
    CHECK(xd_scale_find_neighbour(pair, pair_slots, 2, 0, -1, 0) == 1);
    CHECK(xd_scale_find_neighbour(pair, pair_slots, 2, 0, 1, 0) == 0); /* none further */
    CHECK(xd_scale_find_neighbour(pair, pair_slots, 2, 0, 0, 1) == 0);
}

/* Scale's glide: it gets there, and stops exactly there, in about the
 * time Compiz took (well under two seconds at the defaults), smoothly. */
static void test_scale_motion(void) {
    xd_scale_motion motion = {.x = 100, .y = 100, .scale = 1.0};
    xd_scale_slot target = {.x = 600, .y = 400, .scale = 0.4};
    int frames = 0;
    double furthest_x = motion.x, smallest = motion.scale;
    bool moving = true;
    for (; moving && frames < 1000; frames++) {
        moving = xd_scale_step_motion(&motion, target, 16.0, 1.5, 1.2);
        furthest_x = XD_MAX(furthest_x, motion.x);
        smallest = XD_MIN(smallest, motion.scale);
    }
    CHECK(!moving && frames > 10 && frames < 150);
    /* It may swing past a little, as Compiz's did, but not far. */
    CHECK(furthest_x < 600 + 50 && smallest > 0.4 - 0.1);
    CHECK(motion.x == 600 && motion.y == 400 && motion.scale == 0.4);
    CHECK(!xd_scale_step_motion(&motion, target, 16.0, 1.5, 1.2));
    /* Faster speed, fewer frames; a long stall doesn't overshoot wildly. */
    motion = (xd_scale_motion){.x = 100, .y = 100, .scale = 1.0};
    int fast = 0;
    for (moving = true; moving && fast < 1000; fast++)
        moving = xd_scale_step_motion(&motion, target, 16.0, 6.0, 1.2);
    CHECK(fast < frames);
    motion = (xd_scale_motion){.x = 100, .y = 100, .scale = 1.0};
    xd_scale_step_motion(&motion, target, 100.0, 1.5, 1.2);
    CHECK(motion.x > 100 && motion.x < 700 && isfinite(motion.scale));
}

static void test_effects_scene(void) {
    test_scale_layout();
    test_scale_motion();
}
