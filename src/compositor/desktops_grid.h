/* SPDX-License-Identifier: MIT
 * Virtual desktops' grid: how many there are across and down (General
 * Options' Desktop Size), which one lies which way from another, and
 * which windows a desktop shows. Pure functions, apart from wlroots, so
 * the unit tests can check them; desktops.c is the compositor's side.
 *
 * Desktops are numbered from 0, row by row from the top left, as Compiz
 * numbered its viewports.
 */
#ifndef XD_COMPOSITOR_DESKTOPS_GRID_H
#define XD_COMPOSITOR_DESKTOPS_GRID_H

#include <stdint.h>

enum { XD_DESKTOPS_MAX_SIDE = 16 };

typedef struct xd_desktop_grid {
    int columns, rows; /* 1..XD_DESKTOPS_MAX_SIDE */
    bool wrap;         /* past the last desktop comes the first (Wall's "Allow wrap-around") */
} xd_desktop_grid;

/* The grid made valid: each side clamped to 1..XD_DESKTOPS_MAX_SIDE. */
xd_desktop_grid xd_desktop_grid_make(int columns, int rows, bool wrap);
int xd_desktop_grid_get_count(const xd_desktop_grid *grid);
/* Its column and row, and back; an index past the grid is clamped. */
void xd_desktop_grid_get_cell(const xd_desktop_grid *grid, int desktop, int *column, int *row);
int xd_desktop_grid_get_index(const xd_desktop_grid *grid, int column, int row);
/* The desktop `columns` across and `rows` down from `from` (negative:
 * left or up), round the edges when the grid wraps (`force_wrap` too, a
 * cube's faces going round), else -1 when that's past one. */
int xd_desktop_grid_find_neighbour(const xd_desktop_grid *grid, int from, int columns, int rows,
                                   bool force_wrap);
/* Which way `to` lies from `from`, the shorter way round when the grid
 * wraps (or `force_wrap`): whole desktops across and down. */
void xd_desktop_grid_get_direction(const xd_desktop_grid *grid, int from, int to,
                                   bool force_wrap, int *columns, int *rows);
/* Whether a window on `desktop` shows while `current` does: a sticky
 * one ("Always on Visible Workspace") on every desktop. */
bool xd_desktop_grid_is_shown(int desktop, bool sticky, int current);

#endif
