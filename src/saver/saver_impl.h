/* SPDX-License-Identifier: MIT
 * What the screen savers share inside libxdsaver: the random numbers and
 * the "3D" savers and Digital Rain, each in a file of its own.
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
saver_pipes *saver_pipes_create(const xd_saver_settings *saver_settings, int width, int height,
                                uint64_t *rng);
void saver_pipes_frame(saver_pipes *pipes, cairo_t *cairo, double dt);
void saver_pipes_destroy(saver_pipes *pipes);

/* 3D Maze (maze.c). */
typedef struct saver_maze saver_maze;
saver_maze *saver_maze_create(const xd_saver_settings *saver_settings, int width, int height,
                              uint64_t *rng);
void saver_maze_frame(saver_maze *maze, cairo_t *cairo, double dt);
void saver_maze_destroy(saver_maze *maze);

/* 3D Flower Box (flower.c). */
typedef struct saver_flower saver_flower;
saver_flower *saver_flower_create(const xd_saver_settings *saver_settings, int width, int height,
                                  uint64_t *rng);
void saver_flower_frame(saver_flower *flower, cairo_t *cairo, double dt);
void saver_flower_destroy(saver_flower *flower);

/* 3D Text (text3d.c). */
typedef struct saver_text3d saver_text3d;
saver_text3d *saver_text3d_create(const xd_saver_settings *saver_settings, int width, int height,
                                  uint64_t *rng);
void saver_text3d_frame(saver_text3d *text3d, cairo_t *cairo, double dt);
void saver_text3d_destroy(saver_text3d *text3d);

/* Digital Rain (rain.c; its column model is in rain.h). Its glyphs are
 * sized by `scale`, as text is. */
typedef struct saver_rain saver_rain;
saver_rain *saver_rain_create(const xd_saver_settings *saver_settings, int width, int height,
                              double scale, uint64_t *rng);
void saver_rain_frame(saver_rain *rain, cairo_t *cairo, double dt);
void saver_rain_destroy(saver_rain *rain);

#endif
