/* SPDX-License-Identifier: MIT
 * Unit tests for virtual desktops and their effects' maths (Desktop Wall,
 * Desktop Cube, Expo, Shift Switcher), included by test_main.c after its
 * CHECK macro.
 */

static void test_desktop_grid(void) {
    xd_desktop_grid grid = xd_desktop_grid_make(0, 99, false);
    CHECK(grid.columns == 1 && grid.rows == XD_DESKTOPS_MAX_SIDE);
    grid = xd_desktop_grid_make(3, 2, false);
    CHECK(xd_desktop_grid_get_count(&grid) == 6);
    int column = -1, row = -1;
    xd_desktop_grid_get_cell(&grid, 4, &column, &row);
    CHECK(column == 1 && row == 1);
    CHECK(xd_desktop_grid_get_index(&grid, 2, 1) == 5);
    CHECK(xd_desktop_grid_get_index(&grid, 9, 9) == 5); /* clamped */

    /* No wrap-around by default, as Compiz's Wall: none past the edges. */
    CHECK(xd_desktop_grid_find_neighbour(&grid, 0, 1, 0, false) == 1);
    CHECK(xd_desktop_grid_find_neighbour(&grid, 0, 0, 1, false) == 3);
    CHECK(xd_desktop_grid_find_neighbour(&grid, 0, -1, 0, false) == -1);
    CHECK(xd_desktop_grid_find_neighbour(&grid, 2, 1, 0, false) == -1);
    CHECK(xd_desktop_grid_find_neighbour(&grid, 3, 0, 1, false) == -1);
    /* A cube's faces go round even so; with wrap-around every way does. */
    CHECK(xd_desktop_grid_find_neighbour(&grid, 2, 1, 0, true) == 0);
    grid.wrap = true;
    CHECK(xd_desktop_grid_find_neighbour(&grid, 0, -1, 0, false) == 2);
    CHECK(xd_desktop_grid_find_neighbour(&grid, 1, 0, -1, false) == 4);
    CHECK(xd_desktop_grid_find_neighbour(&grid, 5, 1, 1, false) == 0);

    int columns = 0, rows = 0;
    xd_desktop_grid_get_direction(&grid, 0, 2, false, &columns, &rows);
    CHECK(columns == -1 && rows == 0); /* the short way round */
    grid.wrap = false;
    xd_desktop_grid_get_direction(&grid, 0, 5, false, &columns, &rows);
    CHECK(columns == 2 && rows == 1);

    /* One desktop: nothing anywhere. */
    xd_desktop_grid single = xd_desktop_grid_make(1, 1, true);
    CHECK(xd_desktop_grid_find_neighbour(&single, 0, 1, 0, false) == 0);

    /* Which windows show: their desktop's, and sticky ones everywhere. */
    CHECK(xd_desktop_grid_is_shown(1, false, 1));
    CHECK(!xd_desktop_grid_is_shown(0, false, 1));
    CHECK(xd_desktop_grid_is_shown(0, true, 1));
}

static void test_desktops(void) {
    test_desktop_grid();
}
