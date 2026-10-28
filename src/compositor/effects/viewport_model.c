/* SPDX-License-Identifier: MIT
 * The virtual desktops' effects' maths (viewport_model.h).
 */
#include "viewport_model.h"

#include <math.h>

static double model_clamp(double value, double low, double high) {
    return value < low ? low : value > high ? high : value;
}

static double model_lerp(double from, double to, double progress) {
    return from + (to - from) * progress;
}

/* ---- Desktop Wall ----------------------------------------------------------------- */

double xd_wall_ease(double time) {
    time = model_clamp(time, 0.0, 1.0);
    return time * time * (3.0 - 2.0 * time);
}

void xd_wall_get_offset(int columns, int rows, double progress, bool incoming, double width,
                        double height, double *offset_x, double *offset_y) {
    /* The view moves right: the old desktop goes off to the left, and the
     * new one comes in from the right. */
    double share = incoming ? 1.0 - progress : -progress;
    *offset_x = columns * width * share;
    *offset_y = rows * height * share;
}

/* ---- projection ------------------------------------------------------------------- */

/* Half of Compiz's 60 degree field of view. */
static constexpr double HALF_FIELD = M_PI / 6.0;

void xd_camera_init(xd_camera *camera, double width, double height) {
    camera->width = width > 0 ? width : 1;
    camera->height = height > 0 ? height : 1;
    camera->distance = camera->height / 2.0 / tan(HALF_FIELD);
}

void xd_camera_project(const xd_camera *camera, xd_vec3 point, float clip[4]) {
    double across = point.x - camera->width / 2.0;
    double down = point.y - camera->height / 2.0;
    double depth = camera->distance - point.z; /* in front of the camera */
    clip[0] = (float)(across * camera->distance / (camera->width / 2.0));
    clip[1] = (float)(down * camera->distance / (camera->height / 2.0));
    clip[2] = 0.0f;
    clip[3] = (float)depth;
}

bool xd_camera_project_to_screen(const xd_camera *camera, xd_vec3 point, double *screen_x,
                                 double *screen_y) {
    double depth = camera->distance - point.z;
    if (depth <= 1e-6)
        return false;
    double factor = camera->distance / depth;
    *screen_x = camera->width / 2.0 + (point.x - camera->width / 2.0) * factor;
    *screen_y = camera->height / 2.0 + (point.y - camera->height / 2.0) * factor;
    return true;
}

/* ---- Desktop Cube ----------------------------------------------------------------- */

double xd_cube_get_apothem(double width, int faces) {
    if (faces <= 2)
        return width / 2.0;
    return width / 2.0 / tan(M_PI / faces);
}

/* A point on the cube (its axis at the origin, y down, z towards the
 * viewer) turned and placed in front of the screen. */
static xd_vec3 cube_place(const xd_cube_pose *pose, xd_vec3 local) {
    double cos_angle = cos(pose->angle), sin_angle = sin(pose->angle);
    double turned_x = local.x * cos_angle - local.z * sin_angle;
    double turned_z = local.x * sin_angle + local.z * cos_angle;
    double cos_pitch = cos(pose->pitch), sin_pitch = sin(pose->pitch);
    double pitched_y = local.y * cos_pitch + turned_z * sin_pitch;
    double pitched_z = -local.y * sin_pitch + turned_z * cos_pitch;
    double apothem = xd_cube_get_apothem(pose->width, pose->faces);
    return (xd_vec3){
        pose->width / 2.0 + turned_x,
        pose->height / 2.0 + pitched_y,
        pitched_z - apothem - pose->zoom * pose->height,
    };
}

/* Face `face`'s corners before the cube is turned. */
static void cube_get_local_face(const xd_cube_pose *pose, int face, xd_vec3 corners[4]) {
    int faces = pose->faces < 2 ? 2 : pose->faces;
    double step = 2.0 * M_PI / faces;
    double apothem = xd_cube_get_apothem(pose->width, faces);
    /* Outward and, seen from outside, rightward. */
    double normal_x = sin(face * step), normal_z = cos(face * step);
    double right_x = cos(face * step), right_z = -sin(face * step);
    double half_width = pose->width / 2.0, half_height = pose->height / 2.0;
    double centre_x = normal_x * apothem, centre_z = normal_z * apothem;
    corners[0] = (xd_vec3){centre_x - right_x * half_width, -half_height,
                           centre_z - right_z * half_width};
    corners[1] = (xd_vec3){centre_x + right_x * half_width, -half_height,
                           centre_z + right_z * half_width};
    corners[2] = (xd_vec3){corners[1].x, half_height, corners[1].z};
    corners[3] = (xd_vec3){corners[0].x, half_height, corners[0].z};
}

void xd_cube_get_face(const xd_cube_pose *pose, int face, xd_vec3 corners[4]) {
    xd_vec3 local[4];
    cube_get_local_face(pose, face, local);
    for (int i = 0; i < 4; i++)
        corners[i] = cube_place(pose, local[i]);
}

size_t xd_cube_get_cap(const xd_cube_pose *pose, bool top, xd_vec3 *corners, size_t max) {
    int faces = pose->faces < 2 ? 2 : pose->faces;
    /* Each face's top (or bottom) left corner, going round; two faces
     * also need their right ones, for the square between them. */
    xd_vec3 ring[2 * XD_CUBE_MAX_FACES];
    size_t count = 0;
    for (int face = 0; face < faces && face < XD_CUBE_MAX_FACES; face++) {
        xd_vec3 local[4];
        cube_get_local_face(pose, face, local);
        ring[count++] = top ? local[0] : local[3];
        if (faces == 2)
            ring[count++] = top ? local[1] : local[2];
    }
    /* Seen from outside, clockwise on the screen: the top's ring goes the
     * other way round. */
    size_t written = 0;
    for (size_t i = 0; i < count && written < max; i++) {
        xd_vec3 local = top ? ring[count - 1 - i] : ring[i];
        corners[written++] = cube_place(pose, local);
    }
    return written;
}

bool xd_cube_is_facing(const xd_camera *camera, const xd_vec3 *corners, size_t count) {
    double area = 0;
    double first_x = 0, first_y = 0, last_x = 0, last_y = 0;
    for (size_t i = 0; i < count; i++) {
        double screen_x = 0, screen_y = 0;
        if (!xd_camera_project_to_screen(camera, corners[i], &screen_x, &screen_y))
            return false;
        if (i == 0) {
            first_x = screen_x;
            first_y = screen_y;
        } else {
            area += last_x * screen_y - screen_x * last_y;
        }
        last_x = screen_x;
        last_y = screen_y;
    }
    area += last_x * first_y - first_x * last_y;
    return area > 1e-6;
}

double xd_cube_get_face_angle(int faces, int face, double from) {
    faces = faces < 2 ? 2 : faces;
    double step = 2.0 * M_PI / faces;
    double angle = face * step;
    double turns = round((from - angle) / (2.0 * M_PI));
    return angle + turns * 2.0 * M_PI;
}

int xd_cube_get_front_face(int faces, double angle) {
    faces = faces < 2 ? 2 : faces;
    double step = 2.0 * M_PI / faces;
    long face = lround(angle / step) % faces;
    return (int)(face < 0 ? face + faces : face);
}

/* ---- Expo ------------------------------------------------------------------------- */

/* Desktop (column, row)'s place with the whole wall in view. */
static void expo_get_wall_cell(const xd_expo_layout *layout, int column, int row, double *x,
                               double *y, double *width, double *height) {
    double aspect = layout->width / (layout->height > 0 ? layout->height : 1);
    double room_width = layout->width - layout->spacing * (layout->columns + 1);
    double room_height = layout->height - layout->spacing * (layout->rows + 1);
    double cell_width = room_width / layout->columns;
    cell_width = fmin(cell_width, room_height / layout->rows * aspect);
    cell_width = fmax(cell_width, 1.0);
    double cell_height = cell_width / aspect;
    double wall_width = cell_width * layout->columns + layout->spacing * (layout->columns - 1);
    double wall_height = cell_height * layout->rows + layout->spacing * (layout->rows - 1);
    double left = (layout->width - wall_width) / 2.0;
    double top = (layout->height - wall_height) / 2.0;
    *x = left + column * (cell_width + layout->spacing);
    *y = top + row * (cell_height + layout->spacing);
    *width = cell_width;
    *height = cell_height;
}

void xd_expo_get_cell(const xd_expo_layout *layout, int column, int row, double progress,
                      double *x, double *y, double *width, double *height) {
    double cell_x = 0, cell_y = 0, cell_width = 0, cell_height = 0;
    expo_get_wall_cell(layout, column, row, &cell_x, &cell_y, &cell_width, &cell_height);
    /* Zoomed in, the wall is scaled about so the current desktop fills
     * the screen. */
    double current_x = 0, current_y = 0, current_width = 0, current_height = 0;
    expo_get_wall_cell(layout, layout->current_column, layout->current_row, &current_x,
                       &current_y, &current_width, &current_height);
    double zoom = layout->width / current_width;
    double zoomed_x = (cell_x - current_x) * zoom, zoomed_y = (cell_y - current_y) * zoom;
    progress = model_clamp(progress, 0.0, 1.0);
    *x = model_lerp(zoomed_x, cell_x, progress);
    *y = model_lerp(zoomed_y, cell_y, progress);
    *width = model_lerp(cell_width * zoom, cell_width, progress);
    *height = model_lerp(cell_height * zoom, cell_height, progress);
}

/* Tilt: how far the wall leans back, zoomed out (Compiz's 10 degrees, as
 * well as can be remembered). */
static constexpr double TILT_ANGLE = 10.0 * M_PI / 180.0;
/* Curve at full strength: the wall's edges this far round from its
 * middle (a quarter of a cylinder across); Compiz's default half of it
 * bends it gently. */
static constexpr double CURVE_MAX_ANGLE = M_PI / 4.0;
/* Curve's strips: each this much of the arc, at most. */
static constexpr double STRIP_ANGLE = M_PI / 64.0;
static constexpr double MAX_STRIPS = 64.0;

/* The deformed wall at a progress: Tilt turns it `angle` about the
 * horizontal line through its middle; Curve wraps it round a vertical
 * axis `radius` in front of it, its edges `angle` round. Either is then
 * pushed back so its nearest points are on the screen. */
typedef struct expo_surface {
    xd_expo_deformation deformation;
    double centre_x, centre_y;
    double angle, radius, push;
} expo_surface;

static expo_surface expo_get_surface(const xd_expo_layout *layout, double progress) {
    progress = model_clamp(progress, 0.0, 1.0);
    double left = 0, top = 0, right = 0, bottom = 0, width = 0, height = 0;
    xd_expo_get_cell(layout, 0, 0, progress, &left, &top, &width, &height);
    xd_expo_get_cell(layout, layout->columns - 1, layout->rows - 1, progress, &right, &bottom,
                     &width, &height);
    right += width;
    bottom += height;
    expo_surface surface = {
        .deformation = layout->deformation,
        .centre_x = (left + right) / 2.0,
        .centre_y = (top + bottom) / 2.0,
    };
    switch (layout->deformation) {
    case XD_EXPO_TILT:
        surface.angle = TILT_ANGLE * progress;
        surface.push = (bottom - surface.centre_y) * sin(surface.angle);
        break;
    case XD_EXPO_CURVE:
        surface.angle = CURVE_MAX_ANGLE * model_clamp(layout->curve, 0.0, 1.0) * progress;
        if (surface.angle < 1e-6 || right <= left) {
            surface.deformation = XD_EXPO_FLAT;
            break;
        }
        surface.radius = (right - left) / 2.0 / surface.angle;
        surface.push = surface.radius * (1.0 - cos(surface.angle));
        break;
    case XD_EXPO_FLAT:
        break;
    }
    return surface;
}

xd_vec3 xd_expo_get_point(const xd_expo_layout *layout, double progress, double x, double y) {
    expo_surface surface = expo_get_surface(layout, progress);
    switch (surface.deformation) {
    case XD_EXPO_TILT: {
        double from_middle = y - surface.centre_y; /* above it: negative, so farther */
        return (xd_vec3){x, surface.centre_y + from_middle * cos(surface.angle),
                         from_middle * sin(surface.angle) - surface.push};
    }
    case XD_EXPO_CURVE: {
        /* As far round the arc as along the flat wall. */
        double angle = (x - surface.centre_x) / surface.radius;
        return (xd_vec3){surface.centre_x + surface.radius * sin(angle), y,
                         surface.radius * (1.0 - cos(angle)) - surface.push};
    }
    case XD_EXPO_FLAT:
        break;
    }
    return (xd_vec3){x, y, 0};
}

bool xd_expo_find_point(const xd_expo_layout *layout, double progress, double screen_x,
                        double screen_y, double *x, double *y) {
    expo_surface surface = expo_get_surface(layout, progress);
    xd_camera camera;
    xd_camera_init(&camera, layout->width, layout->height);
    /* The ray from the camera through the screen point: 1 along it is on
     * the screen (z = 0). */
    xd_vec3 eye = {camera.width / 2.0, camera.height / 2.0, camera.distance};
    xd_vec3 ray = {screen_x - eye.x, screen_y - eye.y, -camera.distance};
    switch (surface.deformation) {
    case XD_EXPO_TILT: {
        double cos_angle = cos(surface.angle), sin_angle = sin(surface.angle);
        /* The plane through (any, centre_y, -push), along (0, cos, sin). */
        double normal_y = -sin_angle, normal_z = cos_angle;
        double facing = normal_y * ray.y + normal_z * ray.z;
        if (fabs(facing) < 1e-9)
            return false;
        double along =
            (normal_y * (surface.centre_y - eye.y) + normal_z * (-surface.push - eye.z)) / facing;
        if (along <= 0)
            return false;
        double hit_y = eye.y + along * ray.y, hit_z = eye.z + along * ray.z;
        double from_middle =
            (hit_y - surface.centre_y) * cos_angle + (hit_z + surface.push) * sin_angle;
        *x = eye.x + along * ray.x;
        *y = surface.centre_y + from_middle;
        return true;
    }
    case XD_EXPO_CURVE: {
        /* The cylinder's axis, in front of the wall's middle; seen across
         * the top, the ray meets it where this quadratic is 0. */
        double axis_z = surface.radius - surface.push;
        double offset_x = eye.x - surface.centre_x, offset_z = eye.z - axis_z;
        double squared = ray.x * ray.x + ray.z * ray.z;
        double linear = 2.0 * (offset_x * ray.x + offset_z * ray.z);
        double constant =
            offset_x * offset_x + offset_z * offset_z - surface.radius * surface.radius;
        double discriminant = linear * linear - 4.0 * squared * constant;
        if (squared < 1e-12 || discriminant < 0)
            return false;
        /* The far side, behind the axis: the wall, bent towards the camera. */
        double along = (-linear + sqrt(discriminant)) / (2.0 * squared);
        if (along <= 0)
            return false;
        double hit_x = eye.x + along * ray.x, hit_z = eye.z + along * ray.z;
        double angle = atan2(hit_x - surface.centre_x, axis_z - hit_z);
        *x = surface.centre_x + surface.radius * angle;
        *y = eye.y + along * ray.y;
        return true;
    }
    case XD_EXPO_FLAT:
        break;
    }
    *x = screen_x;
    *y = screen_y;
    return true;
}

int xd_expo_count_strips(const xd_expo_layout *layout, double progress, double width) {
    expo_surface surface = expo_get_surface(layout, progress);
    if (surface.deformation != XD_EXPO_CURVE)
        return 1;
    double strips = model_clamp(ceil(fabs(width) / surface.radius / STRIP_ANGLE), 1.0, MAX_STRIPS);
    return (int)strips;
}

int xd_expo_find_desktop(const xd_expo_layout *layout, double screen_x, double screen_y,
                         double *across, double *down) {
    double x = 0, y = 0;
    if (!xd_expo_find_point(layout, 1.0, screen_x, screen_y, &x, &y))
        return -1;
    for (int row = 0; row < layout->rows; row++) {
        for (int column = 0; column < layout->columns; column++) {
            double cell_x = 0, cell_y = 0, cell_width = 0, cell_height = 0;
            expo_get_wall_cell(layout, column, row, &cell_x, &cell_y, &cell_width,
                               &cell_height);
            if (x >= cell_x && x < cell_x + cell_width && y >= cell_y &&
                y < cell_y + cell_height) {
                *across = (x - cell_x) / cell_width;
                *down = (y - cell_y) / cell_height;
                return row * layout->columns + column;
            }
        }
    }
    return -1;
}

/* ---- Shift Switcher --------------------------------------------------------------- */

/* Cover: the side windows turned this far, at these fractions of the
 * width from the centre and back. */
static constexpr double COVER_TURN = 0.8;
static constexpr double COVER_FIRST = 0.45;
static constexpr double COVER_NEXT = 0.1;
static constexpr double COVER_DEPTH = 0.3;
/* Flip: each window behind the one before by this much of the width,
 * all turned alike. */
static constexpr double FLIP_DEPTH = 0.16;
static constexpr double FLIP_TURN = 0.5;
/* How many windows each side before they've faded out. */
static constexpr double SHOWN_EACH_SIDE = 5.0;

xd_shift_slot xd_shift_get_slot(xd_shift_mode mode, double position, double width,
                                double height) {
    double side = position < 0 ? -1.0 : 1.0;
    double distance = fabs(position);
    xd_shift_slot slot = {.alpha = model_clamp(SHOWN_EACH_SIDE + 1.0 - distance, 0.0, 1.0)};
    if (mode == XD_SHIFT_FLIP) {
        /* Those already flipped past come forward as they fade. */
        slot.centre = (xd_vec3){width / 2.0 + position * width * 0.05,
                                height / 2.0 - position * height * 0.04,
                                -position * width * FLIP_DEPTH};
        slot.turn = FLIP_TURN;
        if (position < 0)
            slot.alpha = model_clamp(1.0 + position, 0.0, 1.0);
        return slot;
    }
    double near = fmin(distance, 1.0), far = fmax(distance - 1.0, 0.0);
    slot.centre = (xd_vec3){width / 2.0 + side * (near * COVER_FIRST + far * COVER_NEXT) * width,
                            height / 2.0, -near * COVER_DEPTH * width};
    slot.turn = side * near * COVER_TURN;
    return slot;
}

void xd_shift_get_corners(const xd_shift_slot *slot, double width, double height,
                          xd_vec3 corners[4]) {
    double cos_turn = cos(slot->turn), sin_turn = sin(slot->turn);
    const double across[4] = {-width / 2.0, width / 2.0, width / 2.0, -width / 2.0};
    const double down[4] = {-height / 2.0, -height / 2.0, height / 2.0, height / 2.0};
    for (int i = 0; i < 4; i++) {
        /* Turned positive, its right edge goes away. */
        corners[i] = (xd_vec3){slot->centre.x + across[i] * cos_turn, slot->centre.y + down[i],
                               slot->centre.z - across[i] * sin_turn};
    }
}

double xd_shift_get_scale(double window_width, double window_height, double width, double height,
                          double size) {
    if (window_width <= 0 || window_height <= 0)
        return 1.0;
    double scale = fmin(width * size / window_width, height * size / window_height);
    return fmin(scale, 1.0);
}
