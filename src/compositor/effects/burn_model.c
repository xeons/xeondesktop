/* SPDX-License-Identifier: MIT
 * Burn's maths (burn_model.h).
 */
#include "burn_model.h"

#include <math.h>

#include "../../core/alloc.h"

static constexpr double CHAR_EDGE = 0.15;
static constexpr double RAGGED_REACH = 0.04;
/* How far before the start and past the end the front goes: past where
 * the noise reaches, so nothing is touched at the start (a window burning
 * backwards ends exactly as itself) and nothing left at the end. */
static constexpr double FRONT_MARGIN = RAGGED_REACH + 0.01;
/* Fire rises: layout units a second, each second. */
static constexpr double RISE = 60.0;
/* Smoke rises more gently, and slows half as fast as the sparks (as
 * Compiz's did). */
static constexpr double SMOKE_RISE = 40.0;
/* A puff swells to this many times its first radius, and fades in over
 * this share of its life. */
static constexpr double SMOKE_GROWTH = 3.0;
static constexpr double SMOKE_FADE_IN = 0.15;

double xd_burn_get_edge(void) {
    return CHAR_EDGE;
}

double xd_burn_get_ragged_reach(void) {
    return RAGGED_REACH;
}

void xd_burn_get_axis(xd_burn_direction direction, double axis[3]) {
    double across = 0, down = 0, offset = 0;
    switch (direction) {
    case XD_BURN_DOWN:
        down = 1;
        break;
    case XD_BURN_UP:
        down = -1;
        offset = 1;
        break;
    case XD_BURN_RIGHT:
        across = 1;
        break;
    case XD_BURN_LEFT:
        across = -1;
        offset = 1;
        break;
    }
    axis[0] = across;
    axis[1] = down;
    axis[2] = offset;
}

double xd_burn_get_along(xd_burn_direction direction, double across, double down) {
    double axis[3];
    xd_burn_get_axis(direction, axis);
    return axis[0] * across + axis[1] * down + axis[2];
}

double xd_burn_get_front(double progress) {
    progress = progress < 0 ? 0 : progress > 1 ? 1 : progress;
    return progress * (1 + CHAR_EDGE + 2 * FRONT_MARGIN) - FRONT_MARGIN;
}

double xd_burn_get_char(double along, double front) {
    double burnt = (front - along) / CHAR_EDGE;
    return burnt < 0 ? 0 : burnt > 1 ? 1 : burnt;
}

void xd_burn_get_front_point(xd_burn_direction direction, double front, double share,
                             double *across, double *down) {
    front = front < 0 ? 0 : front > 1 ? 1 : front;
    switch (direction) {
    case XD_BURN_DOWN:
        *across = share;
        *down = front;
        break;
    case XD_BURN_UP:
        *across = share;
        *down = 1 - front;
        break;
    case XD_BURN_RIGHT:
        *across = front;
        *down = share;
        break;
    case XD_BURN_LEFT:
        *across = 1 - front;
        *down = share;
        break;
    }
}

/* ---- fire particles ------------------------------------------------------------- */

void xd_burn_init_particles(xd_burn_particles *particles, size_t capacity, uint64_t seed) {
    *particles = (xd_burn_particles){0};
    particles->capacity = capacity;
    particles->items = capacity > 0 ? XD_NEW_ARRAY(xd_burn_particle, capacity) : nullptr;
    particles->seed = seed != 0 ? seed : 0x9E3779B97F4A7C15ULL;
}

void xd_burn_free_particles(xd_burn_particles *particles) {
    xd_free(particles->items);
    *particles = (xd_burn_particles){0};
}

double xd_burn_get_random(uint64_t *seed) {
    /* xorshift64*: plenty for where sparks go. */
    uint64_t value = *seed;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *seed = value;
    return (double)((value * 0x2545F4914F6CDD1DULL) >> 11) / 9007199254740992.0;
}

bool xd_burn_add_particle(xd_burn_particles *particles, const xd_burn_particle *particle) {
    if (particles->count >= particles->capacity)
        return false;
    particles->items[particles->count++] = *particle;
    return true;
}

/* Sparks and puffs alike: each ages and moves, rising by `rise` layout
 * units a second each second and slowing by `slowdown` a second. */
static void burn_step(xd_burn_particles *particles, double seconds, double slowdown,
                      double rise) {
    double keep = exp(-slowdown * seconds);
    size_t kept = 0;
    for (size_t i = 0; i < particles->count; i++) {
        xd_burn_particle particle = particles->items[i];
        particle.life -= seconds;
        if (particle.life <= 0)
            continue;
        particle.x += particle.speed_x * seconds;
        particle.y += particle.speed_y * seconds;
        particle.speed_x *= keep;
        particle.speed_y = particle.speed_y * keep - rise * seconds;
        particles->items[kept++] = particle;
    }
    particles->count = kept;
}

void xd_burn_step_particles(xd_burn_particles *particles, double seconds, double slowdown) {
    burn_step(particles, seconds, slowdown, RISE);
}

/* ---- smoke ---------------------------------------------------------------------- */

void xd_burn_spawn_smoke(xd_burn_particles *smoke, const xd_burn_smoke_options *options,
                         xd_burn_direction direction, double front, const double box[4],
                         double seconds, double *owed) {
    double life = options->life > 0 ? options->life : 0.1;
    /* Enough a second to keep about `count` alight. */
    *owed += (double)XD_MAX(options->count, 0) / life * seconds;
    /* From just behind the front, where the window has already burnt. */
    double from = front - CHAR_EDGE * 0.4;
    while (*owed >= 1) {
        *owed -= 1;
        uint64_t *seed = &smoke->seed;
        double across = 0, down = 0;
        xd_burn_get_front_point(direction, from, xd_burn_get_random(seed), &across, &down);
        float grey = (float)(0.25 + 0.25 * xd_burn_get_random(seed));
        xd_burn_particle puff = {
            .x = box[0] + across * box[2] + (xd_burn_get_random(seed) - 0.5) * 16,
            .y = box[1] + down * box[3] + (xd_burn_get_random(seed) - 0.5) * 16,
            .speed_x = (xd_burn_get_random(seed) - 0.5) * 30,
            .speed_y = -40 - xd_burn_get_random(seed) * 50,
            .lifetime = life * (0.6 + 0.4 * xd_burn_get_random(seed)),
            .red = grey,
            .green = grey,
            .blue = grey,
        };
        puff.life = puff.lifetime;
        if (!xd_burn_add_particle(smoke, &puff)) {
            *owed = 0;
            break;
        }
    }
}

void xd_burn_step_smoke(xd_burn_particles *smoke, double seconds, double slowdown) {
    burn_step(smoke, seconds, slowdown * 0.5, SMOKE_RISE);
}

void xd_burn_get_puff(const xd_burn_particle *puff, double size, double *radius,
                      double *opacity) {
    double age = puff->lifetime > 0 ? 1 - puff->life / puff->lifetime : 1;
    age = age < 0 ? 0 : age > 1 ? 1 : age;
    *radius = size * (1 + (SMOKE_GROWTH - 1) * age);
    double fading_in = age < SMOKE_FADE_IN ? age / SMOKE_FADE_IN : 1;
    *opacity = fading_in * (1 - age);
}
