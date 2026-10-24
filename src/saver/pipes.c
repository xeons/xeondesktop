/* SPDX-License-Identifier: MIT
 * 3D Pipes, after Windows' OpenGL saver: pipes grow cell by cell through a
 * 3D grid seen in perspective, turning at random through elbows or ball
 * joints, until the grid is crowded; then the screen clears and it starts
 * again.
 *
 * The pieces are real meshes (cylinders, quarter-torus elbows, spheres)
 * drawn by raster3d.c with a depth buffer and lit per pixel, one light
 * from the upper left with a white highlight, as OpenGL lit the
 * original. The picture only grows, so it's kept with its depth buffer:
 * each new piece is drawn into it once, and a frame only copies it.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../core/alloc.h"
#include "raster3d.h"
#include "saver_impl.h"

enum { MULTIPLE_PIPES = 5, DIRS = 6, SEGMENTS = 18, ELBOW_STEPS = 10 };

/* Cells grown per second, by each pipe. */
static constexpr double GROW_RATE = 18.0;
/* A screen is done when this share of the grid is taken. */
static constexpr double FULL = 0.4;
static constexpr double RADIUS = 0.18;      /* the pipe's, in cells */
static constexpr double BALL_RADIUS = 0.3;  /* a ball joint's */

typedef struct pipe {
    int cell[3];
    int dir; /* the way it came into `cell`; -1 when just started */
    int color;
    bool alive;
} pipe;

struct saver_pipes {
    uint64_t *rng;
    int w, h;
    bool multiple;
    xd_pipes_joint joints_setting;
    xd_pipes_joint joints; /* this screen's (Cycle changes it) */
    int n[3];
    uint8_t *grid;
    int cells, taken;
    double z0; /* the nearest layer's depth */
    xd_r3_target target;
    pipe pipes[MULTIPLE_PIPES];
    int pipe_count;
    double due; /* cells owed to the growth rate */
    cairo_surface_t *canvas;
};

static const int dir_step[DIRS][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

/* Bright and glossy, as the OpenGL saver's lit materials. */
static const double colors[][3] = {
    {0.85, 0.1, 0.1}, {0.1, 0.75, 0.15}, {0.15, 0.3, 0.95}, {0.9, 0.85, 0.1},
    {0.1, 0.8, 0.85}, {0.85, 0.2, 0.85}, {0.95, 0.5, 0.1},  {0.8, 0.8, 0.8},
};

static double rnd(saver_pipes *pipes) { return xd_saver_rand(pipes->rng); }
static int rnd_int(saver_pipes *pipes, int count) {
    return XD_MIN((int)(rnd(pipes) * count), count - 1);
}

static uint8_t *cell_at(saver_pipes *pipes, const int cell[3]) {
    for (int i = 0; i < 3; i++) {
        if (cell[i] < 0 || cell[i] >= pipes->n[i])
            return nullptr;
    }
    return &pipes->grid[(cell[2] * pipes->n[1] + cell[1]) * pipes->n[0] + cell[0]];
}

static bool cell_free(saver_pipes *pipes, const int cell[3]) {
    const uint8_t *grid_cell = cell_at(pipes, cell);
    return grid_cell != nullptr && *grid_cell == 0;
}

static void take(saver_pipes *pipes, const int cell[3]) {
    *cell_at(pipes, cell) = 1;
    pipes->taken++;
}

/* A cell's centre in the world: x right, y down, z away from the eye. */
static xd_r3_vec center(const saver_pipes *pipes, const int cell[3]) {
    return (xd_r3_vec){cell[0] - (pipes->n[0] - 1) / 2.0, cell[1] - (pipes->n[1] - 1) / 2.0,
                pipes->z0 + cell[2]};
}

static xd_r3_vec add(xd_r3_vec base, xd_r3_vec offset, double k) {
    return (xd_r3_vec){base.x + offset.x * k, base.y + offset.y * k, base.z + offset.z * k};
}

static xd_r3_vec dir_vec(int direction) {
    return (xd_r3_vec){dir_step[direction][0], dir_step[direction][1], dir_step[direction][2]};
}


/* ---- drawing ---------------------------------------------------------------- */

static xd_r3_material material(int color) {
    return (xd_r3_material){.r = colors[color][0], .g = colors[color][1], .b = colors[color][2],
                            .specular = 0.65, .shininess = 40};
}

static void draw_pipe(saver_pipes *pipes, xd_r3_vec from, xd_r3_vec to, int color) {
    xd_r3_material lit = material(color);
    xd_r3_cylinder(&pipes->target, from, to, RADIUS, SEGMENTS, &lit);
}

static void draw_ball(saver_pipes *pipes, xd_r3_vec at, double radius, int color) {
    xd_r3_material lit = material(color);
    xd_r3_sphere(&pipes->target, at, radius, SEGMENTS, &lit);
}

/* An elbow turning from `in_dir` to `out_dir` in the cell round `cell_center`:
 * a quarter torus round the cell's corner, from the middle of the face it
 * came in by to the middle of the one it leaves by. */
static void draw_elbow(saver_pipes *pipes, xd_r3_vec cell_center, int in_dir, int out_dir,
                       int color) {
    xd_r3_vec in = add(cell_center, dir_vec(in_dir), -0.5);
    xd_r3_vec corner = add(in, dir_vec(out_dir), 0.5);
    xd_r3_material lit = material(color);
    xd_r3_elbow(&pipes->target, corner, add((xd_r3_vec){0, 0, 0}, dir_vec(out_dir), -1),
                dir_vec(in_dir), 0.5, RADIUS, SEGMENTS, ELBOW_STEPS, &lit);
}

/* ---- growing ---------------------------------------------------------------- */

static void restart(saver_pipes *pipes) {
    memset(pipes->grid, 0, (size_t)pipes->cells);
    pipes->taken = 0;
    pipes->pipe_count = pipes->multiple ? MULTIPLE_PIPES : 1;
    for (int i = 0; i < pipes->pipe_count; i++)
        pipes->pipes[i].alive = false;
    if (pipes->joints_setting == XD_PIPES_CYCLE)
        pipes->joints = pipes->joints == XD_PIPES_ELBOW  ? XD_PIPES_BALL
                        : pipes->joints == XD_PIPES_BALL ? XD_PIPES_MIXED
                                                         : XD_PIPES_ELBOW;
    cairo_surface_flush(pipes->canvas);
    xd_r3_clear(&pipes->target, 0x000000);
    cairo_surface_mark_dirty(pipes->canvas);
}

/* A new pipe in a free cell; false if none turned up (the grid is full). */
static bool start_pipe(saver_pipes *pipes, pipe *pp) {
    for (int tries = 0; tries < 64; tries++) {
        int cell[3] = {rnd_int(pipes, pipes->n[0]), rnd_int(pipes, pipes->n[1]),
                       rnd_int(pipes, pipes->n[2])};
        if (!cell_free(pipes, cell))
            continue;
        memcpy(pp->cell, cell, sizeof(cell));
        take(pipes, cell);
        pp->dir = -1;
        pp->color = rnd_int(pipes, (int)XD_ARRAY_LEN(colors));
        pp->alive = true;
        return true;
    }
    return false;
}

static bool is_ball_joint(saver_pipes *pipes) {
    switch (pipes->joints) {
    case XD_PIPES_BALL:
        return true;
    case XD_PIPES_MIXED:
        return rnd(pipes) < 0.5;
    case XD_PIPES_ELBOW:
    case XD_PIPES_CYCLE:
    case XD_PIPES_JOINT_COUNT:
        break;
    }
    return false;
}

/* Grows a pipe through its cell: from where it came in to where it
 * leaves, and on into the next cell. */
static void grow(saver_pipes *pipes, pipe *pp) {
    int next[3];
    int dir = pp->dir;
    bool straight_ok = false;
    if (dir >= 0) {
        for (int i = 0; i < 3; i++)
            next[i] = pp->cell[i] + dir_step[dir][i];
        straight_ok = cell_free(pipes, next);
    }
    if (!straight_ok || rnd(pipes) < 0.25) {
        /* Turn (or start): any free way, but not back. */
        int ways[DIRS];
        int count = 0;
        for (int direction = 0; direction < DIRS; direction++) {
            if (dir >= 0 && (direction == dir || direction == (dir ^ 1)))
                continue;
            int next_cell[3];
            for (int i = 0; i < 3; i++)
                next_cell[i] = pp->cell[i] + dir_step[direction][i];
            if (cell_free(pipes, next_cell))
                ways[count++] = direction;
        }
        if (count > 0)
            dir = ways[rnd_int(pipes, count)];
        else if (!straight_ok)
            dir = -1; /* boxed in */
    }
    xd_r3_vec cell_center = center(pipes, pp->cell);
    if (dir < 0) {
        /* The end: the half pipe into the cell, with its round end. */
        if (pp->dir >= 0) {
            draw_pipe(pipes, add(cell_center, dir_vec(pp->dir), -0.5), cell_center, pp->color);
            draw_ball(pipes, cell_center, RADIUS, pp->color);
        }
        pp->alive = false;
        return;
    }
    xd_r3_vec out = add(cell_center, dir_vec(dir), 0.5);
    cairo_surface_flush(pipes->canvas);
    if (pp->dir < 0) {
        /* The start: from a rounded end in the middle of the cell. */
        draw_ball(pipes, cell_center, RADIUS, pp->color);
        draw_pipe(pipes, cell_center, out, pp->color);
    } else {
        xd_r3_vec in = add(cell_center, dir_vec(pp->dir), -0.5);
        if (dir == pp->dir) {
            draw_pipe(pipes, in, out, pp->color);
        } else if (is_ball_joint(pipes)) {
            draw_pipe(pipes, in, cell_center, pp->color);
            draw_pipe(pipes, cell_center, out, pp->color);
            draw_ball(pipes, cell_center, BALL_RADIUS, pp->color);
        } else {
            draw_elbow(pipes, cell_center, pp->dir, dir, pp->color);
        }
    }
    cairo_surface_mark_dirty(pipes->canvas);
    for (int i = 0; i < 3; i++)
        pp->cell[i] += dir_step[dir][i];
    take(pipes, pp->cell);
    pp->dir = dir;
}

static void step(saver_pipes *pipes) {
    for (int i = 0; i < pipes->pipe_count; i++) {
        pipe *pp = &pipes->pipes[i];
        if (!pp->alive && !start_pipe(pipes, pp)) {
            restart(pipes);
            return;
        }
        grow(pipes, pp);
    }
    if (pipes->taken >= pipes->cells * FULL)
        restart(pipes);
}

saver_pipes *saver_pipes_create(const xd_saver_settings *saver_settings, int width, int height,
                                uint64_t *rng) {
    saver_pipes *pipes = XD_NEW(saver_pipes);
    pipes->rng = rng;
    pipes->w = width;
    pipes->h = height;
    pipes->multiple = saver_settings->pipes.multiple;
    pipes->joints_setting = saver_settings->pipes.joints;
    /* Cycle starts at elbows: restart moves it on from the last kind. */
    pipes->joints =
        pipes->joints_setting == XD_PIPES_CYCLE ? XD_PIPES_MIXED : pipes->joints_setting;
    pipes->n[1] = 12;
    pipes->n[0] = XD_CLAMP((int)lround(12.0 * width / height), 6, 32);
    pipes->n[2] = 12;
    pipes->cells = pipes->n[0] * pipes->n[1] * pipes->n[2];
    pipes->grid = xd_alloc_zeroed((size_t)pipes->cells, 1);
    /* The grid's middle layer fills the screen's height; nearer layers
     * spill over the edges, as the OpenGL saver's did. */
    pipes->z0 = 1.2 * pipes->n[2];
    double mid = pipes->z0 + (pipes->n[2] - 1) / 2.0;
    pipes->canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    cairo_surface_flush(pipes->canvas);
    pipes->target = (xd_r3_target){
        .pixels = (uint32_t *)(void *)cairo_image_surface_get_data(pipes->canvas),
        .depth = xd_alloc_array((size_t)width * (size_t)height, sizeof(float)),
        .width = width,
        .height = height,
        .stride = cairo_image_surface_get_stride(pipes->canvas) / 4,
        .focal = height * 0.5 * mid / (pipes->n[1] / 2.0),
        .cx = width / 2.0,
        .cy = height / 2.0,
        /* Towards the light: up, left and in front, as the saver's. */
        .light = {-0.45, -0.65, -0.62},
        .ambient = 0.18,
    };
    restart(pipes);
    return pipes;
}

void saver_pipes_frame(saver_pipes *pipes, cairo_t *cairo, double dt) {
    pipes->due += dt * GROW_RATE;
    while (pipes->due >= 1.0) {
        pipes->due -= 1.0;
        step(pipes);
    }
    cairo_surface_flush(pipes->canvas);
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cairo, pipes->canvas, 0, 0);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

void saver_pipes_destroy(saver_pipes *pipes) {
    cairo_surface_destroy(pipes->canvas);
    xd_free(pipes->target.depth);
    xd_free(pipes->grid);
    xd_free(pipes);
}
