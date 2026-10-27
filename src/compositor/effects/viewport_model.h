/* SPDX-License-Identifier: MIT
 * The maths of the virtual desktops' effects, apart from wlroots so the
 * unit tests can check them: Desktop Wall's slide (wall.c), Desktop
 * Cube's faces and their projection (cube.c, rotate.c), Expo's wall of
 * desktops (expo.c) and Shift Switcher's cover flow (shift.c).
 *
 * Positions on the screen are in "output units": 0..width across and
 * 0..height down from the output's top left. Clip space is GL's: -1..1
 * across, and -1 at the top (glfx.h's buffers have their first row at
 * the top).
 */
#ifndef XD_COMPOSITOR_EFFECTS_VIEWPORT_MODEL_H
#define XD_COMPOSITOR_EFFECTS_VIEWPORT_MODEL_H

#include <stddef.h>

/* ---- Desktop Wall ----------------------------------------------------------------- */

/* Eased progress of a slide, 0..1 for time 0..1: slow at both ends. */
double xd_wall_ease(double time);
/* How far a desktop's windows are moved `progress` (0..1) into a slide
 * `columns` across and `rows` down (whole desktops, the way the view
 * moves): the old desktop's (`incoming` false) go the other way, out of
 * sight at 1; the new one's (`incoming`) come from that side, in place
 * at 1. */
void xd_wall_get_offset(int columns, int rows, double progress, bool incoming, double width,
                        double height, double *offset_x, double *offset_y);

/* ---- projection ------------------------------------------------------------------- */

typedef struct xd_vec3 {
    double x, y, z;
} xd_vec3;

/* A camera looking down -z at the screen: Compiz's, with a 60 degree
 * vertical field of view, the output (width x height output units) filling
 * the view where z = 0 when the camera stands `distance` in front. */
typedef struct xd_camera {
    double width, height;
    double distance; /* xd_camera_init: height / 2 / tan(30 degrees) */
} xd_camera;

void xd_camera_init(xd_camera *camera, double width, double height);
/* A point (output units across and down, z towards the viewer) to clip
 * space: x, y, z and w for GL to divide by (perspective-correct texture
 * mapping needs the w). z is never used for depth. */
void xd_camera_project(const xd_camera *camera, xd_vec3 point, float clip[4]);
/* The same to output units on the screen; false behind the camera. */
bool xd_camera_project_to_screen(const xd_camera *camera, xd_vec3 point, double *screen_x,
                                 double *screen_y);

/* ---- Desktop Cube ----------------------------------------------------------------- */

enum { XD_CUBE_MAX_FACES = 16 }; /* General Options' most desktops across */

/* How far each face is from the cube's axis, for `faces` desktops across
 * `width` output units: a regular prism; two faces make a cube whose
 * other two sides are blank. */
double xd_cube_get_apothem(double width, int faces);
/* The cube turned `angle` radians about its vertical axis (positive: the
 * right-hand face comes to the front, as the view moving right),
 * `pitch` radians about the horizontal one (positive: the top comes to
 * the front), pushed `zoom` (0..) times the output's height away. Face
 * `face`'s corners (top left, top right, bottom right, bottom left), in
 * output units: face 0 lies on the screen at no angle, no pitch, no
 * zoom. */
typedef struct xd_cube_pose {
    int faces;
    double width, height;
    double angle, pitch, zoom;
} xd_cube_pose;

void xd_cube_get_face(const xd_cube_pose *pose, int face, xd_vec3 corners[4]);
/* The top (top true) or bottom cap: `faces` corners going round (a
 * square for two faces). Returns how many it wrote, up to max. */
size_t xd_cube_get_cap(const xd_cube_pose *pose, bool top, xd_vec3 *corners, size_t max);
/* Whether a polygon of these corners (in the order the face functions
 * give) faces the camera. */
bool xd_cube_is_facing(const xd_camera *camera, const xd_vec3 *corners, size_t count);
/* The angle that shows face `face` (and the nearest of the ways round to
 * `from`). */
double xd_cube_get_face_angle(int faces, int face, double from);
/* The face nearest the front at `angle`. */
int xd_cube_get_front_face(int faces, double angle);

/* ---- Expo ------------------------------------------------------------------------- */

/* Where desktop (column, row) shows in Expo's wall of `columns` x `rows`,
 * on an output `width` x `height`, `spacing` output units between them and
 * round the edge, at `progress` 0 (the current one filling the screen) to
 * 1 (the whole wall in view, centred). */
typedef struct xd_expo_layout {
    int columns, rows;
    int current_column, current_row;
    double width, height, spacing;
} xd_expo_layout;

void xd_expo_get_cell(const xd_expo_layout *layout, int column, int row, double progress,
                      double *x, double *y, double *width, double *height);
/* The desktop under (x, y) at full zoom-out, and where within it (0..1
 * across and down); -1 for none (the gaps). */
int xd_expo_find_desktop(const xd_expo_layout *layout, double x, double y, double *across,
                         double *down);

/* ---- Shift Switcher --------------------------------------------------------------- */

/* Window `index` of the cover flow, `position` its place from the selected
 * one (0; fractions while it moves): where its centre is (output units,
 * z towards the viewer), how much it's turned about its vertical axis
 * (radians, positive: its right edge away), and how big it is compared
 * with its own size, for the windows at most `size` (0..1) of the output.
 * Flip mode stacks them going back instead. */
typedef enum xd_shift_mode : unsigned char {
    XD_SHIFT_COVER,
    XD_SHIFT_FLIP,
} xd_shift_mode;

typedef struct xd_shift_slot {
    xd_vec3 centre;
    double turn;
    double alpha; /* 0..1: the farthest ones fade out */
} xd_shift_slot;

xd_shift_slot xd_shift_get_slot(xd_shift_mode mode, double position, double width,
                                double height);
/* The corners (top left, top right, bottom right, bottom left) of a
 * window `width` x `height` (output units, scaled already) in that slot. */
void xd_shift_get_corners(const xd_shift_slot *slot, double width, double height,
                          xd_vec3 corners[4]);
/* A window `window_width` x `window_height` scaled to fit `size` (0..1) of
 * the output: the factor. */
double xd_shift_get_scale(double window_width, double window_height, double width, double height,
                          double size);

#endif
