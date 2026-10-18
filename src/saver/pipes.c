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

static double rnd(saver_pipes *p) { return xd_saver_rand(p->rng); }
static int rnd_int(saver_pipes *p, int n) { return XD_MIN((int)(rnd(p) * n), n - 1); }

static uint8_t *cell_at(saver_pipes *p, const int c[3]) {
    for (int i = 0; i < 3; i++) {
        if (c[i] < 0 || c[i] >= p->n[i])
            return nullptr;
    }
    return &p->grid[(c[2] * p->n[1] + c[1]) * p->n[0] + c[0]];
}

static bool cell_free(saver_pipes *p, const int c[3]) {
    const uint8_t *g = cell_at(p, c);
    return g != nullptr && *g == 0;
}

static void take(saver_pipes *p, const int c[3]) {
    *cell_at(p, c) = 1;
    p->taken++;
}

/* A cell's centre in the world: x right, y down, z away from the eye. */
static v3 center(const saver_pipes *p, const int c[3]) {
    return (v3){c[0] - (p->n[0] - 1) / 2.0, c[1] - (p->n[1] - 1) / 2.0, p->z0 + c[2]};
}

static v3 add(v3 a, v3 b, double k) { return (v3){a.x + b.x * k, a.y + b.y * k, a.z + b.z * k}; }

static v3 dir_vec(int d) { return (v3){dir_step[d][0], dir_step[d][1], dir_step[d][2]}; }

static void project(const saver_pipes *p, v3 v, double radius, double *x, double *y, double *r) {
    *x = p->w / 2.0 + v.x * p->f / v.z;
    *y = p->h / 2.0 + v.y * p->f / v.z;
    *r = radius * p->f / v.z;
}

/* ---- drawing ---------------------------------------------------------------- */

static void set_shade(cairo_pattern_t *pat, double at, const piece *pc, double k, double white) {
    cairo_pattern_add_color_stop_rgb(pat, at, fmin(1, pc->r * k + white),
                                     fmin(1, pc->g * k + white), fmin(1, pc->b * k + white));
}

static void draw_ball(cairo_t *cr, const piece *pc, double x, double y, double r) {
    /* Lit from the upper left, as the saver's light. */
    cairo_pattern_t *pat = cairo_pattern_create_radial(x - r * 0.35, y - r * 0.4, r * 0.05, x, y, r);
    set_shade(pat, 0.0, pc, 1.0, 0.55);
    set_shade(pat, 0.35, pc, 0.95, 0.0);
    set_shade(pat, 1.0, pc, 0.25, 0.0);
    cairo_set_source(cr, pat);
    cairo_arc(cr, x, y, r, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

static void draw_piece(cairo_t *cr, const piece *pc) {
    double dx = pc->bx - pc->ax;
    double dy = pc->by - pc->ay;
    double len = hypot(dx, dy);
    double rmax = fmax(pc->ar, pc->br);
    if (len < 0.35 * rmax) {
        /* A ball, or a pipe seen end on. */
        bool a_near = pc->ar >= pc->br;
        draw_ball(cr, pc, a_near ? pc->ax : pc->bx, a_near ? pc->ay : pc->by, rmax);
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
    cairo_set_source(cr, pat);
    cairo_new_path(cr);
    cairo_move_to(cr, pc->ax + nx * pc->ar, pc->ay + ny * pc->ar);
    cairo_line_to(cr, pc->bx + nx * pc->br, pc->by + ny * pc->br);
    cairo_line_to(cr, pc->bx - nx * pc->br, pc->by - ny * pc->br);
    cairo_line_to(cr, pc->ax - nx * pc->ar, pc->ay - ny * pc->ar);
    cairo_close_path(cr);
    if (pc->cap_a) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, pc->ax, pc->ay, pc->ar, 0, 2 * M_PI);
    }
    if (pc->cap_b) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, pc->bx, pc->by, pc->br, 0, 2 * M_PI);
    }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

static int by_depth(const void *a, const void *b, void *user) {
    const piece *pieces = user;
    double da = pieces[*(const size_t *)a].depth;
    double db = pieces[*(const size_t *)b].depth;
    if (da != db)
        return da > db ? -1 : 1; /* far first */
    /* Then in the order they grew, so ties paint the same every time. */
    return *(const size_t *)a < *(const size_t *)b ? -1 : 1;
}

/* Repaints the rectangle the newest piece covers, everything in it far
 * to near. */
static void repaint_newest(saver_pipes *p) {
    const piece *nw = &p->pieces[p->count - 1];
    double x0 = floor(nw->x0) - 1;
    double y0 = floor(nw->y0) - 1;
    double x1 = ceil(nw->x1) + 1;
    double y1 = ceil(nw->y1) + 1;
    if (x1 < 0 || y1 < 0 || x0 > p->w || y0 > p->h)
        return;
    size_t n = 0;
    for (size_t i = 0; i < p->count; i++) {
        const piece *pc = &p->pieces[i];
        if (pc->x1 >= x0 && pc->x0 <= x1 && pc->y1 >= y0 && pc->y0 <= y1)
            p->order[n++] = i;
    }
    qsort_r(p->order, n, sizeof(*p->order), by_depth, p->pieces);
    cairo_t *cr = p->cr;
    cairo_save(cr);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    for (size_t i = 0; i < n; i++)
        draw_piece(cr, &p->pieces[p->order[i]]);
    cairo_restore(cr);
}

enum { CAP_A = 1, CAP_B = 2 };

static void add_piece(saver_pipes *p, v3 a, v3 b, double radius, int color, unsigned caps) {
    /* A square end runs a little into its neighbour, so that no seam of
     * background shows between them. */
    v3 d = {b.x - a.x, b.y - a.y, b.z - a.z};
    double len = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    /* Going away from the eye, a pipe's outline is mostly its ends. */
    if (fabs(d.z) > 0.5 * len)
        caps = CAP_A | CAP_B;
    if (len > 0) {
        if (!(caps & CAP_A))
            a = add(a, d, -0.03 / len);
        if (!(caps & CAP_B))
            b = add(b, d, 0.03 / len);
    }
    if (p->count == p->cap) {
        p->cap = p->cap != 0 ? p->cap * 2 : 256;
        p->pieces = xd_realloc_array(p->pieces, p->cap, sizeof(*p->pieces));
        p->order = xd_realloc_array(p->order, p->cap, sizeof(*p->order));
    }
    piece *pc = &p->pieces[p->count++];
    *pc = (piece){.r = colors[color][0], .g = colors[color][1], .b = colors[color][2],
                  .cap_a = (caps & CAP_A) != 0, .cap_b = (caps & CAP_B) != 0};
    project(p, a, radius, &pc->ax, &pc->ay, &pc->ar);
    project(p, b, radius, &pc->bx, &pc->by, &pc->br);
    v3 m = add(a, (v3){b.x - a.x, b.y - a.y, b.z - a.z}, 0.5);
    pc->depth = sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
    pc->x0 = fmin(pc->ax - pc->ar, pc->bx - pc->br);
    pc->y0 = fmin(pc->ay - pc->ar, pc->by - pc->br);
    pc->x1 = fmax(pc->ax + pc->ar, pc->bx + pc->br);
    pc->y1 = fmax(pc->ay + pc->ar, pc->by + pc->br);
    repaint_newest(p);
}

/* ---- growing ---------------------------------------------------------------- */

static void restart(saver_pipes *p) {
    memset(p->grid, 0, (size_t)p->cells);
    p->taken = 0;
    p->count = 0;
    p->pipe_count = p->multiple ? MULTIPLE_PIPES : 1;
    for (int i = 0; i < p->pipe_count; i++)
        p->pipes[i].alive = false;
    if (p->joints_setting == XD_PIPES_CYCLE)
        p->joints = p->joints == XD_PIPES_ELBOW  ? XD_PIPES_BALL
                    : p->joints == XD_PIPES_BALL ? XD_PIPES_MIXED
                                                 : XD_PIPES_ELBOW;
    cairo_save(p->cr);
    cairo_set_source_rgb(p->cr, 0, 0, 0);
    cairo_paint(p->cr);
    cairo_restore(p->cr);
}

/* A new pipe in a free cell; false if none turned up (the grid is full). */
static bool start_pipe(saver_pipes *p, pipe *pp) {
    for (int tries = 0; tries < 64; tries++) {
        int c[3] = {rnd_int(p, p->n[0]), rnd_int(p, p->n[1]), rnd_int(p, p->n[2])};
        if (!cell_free(p, c))
            continue;
        memcpy(pp->cell, c, sizeof(c));
        take(p, c);
        pp->dir = -1;
        pp->color = rnd_int(p, (int)XD_ARRAY_LEN(colors));
        pp->alive = true;
        return true;
    }
    return false;
}

static bool is_ball_joint(saver_pipes *p) {
    switch (p->joints) {
    case XD_PIPES_BALL:
        return true;
    case XD_PIPES_MIXED:
        return rnd(p) < 0.5;
    case XD_PIPES_ELBOW:
    case XD_PIPES_CYCLE:
    case XD_PIPES_JOINT_COUNT:
        break;
    }
    return false;
}

/* Grows a pipe through its cell: from where it came in to where it
 * leaves, and on into the next cell. */
static void grow(saver_pipes *p, pipe *pp) {
    int next[3];
    int dir = pp->dir;
    bool straight_ok = false;
    if (dir >= 0) {
        for (int i = 0; i < 3; i++)
            next[i] = pp->cell[i] + dir_step[dir][i];
        straight_ok = cell_free(p, next);
    }
    if (!straight_ok || rnd(p) < 0.25) {
        /* Turn (or start): any free way, but not back. */
        int ways[DIRS];
        int n = 0;
        for (int d = 0; d < DIRS; d++) {
            if (dir >= 0 && (d == dir || d == (dir ^ 1)))
                continue;
            int c[3];
            for (int i = 0; i < 3; i++)
                c[i] = pp->cell[i] + dir_step[d][i];
            if (cell_free(p, c))
                ways[n++] = d;
        }
        if (n > 0)
            dir = ways[rnd_int(p, n)];
        else if (!straight_ok)
            dir = -1; /* boxed in */
    }
    v3 c = center(p, pp->cell);
    if (dir < 0) {
        /* The end: the half pipe into the cell, with its round end. */
        if (pp->dir >= 0)
            add_piece(p, add(c, dir_vec(pp->dir), -0.5), c, RADIUS, pp->color, CAP_B);
        pp->alive = false;
        return;
    }
    v3 out = add(c, dir_vec(dir), 0.5);
    if (pp->dir < 0) {
        add_piece(p, c, out, RADIUS, pp->color, CAP_A);
    } else {
        v3 in = add(c, dir_vec(pp->dir), -0.5);
        if (dir == pp->dir) {
            add_piece(p, in, out, RADIUS, pp->color, 0);
        } else if (is_ball_joint(p)) {
            add_piece(p, in, c, RADIUS, pp->color, 0);
            add_piece(p, c, out, RADIUS, pp->color, 0);
            add_piece(p, c, c, BALL_RADIUS, pp->color, 0);
        } else {
            /* An elbow: a quarter circle round the cell's corner. */
            v3 corner = add(in, dir_vec(dir), 0.5);
            v3 prev = in;
            for (int i = 1; i <= ARC_STEPS; i++) {
                double a = M_PI / 2 * i / (double)ARC_STEPS;
                v3 pt = add(add(corner, dir_vec(dir), -0.5 * cos(a)), dir_vec(pp->dir),
                            0.5 * sin(a));
                add_piece(p, prev, pt, RADIUS, pp->color,
                          (i > 1 ? CAP_A : 0u) | (i < ARC_STEPS ? CAP_B : 0u));
                prev = pt;
            }
        }
    }
    for (int i = 0; i < 3; i++)
        pp->cell[i] += dir_step[dir][i];
    take(p, pp->cell);
    pp->dir = dir;
}

static void step(saver_pipes *p) {
    for (int i = 0; i < p->pipe_count; i++) {
        pipe *pp = &p->pipes[i];
        if (!pp->alive && !start_pipe(p, pp)) {
            restart(p);
            return;
        }
        grow(p, pp);
    }
    if (p->taken >= p->cells * FULL)
        restart(p);
}

saver_pipes *saver_pipes_create(const xd_saver_settings *o, int w, int h, uint64_t *rng) {
    saver_pipes *p = XD_NEW(saver_pipes);
    p->rng = rng;
    p->w = w;
    p->h = h;
    p->multiple = o->pipes.multiple;
    p->joints_setting = o->pipes.joints;
    /* Cycle starts at elbows: restart moves it on from the last kind. */
    p->joints = p->joints_setting == XD_PIPES_CYCLE ? XD_PIPES_MIXED : p->joints_setting;
    p->n[1] = 12;
    p->n[0] = XD_CLAMP((int)lround(12.0 * w / h), 6, 32);
    p->n[2] = 12;
    p->cells = p->n[0] * p->n[1] * p->n[2];
    p->grid = xd_alloc_zeroed((size_t)p->cells, 1);
    /* The grid's middle layer fills the screen's height; nearer layers
     * spill over the edges, as the OpenGL saver's did. */
    p->z0 = 1.2 * p->n[2];
    double mid = p->z0 + (p->n[2] - 1) / 2.0;
    p->f = h * 0.5 * mid / (p->n[1] / 2.0);
    p->canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    p->cr = cairo_create(p->canvas);
    restart(p);
    return p;
}

void saver_pipes_frame(saver_pipes *p, cairo_t *cr, double dt) {
    p->due += dt * GROW_RATE;
    while (p->due >= 1.0) {
        p->due -= 1.0;
        step(p);
    }
    cairo_surface_flush(p->canvas);
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, p->canvas, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

void saver_pipes_destroy(saver_pipes *p) {
    cairo_destroy(p->cr);
    cairo_surface_destroy(p->canvas);
    xd_free(p->grid);
    xd_free(p->pieces);
    xd_free(p->order);
    xd_free(p);
}
