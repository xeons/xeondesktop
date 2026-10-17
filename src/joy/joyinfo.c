/* SPDX-License-Identifier: MIT */
#include <linux/input-event-codes.h>
#include <stdlib.h>

#include "../core/common.h"
#include "joyinfo.h"

double xd_joy_axis_fraction(int value, int min, int max) {
    if (max <= min)
        return 0.5;
    double f = ((double)value - min) / ((double)max - min);
    return XD_CLAMP(f, 0.0, 1.0);
}

double xd_joy_axis_centered(int value, int min, int max) {
    if (max <= min)
        return 0;
    return xd_joy_axis_fraction(value, min, max) * 2 - 1;
}

const char *xd_joy_axis_name(unsigned code) {
    static const char *const names[] = {
        [ABS_X] = "X Axis",
        [ABS_Y] = "Y Axis",
        [ABS_Z] = "Z Axis",
        [ABS_RX] = "X Rotation",
        [ABS_RY] = "Y Rotation",
        [ABS_RZ] = "Z Rotation",
        [ABS_THROTTLE] = "Throttle",
        [ABS_RUDDER] = "Rudder",
        [ABS_WHEEL] = "Wheel",
        [ABS_GAS] = "Accelerator",
        [ABS_BRAKE] = "Brake",
    };
    if (code < XD_ARRAY_LEN(names) && names[code] != nullptr)
        return names[code];
    /* The rest a controller may have: sliders and dials. */
    if (code == ABS_MISC)
        return "Slider";
    if (code == ABS_PRESSURE || code == ABS_DISTANCE)
        return "Dial";
    return nullptr;
}

bool xd_joy_axis_is_hat(unsigned code) { return code >= ABS_HAT0X && code <= ABS_HAT3Y; }

int xd_joy_hat_direction(int x, int y) {
    int sx = (x > 0) - (x < 0), sy = (y > 0) - (y < 0);
    /* By [sy + 1][sx + 1]; up is negative y. */
    static const int dirs[3][3] = {
        {7, 0, 1},
        {6, -1, 2},
        {5, 4, 3},
    };
    return dirs[sy + 1][sx + 1];
}

/* joydev's order: BTN_JOYSTICK..KEY_MAX, then BTN_MISC..BTN_JOYSTICK. */
static unsigned rank(unsigned code) {
    return code >= BTN_JOYSTICK ? code - BTN_JOYSTICK : code + KEY_MAX;
}

static int by_rank(const void *a, const void *b) {
    unsigned x = rank(*(const unsigned *)a), y = rank(*(const unsigned *)b);
    return x < y ? -1 : x > y ? 1 : 0;
}

size_t xd_joy_button_order(const unsigned *codes, size_t n, unsigned *out) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (codes[i] >= BTN_MISC && codes[i] <= KEY_MAX)
            out[k++] = codes[i];
    qsort(out, k, sizeof(*out), by_rank);
    return k;
}
