/* SPDX-License-Identifier: MIT */
#include <math.h>

#include "nightlight.h"

static double clamp01(double value) { return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value; }

/* Tanner Helland's fit to the black body colours of Mitchell Charity's
 * table ("How to Convert Temperature (K) to RGB: Algorithm and Sample
 * Code", tannerhelland.com, 2012), in 0..255 sRGB. */
static void helland(double kelvin, double out[3]) {
    double hundreds = kelvin / 100.0;
    out[0] = hundreds <= 66.0 ? 255.0 : 329.698727446 * pow(hundreds - 60.0, -0.1332047592);
    out[1] = hundreds <= 66.0 ? 99.4708025861 * log(hundreds) - 161.1195681661
                              : 288.1221695283 * pow(hundreds - 60.0, -0.0755148492);
    out[2] = hundreds >= 66.0   ? 255.0
             : hundreds <= 19.0 ? 0.0
                                : 138.5177312231 * log(hundreds - 10.0) - 305.0447927307;
}

void xd_blackbody_rgb(double kelvin, double rgb[3]) {
    if (!(kelvin >= 1000.0))
        kelvin = 1000.0;
    /* Daylight is exactly white, not the fit divided by itself: an
     * optimizer may fold one of the two and fuse the other's multiply-adds,
     * leaving a ramp a hair off identity with night light off. */
    if (kelvin >= XD_NIGHTLIGHT_DAY) {
        rgb[0] = rgb[1] = rgb[2] = 1.0;
        return;
    }
    /* Relative to 6500 K, which the fit puts a touch off white
     * (255, 254, 250): white stays white with night light off. */
    double color[3], white[3];
    helland(kelvin, color);
    helland(XD_NIGHTLIGHT_DAY, white);
    for (int i = 0; i < 3; i++)
        rgb[i] = clamp01(color[i] / white[i]);
}

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil). */
static int64_t days_from_civil(int64_t y, int month, int day) {
    y -= month <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
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

static double wrap_day(double minute) {
    minute = fmod(minute, 1440.0);
    return minute < 0.0 ? minute + 1440.0 : minute;
}

double xd_nightlight_fraction(double minute, double start, double end, double ramp) {
    double night = wrap_day(end - start);
    double ramp_width = fmin(fmax(ramp, 0.0), fmin(night, 1440.0 - night));
    double since_start = wrap_day(minute - start); /* the night is [0, night) from here */
    if (ramp_width <= 0.0)
        return since_start < night ? 1.0 : 0.0;
    if (since_start < ramp_width / 2.0)
        return 0.5 + since_start / ramp_width;
    if (since_start >= 1440.0 - ramp_width / 2.0)
        return 0.5 - (1440.0 - since_start) / ramp_width;
    if (since_start < night - ramp_width / 2.0)
        return 1.0;
    if (since_start < night + ramp_width / 2.0)
        return 0.5 - (since_start - night) / ramp_width;
    return 0.0;
}

xd_sun_day xd_nightlight_times(const xd_nightlight_settings *nightlight_settings, time_t now,
                               double *on_minute, double *off_minute) {
    *on_minute = nightlight_settings->on_minute;
    *off_minute = nightlight_settings->off_minute;
    if (nightlight_settings->schedule != XD_NIGHTLIGHT_SUNSET || !nightlight_settings->has_location)
        return XD_SUN_RISES_AND_SETS;
    struct tm tm;
    if (localtime_r(&now, &tm) == nullptr)
        return XD_SUN_RISES_AND_SETS;
    double rise, set;
    xd_sun_day kind =
        xd_sun_times(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, nightlight_settings->latitude,
                     nightlight_settings->longitude, &rise, &set);
    if (kind != XD_SUN_RISES_AND_SETS)
        return kind;
    double offset = (double)tm.tm_gmtoff / 60.0;
    *on_minute = wrap_day(set + offset);
    *off_minute = wrap_day(rise + offset);
    return kind;
}

double xd_nightlight_kelvin(const xd_nightlight_settings *nightlight_settings, time_t now) {
    if (!nightlight_settings->enabled)
        return XD_NIGHTLIGHT_DAY;
    double night_k = nightlight_settings->temperature;
    if (nightlight_settings->schedule == XD_NIGHTLIGHT_ALWAYS)
        return night_k;
    double on, off;
    double fraction;
    switch (xd_nightlight_times(nightlight_settings, now, &on, &off)) {
    case XD_SUN_UP_ALL_DAY:
        fraction = 0.0;
        break;
    case XD_SUN_DOWN_ALL_DAY:
        fraction = 1.0;
        break;
    case XD_SUN_RISES_AND_SETS:
    default: {
        struct tm tm;
        if (localtime_r(&now, &tm) == nullptr)
            return XD_NIGHTLIGHT_DAY;
        double minute = tm.tm_hour * 60.0 + tm.tm_min + tm.tm_sec / 60.0;
        fraction = xd_nightlight_fraction(minute, on, off, XD_NIGHTLIGHT_RAMP_MINUTES);
        break;
    }
    }
    return XD_NIGHTLIGHT_DAY - fraction * (XD_NIGHTLIGHT_DAY - night_k);
}

void xd_gamma_ramp_fill(uint16_t *red, uint16_t *green, uint16_t *blue, size_t size,
                        const double rgb[3], const float *calibration) {
    uint16_t *ramps[3] = {red, green, blue};
    for (size_t channel = 0; channel < 3; channel++) {
        for (size_t i = 0; i < size; i++) {
            double x = size > 1 ? (double)i / (double)(size - 1) : 0.0;
            if (calibration != nullptr)
                x = clamp01((double)calibration[channel * size + i]);
            ramps[channel][i] = (uint16_t)lround(clamp01(x * rgb[channel]) * 65535.0);
        }
    }
}
