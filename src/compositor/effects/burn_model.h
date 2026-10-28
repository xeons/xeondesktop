/* SPDX-License-Identifier: MIT
 * Burn's maths: the flame front crossing a closing window, and the fire
 * and smoke particles it gives off. Pure, no wlroots or GL, so it's
 * unit-tested; burn.c's shader draws the front the same way (with some noise).
 */
#ifndef XD_COMPOSITOR_EFFECTS_BURN_MODEL_H
#define XD_COMPOSITOR_EFFECTS_BURN_MODEL_H

#include <stddef.h>
#include <stdint.h>

/* The way the front moves across the window. */
typedef enum xd_burn_direction : uint8_t {
    XD_BURN_DOWN, /* from the top edge to the bottom one */
    XD_BURN_UP,
    XD_BURN_LEFT,
    XD_BURN_RIGHT,
} xd_burn_direction;

/* How deep the charred band behind the front is, as a share of the
 * window. */
double xd_burn_get_edge(void);
/* How far the shader's noise moves the front either way, as a share of
 * the window. */
double xd_burn_get_ragged_reach(void);
/* `along` = axis[0] * across + axis[1] * down + axis[2]: 0 where the
 * fire starts, 1 where it ends (across and down 0..1 over the window). */
void xd_burn_get_axis(xd_burn_direction direction, double axis[3]);
double xd_burn_get_along(xd_burn_direction direction, double across, double down);
/* Where the front is along at `progress` (0..1): nothing is burnt at 0,
 * everything at 1, however ragged the front (so a window burning
 * backwards ends exactly as itself). */
double xd_burn_get_front(double progress);
/* How burnt a point `along` is with the front at `front`: 0 untouched,
 * 0..1 charring, 1 gone. */
double xd_burn_get_char(double along, double front);
/* The point `share` (0..1) of the way across the front, over the window. */
void xd_burn_get_front_point(xd_burn_direction direction, double front, double share,
                             double *across, double *down);

/* ---- fire particles ------------------------------------------------------------- */

typedef struct xd_burn_particle {
    double x, y;             /* layout units */
    double speed_x, speed_y; /* layout units a second */
    double life, lifetime;   /* seconds left, of how many */
    float red, green, blue;
} xd_burn_particle;

typedef struct xd_burn_particles {
    xd_burn_particle *items;
    size_t count, capacity;
    uint64_t seed; /* xd_burn_get_random's */
} xd_burn_particles;

void xd_burn_init_particles(xd_burn_particles *particles, size_t capacity, uint64_t seed);
void xd_burn_free_particles(xd_burn_particles *particles);
/* 0..1, the next of a fixed sequence from `seed`. */
double xd_burn_get_random(uint64_t *seed);
/* False when there's no room. */
bool xd_burn_add_particle(xd_burn_particles *particles, const xd_burn_particle *particle);
/* Moves each by `seconds`: it rises (fire goes up), slows by `slowdown`
 * a second, and ages; dead ones go. */
void xd_burn_step_particles(xd_burn_particles *particles, double seconds, double slowdown);

/* ---- smoke ---------------------------------------------------------------------- */

/* Grey puffs rising from the front, drawn behind the sparks: they drift
 * up, slow, grow and fade, each within `life` seconds, so the last are
 * gone that long after the fire is. Each is an xd_burn_particle whose
 * colour is its grey. */
typedef struct xd_burn_smoke_options {
    int count;   /* about how many alight at once */
    double size; /* a new puff's radius, layout units; it grows to 3 times that */
    double life; /* the longest a puff lasts, seconds */
} xd_burn_smoke_options;

/* Gives off the puffs owed for `seconds` of burning along the front at
 * `front` (xd_burn_get_front's) of the window at `box` (x, y, width and
 * height, layout units); `owed` carries the fraction of a puff over to
 * the next call. */
void xd_burn_spawn_smoke(xd_burn_particles *smoke, const xd_burn_smoke_options *options,
                         xd_burn_direction direction, double front, const double box[4],
                         double seconds, double *owed);
/* Moves each by `seconds`: it drifts up more gently than a spark, slows
 * half as fast (`slowdown` is the sparks'), and ages; dead ones go. */
void xd_burn_step_smoke(xd_burn_particles *smoke, double seconds, double slowdown);
/* A puff's radius as it is now (it swells as it ages), and its opacity
 * (0..1: fading in as it's born and out as it dies). */
void xd_burn_get_puff(const xd_burn_particle *puff, double size, double *radius,
                      double *opacity);

#endif
