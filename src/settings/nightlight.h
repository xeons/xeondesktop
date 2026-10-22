/* SPDX-License-Identifier: MIT
 * Night light's arithmetic, shared by the compositor (which applies it)
 * and Display Properties (which shows tonight's times): colour
 * temperatures as RGB, sunrise and sunset, the schedule with its gradual
 * transitions, and the gamma ramps that combine a monitor's calibration
 * with the night light.
 */
#ifndef XD_SETTINGS_NIGHTLIGHT_H
#define XD_SETTINGS_NIGHTLIGHT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "settings.h"

/* How long the change takes at each end of the night, centred on the
 * time it's due, in minutes. */
enum { XD_NIGHTLIGHT_RAMP_MINUTES = 30 };

/* XD_NIGHTLIGHT_K_DAY for arithmetic in kelvin. */
static constexpr double XD_NIGHTLIGHT_DAY = (double)XD_NIGHTLIGHT_K_DAY;

/* The RGB multipliers (0..1, all 1 at XD_NIGHTLIGHT_K_DAY) that make
 * white look like a black body at `kelvin` (1000..XD_NIGHTLIGHT_K_DAY). */
void xd_blackbody_rgb(double kelvin, double rgb[3]);

typedef enum xd_sun_day : uint8_t {
    XD_SUN_RISES_AND_SETS,
    XD_SUN_UP_ALL_DAY,   /* midnight sun */
    XD_SUN_DOWN_ALL_DAY, /* polar night */
} xd_sun_day;

/* Sunrise and sunset on a date (Gregorian, month 1..12) at a place
 * (degrees, north and east positive), in minutes after midnight UTC of
 * that date (they may fall outside 0..1440 far from Greenwich). */
xd_sun_day xd_sun_times(int year, int month, int day, double latitude, double longitude,
                        double *rise_utc, double *set_utc);

/* How far into the night (0 day .. 1 night) minute `minute` of the day is, for
 * a night from minute `start` to minute `end` (wrapping past midnight
 * when end < start), each change spread over `ramp` minutes centred on
 * it. A night shorter than the ramps never gets all the way. */
double xd_nightlight_fraction(double minute, double start, double end, double ramp);

/* Tonight's on and off times, in minutes after local midnight, for the
 * settings at `now`: sunset and sunrise at the location, else the set
 * hours. Returns XD_SUN_UP_ALL_DAY/XD_SUN_DOWN_ALL_DAY in the polar
 * summer and winter (no times). */
xd_sun_day xd_nightlight_times(const xd_nightlight_settings *nightlight_settings, time_t now,
                               double *on_minute, double *off_minute);

/* The colour temperature the settings ask for at `now`, in K
 * (XD_NIGHTLIGHT_K_DAY when off or in the day). */
double xd_nightlight_kelvin(const xd_nightlight_settings *nightlight_settings, time_t now);

/* Fills a gamma ramp of n entries per channel: the calibration curves
 * (`calibration`: 3 x n values 0..1, red then green then blue; nullptr
 * for none), then the night light's multipliers. */
void xd_gamma_ramp_fill(uint16_t *red, uint16_t *green, uint16_t *blue, size_t size,
                        const double rgb[3], const float *calibration);

#endif
