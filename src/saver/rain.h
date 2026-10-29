/* SPDX-License-Identifier: MIT
 * Digital Rain's column model and its choice of characters, apart from
 * the drawing (rain.c) so the unit tests can check them without a
 * screen: where each column's leading glyph is, how fast it falls, how
 * long its trail is and how long it waits before falling again. Its
 * characters are rain_glyphs.h's.
 */
#ifndef XD_SAVER_RAIN_H
#define XD_SAVER_RAIN_H

#include <stdint.h>

#include "../settings/settings.h"

/* What every column of one screen shares. */
typedef struct saver_rain_pace {
    int rows;       /* glyphs from top to bottom */
    double speed;   /* a column's mean fall, rows a second */
    double density; /* the share of the time a column is falling, 0..1 */
} saver_rain_pace;

typedef struct saver_rain_column {
    double head;  /* the leading glyph's row, fractions between rows; may start above the top */
    double speed; /* rows a second */
    int trail;    /* rows behind the head that still show, fading */
    double wait;  /* > 0: seconds before it falls again (nothing shows meanwhile) */
} saver_rain_column;

/* The pace for a screen `rows` glyphs high, from the Speed and Density
 * sliders (0..XD_SAVER_SLIDER_MAX). */
saver_rain_pace saver_rain_get_pace(int rows, int speed, int density);
/* The screen as it is a while after the saver starts: some columns
 * falling, part way down, and the rest waiting. */
void saver_rain_column_init(saver_rain_column *column, const saver_rain_pace *pace, uint64_t *rng);
/* Falls dt seconds; once the trail has left the bottom it waits, then
 * starts again from the top at another speed and length. */
void saver_rain_column_advance(saver_rain_column *column, const saver_rain_pace *pace, double dt,
                               uint64_t *rng);
/* How bright the glyph at `row` is: 1 at the head, fading to 0 at the
 * end of the trail; 0 where nothing shows. */
double saver_rain_column_get_brightness(const saver_rain_column *column, int row);

#endif
