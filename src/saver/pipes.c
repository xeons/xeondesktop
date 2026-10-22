/* SPDX-License-Identifier: MIT
 * 3D Pipes, after Windows' OpenGL saver, drawn in 2D: pipes grow cell by
 * cell through a 3D grid seen in perspective, turning at random through
 * elbows or ball joints, until the grid is crowded; then the screen
 * clears and it starts again.
 *
 * Pipes are shaded capsules (a cylinder with round ends) and balls,
 * painted far to near. The picture only grows, so it is kept in an image
 * of its own: each new piece repaints just the rectangle it covers, with
 * every piece there in depth order, and a frame only copies the image.
 * That keeps the cost per frame flat however full the screen gets.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../core/alloc.h"
#include "saver_impl.h"

enum { MULTIPLE_PIPES = 5, DIRS = 6, ARC_STEPS = 4 };

/* Cells grown per second, by each pipe. */
static constexpr double GROW_RATE = 18.0;
/* A screen is done when this share of the grid is taken. */
static constexpr double FULL = 0.4;
static constexpr double RADIUS = 0.22;      /* the pipe's, in cells */
static constexpr double BALL_RADIUS = 0.36; /* a ball joint's */

typedef struct v3 {
    double x, y, z;
} v3;

typedef struct piece {
    double r, g, b;
    double depth;        /* distance from the eye, for the painter's order */
    double ax, ay, ar;   /* one end on the screen, and its radius there */
    double bx, by, br;   /* the other (a ball's is the same) */
    bool cap_a, cap_b;   /* round ends: a pipe's start and end, an elbow's bends */
    double x0, y0, x1, y1; /* bounding box on the screen */
} piece;

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
    double f, z0; /* projection: focal length (px) and the nearest layer's depth */
    piece *pieces;
    size_t count, cap;
    size_t *order; /* scratch for repaints */
    pipe pipes[MULTIPLE_PIPES];
    int pipe_count;
    double due; /* cells owed to the growth rate */
    cairo_surface_t *canvas;
    cairo_t *cr;
};

static const int dir_step[DIRS][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

/* Bright, as the OpenGL saver's lit materials. */
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
static v3 center(const saver_pipes *pipes, const int cell[3]) {
    return (v3){cell[0] - (pipes->n[0] - 1) / 2.0, cell[1] - (pipes->n[1] - 1) / 2.0,
                pipes->z0 + cell[2]};
}

static v3 add(v3 base, v3 offset, double k) {
    return (v3){base.x + offset.x * k, base.y + offset.y * k, base.z + offset.z * k};
}

static v3 dir_vec(int direction) {
    return (v3){dir_step[direction][0], dir_step[direction][1], dir_step[direction][2]};
}

static void project(const saver_pipes *pipes, v3 point, double radius, double *screen_x,
                    double *screen_y, double *screen_radius) {
    *screen_x = pipes->w / 2.0 + point.x * pipes->f / point.z;
    *screen_y = pipes->h / 2.0 + point.y * pipes->f / point.z;
    *screen_radius = radius * pipes->f / point.z;
}

/* ---- drawing ---------------------------------------------------------------- */

static void set_shade(cairo_pattern_t *pat, double at, const piece *pc, double k, double white) {
    cairo_pattern_add_color_stop_rgb(pat, at, fmin(1, pc->r * k + white),
                                     fmin(1, pc->g * k + white), fmin(1, pc->b * k + white));
}

static void draw_ball(cairo_t *cairo, const piece *pc, double x, double y, double radius) {
    /* Lit from the upper left, as the saver's light. */
    cairo_pattern_t *pat = cairo_pattern_create_radial(x - radius * 0.35, y - radius * 0.4,
                                                       radius * 0.05, x, y, radius);
    set_shade(pat, 0.0, pc, 1.0, 0.55);
    set_shade(pat, 0.35, pc, 0.95, 0.0);
    set_shade(pat, 1.0, pc, 0.25, 0.0);
    cairo_set_source(cairo, pat);
    cairo_arc(cairo, x, y, radius, 0, 2 * M_PI);
    cairo_fill(cairo);
    cairo_pattern_destroy(pat);
}

static void draw_piece(cairo_t *cairo, const piece *pc) {
    double dx = pc->bx - pc->ax;
    double dy = pc->by - pc->ay;
    double len = hypot(dx, dy);
    double rmax = fmax(pc->ar, pc->br);
    if (len < 0.35 * rmax) {
        /* A ball, or a pipe seen end on. */
        bool a_near = pc->ar >= pc->br;
        draw_ball(cairo, pc, a_near ? pc->ax : pc->bx, a_near ? pc->ay : pc->by, rmax);
        return;
    }
    /* Across the pipe, from its dark side to the side facing the light. */
    double nx = -dy / len;
    double ny = dx / len;
    if (nx * -0.5 + ny * -0.8 < 0) {
        nx = -nx;
        ny = -ny;
    }
    double mx = (pc->ax + pc->bx) / 2;
    double my = (pc->ay + pc->by) / 2;
    double rm = (pc->ar + pc->br) / 2;
    cairo_pattern_t *pat =
        cairo_pattern_create_linear(mx - nx * rm, my - ny * rm, mx + nx * rm, my + ny * rm);
    set_shade(pat, 0.0, pc, 0.18, 0.0);
    set_shade(pat, 0.45, pc, 0.75, 0.0);
    set_shade(pat, 0.72, pc, 1.0, 0.08);
    set_shade(pat, 0.86, pc, 0.9, 0.55);
    set_shade(pat, 1.0, pc, 0.6, 0.0);
    cairo_set_source(cairo, pat);
    cairo_new_path(cairo);
    cairo_move_to(cairo, pc->ax + nx * pc->ar, pc->ay + ny * pc->ar);
    cairo_line_to(cairo, pc->bx + nx * pc->br, pc->by + ny * pc->br);
    cairo_line_to(cairo, pc->bx - nx * pc->br, pc->by - ny * pc->br);
    cairo_line_to(cairo, pc->ax - nx * pc->ar, pc->ay - ny * pc->ar);
    cairo_close_path(cairo);
    if (pc->cap_a) {
        cairo_new_sub_path(cairo);
        cairo_arc(cairo, pc->ax, pc->ay, pc->ar, 0, 2 * M_PI);
    }
    if (pc->cap_b) {
        cairo_new_sub_path(cairo);
        cairo_arc(cairo, pc->bx, pc->by, pc->br, 0, 2 * M_PI);
    }
    cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_WINDING);
    cairo_fill(cairo);
    cairo_pattern_destroy(pat);
}

static int by_depth(const void *left, const void *right, void *user) {
    const piece *pieces = user;
    double da = pieces[*(const size_t *)left].depth;
    double db = pieces[*(const size_t *)right].depth;
    if (da != db)
        return da > db ? -1 : 1; /* far first */
    /* Then in the order they grew, so ties paint the same every time. */
    return *(const size_t *)left < *(const size_t *)right ? -1 : 1;
}

/* Repaints the rectangle the newest piece covers, everything in it far
 * to near. */
static void repaint_newest(saver_pipes *pipes) {
    const piece *nw = &pipes->pieces[pipes->count - 1];
    double x0 = floor(nw->x0) - 1;
    double y0 = floor(nw->y0) - 1;
    double x1 = ceil(nw->x1) + 1;
    double y1 = ceil(nw->y1) + 1;
    if (x1 < 0 || y1 < 0 || x0 > pipes->w || y0 > pipes->h)
        return;
    size_t count = 0;
    for (size_t i = 0; i < pipes->count; i++) {
        const piece *pc = &pipes->pieces[i];
        if (pc->x1 >= x0 && pc->x0 <= x1 && pc->y1 >= y0 && pc->y0 <= y1)
            pipes->order[count++] = i;
    }
    qsort_r(pipes->order, count, sizeof(*pipes->order), by_depth, pipes->pieces);
    cairo_t *cairo = pipes->cr;
    cairo_save(cairo);
    cairo_rectangle(cairo, x0, y0, x1 - x0, y1 - y0);
    cairo_clip(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_paint(cairo);
    for (size_t i = 0; i < count; i++)
        draw_piece(cairo, &pipes->pieces[pipes->order[i]]);
    cairo_restore(cairo);
}

enum { CAP_A = 1, CAP_B = 2 };

static void add_piece(saver_pipes *pipes, v3 start, v3 end, double radius, int color,
                      unsigned caps) {
    /* A square end runs a little into its neighbour, so that no seam of
     * background shows between them. */
    v3 delta = {end.x - start.x, end.y - start.y, end.z - start.z};
    double len = sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    /* Going away from the eye, a pipe's outline is mostly its ends. */
    if (fabs(delta.z) > 0.5 * len)
        caps = CAP_A | CAP_B;
    if (len > 0) {
        if (!(caps & CAP_A))
            start = add(start, delta, -0.03 / len);
        if (!(caps & CAP_B))
            end = add(end, delta, 0.03 / len);
    }
    if (pipes->count == pipes->cap) {
        pipes->cap = pipes->cap != 0 ? pipes->cap * 2 : 256;
        pipes->pieces = xd_realloc_array(pipes->pieces, pipes->cap, sizeof(*pipes->pieces));
        pipes->order = xd_realloc_array(pipes->order, pipes->cap, sizeof(*pipes->order));
    }
    piece *pc = &pipes->pieces[pipes->count++];
    *pc = (piece){.r = colors[color][0], .g = colors[color][1], .b = colors[color][2],
                  .cap_a = (caps & CAP_A) != 0, .cap_b = (caps & CAP_B) != 0};
    project(pipes, start, radius, &pc->ax, &pc->ay, &pc->ar);
    project(pipes, end, radius, &pc->bx, &pc->by, &pc->br);
    v3 middle = add(start, (v3){end.x - start.x, end.y - start.y, end.z - start.z}, 0.5);
    pc->depth = sqrt(middle.x * middle.x + middle.y * middle.y + middle.z * middle.z);
    pc->x0 = fmin(pc->ax - pc->ar, pc->bx - pc->br);
    pc->y0 = fmin(pc->ay - pc->ar, pc->by - pc->br);
    pc->x1 = fmax(pc->ax + pc->ar, pc->bx + pc->br);
    pc->y1 = fmax(pc->ay + pc->ar, pc->by + pc->br);
    repaint_newest(pipes);
}

/* ---- growing ---------------------------------------------------------------- */

static void restart(saver_pipes *pipes) {
    memset(pipes->grid, 0, (size_t)pipes->cells);
    pipes->taken = 0;
    pipes->count = 0;
    pipes->pipe_count = pipes->multiple ? MULTIPLE_PIPES : 1;
    for (int i = 0; i < pipes->pipe_count; i++)
        pipes->pipes[i].alive = false;
    if (pipes->joints_setting == XD_PIPES_CYCLE)
        pipes->joints = pipes->joints == XD_PIPES_ELBOW  ? XD_PIPES_BALL
                        : pipes->joints == XD_PIPES_BALL ? XD_PIPES_MIXED
                                                         : XD_PIPES_ELBOW;
    cairo_save(pipes->cr);
    cairo_set_source_rgb(pipes->cr, 0, 0, 0);
    cairo_paint(pipes->cr);
    cairo_restore(pipes->cr);
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
    v3 cell_center = center(pipes, pp->cell);
    if (dir < 0) {
        /* The end: the half pipe into the cell, with its round end. */
        if (pp->dir >= 0)
            add_piece(pipes, add(cell_center, dir_vec(pp->dir), -0.5), cell_center, RADIUS,
                      pp->color, CAP_B);
        pp->alive = false;
        return;
    }
    v3 out = add(cell_center, dir_vec(dir), 0.5);
    if (pp->dir < 0) {
        add_piece(pipes, cell_center, out, RADIUS, pp->color, CAP_A);
    } else {
        v3 in = add(cell_center, dir_vec(pp->dir), -0.5);
        if (dir == pp->dir) {
            add_piece(pipes, in, out, RADIUS, pp->color, 0);
        } else if (is_ball_joint(pipes)) {
            add_piece(pipes, in, cell_center, RADIUS, pp->color, 0);
            add_piece(pipes, cell_center, out, RADIUS, pp->color, 0);
            add_piece(pipes, cell_center, cell_center, BALL_RADIUS, pp->color, 0);
        } else {
            /* An elbow: a quarter circle round the cell's corner. */
            v3 corner = add(in, dir_vec(dir), 0.5);
            v3 prev = in;
            for (int i = 1; i <= ARC_STEPS; i++) {
                double angle = M_PI / 2 * i / (double)ARC_STEPS;
                v3 pt = add(add(corner, dir_vec(dir), -0.5 * cos(angle)), dir_vec(pp->dir),
                            0.5 * sin(angle));
                add_piece(pipes, prev, pt, RADIUS, pp->color,
                          (i > 1 ? CAP_A : 0u) | (i < ARC_STEPS ? CAP_B : 0u));
                prev = pt;
            }
        }
    }
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
    pipes->f = height * 0.5 * mid / (pipes->n[1] / 2.0);
    pipes->canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    pipes->cr = cairo_create(pipes->canvas);
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
    cairo_destroy(pipes->cr);
    cairo_surface_destroy(pipes->canvas);
    xd_free(pipes->grid);
    xd_free(pipes->pieces);
    xd_free(pipes->order);
    xd_free(pipes);
}
