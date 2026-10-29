/* SPDX-License-Identifier: MIT
 * Digital Rain's maths: a window built out of falling glyphs from the
 * bottom up (opening, restoring) or unbuilt into them from the top
 * down (closing, minimizing). Pure, no wlroots or GL, so it's
 * unit-tested; rain.c's shader works out the same per cell of glyphs
 * (each column's jitter and drops from its own hashes).
 *
 * Everything is in cells: the window is `rows` glyphs high, row 0 at
 * the top. Progress is the closing's, as Animations' events have it (0
 * the window whole, 1 gone): opening plays it from 1 back to 0, so the
 * front that falls through a closing window rises through an opening
 * one.
 */
#ifndef XD_COMPOSITOR_EFFECTS_RAIN_MODEL_H
#define XD_COMPOSITOR_EFFECTS_RAIN_MODEL_H

#include <stdint.h>

/* How many rows below the front glyphs and the window's pixels mix: a
 * glyph lands bright and turns into what's there. */
double xd_digital_rain_get_band(void);
/* How far, in rows, a column's front can be ahead of or behind the
 * others' in a window `rows` high: its raggedness. */
double xd_digital_rain_get_reach(int rows);
/* The row a column's front is at (rows from the top; cells above it
 * are rain, those below it the window), `progress` (0..1) of the way,
 * its `jitter` (-1..1) putting it ahead or behind. At 0 every cell is
 * the window, however ragged; at 1 none is. */
double xd_digital_rain_get_front(double progress, int rows, double jitter);
/* How far the cell at `row` has been built, with its column's front at
 * `front`: 0 rain only (above the front); past 0 the window shows there,
 * a glyph that has just landed flashing bright over it and fading as
 * this rises to 1 over the band below the front; 1 the window as it
 * is. */
double xd_digital_rain_get_reveal(int row, double front);
/* The share of drops that fall in a cell above the front: thickest
 * just above it (where the window has just broken up, or is about to
 * be built), thinning out over xd_digital_rain_get_falloff rows to
 * xd_digital_rain_get_sparse further up, all times the envelope. */
double xd_digital_rain_get_density(int row, double front, int rows, double progress);
double xd_digital_rain_get_falloff(int rows);
double xd_digital_rain_get_sparse(void);
/* How much of the rain shows at all: it fades in and out at the gone
 * end (progress near 1), so nothing is left once a window has gone and
 * an opening one's rain doesn't start all at once. */
double xd_digital_rain_get_envelope(double progress);
/* How fast drops fall, rows a second, in a window `rows` high: about a
 * window's height a second, within bounds, each column a little faster
 * or slower. */
double xd_digital_rain_get_fall_speed(int rows);

/* A column's drops: one every `period` rows, falling `fall` rows so
 * far (its speed times the time, plus its start). How many rows a cell
 * at `row` is behind the last drop's head (0 up to period); the drop's
 * number, which picks whether it falls (the density) and its glyphs. */
double xd_digital_rain_get_behind(double fall, double period, int row, int64_t *drop);
/* How bright a glyph is that many rows behind its drop's head, with a
 * trail `trail` rows long: 1 at the head, falling to 0 at its end. */
double xd_digital_rain_get_brightness(double behind, double trail);
/* A glyph's colour (0..1, unpremultiplied) at that brightness, in the
 * rain's colour `base`: the head near white, then bright for most of
 * the trail and dark only towards its end, as the screen saver draws
 * them. */
void xd_digital_rain_get_glyph_color(double brightness, const double base[3], double out[3]);
/* How opaque a glyph is at that brightness: over the desktop or the
 * window, a trail fades out towards its end rather than to black. */
double xd_digital_rain_get_glyph_opacity(double brightness);

#endif
