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

static v3 v3_add(v3 a, v3 b) { return (v3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static double dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 norm(v3 a) {
    double l = sqrt(dot(a, a));
    return l > 0 ? (v3){a.x / l, a.y / l, a.z / l} : a;
}

/* ---- the outline ------------------------------------------------------------ */

static void free_outline(saver_text3d *t) {
    xd_free(t->px);
    xd_free(t->py);
    xd_free(t->contours);
    xd_free(t->front);
    xd_free(t->back);
    xd_free(t->fsx);
    xd_free(t->fsy);
    xd_free(t->bsx);
    xd_free(t->bsy);
    xd_free(t->sides);
    t->px = t->py = nullptr;
    t->contours = nullptr;
    t->front = t->back = nullptr;
    t->fsx = t->fsy = t->bsx = t->bsy = nullptr;
    t->sides = nullptr;
    t->npts = t->ncont = 0;
}

static void build_outline(saver_text3d *t, const char *text) {
    free_outline(t);
    snprintf(t->shown, sizeof(t->shown), "%s", text);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(surf);
    /* Coarse: a side per half pixel of a 100px letter is plenty. */
    cairo_set_tolerance(cr, 0.5);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold");
    pango_font_description_set_absolute_size(fd, FONT_PX * PANGO_SCALE);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(layout, text, -1);
    pango_cairo_layout_path(cr, layout);
    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout, &ink, nullptr);
    cairo_path_t *path = cairo_copy_path_flat(cr);

    size_t cap = 0;
    for (int i = 0; i < path->num_data; i += path->data[i].header.length)
        cap++;
    t->px = xd_alloc_array(cap + 1, sizeof(double));
    t->py = xd_alloc_array(cap + 1, sizeof(double));
    t->contours = xd_alloc_array(cap + 1, sizeof(contour));
    double cx = ink.x + ink.width / 2.0;
    double cy = ink.y + ink.height / 2.0;
    for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
        const cairo_path_data_t *d = &path->data[i];
        switch (d->header.type) {
        case CAIRO_PATH_MOVE_TO:
            t->contours[t->ncont++] = (contour){.start = t->npts};
            [[fallthrough]];
        case CAIRO_PATH_LINE_TO:
            if (t->ncont == 0)
                break;
            t->px[t->npts] = d[1].point.x - cx;
            t->py[t->npts] = d[1].point.y - cy;
            t->npts++;
            t->contours[t->ncont - 1].count++;
            break;
        case CAIRO_PATH_CURVE_TO: /* not in a flattened path */
        case CAIRO_PATH_CLOSE_PATH:
            break;
        }
    }
    cairo_path_destroy(path);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    /* Drop each contour's closing point where it repeats its first, and
     * contours too small to have sides. */
    size_t kept = 0;
    double biggest = 0;
    t->outward = 1;
    for (size_t c = 0; c < t->ncont; c++) {
        contour ct = t->contours[c];
        while (ct.count > 1 && t->px[ct.start + ct.count - 1] == t->px[ct.start] &&
               t->py[ct.start + ct.count - 1] == t->py[ct.start])
            ct.count--;
        if (ct.count < 3)
            continue;
        /* Fonts wind outer contours one way and holes the other, so one
         * rule finds the outside of every edge: the biggest contour is an
         * outer one. */
        double area = 0;
        for (size_t i = 0; i < ct.count; i++) {
            size_t a = ct.start + i;
            size_t b = ct.start + (i + 1) % ct.count;
            area += t->px[a] * t->py[b] - t->px[b] * t->py[a];
        }
        if (fabs(area) > biggest) {
            biggest = fabs(area);
            t->outward = area > 0 ? 1 : -1;
        }
        t->contours[kept++] = ct;
    }
    t->ncont = kept;
    t->width = fmax(ink.width, FONT_PX * 0.5);
    t->height = fmax(ink.height, FONT_PX * 0.5);
    t->front = xd_alloc_array(t->npts + 1, sizeof(v3));
    t->back = xd_alloc_array(t->npts + 1, sizeof(v3));
    t->fsx = xd_alloc_array(t->npts + 1, sizeof(double));
    t->fsy = xd_alloc_array(t->npts + 1, sizeof(double));
    t->bsx = xd_alloc_array(t->npts + 1, sizeof(double));
    t->bsy = xd_alloc_array(t->npts + 1, sizeof(double));
    t->sides = xd_alloc_array(t->npts + 1, sizeof(side));
}

static void update_text(saver_text3d *t) {
    if (!t->o.show_time) {
        if (t->px == nullptr)
            build_outline(t, t->o.text);
        return;
    }
    time_t now = time(nullptr);
    if (now == t->shown_second && t->px != nullptr)
        return;
    t->shown_second = now;
    char buf[64];
    struct tm tm;
    localtime_r(&now, &tm);
    if (strftime(buf, sizeof(buf), "%X", &tm) == 0)
        buf[0] = '\0';
    if (t->px == nullptr || strcmp(buf, t->shown) != 0)
        build_outline(t, buf);
}

/* ---- motion ------------------------------------------------------------------ */

static void rotation(saver_text3d *t, double dt, double a[3]) {
    double k = t->k;
    double s = t->time;
    a[0] = a[1] = a[2] = 0;
    switch (t->o.rotation) {
    case XD_TEXT3D_NONE:
    case XD_TEXT3D_ROTATION_COUNT:
        break;
    case XD_TEXT3D_SPIN: /* round the upright axis */
        a[1] = s * 0.9 * k;
        break;
    case XD_TEXT3D_SEESAW: /* rocking, one end up and then the other */
        a[2] = 0.45 * sin(1.2 * k * s);
        a[1] = 0.35 * sin(1.2 * k * s);
        break;
    case XD_TEXT3D_WOBBLE:
        a[0] = 0.5 * sin(1.1 * k * s);
        a[1] = 0.7 * sin(0.8 * k * s + 1.0);
        break;
    case XD_TEXT3D_RANDOM:
        /* Turning every way, easing into a new turn every few seconds. */
        if (s >= t->next_change) {
            for (int i = 0; i < 3; i++)
                t->target[i] = xd_saver_between(t->rng, -1.0, 1.0) * k;
            t->next_change = s + xd_saver_between(t->rng, 3.0, 6.0);
        }
        for (int i = 0; i < 3; i++) {
            t->vel[i] += (t->target[i] - t->vel[i]) * fmin(1.0, dt * 0.8);
            t->ang[i] += t->vel[i] * dt;
            a[i] = t->ang[i];
        }
        break;
    }
}

/* R = Rz * Ry * Rx, as rows. */
static void matrix(const double a[3], double m[3][3]) {
    double cx = cos(a[0]), sx = sin(a[0]);
    double cy = cos(a[1]), sy = sin(a[1]);
    double cz = cos(a[2]), sz = sin(a[2]);
    m[0][0] = cz * cy;
    m[0][1] = cz * sy * sx - sz * cx;
    m[0][2] = cz * sy * cx + sz * sx;
    m[1][0] = sz * cy;
    m[1][1] = sz * sy * sx + cz * cx;
    m[1][2] = sz * sy * cx - cz * sx;
    m[2][0] = -sy;
    m[2][1] = cy * sx;
    m[2][2] = cy * cx;
}

static v3 rotate(const double m[3][3], v3 v) {
    return (v3){m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

/* ---- shading ----------------------------------------------------------------- */

static xd_rgba rgb3(double r, double g, double b) {
    return (xd_rgba){fmin(1, fmax(0, r)), fmin(1, fmax(0, g)), fmin(1, fmax(0, b)), 1};
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

static double specular(v3 n) {
    v3 h = norm(v3_add(light_dir, (v3){0, 0, -1}));
    return pow(fmax(0, dot(n, h)), 24) * 0.6;
}

/* A face's colour, by its normal in the world (the eye looks along +z). */
static xd_rgba shade(const saver_text3d *t, v3 n) {
    if (t->o.surface == XD_TEXT3D_REFLECTION) {
        double vn = n.z; /* dot((0, 0, 1), n) */
        v3 r = {-2 * vn * n.x, -2 * vn * n.y, 1 - 2 * vn * n.z};
        xd_rgba c = environment(-r.y);
        double sp = specular(n);
        return rgb3(c.r + sp, c.g + sp, c.b + sp);
    }
    xd_rgba base = xd_rgba_from_rgb(t->o.color);
    double l = 0.22 + 0.78 * fmax(0, dot(n, light_dir));
    double sp = specular(n);
    return rgb3(base.r * l + sp, base.g * l + sp, base.b * l + sp);
}

static uint32_t pack(xd_rgba c) {
    /* 6 bits a channel: like sides share a fill without visible banding. */
    uint32_t r = (uint32_t)lround(c.r * 63);
    uint32_t g = (uint32_t)lround(c.g * 63);
    uint32_t b = (uint32_t)lround(c.b * 63);
    return r << 16 | g << 8 | b;
}

static void set_packed(cairo_t *cr, uint32_t c) {
    cairo_set_source_rgb(cr, (c >> 16 & 0xFF) / 63.0, (c >> 8 & 0xFF) / 63.0, (c & 0xFF) / 63.0);
}

static int far_first(const void *a, const void *b) {
    const side *x = a;
    const side *y = b;
    if (x->depth != y->depth)
        return x->depth > y->depth ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

/* ---- a frame ------------------------------------------------------------------- */

static void draw_sides(cairo_t *cr, const saver_text3d *t, const side *s, size_t n) {
    for (size_t a = 0; a < n;) {
        size_t b = a;
        cairo_new_path(cr);
        while (b < n && s[b].color == s[a].color) {
            size_t i = s[b].i;
            size_t j = s[b].j;
            cairo_move_to(cr, t->fsx[i], t->fsy[i]);
            cairo_line_to(cr, t->fsx[j], t->fsy[j]);
            cairo_line_to(cr, t->bsx[j], t->bsy[j]);
            cairo_line_to(cr, t->bsx[i], t->bsy[i]);
            cairo_close_path(cr);
            b++;
        }
        set_packed(cr, s[a].color);
        /* The outline too: no hairline gaps between neighbouring sides. */
        cairo_fill_preserve(cr);
        cairo_stroke(cr);
        a = b;
    }
}

static void draw_face(cairo_t *cr, const saver_text3d *t, bool front, v3 n, const double m[3][3],
                      double D, double f) {
    const double *sx = front ? t->fsx : t->bsx;
    const double *sy = front ? t->fsy : t->bsy;
    cairo_new_path(cr);
    for (size_t c = 0; c < t->ncont; c++) {
        const contour *ct = &t->contours[c];
        cairo_move_to(cr, sx[ct->start], sy[ct->start]);
        for (size_t i = 1; i < ct->count; i++)
            cairo_line_to(cr, sx[ct->start + i], sy[ct->start + i]);
        cairo_close_path(cr);
    }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    if (t->o.surface != XD_TEXT3D_REFLECTION) {
        set_packed(cr, pack(shade(t, n)));
        cairo_fill(cr);
        return;
    }
    /* Chrome: a flat face still reflects the horizon going by, top to
     * bottom of the letters. */
    double z = (front ? -0.5 : 0.5) * DEPTH * FONT_PX;
    v3 top = v3_add(rotate(m, (v3){0, -t->height / 2, z}), (v3){0, 0, D});
    v3 bottom = v3_add(rotate(m, (v3){0, t->height / 2, z}), (v3){0, 0, D});
    cairo_pattern_t *pat = cairo_pattern_create_linear(
        t->w / 2.0 + top.x * f / top.z, t->h / 2.0 + top.y * f / top.z,
        t->w / 2.0 + bottom.x * f / bottom.z, t->h / 2.0 + bottom.y * f / bottom.z);
    double vn = n.z;
    double up = -(-2 * vn * n.y);
    double sp = specular(n);
    for (int i = 0; i <= 6; i++) {
        double at = i / 6.0;
        xd_rgba c = environment(fmin(1, fmax(-1, up + 0.45 - 0.9 * at)));
        cairo_pattern_add_color_stop_rgb(pat, at, fmin(1, c.r + sp), fmin(1, c.g + sp),
                                         fmin(1, c.b + sp));
    }
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

void saver_text3d_frame(saver_text3d *t, cairo_t *cr, double dt) {
    t->time += dt;
    update_text(t);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (t->ncont == 0)
        return;
    double a[3];
    rotation(t, dt, a);
    double m[3][3];
    matrix(a, m);

    /* Fit the text to the Size trackbar's share of the screen. */
    double share = 0.35 + 0.06 * t->o.size;
    double D = 2.5 * fmax(t->width, t->height * 2);
    double f = fmin(share * t->w / t->width, 0.7 * t->h / t->height) * D;
    double half = DEPTH * FONT_PX / 2;
    v3 eye_to = {0, 0, D};
    for (size_t i = 0; i < t->npts; i++) {
        v3 fr = v3_add(rotate(m, (v3){t->px[i], t->py[i], -half}), eye_to);
        v3 bk = v3_add(rotate(m, (v3){t->px[i], t->py[i], half}), eye_to);
        t->front[i] = fr;
        t->back[i] = bk;
        t->fsx[i] = t->w / 2.0 + fr.x * f / fr.z;
        t->fsy[i] = t->h / 2.0 + fr.y * f / fr.z;
        t->bsx[i] = t->w / 2.0 + bk.x * f / bk.z;
        t->bsy[i] = t->h / 2.0 + bk.y * f / bk.z;
    }

    /* The sides that face the eye. */
    size_t n = 0;
    for (size_t c = 0; c < t->ncont; c++) {
        const contour *ct = &t->contours[c];
        for (size_t k = 0; k < ct->count; k++) {
            size_t i = ct->start + k;
            size_t j = ct->start + (k + 1) % ct->count;
            double ex = t->px[j] - t->px[i];
            double ey = t->py[j] - t->py[i];
            v3 nw = norm(rotate(m, (v3){ey * t->outward, -ex * t->outward, 0}));
            v3 mid = {(t->front[i].x + t->back[j].x) / 2, (t->front[i].y + t->back[j].y) / 2,
                      (t->front[i].z + t->back[j].z) / 2};
            if (dot(nw, mid) >= 0)
                continue;
            t->sides[n++] = (side){.i = i, .j = j, .depth = sqrt(dot(mid, mid)),
                                   .color = pack(shade(t, nw))};
        }
    }
    qsort(t->sides, n, sizeof(*t->sides), far_first);

    /* Then the face towards the eye: every side lies behind its plane, so
     * nothing can cover it. */
    v3 fn = rotate(m, (v3){0, 0, -1});
    v3 fc = v3_add(rotate(m, (v3){0, 0, -half}), eye_to);
    bool front = dot(fn, fc) < 0;
    v3 face_n = front ? fn : (v3){-fn.x, -fn.y, -fn.z};
    cairo_save(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
    draw_sides(cr, t, t->sides, n);
    draw_face(cr, t, front, face_n, m, D, f);
    cairo_restore(cr);
}

saver_text3d *saver_text3d_create(const xd_saver_settings *o, int w, int h, uint64_t *rng) {
    saver_text3d *t = XD_NEW(saver_text3d);
    t->rng = rng;
    t->w = w;
    t->h = h;
    t->o = o->text3d;
    double pos = (double)t->o.speed / (double)XD_SAVER_SLIDER_MAX;
    t->k = 0.2 + 2.3 * pos; /* turns a second, roughly */
    update_text(t);
    return t;
}

void saver_text3d_destroy(saver_text3d *t) {
    free_outline(t);
    xd_free(t);
}
