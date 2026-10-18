/* SPDX-License-Identifier: MIT
 * What the screen savers share inside libxdsaver: the random numbers and
 * the two "3D" savers, each in a file of its own.
 */
#ifndef XD_SAVER_SAVER_IMPL_H
#define XD_SAVER_SAVER_IMPL_H

#include <cairo.h>
#include <stdint.h>

#include "../settings/settings.h"

/* xorshift64*, in [0, 1): plenty for animation, and the same sequence
 * from the same seed (the tests rely on it). */
static inline double xd_saver_rand(uint64_t *state) {
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return (double)((*state * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}

static inline double xd_saver_between(uint64_t *state, double lo, double hi) {
    return lo + (hi - lo) * xd_saver_rand(state);
}

/* 3D Pipes (pipes.c). `rng` is the saver's and outlives it. */
typedef struct saver_pipes saver_pipes;
saver_pipes *saver_pipes_create(const xd_saver_settings *o, int w, int h, uint64_t *rng);
void saver_pipes_frame(saver_pipes *p, cairo_t *cr, double dt);
void saver_pipes_destroy(saver_pipes *p);

/* 3D Text (text3d.c). */
typedef struct saver_text3d saver_text3d;
saver_text3d *saver_text3d_create(const xd_saver_settings *o, int w, int h, uint64_t *rng);
void saver_text3d_frame(saver_text3d *t, cairo_t *cr, double dt);
void saver_text3d_destroy(saver_text3d *t);

#endif
