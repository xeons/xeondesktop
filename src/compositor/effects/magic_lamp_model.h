/* SPDX-License-Identifier: MIT
 * Magic Lamp's shape: a window sucked into its taskbar button as a genie
 * goes into its lamp. Pure maths, no wlroots, so it's unit-tested.
 *
 * The window first bends: the end nearer the button narrows toward it
 * along a curve while the far end stays put; then it slides along that
 * curve into the button, squeezed to the button's size as it gets there.
 * Worked out with the button below the window and turned to suit a
 * taskbar on any edge.
 */
#ifndef XD_COMPOSITOR_EFFECTS_MAGIC_LAMP_MODEL_H
#define XD_COMPOSITOR_EFFECTS_MAGIC_LAMP_MODEL_H

#include <stdint.h>

typedef struct xd_lamp_rect {
    double x, y, width, height;
} xd_lamp_rect;

/* Where the button is from the window: where it goes. */
typedef enum xd_lamp_direction : uint8_t {
    XD_LAMP_DOWN,
    XD_LAMP_UP,
    XD_LAMP_LEFT,
    XD_LAMP_RIGHT,
} xd_lamp_direction;

typedef struct xd_lamp {
    xd_lamp_direction direction;
    /* Both turned so that the button is below the window. */
    xd_lamp_rect window, icon;
} xd_lamp;

/* The button's side of the window: the edge it's past, or the farthest
 * one when it's beside a corner or overlaps the window. */
xd_lamp_direction xd_lamp_get_direction(xd_lamp_rect window, xd_lamp_rect icon);
void xd_lamp_init(xd_lamp *lamp, xd_lamp_rect window, xd_lamp_rect icon);
/* The point (across, down: 0..1 over the window's picture, from its top
 * left) at `progress`: 0 is the window as it is, 1 all in the button. */
void xd_lamp_get_point(const xd_lamp *lamp, double across, double down, double progress,
                       double *x, double *y);

#endif
