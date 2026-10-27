/* SPDX-License-Identifier: MIT
 * Explode's maths (explode_model.h).
 */
#include "explode_model.h"

#include <math.h>

#include "../../core/alloc.h"

static constexpr double EXPLODE_PI = 3.14159265358979323846;
/* The viewer's distance, in window diagonals: how strongly pieces
 * coming closer grow. */
static constexpr double VIEWER_DISTANCE = 1.6;

typedef struct polygon {
    int count;
    double points[XD_EXPLODE_MAX_CORNERS + 4][2];
} polygon;

/* 0..1 from a fixed sequence (xorshift64*). */
static double explode_get_random(uint64_t *seed) {
    uint64_t value = *seed;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *seed = value;
    return (double)((value * 0x2545F4914F6CDD1DULL) >> 11) / 9007199254740992.0;
}

/* Keeps what's on the side of the line `x * normal_x + y * normal_y <=
 * limit` (Sutherland-Hodgman, one edge). */
static void explode_clip_edge(polygon *shape, double normal_x, double normal_y, double limit) {
    polygon kept = {0};
    for (int i = 0; i < shape->count; i++) {
        const double *current = shape->points[i];
        const double *next = shape->points[(i + 1) % shape->count];
        double current_side = current[0] * normal_x + current[1] * normal_y - limit;
        double next_side = next[0] * normal_x + next[1] * normal_y - limit;
        if (current_side <= 0 && kept.count < XD_EXPLODE_MAX_CORNERS + 4) {
            kept.points[kept.count][0] = current[0];
            kept.points[kept.count][1] = current[1];
            kept.count++;
        }
        if ((current_side < 0 && next_side > 0) || (current_side > 0 && next_side < 0)) {
            double share = current_side / (current_side - next_side);
            if (kept.count < XD_EXPLODE_MAX_CORNERS + 4) {
                kept.points[kept.count][0] = current[0] + (next[0] - current[0]) * share;
                kept.points[kept.count][1] = current[1] + (next[1] - current[1]) * share;
                kept.count++;
            }
        }
    }
    *shape = kept;
}

static double explode_get_area(const polygon *shape) {
    double twice = 0;
    for (int i = 0; i < shape->count; i++) {
        const double *current = shape->points[i];
        const double *next = shape->points[(i + 1) % shape->count];
        twice += current[0] * next[1] - next[0] * current[1];
    }
    return fabs(twice) / 2;
}

/* Clipped to the window and added, with a motion of its own. */
static void explode_add_piece(xd_explode *explode, size_t capacity, polygon shape,
                              uint64_t *seed) {
    explode_clip_edge(&shape, -1, 0, 0);
    explode_clip_edge(&shape, 1, 0, explode->width);
    explode_clip_edge(&shape, 0, -1, 0);
    explode_clip_edge(&shape, 0, 1, explode->height);
    if (shape.count < 3 || shape.count > XD_EXPLODE_MAX_CORNERS ||
        explode_get_area(&shape) < 0.5 || explode->count >= capacity)
        return;
    xd_explode_piece *piece = &explode->pieces[explode->count++];
    *piece = (xd_explode_piece){.corner_count = shape.count};
    for (int i = 0; i < shape.count; i++) {
        piece->corners[i][0] = shape.points[i][0];
        piece->corners[i][1] = shape.points[i][1];
        piece->center_x += shape.points[i][0] / shape.count;
        piece->center_y += shape.points[i][1] / shape.count;
    }
    /* Out from the middle, a little off the straight line. */
    double out_x = piece->center_x - explode->width / 2;
    double out_y = piece->center_y - explode->height / 2;
    double angle = atan2(out_y, out_x);
    if (fabs(out_x) < 1e-9 && fabs(out_y) < 1e-9)
        angle = explode_get_random(seed) * 2 * EXPLODE_PI;
    angle += (explode_get_random(seed) - 0.5) * 0.6;
    piece->direction_x = cos(angle);
    piece->direction_y = sin(angle);
    piece->speed = 0.12 + 0.36 * explode_get_random(seed);
    double axis_x = explode_get_random(seed) - 0.5;
    double axis_y = explode_get_random(seed) - 0.5;
    double axis_z = explode_get_random(seed) - 0.5;
    double length = sqrt(axis_x * axis_x + axis_y * axis_y + axis_z * axis_z);
    if (length < 1e-6) {
        axis_x = 0;
        axis_y = 1;
        axis_z = 0;
        length = 1;
    }
    piece->axis[0] = axis_x / length;
    piece->axis[1] = axis_y / length;
    piece->axis[2] = axis_z / length;
    piece->spin = (1 + 2 * explode_get_random(seed)) * EXPLODE_PI *
                  (explode_get_random(seed) < 0.5 ? -1 : 1);
    piece->rise = 0.05 + 0.3 * explode_get_random(seed);
}

static polygon explode_make_quad(double left, double top, double right, double bottom) {
    return (polygon){4, {{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
}

bool xd_explode_init(xd_explode *explode, xd_explode_tessellation tessellation, double width,
                     double height, int across, int down, int spokes, int tiers, uint64_t seed) {
    *explode = (xd_explode){.width = width, .height = height};
    if (!(width >= 1 && height >= 1))
        return false;
    across = across < 1 ? 1 : across;
    down = down < 1 ? 1 : down;
    spokes = spokes < 3 ? 3 : spokes;
    tiers = tiers < 1 ? 1 : tiers;
    if (seed == 0)
        seed = 0x9E3779B97F4A7C15ULL;
    size_t capacity = 0;
    switch (tessellation) {
    case XD_EXPLODE_RECTANGULAR:
        capacity = (size_t)across * (size_t)down;
        break;
    case XD_EXPLODE_HEXAGONAL:
        capacity = ((size_t)across + 3) * ((size_t)down + 3);
        break;
    case XD_EXPLODE_GLASS:
        capacity = (size_t)spokes * (size_t)tiers;
        break;
    }
    explode->pieces = XD_NEW_ARRAY(xd_explode_piece, capacity);
    double cell_width = width / across, cell_height = height / down;
    switch (tessellation) {
    case XD_EXPLODE_RECTANGULAR:
        for (int row = 0; row < down; row++) {
            for (int column = 0; column < across; column++)
                explode_add_piece(explode, capacity,
                                  explode_make_quad(column * cell_width, row * cell_height,
                                                    (column + 1) * cell_width,
                                                    (row + 1) * cell_height),
                                  &seed);
        }
        break;
    case XD_EXPLODE_HEXAGONAL:
        /* Pointy-topped hexagons that tile: rows cell_height apart, every
         * other one half a cell across. */
        for (int row = -1; row <= down + 1; row++) {
            for (int column = -1; column <= across + 1; column++) {
                double center_x = (column + (row & 1 ? 0.5 : 0.0)) * cell_width;
                double center_y = row * cell_height;
                double half = cell_width / 2, third = cell_height / 3;
                polygon hexagon = {6,
                                   {{center_x, center_y - 2 * third},
                                    {center_x + half, center_y - third},
                                    {center_x + half, center_y + third},
                                    {center_x, center_y + 2 * third},
                                    {center_x - half, center_y + third},
                                    {center_x - half, center_y - third}}};
                explode_add_piece(explode, capacity, hexagon, &seed);
            }
        }
        break;
    case XD_EXPLODE_GLASS: {
        /* Rings round the middle, cut by spokes; the last ring reaches
         * past the corners. */
        double reach = sqrt(width * width + height * height) / 2;
        double angles[64];
        int spoke_count = spokes > 64 ? 64 : spokes;
        for (int spoke = 0; spoke < spoke_count; spoke++)
            angles[spoke] = (spoke + (explode_get_random(&seed) - 0.5) * 0.6) * 2 * EXPLODE_PI /
                            spoke_count;
        double radii[33][64];
        int tier_count = tiers > 32 ? 32 : tiers;
        for (int tier = 0; tier <= tier_count; tier++) {
            for (int spoke = 0; spoke < spoke_count; spoke++) {
                double share = pow((double)tier / tier_count, 1.3);
                double wobble = tier > 0 && tier < tier_count
                                    ? 1 + (explode_get_random(&seed) - 0.5) * 0.3
                                    : 1;
                radii[tier][spoke] =
                    tier == tier_count ? reach * 1.5 : reach * share * wobble;
            }
        }
        double middle_x = width / 2, middle_y = height / 2;
        for (int tier = 0; tier < tier_count; tier++) {
            for (int spoke = 0; spoke < spoke_count; spoke++) {
                int next = (spoke + 1) % spoke_count;
                double inner = radii[tier][spoke], inner_next = radii[tier][next];
                double outer = radii[tier + 1][spoke], outer_next = radii[tier + 1][next];
                polygon shard = {0};
                shard.points[shard.count][0] = middle_x + inner * cos(angles[spoke]);
                shard.points[shard.count][1] = middle_y + inner * sin(angles[spoke]);
                shard.count++;
                shard.points[shard.count][0] = middle_x + outer * cos(angles[spoke]);
                shard.points[shard.count][1] = middle_y + outer * sin(angles[spoke]);
                shard.count++;
                shard.points[shard.count][0] = middle_x + outer_next * cos(angles[next]);
                shard.points[shard.count][1] = middle_y + outer_next * sin(angles[next]);
                shard.count++;
                if (tier > 0) {
                    shard.points[shard.count][0] = middle_x + inner_next * cos(angles[next]);
                    shard.points[shard.count][1] = middle_y + inner_next * sin(angles[next]);
                    shard.count++;
                }
                explode_add_piece(explode, capacity, shard, &seed);
            }
        }
        break;
    }
    }
    return explode->count > 0;
}

void xd_explode_free(xd_explode *explode) {
    xd_free(explode->pieces);
    *explode = (xd_explode){0};
}

/* Fast at first, slowing: it's an explosion. */
static double explode_ease(double progress) {
    progress = progress < 0 ? 0 : progress > 1 ? 1 : progress;
    return progress * 0.5 + (1 - (1 - progress) * (1 - progress)) * 0.5;
}

/* `vector` turned by `angle` about the unit `axis` (Rodrigues). */
static void explode_rotate(const double axis[3], double angle, const double vector[3],
                           double out[3]) {
    double cosine = cos(angle), sine = sin(angle);
    double dot = axis[0] * vector[0] + axis[1] * vector[1] + axis[2] * vector[2];
    double cross[3] = {axis[1] * vector[2] - axis[2] * vector[1],
                       axis[2] * vector[0] - axis[0] * vector[2],
                       axis[0] * vector[1] - axis[1] * vector[0]};
    for (int i = 0; i < 3; i++)
        out[i] = vector[i] * cosine + cross[i] * sine + axis[i] * dot * (1 - cosine);
}

void xd_explode_get_corner(const xd_explode *explode, const xd_explode_piece *piece, int corner,
                           double progress, double *x, double *y) {
    double diagonal = sqrt(explode->width * explode->width + explode->height * explode->height);
    double travel = explode_ease(progress);
    double offset[3] = {piece->corners[corner][0] - piece->center_x,
                        piece->corners[corner][1] - piece->center_y, 0};
    double turned[3];
    explode_rotate(piece->axis, piece->spin * progress, offset, turned);
    double flat_x = piece->center_x + piece->direction_x * piece->speed * diagonal * travel +
                    turned[0];
    double flat_y = piece->center_y + piece->direction_y * piece->speed * diagonal * travel +
                    turned[1];
    double viewer = VIEWER_DISTANCE * diagonal;
    double depth = piece->rise * diagonal * travel + turned[2];
    depth = fmin(depth, viewer * 0.8);
    double grow = viewer / (viewer - depth);
    double middle_x = explode->width / 2, middle_y = explode->height / 2;
    *x = middle_x + (flat_x - middle_x) * grow;
    *y = middle_y + (flat_y - middle_y) * grow;
}

void xd_explode_get_normal(const xd_explode_piece *piece, double progress, double normal[3]) {
    const double facing[3] = {0, 0, 1};
    explode_rotate(piece->axis, piece->spin * progress, facing, normal);
}

double xd_explode_get_alpha(double progress) {
    if (progress <= 0.4)
        return 1;
    if (progress >= 1)
        return 0;
    double share = (progress - 0.4) / 0.6;
    return 1 - share * share;
}
