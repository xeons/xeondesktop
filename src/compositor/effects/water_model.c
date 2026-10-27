/* SPDX-License-Identifier: MIT
 * Water's maths (water_model.h).
 */
#include "water_model.h"

#include <math.h>
#include <string.h>

#include "../../core/alloc.h"

static constexpr double WATER_PI = 3.14159265358979323846;
static constexpr double DAMPING = 0.98;
/* Waves lower than this are none. */
static constexpr float STILL = 0.002f;
static constexpr int MAX_CELLS = 1 << 20;

bool xd_water_init(xd_water *water, int columns, int rows) {
    *water = (xd_water){0};
    if (columns < 3 || rows < 3 || (long)columns * rows > MAX_CELLS)
        return false;
    size_t cells = (size_t)columns * (size_t)rows;
    water->columns = columns;
    water->rows = rows;
    water->heights = XD_NEW_ARRAY(float, cells);
    water->previous = XD_NEW_ARRAY(float, cells);
    water->damping = DAMPING;
    return true;
}

void xd_water_free(xd_water *water) {
    xd_free(water->heights);
    xd_free(water->previous);
    *water = (xd_water){0};
}

/* A whole number within low..high (NaN is low). */
static int water_clamp(double value, int low, int high) {
    if (!(value >= low))
        return low;
    if (value > high)
        return high;
    return (int)value;
}

void xd_water_add_drop(xd_water *water, double column, double row, double radius, double depth) {
    if (water->heights == nullptr || radius <= 0)
        return;
    /* The edges stay flat. */
    int first_column = water_clamp(floor(column - radius), 1, water->columns - 2);
    int last_column = water_clamp(ceil(column + radius), 1, water->columns - 2);
    int first_row = water_clamp(floor(row - radius), 1, water->rows - 2);
    int last_row = water_clamp(ceil(row + radius), 1, water->rows - 2);
    for (int cell_row = first_row; cell_row <= last_row; cell_row++) {
        for (int cell_column = first_column; cell_column <= last_column; cell_column++) {
            double distance = hypot(cell_column - column, cell_row - row);
            if (distance >= radius)
                continue;
            size_t cell = (size_t)cell_row * (size_t)water->columns + (size_t)cell_column;
            /* Both steps: the dent stays put for a step, then springs
             * back, sending its ring out. */
            float dent = (float)(depth * (cos(WATER_PI * distance / radius) + 1) / 2);
            water->heights[cell] -= dent;
            water->previous[cell] -= dent;
        }
    }
}

void xd_water_add_line(xd_water *water, double from_column, double from_row, double to_column,
                       double to_row, double radius, double depth) {
    double length = hypot(to_column - from_column, to_row - from_row);
    int drops = water_clamp(ceil(length), 0, 4096);
    for (int drop = 0; drop <= drops; drop++) {
        double share = drops > 0 ? (double)drop / drops : 0;
        xd_water_add_drop(water, from_column + (to_column - from_column) * share,
                          from_row + (to_row - from_row) * share, radius, depth);
    }
}

bool xd_water_step(xd_water *water) {
    if (water->heights == nullptr)
        return false;
    int columns = water->columns, rows = water->rows;
    const float *now = water->heights;
    float *next = water->previous; /* overwritten with the step after */
    float damping = (float)water->damping;
    float highest = 0;
    for (int row = 1; row < rows - 1; row++) {
        for (int column = 1; column < columns - 1; column++) {
            size_t cell = (size_t)row * (size_t)columns + (size_t)column;
            float around = now[cell - 1] + now[cell + 1] + now[cell - (size_t)columns] +
                           now[cell + (size_t)columns];
            float height = (around * 0.5f - next[cell]) * damping;
            next[cell] = height;
            highest = fmaxf(highest, fabsf(height));
        }
    }
    water->previous = water->heights;
    water->heights = next;
    if (highest >= STILL)
        return true;
    size_t bytes = (size_t)columns * (size_t)rows * sizeof(float);
    memset(water->heights, 0, bytes);
    memset(water->previous, 0, bytes);
    return false;
}

void xd_water_get_slope(const xd_water *water, int column, int row, double *across,
                        double *down) {
    *across = 0;
    *down = 0;
    if (column <= 0 || row <= 0 || column >= water->columns - 1 || row >= water->rows - 1)
        return;
    *across = (double)(xd_water_get_height(water, column + 1, row) -
                       xd_water_get_height(water, column - 1, row)) / 2;
    *down = (double)(xd_water_get_height(water, column, row + 1) -
                     xd_water_get_height(water, column, row - 1)) / 2;
}
