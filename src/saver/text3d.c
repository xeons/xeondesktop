/* SPDX-License-Identifier: MIT
 * 3D Text, after Windows' OpenGL saver, drawn in 2D: the text (or the
 * time) extruded into a solid, turning as its Rotation type says, lit
 * from the upper left, in a solid colour or as chrome reflecting a sky
 * and the ground.
 *
 * The letters' outlines come from Pango, flattened to polygons once (each
 * second in Time mode). A frame turns them, projects them in perspective
 * and paints the sides facing the eye far to near, then the face; sides
 * of the same shade go in one fill, so a frame is a few dozen fills
 * however long the text.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pango/pangocairo.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "saver_impl.h"

static constexpr double FONT_PX = 100.0; /* the outlines' size, before fitting */
static constexpr double DEPTH = 0.32;    /* extrusion, in font sizes */

typedef struct v3 {
    double x, y, z;
} v3;

typedef struct contour {
    size_t start, count;
} contour;

typedef struct side {
    size_t i, j; /* the edge's two points */
    double depth;
    uint32_t color; /* 0xRRGGBB, rounded so that like sides fill together */
} side;

struct saver_text3d {
    uint64_t *rng;
    int w, h;
    xd_text3d_settings o;
    double k; /* the Speed trackbar as a factor */
    double time;
    time_t shown_second;
    char shown[64];

    /* The outline, centred on the origin, y down. */
    double *px, *py;
    size_t npts;
    contour *contours;
    size_t ncont;
    double width, height;
    double outward; /* 1 or -1: which side of an edge is outside the letter */

    /* Random rotation. */
    double ang[3], vel[3], target[3];
    double next_change;

    /* Scratch for a frame. */
    v3 *front, *back; /* in the world */
    double *fsx, *fsy, *bsx, *bsy; /* on the screen */
    side *sides;
};

static v3 v3_add(v3 left, v3 right) {
    return (v3){left.x + right.x, left.y + right.y, left.z + right.z};
}
static double dot(v3 left, v3 right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}
static v3 norm(v3 vector) {
    double length = sqrt(dot(vector, vector));
    return length > 0 ? (v3){vector.x / length, vector.y / length, vector.z / length} : vector;
}

/* ---- the outline ------------------------------------------------------------ */

static void free_outline(saver_text3d *text3d) {
    xd_free(text3d->px);
    xd_free(text3d->py);
    xd_free(text3d->contours);
    xd_free(text3d->front);
    xd_free(text3d->back);
    xd_free(text3d->fsx);
    xd_free(text3d->fsy);
    xd_free(text3d->bsx);
    xd_free(text3d->bsy);
    xd_free(text3d->sides);
    text3d->px = text3d->py = nullptr;
    text3d->contours = nullptr;
    text3d->front = text3d->back = nullptr;
    text3d->fsx = text3d->fsy = text3d->bsx = text3d->bsy = nullptr;
    text3d->sides = nullptr;
    text3d->npts = text3d->ncont = 0;
}

static void build_outline(saver_text3d *text3d, const char *text) {
    free_outline(text3d);
    snprintf(text3d->shown, sizeof(text3d->shown), "%s", text);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cairo = cairo_create(surf);
    /* Coarse: a side per half pixel of a 100px letter is plenty. */
    cairo_set_tolerance(cairo, 0.5);
    PangoLayout *layout = pango_cairo_create_layout(cairo);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold");
    pango_font_description_set_absolute_size(fd, FONT_PX * PANGO_SCALE);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(layout, text, -1);
    pango_cairo_layout_path(cairo, layout);
    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout, &ink, nullptr);
    cairo_path_t *path = cairo_copy_path_flat(cairo);

    size_t cap = 0;
    for (int i = 0; i < path->num_data; i += path->data[i].header.length)
        cap++;
    text3d->px = xd_alloc_array(cap + 1, sizeof(double));
    text3d->py = xd_alloc_array(cap + 1, sizeof(double));
    text3d->contours = xd_alloc_array(cap + 1, sizeof(contour));
    double cx = ink.x + ink.width / 2.0;
    double cy = ink.y + ink.height / 2.0;
    for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
        const cairo_path_data_t *cairo_path_data = &path->data[i];
        switch (cairo_path_data->header.type) {
        case CAIRO_PATH_MOVE_TO:
            text3d->contours[text3d->ncont++] = (contour){.start = text3d->npts};
            [[fallthrough]];
        case CAIRO_PATH_LINE_TO:
            if (text3d->ncont == 0)
                break;
            text3d->px[text3d->npts] = cairo_path_data[1].point.x - cx;
            text3d->py[text3d->npts] = cairo_path_data[1].point.y - cy;
            text3d->npts++;
            text3d->contours[text3d->ncont - 1].count++;
            break;
        case CAIRO_PATH_CURVE_TO: /* not in a flattened path */
        case CAIRO_PATH_CLOSE_PATH:
            break;
        }
    }
    cairo_path_destroy(path);
    g_object_unref(layout);
    cairo_destroy(cairo);
    cairo_surface_destroy(surf);

    /* Drop each contour's closing point where it repeats its first, and
     * contours too small to have sides. */
    size_t kept = 0;
    double biggest = 0;
    text3d->outward = 1;
    for (size_t contour_index = 0; contour_index < text3d->ncont; contour_index++) {
        contour ct = text3d->contours[contour_index];
        while (ct.count > 1 && text3d->px[ct.start + ct.count - 1] == text3d->px[ct.start] &&
               text3d->py[ct.start + ct.count - 1] == text3d->py[ct.start])
            ct.count--;
        if (ct.count < 3)
            continue;
        /* Fonts wind outer contours one way and holes the other, so one
         * rule finds the outside of every edge: the biggest contour is an
         * outer one. */
        double area = 0;
        for (size_t i = 0; i < ct.count; i++) {
            size_t this_point = ct.start + i;
            size_t next_point = ct.start + (i + 1) % ct.count;
            area += text3d->px[this_point] * text3d->py[next_point] -
                    text3d->px[next_point] * text3d->py[this_point];
        }
        if (fabs(area) > biggest) {
            biggest = fabs(area);
            text3d->outward = area > 0 ? 1 : -1;
        }
        text3d->contours[kept++] = ct;
    }
    text3d->ncont = kept;
    text3d->width = fmax(ink.width, FONT_PX * 0.5);
    text3d->height = fmax(ink.height, FONT_PX * 0.5);
    text3d->front = xd_alloc_array(text3d->npts + 1, sizeof(v3));
    text3d->back = xd_alloc_array(text3d->npts + 1, sizeof(v3));
    text3d->fsx = xd_alloc_array(text3d->npts + 1, sizeof(double));
    text3d->fsy = xd_alloc_array(text3d->npts + 1, sizeof(double));
    text3d->bsx = xd_alloc_array(text3d->npts + 1, sizeof(double));
    text3d->bsy = xd_alloc_array(text3d->npts + 1, sizeof(double));
    text3d->sides = xd_alloc_array(text3d->npts + 1, sizeof(side));
}

static void update_text(saver_text3d *text3d) {
    if (!text3d->o.show_time) {
        if (text3d->px == nullptr)
            build_outline(text3d, text3d->o.text);
        return;
    }
    time_t now = time(nullptr);
    if (now == text3d->shown_second && text3d->px != nullptr)
        return;
    text3d->shown_second = now;
    char buf[64];
    struct tm tm;
    localtime_r(&now, &tm);
    if (strftime(buf, sizeof(buf), "%X", &tm) == 0)
        buf[0] = '\0';
    if (text3d->px == nullptr || strcmp(buf, text3d->shown) != 0)
        build_outline(text3d, buf);
}

/* ---- motion ------------------------------------------------------------------ */

static void rotation(saver_text3d *text3d, double dt, double angles[3]) {
    double k = text3d->k;
    double seconds = text3d->time;
    angles[0] = angles[1] = angles[2] = 0;
    switch (text3d->o.rotation) {
    case XD_TEXT3D_NONE:
    case XD_TEXT3D_ROTATION_COUNT:
        break;
    case XD_TEXT3D_SPIN: /* round the upright axis */
        angles[1] = seconds * 0.9 * k;
        break;
    case XD_TEXT3D_SEESAW: /* rocking, one end up and then the other */
        angles[2] = 0.45 * sin(1.2 * k * seconds);
        angles[1] = 0.35 * sin(1.2 * k * seconds);
        break;
    case XD_TEXT3D_WOBBLE:
        angles[0] = 0.5 * sin(1.1 * k * seconds);
        angles[1] = 0.7 * sin(0.8 * k * seconds + 1.0);
        break;
    case XD_TEXT3D_RANDOM:
        /* Turning every way, easing into a new turn every few seconds. */
        if (seconds >= text3d->next_change) {
            for (int i = 0; i < 3; i++)
                text3d->target[i] = xd_saver_between(text3d->rng, -1.0, 1.0) * k;
            text3d->next_change = seconds + xd_saver_between(text3d->rng, 3.0, 6.0);
        }
        for (int i = 0; i < 3; i++) {
            text3d->vel[i] += (text3d->target[i] - text3d->vel[i]) * fmin(1.0, dt * 0.8);
            text3d->ang[i] += text3d->vel[i] * dt;
            angles[i] = text3d->ang[i];
        }
        break;
    }
}

/* R = Rz * Ry * Rx, as rows. */
static void matrix(const double angles[3], double rotation_matrix[3][3]) {
    double cx = cos(angles[0]), sx = sin(angles[0]);
    double cy = cos(angles[1]), sy = sin(angles[1]);
    double cz = cos(angles[2]), sz = sin(angles[2]);
    rotation_matrix[0][0] = cz * cy;
    rotation_matrix[0][1] = cz * sy * sx - sz * cx;
    rotation_matrix[0][2] = cz * sy * cx + sz * sx;
    rotation_matrix[1][0] = sz * cy;
    rotation_matrix[1][1] = sz * sy * sx + cz * cx;
    rotation_matrix[1][2] = sz * sy * cx - cz * sx;
    rotation_matrix[2][0] = -sy;
    rotation_matrix[2][1] = cy * sx;
    rotation_matrix[2][2] = cy * cx;
}

static v3 rotate(const double rotation_matrix[3][3], v3 vector) {
    return (v3){rotation_matrix[0][0] * vector.x + rotation_matrix[0][1] * vector.y +
                    rotation_matrix[0][2] * vector.z,
                rotation_matrix[1][0] * vector.x + rotation_matrix[1][1] * vector.y +
                    rotation_matrix[1][2] * vector.z,
                rotation_matrix[2][0] * vector.x + rotation_matrix[2][1] * vector.y +
                    rotation_matrix[2][2] * vector.z};
}

/* ---- shading ----------------------------------------------------------------- */

static xd_rgba rgb3(double red, double green, double blue) {
    return (xd_rgba){fmin(1, fmax(0, red)), fmin(1, fmax(0, green)), fmin(1, fmax(0, blue)), 1};
}

/* Reflection's world: sky above the horizon, ground below. up is how far
 * the reflected ray points up, -1..1. */
static xd_rgba environment(double up) {
    if (up >= 0) {
        double k = pow(up, 0.6);
        return xd_rgba_mix(rgb3(0.9, 0.93, 1.0), rgb3(0.12, 0.28, 0.7), k);
    }
    double k = pow(-up, 0.5);
    return xd_rgba_mix(rgb3(0.6, 0.47, 0.3), rgb3(0.12, 0.08, 0.05), k);
}

static const v3 light_dir = {-0.38, -0.55, -0.74}; /* towards the light: upper left, in front */

static double specular(v3 normal) {
    v3 halfway = norm(v3_add(light_dir, (v3){0, 0, -1}));
    return pow(fmax(0, dot(normal, halfway)), 24) * 0.6;
}

/* A face's colour, by its normal in the world (the eye looks along +z). */
static xd_rgba shade(const saver_text3d *text3d, v3 normal) {
    if (text3d->o.surface == XD_TEXT3D_REFLECTION) {
        double vn = normal.z; /* dot((0, 0, 1), n) */
        v3 reflected = {-2 * vn * normal.x, -2 * vn * normal.y, 1 - 2 * vn * normal.z};
        xd_rgba color = environment(-reflected.y);
        double sp = specular(normal);
        return rgb3(color.r + sp, color.g + sp, color.b + sp);
    }
    xd_rgba base = xd_rgba_from_rgb(text3d->o.color);
    double lighting = 0.22 + 0.78 * fmax(0, dot(normal, light_dir));
    double sp = specular(normal);
    return rgb3(base.r * lighting + sp, base.g * lighting + sp, base.b * lighting + sp);
}

static uint32_t pack(xd_rgba color) {
    /* 6 bits a channel: like sides share a fill without visible banding. */
    uint32_t red = (uint32_t)lround(color.r * 63);
    uint32_t green = (uint32_t)lround(color.g * 63);
    uint32_t blue = (uint32_t)lround(color.b * 63);
    return red << 16 | green << 8 | blue;
}

static void set_packed(cairo_t *cairo, uint32_t packed) {
    cairo_set_source_rgb(cairo, (packed >> 16 & 0xFF) / 63.0, (packed >> 8 & 0xFF) / 63.0,
                         (packed & 0xFF) / 63.0);
}

static int far_first(const void *left, const void *right) {
    const side *left_side = left;
    const side *right_side = right;
    if (left_side->depth != right_side->depth)
        return left_side->depth > right_side->depth ? -1 : 1;
    return left_side->i < right_side->i ? -1 : left_side->i > right_side->i;
}

/* ---- a frame ------------------------------------------------------------------- */

static void draw_sides(cairo_t *cairo, const saver_text3d *text3d, const side *sides,
                       size_t count) {
    for (size_t run_start = 0; run_start < count;) {
        size_t run_end = run_start;
        cairo_new_path(cairo);
        while (run_end < count && sides[run_end].color == sides[run_start].color) {
            size_t i = sides[run_end].i;
            size_t j = sides[run_end].j;
            cairo_move_to(cairo, text3d->fsx[i], text3d->fsy[i]);
            cairo_line_to(cairo, text3d->fsx[j], text3d->fsy[j]);
            cairo_line_to(cairo, text3d->bsx[j], text3d->bsy[j]);
            cairo_line_to(cairo, text3d->bsx[i], text3d->bsy[i]);
            cairo_close_path(cairo);
            run_end++;
        }
        set_packed(cairo, sides[run_start].color);
        /* The outline too: no hairline gaps between neighbouring sides. */
        cairo_fill_preserve(cairo);
        cairo_stroke(cairo);
        run_start = run_end;
    }
}

static void draw_face(cairo_t *cairo, const saver_text3d *text3d, bool front, v3 normal,
                      const double rotation_matrix[3][3], double distance, double focal) {
    const double *sx = front ? text3d->fsx : text3d->bsx;
    const double *sy = front ? text3d->fsy : text3d->bsy;
    cairo_new_path(cairo);
    for (size_t contour_index = 0; contour_index < text3d->ncont; contour_index++) {
        const contour *ct = &text3d->contours[contour_index];
        cairo_move_to(cairo, sx[ct->start], sy[ct->start]);
        for (size_t i = 1; i < ct->count; i++)
            cairo_line_to(cairo, sx[ct->start + i], sy[ct->start + i]);
        cairo_close_path(cairo);
    }
    cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_WINDING);
    if (text3d->o.surface != XD_TEXT3D_REFLECTION) {
        set_packed(cairo, pack(shade(text3d, normal)));
        cairo_fill(cairo);
        return;
    }
    /* Chrome: a flat face still reflects the horizon going by, top to
     * bottom of the letters. */
    double z = (front ? -0.5 : 0.5) * DEPTH * FONT_PX;
    v3 top = v3_add(rotate(rotation_matrix, (v3){0, -text3d->height / 2, z}), (v3){0, 0, distance});
    v3 bottom =
        v3_add(rotate(rotation_matrix, (v3){0, text3d->height / 2, z}), (v3){0, 0, distance});
    cairo_pattern_t *pat = cairo_pattern_create_linear(
        text3d->w / 2.0 + top.x * focal / top.z, text3d->h / 2.0 + top.y * focal / top.z,
        text3d->w / 2.0 + bottom.x * focal / bottom.z,
        text3d->h / 2.0 + bottom.y * focal / bottom.z);
    double vn = normal.z;
    double up = -(-2 * vn * normal.y);
    double sp = specular(normal);
    for (int i = 0; i <= 6; i++) {
        double at = i / 6.0;
        xd_rgba color = environment(fmin(1, fmax(-1, up + 0.45 - 0.9 * at)));
        cairo_pattern_add_color_stop_rgb(pat, at, fmin(1, color.r + sp), fmin(1, color.g + sp),
                                         fmin(1, color.b + sp));
    }
    cairo_set_source(cairo, pat);
    cairo_fill(cairo);
    cairo_pattern_destroy(pat);
}

void saver_text3d_frame(saver_text3d *text3d, cairo_t *cairo, double dt) {
    text3d->time += dt;
    update_text(text3d);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_paint(cairo);
    if (text3d->ncont == 0)
        return;
    double angles[3];
    rotation(text3d, dt, angles);
    double rotation_matrix[3][3];
    matrix(angles, rotation_matrix);

    /* Fit the text to the Size trackbar's share of the screen. */
    double share = 0.35 + 0.06 * text3d->o.size;
    double distance = 2.5 * fmax(text3d->width, text3d->height * 2);
    double focal =
        fmin(share * text3d->w / text3d->width, 0.7 * text3d->h / text3d->height) * distance;
    double half = DEPTH * FONT_PX / 2;
    v3 eye_to = {0, 0, distance};
    for (size_t i = 0; i < text3d->npts; i++) {
        v3 fr = v3_add(rotate(rotation_matrix, (v3){text3d->px[i], text3d->py[i], -half}), eye_to);
        v3 bk = v3_add(rotate(rotation_matrix, (v3){text3d->px[i], text3d->py[i], half}), eye_to);
        text3d->front[i] = fr;
        text3d->back[i] = bk;
        text3d->fsx[i] = text3d->w / 2.0 + fr.x * focal / fr.z;
        text3d->fsy[i] = text3d->h / 2.0 + fr.y * focal / fr.z;
        text3d->bsx[i] = text3d->w / 2.0 + bk.x * focal / bk.z;
        text3d->bsy[i] = text3d->h / 2.0 + bk.y * focal / bk.z;
    }

    /* The sides that face the eye. */
    size_t count = 0;
    for (size_t contour_index = 0; contour_index < text3d->ncont; contour_index++) {
        const contour *ct = &text3d->contours[contour_index];
        for (size_t k = 0; k < ct->count; k++) {
            size_t i = ct->start + k;
            size_t j = ct->start + (k + 1) % ct->count;
            double ex = text3d->px[j] - text3d->px[i];
            double ey = text3d->py[j] - text3d->py[i];
            v3 nw =
                norm(rotate(rotation_matrix, (v3){ey * text3d->outward, -ex * text3d->outward, 0}));
            v3 mid = {(text3d->front[i].x + text3d->back[j].x) / 2,
                      (text3d->front[i].y + text3d->back[j].y) / 2,
                      (text3d->front[i].z + text3d->back[j].z) / 2};
            if (dot(nw, mid) >= 0)
                continue;
            text3d->sides[count++] = (side){
                .i = i, .j = j, .depth = sqrt(dot(mid, mid)), .color = pack(shade(text3d, nw))};
        }
    }
    qsort(text3d->sides, count, sizeof(*text3d->sides), far_first);

    /* Then the face towards the eye: every side lies behind its plane, so
     * nothing can cover it. */
    v3 fn = rotate(rotation_matrix, (v3){0, 0, -1});
    v3 fc = v3_add(rotate(rotation_matrix, (v3){0, 0, -half}), eye_to);
    bool front = dot(fn, fc) < 0;
    v3 face_n = front ? fn : (v3){-fn.x, -fn.y, -fn.z};
    cairo_save(cairo);
    cairo_set_line_width(cairo, 1.0);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_BEVEL);
    draw_sides(cairo, text3d, text3d->sides, count);
    draw_face(cairo, text3d, front, face_n, rotation_matrix, distance, focal);
    cairo_restore(cairo);
}

saver_text3d *saver_text3d_create(const xd_saver_settings *saver_settings, int width, int height,
                                  uint64_t *rng) {
    saver_text3d *text3d = XD_NEW(saver_text3d);
    text3d->rng = rng;
    text3d->w = width;
    text3d->h = height;
    text3d->o = saver_settings->text3d;
    double pos = (double)text3d->o.speed / (double)XD_SAVER_SLIDER_MAX;
    text3d->k = 0.2 + 2.3 * pos; /* turns a second, roughly */
    update_text(text3d);
    return text3d;
}

void saver_text3d_destroy(saver_text3d *text3d) {
    free_outline(text3d);
    xd_free(text3d);
}
