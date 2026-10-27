/* SPDX-License-Identifier: MIT
 * Water's maths: a height field over the desktop, coarser than its
 * pixels, that waves spread across (the discrete wave equation, damped),
 * with drops and lines dropped into it. Pure, no wlroots or GL, so it's
 * unit-tested; water.c turns its slopes into the screen's distortion.
 */
#ifndef XD_COMPOSITOR_EFFECTS_WATER_MODEL_H
#define XD_COMPOSITOR_EFFECTS_WATER_MODEL_H

#include <stddef.h>
#include <stdint.h>

typedef struct xd_water {
    int columns, rows;
    float *heights;  /* now */
    float *previous; /* a step ago */
    double damping;  /* what's left of a wave each step */
} xd_water;

/* Flat; false for fewer than 3 x 3 cells or too many. */
bool xd_water_init(xd_water *water, int columns, int rows);
void xd_water_free(xd_water *water);
/* A drop at (column, row): a dent `depth` deep, `radius` cells across,
 * round (a half cosine). */
void xd_water_add_drop(xd_water *water, double column, double row, double radius, double depth);
/* Drops along a line, a cell apart. */
void xd_water_add_line(xd_water *water, double from_column, double from_row, double to_column,
                       double to_row, double radius, double depth);
/* One step of the waves: false once the water is still (and then
 * exactly flat). */
bool xd_water_step(xd_water *water);
static inline float xd_water_get_height(const xd_water *water, int column, int row) {
    return water->heights[(size_t)row * (size_t)water->columns + (size_t)column];
}
/* The surface's slope at a cell, across and down (0 at the edges). */
void xd_water_get_slope(const xd_water *water, int column, int row, double *across,
                        double *down);

#endif
