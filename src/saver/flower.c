/* SPDX-License-Identifier: MIT
 * 3D Flower Box, after Windows' OpenGL saver: a shape (a cube, a
 * tetrahedron or an octahedron) tumbles and drifts round the screen,
 * bouncing off its edges, while its faces bloom: each pushes out from
 * its middle into a pointed petal and folds back flat, over and over.
 * The faces are chequered in two colours each, which can cycle.
 *
 * Each face is a grid of triangles whose points move out along the
 * face's normal, most at its middle and none at its edges, so the shape
 * stays closed. Drawn by raster3d.c each frame into a picture at most
 * 540 lines high, enlarged to the screen.
 */
#include <math.h>
#include <string.h>

#include "../core/alloc.h"
#include "raster3d.h"
#include "saver_impl.h"

static constexpr int GRID = 8;        /* triangles' rows along a face's side */
static constexpr int MAX_FACES = 8;   /* the octahedron's */
static constexpr int MAX_LINES = 540; /* the picture's height at most */

typedef struct face {
    xd_r3_vec corner[4]; /* a quad's, or a triangle's three (the fourth repeats the third) */
    bool triangle;
    xd_r3_vec normal;
} face;

struct saver_flower {
    uint64_t *rng;
    int w, h;
    face faces[MAX_FACES];
    int face_count;
    double size;                /* the shape's radius, in picture heights */
    double spin_speed, bloom_speed;
    bool cycle;
    double time;
    double x, y, vx, vy;        /* where it is on the picture, and drifting */
    double angle_a, angle_b;    /* the tumble */
    double hue;
    int vw, vh;
    cairo_surface_t *view;
    float *depth;
    xd_r3_target target;
};

static xd_r3_vec vec_add(xd_r3_vec a, xd_r3_vec b) {
    return (xd_r3_vec){a.x + b.x, a.y + b.y, a.z + b.z};
}
static xd_r3_vec vec_sub(xd_r3_vec a, xd_r3_vec b) {
    return (xd_r3_vec){a.x - b.x, a.y - b.y, a.z - b.z};
}
static xd_r3_vec vec_scale(xd_r3_vec a, double k) { return (xd_r3_vec){a.x * k, a.y * k, a.z * k}; }
static xd_r3_vec vec_cross(xd_r3_vec a, xd_r3_vec b) {
    return (xd_r3_vec){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static xd_r3_vec vec_unit(xd_r3_vec a) {
    double length = sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return length > 0 ? vec_scale(a, 1.0 / length) : a;
}

static void add_face(saver_flower *flower, const xd_r3_vec *corners, int count) {
    face *f = &flower->faces[flower->face_count++];
    for (int i = 0; i < 4; i++)
        f->corner[i] = corners[i < count ? i : count - 1];
    f->triangle = count == 3;
    xd_r3_vec centre = {0, 0, 0};
    for (int i = 0; i < count; i++)
        centre = vec_add(centre, corners[i]);
    /* Outwards: the shapes are centred on the origin. */
    xd_r3_vec n = vec_unit(vec_cross(vec_sub(corners[1], corners[0]), vec_sub(corners[2], corners[0])));
    if (n.x * centre.x + n.y * centre.y + n.z * centre.z < 0)
        n = vec_scale(n, -1);
    f->normal = n;
}

static void make_shape(saver_flower *flower, xd_flower_shape shape) {
    flower->face_count = 0;
    switch (shape) {
    case XD_FLOWER_TETRAHEDRON: {
        static const xd_r3_vec p[4] = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
        static const int faces[4][3] = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
        for (int i = 0; i < 4; i++)
            add_face(flower, (const xd_r3_vec[]){vec_scale(p[faces[i][0]], 0.7),
                                                 vec_scale(p[faces[i][1]], 0.7),
                                                 vec_scale(p[faces[i][2]], 0.7)},
                     3);
        break;
    }
    case XD_FLOWER_OCTAHEDRON: {
        static const xd_r3_vec p[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        static const int faces[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                                        {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
        for (int i = 0; i < 8; i++)
            add_face(flower, (const xd_r3_vec[]){p[faces[i][0]], p[faces[i][1]], p[faces[i][2]]},
                     3);
        break;
    }
    case XD_FLOWER_CUBE:
    case XD_FLOWER_SHAPE_COUNT: {
        static const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
                                        {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
        xd_r3_vec p[8];
        for (int i = 0; i < 8; i++)
            p[i] = (xd_r3_vec){i & 1 ? 0.6 : -0.6, i & 2 ? 0.6 : -0.6, i & 4 ? 0.6 : -0.6};
        for (int i = 0; i < 6; i++)
            add_face(flower,
                     (const xd_r3_vec[]){p[faces[i][0]], p[faces[i][1]], p[faces[i][2]],
                                         p[faces[i][3]]},
                     4);
        break;
    }
    }
}

/* A colour round the hue circle (0..1), bright. */
static void hue_rgb(double hue, double *r, double *g, double *b) {
    hue = hue - floor(hue);
    double k = hue * 6;
    double x = 1 - fabs(fmod(k, 2) - 1);
    double rgb[3] = {0, 0, 0};
    int sector = (int)k % 6;
    static const int order[6][3] = {{0, 1, 2}, {1, 0, 2}, {2, 0, 1},
                                    {2, 1, 0}, {1, 2, 0}, {0, 2, 1}};
    rgb[order[sector][0]] = 1;
    rgb[order[sector][1]] = x;
    *r = 0.15 + 0.85 * rgb[0];
    *g = 0.15 + 0.85 * rgb[1];
    *b = 0.15 + 0.85 * rgb[2];
}

/* A point on a face at (u, v) (0..1 across it), pushed out by the bloom. */
static xd_r3_vec face_point(const face *f, double u, double v, double bloom) {
    xd_r3_vec p;
    double edge; /* 0 at the face's edges, 1 at its middle */
    if (f->triangle) {
        /* u along one side, v towards the third corner. */
        double a = 1 - u - v;
        p = vec_add(vec_add(vec_scale(f->corner[0], a), vec_scale(f->corner[1], u)),
                  vec_scale(f->corner[2], v));
        edge = fmin(a, fmin(u, v)) * 3;
    } else {
        xd_r3_vec top = vec_add(vec_scale(f->corner[0], 1 - u), vec_scale(f->corner[1], u));
        xd_r3_vec bottom = vec_add(vec_scale(f->corner[3], 1 - u), vec_scale(f->corner[2], u));
        p = vec_add(vec_scale(top, 1 - v), vec_scale(bottom, v));
        edge = fmin(fmin(u, 1 - u), fmin(v, 1 - v)) * 2;
    }
    /* A petal: pointed in the middle, rising steeply from the edges. */
    return vec_add(p, vec_scale(f->normal, bloom * pow(XD_CLAMP(edge, 0, 1), 1.6)));
}

static xd_r3_vec rotate(xd_r3_vec p, double a, double b) {
    double ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b);
    xd_r3_vec turned = {p.x * ca + p.z * sa, p.y, -p.x * sa + p.z * ca};
    return (xd_r3_vec){turned.x, turned.y * cb - turned.z * sb, turned.y * sb + turned.z * cb};
}

static void draw_shape(saver_flower *flower) {
    /* Blooming: out and back, flat for only a moment between. */
    double bloom = 1.2 * (0.5 - 0.5 * cos(flower->time * flower->bloom_speed));
    xd_r3_vec centre = {(flower->x - flower->vw / 2.0) / flower->target.focal * 4.0,
                        (flower->y - flower->vh / 2.0) / flower->target.focal * 4.0, 4.0};
    double scale = flower->size * 4.0 * flower->vh / flower->target.focal;
    for (int fi = 0; fi < flower->face_count; fi++) {
        const face *f = &flower->faces[fi];
        double r1, g1, b1, r2, g2, b2;
        double hue = flower->hue + (double)fi / flower->face_count;
        hue_rgb(hue, &r1, &g1, &b1);
        hue_rgb(hue + 0.5, &r2, &g2, &b2);
        xd_r3_material light = {r1, g1, b1, 0.5, 30, false};
        xd_r3_material dark = {r2, g2, b2, 0.5, 30, false};
        xd_r3_vec grid[GRID + 1][GRID + 1];
        for (int i = 0; i <= GRID; i++)
            for (int j = 0; j <= GRID; j++) {
                double u = (double)i / GRID, v = (double)j / GRID;
                if (f->triangle && u + v > 1.0000001)
                    continue;
                xd_r3_vec p = rotate(face_point(f, u, v, bloom), flower->angle_a, flower->angle_b);
                grid[i][j] = vec_add(centre, vec_scale(p, scale));
            }
        for (int i = 0; i < GRID; i++)
            for (int j = 0; j < GRID; j++) {
                const xd_r3_material *m = (i + j) % 2 == 0 ? &light : &dark;
                bool second = !f->triangle || i + j + 1 < GRID;
                if (f->triangle && i + j >= GRID)
                    continue;
                /* Facet normals from the bent grid, so petals catch the light. */
                xd_r3_vec a[3] = {grid[i][j], grid[i + 1][j], grid[i][j + 1]};
                xd_r3_vec na = vec_cross(vec_sub(a[1], a[0]), vec_sub(a[2], a[0]));
                xd_r3_triangle(&flower->target, a, (const xd_r3_vec[]){na, na, na}, m);
                if (second) {
                    xd_r3_vec c[3] = {grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]};
                    xd_r3_vec nc = vec_cross(vec_sub(c[1], c[0]), vec_sub(c[2], c[0]));
                    xd_r3_triangle(&flower->target, c, (const xd_r3_vec[]){nc, nc, nc}, m);
                }
            }
    }
}

saver_flower *saver_flower_create(const xd_saver_settings *saver_settings, int width, int height,
                                  uint64_t *rng) {
    saver_flower *flower = XD_NEW(saver_flower);
    flower->rng = rng;
    flower->w = width;
    flower->h = height;
    make_shape(flower, saver_settings->flower.shape);
    flower->size = 0.2 + 0.03 * saver_settings->flower.size; /* 0.2 to 0.5 */
    flower->spin_speed = 0.3 + 0.17 * saver_settings->flower.spin;
    flower->bloom_speed = 0.4 + 0.12 * saver_settings->flower.bloom;
    flower->cycle = saver_settings->flower.cycle;
    flower->hue = xd_saver_rand(rng);
    flower->vh = XD_CLAMP(height, 16, MAX_LINES);
    flower->vw = XD_MAX((int)lround((double)flower->vh * width / XD_MAX(height, 1)), 16);
    flower->x = flower->vw * xd_saver_between(rng, 0.3, 0.7);
    flower->y = flower->vh * xd_saver_between(rng, 0.3, 0.7);
    double heading = xd_saver_rand(rng) * 2 * M_PI;
    flower->vx = cos(heading) * flower->vh * 0.12;
    flower->vy = sin(heading) * flower->vh * 0.12;
    flower->view = cairo_image_surface_create(CAIRO_FORMAT_RGB24, flower->vw, flower->vh);
    flower->depth = xd_alloc_array((size_t)flower->vw * (size_t)flower->vh, sizeof(float));
    cairo_surface_flush(flower->view);
    flower->target = (xd_r3_target){
        .pixels = (uint32_t *)(void *)cairo_image_surface_get_data(flower->view),
        .depth = flower->depth,
        .width = flower->vw,
        .height = flower->vh,
        .stride = cairo_image_surface_get_stride(flower->view) / 4,
        .focal = flower->vh * 1.2,
        .cx = flower->vw / 2.0,
        .cy = flower->vh / 2.0,
        .light = {-0.45, -0.6, -0.66},
        .ambient = 0.22,
    };
    return flower;
}

void saver_flower_frame(saver_flower *flower, cairo_t *cairo, double dt) {
    flower->time += dt;
    flower->angle_a += dt * flower->spin_speed;
    flower->angle_b += dt * flower->spin_speed * 0.63;
    if (flower->cycle)
        flower->hue += dt * 0.05;
    /* Drifting, bouncing off the edges (its reach, at full bloom). */
    double reach = flower->size * flower->vh * 1.2;
    flower->x += flower->vx * dt;
    flower->y += flower->vy * dt;
    if ((flower->x < reach && flower->vx < 0) || (flower->x > flower->vw - reach && flower->vx > 0))
        flower->vx = -flower->vx;
    if ((flower->y < reach && flower->vy < 0) || (flower->y > flower->vh - reach && flower->vy > 0))
        flower->vy = -flower->vy;
    cairo_surface_flush(flower->view);
    xd_r3_clear(&flower->target, 0x000000);
    draw_shape(flower);
    cairo_surface_mark_dirty(flower->view);
    cairo_save(cairo);
    cairo_scale(cairo, (double)flower->w / flower->vw, (double)flower->h / flower->vh);
    cairo_set_source_surface(cairo, flower->view, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_GOOD);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

void saver_flower_destroy(saver_flower *flower) {
    cairo_surface_destroy(flower->view);
    xd_free(flower->depth);
    xd_free(flower);
}
