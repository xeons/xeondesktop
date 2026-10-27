/* SPDX-License-Identifier: MIT
 * Virtual desktops' grid (desktops_grid.h).
 */
#include "desktops_grid.h"

static int grid_clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

/* `value` brought into 0..count-1, round the ends. */
static int grid_wrap(int value, int count) {
    int rest = value % count;
    return rest < 0 ? rest + count : rest;
}

xd_desktop_grid xd_desktop_grid_make(int columns, int rows, bool wrap) {
    return (xd_desktop_grid){
        .columns = grid_clamp(columns, 1, XD_DESKTOPS_MAX_SIDE),
        .rows = grid_clamp(rows, 1, XD_DESKTOPS_MAX_SIDE),
        .wrap = wrap,
    };
}

int xd_desktop_grid_get_count(const xd_desktop_grid *grid) {
    return grid->columns * grid->rows;
}

void xd_desktop_grid_get_cell(const xd_desktop_grid *grid, int desktop, int *column, int *row) {
    desktop = grid_clamp(desktop, 0, xd_desktop_grid_get_count(grid) - 1);
    *column = desktop % grid->columns;
    *row = desktop / grid->columns;
}

int xd_desktop_grid_get_index(const xd_desktop_grid *grid, int column, int row) {
    return grid_clamp(row, 0, grid->rows - 1) * grid->columns +
           grid_clamp(column, 0, grid->columns - 1);
}

int xd_desktop_grid_find_neighbour(const xd_desktop_grid *grid, int from, int columns, int rows,
                                   bool force_wrap) {
    int column = 0, row = 0;
    xd_desktop_grid_get_cell(grid, from, &column, &row);
    column += columns;
    row += rows;
    if (grid->wrap || force_wrap) {
        column = grid_wrap(column, grid->columns);
        row = grid_wrap(row, grid->rows);
    } else if (column < 0 || column >= grid->columns || row < 0 || row >= grid->rows) {
        return -1;
    }
    return xd_desktop_grid_get_index(grid, column, row);
}

/* The shorter of the two ways round a ring of `count`, from 0 to `step`
 * (a tie goes forwards). */
static int grid_get_shorter(int step, int count) {
    step = grid_wrap(step, count);
    return step * 2 > count ? step - count : step;
}

void xd_desktop_grid_get_direction(const xd_desktop_grid *grid, int from, int to,
                                   bool force_wrap, int *columns, int *rows) {
    int from_column = 0, from_row = 0, to_column = 0, to_row = 0;
    xd_desktop_grid_get_cell(grid, from, &from_column, &from_row);
    xd_desktop_grid_get_cell(grid, to, &to_column, &to_row);
    *columns = to_column - from_column;
    *rows = to_row - from_row;
    if (grid->wrap || force_wrap) {
        *columns = grid_get_shorter(*columns, grid->columns);
        *rows = grid_get_shorter(*rows, grid->rows);
    }
}

bool xd_desktop_grid_is_shown(int desktop, bool sticky, int current) {
    return sticky || desktop == current;
}
