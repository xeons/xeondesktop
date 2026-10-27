/* SPDX-License-Identifier: MIT
 * Wobbly Windows (xd-compositor-fx): a window being moved, maximized,
 * snapped, opened or activated is drawn as the spring model of
 * wobbly_model.h, a mesh bent over its picture, until it comes to rest.
 */
#include <wlr/util/log.h>

#include "../../core/alloc.h"
#include "glfx.h"
#include "wobbly_model.h"

/* A shiver's push, layout units a step (Compiz's map and focus effect). */
static constexpr double SHIVER_STRENGTH = 2.5;
/* Maximized or snapped: how much of the old shape it starts from. */
static constexpr double JUMP_SHARE = 0.12;
/* How long a jump waits for the program to draw its new size before it
 * wobbly_can_wobble anyway. */
static constexpr double JUMP_WAIT_SECONDS = 0.25;

typedef struct wobbly_options {
    double spring, friction;
    int grid;
    bool on_move, on_maximize, on_map, on_focus;
    char windows[512]; /* which windows: xd_effects_match_window */
} wobbly_options;

typedef struct wobbly_state {
    xd_server *server;
    wobbly_options options;
    struct wl_list windows; /* wobbly_window.link */
} wobbly_state;

typedef enum wobbly_start : uint8_t {
    WOBBLY_START_NONE,
    WOBBLY_START_SHIVER,
    WOBBLY_START_JUMP, /* from `from`, once the program has its new size */
} wobbly_start;

typedef struct wobbly_window {
    struct wl_list link; /* wobbly_state.windows */
    wobbly_state *state;
    xd_window_effect *effect;
    xd_wobbly_model model;
    xd_glfx_mesh mesh;
    wobbly_start start;
    struct wlr_box from; /* JUMP: its outer box before */
    double waited;
} wobbly_window;

static const xd_window_effect_impl window_impl;

static bool wobbly_is_held(const wobbly_window *window) {
    const xd_server *server = window->state->server;
    return server->mode == XD_CURSOR_MOVE && server->grab_view == window->effect->view;
}

/* ---- the window effect ----------------------------------------------------------- */

static void wobbly_reset_model(wobbly_window *window) {
    const struct wlr_fbox *box = &window->effect->texture.box;
    const wobbly_options *options = &window->state->options;
    xd_wobbly_init(&window->model, box->x, box->y, box->width, box->height, options->spring,
                   options->friction);
}

static void wobbly_begin(xd_window_effect *effect) {
    wobbly_window *window = effect->data;
    wobbly_reset_model(window);
    int grid = window->state->options.grid;
    if (!xd_glfx_init_mesh(&window->mesh, grid, grid))
        xd_glfx_init_mesh(&window->mesh, 8, 8);
}

static bool wobbly_update(xd_window_effect *effect, double seconds) {
    wobbly_window *window = effect->data;
    xd_wobbly_model *model = &window->model;
    const struct wlr_fbox *box = &effect->texture.box;
    model->spring = window->state->options.spring;
    model->friction = window->state->options.friction;
    if (window->start == WOBBLY_START_SHIVER) {
        xd_wobbly_shiver(model, SHIVER_STRENGTH);
        window->start = WOBBLY_START_NONE;
    } else if (window->start == WOBBLY_START_JUMP) {
        /* The frame changes size at once, the program's surface when it
         * has drawn it: wobble from the old shape once it has. */
        struct wlr_box outer = xd_view_outer_box(effect->view);
        window->waited += seconds;
        bool resized = outer.width != window->from.width || outer.height != window->from.height;
        bool drawn = (int)box->width >= outer.width && (int)box->height >= outer.height &&
                     (int)box->width <= outer.width + 64 && (int)box->height <= outer.height + 64;
        wobbly_reset_model(window);
        if (!(resized && drawn) && window->waited < JUMP_WAIT_SECONDS)
            return true;
        /* `from` was the outer box; the picture may reach past it. */
        xd_wobbly_displace(model, window->from.x + (box->x - outer.x),
                           window->from.y + (box->y - outer.y),
                           window->from.width + (box->width - outer.width),
                           window->from.height + (box->height - outer.height), JUMP_SHARE);
        window->start = WOBBLY_START_NONE;
    }
    xd_wobbly_set_rest(model, box->x, box->y, box->width, box->height);
    bool held = wobbly_is_held(window);
    if (held && model->grabbed < 0) {
        const struct wlr_cursor *cursor = window->state->server->cursor;
        xd_wobbly_grab(model, cursor->x, cursor->y);
    } else if (!held && model->grabbed >= 0) {
        xd_wobbly_release(model);
    }
    return xd_wobbly_step(model, seconds) || held;
}

static struct wlr_fbox wobbly_get_bounds(xd_window_effect *effect) {
    wobbly_window *window = effect->data;
    xd_glfx_mesh *mesh = &window->mesh;
    for (size_t vertex = 0; vertex < mesh->vertex_count; vertex++) {
        double point_x = 0, point_y = 0;
        xd_wobbly_evaluate(&window->model, (double)mesh->texcoords[vertex * 2],
                           (double)mesh->texcoords[vertex * 2 + 1], &point_x, &point_y);
        mesh->positions[vertex * 2] = (float)point_x;
        mesh->positions[vertex * 2 + 1] = (float)point_y;
    }
    struct wlr_fbox bounds = xd_glfx_get_mesh_bounds(mesh);
    /* A pixel round it for the filtered edge. */
    return (struct wlr_fbox){bounds.x - 1, bounds.y - 1, bounds.width + 2, bounds.height + 2};
}

static void wobbly_draw(xd_window_effect *effect, const xd_glfx_draw *draw) {
    wobbly_window *window = effect->data;
    xd_glfx_draw_mesh(draw, &window->mesh, &effect->texture, 1.0f);
}

static void wobbly_finish(xd_window_effect *effect) {
    wobbly_window *window = effect->data;
    wl_list_remove(&window->link);
    xd_glfx_free_mesh(&window->mesh);
    xd_free(window);
}

static const xd_window_effect_impl window_impl = {
    .name = "Wobbly Windows",
    .begin = wobbly_begin,
    .update = wobbly_update,
    .get_bounds = wobbly_get_bounds,
    .draw = wobbly_draw,
    .done = wobbly_finish,
};

/* ---- the effect ----------------------------------------------------------------- */

static bool wobbly_can_wobble(const wobbly_state *state, const xd_view *view) {
    return view->mapped && !view->minimized && !view->fullscreen &&
           xd_effects_match_window(state->options.windows, xd_view_app_id(view),
                                   xd_view_title(view), view->dialog);
}

/* The window's wobble, started if it isn't wobbling. */
static wobbly_window *wobbly_start_window(wobbly_state *state, xd_view *view) {
    xd_window_effect *effect = xd_glfx_find_window(view, &window_impl);
    if (effect != nullptr)
        return effect->data;
    wobbly_window *window = XD_NEW(wobbly_window);
    window->state = state;
    window->effect = xd_glfx_begin_window(view, &window_impl, window);
    if (window->effect == nullptr) {
        xd_free(window);
        return nullptr;
    }
    wl_list_insert(&state->windows, &window->link);
    return window;
}

static void *wobbly_enable(xd_server *server) {
    wobbly_state *state = XD_NEW(wobbly_state);
    state->server = server;
    wl_list_init(&state->windows);
    return state;
}

static void wobbly_disable([[maybe_unused]] xd_server *server, void *data) {
    wobbly_state *state = data;
    wobbly_window *window, *next;
    wl_list_for_each_safe(window, next, &state->windows, link) {
        xd_glfx_end_window(window->effect); /* wobbly_finish frees it */
    }
    xd_free(state);
}

static void wobbly_configure([[maybe_unused]] xd_server *server, void *data,
                             const xd_effects_settings *settings, size_t effect) {
    wobbly_state *state = data;
    wobbly_options *options = &state->options;
    options->spring = xd_effects_get_double(settings, effect, "SpringConstant");
    options->friction = xd_effects_get_double(settings, effect, "Friction");
    options->grid = xd_effects_get_int(settings, effect, "GridResolution");
    options->on_move = xd_effects_get_bool(settings, effect, "WobbleOnMove");
    options->on_maximize = xd_effects_get_bool(settings, effect, "WobbleOnMaximize");
    options->on_map = xd_effects_get_bool(settings, effect, "WobbleOnMap");
    options->on_focus = xd_effects_get_bool(settings, effect, "WobbleOnFocus");
    snprintf(options->windows, sizeof(options->windows), "%s",
             xd_effects_get_text(settings, effect, "Windows"));
}

static void wobbly_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                const xd_window_event *event) {
    wobbly_state *state = data;
    const wobbly_options *options = &state->options;
    xd_view *view = event->view;
    bool shiver = (event->kind == XD_WINDOW_EVENT_MAP && options->on_map) ||
                  (event->kind == XD_WINDOW_EVENT_FOCUS && options->on_focus);
    bool jump = event->kind == XD_WINDOW_EVENT_MAXIMIZE && options->on_maximize;
    if ((!shiver && !jump) || !wobbly_can_wobble(state, view))
        return;
    wobbly_window *window = wobbly_start_window(state, view);
    if (window == nullptr)
        return;
    if (jump) {
        window->start = WOBBLY_START_JUMP;
        window->from = event->from;
        window->waited = 0;
    } else if (window->start == WOBBLY_START_NONE) {
        window->start = WOBBLY_START_SHIVER;
    }
}

/* A move began (by the title bar, a program's request or the keyboard's
 * Move with the mouse): the window is grabbed where the pointer is. */
static void wobbly_handle_frame(xd_server *server, void *data, [[maybe_unused]] xd_output *output,
                         [[maybe_unused]] double now) {
    wobbly_state *state = data;
    xd_view *view = server->grab_view;
    if (!state->options.on_move || server->mode != XD_CURSOR_MOVE || view == nullptr ||
        view->maximized || !wobbly_can_wobble(state, view) ||
        xd_glfx_find_window(view, &window_impl) != nullptr)
        return;
    wobbly_start_window(state, view);
}

const xd_effect_impl xd_effect_impl_wobbly_windows = {
    .id = "WobblyWindows",
    .enable = wobbly_enable,
    .disable = wobbly_disable,
    .configure = wobbly_configure,
    .window_event = wobbly_handle_window_event,
    .frame = wobbly_handle_frame,
};
