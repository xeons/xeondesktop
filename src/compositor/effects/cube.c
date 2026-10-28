/* SPDX-License-Identifier: MIT
 * Desktop Cube and Rotate Cube (xd-compositor-fx): the desktops of the
 * current row are the faces of a cube (a prism for more than four; two
 * are two sides of a cube whose others are blank). A switch across turns it from the old
 * face to the new (instead of Desktop Wall's slide); with Rotate Cube,
 * Ctrl+Alt and the left button drag it round, and tilt it to show its
 * caps, and let go it settles on the face nearest the front, whose desktop
 * then shows.
 *
 * While it turns, a whole-output effect (glfx.h) draws it: each face that
 * shows is its desktop captured into a texture (glfx_output.c's scene
 * capture: background, layer surfaces, that desktop's windows and the
 * sticky ones), drawn as a quad in perspective (viewport_model.h's
 * camera and cube maths, unit-tested), back faces culled, caps filled
 * with their colour. The two effects are one file: Rotate Cube only
 * drives Desktop Cube's cube.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <time.h>
#include <wlr/util/log.h>

#include "../../core/alloc.h"
#include "../desktops_grid.h"
#include "glfx.h"
#include "viewport_model.h"

/* Above Desktop Wall's: across, the cube turns instead of a slide. */
static constexpr int CUBE_PRIORITY = 20;
static constexpr int TICK_MS = 16;
/* Settling after a drag, at most this long. */
static constexpr double SETTLE_SECONDS = 0.35;
/* Turned or tilted this little, a press is a click: the cube isn't shown. */
static constexpr double DRAG_THRESHOLD = 3.0;

typedef enum cube_phase : uint8_t {
    CUBE_IDLE,
    CUBE_TURNING,  /* a switch: from one face's angle to the other's */
    CUBE_DRAGGING, /* Rotate Cube: following the pointer */
    CUBE_SETTLING, /* let go: to the nearest face, then that desktop */
} cube_phase;

typedef struct cube_options {
    double zoom; /* screen heights away, half-way through a turn */
    float cap_color[4];
    float background_color[4];
    bool desktop_background;
    double rotation_opacity; /* 0..1 */
    double rotation_seconds;
} cube_options;

typedef struct rotate_options {
    xd_effect_binding initiate;
    double sensitivity;
} rotate_options;

typedef struct cube_state {
    xd_server *server;
    cube_options options;
    rotate_options rotate; /* Rotate Cube's, while it's on */
    cube_phase phase;
    xd_output_effect *effect;
    struct wl_event_source *timer;
    int faces;    /* the cube's sides */
    int desktops; /* the first of them that are desktops, the rest blank */
    int row;      /* the row of desktops turning */
    /* The pose now, and the animation from `start_*` to `end_*`. */
    double angle, pitch, zoom;
    double start_angle, start_pitch, start_zoom, end_angle, start_time, duration;
    bool zoom_out; /* a switch: zoomed out half-way */
    int target_face; /* SETTLING: the desktop shown once settled */
    /* DRAGGING: where it began. */
    double drag_x, drag_y, drag_angle, drag_pitch;
    bool dragged;
    bool quiet_switch; /* our own switch: no animation for it */
    xd_glfx_scene_capture *captures[XD_CUBE_MAX_FACES];
    xd_glfx_texture textures[XD_CUBE_MAX_FACES];
    bool captured[XD_CUBE_MAX_FACES];
} cube_state;

/* The one running, for Rotate Cube. */
static cube_state *running_cube;

static double cube_get_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void cube_set_color(float out[4], uint32_t rgba) {
    out[0] = (float)((rgba >> 24) & 0xFF) / 255.0f;
    out[1] = (float)((rgba >> 16) & 0xFF) / 255.0f;
    out[2] = (float)((rgba >> 8) & 0xFF) / 255.0f;
    out[3] = (float)(rgba & 0xFF) / 255.0f;
}

static void cube_schedule_frames(xd_server *server) {
    XD_LIST_FOR_EACH(output, &server->outputs, xd_output, link) {
        if (output->wlr_output->enabled)
            wlr_output_schedule_frame(output->wlr_output);
    }
}

/* ---- drawing ---------------------------------------------------------------------- */

typedef struct face_filter {
    int desktop;
} face_filter;

static bool cube_shows_view(const xd_view *view, void *data) {
    const face_filter *filter = data;
    return !view->minimized && (view->sticky || view->desktop == filter->desktop);
}

static xd_cube_pose cube_get_pose(const cube_state *state, struct wlr_box box) {
    return (xd_cube_pose){
        .faces = state->faces,
        .width = box.width,
        .height = box.height,
        .angle = state->angle,
        .pitch = state->pitch,
        .zoom = state->zoom,
    };
}

/* The desktops of the faces that show on this output, captured. */
static void cube_capture_faces(cube_state *state, xd_output *output) {
    xd_server *server = state->server;
    struct wlr_box box = {0};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
    xd_camera camera;
    xd_camera_init(&camera, box.width, box.height);
    xd_cube_pose pose = cube_get_pose(state, box);
    const xd_desktop_grid *grid = xd_desktops_get_grid(server);
    for (int face = 0; face < state->desktops; face++) {
        xd_vec3 corners[4];
        xd_cube_get_face(&pose, face, corners);
        state->captured[face] = false;
        bool needed = xd_cube_is_facing(&camera, corners, 4) || state->options.rotation_opacity < 1;
        if (!needed)
            continue;
        if (state->captures[face] == nullptr)
            state->captures[face] = xd_glfx_create_scene_capture(server);
        face_filter filter = {xd_desktop_grid_get_index(grid, face, state->row)};
        double scale = (double)output->wlr_output->scale;
        state->captured[face] = xd_glfx_capture_scene(state->captures[face], box, scale, true,
                                                      cube_shows_view, &filter,
                                                      &state->textures[face]);
    }
}

static void cube_project(const xd_camera *camera, const xd_vec3 *corners, size_t count,
                         float (*clip)[4]) {
    for (size_t i = 0; i < count; i++)
        xd_camera_project(camera, corners[i], clip[i]);
}

static void cube_draw_cap(const xd_glfx_draw *draw, const xd_camera *camera,
                          const xd_cube_pose *pose, bool top, const float color[4]) {
    xd_vec3 corners[XD_CUBE_MAX_FACES * 2];
    size_t count = xd_cube_get_cap(pose, top, corners, XD_ARRAY_LEN(corners));
    if (count < 3 || !xd_cube_is_facing(camera, corners, count))
        return;
    float clip[XD_CUBE_MAX_FACES * 2][4];
    cube_project(camera, corners, count, clip);
    xd_glfx_fill_polygon(draw, clip, count, color);
}

static void cube_draw(xd_output_effect *effect, [[maybe_unused]] xd_output *output,
                      const xd_glfx_draw *draw) {
    cube_state *state = effect->data;
    struct wlr_box box = draw->bounds;
    xd_camera camera;
    xd_camera_init(&camera, box.width, box.height);
    xd_cube_pose pose = cube_get_pose(state, box);
    /* Behind it: the desktop's colour (the root window's, as Compiz
     * showed), or the colour chosen. */
    const float *background = state->options.background_color;
    float desktop[4] = {0, 0, 0, 1};
    if (state->options.desktop_background) {
        xd_rgb rgb = state->server->config.scheme.colors[XD_COLOR_BACKGROUND];
        desktop[0] = (float)rgb.r / 255.0f;
        desktop[1] = (float)rgb.g / 255.0f;
        desktop[2] = (float)rgb.b / 255.0f;
        background = desktop;
    }
    glClearColor(background[0] * background[3], background[1] * background[3],
                 background[2] * background[3], background[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    float alpha = state->phase == CUBE_IDLE ? 1.0f : (float)state->options.rotation_opacity;
    /* See-through, the faces behind first (seen from inside, mirrored, as
     * through glass); opaque, those facing alone. */
    for (int pass = alpha < 1.0f ? 0 : 1; pass < 2; pass++) {
        if (pass == 1) {
            cube_draw_cap(draw, &camera, &pose, true, state->options.cap_color);
            cube_draw_cap(draw, &camera, &pose, false, state->options.cap_color);
        }
        for (int face = 0; face < state->faces; face++) {
            xd_vec3 corners[4];
            xd_cube_get_face(&pose, face, corners);
            bool facing = xd_cube_is_facing(&camera, corners, 4);
            if (facing != (pass == 1))
                continue;
            float clip[4][4];
            cube_project(&camera, corners, 4, clip);
            /* The blank sides of two desktops' cube: the caps' colour. */
            if (face >= state->desktops)
                xd_glfx_fill_polygon(draw, clip, 4, state->options.cap_color);
            else if (state->captured[face])
                xd_glfx_draw_quad(draw, &state->textures[face], clip, alpha, 1.0f);
        }
    }
}

static const xd_output_effect_impl cube_output_impl = {
    .name = "Desktop Cube",
    .draw = cube_draw,
};

/* ---- turning ---------------------------------------------------------------------- */

static void cube_release_captures(cube_state *state) {
    for (int face = 0; face < XD_CUBE_MAX_FACES; face++) {
        xd_glfx_destroy_scene_capture(state->captures[face]);
        state->captures[face] = nullptr;
        state->captured[face] = false;
    }
}

/* The cube put away: the desktop shows as itself. */
static void cube_stop(cube_state *state) {
    state->phase = CUBE_IDLE;
    if (state->effect != nullptr)
        xd_glfx_end_output(state->effect);
    state->effect = nullptr;
    cube_release_captures(state);
    state->pitch = state->zoom = 0;
}

static bool cube_start(cube_state *state) {
    xd_server *server = state->server;
    const xd_desktop_grid *grid = xd_desktops_get_grid(server);
    if (grid->columns < 2)
        return false;
    if (state->effect == nullptr) {
        state->effect = xd_glfx_begin_output(server, &cube_output_impl, state);
        if (state->effect == nullptr)
            return false;
    }
    int column = 0;
    xd_desktop_grid_get_cell(grid, xd_desktops_get_current(server), &column, &state->row);
    state->desktops = XD_MIN(grid->columns, XD_CUBE_MAX_FACES);
    /* Two desktops side by side: two sides of a cube, a quarter turn
     * apart, not the front and back of a card. */
    state->faces = state->desktops == 2 ? 4 : state->desktops;
    wl_event_source_timer_update(state->timer, TICK_MS);
    cube_schedule_frames(server);
    return true;
}

static void cube_animate_to(cube_state *state, double angle, double seconds) {
    state->start_angle = state->angle;
    state->start_pitch = state->pitch;
    state->start_zoom = state->zoom;
    state->end_angle = angle;
    state->start_time = cube_get_now();
    state->duration = seconds;
}

static void cube_settled(cube_state *state) {
    xd_server *server = state->server;
    int face = state->target_face;
    int row = state->row;
    cube_stop(state);
    const xd_desktop_grid *grid = xd_desktops_get_grid(server);
    state->quiet_switch = true;
    xd_desktops_switch_to(server, xd_desktop_grid_get_index(grid, face, row), nullptr);
    state->quiet_switch = false;
}

static int cube_tick(void *data) {
    cube_state *state = data;
    if (state->phase == CUBE_IDLE)
        return 0;
    if (state->phase == CUBE_TURNING || state->phase == CUBE_SETTLING) {
        double time = state->duration > 0
                          ? (cube_get_now() - state->start_time) / state->duration
                          : 1.0;
        double progress = xd_wall_ease(time);
        state->angle = state->start_angle + (state->end_angle - state->start_angle) * progress;
        state->pitch = state->start_pitch * (1.0 - progress);
        state->zoom = state->zoom_out ? state->options.zoom * sin(M_PI * fmin(time, 1.0))
                                      : state->start_zoom * (1.0 - progress);
        if (time >= 1.0) {
            if (state->phase == CUBE_SETTLING)
                cube_settled(state);
            else
                cube_stop(state);
            return 0;
        }
    }
    cube_schedule_frames(state->server);
    wl_event_source_timer_update(state->timer, TICK_MS);
    return 0;
}

static bool cube_animate(xd_server *server, void *data, int from, int to,
                         [[maybe_unused]] xd_view *carried) {
    cube_state *state = data;
    if (state->quiet_switch)
        return true;
    const xd_desktop_grid *grid = xd_desktops_get_grid(server);
    int from_column = 0, from_row = 0, to_column = 0, to_row = 0;
    xd_desktop_grid_get_cell(grid, from, &from_column, &from_row);
    xd_desktop_grid_get_cell(grid, to, &to_column, &to_row);
    /* Up and down aren't the cube's (Desktop Wall slides them). */
    if (from_row != to_row || state->phase == CUBE_DRAGGING)
        return false;
    bool idle = state->phase == CUBE_IDLE;
    if (!cube_start(state))
        return false;
    int faces = state->faces;
    double step = 2.0 * M_PI / faces;
    if (idle)
        state->angle = xd_cube_get_face_angle(faces, from_column, 0);
    /* The short way round (going right when it's as short), from the face
     * nearest the front now; never past blank sides. */
    int columns = 0, rows = 0;
    xd_desktop_grid_get_direction(grid, from, to, state->faces == state->desktops, &columns,
                                  &rows);
    double target = xd_cube_get_face_angle(faces, from_column, state->angle) + columns * step;
    int steps = (int)lround(fabs(target - state->angle) / step);
    state->phase = CUBE_TURNING;
    state->zoom_out = true;
    cube_animate_to(state, target, state->options.rotation_seconds * XD_MAX(steps, 1));
    return true;
}

static void cube_handle_frame([[maybe_unused]] xd_server *server, void *data, xd_output *output,
                              [[maybe_unused]] double now) {
    cube_state *state = data;
    if (state->phase == CUBE_IDLE || state->effect == nullptr)
        return;
    cube_capture_faces(state, output);
    xd_glfx_draw_output(state->effect, output);
}

static void *cube_enable(xd_server *server) {
    cube_state *state = XD_NEW(cube_state);
    state->server = server;
    state->timer = wl_event_loop_add_timer(server->loop, cube_tick, state);
    xd_desktops_add_animator(server, CUBE_PRIORITY, cube_animate, state);
    running_cube = state;
    return state;
}

static void cube_disable(xd_server *server, void *data) {
    cube_state *state = data;
    xd_desktops_remove_animator(server, cube_animate, state);
    cube_stop(state);
    wl_event_source_remove(state->timer);
    running_cube = nullptr;
    xd_free(state);
}

static void cube_configure([[maybe_unused]] xd_server *server, void *data,
                           const xd_effects_settings *settings, size_t effect) {
    cube_state *state = data;
    cube_options *options = &state->options;
    cube_set_color(options->cap_color, xd_effects_get_color(settings, effect, "CapColor"));
    cube_set_color(options->background_color,
                   xd_effects_get_color(settings, effect, "BackgroundColor"));
    options->desktop_background = xd_effects_get_choice(settings, effect, "Background") == 0;
    options->rotation_opacity = xd_effects_get_int(settings, effect, "RotationOpacity") / 100.0;
    options->rotation_seconds = xd_effects_get_double(settings, effect, "RotationTime");
    options->zoom = xd_effects_get_double(settings, effect, "Zoom");
}

const xd_effect_impl xd_effect_impl_desktop_cube = {
    .id = "DesktopCube",
    .enable = cube_enable,
    .disable = cube_disable,
    .configure = cube_configure,
    .frame = cube_handle_frame,
};

/* ---- Rotate Cube ------------------------------------------------------------------ */

typedef struct rotate_state {
    rotate_options options;
    bool held; /* the binding pressed, the drag ours */
} rotate_state;

static void *rotate_enable([[maybe_unused]] xd_server *server) {
    return XD_NEW(rotate_state);
}

static void rotate_disable([[maybe_unused]] xd_server *server, void *data) {
    if (running_cube != nullptr && running_cube->phase == CUBE_DRAGGING)
        cube_stop(running_cube);
    xd_free(data);
}

static void rotate_configure([[maybe_unused]] xd_server *server, void *data,
                             const xd_effects_settings *settings, size_t effect) {
    rotate_state *rotate = data;
    xd_effects_get_binding(settings, effect, "Initiate", &rotate->options.initiate);
    rotate->options.sensitivity = xd_effects_get_double(settings, effect, "Sensitivity");
    if (running_cube != nullptr)
        running_cube->rotate = rotate->options;
}

static bool rotate_handle_button(xd_server *server, rotate_state *rotate,
                                 const xd_effect_input *input) {
    cube_state *state = running_cube;
    if (!input->pressed) {
        if (!rotate->held || input->button != rotate->options.initiate.button)
            return false;
        rotate->held = false;
        if (state == nullptr || state->phase != CUBE_DRAGGING)
            return true;
        if (!state->dragged) {
            cube_stop(state);
            return true;
        }
        /* To the face nearest the front, level again; from a blank side,
         * to the nearer desktop round it. */
        int front = xd_cube_get_front_face(state->faces, state->angle);
        if (front >= state->desktops)
            front = front - (state->desktops - 1) <= state->faces - front ? state->desktops - 1
                                                                          : 0;
        state->target_face = front;
        state->phase = CUBE_SETTLING;
        state->zoom_out = false;
        cube_animate_to(state,
                        xd_cube_get_face_angle(state->faces, state->target_face, state->angle),
                        SETTLE_SECONDS);
        return true;
    }
    if (state == nullptr || server->mode != XD_CURSOR_PASSTHROUGH ||
        !xd_effects_binding_matches(&rotate->options.initiate, input))
        return false;
    state->rotate = rotate->options;
    if (state->phase == CUBE_IDLE) {
        if (!cube_start(state))
            return false;
        int column = 0, row = 0;
        xd_desktop_grid_get_cell(xd_desktops_get_grid(server), xd_desktops_get_current(server),
                                 &column, &row);
        state->angle = xd_cube_get_face_angle(state->faces, column, 0);
        state->pitch = 0;
        state->zoom = 0;
    }
    rotate->held = true;
    state->phase = CUBE_DRAGGING;
    state->dragged = false;
    state->drag_x = server->cursor->x;
    state->drag_y = server->cursor->y;
    state->drag_angle = state->angle;
    state->drag_pitch = state->pitch;
    return true;
}

static bool rotate_handle_motion(xd_server *server, const rotate_state *rotate) {
    cube_state *state = running_cube;
    if (!rotate->held || state == nullptr || state->phase != CUBE_DRAGGING)
        return false;
    xd_output *output = xd_output_at(server, state->drag_x, state->drag_y);
    struct wlr_box box = {0, 0, 1024, 768};
    if (output != nullptr)
        wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
    double moved_x = server->cursor->x - state->drag_x;
    double moved_y = server->cursor->y - state->drag_y;
    if (!state->dragged && hypot(moved_x, moved_y) < DRAG_THRESHOLD)
        return true;
    state->dragged = true;
    /* Grabbing a face and pulling it: right turns the left face in, down
     * tilts the top towards the viewer. Across the screen is half a turn. */
    double sensitivity = state->rotate.sensitivity;
    state->angle = state->drag_angle - moved_x / box.width * M_PI * sensitivity;
    state->pitch = fmax(-M_PI / 2.0, fmin(M_PI / 2.0, state->drag_pitch +
                                                          moved_y / box.height * M_PI / 2.0 *
                                                              sensitivity));
    state->zoom = state->options.zoom;
    cube_schedule_frames(server);
    return true;
}

static bool rotate_handle_input(xd_server *server, void *data, const xd_effect_input *input) {
    rotate_state *rotate = data;
    switch (input->kind) {
    case XD_EFFECT_INPUT_BUTTON:
        return rotate_handle_button(server, rotate, input);
    case XD_EFFECT_INPUT_MOTION:
        return rotate_handle_motion(server, rotate);
    case XD_EFFECT_INPUT_KEY:
    case XD_EFFECT_INPUT_AXIS:
    case XD_EFFECT_INPUT_MODIFIERS:
        return false;
    }
    return false;
}

const xd_effect_impl xd_effect_impl_rotate_cube = {
    .id = "RotateCube",
    .enable = rotate_enable,
    .disable = rotate_disable,
    .configure = rotate_configure,
    .input = rotate_handle_input,
};
