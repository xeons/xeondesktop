/* SPDX-License-Identifier: MIT
 * Burn's maths (burn_model.h).
 */
#include "burn_model.h"

#include <math.h>

#include "../../core/alloc.h"

static constexpr double CHAR_EDGE = 0.15;
/* Fire rises: layout units a second, each second. */
static constexpr double RISE = 60.0;

double xd_burn_get_edge(void) {
    return CHAR_EDGE;
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
    return progress * (1 + CHAR_EDGE);
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

void xd_burn_step_particles(xd_burn_particles *particles, double seconds, double slowdown) {
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
        particle.speed_y = particle.speed_y * keep - RISE * seconds;
        particles->items[kept++] = particle;
    }
    particles->count = kept;
}
