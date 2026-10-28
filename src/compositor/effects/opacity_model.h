/* SPDX-License-Identifier: MIT
 * Window Opacity's values (Compiz's Opacity, Brightness and Saturation),
 * apart from wlroots so the unit tests can check them: a window's
 * opacity, brightness and saturation from the rules it matches and the
 * wheel's steps, and what brightness and saturation do to a colour (as
 * glfx_color.c's shader does it).
 */
#ifndef XD_COMPOSITOR_EFFECTS_OPACITY_MODEL_H
#define XD_COMPOSITOR_EFFECTS_OPACITY_MODEL_H

#include <stddef.h>
#include <stdint.h>

/* What Window Opacity changes, as Compiz's paint attributes: 0..1 each,
 * 1 unchanged. */
typedef enum xd_opacity_property : uint8_t {
    XD_OPACITY_PROPERTY_OPACITY,
    XD_OPACITY_PROPERTY_BRIGHTNESS,
    XD_OPACITY_PROPERTY_SATURATION,
    XD_OPACITY_PROPERTY_COUNT,
} xd_opacity_property;

enum { XD_OPACITY_RULES = 3 };

typedef struct xd_opacity_rule {
    char windows[512]; /* XD_EFFECT_OPTION_MATCH's syntax; "" is no rule */
    float value;
} xd_opacity_rule;

/* The luminance weights of red, green and blue that saturation greys
 * towards, as Compiz's (and glfx_color.c's shader). */
#define XD_OPACITY_RED_WEIGHT 0.30f
#define XD_OPACITY_GREEN_WEIGHT 0.59f
#define XD_OPACITY_BLUE_WEIGHT 0.11f

/* The lowest a binding takes the property: opacity stops short of 0, so
 * a window can't vanish; a black or grey window is still there. */
float xd_opacity_get_lowest(xd_opacity_property property);
/* `value` changed by `step` (negative: lower) `steps` times, clamped to
 * the property's range. */
float xd_opacity_step_value(xd_opacity_property property, float value, float step, int steps);
/* The value of the first of `count` rules whose windows match the
 * window, or 1 when none does. */
float xd_opacity_choose_rule(const xd_opacity_rule *rules, size_t count, const char *app_id,
                             const char *title, bool is_dialog);
/* Whether brightness and saturation change a window's colours (an
 * OpenGL pass): either is below 1. */
bool xd_opacity_is_colored(float brightness, float saturation);
/* A premultiplied colour (rgb, 0..1) with saturation and brightness
 * applied: greyed towards its luminance by saturation (0 is grey), then
 * darkened by brightness (0 is black). Its alpha doesn't change. */
void xd_opacity_adjust_color(float rgb[3], float brightness, float saturation);

#endif
