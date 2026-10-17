/* SPDX-License-Identifier: MIT
 * Game Controllers' Test tab, the arithmetic: evdev axis values scaled
 * for drawing, axis and hat names, and buttons numbered as the kernel's
 * joydev (and so SDL and games) number them.
 */
#ifndef XD_JOY_JOYINFO_H
#define XD_JOY_JOYINFO_H

#include <stddef.h>

/* `value` within [min, max] as -1 (min) to 1 (max), clamped; 0 when the
 * range is empty. */
double xd_joy_axis_centered(int value, int min, int max);
/* The same as 0 (min) to 1 (max), for bars (throttles, triggers). */
double xd_joy_axis_fraction(int value, int min, int max);

/* "X Axis", "Z Rotation", "Throttle", ...; nullptr for codes that aren't
 * a controller axis (hats included). */
const char *xd_joy_axis_name(unsigned code);
/* ABS_HAT0X ... ABS_HAT3Y. */
bool xd_joy_axis_is_hat(unsigned code);
/* A hat's position from its X and Y axes (negative is left and up):
 * -1 when centred, else 0 (up) to 7 clockwise in 45 degree steps. */
int xd_joy_hat_direction(int x, int y);

/* Orders the button codes `codes` (n of them, any order) into `out` as
 * joydev numbers them: BTN_JOYSTICK and up first, then BTN_MISC up to
 * BTN_JOYSTICK; codes below BTN_MISC (keyboard keys) are left out.
 * Returns how many were written. */
size_t xd_joy_button_order(const unsigned *codes, size_t n, unsigned *out);

#endif
