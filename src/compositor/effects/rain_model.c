/* SPDX-License-Identifier: MIT
 * Digital Rain's maths (rain_model.h).
 */
#include "rain_model.h"

#include <math.h>

#include "../../core/common.h"

/* Rows below the front where a landing glyph turns into the window. */
static constexpr double BAND = 1.5;
/* A column's front is up to this share of the window's rows (and at
 * least a row) ahead of or behind the others'. */
static constexpr double REACH_SHARE = 0.06;
/* Above the front the drops thin out over this share of the rows (at
 * least FALLOFF_MIN), to SPARSE of them. */
static constexpr double FALLOFF_SHARE = 0.2;
static constexpr double FALLOFF_MIN = 3.0;
static constexpr double SPARSE = 0.25;
/* The rain fades in and out over this last share of the progress. */
static constexpr double FADE_SHARE = 0.15;
/* A drop falls about this many window heights a second: the same pace
 * in a small window as a big one, within bounds (rows a second). */
static constexpr double FALL_HEIGHTS = 1.0;
static constexpr double FALL_MIN = 12.0;
static constexpr double FALL_MAX = 60.0;

double xd_digital_rain_get_band(void) {
    return BAND;
}

double xd_digital_rain_get_reach(int rows) {
    return fmax(1.0, (double)rows * REACH_SHARE);
}

double xd_digital_rain_get_front(double progress, int rows, double jitter) {
    double reach = xd_digital_rain_get_reach(rows);
    double span = (double)rows + 2 * reach + BAND;
    /* From above the top, past the band and the raggedness (every cell
     * whole), to below the bottom however far behind a column is. */
    return XD_CLAMP(progress, 0.0, 1.0) * span - reach - BAND + XD_CLAMP(jitter, -1.0, 1.0) * reach;
}

double xd_digital_rain_get_reveal(int row, double front) {
    return XD_CLAMP(((double)row + 0.5 - front) / BAND, 0.0, 1.0);
}

static double rain_smoothstep(double from, double to, double value) {
    double share = XD_CLAMP((value - from) / (to - from), 0.0, 1.0);
    return share * share * (3 - 2 * share);
}

double xd_digital_rain_get_envelope(double progress) {
    return 1 - rain_smoothstep(1 - FADE_SHARE, 1, progress);
}

double xd_digital_rain_get_falloff(int rows) {
    return fmax(FALLOFF_MIN, (double)rows * FALLOFF_SHARE);
}

double xd_digital_rain_get_sparse(void) {
    return SPARSE;
}

double xd_digital_rain_get_density(int row, double front, int rows, double progress) {
    double envelope = xd_digital_rain_get_envelope(progress);
    double above = front - ((double)row + 0.5);
    if (above <= 0)
        return envelope;
    double near = exp(-above / xd_digital_rain_get_falloff(rows));
    return (SPARSE + (1 - SPARSE) * near) * envelope;
}

double xd_digital_rain_get_fall_speed(int rows) {
    return XD_CLAMP((double)rows * FALL_HEIGHTS, FALL_MIN, FALL_MAX);
}

double xd_digital_rain_get_behind(double fall, double period, int row, int64_t *drop) {
    period = fmax(period, 1.0);
    double distance = fall - (double)row;
    double number = floor(distance / period);
    *drop = (int64_t)number;
    return distance - number * period;
}

double xd_digital_rain_get_brightness(double behind, double trail) {
    double cell = floor(behind);
    if (behind < 0 || trail <= 0 || cell >= trail)
        return 0;
    return 1 - cell / trail;
}

void xd_digital_rain_get_glyph_color(double brightness, const double base[3], double out[3]) {
    for (int i = 0; i < 3; i++) {
        if (brightness >= 1) {
            /* Near white, a touch of the colour. */
            out[i] = 0.8 + 0.2 * base[i];
            continue;
        }
        double level = brightness * (1.6 - 0.6 * brightness);
        double white = fmax(0.0, brightness - 0.9) * 2.5;
        out[i] = (base[i] + (1 - base[i]) * white) * level;
    }
}

double xd_digital_rain_get_glyph_opacity(double brightness) {
    return XD_CLAMP(brightness * 1.5, 0.0, 1.0);
}
