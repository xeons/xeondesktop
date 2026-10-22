/* SPDX-License-Identifier: MIT
 * Windows' colour button, as Advanced Appearance's Color 1, Color 2 and
 * font Color: a push button showing its colour that drops down the 20
 * colours of the system palette and Other..., which opens the Color
 * dialog (ChooseColor).
 */
#ifndef XD_APPS_DESK_COLOR_H
#define XD_APPS_DESK_COLOR_H

#include "../ui/ui.h"

typedef struct color_button color_button;

/* Runs when the user picks a colour (from the list or the Color dialog). */
typedef void (*color_button_cb)(color_button *button, xd_rgb rgb, void *user);

color_button *color_button_create(xd_window *win, xd_rect rect, color_button_cb on_change,
                                  void *user);
void color_button_set(color_button *button, xd_rgb rgb);
xd_rgb color_button_value(const color_button *button);
/* Disabled, it shows no colour, as Windows' does. */
void color_button_set_enabled(color_button *button, bool enabled);
/* Closes its Color dialog, if one is open, and frees it; the widget itself
 * goes with its window. */
void color_button_free(color_button *button);

/* The Color dialog on its own: done runs once it closes, with ok false on
 * Cancel. */
typedef struct color_dialog color_dialog;
typedef void (*color_dialog_done)(bool ok, xd_rgb rgb, void *user);
color_dialog *color_dialog_open(xd_ui *ui, xd_rgb initial, color_dialog_done done, void *user);
/* Closes it as Cancel does (done runs). */
void color_dialog_cancel(color_dialog *color_window);

/* Windows' hue, luminosity and saturation (ColorRGBToHLS), each 0-240,
 * as the Color dialog shows them. */
void color_rgb_to_hls(xd_rgb color, int *hue, int *lum, int *sat);
xd_rgb color_rgb_from_hls(int hue, int lum, int sat);

#endif
