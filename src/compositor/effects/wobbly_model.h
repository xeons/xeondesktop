/* SPDX-License-Identifier: MIT
 * Wobbly Windows' spring model, as Compiz's wobbly plugin has it: a 4 x 4
 * grid of masses joined to their neighbours by springs that want them as
 * far apart as on the window's rectangle. The grabbed mass is held where
 * the pointer has the window; the others follow it late and overshoot.
 * Let go, every mass is also drawn to its place on the window, so the
 * sheet settles where the window is (Compiz moved the window to where
 * the sheet came to rest instead). The window is drawn as the bicubic
 * Bézier patch these masses are the control points of: at rest that is
 * exactly the rectangle.
 *
 * Time goes in Compiz's steps of 15 ms. No GL and no display: unit-tested.
 */
#ifndef XD_COMPOSITOR_EFFECTS_WOBBLY_MODEL_H
#define XD_COMPOSITOR_EFFECTS_WOBBLY_MODEL_H

#include <stdint.h>

enum { XD_WOBBLY_SIDE = 4, XD_WOBBLY_POINTS = XD_WOBBLY_SIDE * XD_WOBBLY_SIDE };

typedef struct xd_wobbly_point {
    double x, y;   /* layout units */
    double vx, vy; /* per step */
} xd_wobbly_point;

typedef struct xd_wobbly_model {
    xd_wobbly_point points[XD_WOBBLY_POINTS]; /* row by row, from the top left */
    /* The rectangle it settles on: the window as it is now. */
    double rest_x, rest_y, rest_width, rest_height;
    double spring;   /* Compiz's spring constant, 0.1..10 */
    double friction; /* 0.1..10 */
    int grabbed;     /* the mass held, or -1 */
    double leftover; /* time not yet stepped, in seconds */
    uint32_t seed;   /* for shivers */
} xd_wobbly_model;

/* At rest on the rectangle. */
void xd_wobbly_init(xd_wobbly_model *model, double x, double y, double width, double height,
                    double spring, double friction);
/* The window moved or changed size: where the sheet settles. */
void xd_wobbly_set_rest(xd_wobbly_model *model, double x, double y, double width,
                        double height);
/* Holds the mass nearest (x, y) at its place on the window; released,
 * the sheet springs back to the window. */
void xd_wobbly_grab(xd_wobbly_model *model, double x, double y);
void xd_wobbly_release(xd_wobbly_model *model);
/* Each mass gets a push of up to `strength` layout units a step in a
 * random direction (Compiz's "shiver"). */
void xd_wobbly_shiver(xd_wobbly_model *model, double strength);
/* Moves each mass `share` of the way back to where it would be on the
 * rectangle `from`: the window jumped from there (maximized, snapped)
 * and wobbles into its new place. */
void xd_wobbly_displace(xd_wobbly_model *model, double from_x, double from_y, double from_width,
                        double from_height, double share);
/* Advances by `seconds`; false once it has come to rest (and snapped to
 * the rectangle), which a held sheet never does. */
bool xd_wobbly_step(xd_wobbly_model *model, double seconds);
/* Where the window's point (u, v), each 0..1 across it, is drawn. */
void xd_wobbly_evaluate(const xd_wobbly_model *model, double u, double v, double *x, double *y);
/* The rest position of mass `index`. */
void xd_wobbly_get_rest_point(const xd_wobbly_model *model, int index, double *x, double *y);

#endif
