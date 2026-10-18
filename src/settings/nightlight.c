/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "nightlight.h"

static double clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }

/* Tanner Helland's fit to the black body colours of Mitchell Charity's
 * table ("How to Convert Temperature (K) to RGB: Algorithm and Sample
 * Code", tannerhelland.com, 2012), in 0..255 sRGB. */
static void helland(double kelvin, double out[3]) {
    double t = kelvin / 100.0;
    out[0] = t <= 66.0 ? 255.0 : 329.698727446 * pow(t - 60.0, -0.1332047592);
    out[1] = t <= 66.0 ? 99.4708025861 * log(t) - 161.1195681661
                       : 288.1221695283 * pow(t - 60.0, -0.0755148492);
    out[2] = t >= 66.0   ? 255.0
             : t <= 19.0 ? 0.0
                         : 138.5177312231 * log(t - 10.0) - 305.0447927307;
}

void xd_blackbody_rgb(double kelvin, double rgb[3]) {
    if (!(kelvin >= 1000.0))
        kelvin = 1000.0;
    if (kelvin > XD_NIGHTLIGHT_DAY)
        kelvin = XD_NIGHTLIGHT_DAY;
    /* Relative to 6500 K, which the fit puts a touch off white
     * (255, 254, 250): white stays white with night light off. */
    double c[3], white[3];
    helland(kelvin, c);
    helland(XD_NIGHTLIGHT_DAY, white);
    for (int i = 0; i < 3; i++)
        rgb[i] = clamp01(c[i] / white[i]);
}

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil). */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* NOAA's approximate sunrise/sunset equations ("General Solar Position
 * Calculations", NOAA Global Monitoring Division): the fractional year,
 * the equation of time and the sun's declination from Spencer's Fourier
 * series, and the hour angle at the zenith of 90.833 degrees (the sun's
 * upper edge on the horizon, with average refraction). Good to a minute
 * or two away from the poles. */
xd_sun_day xd_sun_times(int year, int month, int day, double latitude, double longitude,
                        double *rise_utc, double *set_utc) {
    int64_t doy = days_from_civil(year, month, day) - days_from_civil(year, 1, 1); /* 0-based */
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    double gamma = 2.0 * M_PI / (leap ? 366.0 : 365.0) * (double)doy; /* at noon: (12-12)/24 */
    double eqtime = 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma) -
                              0.014615 * cos(2 * gamma) - 0.040849 * sin(2 * gamma));
    double decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma) -
                  0.006758 * cos(2 * gamma) + 0.000907 * sin(2 * gamma) -
                  0.002697 * cos(3 * gamma) + 0.00148 * sin(3 * gamma);
    double lat = latitude * M_PI / 180.0;
    double cos_ha = cos(90.833 * M_PI / 180.0) / (cos(lat) * cos(decl)) - tan(lat) * tan(decl);
    *rise_utc = *set_utc = 0.0;
    if (cos_ha > 1.0)
        return XD_SUN_DOWN_ALL_DAY;
    if (cos_ha < -1.0)
        return XD_SUN_UP_ALL_DAY;
    double ha = acos(cos_ha) * 180.0 / M_PI;
    *rise_utc = 720.0 - 4.0 * (longitude + ha) - eqtime;
    *set_utc = 720.0 - 4.0 * (longitude - ha) - eqtime;
    return XD_SUN_RISES_AND_SETS;
}

static double wrap_day(double m) {
    m = fmod(m, 1440.0);
    return m < 0.0 ? m + 1440.0 : m;
}

double xd_nightlight_fraction(double t, double start, double end, double ramp) {
    double night = wrap_day(end - start);
    double r = fmin(fmax(ramp, 0.0), fmin(night, 1440.0 - night));
    double a = wrap_day(t - start); /* the night is [0, night) from here */
    if (r <= 0.0)
        return a < night ? 1.0 : 0.0;
    if (a < r / 2.0)
        return 0.5 + a / r;
    if (a >= 1440.0 - r / 2.0)
        return 0.5 - (1440.0 - a) / r;
    if (a < night - r / 2.0)
        return 1.0;
    if (a < night + r / 2.0)
        return 0.5 - (a - night) / r;
    return 0.0;
}

xd_sun_day xd_nightlight_times(const xd_nightlight_settings *s, time_t now, double *on_minute,
                               double *off_minute) {
    *on_minute = s->on_minute;
    *off_minute = s->off_minute;
    if (s->schedule != XD_NIGHTLIGHT_SUNSET || !s->has_location)
        return XD_SUN_RISES_AND_SETS;
    struct tm tm;
    if (localtime_r(&now, &tm) == nullptr)
        return XD_SUN_RISES_AND_SETS;
    double rise, set;
    xd_sun_day kind = xd_sun_times(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, s->latitude,
                                   s->longitude, &rise, &set);
    if (kind != XD_SUN_RISES_AND_SETS)
        return kind;
    double offset = (double)tm.tm_gmtoff / 60.0;
    *on_minute = wrap_day(set + offset);
    *off_minute = wrap_day(rise + offset);
    return kind;
}

double xd_nightlight_kelvin(const xd_nightlight_settings *s, time_t now) {
    if (!s->enabled)
        return XD_NIGHTLIGHT_DAY;
    double night_k = s->temperature;
    if (s->schedule == XD_NIGHTLIGHT_ALWAYS)
        return night_k;
    double on, off;
    double f;
    switch (xd_nightlight_times(s, now, &on, &off)) {
    case XD_SUN_UP_ALL_DAY:
        f = 0.0;
        break;
    case XD_SUN_DOWN_ALL_DAY:
        f = 1.0;
        break;
    case XD_SUN_RISES_AND_SETS:
    default: {
        struct tm tm;
        if (localtime_r(&now, &tm) == nullptr)
            return XD_NIGHTLIGHT_DAY;
        double t = tm.tm_hour * 60.0 + tm.tm_min + tm.tm_sec / 60.0;
        f = xd_nightlight_fraction(t, on, off, XD_NIGHTLIGHT_RAMP_MINUTES);
        break;
    }
    }
    return XD_NIGHTLIGHT_DAY - f * (XD_NIGHTLIGHT_DAY - night_k);
}

void xd_gamma_ramp_fill(uint16_t *r, uint16_t *g, uint16_t *b, size_t n, const double rgb[3],
                        const float *calibration) {
    uint16_t *ramps[3] = {r, g, b};
    for (size_t c = 0; c < 3; c++) {
        for (size_t i = 0; i < n; i++) {
            double x = n > 1 ? (double)i / (double)(n - 1) : 0.0;
            if (calibration != nullptr)
                x = clamp01((double)calibration[c * n + i]);
            ramps[c][i] = (uint16_t)lround(clamp01(x * rgb[c]) * 65535.0);
        }
    }
}
