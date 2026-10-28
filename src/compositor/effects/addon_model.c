/* SPDX-License-Identifier: MIT
 * The Animations Add-On's maths (addon_model.h).
 */
#include "addon_model.h"

#include <math.h>

#include "../../core/alloc.h"
#include "../../core/common.h"

static constexpr double ADDON_PI = 3.14159265358979323846;
/* Enough for any of the effects' cuts; Skewer's are Explode's. */
static constexpr size_t MAX_PIECES = 4096;
static constexpr size_t MAX_BEAMS = 2048;

typedef struct addon_polygon {
    int count;
    double points[XD_ADDON_MAX_CORNERS + 4][2];
} addon_polygon;

/* ---- helpers -------------------------------------------------------------------- */

/* 0..1 from a fixed sequence (xorshift64*, as Explode's). */
static double addon_get_random(uint64_t *seed) {
    uint64_t value = *seed;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *seed = value;
    return (double)((value * 0x2545F4914F6CDD1DULL) >> 11) / 9007199254740992.0;
}

static double addon_clamp_share(double value) {
    return XD_CLAMP(value, 0.0, 1.0);
}

/* How far `progress` is from `start` to `end`, 0..1. */
static double addon_get_share(double progress, double start, double end) {
    if (end <= start)
        return progress >= end ? 1.0 : 0.0;
    return addon_clamp_share((progress - start) / (end - start));
}

/* Easing in and out. */
static double addon_ease(double share) {
    double value = addon_clamp_share(share);
    return value * value * (3 - 2 * value);
}

/* `vector` turned by `angle` about the unit `axis` (Rodrigues). */
static void addon_rotate(const double axis[3], double angle, const double vector[3],
                         double out[3]) {
    double cosine = cos(angle), sine = sin(angle);
    double dot = axis[0] * vector[0] + axis[1] * vector[1] + axis[2] * vector[2];
    double cross[3] = {axis[1] * vector[2] - axis[2] * vector[1],
                       axis[2] * vector[0] - axis[0] * vector[2],
                       axis[0] * vector[1] - axis[1] * vector[0]};
    for (int i = 0; i < 3; i++)
        out[i] = vector[i] * cosine + cross[i] * sine + axis[i] * dot * (1 - cosine);
}

/* `point` turned by `angle` about the line through `pivot` along `axis`. */
static void addon_rotate_about(const double pivot[3], const double axis[3], double angle,
                               double point[3]) {
    double offset[3] = {point[0] - pivot[0], point[1] - pivot[1], point[2] - pivot[2]};
    double turned[3];
    addon_rotate(axis, angle, offset, turned);
    for (int i = 0; i < 3; i++)
        point[i] = pivot[i] + turned[i];
}

static void addon_normalize(double vector[3]) {
    double length = sqrt(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
    if (length < 1e-12) {
        vector[0] = 0;
        vector[1] = 0;
        vector[2] = 1;
        return;
    }
    for (int i = 0; i < 3; i++)
        vector[i] /= length;
}

/* `angle` or its opposite: the one that first moves (x, y) toward the
 * viewer, or away from it, about the hinge line. */
static double addon_choose_angle(const double pivot[3], const double axis[3], double angle,
                                 double x, double y, bool toward_viewer) {
    double point[3] = {x, y, 0};
    addon_rotate_about(pivot, axis, 0.3, point);
    return (point[2] > 0) == toward_viewer ? angle : -angle;
}

/* Keeps what's on the side of the line `x * normal_x + y * normal_y <=
 * limit` (Sutherland-Hodgman, one edge). */
static void addon_clip(addon_polygon *shape, double normal_x, double normal_y, double limit) {
    addon_polygon kept = {0};
    int cap = (int)XD_ARRAY_LEN(kept.points);
    for (int i = 0; i < shape->count; i++) {
        const double *current = shape->points[i];
        const double *next = shape->points[(i + 1) % shape->count];
        double current_side = current[0] * normal_x + current[1] * normal_y - limit;
        double next_side = next[0] * normal_x + next[1] * normal_y - limit;
        if (current_side <= 0 && kept.count < cap) {
            kept.points[kept.count][0] = current[0];
            kept.points[kept.count][1] = current[1];
            kept.count++;
        }
        if (((current_side < 0 && next_side > 0) || (current_side > 0 && next_side < 0)) &&
            kept.count < cap) {
            double share = current_side / (current_side - next_side);
            kept.points[kept.count][0] = current[0] + (next[0] - current[0]) * share;
            kept.points[kept.count][1] = current[1] + (next[1] - current[1]) * share;
            kept.count++;
        }
    }
    *shape = kept;
}

static double addon_get_area(const addon_polygon *shape) {
    double twice = 0;
    for (int i = 0; i < shape->count; i++) {
        const double *current = shape->points[i];
        const double *next = shape->points[(i + 1) % shape->count];
        twice += current[0] * next[1] - next[0] * current[1];
    }
    return fabs(twice) / 2;
}

/* `shape` reflected across the line through (x0, y0) and (x1, y1). */
static void addon_reflect(addon_polygon *shape, double x0, double y0, double x1, double y1) {
    double along_x = x1 - x0, along_y = y1 - y0;
    double length = sqrt(along_x * along_x + along_y * along_y);
    if (length < 1e-12)
        return;
    along_x /= length;
    along_y /= length;
    for (int i = 0; i < shape->count; i++) {
        double offset_x = shape->points[i][0] - x0, offset_y = shape->points[i][1] - y0;
        double dot = offset_x * along_x + offset_y * along_y;
        shape->points[i][0] = x0 + 2 * dot * along_x - offset_x;
        shape->points[i][1] = y0 + 2 * dot * along_y - offset_y;
    }
}

/* A piece where `shape` is (clipped to nothing: none), with no motion
 * yet. */
static xd_addon_piece *addon_add_piece(xd_addon *addon, const addon_polygon *shape) {
    if (shape->count < 3 || shape->count > XD_ADDON_MAX_CORNERS || addon_get_area(shape) < 0.5 ||
        addon->count >= MAX_PIECES)
        return nullptr;
    xd_addon_piece *piece = &addon->pieces[addon->count++];
    *piece = (xd_addon_piece){.corner_count = shape->count, .end = 1};
    for (int i = 0; i < shape->count; i++) {
        piece->corners[i][0] = shape->points[i][0];
        piece->corners[i][1] = shape->points[i][1];
        piece->center_x += shape->points[i][0] / shape->count;
        piece->center_y += shape->points[i][1] / shape->count;
    }
    return piece;
}

static addon_polygon addon_make_rect(double left, double top, double right, double bottom) {
    return (addon_polygon){4, {{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
}

static xd_addon_hinge *addon_add_hinge(xd_addon_piece *piece) {
    XD_ASSERT(piece->hinge_count < XD_ADDON_MAX_HINGES);
    xd_addon_hinge *hinge = &piece->hinges[piece->hinge_count++];
    *hinge = (xd_addon_hinge){0};
    return hinge;
}

static void addon_set_hinge_line(xd_addon_hinge *hinge, double x0, double y0, double x1,
                                 double y1) {
    hinge->point[0] = x0;
    hinge->point[1] = y0;
    hinge->point[2] = 0;
    hinge->axis[0] = x1 - x0;
    hinge->axis[1] = y1 - y0;
    hinge->axis[2] = 0;
    addon_normalize(hinge->axis);
}

/* Pieces across x down, about `size` px each, at least one. */
static int addon_count_cells(double length, double size, int most) {
    double cells = round(length / XD_MAX(size, 1.0));
    return (int)XD_CLAMP(cells, 1.0, (double)most);
}

/* ---- options -------------------------------------------------------------------- */

void xd_addon_get_default_options(xd_addon_options *options) {
    *options = (xd_addon_options){
        .airplane_path_length = 1.0,
        .airplane_fly_to_taskbar = true,
        .beam_size = 8,
        .beam_spacing = 5,
        .beam_life = 0.5,
        .beam_slowdown = 1.0,
        .domino_direction = XD_ADDON_DOWN,
        .razr_direction = XD_ADDON_UP,
        .domino_piece_size = 60,
        .razr_piece_size = 60,
        .fold_columns = 3,
        .fold_rows = 3,
        .fold_count = 4,
        .fold_out = false,
        .glide_away_position = 0.4,
        .glide_away_angle = 45,
        .glide_thickness = 0,
        .leaf_spread = 1.0,
        .skewer_direction = XD_ADDON_LEFT_RIGHT,
        .skewer_tessellation = XD_EXPLODE_RECTANGULAR,
        .skewer_across = 6,
        .skewer_down = 4,
        .skewer_thickness = 10,
        .skewer_rotation = 90,
        .skewer_depth = 0.3,
    };
}

void xd_addon_clamp_options(xd_addon_options *options) {
    options->airplane_path_length = XD_CLAMP(options->airplane_path_length, 0.2, 3.0);
    options->beam_size = XD_CLAMP(options->beam_size, 1.0, 64.0);
    options->beam_spacing = XD_CLAMP(options->beam_spacing, 1.0, 64.0);
    options->beam_life = XD_CLAMP(options->beam_life, 0.1, 1.0);
    options->beam_slowdown = XD_CLAMP(options->beam_slowdown, 0.0, 10.0);
    if (options->domino_direction > XD_ADDON_RIGHT &&
        options->domino_direction != XD_ADDON_RANDOM)
        options->domino_direction = XD_ADDON_DOWN;
    if (options->razr_direction > XD_ADDON_RIGHT && options->razr_direction != XD_ADDON_RANDOM)
        options->razr_direction = XD_ADDON_UP;
    options->domino_piece_size = XD_CLAMP(options->domino_piece_size, 10.0, 400.0);
    options->razr_piece_size = XD_CLAMP(options->razr_piece_size, 10.0, 400.0);
    options->fold_columns = XD_CLAMP(options->fold_columns, 1, 10);
    options->fold_rows = XD_CLAMP(options->fold_rows, 1, 10);
    options->fold_count = XD_CLAMP(options->fold_count, 0, 18);
    options->glide_away_position = XD_CLAMP(options->glide_away_position, 0.0, 2.0);
    options->glide_away_angle = XD_CLAMP(options->glide_away_angle, -180.0, 180.0);
    options->glide_thickness = XD_CLAMP(options->glide_thickness, 0.0, 100.0);
    options->leaf_spread = XD_CLAMP(options->leaf_spread, 0.1, 5.0);
    if (options->skewer_direction > XD_ADDON_RANDOM)
        options->skewer_direction = XD_ADDON_LEFT_RIGHT;
    if (options->skewer_tessellation > XD_EXPLODE_HEXAGONAL)
        options->skewer_tessellation = XD_EXPLODE_RECTANGULAR;
    options->skewer_across = XD_CLAMP(options->skewer_across, 1, 50);
    options->skewer_down = XD_CLAMP(options->skewer_down, 1, 50);
    options->skewer_thickness = XD_CLAMP(options->skewer_thickness, 0.0, 100.0);
    options->skewer_rotation = XD_CLAMP(options->skewer_rotation, -720.0, 720.0);
    options->skewer_depth = XD_CLAMP(options->skewer_depth, 0.0, 1.0);
}

double xd_addon_get_eye_distance(double height) {
    /* Half the height over tan(30 degrees). */
    return height * 0.86602540378443865;
}

void xd_addon_set_eye(xd_addon *addon, double x, double y, double distance) {
    addon->eye[0] = x;
    addon->eye[1] = y;
    addon->eye[2] = XD_MAX(distance, 1.0);
}

/* ---- the cuts ------------------------------------------------------------------- */

/* A paper plane: the top corners fold down onto the window, it folds in
 * half along its middle (the halves going back, a keel) and the wings
 * fold out flat again; seen from the front, a plane seen from above. */
static void addon_build_airplane(xd_addon *addon) {
    double width = addon->width, height = addon->height;
    double middle = width / 2;
    double flap = XD_MIN(middle, height * 0.6); /* how far down the sides the corners fold */
    double wing = middle * 0.55;                /* the wing folds' way out at the bottom */
    addon_polygon body = {5, {{0, flap}, {middle, 0}, {width, flap}, {width, height}, {0, height}}};
    addon_polygon flap_left = {3, {{0, 0}, {middle, 0}, {0, flap}}};
    addon_polygon flap_right = {3, {{middle, 0}, {width, 0}, {width, flap}}};
    /* The flaps where they lie folded. */
    addon_reflect(&flap_left, middle, 0, 0, flap);
    addon_reflect(&flap_right, middle, 0, width, flap);
    const addon_polygon *layers[3] = {&body, &flap_left, &flap_right};
    for (int layer = 0; layer < 3; layer++) {
        for (int side = 0; side < 2; side++) {
            bool left = side == 0;
            /* The wing fold's line, from the nose; its outward normal. */
            double wing_x = left ? middle - wing : middle + wing;
            double normal_x = left ? -height : height, normal_y = -wing;
            double normal_length = sqrt(normal_x * normal_x + normal_y * normal_y);
            normal_x /= normal_length;
            normal_y /= normal_length;
            double limit = middle * normal_x;
            for (int part = 0; part < 2; part++) {
                bool outer = part == 1;
                addon_polygon shape = *layers[layer];
                if (left)
                    addon_clip(&shape, 1, 0, middle);
                else
                    addon_clip(&shape, -1, 0, -middle);
                if (outer)
                    addon_clip(&shape, -normal_x, -normal_y, -limit);
                else
                    addon_clip(&shape, normal_x, normal_y, limit);
                if (shape.count < 3 || addon_get_area(&shape) < 0.5)
                    continue;
                double folded_x = 0, folded_y = 0;
                for (int i = 0; i < shape.count; i++) {
                    folded_x += shape.points[i][0] / shape.count;
                    folded_y += shape.points[i][1] / shape.count;
                }
                /* A flap's piece is cut where it lies folded; where it is
                 * in the window is that reflected back. */
                addon_polygon rest = shape;
                if (layer == 1)
                    addon_reflect(&rest, middle, 0, 0, flap);
                else if (layer == 2)
                    addon_reflect(&rest, middle, 0, width, flap);
                xd_addon_piece *piece = addon_add_piece(addon, &rest);
                if (piece == nullptr)
                    continue;
                if (layer > 0) {
                    xd_addon_hinge *hinge = addon_add_hinge(piece);
                    addon_set_hinge_line(hinge, middle, 0, layer == 1 ? 0 : width, flap);
                    hinge->angle = addon_choose_angle(hinge->point, hinge->axis, ADDON_PI,
                                                      piece->center_x, piece->center_y, true);
                    hinge->start = 0;
                    hinge->end = 0.25;
                    hinge->lift = 1;
                }
                if (outer) {
                    xd_addon_hinge *hinge = addon_add_hinge(piece);
                    addon_set_hinge_line(hinge, middle, 0, wing_x, height);
                    hinge->angle = addon_choose_angle(hinge->point, hinge->axis,
                                                      80 * ADDON_PI / 180, folded_x, folded_y,
                                                      true);
                    hinge->start = 0.3;
                    hinge->end = 0.55;
                }
                xd_addon_hinge *half = addon_add_hinge(piece);
                addon_set_hinge_line(half, middle, 0, middle, height);
                half->angle = addon_choose_angle(half->point, half->axis, 80 * ADDON_PI / 180,
                                                 folded_x, folded_y, false);
                half->start = 0.2;
                half->end = 0.45;
            }
        }
    }
}

/* Strips, so the window can fade from the bottom up, and the beams. */
static void addon_build_beam_up(xd_addon *addon, uint64_t *seed) {
    double width = addon->width, height = addon->height;
    double strips_wanted = round(height / 12);
    int strips = (int)XD_CLAMP(strips_wanted, 8.0, 64.0);
    for (int i = 0; i < strips; i++) {
        addon_polygon strip =
            addon_make_rect(0, height * i / strips, width, height * (i + 1) / strips);
        addon_add_piece(addon, &strip);
    }
    const xd_addon_options *options = &addon->options;
    double pitch = options->beam_size + options->beam_spacing;
    double columns_wanted = floor(width / pitch);
    size_t columns = (size_t)XD_CLAMP(columns_wanted, 1.0, (double)(MAX_BEAMS / 2));
    double margin = (width - pitch * (double)columns) / 2;
    addon->beams = XD_NEW_ARRAY(xd_addon_beam, columns * 2);
    for (size_t column = 0; column < columns; column++) {
        for (int twice = 0; twice < 2; twice++) {
            xd_addon_beam *beam = &addon->beams[addon->beam_count++];
            beam->x = margin + pitch * ((double)column + 0.5);
            beam->y = height * (0.15 + 0.85 * addon_get_random(seed));
            beam->length = height * (0.15 + 0.35 * addon_get_random(seed));
            beam->start = 0.05 + 0.45 * addon_get_random(seed);
            double life = options->beam_life * (0.6 + 0.4 * addon_get_random(seed));
            beam->life = XD_MAX(XD_MIN(life, 1 - beam->start), 0.05);
            beam->speed = height * (0.6 + 0.8 * addon_get_random(seed));
            beam->brightness = 0.5 + 0.5 * addon_get_random(seed);
        }
    }
}

/* Where along `direction` a piece is: 0 for the first to go, 1 the last. */
static double addon_get_order(xd_addon_direction direction, int column, int row, int across,
                              int down) {
    double last_column = XD_MAX(across - 1, 1), last_row = XD_MAX(down - 1, 1);
    switch (direction) {
    case XD_ADDON_UP:
        return (down - 1 - row) / last_row;
    case XD_ADDON_LEFT:
        return (across - 1 - column) / last_column;
    case XD_ADDON_RIGHT:
        return column / last_column;
    case XD_ADDON_DOWN:
    case XD_ADDON_IN:
    case XD_ADDON_OUT:
    case XD_ADDON_LEFT_RIGHT:
    case XD_ADDON_UP_DOWN:
    case XD_ADDON_IN_OUT:
    case XD_ADDON_RANDOM:
        break;
    }
    return row / last_row;
}

/* Domino and Razr: a grid of pieces, each turning toward the viewer
 * about its edge on the side they go, one row (or column) after another:
 * a domino falls a quarter turn and drops; a Razr piece flips over like a
 * phone's lid. */
static void addon_build_dominoes(xd_addon *addon, bool razr, uint64_t *seed) {
    double width = addon->width, height = addon->height;
    double size = razr ? addon->options.razr_piece_size : addon->options.domino_piece_size;
    int across = addon_count_cells(width, size, 64), down = addon_count_cells(height, size, 64);
    double cell_width = width / across, cell_height = height / down;
    xd_addon_direction direction = addon->direction;
    addon->thickness = razr ? 3 : XD_CLAMP(XD_MIN(cell_width, cell_height) * 0.12, 2.0, 12.0);
    for (int row = 0; row < down; row++) {
        for (int column = 0; column < across; column++) {
            double left = column * cell_width, top = row * cell_height;
            double right = left + cell_width, bottom = top + cell_height;
            addon_polygon rect = addon_make_rect(left, top, right, bottom);
            xd_addon_piece *piece = addon_add_piece(addon, &rect);
            if (piece == nullptr)
                continue;
            xd_addon_hinge *hinge = addon_add_hinge(piece);
            switch (direction) {
            case XD_ADDON_UP:
                addon_set_hinge_line(hinge, left, top, right, top);
                piece->direction[1] = -1;
                break;
            case XD_ADDON_LEFT:
                addon_set_hinge_line(hinge, left, top, left, bottom);
                piece->direction[0] = -1;
                break;
            case XD_ADDON_RIGHT:
                addon_set_hinge_line(hinge, right, top, right, bottom);
                piece->direction[0] = 1;
                break;
            case XD_ADDON_DOWN:
            case XD_ADDON_IN:
            case XD_ADDON_OUT:
            case XD_ADDON_LEFT_RIGHT:
            case XD_ADDON_UP_DOWN:
            case XD_ADDON_IN_OUT:
            case XD_ADDON_RANDOM:
                addon_set_hinge_line(hinge, left, bottom, right, bottom);
                piece->direction[1] = 1;
                break;
            }
            double order = addon_get_order(direction, column, row, across, down);
            double jitter = addon_get_random(seed);
            hinge->start = order * (razr ? 0.55 : 0.5) + jitter * 0.05;
            hinge->end = hinge->start + (razr ? 0.4 : 0.3);
            hinge->angle = addon_choose_angle(hinge->point, hinge->axis,
                                              razr ? ADDON_PI : ADDON_PI / 2, piece->center_x,
                                              piece->center_y, true);
            /* A domino down drops the way it fell. */
            piece->start = razr ? hinge->end : hinge->end - 0.05;
            piece->end = XD_MIN(piece->start + 0.25, 1.0);
            piece->distance = razr ? 0 : XD_MAX(cell_width, cell_height) * 1.2;
        }
    }
}

/* A grid folded along its lines, one after another: the bottom row onto
 * the one above it, then those two onto the next... then the columns,
 * right to left; what's left fades. */
static void addon_build_fold(xd_addon *addon) {
    const xd_addon_options *options = &addon->options;
    int columns = options->fold_columns, rows = options->fold_rows;
    double cell_width = addon->width / columns, cell_height = addon->height / rows;
    int folds = XD_MIN(options->fold_count, (rows - 1) + (columns - 1));
    double stage = 0.8 / XD_MAX(folds, 1);
    bool out = options->fold_out;
    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            addon_polygon rect =
                addon_make_rect(column * cell_width, row * cell_height, (column + 1) * cell_width,
                                (row + 1) * cell_height);
            xd_addon_piece *piece = addon_add_piece(addon, &rect);
            if (piece == nullptr)
                continue;
            /* The folds nearest it first: each one's line is on the part
             * that stays, so they compose in the window's frame. */
            for (int line = row; line >= 1; line--) {
                int order = rows - 1 - line;
                if (order >= folds)
                    continue;
                xd_addon_hinge *hinge = addon_add_hinge(piece);
                addon_set_hinge_line(hinge, 0, line * cell_height, addon->width,
                                     line * cell_height);
                hinge->angle = addon_choose_angle(hinge->point, hinge->axis, ADDON_PI, 0,
                                                  (line + 0.5) * cell_height, out);
                hinge->start = order * stage;
                hinge->end = (order + 1) * stage;
                hinge->lift = (order + 1) * (out ? 0.6 : -0.6);
            }
            for (int line = column; line >= 1; line--) {
                int order = (rows - 1) + (columns - 1 - line);
                if (order >= folds)
                    continue;
                xd_addon_hinge *hinge = addon_add_hinge(piece);
                addon_set_hinge_line(hinge, line * cell_width, 0, line * cell_width,
                                     addon->height);
                hinge->angle = addon_choose_angle(hinge->point, hinge->axis, ADDON_PI,
                                                  (line + 0.5) * cell_width, 0, out);
                hinge->start = order * stage;
                hinge->end = (order + 1) * stage;
                hinge->lift = (order + 1) * (out ? 0.6 : -0.6);
            }
            piece->start = folds > 0 ? folds * stage - 0.05 : 0;
            piece->end = 1;
        }
    }
}

/* One piece, tipping back about its middle as it goes away. */
static void addon_build_glide(xd_addon *addon) {
    const xd_addon_options *options = &addon->options;
    addon_polygon rect = addon_make_rect(0, 0, addon->width, addon->height);
    xd_addon_piece *piece = addon_add_piece(addon, &rect);
    if (piece == nullptr)
        return;
    addon->thickness = options->glide_thickness;
    xd_addon_hinge *hinge = addon_add_hinge(piece);
    addon_set_hinge_line(hinge, 0, addon->height / 2, addon->width, addon->height / 2);
    /* A positive angle tips its top away. */
    double away = addon_choose_angle(hinge->point, hinge->axis, 1, 0, 0, false);
    hinge->angle = away * options->glide_away_angle * ADDON_PI / 180;
    hinge->start = 0;
    hinge->end = 1;
    piece->direction[2] = -1;
    piece->distance = options->glide_away_position * addon->height * 2;
}

/* Small pieces drifting out from the middle, turning and falling a
 * little as they go, as leaves on the wind. */
static void addon_build_leaf_spread(xd_addon *addon, uint64_t *seed) {
    double width = addon->width, height = addon->height;
    int across = (int)XD_CLAMP(round(width / 50), 3.0, 40.0);
    int down = (int)XD_CLAMP(round(height / 50), 3.0, 40.0);
    double cell_width = width / across, cell_height = height / down;
    double diagonal = sqrt(width * width + height * height);
    for (int row = 0; row < down; row++) {
        for (int column = 0; column < across; column++) {
            addon_polygon rect =
                addon_make_rect(column * cell_width, row * cell_height, (column + 1) * cell_width,
                                (row + 1) * cell_height);
            xd_addon_piece *piece = addon_add_piece(addon, &rect);
            if (piece == nullptr)
                continue;
            double angle = atan2(piece->center_y - height / 2, piece->center_x - width / 2) +
                           (addon_get_random(seed) - 0.5) * 1.6;
            piece->direction[0] = cos(angle);
            piece->direction[1] = sin(angle);
            piece->direction[2] = (addon_get_random(seed) - 0.5) * 0.4;
            addon_normalize(piece->direction);
            piece->distance =
                addon->options.leaf_spread * diagonal * (0.2 + 0.4 * addon_get_random(seed));
            piece->axis[0] = addon_get_random(seed) - 0.5;
            piece->axis[1] = addon_get_random(seed) - 0.5;
            piece->axis[2] = addon_get_random(seed) - 0.5;
            addon_normalize(piece->axis);
            piece->spin = (1 + 2 * addon_get_random(seed)) * ADDON_PI *
                          (addon_get_random(seed) < 0.5 ? -1 : 1);
            piece->sway = addon_get_random(seed) * 2 * ADDON_PI;
            piece->start = addon_get_random(seed) * 0.3;
            piece->end = piece->start + 0.7;
        }
    }
}

/* Explode's cut (rectangles or hexagons), every piece flying off the
 * chosen way as if along a skewer, turning about it. */
static void addon_build_skewer(xd_addon *addon, uint64_t *seed) {
    const xd_addon_options *options = &addon->options;
    double width = addon->width, height = addon->height;
    double diagonal = sqrt(width * width + height * height);
    xd_explode explode;
    xd_explode_init(&explode, options->skewer_tessellation, width, height, options->skewer_across,
                    options->skewer_down, 3, 1, *seed);
    addon->thickness = options->skewer_thickness;
    for (size_t i = 0; i < explode.count; i++) {
        const xd_explode_piece *cut = &explode.pieces[i];
        addon_polygon shape = {.count = cut->corner_count};
        for (int corner = 0; corner < cut->corner_count; corner++) {
            shape.points[corner][0] = cut->corners[corner][0];
            shape.points[corner][1] = cut->corners[corner][1];
        }
        xd_addon_piece *piece = addon_add_piece(addon, &shape);
        if (piece == nullptr)
            continue;
        double chance = addon_get_random(seed);
        xd_addon_direction way = addon->direction;
        if (way == XD_ADDON_LEFT_RIGHT)
            way = chance < 0.5 ? XD_ADDON_LEFT : XD_ADDON_RIGHT;
        else if (way == XD_ADDON_UP_DOWN)
            way = chance < 0.5 ? XD_ADDON_UP : XD_ADDON_DOWN;
        else if (way == XD_ADDON_IN_OUT)
            way = chance < 0.5 ? XD_ADDON_IN : XD_ADDON_OUT;
        double extra = addon_get_random(seed);
        double travel[3] = {0, 0, 0};
        switch (way) {
        case XD_ADDON_LEFT:
            travel[0] = -(piece->center_x + width * (0.3 + 0.5 * extra));
            break;
        case XD_ADDON_RIGHT:
            travel[0] = width - piece->center_x + width * (0.3 + 0.5 * extra);
            break;
        case XD_ADDON_UP:
            travel[1] = -(piece->center_y + height * (0.3 + 0.5 * extra));
            break;
        case XD_ADDON_IN:
            travel[2] = -diagonal * (1.0 + extra);
            break;
        case XD_ADDON_OUT:
            travel[2] = diagonal * (0.3 + 0.2 * extra);
            break;
        case XD_ADDON_DOWN:
        case XD_ADDON_LEFT_RIGHT:
        case XD_ADDON_UP_DOWN:
        case XD_ADDON_IN_OUT:
        case XD_ADDON_RANDOM:
            travel[1] = height - piece->center_y + height * (0.3 + 0.5 * extra);
            break;
        }
        /* The skewer is the way it flies; Depth parts the pieces toward
         * and away from the viewer as well. */
        piece->axis[0] = travel[0];
        piece->axis[1] = travel[1];
        piece->axis[2] = travel[2];
        addon_normalize(piece->axis);
        travel[2] += (addon_get_random(seed) - 0.5) * 2 * options->skewer_depth * diagonal;
        piece->distance = sqrt(travel[0] * travel[0] + travel[1] * travel[1] +
                               travel[2] * travel[2]);
        piece->direction[0] = travel[0];
        piece->direction[1] = travel[1];
        piece->direction[2] = travel[2];
        addon_normalize(piece->direction);
        piece->spin = options->skewer_rotation * ADDON_PI / 180 *
                      (0.6 + 0.8 * addon_get_random(seed));
        piece->start = addon_get_random(seed) * 0.35;
        piece->end = piece->start + 0.65;
    }
    xd_explode_free(&explode);
}

static xd_addon_direction addon_resolve_direction(xd_addon_effect effect,
                                                  const xd_addon_options *options,
                                                  uint64_t *seed) {
    xd_addon_direction direction = XD_ADDON_DOWN;
    double chance = addon_get_random(seed);
    switch (effect) {
    case XD_ADDON_DOMINO:
    case XD_ADDON_RAZR:
        direction = effect == XD_ADDON_DOMINO ? options->domino_direction
                                              : options->razr_direction;
        if (direction == XD_ADDON_RANDOM)
            direction = (xd_addon_direction)XD_MIN((int)(chance * 4), 3);
        return direction;
    case XD_ADDON_SKEWER:
        direction = options->skewer_direction;
        if (direction == XD_ADDON_RANDOM)
            direction = (xd_addon_direction)XD_MIN((int)(chance * 9), 8);
        return direction;
    case XD_ADDON_AIRPLANE:
    case XD_ADDON_BEAM_UP:
    case XD_ADDON_FOLD:
    case XD_ADDON_GLIDE_3:
    case XD_ADDON_LEAF_SPREAD:
    case XD_ADDON_EFFECT_COUNT:
        break;
    }
    return direction;
}

bool xd_addon_init(xd_addon *addon, xd_addon_effect effect, double width, double height,
                   const xd_addon_options *options, const xd_effect_rect *target, uint64_t seed) {
    *addon = (xd_addon){.effect = effect, .width = width, .height = height, .options = *options};
    xd_addon_clamp_options(&addon->options);
    if (!(width >= 1 && height >= 1) || effect >= XD_ADDON_EFFECT_COUNT)
        return false;
    if (seed == 0)
        seed = 0x9E3779B97F4A7C15ULL;
    xd_addon_set_eye(addon, width / 2, height / 2,
                     xd_addon_get_eye_distance(XD_MAX(width, height) * 1.5));
    if (target != nullptr) {
        addon->has_target = true;
        addon->target = *target;
    }
    addon->direction = addon_resolve_direction(effect, &addon->options, &seed);
    addon->pieces = XD_NEW_ARRAY(xd_addon_piece, MAX_PIECES);
    switch (effect) {
    case XD_ADDON_AIRPLANE:
        addon_build_airplane(addon);
        break;
    case XD_ADDON_BEAM_UP:
        addon_build_beam_up(addon, &seed);
        break;
    case XD_ADDON_DOMINO:
        addon_build_dominoes(addon, false, &seed);
        break;
    case XD_ADDON_RAZR:
        addon_build_dominoes(addon, true, &seed);
        break;
    case XD_ADDON_FOLD:
        addon_build_fold(addon);
        break;
    case XD_ADDON_GLIDE_3:
        addon_build_glide(addon);
        break;
    case XD_ADDON_LEAF_SPREAD:
        addon_build_leaf_spread(addon, &seed);
        break;
    case XD_ADDON_SKEWER:
        addon_build_skewer(addon, &seed);
        break;
    case XD_ADDON_EFFECT_COUNT:
        break;
    }
    return addon->count > 0;
}

void xd_addon_free(xd_addon *addon) {
    xd_free(addon->pieces);
    xd_free(addon->beams);
    *addon = (xd_addon){0};
}

/* ---- poses ---------------------------------------------------------------------- */

static void addon_set_alpha(xd_addon_pose *pose, int count, double alpha) {
    for (int i = 0; i < count; i++)
        pose->alpha[i] = alpha;
}

/* Every corner and the normal turned by `angle` about the line through
 * `pivot` along `axis`. */
static void addon_turn_pose(xd_addon_pose *pose, int count, const double pivot[3],
                            const double axis[3], double angle) {
    for (int i = 0; i < count; i++)
        addon_rotate_about(pivot, axis, angle, pose->corners[i]);
    double normal[3];
    addon_rotate(axis, angle, pose->normal, normal);
    for (int i = 0; i < 3; i++)
        pose->normal[i] = normal[i];
}

static void addon_get_middle(const xd_addon_pose *pose, int count, double middle[3]) {
    middle[0] = middle[1] = middle[2] = 0;
    for (int i = 0; i < count; i++) {
        for (int axis = 0; axis < 3; axis++)
            middle[axis] += pose->corners[i][axis] / count;
    }
}

/* The folded plane flies off: up and away, or (minimizing) along an arc
 * into its taskbar button, shrinking to it; its nose turned the way it
 * flies. */
static void addon_fly_airplane(const xd_addon *addon, xd_addon_pose *pose, int count,
                               double progress) {
    double flight = addon_get_share(progress, 0.5, 1.0);
    if (flight <= 0)
        return;
    double width = addon->width, height = addon->height;
    double path = addon->options.airplane_path_length;
    double pivot[3] = {width / 2, height * 0.45, 0};
    double position[3], ahead[3];
    double scale = 1, pitch = 0, alpha = 1;
    if (addon->has_target) {
        const xd_effect_rect *target = &addon->target;
        double end[3] = {target->x + target->width / 2, target->y + target->height / 2, 0};
        double arc = height * 0.4 * path;
        for (int step = 0; step < 2; step++) {
            double eased = addon_ease(flight + step * 0.02);
            double *out = step == 0 ? position : ahead;
            for (int axis = 0; axis < 3; axis++)
                out[axis] = pivot[axis] + (end[axis] - pivot[axis]) * eased;
            out[1] -= sin(ADDON_PI * eased) * arc;
        }
        double end_scale = XD_MAX(target->width, target->height) / XD_MAX(width, height);
        scale = 1 + (XD_CLAMP(end_scale, 0.02, 1.0) - 1) * addon_ease(flight);
        alpha = 1 - addon_get_share(progress, 0.92, 1.0);
    } else {
        double travel = path * XD_MAX(width, height) * 1.6;
        for (int step = 0; step < 2; step++) {
            double share = flight + step * 0.02;
            double accelerated = share * share;
            double *out = step == 0 ? position : ahead;
            out[0] = pivot[0] + sin(accelerated * ADDON_PI / 2) * 0.35 * travel;
            out[1] = pivot[1] - accelerated * travel;
            out[2] = -accelerated * travel * 1.2;
        }
        pitch = 0.6 * flight * flight;
        alpha = 1 - addon_get_share(progress, 0.8, 1.0);
    }
    /* Its nose (up, at first) the way it goes, turning to it early on. */
    double heading = atan2(ahead[0] - position[0], -(ahead[1] - position[1]));
    heading *= addon_ease(flight / 0.3);
    const double across[3] = {1, 0, 0}, facing[3] = {0, 0, 1};
    for (int i = 0; i < count; i++) {
        double *corner = pose->corners[i];
        double offset[3] = {(corner[0] - pivot[0]) * scale, (corner[1] - pivot[1]) * scale,
                            (corner[2] - pivot[2]) * scale};
        double pitched[3], turned[3];
        addon_rotate(across, -pitch, offset, pitched);
        addon_rotate(facing, heading, pitched, turned);
        for (int axis = 0; axis < 3; axis++)
            corner[axis] = position[axis] + turned[axis];
    }
    double pitched[3], turned[3];
    addon_rotate(across, -pitch, pose->normal, pitched);
    addon_rotate(facing, heading, pitched, turned);
    for (int axis = 0; axis < 3; axis++)
        pose->normal[axis] = turned[axis];
    addon_set_alpha(pose, count, alpha);
}

/* The window glows with the beams' light and goes from the bottom up,
 * drawn in to a column. */
static double addon_get_beam_squeeze(double progress) {
    return 1 - 0.8 * addon_ease(addon_get_share(progress, 0.4, 0.9));
}

static void addon_pose_beam_up(const xd_addon *addon, xd_addon_pose *pose, int count,
                               double progress) {
    double middle = addon->width / 2;
    double squeeze = addon_get_beam_squeeze(progress);
    double front = 1 - (progress - 0.3) / 0.55;
    pose->glow = 0.85 * addon_ease(progress / 0.5);
    for (int i = 0; i < count; i++) {
        double *corner = pose->corners[i];
        double down = corner[1] / addon->height;
        corner[0] = middle + (corner[0] - middle) * squeeze;
        pose->alpha[i] = addon_ease((front + 0.25 - down) / 0.25);
    }
}

/* Flies `distance` along `direction` and spins about `axis`, from start
 * to end; `accelerating` sets off slowly (Skewer), else it drifts out
 * quickly and slows (Leaf Spread, whose pieces also sway and fall). */
static void addon_pose_flight(const xd_addon *addon, const xd_addon_piece *piece,
                              xd_addon_pose *pose, int count, double progress) {
    double share = addon_get_share(progress, piece->start, piece->end);
    bool leaf = addon->effect == XD_ADDON_LEAF_SPREAD;
    double travel = leaf ? 1 - (1 - share) * (1 - share) : share * share;
    double middle[3];
    addon_get_middle(pose, count, middle);
    if (piece->spin != 0)
        addon_turn_pose(pose, count, middle, piece->axis, piece->spin * share);
    double offset[3] = {piece->direction[0] * piece->distance * travel,
                        piece->direction[1] * piece->distance * travel,
                        piece->direction[2] * piece->distance * travel};
    if (leaf) {
        double cell = sqrt((piece->corners[2][0] - piece->corners[0][0]) *
                               (piece->corners[2][0] - piece->corners[0][0]) +
                           (piece->corners[2][1] - piece->corners[0][1]) *
                               (piece->corners[2][1] - piece->corners[0][1]));
        offset[0] += sin(piece->sway + share * 2 * ADDON_PI) * cell * 0.6 * share;
        offset[1] += addon->height * 0.3 * share * share;
    }
    for (int i = 0; i < count; i++) {
        for (int axis = 0; axis < 3; axis++)
            pose->corners[i][axis] += offset[axis];
    }
    addon_set_alpha(pose, count, 1 - addon_ease(addon_get_share(share, leaf ? 0.4 : 0.6, 1)));
}

void xd_addon_get_pose(const xd_addon *addon, size_t index, double progress,
                       xd_addon_pose *pose) {
    const xd_addon_piece *piece = &addon->pieces[index];
    double now = addon_clamp_share(progress);
    int count = piece->corner_count;
    *pose = (xd_addon_pose){.normal = {0, 0, 1}};
    for (int i = 0; i < count; i++) {
        pose->corners[i][0] = piece->corners[i][0];
        pose->corners[i][1] = piece->corners[i][1];
        pose->alpha[i] = 1;
    }
    double lift = 0;
    for (int i = 0; i < piece->hinge_count; i++) {
        const xd_addon_hinge *hinge = &piece->hinges[i];
        double turned = addon_ease(addon_get_share(now, hinge->start, hinge->end));
        /* Dominoes fall faster and faster. */
        if (addon->effect == XD_ADDON_DOMINO)
            turned = addon_get_share(now, hinge->start, hinge->end) *
                     addon_get_share(now, hinge->start, hinge->end);
        if (turned > 0)
            addon_turn_pose(pose, count, hinge->point, hinge->axis, hinge->angle * turned);
        lift += hinge->lift * turned;
    }
    for (int i = 0; i < count; i++)
        pose->corners[i][2] += lift;
    switch (addon->effect) {
    case XD_ADDON_AIRPLANE:
        addon_fly_airplane(addon, pose, count, now);
        break;
    case XD_ADDON_BEAM_UP:
        addon_pose_beam_up(addon, pose, count, now);
        break;
    case XD_ADDON_DOMINO: {
        double drop = addon_get_share(now, piece->start, piece->end);
        for (int i = 0; i < count; i++) {
            for (int axis = 0; axis < 3; axis++)
                pose->corners[i][axis] += piece->direction[axis] * piece->distance * drop * drop;
        }
        addon_set_alpha(pose, count, 1 - addon_ease(drop));
        break;
    }
    case XD_ADDON_RAZR: {
        /* Gone as it lands, flipped. */
        const xd_addon_hinge *hinge = &piece->hinges[0];
        double middle = (hinge->start + hinge->end) / 2;
        addon_set_alpha(pose, count, 1 - addon_ease(addon_get_share(now, middle, hinge->end)));
        break;
    }
    case XD_ADDON_FOLD:
        addon_set_alpha(pose, count, 1 - addon_ease(addon_get_share(now, piece->start, 1.0)));
        break;
    case XD_ADDON_GLIDE_3: {
        double eased = addon_ease(now);
        for (int i = 0; i < count; i++)
            pose->corners[i][2] -= piece->distance * eased;
        addon_set_alpha(pose, count, 1 - addon_ease(addon_get_share(now, 0.1, 1.0)));
        break;
    }
    case XD_ADDON_LEAF_SPREAD:
    case XD_ADDON_SKEWER:
        addon_pose_flight(addon, piece, pose, count, now);
        break;
    case XD_ADDON_EFFECT_COUNT:
        break;
    }
    if (now >= 1)
        addon_set_alpha(pose, count, 0);
}

void xd_addon_project(const xd_addon *addon, const double point[3], double *x, double *y,
                      double *grow) {
    double distance = addon->eye[2];
    /* Never at or behind the eye. */
    double depth = XD_MIN(point[2], distance * 0.8);
    double factor = distance / (distance - depth);
    *x = addon->eye[0] + (point[0] - addon->eye[0]) * factor;
    *y = addon->eye[1] + (point[1] - addon->eye[1]) * factor;
    *grow = factor;
}

bool xd_addon_get_beam(const xd_addon *addon, size_t index, double progress,
                       xd_addon_beam_state *state) {
    const xd_addon_beam *beam = &addon->beams[index];
    double now = addon_clamp_share(progress);
    double age = (now - beam->start) / beam->life;
    if (age < 0 || age > 1)
        return false;
    /* It rises, slowing by Slowdown: exponentially, as Compiz's particles
     * did. */
    double time = age * beam->life;
    double slowdown = addon->options.beam_slowdown * 3;
    double rise = slowdown > 1e-6 ? beam->speed * (1 - exp(-slowdown * time)) / slowdown
                                  : beam->speed * time;
    double middle = addon->width / 2;
    double squeeze = addon_get_beam_squeeze(now);
    *state = (xd_addon_beam_state){
        .x = middle + (beam->x - middle) * squeeze,
        .top = beam->y - rise - beam->length,
        .width = addon->options.beam_size,
        .height = beam->length,
        .brightness = beam->brightness * sin(ADDON_PI * age),
    };
    return true;
}
