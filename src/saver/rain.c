/* SPDX-License-Identifier: MIT
 * Digital Rain: columns of glyphs falling down a black screen, each led
 * by a bright, near-white glyph with a trail fading to dark behind it,
 * the trail's glyphs changing now and then. A Linux addition after the
 * film's "digital rain", with no Windows original.
 *
 * Each glyph is drawn once, through Pango (rain_glyphs.c, shared with
 * the compositor's Digital Rain effect), into a small A8 mask, and
 * every frame paints those masks in the colour each place needs (the
 * film's half-width katakana mirrored, as it showed them). Masks keep
 * one cache for every colour, Rainbow's hues and the trail's continuous
 * fade; pre-tinted copies at a few levels paint a little faster
 * (pixman's plain OVER against its masked one: about 0.35 ms a frame at
 * 2560x1440) but need a set for each hue.
 */
#include "rain.h"

#include <math.h>
#include <stdlib.h>

#include <pango/pangocairo.h>

#include "../core/alloc.h"
#include "rain_glyphs.h"
#include "saver_impl.h"

/* The glyphs' height at 96 DPI, small to large. */
static const double RAIN_FONT_PX[XD_RAIN_SIZE_COUNT] = {
    [XD_RAIN_SMALL] = 12, [XD_RAIN_MEDIUM] = 18, [XD_RAIN_LARGE] = 26};
/* Below this the glyphs are smudges: the preview monitor's miniature. */
static constexpr double RAIN_FONT_PX_MIN = 6;
/* How often a lit glyph in a trail changes, per second. */
static constexpr double RAIN_FLICKER = 0.7;
static constexpr int RAIN_GLYPHS_MAX = XD_RAIN_GLYPHS_MAX;

struct saver_rain {
    uint64_t *rng;
    int w, h;
    int cell_w, cell_h;
    int columns, rows;
    int left; /* the columns are centred */
    saver_rain_pace pace;
    saver_rain_column *column_states;
    uint8_t *glyph_at; /* columns x rows, which glyph shows there */
    cairo_surface_t *glyphs[RAIN_GLYPHS_MAX];
    int glyph_count;
    cairo_surface_t *glow;
    int glow_size;
    xd_rain_color color;
    double base[3]; /* the colour, but for Rainbow */
    bool has_glow;
    double time;
};

/* ---- the column model ------------------------------------------------------------ */

/* The row a position falls in (rounded down, above the top too). */
static int rain_get_row(double position) {
    double row = floor(position);
    return (int)row;
}

/* A slider position as a factor: 1 in the middle, `lo` and `hi` at the ends. */
static double rain_get_factor(int position, double lo, double hi) {
    double offset = (double)position / (double)XD_SAVER_SLIDER_MAX * 2.0 - 1.0;
    return offset < 0 ? 1.0 + offset * (1.0 - lo) : 1.0 + offset * (hi - 1.0);
}

saver_rain_pace saver_rain_get_pace(int rows, int speed, int density) {
    rows = XD_MAX(rows, 1);
    /* As screen heights a second, so the preview's miniature falls like the
     * real screen: about four seconds from top to bottom. */
    double rows_per_second = rows * 0.28 * rain_get_factor(speed, 0.25, 3.0);
    double share = 0.25 + 0.7 * (double)density / (double)XD_SAVER_SLIDER_MAX;
    return (saver_rain_pace){.rows = rows, .speed = rows_per_second, .density = share};
}

/* A fresh fall from just above the top. */
static void rain_spawn_column(saver_rain_column *column, const saver_rain_pace *pace,
                              uint64_t *rng) {
    column->speed = pace->speed * xd_saver_between(rng, 0.5, 1.5);
    int shortest = XD_MAX(3, pace->rows / 4);
    int longest = XD_MAX(shortest + 1, pace->rows * 9 / 10);
    double trail = xd_saver_between(rng, shortest, longest);
    column->trail = (int)trail;
    column->head = -xd_saver_between(rng, 0.0, 3.0);
    column->wait = 0;
}

/* A wait that, on average, keeps the share of falling columns at the
 * density: from a few moments to twice the mean. */
static double rain_pick_wait(const saver_rain_column *column, const saver_rain_pace *pace,
                             uint64_t *rng) {
    double fall = (pace->rows + column->trail) / fmax(column->speed, 0.01);
    double mean = fall * (1.0 / pace->density - 1.0);
    return fmax(0.05, mean * xd_saver_between(rng, 0.1, 1.9));
}

void saver_rain_column_init(saver_rain_column *column, const saver_rain_pace *pace,
                            uint64_t *rng) {
    rain_spawn_column(column, pace, rng);
    if (xd_saver_rand(rng) < pace->density)
        column->head = xd_saver_between(rng, 0.0, pace->rows + column->trail);
    else
        column->wait = rain_pick_wait(column, pace, rng) * xd_saver_rand(rng) + 0.01;
}

void saver_rain_column_advance(saver_rain_column *column, const saver_rain_pace *pace, double dt,
                               uint64_t *rng) {
    if (column->wait > 0) {
        column->wait -= dt;
        if (column->wait <= 0)
            rain_spawn_column(column, pace, rng);
        return;
    }
    column->head += column->speed * dt;
    if (column->head - column->trail >= pace->rows)
        column->wait = rain_pick_wait(column, pace, rng);
}

double saver_rain_column_get_brightness(const saver_rain_column *column, int row) {
    if (column->wait > 0)
        return 0;
    double behind = rain_get_row(column->head) - row;
    if (behind < 0 || behind >= column->trail)
        return 0;
    return 1.0 - behind / column->trail;
}

/* ---- drawing ------------------------------------------------------------------------ */

/* One glyph, white on nothing, centred in a cell (rain_glyphs.c). */
static cairo_surface_t *rain_draw_glyph(saver_rain *rain, PangoLayout *layout,
                                        const xd_rain_glyph *glyph) {
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_A8, rain->cell_w, rain->cell_h);
    cairo_t *cairo = cairo_create(surface);
    xd_rain_draw_glyph(cairo, layout, glyph, 0, 0, rain->cell_w, rain->cell_h);
    cairo_destroy(cairo);
    return surface;
}

static void rain_draw_glyphs(saver_rain *rain, xd_rain_characters characters, double font_px) {
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cairo = cairo_create(scratch);
    PangoLayout *layout = xd_rain_create_layout(cairo, characters, font_px);
    xd_rain_glyph glyphs[XD_RAIN_GLYPHS_MAX];
    size_t count = xd_rain_list_glyphs(characters, glyphs);
    for (size_t i = 0; i < count && rain->glyph_count < RAIN_GLYPHS_MAX; i++)
        rain->glyphs[rain->glyph_count++] = rain_draw_glyph(rain, layout, &glyphs[i]);
    g_object_unref(layout);
    cairo_destroy(cairo);
    cairo_surface_destroy(scratch);
}

/* A soft round light, painted behind each leading glyph. */
static void rain_draw_glow(saver_rain *rain) {
    rain->glow_size = XD_MAX(4, rain->cell_h * 2);
    rain->glow = cairo_image_surface_create(CAIRO_FORMAT_A8, rain->glow_size, rain->glow_size);
    cairo_t *cairo = cairo_create(rain->glow);
    double middle = rain->glow_size / 2.0;
    cairo_pattern_t *light = cairo_pattern_create_radial(middle, middle, 0, middle, middle, middle);
    cairo_pattern_add_color_stop_rgba(light, 0.0, 0, 0, 0, 0.6);
    cairo_pattern_add_color_stop_rgba(light, 0.4, 0, 0, 0, 0.2);
    cairo_pattern_add_color_stop_rgba(light, 1.0, 0, 0, 0, 0.0);
    cairo_set_source(cairo, light);
    cairo_paint(cairo);
    cairo_pattern_destroy(light);
    cairo_destroy(cairo);
}

/* A bright hue, 0..1 round the colour wheel. */
static void rain_get_hue(double hue, double *rgb) {
    hue = (hue - floor(hue)) * 6.0;
    rgb[0] = fmin(fmax(fabs(hue - 3.0) - 1.0, 0.0), 1.0);
    rgb[1] = fmin(fmax(2.0 - fabs(hue - 2.0), 0.0), 1.0);
    rgb[2] = fmin(fmax(2.0 - fabs(hue - 4.0), 0.0), 1.0);
}

saver_rain *saver_rain_create(const xd_saver_settings *saver_settings, int width, int height,
                              double scale, uint64_t *rng) {
    saver_rain *rain = XD_NEW(saver_rain);
    rain->rng = rng;
    rain->w = width;
    rain->h = height;
    rain->color = saver_settings->rain.color;
    rain->has_glow = saver_settings->rain.glow;
    xd_rain_get_base_color(rain->color, saver_settings->rain.custom_color, rain->base);

    xd_rain_glyph_size size =
        saver_settings->rain.size < XD_RAIN_SIZE_COUNT ? saver_settings->rain.size : XD_RAIN_MEDIUM;
    double font_px = fmax(RAIN_FONT_PX_MIN, RAIN_FONT_PX[size] * scale);
    xd_rain_get_cell_size(font_px, &rain->cell_w, &rain->cell_h);
    rain->columns = XD_MAX(1, width / rain->cell_w);
    rain->rows = height / rain->cell_h + 1; /* the last one cut off at the bottom */
    rain->left = (width - rain->columns * rain->cell_w) / 2;
    rain->pace =
        saver_rain_get_pace(rain->rows, saver_settings->rain.speed, saver_settings->rain.density);

    xd_rain_characters characters = xd_rain_pick_characters(
        saver_settings->rain.characters,
        saver_settings->rain.characters != XD_RAIN_KATAKANA || xd_rain_has_katakana());
    rain_draw_glyphs(rain, characters, font_px);
    if (rain->has_glow)
        rain_draw_glow(rain);

    rain->column_states = xd_alloc_array((size_t)rain->columns, sizeof(saver_rain_column));
    size_t cells = (size_t)rain->columns * (size_t)rain->rows;
    rain->glyph_at = xd_alloc_array(cells, 1);
    for (size_t i = 0; i < cells; i++)
        rain->glyph_at[i] = (uint8_t)(xd_saver_rand(rng) * rain->glyph_count);
    for (int i = 0; i < rain->columns; i++)
        saver_rain_column_init(&rain->column_states[i], &rain->pace, rng);
    return rain;
}

static uint8_t rain_pick_glyph(saver_rain *rain) {
    return (uint8_t)(xd_saver_rand(rain->rng) * rain->glyph_count);
}

/* One column: falls, its glyphs change, and it's painted. */
static void rain_draw_column(saver_rain *rain, cairo_t *cairo, int index, double dt) {
    saver_rain_column *column = &rain->column_states[index];
    bool was_falling = column->wait <= 0;
    int head_before = rain_get_row(column->head);
    saver_rain_column_advance(column, &rain->pace, dt, rain->rng);
    if (column->wait > 0)
        return;
    uint8_t *glyphs = &rain->glyph_at[(size_t)index * (size_t)rain->rows];
    int head = rain_get_row(column->head);
    /* Each row the head reaches shows a new glyph. */
    for (int row = was_falling ? head_before + 1 : head; row <= head; row++) {
        if (row >= 0 && row < rain->rows)
            glyphs[row] = rain_pick_glyph(rain);
    }

    double color[3] = {rain->base[0], rain->base[1], rain->base[2]};
    if (rain->color == XD_RAIN_RAINBOW)
        rain_get_hue((double)index / rain->columns + rain->time * 0.04, color);
    int x = rain->left + index * rain->cell_w;
    int first = XD_MAX(0, head - column->trail + 1);
    int last = XD_MIN(head, rain->rows - 1);
    for (int row = first; row <= last; row++) {
        double brightness = saver_rain_column_get_brightness(column, row);
        if (brightness <= 0)
            continue;
        if (row != head && xd_saver_rand(rain->rng) < dt * RAIN_FLICKER)
            glyphs[row] = rain_pick_glyph(rain);
        int y = row * rain->cell_h;
        cairo_surface_t *glyph = rain->glyphs[glyphs[row]];
        if (row == head) {
            if (rain->glow != nullptr) {
                /* Added, as light is, rather than covering the trail. */
                cairo_set_operator(cairo, CAIRO_OPERATOR_ADD);
                cairo_set_source_rgba(cairo, color[0], color[1], color[2], 0.5);
                cairo_mask_surface(cairo, rain->glow, x + (rain->cell_w - rain->glow_size) / 2,
                                   y + (rain->cell_h - rain->glow_size) / 2);
                cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
            }
            /* Near white, a touch of the colour. */
            cairo_set_source_rgb(cairo, 0.8 + 0.2 * color[0], 0.8 + 0.2 * color[1],
                                 0.8 + 0.2 * color[2]);
        } else {
            /* Bright for most of the trail, dark only towards its end; the
             * glyph just behind the head still a little white. */
            double level = brightness * (1.6 - 0.6 * brightness);
            double white = fmax(0.0, brightness - 0.9) * 2.5;
            cairo_set_source_rgb(cairo, (color[0] + (1 - color[0]) * white) * level,
                                 (color[1] + (1 - color[1]) * white) * level,
                                 (color[2] + (1 - color[2]) * white) * level);
        }
        cairo_mask_surface(cairo, glyph, x, y);
    }
}

void saver_rain_frame(saver_rain *rain, cairo_t *cairo, double dt) {
    rain->time += dt;
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_rectangle(cairo, 0, 0, rain->w, rain->h);
    cairo_fill(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    for (int i = 0; i < rain->columns; i++)
        rain_draw_column(rain, cairo, i, dt);
    cairo_restore(cairo);
}

void saver_rain_destroy(saver_rain *rain) {
    if (rain == nullptr)
        return;
    for (int i = 0; i < rain->glyph_count; i++)
        cairo_surface_destroy(rain->glyphs[i]);
    if (rain->glow != nullptr)
        cairo_surface_destroy(rain->glow);
    xd_free(rain->column_states);
    xd_free(rain->glyph_at);
    xd_free(rain);
}
