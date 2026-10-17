/* SPDX-License-Identifier: MIT
 * Screen savers after the ones Windows 2000 shipped, drawn with cairo:
 * Blank Screen, Starfield Simulation, Mystify Your Mind, Beziers, Marquee
 * Display, and Flying Penguins (Flying Windows with our Tux logo).
 *
 * Every frame repaints the whole area, so a saver needs no state from the
 * previous frame's pixels. Speeds are per second and sizes scale with DPI.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "savers.h"

static const xd_saver_info savers[] = {
    {"blank", "Blank Screen"},
    {"starfield", "Starfield Simulation"},
    {"mystify", "Mystify Your Mind"},
    {"beziers", "Beziers"},
    {"marquee", "Marquee Display"},
    {"flying", "Flying Penguins"},
};

size_t xd_savers(const xd_saver_info **out) {
    *out = savers;
    return XD_ARRAY_LEN(savers);
}

const xd_saver_info *xd_saver_find(const char *id) {
    for (size_t i = 0; i < XD_ARRAY_LEN(savers); i++) {
        if (id != nullptr && strcmp(savers[i].id, id) == 0)
            return &savers[i];
    }
    return nullptr;
}

bool xd_saver_animated(const char *id) {
    return xd_saver_find(id) != nullptr && strcmp(id, "blank") != 0;
}

/* ---- shared ------------------------------------------------------------------- */

enum { MAX_STARS = 300, POLY_POINTS = 4, POLYS = 2, TRAIL = 8, CURVE_TRAIL = 24, FLYERS = 24 };

typedef struct vec2 {
    double x, y;
} vec2;

typedef struct mover {
    vec2 p[POLY_POINTS];
    vec2 v[POLY_POINTS];
    vec2 trail[CURVE_TRAIL][POLY_POINTS]; /* a ring; trail_head is the newest */
    int trail_head, trail_len;
    double hue;
} mover;

typedef enum kind : uint8_t { K_BLANK, K_STARFIELD, K_MYSTIFY, K_BEZIERS, K_MARQUEE, K_FLYING } kind;

struct xd_saver {
    kind k;
    int w, h;
    double scale;
    uint64_t rng;
    double time;
    double since_trail;

    /* starfield, flying */
    struct {
        double x, y, z;
    } stars[MAX_STARS];
    int star_count;

    /* mystify, beziers */
    mover movers[POLYS];
    int mover_count;

    /* marquee */
    double text_x, text_y;
    int text_w;
};

static double rnd(xd_saver *s) {
    /* xorshift64*: plenty for animation. */
    s->rng ^= s->rng >> 12;
    s->rng ^= s->rng << 25;
    s->rng ^= s->rng >> 27;
    return (double)((s->rng * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}

static double between(xd_saver *s, double lo, double hi) { return lo + (hi - lo) * rnd(s); }

/* HSV with full saturation and value, h in [0, 1). */
static void set_hue(cairo_t *cr, double h, double alpha) {
    h = fmod(h, 1.0) * 6.0;
    double r = fmin(fmax(fabs(h - 3.0) - 1.0, 0.0), 1.0);
    double g = fmin(fmax(2.0 - fabs(h - 2.0), 0.0), 1.0);
    double b = fmin(fmax(2.0 - fabs(h - 4.0), 0.0), 1.0);
    cairo_set_source_rgba(cr, r, g, b, alpha);
}

static void black(xd_saver *s, cairo_t *cr) {
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, 0, 0, s->w, s->h);
    cairo_fill(cr);
}

/* ---- starfield and flying ----------------------------------------------------- */

static void star_spawn(xd_saver *s, int i, bool anywhere) {
    s->stars[i].x = between(s, -1.0, 1.0);
    s->stars[i].y = between(s, -1.0, 1.0);
    s->stars[i].z = anywhere ? between(s, 0.05, 1.0) : 1.0;
}

static void stars_step(xd_saver *s, double dt, double speed) {
    for (int i = 0; i < s->star_count; i++) {
        s->stars[i].z -= speed * dt;
        if (s->stars[i].z <= 0.02)
            star_spawn(s, i, false);
    }
}

static bool star_project(const xd_saver *s, int i, double *x, double *y) {
    double half_w = s->w / 2.0;
    double half_h = s->h / 2.0;
    double z = s->stars[i].z;
    *x = half_w + s->stars[i].x / z * half_w * 0.5;
    *y = half_h + s->stars[i].y / z * half_h * 0.5;
    return *x >= -200 && *y >= -200 && *x < s->w + 200 && *y < s->h + 200;
}

static void starfield_frame(xd_saver *s, cairo_t *cr, double dt) {
    stars_step(s, dt, 0.25);
    black(s, cr);
    for (int i = 0; i < s->star_count; i++) {
        double x;
        double y;
        if (!star_project(s, i, &x, &y)) {
            star_spawn(s, i, false);
            continue;
        }
        double near = 1.0 - s->stars[i].z;
        double size = fmax(1.0, near * near * 4.0 * s->scale);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.3 + 0.7 * near);
        cairo_rectangle(cr, x - size / 2, y - size / 2, size, size);
        cairo_fill(cr);
    }
}

static void flying_frame(xd_saver *s, cairo_t *cr, double dt) {
    stars_step(s, dt, 0.18);
    black(s, cr);
    /* Far ones first, so near ones cover them. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s->star_count; i++) {
            bool far = s->stars[i].z > 0.5;
            if ((pass == 0) != far)
                continue;
            double x;
            double y;
            if (!star_project(s, i, &x, &y)) {
                star_spawn(s, i, false);
                continue;
            }
            double size = fmin(s->w, s->h) * 0.04 / s->stars[i].z;
            if (size < 2)
                continue;
            xd_draw_start_logo(cr, (xd_rect){(int)(x - size / 2), (int)(y - size / 2), (int)size,
                                             (int)size});
        }
    }
}

/* ---- mystify and beziers ------------------------------------------------------ */

static void movers_init(xd_saver *s, int count) {
    s->mover_count = count;
    double speed = 140.0 * s->scale * fmax(0.35, fmin(s->w, s->h) / 768.0);
    for (int m = 0; m < count; m++) {
        mover *mv = &s->movers[m];
        mv->hue = rnd(s);
        for (int i = 0; i < POLY_POINTS; i++) {
            mv->p[i] = (vec2){between(s, 0, s->w), between(s, 0, s->h)};
            double a = between(s, 0, 2 * M_PI);
            double sp = between(s, 0.5, 1.0) * speed;
            mv->v[i] = (vec2){cos(a) * sp, sin(a) * sp};
        }
    }
}

static void movers_step(xd_saver *s, double dt, int trail_max, double trail_every) {
    s->since_trail += dt;
    bool snap = s->since_trail >= trail_every;
    if (snap)
        s->since_trail = 0;
    for (int m = 0; m < s->mover_count; m++) {
        mover *mv = &s->movers[m];
        mv->hue = fmod(mv->hue + dt * 0.05, 1.0);
        for (int i = 0; i < POLY_POINTS; i++) {
            vec2 *p = &mv->p[i];
            vec2 *v = &mv->v[i];
            p->x += v->x * dt;
            p->y += v->y * dt;
            if (p->x < 0 || p->x > s->w) {
                v->x = -v->x;
                p->x = fmin(fmax(p->x, 0), s->w);
            }
            if (p->y < 0 || p->y > s->h) {
                v->y = -v->y;
                p->y = fmin(fmax(p->y, 0), s->h);
            }
        }
        if (snap) {
            mv->trail_head = (mv->trail_head + CURVE_TRAIL - 1) % CURVE_TRAIL;
            memcpy(mv->trail[mv->trail_head], mv->p, sizeof(mv->p));
            mv->trail_len = mv->trail_len < trail_max ? mv->trail_len + 1 : trail_max;
        }
    }
}

/* The outline `t` snapshots ago (0 is the newest). */
static const vec2 *trail_at(const mover *mv, int t) {
    return mv->trail[(mv->trail_head + t) % CURVE_TRAIL];
}

static void mystify_frame(xd_saver *s, cairo_t *cr, double dt) {
    movers_step(s, dt, TRAIL, 0.06);
    black(s, cr);
    cairo_set_line_width(cr, fmax(1.0, s->scale));
    for (int m = 0; m < s->mover_count; m++) {
        const mover *mv = &s->movers[m];
        for (int t = mv->trail_len - 1; t >= 0; t--) {
            set_hue(cr, mv->hue + m * 0.5, 1.0 - (double)t / (TRAIL + 1));
            const vec2 *p = trail_at(mv, t);
            cairo_move_to(cr, p[0].x, p[0].y);
            for (int i = 1; i < POLY_POINTS; i++)
                cairo_line_to(cr, p[i].x, p[i].y);
            cairo_close_path(cr);
            cairo_stroke(cr);
        }
    }
}

static void beziers_frame(xd_saver *s, cairo_t *cr, double dt) {
    movers_step(s, dt, CURVE_TRAIL, 0.04);
    black(s, cr);
    cairo_set_line_width(cr, fmax(1.0, s->scale));
    const mover *mv = &s->movers[0];
    for (int t = mv->trail_len - 1; t >= 0; t--) {
        const vec2 *p = trail_at(mv, t);
        set_hue(cr, mv->hue + t * 0.01, 1.0 - (double)t / (CURVE_TRAIL + 1));
        /* A closed loop: out along p0..p3, back through the mirror. */
        cairo_move_to(cr, p[0].x, p[0].y);
        cairo_curve_to(cr, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
        cairo_curve_to(cr, s->w - p[2].x, s->h - p[2].y, s->w - p[1].x, s->h - p[1].y, p[0].x,
                       p[0].y);
        cairo_stroke(cr);
    }
}

/* ---- marquee ------------------------------------------------------------------ */

static xd_font_spec marquee_font(const xd_saver *s) {
    xd_font_spec f = {.size_pt = fmax(14.0, s->h / 12.0 / s->scale)};
    snprintf(f.family, sizeof(f.family), "%s", "Times New Roman, Liberation Serif, serif");
    return f;
}

static void marquee_frame(xd_saver *s, cairo_t *cr, double dt) {
    static const char text[] = "XeonDesktop";
    xd_font_spec font = marquee_font(s);
    xd_text_style st = {.font = &font, .scale = s->scale, .color = xd_rgba_hex(0xFF2020)};
    if (s->text_w == 0) {
        s->text_w = xd_text_width(cr, text, &st);
        s->text_x = s->w;
        s->text_y = between(s, 0, s->h * 0.8);
    }
    s->text_x -= dt * 150.0 * s->scale * fmax(0.35, s->w / 1024.0);
    if (s->text_x < -s->text_w) {
        s->text_x = s->w;
        s->text_y = between(s, 0, s->h * 0.8);
    }
    black(s, cr);
    int line = (int)(font.size_pt * s->scale * 2.0);
    xd_draw_text(cr, (xd_rect){(int)s->text_x, (int)s->text_y, s->text_w + line, line}, text, &st);
}

/* ---- public ------------------------------------------------------------------- */

xd_saver *xd_saver_create(const char *id, int w, int h, double scale) {
    static const char *const ids[] = {[K_BLANK] = "blank",     [K_STARFIELD] = "starfield",
                                      [K_MYSTIFY] = "mystify", [K_BEZIERS] = "beziers",
                                      [K_MARQUEE] = "marquee", [K_FLYING] = "flying"};
    int k = -1;
    for (size_t i = 0; i < XD_ARRAY_LEN(ids); i++) {
        if (id != nullptr && strcmp(ids[i], id) == 0)
            k = (int)i;
    }
    if (k < 0)
        return nullptr;
    xd_saver *s = XD_NEW(xd_saver);
    s->k = (kind)k;
    s->w = XD_MAX(w, 1);
    s->h = XD_MAX(h, 1);
    s->scale = scale > 0 ? scale : 1.0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    s->rng = ((uint64_t)now.tv_nsec << 20) ^ (uint64_t)now.tv_sec ^ 0x9E3779B97F4A7C15ULL;
    switch (s->k) {
    case K_STARFIELD:
    case K_FLYING:
        s->star_count = s->k == K_FLYING ? FLYERS : MAX_STARS;
        for (int i = 0; i < s->star_count; i++)
            star_spawn(s, i, true);
        break;
    case K_MYSTIFY:
        movers_init(s, POLYS);
        break;
    case K_BEZIERS:
        movers_init(s, 1);
        break;
    case K_BLANK:
    case K_MARQUEE:
        break;
    }
    return s;
}

void xd_saver_frame(xd_saver *s, cairo_t *cr, double dt) {
    dt = fmin(fmax(dt, 0.0), 0.25); /* after a stall, don't jump */
    s->time += dt;
    switch (s->k) {
    case K_BLANK:
        black(s, cr);
        break;
    case K_STARFIELD:
        starfield_frame(s, cr, dt);
        break;
    case K_MYSTIFY:
        mystify_frame(s, cr, dt);
        break;
    case K_BEZIERS:
        beziers_frame(s, cr, dt);
        break;
    case K_MARQUEE:
        marquee_frame(s, cr, dt);
        break;
    case K_FLYING:
        flying_frame(s, cr, dt);
        break;
    }
}

void xd_saver_destroy(xd_saver *s) { xd_free(s); }
