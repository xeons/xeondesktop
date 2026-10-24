/* SPDX-License-Identifier: MIT
 * Screen savers after the ones Windows 2000 shipped, drawn with cairo:
 * 3D Flower Box, 3D Maze, 3D Pipes and 3D Text (flower.c, maze.c, pipes.c, text3d.c),
 * Beziers, Blank Screen, Flying
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
    {"flowerbox", "3D Flower Box"},
    {"maze", "3D Maze"},
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
    K_MAZE,
    K_FLOWER,
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
    saver_maze *maze;
    saver_flower *flower;
};

static double rnd(xd_saver *saver) { return xd_saver_rand(&saver->rng); }

static double between(xd_saver *saver, double lo, double hi) {
    return xd_saver_between(&saver->rng, lo, hi);
}

/* A trackbar position (0..XD_SAVER_SLIDER_MAX) as a factor: 1 in the
 * middle, `lo` at the slow end, `hi` at the fast end. */
static double speed_factor(int pos, double lo, double hi) {
    double position = (double)pos / (double)XD_SAVER_SLIDER_MAX * 2.0 - 1.0;
    return position < 0 ? 1.0 + position * (1.0 - lo) : 1.0 + position * (hi - 1.0);
}

/* HSV with full saturation and value, h in [0, 1). */
static void set_hue(cairo_t *cairo, double hue, double alpha) {
    hue = fmod(hue, 1.0) * 6.0;
    double red = fmin(fmax(fabs(hue - 3.0) - 1.0, 0.0), 1.0);
    double green = fmin(fmax(2.0 - fabs(hue - 2.0), 0.0), 1.0);
    double blue = fmin(fmax(2.0 - fabs(hue - 4.0), 0.0), 1.0);
    cairo_set_source_rgba(cairo, red, green, blue, alpha);
}

static void black(xd_saver *saver, cairo_t *cairo) {
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_rectangle(cairo, 0, 0, saver->w, saver->h);
    cairo_fill(cairo);
}

/* ---- starfield and flying ----------------------------------------------------- */

static void star_spawn(xd_saver *saver, int i, bool anywhere) {
    saver->stars[i].x = between(saver, -1.0, 1.0);
    saver->stars[i].y = between(saver, -1.0, 1.0);
    saver->stars[i].z = anywhere ? between(saver, 0.05, 1.0) : 1.0;
}

static void stars_step(xd_saver *saver, double dt) {
    for (int i = 0; i < saver->star_count; i++) {
        saver->stars[i].z -= saver->star_speed * dt;
        if (saver->stars[i].z <= 0.02)
            star_spawn(saver, i, false);
    }
}

static bool star_project(const xd_saver *saver, int i, double *screen_x, double *screen_y) {
    double half_w = saver->w / 2.0;
    double half_h = saver->h / 2.0;
    double z = saver->stars[i].z;
    *screen_x = half_w + saver->stars[i].x / z * half_w * 0.5;
    *screen_y = half_h + saver->stars[i].y / z * half_h * 0.5;
    return *screen_x >= -200 && *screen_y >= -200 && *screen_x < saver->w + 200 &&
           *screen_y < saver->h + 200;
}

static void starfield_frame(xd_saver *saver, cairo_t *cairo, double dt) {
    stars_step(saver, dt);
    black(saver, cairo);
    for (int i = 0; i < saver->star_count; i++) {
        double x;
        double y;
        if (!star_project(saver, i, &x, &y)) {
            star_spawn(saver, i, false);
            continue;
        }
        double near = 1.0 - saver->stars[i].z;
        double size = fmax(1.0, near * near * 4.0 * saver->scale);
        cairo_set_source_rgba(cairo, 1, 1, 1, 0.3 + 0.7 * near);
        cairo_rectangle(cairo, x - size / 2, y - size / 2, size, size);
        cairo_fill(cairo);
    }
}

static void flying_frame(xd_saver *saver, cairo_t *cairo, double dt) {
    stars_step(saver, dt);
    black(saver, cairo);
    /* Far ones first, so near ones cover them. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < saver->star_count; i++) {
            bool far = saver->stars[i].z > 0.5;
            if ((pass == 0) != far)
                continue;
            double x;
            double y;
            if (!star_project(saver, i, &x, &y)) {
                star_spawn(saver, i, false);
                continue;
            }
            double size = fmin(saver->w, saver->h) * 0.04 / saver->stars[i].z;
            if (size < 2)
                continue;
            xd_draw_start_logo(
                cairo, (xd_rect){(int)(x - size / 2), (int)(y - size / 2), (int)size, (int)size});
        }
    }
}

/* ---- mystify and beziers ------------------------------------------------------ */

static void movers_init(xd_saver *saver, int count, double speed_k) {
    saver->mover_count = count;
    double speed = 140.0 * speed_k * saver->scale * fmax(0.35, fmin(saver->w, saver->h) / 768.0);
    for (int shape = 0; shape < count; shape++) {
        mover *mv = &saver->movers[shape];
        mv->on = true;
        mv->hue = rnd(saver);
        for (int i = 0; i < POLY_POINTS; i++) {
            mv->p[i] = (vec2){between(saver, 0, saver->w), between(saver, 0, saver->h)};
            double angle = between(saver, 0, 2 * M_PI);
            double sp = between(saver, 0.5, 1.0) * speed;
            mv->v[i] = (vec2){cos(angle) * sp, sin(angle) * sp};
        }
    }
}

/* Moves the outlines; true when it's time for another in the trails. */
static bool movers_step(xd_saver *saver, double dt) {
    saver->since_trail += dt;
    bool snap = saver->since_trail >= saver->trail_every;
    if (snap)
        saver->since_trail = 0;
    for (int shape = 0; shape < saver->mover_count; shape++) {
        mover *mv = &saver->movers[shape];
        if (!mv->on)
            continue;
        mv->hue = fmod(mv->hue + dt * 0.05, 1.0);
        for (int i = 0; i < POLY_POINTS; i++) {
            vec2 *point = &mv->p[i];
            vec2 *velocity = &mv->v[i];
            point->x += velocity->x * dt;
            point->y += velocity->y * dt;
            if (point->x < 0 || point->x > saver->w) {
                velocity->x = -velocity->x;
                point->x = fmin(fmax(point->x, 0), saver->w);
            }
            if (point->y < 0 || point->y > saver->h) {
                velocity->y = -velocity->y;
                point->y = fmin(fmax(point->y, 0), saver->h);
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

/* The outline `age` snapshots ago (0 is the newest). */
static const vec2 *trail_at(const mover *mv, int age) {
    return mv->trail[(mv->trail_head + age) % TRAIL_MAX];
}

/* A polygon's colour now: Two Colors fades from one to the other and
 * back; Multiple Random Colors goes round the colour wheel. */
static void mystify_color(xd_saver *saver, cairo_t *cairo, int shape, double alpha) {
    const xd_mystify_shape *sh = &saver->o.mystify.shapes[shape];
    if (!sh->two_colors) {
        set_hue(cairo, saver->movers[shape].hue + shape * 0.5, alpha);
        return;
    }
    double blend = 0.5 - 0.5 * cos(saver->time * 2 * M_PI / 8.0);
    xd_rgba color =
        xd_rgba_mix(xd_rgba_from_rgb(sh->colors[0]), xd_rgba_from_rgb(sh->colors[1]), blend);
    cairo_set_source_rgba(cairo, color.r, color.g, color.b, alpha);
}

static void outline_path(cairo_t *cairo, const vec2 *points) {
    cairo_move_to(cairo, points[0].x, points[0].y);
    for (int i = 1; i < POLY_POINTS; i++)
        cairo_line_to(cairo, points[i].x, points[i].y);
    cairo_close_path(cairo);
}

static void mystify_frame(xd_saver *saver, cairo_t *cairo, double dt) {
    bool snap = movers_step(saver, dt);
    if (saver->canvas != nullptr) {
        /* No Clear Screen: each new outline joins what's there. */
        if (snap) {
            cairo_t *canvas_cairo = cairo_create(saver->canvas);
            cairo_set_line_width(canvas_cairo, fmax(1.0, saver->scale));
            for (int shape = 0; shape < saver->mover_count; shape++) {
                if (!saver->movers[shape].on)
                    continue;
                mystify_color(saver, canvas_cairo, shape, 1.0);
                outline_path(canvas_cairo, trail_at(&saver->movers[shape], 0));
                cairo_stroke(canvas_cairo);
            }
            cairo_destroy(canvas_cairo);
        }
        cairo_save(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cairo, saver->canvas, 0, 0);
        cairo_paint(cairo);
        cairo_restore(cairo);
        return;
    }
    black(saver, cairo);
    cairo_set_line_width(cairo, fmax(1.0, saver->scale));
    for (int shape = 0; shape < saver->mover_count; shape++) {
        const mover *mv = &saver->movers[shape];
        if (!mv->on)
            continue;
        for (int age = mv->trail_len - 1; age >= 0; age--) {
            mystify_color(saver, cairo, shape, 1.0 - (double)age / (mv->trail_max + 1));
            outline_path(cairo, trail_at(mv, age));
            cairo_stroke(cairo);
        }
    }
}

static void beziers_frame(xd_saver *saver, cairo_t *cairo, double dt) {
    movers_step(saver, dt);
    black(saver, cairo);
    cairo_set_line_width(cairo, fmax(1.0, saver->scale));
    for (int shape = 0; shape < saver->mover_count; shape++) {
        const mover *mv = &saver->movers[shape];
        for (int age = mv->trail_len - 1; age >= 0; age--) {
            const vec2 *points = trail_at(mv, age);
            set_hue(cairo, mv->hue + age * 0.01, 1.0 - (double)age / (mv->trail_max + 1));
            /* A closed loop: out along p0..p3, back through the mirror. */
            cairo_move_to(cairo, points[0].x, points[0].y);
            cairo_curve_to(cairo, points[1].x, points[1].y, points[2].x, points[2].y, points[3].x,
                           points[3].y);
            cairo_curve_to(cairo, saver->w - points[2].x, saver->h - points[2].y,
                           saver->w - points[1].x, saver->h - points[1].y, points[0].x,
                           points[0].y);
            cairo_stroke(cairo);
        }
    }
}

/* ---- marquee ------------------------------------------------------------------ */

/* The font is sized for a 768-pixel-high screen and scaled with the
 * area, so the preview's miniature looks like the real thing. */
static double marquee_scale(const xd_saver *saver) { return saver->h / 768.0; }

static void marquee_place(xd_saver *saver, int line) {
    saver->text_x = saver->w;
    saver->text_y = saver->o.marquee.random_position ? between(saver, 0, fmax(0, saver->h - line))
                                                     : (saver->h - line) / 2.0;
}

static void marquee_frame(xd_saver *saver, cairo_t *cairo, double dt) {
    const xd_marquee_settings *marquee_settings = &saver->o.marquee;
    /* Windows' default, Times New Roman, is often not installed: then a
     * serif face like it, rather than the system's sans. */
    xd_font_spec font = marquee_settings->font;
    snprintf(font.family, sizeof(font.family), "%.100s, Liberation Serif, serif",
             marquee_settings->font.family);
    xd_text_style st = {.font = &font,
                        .scale = marquee_scale(saver),
                        .color = xd_rgba_from_rgb(marquee_settings->color)};
    double line_px = ceil(marquee_settings->font.size_pt * st.scale * 2.0);
    int line = (int)line_px;
    if (saver->text_w == 0) {
        saver->text_w = XD_MAX(xd_text_width(cairo, marquee_settings->text, &st), 1);
        marquee_place(saver, line);
    }
    saver->text_x -= dt * saver->w * 0.19 * speed_factor(marquee_settings->speed, 0.25, 3.0);
    if (saver->text_x < -saver->text_w)
        marquee_place(saver, line);
    xd_fill_rect(cairo, (xd_rect){0, 0, saver->w, saver->h},
                 xd_rgba_from_rgb(marquee_settings->background));
    xd_draw_text(cairo,
                 (xd_rect){(int)saver->text_x, (int)saver->text_y, saver->text_w + line, line},
                 marquee_settings->text, &st);
}

/* ---- public ------------------------------------------------------------------- */

static void saver_init(xd_saver *saver) {
    const xd_saver_settings *saver_settings = &saver->o;
    switch (saver->k) {
    case K_STARFIELD:
        saver->star_count = saver_settings->starfield.density;
        saver->star_speed = 0.25 * speed_factor(saver_settings->starfield.speed, 0.15, 4.0);
        break;
    case K_FLYING:
        saver->star_count = saver_settings->flying.density;
        saver->star_speed = 0.18 * speed_factor(saver_settings->flying.speed, 0.15, 4.0);
        break;
    case K_MYSTIFY:
        movers_init(saver, XD_MYSTIFY_SHAPES, 1.0);
        saver->trail_every = 0.06;
        for (int shape = 0; shape < XD_MYSTIFY_SHAPES; shape++) {
            saver->movers[shape].on = saver_settings->mystify.shapes[shape].active;
            saver->movers[shape].trail_max = saver_settings->mystify.shapes[shape].lines;
        }
        if (!saver_settings->mystify.clear_screen) {
            saver->canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, saver->w, saver->h);
            cairo_t *cairo = cairo_create(saver->canvas);
            black(saver, cairo);
            cairo_destroy(cairo);
        }
        break;
    case K_BEZIERS: {
        double k = speed_factor(saver_settings->beziers.speed, 0.2, 3.0);
        movers_init(saver, saver_settings->beziers.curves, k);
        saver->trail_every = 0.04 / k;
        for (int shape = 0; shape < saver->mover_count; shape++)
            saver->movers[shape].trail_max = saver_settings->beziers.lines;
        break;
    }
    case K_PIPES:
        saver->pipes = saver_pipes_create(saver_settings, saver->w, saver->h, &saver->rng);
        break;
    case K_TEXT3D:
        saver->text3d = saver_text3d_create(saver_settings, saver->w, saver->h, &saver->rng);
        break;
    case K_MAZE:
        saver->maze = saver_maze_create(saver_settings, saver->w, saver->h, &saver->rng);
        break;
    case K_FLOWER:
        saver->flower = saver_flower_create(saver_settings, saver->w, saver->h, &saver->rng);
        break;
    case K_BLANK:
    case K_MARQUEE:
        break;
    }
    for (int i = 0; i < saver->star_count; i++)
        star_spawn(saver, i, true);
}

xd_saver *xd_saver_create_seeded(const char *id, const xd_saver_settings *saver_settings, int width,
                                 int height, double scale, uint64_t seed) {
    static const char *const ids[] = {[K_BLANK] = "blank",     [K_STARFIELD] = "starfield",
                                      [K_MYSTIFY] = "mystify", [K_BEZIERS] = "beziers",
                                      [K_MARQUEE] = "marquee", [K_FLYING] = "flying",
                                      [K_PIPES] = "pipes",     [K_TEXT3D] = "text3d",
                                      [K_MAZE] = "maze",       [K_FLOWER] = "flowerbox"};
    int k = -1;
    for (size_t i = 0; i < XD_ARRAY_LEN(ids); i++) {
        if (id != nullptr && strcmp(ids[i], id) == 0)
            k = (int)i;
    }
    if (k < 0)
        return nullptr;
    xd_saver *saver = XD_NEW(xd_saver);
    saver->k = (kind)k;
    saver->w = XD_MAX(width, 1);
    saver->h = XD_MAX(height, 1);
    saver->scale = scale > 0 ? scale : 1.0;
    saver->rng = seed != 0 ? seed : 0x9E3779B97F4A7C15ULL; /* xorshift never leaves 0 */
    if (saver_settings != nullptr)
        saver->o = *saver_settings;
    else
        xd_saver_settings_defaults(&saver->o);
    saver_init(saver);
    return saver;
}

xd_saver *xd_saver_create(const char *id, const xd_saver_settings *saver_settings, int width,
                          int height, double scale) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t seed = ((uint64_t)now.tv_nsec << 20) ^ (uint64_t)now.tv_sec ^ 0x9E3779B97F4A7C15ULL;
    return xd_saver_create_seeded(id, saver_settings, width, height, scale, seed);
}

void xd_saver_frame(xd_saver *saver, cairo_t *cairo, double dt) {
    dt = fmin(fmax(dt, 0.0), 0.25); /* after a stall, don't jump */
    saver->time += dt;
    switch (saver->k) {
    case K_BLANK:
        black(saver, cairo);
        break;
    case K_STARFIELD:
        starfield_frame(saver, cairo, dt);
        break;
    case K_MYSTIFY:
        mystify_frame(saver, cairo, dt);
        break;
    case K_BEZIERS:
        beziers_frame(saver, cairo, dt);
        break;
    case K_MARQUEE:
        marquee_frame(saver, cairo, dt);
        break;
    case K_FLYING:
        flying_frame(saver, cairo, dt);
        break;
    case K_PIPES:
        saver_pipes_frame(saver->pipes, cairo, dt);
        break;
    case K_TEXT3D:
        saver_text3d_frame(saver->text3d, cairo, dt);
        break;
    case K_MAZE:
        saver_maze_frame(saver->maze, cairo, dt);
        break;
    case K_FLOWER:
        saver_flower_frame(saver->flower, cairo, dt);
        break;
    }
}

void xd_saver_destroy(xd_saver *saver) {
    if (saver == nullptr)
        return;
    if (saver->canvas != nullptr)
        cairo_surface_destroy(saver->canvas);
    if (saver->pipes != nullptr)
        saver_pipes_destroy(saver->pipes);
    if (saver->text3d != nullptr)
        saver_text3d_destroy(saver->text3d);
    if (saver->maze != nullptr)
        saver_maze_destroy(saver->maze);
    if (saver->flower != nullptr)
        saver_flower_destroy(saver->flower);
    xd_free(saver);
}
