/* SPDX-License-Identifier: MIT
 * Screen savers after the ones Windows 2000 shipped, drawn with cairo:
 * 3D Pipes and 3D Text (pipes.c, text3d.c), Beziers, Blank Screen, Flying
 * Penguins (Flying Windows with our Tux logo), Marquee Display, Mystify
 * Your Mind and Starfield Simulation, each with the options of its
 * Settings dialog ([ScreenSaver.*]).
 *
 * Every frame paints the whole area. A saver whose picture builds up
 * (3D Pipes, Mystify without Clear Screen) keeps it in an image of its own
 * and paints that. Speeds are per second and sizes scale with DPI.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "saver_impl.h"
#include "savers.h"

/* Windows lists them by name, as the files sort. */
static const xd_saver_info savers[] = {
    {"pipes", "3D Pipes"},
    {"text3d", "3D Text"},
    {"beziers", "Beziers"},
    {"blank", "Blank Screen"},
    {"flying", "Flying Penguins"},
    {"marquee", "Marquee Display"},
    {"mystify", "Mystify Your Mind"},
    {"starfield", "Starfield Simulation"},
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

bool xd_saver_has_settings(const char *id) { return xd_saver_animated(id); }

/* ---- shared ------------------------------------------------------------------- */

#define MAX_STARS XD_MAX((int)XD_STARFIELD_DENSITY_MAX, (int)XD_FLYING_DENSITY_MAX)
#define MAX_MOVERS ((int)XD_BEZIERS_CURVES_MAX)
#define TRAIL_MAX ((int)XD_BEZIERS_LINES_MAX)
enum { POLY_POINTS = 4 };
static_assert((int)XD_MYSTIFY_SHAPES <= MAX_MOVERS && (int)XD_MYSTIFY_LINES_MAX <= TRAIL_MAX);

typedef struct vec2 {
    double x, y;
} vec2;

typedef struct mover {
    vec2 p[POLY_POINTS];
    vec2 v[POLY_POINTS];
    vec2 trail[TRAIL_MAX][POLY_POINTS]; /* a ring; trail_head is the newest */
    int trail_head, trail_len, trail_max;
    double hue;
    bool on; /* Mystify's Active */
} mover;

typedef enum kind : uint8_t {
    K_BLANK,
    K_STARFIELD,
    K_MYSTIFY,
    K_BEZIERS,
    K_MARQUEE,
    K_FLYING,
    K_PIPES,
    K_TEXT3D,
} kind;

struct xd_saver {
    kind k;
    int w, h;
    double scale;
    uint64_t rng;
    double time;
    double since_trail;
    xd_saver_settings o;

    /* starfield, flying */
    struct {
        double x, y, z;
    } stars[MAX_STARS];
    int star_count;
    double star_speed; /* depth per second */

    /* mystify, beziers */
    mover movers[MAX_MOVERS];
    int mover_count;
    double trail_every; /* seconds between outlines */
    cairo_surface_t *canvas; /* Mystify without Clear Screen: what's drawn so far */

    /* marquee */
    double text_x, text_y;
    int text_w;

    saver_pipes *pipes;
    saver_text3d *text3d;
};

static double rnd(xd_saver *s) { return xd_saver_rand(&s->rng); }

static double between(xd_saver *s, double lo, double hi) { return xd_saver_between(&s->rng, lo, hi); }

/* A trackbar position (0..XD_SAVER_SLIDER_MAX) as a factor: 1 in the
 * middle, `lo` at the slow end, `hi` at the fast end. */
static double speed_factor(int pos, double lo, double hi) {
    double t = (double)pos / (double)XD_SAVER_SLIDER_MAX * 2.0 - 1.0;
    return t < 0 ? 1.0 + t * (1.0 - lo) : 1.0 + t * (hi - 1.0);
}

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

static void stars_step(xd_saver *s, double dt) {
    for (int i = 0; i < s->star_count; i++) {
        s->stars[i].z -= s->star_speed * dt;
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
    stars_step(s, dt);
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
    stars_step(s, dt);
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

static void movers_init(xd_saver *s, int count, double speed_k) {
    s->mover_count = count;
    double speed = 140.0 * speed_k * s->scale * fmax(0.35, fmin(s->w, s->h) / 768.0);
    for (int m = 0; m < count; m++) {
        mover *mv = &s->movers[m];
        mv->on = true;
        mv->hue = rnd(s);
        for (int i = 0; i < POLY_POINTS; i++) {
            mv->p[i] = (vec2){between(s, 0, s->w), between(s, 0, s->h)};
            double a = between(s, 0, 2 * M_PI);
            double sp = between(s, 0.5, 1.0) * speed;
            mv->v[i] = (vec2){cos(a) * sp, sin(a) * sp};
        }
    }
}

/* Moves the outlines; true when it's time for another in the trails. */
static bool movers_step(xd_saver *s, double dt) {
    s->since_trail += dt;
    bool snap = s->since_trail >= s->trail_every;
    if (snap)
        s->since_trail = 0;
    for (int m = 0; m < s->mover_count; m++) {
        mover *mv = &s->movers[m];
        if (!mv->on)
            continue;
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
            mv->trail_head = (mv->trail_head + TRAIL_MAX - 1) % TRAIL_MAX;
            memcpy(mv->trail[mv->trail_head], mv->p, sizeof(mv->p));
            mv->trail_len = mv->trail_len < mv->trail_max ? mv->trail_len + 1 : mv->trail_max;
        }
    }
    return snap;
}

/* The outline `t` snapshots ago (0 is the newest). */
static const vec2 *trail_at(const mover *mv, int t) {
    return mv->trail[(mv->trail_head + t) % TRAIL_MAX];
}

/* A polygon's colour now: Two Colors fades from one to the other and
 * back; Multiple Random Colors goes round the colour wheel. */
static void mystify_color(xd_saver *s, cairo_t *cr, int m, double alpha) {
    const xd_mystify_shape *sh = &s->o.mystify.shapes[m];
    if (!sh->two_colors) {
        set_hue(cr, s->movers[m].hue + m * 0.5, alpha);
        return;
    }
    double t = 0.5 - 0.5 * cos(s->time * 2 * M_PI / 8.0);
    xd_rgba c = xd_rgba_mix(xd_rgba_from_rgb(sh->colors[0]), xd_rgba_from_rgb(sh->colors[1]), t);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
}

static void outline_path(cairo_t *cr, const vec2 *p) {
    cairo_move_to(cr, p[0].x, p[0].y);
    for (int i = 1; i < POLY_POINTS; i++)
        cairo_line_to(cr, p[i].x, p[i].y);
    cairo_close_path(cr);
}

static void mystify_frame(xd_saver *s, cairo_t *cr, double dt) {
    bool snap = movers_step(s, dt);
    if (s->canvas != nullptr) {
        /* No Clear Screen: each new outline joins what's there. */
        if (snap) {
            cairo_t *c = cairo_create(s->canvas);
            cairo_set_line_width(c, fmax(1.0, s->scale));
            for (int m = 0; m < s->mover_count; m++) {
                if (!s->movers[m].on)
                    continue;
                mystify_color(s, c, m, 1.0);
                outline_path(c, trail_at(&s->movers[m], 0));
                cairo_stroke(c);
            }
            cairo_destroy(c);
        }
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, s->canvas, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }
    black(s, cr);
    cairo_set_line_width(cr, fmax(1.0, s->scale));
    for (int m = 0; m < s->mover_count; m++) {
        const mover *mv = &s->movers[m];
        if (!mv->on)
            continue;
        for (int t = mv->trail_len - 1; t >= 0; t--) {
            mystify_color(s, cr, m, 1.0 - (double)t / (mv->trail_max + 1));
            outline_path(cr, trail_at(mv, t));
            cairo_stroke(cr);
        }
    }
}

static void beziers_frame(xd_saver *s, cairo_t *cr, double dt) {
    movers_step(s, dt);
    black(s, cr);
    cairo_set_line_width(cr, fmax(1.0, s->scale));
    for (int m = 0; m < s->mover_count; m++) {
        const mover *mv = &s->movers[m];
        for (int t = mv->trail_len - 1; t >= 0; t--) {
            const vec2 *p = trail_at(mv, t);
            set_hue(cr, mv->hue + t * 0.01, 1.0 - (double)t / (mv->trail_max + 1));
            /* A closed loop: out along p0..p3, back through the mirror. */
            cairo_move_to(cr, p[0].x, p[0].y);
            cairo_curve_to(cr, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
            cairo_curve_to(cr, s->w - p[2].x, s->h - p[2].y, s->w - p[1].x, s->h - p[1].y,
                           p[0].x, p[0].y);
            cairo_stroke(cr);
        }
    }
}

/* ---- marquee ------------------------------------------------------------------ */

/* The font is sized for a 768-pixel-high screen and scaled with the
 * area, so the preview's miniature looks like the real thing. */
static double marquee_scale(const xd_saver *s) { return s->h / 768.0; }

static void marquee_place(xd_saver *s, int line) {
    s->text_x = s->w;
    s->text_y = s->o.marquee.random_position ? between(s, 0, fmax(0, s->h - line))
                                             : (s->h - line) / 2.0;
}

static void marquee_frame(xd_saver *s, cairo_t *cr, double dt) {
    const xd_marquee_settings *m = &s->o.marquee;
    /* Windows' default, Times New Roman, is often not installed: then a
     * serif face like it, rather than the system's sans. */
    xd_font_spec font = m->font;
    snprintf(font.family, sizeof(font.family), "%.100s, Liberation Serif, serif", m->font.family);
    xd_text_style st = {.font = &font, .scale = marquee_scale(s),
                        .color = xd_rgba_from_rgb(m->color)};
    double line_px = ceil(m->font.size_pt * st.scale * 2.0);
    int line = (int)line_px;
    if (s->text_w == 0) {
        s->text_w = XD_MAX(xd_text_width(cr, m->text, &st), 1);
        marquee_place(s, line);
    }
    s->text_x -= dt * s->w * 0.19 * speed_factor(m->speed, 0.25, 3.0);
    if (s->text_x < -s->text_w)
        marquee_place(s, line);
    xd_fill_rect(cr, (xd_rect){0, 0, s->w, s->h}, xd_rgba_from_rgb(m->background));
    xd_draw_text(cr, (xd_rect){(int)s->text_x, (int)s->text_y, s->text_w + line, line}, m->text,
                 &st);
}

/* ---- public ------------------------------------------------------------------- */

static void saver_init(xd_saver *s) {
    const xd_saver_settings *o = &s->o;
    switch (s->k) {
    case K_STARFIELD:
        s->star_count = o->starfield.density;
        s->star_speed = 0.25 * speed_factor(o->starfield.speed, 0.15, 4.0);
        break;
    case K_FLYING:
        s->star_count = o->flying.density;
        s->star_speed = 0.18 * speed_factor(o->flying.speed, 0.15, 4.0);
        break;
    case K_MYSTIFY:
        movers_init(s, XD_MYSTIFY_SHAPES, 1.0);
        s->trail_every = 0.06;
        for (int m = 0; m < XD_MYSTIFY_SHAPES; m++) {
            s->movers[m].on = o->mystify.shapes[m].active;
            s->movers[m].trail_max = o->mystify.shapes[m].lines;
        }
        if (!o->mystify.clear_screen) {
            s->canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, s->w, s->h);
            cairo_t *c = cairo_create(s->canvas);
            black(s, c);
            cairo_destroy(c);
        }
        break;
    case K_BEZIERS: {
        double k = speed_factor(o->beziers.speed, 0.2, 3.0);
        movers_init(s, o->beziers.curves, k);
        s->trail_every = 0.04 / k;
        for (int m = 0; m < s->mover_count; m++)
            s->movers[m].trail_max = o->beziers.lines;
        break;
    }
    case K_PIPES:
        s->pipes = saver_pipes_create(o, s->w, s->h, &s->rng);
        break;
    case K_TEXT3D:
        s->text3d = saver_text3d_create(o, s->w, s->h, &s->rng);
        break;
    case K_BLANK:
    case K_MARQUEE:
        break;
    }
    for (int i = 0; i < s->star_count; i++)
        star_spawn(s, i, true);
}

xd_saver *xd_saver_create_seeded(const char *id, const xd_saver_settings *o, int w, int h,
                                 double scale, uint64_t seed) {
    static const char *const ids[] = {[K_BLANK] = "blank",     [K_STARFIELD] = "starfield",
                                      [K_MYSTIFY] = "mystify", [K_BEZIERS] = "beziers",
                                      [K_MARQUEE] = "marquee", [K_FLYING] = "flying",
                                      [K_PIPES] = "pipes",     [K_TEXT3D] = "text3d"};
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
    s->rng = seed != 0 ? seed : 0x9E3779B97F4A7C15ULL; /* xorshift never leaves 0 */
    if (o != nullptr)
        s->o = *o;
    else
        xd_saver_settings_defaults(&s->o);
    saver_init(s);
    return s;
}

xd_saver *xd_saver_create(const char *id, const xd_saver_settings *o, int w, int h, double scale) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t seed = ((uint64_t)now.tv_nsec << 20) ^ (uint64_t)now.tv_sec ^ 0x9E3779B97F4A7C15ULL;
    return xd_saver_create_seeded(id, o, w, h, scale, seed);
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
    case K_PIPES:
        saver_pipes_frame(s->pipes, cr, dt);
        break;
    case K_TEXT3D:
        saver_text3d_frame(s->text3d, cr, dt);
        break;
    }
}

void xd_saver_destroy(xd_saver *s) {
    if (s == nullptr)
        return;
    if (s->canvas != nullptr)
        cairo_surface_destroy(s->canvas);
    if (s->pipes != nullptr)
        saver_pipes_destroy(s->pipes);
    if (s->text3d != nullptr)
        saver_text3d_destroy(s->text3d);
    xd_free(s);
}
