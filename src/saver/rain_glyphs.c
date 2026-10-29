/* SPDX-License-Identifier: MIT
 * Digital Rain's glyphs (rain_glyphs.h).
 */
#include "rain_glyphs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "../core/common.h"

/* Fontconfig falls back through every font for each character, so a
 * katakana family that's missing is fine as long as some font has them. */
static const char RAIN_KATAKANA_FONT[] = "Noto Sans Mono CJK JP, Monospace";
static const char RAIN_LATIN_FONT[] = "Monospace";

static constexpr unsigned RAIN_KATAKANA_FIRST = 0xFF66;
static constexpr unsigned RAIN_KATAKANA_LAST = 0xFF9D;

xd_rain_characters xd_rain_pick_characters(xd_rain_characters wanted, bool has_katakana) {
    return wanted == XD_RAIN_KATAKANA && !has_katakana ? XD_RAIN_LETTERS : wanted;
}

/* The katakana's UTF-8, three bytes each. */
static void rain_get_katakana(unsigned code, char out[8]) {
    out[0] = (char)(0xE0u | (code >> 12));
    out[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
    out[2] = (char)(0x80u | (code & 0x3Fu));
    out[3] = '\0';
}

bool xd_rain_has_katakana(void) {
    const char *forced = getenv("XD_RAIN_NO_KATAKANA");
    if (forced != nullptr && forced[0] != '\0')
        return false;
    static int known = -1; /* the fonts don't change while we run */
    if (known >= 0)
        return known == 1;
    char text[64 * 3 + 1] = "";
    for (unsigned code = RAIN_KATAKANA_FIRST; code <= RAIN_KATAKANA_LAST; code++) {
        char one[8];
        rain_get_katakana(code, one);
        g_strlcat(text, one, sizeof(text));
    }
    PangoContext *context = pango_font_map_create_context(pango_cairo_font_map_get_default());
    PangoLayout *layout = pango_layout_new(context);
    PangoFontDescription *description = pango_font_description_from_string(RAIN_KATAKANA_FONT);
    pango_font_description_set_absolute_size(description, 16 * PANGO_SCALE);
    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    pango_layout_set_text(layout, text, -1);
    known = pango_layout_get_unknown_glyphs_count(layout) == 0 ? 1 : 0;
    g_object_unref(layout);
    g_object_unref(context);
    return known == 1;
}

static void rain_add_glyph(xd_rain_glyph glyphs[XD_RAIN_GLYPHS_MAX], size_t *count,
                           const char *text, bool mirrored) {
    if (*count >= XD_RAIN_GLYPHS_MAX)
        return;
    xd_rain_glyph *glyph = &glyphs[(*count)++];
    g_strlcpy(glyph->text, text, sizeof(glyph->text));
    glyph->mirrored = mirrored;
}

static void rain_add_range(xd_rain_glyph glyphs[XD_RAIN_GLYPHS_MAX], size_t *count, char first,
                           char last) {
    for (char character = first; character <= last; character++)
        rain_add_glyph(glyphs, count, (char[]){character, '\0'}, false);
}

size_t xd_rain_list_glyphs(xd_rain_characters characters,
                           xd_rain_glyph glyphs[XD_RAIN_GLYPHS_MAX]) {
    size_t count = 0;
    switch (characters) {
    case XD_RAIN_KATAKANA:
        /* Mirrored, as the film showed them. */
        for (unsigned code = RAIN_KATAKANA_FIRST; code <= RAIN_KATAKANA_LAST; code++) {
            char one[8];
            rain_get_katakana(code, one);
            rain_add_glyph(glyphs, &count, one, true);
        }
        [[fallthrough]];
    case XD_RAIN_DIGITS:
    case XD_RAIN_CHARACTERS_COUNT:
        rain_add_range(glyphs, &count, '0', '9');
        break;
    case XD_RAIN_BINARY:
        rain_add_range(glyphs, &count, '0', '1');
        break;
    case XD_RAIN_LETTERS:
        rain_add_range(glyphs, &count, 'A', 'Z');
        rain_add_range(glyphs, &count, '0', '9');
        break;
    }
    return count;
}

void xd_rain_get_cell_size(double font_px, int *width, int *height) {
    *width = XD_MAX(3, (int)lround(font_px * 0.72));
    *height = XD_MAX(4, (int)lround(font_px * 1.12));
}

PangoLayout *xd_rain_create_layout(cairo_t *cairo, xd_rain_characters characters, double font_px) {
    PangoLayout *layout = pango_cairo_create_layout(cairo);
    cairo_font_options_t *options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(pango_layout_get_context(layout), options);
    cairo_font_options_destroy(options);
    pango_layout_context_changed(layout);
    PangoFontDescription *description = pango_font_description_from_string(
        characters == XD_RAIN_KATAKANA ? RAIN_KATAKANA_FONT : RAIN_LATIN_FONT);
    pango_font_description_set_absolute_size(description, font_px * PANGO_SCALE);
    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    return layout;
}

void xd_rain_draw_glyph(cairo_t *cairo, PangoLayout *layout, const xd_rain_glyph *glyph, double x,
                        double y, int cell_width, int cell_height) {
    pango_layout_set_text(layout, glyph->text, -1);
    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout, &ink, nullptr);
    double left = floor((cell_width - ink.width) / 2.0) - ink.x;
    double top = floor((cell_height - ink.height) / 2.0) - ink.y;
    cairo_save(cairo);
    /* Its own cell only: an atlas's neighbours stay clean. */
    cairo_rectangle(cairo, x, y, cell_width, cell_height);
    cairo_clip(cairo);
    cairo_translate(cairo, x, y);
    if (glyph->mirrored) {
        cairo_translate(cairo, cell_width, 0);
        cairo_scale(cairo, -1, 1);
    }
    cairo_move_to(cairo, left, top);
    pango_cairo_update_layout(cairo, layout);
    pango_cairo_show_layout(cairo, layout);
    cairo_restore(cairo);
}

void xd_rain_get_base_color(xd_rain_color color, xd_rgb custom, double rgb[3]) {
    /* Rainbow's columns have hues of their own; this is for its first frame. */
    static const double colors[XD_RAIN_COLOR_COUNT][3] = {[XD_RAIN_GREEN] = {0.0, 1.0, 0.27},
                                                          [XD_RAIN_AMBER] = {1.0, 0.62, 0.05},
                                                          [XD_RAIN_BLUE] = {0.2, 0.55, 1.0},
                                                          [XD_RAIN_WHITE] = {0.85, 0.88, 0.9},
                                                          [XD_RAIN_RAINBOW] = {0.0, 1.0, 0.27}};
    if (color == XD_RAIN_CUSTOM) {
        rgb[0] = custom.r / 255.0;
        rgb[1] = custom.g / 255.0;
        rgb[2] = custom.b / 255.0;
        return;
    }
    if (color >= XD_RAIN_COLOR_COUNT)
        color = XD_RAIN_GREEN;
    memcpy(rgb, colors[color], sizeof(colors[color]));
}
