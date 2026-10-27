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

static bool near_value(double value, double expected, double tolerance) {
    return fabs(value - expected) <= tolerance;
}

static void test_wall_slide(void) {
    CHECK(xd_wall_ease(0.0) == 0.0 && xd_wall_ease(1.0) == 1.0);
    CHECK(near_value(xd_wall_ease(0.5), 0.5, 1e-9));
    CHECK(xd_wall_ease(0.1) < 0.1 && xd_wall_ease(0.9) > 0.9); /* slow at both ends */
    CHECK(xd_wall_ease(-1.0) == 0.0 && xd_wall_ease(2.0) == 1.0);
    double x = 0, y = 0;
    /* To the right: the old desktop goes left, the new one comes from the right. */
    xd_wall_get_offset(1, 0, 0.0, false, 1024, 768, &x, &y);
    CHECK(x == 0 && y == 0);
    xd_wall_get_offset(1, 0, 0.0, true, 1024, 768, &x, &y);
    CHECK(x == 1024 && y == 0);
    xd_wall_get_offset(1, 0, 0.5, false, 1024, 768, &x, &y);
    CHECK(x == -512);
    xd_wall_get_offset(1, 0, 0.5, true, 1024, 768, &x, &y);
    CHECK(x == 512);
    xd_wall_get_offset(1, 0, 1.0, true, 1024, 768, &x, &y);
    CHECK(x == 0);
    /* Up a row: the new desktop comes from above. */
    xd_wall_get_offset(0, -1, 0.25, true, 1024, 768, &x, &y);
    CHECK(x == 0 && y == -576);
}

static void test_cube_projection(void) {
    xd_camera camera;
    xd_camera_init(&camera, 1024, 768);
    /* The screen plane maps to the screen, the middle to the middle. */
    double x = 0, y = 0;
    CHECK(xd_camera_project_to_screen(&camera, (xd_vec3){0, 0, 0}, &x, &y));
    CHECK(near_value(x, 0, 1e-9) && near_value(y, 0, 1e-9));
    CHECK(xd_camera_project_to_screen(&camera, (xd_vec3){1024, 768, 0}, &x, &y));
    CHECK(near_value(x, 1024, 1e-9) && near_value(y, 768, 1e-9));
    /* Farther back is smaller, towards the middle. */
    CHECK(xd_camera_project_to_screen(&camera, (xd_vec3){0, 0, -500}, &x, &y));
    CHECK(x > 0 && x < 512 && y > 0 && y < 384);
    CHECK(!xd_camera_project_to_screen(&camera, (xd_vec3){0, 0, camera.distance + 1}, &x, &y));
    /* Clip space: x/w and y/w are -1..1 over the screen, top -1. */
    float clip[4];
    xd_camera_project(&camera, (xd_vec3){1024, 0, 0}, clip);
    CHECK(near_value((double)(clip[0] / clip[3]), 1.0, 1e-5));
    CHECK(near_value((double)(clip[1] / clip[3]), -1.0, 1e-5));

    /* Four desktops: face 0 on the screen unturned, face 1 once turned
     * a quarter, each exactly the screen. */
    xd_cube_pose pose = {.faces = 4, .width = 1024, .height = 768};
    const double screen[4][2] = {{0, 0}, {1024, 0}, {1024, 768}, {0, 768}};
    xd_vec3 corners[4];
    for (int face = 0; face < 2; face++) {
        pose.angle = face * M_PI / 2.0;
        xd_cube_get_face(&pose, face, corners);
        bool on_screen = true;
        for (int i = 0; i < 4; i++) {
            CHECK(xd_camera_project_to_screen(&camera, corners[i], &x, &y));
            on_screen = on_screen && near_value(x, screen[i][0], 1e-6) &&
                        near_value(y, screen[i][1], 1e-6);
        }
        CHECK(on_screen);
        CHECK(xd_cube_is_facing(&camera, corners, 4));
        /* The face opposite isn't. */
        xd_cube_get_face(&pose, face + 2, corners);
        CHECK(!xd_cube_is_facing(&camera, corners, 4));
    }
    CHECK(near_value(xd_cube_get_apothem(1024, 4), 512, 1e-9));
    CHECK(near_value(xd_cube_get_apothem(1024, 2), 512, 1e-9));
    CHECK(xd_cube_get_apothem(1024, 6) > 512);

    /* Half-way, both faces show, each narrower than the screen. */
    pose.angle = M_PI / 4.0;
    xd_cube_get_face(&pose, 0, corners);
    CHECK(xd_cube_is_facing(&camera, corners, 4));
    double left = 0, right = 0;
    CHECK(xd_camera_project_to_screen(&camera, corners[0], &left, &y));
    CHECK(xd_camera_project_to_screen(&camera, corners[1], &right, &y));
    CHECK(right - left < 1024 && right - left > 100);
    xd_cube_get_face(&pose, 1, corners);
    CHECK(xd_cube_is_facing(&camera, corners, 4));
    /* Turning right brings face 1 in from the right. */
    CHECK(xd_camera_project_to_screen(&camera, corners[1], &x, &y) && x > right);

    /* Two desktops: a cube with blank sides, face 1 at the back. */
    xd_cube_pose two = {.faces = 2, .width = 1024, .height = 768};
    xd_cube_get_face(&two, 1, corners);
    CHECK(!xd_cube_is_facing(&camera, corners, 4));
    two.angle = M_PI;
    xd_cube_get_face(&two, 1, corners);
    CHECK(xd_cube_is_facing(&camera, corners, 4));
    CHECK(xd_camera_project_to_screen(&camera, corners[0], &x, &y) && near_value(x, 0, 1e-6));

    /* Caps: the top shows when the top comes forward, the bottom not. */
    xd_vec3 cap[XD_CUBE_MAX_FACES * 2];
    pose.angle = 0;
    pose.pitch = 0.4;
    pose.zoom = 0.5;
    size_t count = xd_cube_get_cap(&pose, true, cap, XD_CUBE_MAX_FACES * 2);
    CHECK(count == 4 && xd_cube_is_facing(&camera, cap, count));
    count = xd_cube_get_cap(&pose, false, cap, XD_CUBE_MAX_FACES * 2);
    CHECK(count == 4 && !xd_cube_is_facing(&camera, cap, count));
    pose.pitch = -0.4;
    CHECK(xd_cube_is_facing(&camera, cap, xd_cube_get_cap(&pose, false, cap, 8)));
    CHECK(xd_cube_get_cap(&two, true, cap, 8) == 4);
    /* Zoomed out, face 0 is smaller than the screen. */
    pose.pitch = 0;
    xd_cube_get_face(&pose, 0, corners);
    CHECK(xd_camera_project_to_screen(&camera, corners[0], &x, &y) && x > 0 && y > 0);

    /* Turning to a face: the nearest way round, and back. */
    CHECK(near_value(xd_cube_get_face_angle(4, 1, 0), M_PI / 2.0, 1e-9));
    CHECK(near_value(xd_cube_get_face_angle(4, 3, 0), -M_PI / 2.0, 1e-9));
    CHECK(near_value(xd_cube_get_face_angle(4, 0, 2.0 * M_PI - 0.1), 2.0 * M_PI, 1e-9));
    CHECK(xd_cube_get_front_face(4, 0.2) == 0);
    CHECK(xd_cube_get_front_face(4, M_PI / 2.0 + 0.2) == 1);
    CHECK(xd_cube_get_front_face(4, -M_PI / 2.0) == 3);
    CHECK(xd_cube_get_front_face(2, M_PI) == 1);
}

static void test_expo_layout(void) {
    xd_expo_layout layout = {.columns = 2, .rows = 1, .current_column = 0, .width = 1024,
                             .height = 768, .spacing = 10};
    double x = 0, y = 0, width = 0, height = 0;
    /* Zoomed in, the current desktop fills the screen, the next beside it. */
    xd_expo_get_cell(&layout, 0, 0, 0.0, &x, &y, &width, &height);
    CHECK(near_value(x, 0, 1e-6) && near_value(y, 0, 1e-6) && near_value(width, 1024, 1e-6) &&
          near_value(height, 768, 1e-6));
    xd_expo_get_cell(&layout, 1, 0, 0.0, &x, &y, &width, &height);
    CHECK(x > 1024 && near_value(width, 1024, 1e-6));
    /* Zoomed out, side by side, centred, the screen's shape, with spacing. */
    double second_x = 0;
    xd_expo_get_cell(&layout, 1, 0, 1.0, &second_x, &y, &width, &height);
    xd_expo_get_cell(&layout, 0, 0, 1.0, &x, &y, &width, &height);
    CHECK(near_value(width, (1024 - 30) / 2.0, 1e-6));
    CHECK(near_value(width / height, 1024.0 / 768.0, 1e-6));
    CHECK(near_value(second_x - (x + width), 10, 1e-6));
    CHECK(near_value(x, 1024 - (second_x + width), 1e-6));
    CHECK(near_value(y + height / 2.0, 384, 1e-6));
    double across = 0, down = 0;
    CHECK(xd_expo_find_desktop(&layout, second_x + width / 2.0, 384, &across, &down) == 1);
    CHECK(near_value(across, 0.5, 1e-6) && near_value(down, 0.5, 1e-6));
    CHECK(xd_expo_find_desktop(&layout, x + width + 5, 384, &across, &down) == -1);
    CHECK(xd_expo_find_desktop(&layout, 512, 5, &across, &down) == -1);
    /* A 2 x 2 wall fits by its height. */
    xd_expo_layout square = {.columns = 2, .rows = 2, .current_row = 1, .width = 1024,
                             .height = 768, .spacing = 10};
    xd_expo_get_cell(&square, 0, 1, 1.0, &x, &y, &width, &height);
    CHECK(near_value(height, (768 - 30) / 2.0, 1e-6) && y + height < 768);
    xd_expo_get_cell(&square, 0, 1, 0.0, &x, &y, &width, &height);
    CHECK(near_value(y, 0, 1e-6) && near_value(height, 768, 1e-6));
}

static void test_shift_layout(void) {
    /* Cover: the selected window in the middle, facing; the next to the
     * right turned away and farther back; fading out past five. */
    xd_shift_slot middle = xd_shift_get_slot(XD_SHIFT_COVER, 0, 1024, 768);
    CHECK(middle.centre.x == 512 && middle.centre.y == 384 && middle.centre.z == 0 &&
          middle.turn == 0 && middle.alpha == 1);
    xd_shift_slot right = xd_shift_get_slot(XD_SHIFT_COVER, 1, 1024, 768);
    xd_shift_slot left = xd_shift_get_slot(XD_SHIFT_COVER, -1, 1024, 768);
    CHECK(right.centre.x > 512 && right.centre.z < 0 && right.turn > 0);
    CHECK(near_value(left.centre.x, 1024 - right.centre.x, 1e-9) && left.turn < 0);
    xd_shift_slot farther = xd_shift_get_slot(XD_SHIFT_COVER, 2, 1024, 768);
    CHECK(farther.centre.x > right.centre.x && farther.turn == right.turn);
    xd_shift_slot halfway = xd_shift_get_slot(XD_SHIFT_COVER, 0.5, 1024, 768);
    CHECK(halfway.centre.x > 512 && halfway.centre.x < right.centre.x);
    CHECK(xd_shift_get_slot(XD_SHIFT_COVER, 7, 1024, 768).alpha == 0);
    /* Its right edge turned away: farther back than its left. */
    xd_vec3 corners[4];
    xd_shift_get_corners(&right, 400, 300, corners);
    CHECK(corners[1].z < corners[0].z && corners[1].x > corners[0].x);
    xd_shift_get_corners(&middle, 400, 300, corners);
    CHECK(corners[0].x == 312 && corners[2].y == 534 && corners[0].z == 0);
    /* Flip: stacked going back; those passed fade. */
    xd_shift_slot behind = xd_shift_get_slot(XD_SHIFT_FLIP, 1, 1024, 768);
    CHECK(behind.centre.z < 0 && behind.alpha == 1);
    CHECK(xd_shift_get_slot(XD_SHIFT_FLIP, -0.5, 1024, 768).alpha == 0.5);
    /* Windows fit half the screen, never grown. */
    CHECK(near_value(xd_shift_get_scale(1024, 768, 1024, 768, 0.5), 0.5, 1e-9));
    CHECK(xd_shift_get_scale(100, 100, 1024, 768, 0.5) == 1.0);
}

static void test_desktops(void) {
    test_desktop_grid();
    test_wall_slide();
    test_cube_projection();
    test_expo_layout();
    test_shift_layout();
}
