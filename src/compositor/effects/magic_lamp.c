/* SPDX-License-Identifier: MIT
 * Magic Lamp (xd-compositor-fx): a window being minimized is drawn as a
 * mesh bent and slid into its taskbar button (magic_lamp_model.h), and
 * comes back out of it when restored; with OpenClose, windows also come
 * out of their button (or the pointer) when they open and go into it when
 * they close. It replaces XP's minimize animation while it's on.
 */
#include "../../core/alloc.h"
#include "glfx.h"
#include "magic_lamp_model.h"

/* Across, the lamp is straight: a few columns keep the texture from
 * shearing on the bent strips' two triangles. */
static constexpr int LAMP_COLUMNS = 6;

typedef struct lamp_options {
    int duration_ms;
    int grid;
    bool open_close;
    char windows[512]; /* which windows: xd_effects_match_window */
} lamp_options;

typedef struct lamp_state {
    xd_server *server;
    lamp_options options;
    struct wl_list windows; /* lamp_window.link */
} lamp_state;

typedef struct lamp_window {
    struct wl_list link; /* lamp_state.windows */
    lamp_state *state;
    xd_window_effect *effect;
    struct wlr_box icon; /* where it goes or comes from, layout units */
    double progress;     /* 0: the window, 1: in the icon */
    bool into_icon;      /* minimizing or closing: progress grows */
    xd_glfx_mesh mesh;
} lamp_window;

static const xd_window_effect_impl window_impl;

/* ---- the window effect ----------------------------------------------------------- */

static void lamp_begin(xd_window_effect *effect) {
    lamp_window *window = effect->data;
    if (!xd_glfx_init_mesh(&window->mesh, LAMP_COLUMNS, window->state->options.grid))
        xd_glfx_init_mesh(&window->mesh, LAMP_COLUMNS, 16);
}

static bool lamp_update(xd_window_effect *effect, double seconds) {
    lamp_window *window = effect->data;
    double step = seconds * 1000.0 / XD_MAX(window->state->options.duration_ms, 1);
    window->progress += window->into_icon ? step : -step;
    return window->into_icon ? window->progress < 1.0 : window->progress > 0.0;
}

static struct wlr_fbox lamp_get_bounds(xd_window_effect *effect) {
    lamp_window *window = effect->data;
    const struct wlr_fbox *box = &effect->texture.box;
    xd_lamp lamp;
    xd_lamp_init(&lamp, (xd_lamp_rect){box->x, box->y, box->width, box->height},
                 (xd_lamp_rect){window->icon.x, window->icon.y, window->icon.width,
                                window->icon.height});
    xd_glfx_mesh *mesh = &window->mesh;
    for (size_t vertex = 0; vertex < mesh->vertex_count; vertex++) {
        double point_x = 0, point_y = 0;
        xd_lamp_get_point(&lamp, (double)mesh->texcoords[vertex * 2],
                          (double)mesh->texcoords[vertex * 2 + 1], window->progress, &point_x,
                          &point_y);
        mesh->positions[vertex * 2] = (float)point_x;
        mesh->positions[vertex * 2 + 1] = (float)point_y;
    }
    struct wlr_fbox bounds = xd_glfx_get_mesh_bounds(mesh);
    return (struct wlr_fbox){bounds.x - 1, bounds.y - 1, bounds.width + 2, bounds.height + 2};
}

static void lamp_draw(xd_window_effect *effect, const xd_glfx_draw *draw) {
    lamp_window *window = effect->data;
    xd_glfx_draw_mesh(draw, &window->mesh, &effect->texture, 1.0f);
}

static void lamp_finish(xd_window_effect *effect) {
    lamp_window *window = effect->data;
    wl_list_remove(&window->link);
    xd_glfx_free_mesh(&window->mesh);
    xd_free(window);
}

static const xd_window_effect_impl window_impl = {
    .name = "Magic Lamp",
    .begin = lamp_begin,
    .update = lamp_update,
    .get_bounds = lamp_get_bounds,
    .draw = lamp_draw,
    .done = lamp_finish,
};

/* ---- the effect ----------------------------------------------------------------- */

static bool lamp_can_animate(const lamp_state *state, const xd_view *view) {
    return !view->fullscreen && xd_effects_match_window(state->options.windows,
                                                        xd_view_app_id(view),
                                                        xd_view_title(view), view->dialog);
}

/* Its taskbar button, or (opening or closing without one) the pointer. */
static bool lamp_get_icon(const lamp_state *state, const xd_view *view, bool pointer,
                          struct wlr_box *icon) {
    if (view->has_button_box) {
        *icon = view->button_box;
        return true;
    }
    if (!pointer)
        return false;
    const struct wlr_cursor *cursor = state->server->cursor;
    *icon = (struct wlr_box){(int)cursor->x, (int)cursor->y, 1, 1};
    return true;
}

static void lamp_start(lamp_state *state, xd_view *view, struct wlr_box icon, double progress,
                       bool into_icon) {
    lamp_window *window = XD_NEW(lamp_window);
    window->state = state;
    window->icon = icon;
    window->progress = progress;
    window->into_icon = into_icon;
    /* Into the icon the window is going: its last picture is kept. */
    window->effect = into_icon ? xd_glfx_begin_closing_window(view, &window_impl, window)
                               : xd_glfx_begin_window(view, &window_impl, window);
    if (window->effect == nullptr) {
        xd_free(window);
        return;
    }
    wl_list_insert(&state->windows, &window->link);
}

static void *lamp_enable(xd_server *server) {
    lamp_state *state = XD_NEW(lamp_state);
    state->server = server;
    wl_list_init(&state->windows);
    return state;
}

static void lamp_disable([[maybe_unused]] xd_server *server, void *data) {
    lamp_state *state = data;
    lamp_window *window, *next;
    wl_list_for_each_safe(window, next, &state->windows, link) {
        xd_glfx_end_window(window->effect); /* lamp_finish frees it */
    }
    xd_free(state);
}

static void lamp_configure([[maybe_unused]] xd_server *server, void *data,
                           const xd_effects_settings *settings, size_t effect) {
    lamp_state *state = data;
    lamp_options *options = &state->options;
    options->duration_ms = xd_effects_get_int(settings, effect, "Duration");
    options->grid = xd_effects_get_int(settings, effect, "GridResolution");
    options->open_close = xd_effects_get_bool(settings, effect, "OpenClose");
    snprintf(options->windows, sizeof(options->windows), "%s",
             xd_effects_get_text(settings, effect, "Windows"));
}

static void lamp_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                     const xd_window_event *event) {
    lamp_state *state = data;
    xd_view *view = event->view;
    xd_window_event_kind kind = event->kind;
    bool opening = kind == XD_WINDOW_EVENT_MAP && state->options.open_close;
    bool closing = kind == XD_WINDOW_EVENT_UNMAP && state->options.open_close;
    bool minimizing = kind == XD_WINDOW_EVENT_MINIMIZE;
    bool restoring = kind == XD_WINDOW_EVENT_UNMINIMIZE;
    struct wlr_box icon;
    if ((!opening && !closing && !minimizing && !restoring) || !lamp_can_animate(state, view) ||
        !lamp_get_icon(state, view, opening || closing, &icon))
        return;
    if (minimizing || closing) {
        lamp_start(state, view, icon, 0.0, true);
        return;
    }
    /* Restored while it's still going in: it comes back from where it is
     * (beginning the new effect ends that one). */
    double progress = 1.0;
    xd_window_effect *going = xd_glfx_find_window(view, &window_impl);
    if (going != nullptr && going->held)
        progress = ((const lamp_window *)going->data)->progress;
    lamp_start(state, view, icon, progress, false);
}

const xd_effect_impl xd_effect_impl_magic_lamp = {
    .id = "MagicLamp",
    .enable = lamp_enable,
    .disable = lamp_disable,
    .configure = lamp_configure,
    .window_event = lamp_handle_window_event,
    .animates_minimize = true,
};
