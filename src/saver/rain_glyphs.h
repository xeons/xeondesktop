/* SPDX-License-Identifier: MIT
 * Digital Rain's glyphs, shared by the screen saver (rain.c) and the
 * compositor's Digital Rain window effect (src/compositor/effects/rain.c):
 * which characters each set has, the film's half-width katakana mirrored
 * as it showed them, the fallback to letters without a font for them,
 * drawing one glyph centred in its cell through Pango, and the colours.
 */
#ifndef XD_SAVER_RAIN_GLYPHS_H
#define XD_SAVER_RAIN_GLYPHS_H

#include <stddef.h>

#include <pango/pangocairo.h>

#include "../settings/settings.h"

/* The most glyphs a set has: katakana and digits, 66. */
enum { XD_RAIN_GLYPHS_MAX = 80 };

typedef struct xd_rain_glyph {
    char text[8];  /* UTF-8 */
    bool mirrored; /* drawn left to right reversed: the katakana */
} xd_rain_glyph;

/* The characters drawn for those asked for: without a font that has
 * half-width katakana, letters and digits rather than boxes. */
xd_rain_characters xd_rain_pick_characters(xd_rain_characters wanted, bool has_katakana);
/* Whether the fonts cover U+FF66..U+FF9D (asked of Pango once). False
 * when XD_RAIN_NO_KATAKANA is set, so tests can try the fallback. */
bool xd_rain_has_katakana(void);
/* The glyphs of a set, in order; how many (at most XD_RAIN_GLYPHS_MAX). */
size_t xd_rain_list_glyphs(xd_rain_characters characters, xd_rain_glyph glyphs[XD_RAIN_GLYPHS_MAX]);
/* A glyph's cell for a font `font_px` high: a little narrower than it is
 * high, as the film's columns were. */
void xd_rain_get_cell_size(double font_px, int *width, int *height);
/* A layout on `cairo`'s context in the set's font, `font_px` high, grey
 * antialiased with unhinted metrics (glyphs centre the same in every
 * cell); g_object_unref it. */
PangoLayout *xd_rain_create_layout(cairo_t *cairo, xd_rain_characters characters, double font_px);
/* Draws a glyph with `cairo`'s source, centred in the cell whose top left
 * is `x`, `y`. */
void xd_rain_draw_glyph(cairo_t *cairo, PangoLayout *layout, const xd_rain_glyph *glyph, double x,
                        double y, int cell_width, int cell_height);

/* The colour glyphs of `color` are drawn in, 0..1; Rainbow's first
 * (its columns have hues of their own), `custom` for Custom. */
void xd_rain_get_base_color(xd_rain_color color, xd_rgb custom, double rgb[3]);

#endif
