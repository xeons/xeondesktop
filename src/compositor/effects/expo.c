/* SPDX-License-Identifier: MIT
 * Expo (xd-compositor-fx): every desktop at once as a wall, Compiz's expo
 * plugin flat. Its key or its corner zooms out from the current desktop
 * (viewport_model.h's xd_expo_get_cell, unit-tested) to the whole wall;
 * a click on a desktop zooms in to it and shows it, Escape or the key
 * again back to the current one. A window dragged from one desktop to
 * another follows the pointer, and goes to that desktop where it's let
 * go.
 *
 * A whole-output effect (glfx.h): each desktop is a scene capture, at
 * about the size it's drawn so the wall stays cheap on software
 * rendering, and the dragged window one of its own. While it shows, the
 * pointer and the clicks are Expo's.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <time.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../../core/alloc.h"
#include "../desktops_grid.h"
#include "glfx.h"
#include "viewport_model.h"

/* Above Desktop Cube's: a switch Expo makes isn't animated again. */
static constexpr int EXPO_PRIORITY = 30;
static constexpr int TICK_MS = 16;
enum { MAX_DESKTOPS = 16 };
/* A press moved this far is a drag. */
static constexpr double DRAG_THRESHOLD = 5.0;
/* The desktops but the one under the pointer, a little darker. */
static constexpr float INACTIVE_BRIGHTNESS = 0.75f;
/* Captures never smaller than this share of the output. */
static constexpr double MIN_CAPTURE_SCALE = 0.2;

typedef enum expo_phase : uint8_t {
    EXPO_HIDDEN,
    EXPO_ZOOMING_OUT,
    EXPO_SHOWN,
    EXPO_ZOOMING_IN, /* then hidden */
} expo_phase;

typedef struct expo_options {
    xd_effect_binding key;
    uint32_t edges;
    double zoom_seconds;
    double spacing;
} expo_options;

typedef struct expo_state {
    xd_server *server;
    expo_options options;
    expo_phase phase;
    xd_output_effect *effect;
    struct wl_event_source *timer;
    double progress; /* 0: the current desktop fills the screen; 1: the wall */
    double phase_start;
    int focus; /* the desktop zoomed in to and out from */
    bool quiet_switch;
    bool at_edge; /* the pointer is in the corner (once per visit) */
    /* A press on a desktop, and the window it may drag. */
    bool pressed;
    double press_x, press_y;
    int press_desktop;
    xd_view *drag_view;
    bool dragging;
    double grab_x, grab_y; /* where in the window, layout units */
    int hover; /* the desktop under the pointer, or -1 */
    xd_glfx_scene_capture *captures[MAX_DESKTOPS];
    xd_glfx_texture textures[MAX_DESKTOPS];
    bool captured[MAX_DESKTOPS];
    xd_glfx_scene_capture *drag_capture;
    xd_glfx_texture drag_texture;
    bool drag_captured;
} expo_state;

static double expo_get_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void expo_schedule_frames(xd_server *server) {
    XD_LIST_FOR_EACH(output, &server->outputs, xd_output, link) {
        if (output->wlr_output->enabled)
            wlr_output_schedule_frame(output->wlr_output);
    }
}

static int expo_count_desktops(const expo_state *state) {
    return XD_MIN(xd_desktops_get_count(state->server), MAX_DESKTOPS);
}

static xd_expo_layout expo_get_layout(const expo_state *state, struct wlr_box box) {
    const xd_desktop_grid *grid = xd_desktops_get_grid(state->server);
    xd_expo_layout layout = {
        .columns = grid->columns,
        .rows = grid->rows,
        .width = box.width,
        .height = box.height,
        .spacing = state->options.spacing,
    };
    xd_desktop_grid_get_cell(grid, state->focus, &layout.current_column, &layout.current_row);
    return layout;
}

static struct wlr_box expo_get_output_box(xd_server *server, xd_output *output) {
    struct wlr_box box = {0};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
    return box;
}

/* ---- drawing ---------------------------------------------------------------------- */

typedef struct expo_filter {
    int desktop;
    const xd_view *skip; /* the window being dragged */
} expo_filter;

static bool expo_shows_view(const xd_view *view, void *data) {
    const expo_filter *filter = data;
    return view != filter->skip && !view->minimized &&
           (view->sticky || view->desktop == filter->desktop);
}

static bool expo_is_drag_view(const xd_view *view, void *data) {
    return view == data;
}

static void expo_capture(expo_state *state, xd_output *output) {
    xd_server *server = state->server;
    struct wlr_box box = expo_get_output_box(server, output);
    xd_expo_layout layout = expo_get_layout(state, box);
    double x = 0, y = 0, width = 0, height = 0;
    xd_expo_get_cell(&layout, 0, 0, state->progress, &x, &y, &width, &height);
    double output_scale = (double)output->wlr_output->scale;
    double scale = output_scale * fmin(fmax(width / box.width, MIN_CAPTURE_SCALE), 1.0);
    const xd_desktop_grid *grid = xd_desktops_get_grid(server);
    for (int desktop = 0; desktop < expo_count_desktops(state); desktop++) {
        int column = 0, row = 0;
        xd_desktop_grid_get_cell(grid, desktop, &column, &row);
        xd_expo_get_cell(&layout, column, row, state->progress, &x, &y, &width, &height);
        state->captured[desktop] = false;
        /* Off the screen while zoomed in: not drawn. */
        if (x >= box.width || y >= box.height || x + width <= 0 || y + height <= 0)
            continue;
        if (state->captures[desktop] == nullptr)
            state->captures[desktop] = xd_glfx_create_scene_capture(server);
        expo_filter filter = {desktop, state->dragging ? state->drag_view : nullptr};
        state->captured[desktop] =
            xd_glfx_capture_scene(state->captures[desktop], box, scale, true, expo_shows_view,
                                  &filter, &state->textures[desktop]);
    }
    state->drag_captured = false;
    if (state->dragging && state->drag_view != nullptr) {
        if (state->drag_capture == nullptr)
            state->drag_capture = xd_glfx_create_scene_capture(server);
        state->drag_captured = xd_glfx_capture_scene(
            state->drag_capture, xd_view_outer_box(state->drag_view), scale, false,
            expo_is_drag_view, state->drag_view, &state->drag_texture);
    }
}

/* A rectangle (output units) as a flat quad in clip space. */
static void expo_get_quad(struct wlr_box box, double x, double y, double width, double height,
                          float quad[4][4]) {
    const double corners[4][2] = {{x, y}, {x + width, y}, {x + width, y + height},
                                  {x, y + height}};
    for (int i = 0; i < 4; i++) {
        quad[i][0] = (float)(corners[i][0] / box.width * 2.0 - 1.0);
        quad[i][1] = (float)(corners[i][1] / box.height * 2.0 - 1.0);
        quad[i][2] = 0.0f;
        quad[i][3] = 1.0f;
    }
}

static void expo_draw(xd_output_effect *effect, [[maybe_unused]] xd_output *output,
                      const xd_glfx_draw *draw) {
    expo_state *state = effect->data;
    struct wlr_box box = {0, 0, draw->bounds.width, draw->bounds.height};
    /* Compiz's dark backdrop behind the wall. */
    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    xd_expo_layout layout = expo_get_layout(state, box);
    const xd_desktop_grid *grid = xd_desktops_get_grid(state->server);
    double cell_width = 0, cell_height = 0;
    for (int desktop = 0; desktop < expo_count_desktops(state); desktop++) {
        if (!state->captured[desktop])
            continue;
        int column = 0, row = 0;
        xd_desktop_grid_get_cell(grid, desktop, &column, &row);
        double x = 0, y = 0;
        xd_expo_get_cell(&layout, column, row, state->progress, &x, &y, &cell_width,
                         &cell_height);
        float quad[4][4];
        expo_get_quad(box, x, y, cell_width, cell_height, quad);
        bool lit = state->phase != EXPO_SHOWN ? desktop == state->focus
                                              : desktop == state->hover;
        float brightness =
            lit ? 1.0f : 1.0f - (1.0f - INACTIVE_BRIGHTNESS) * (float)state->progress;
        xd_glfx_draw_quad(draw, &state->textures[desktop], quad, 1.0f, brightness);
    }
    /* The dragged window, at the wall's scale, held where it was taken. */
    if (state->dragging && state->drag_captured && state->drag_view != nullptr) {
        struct wlr_box outer = xd_view_outer_box(state->drag_view);
        double scale = cell_width / box.width;
        double pointer_x = state->server->cursor->x - draw->bounds.x;
        double pointer_y = state->server->cursor->y - draw->bounds.y;
        float quad[4][4];
        expo_get_quad(box, pointer_x - state->grab_x * scale, pointer_y - state->grab_y * scale,
                      outer.width * scale, outer.height * scale, quad);
        xd_glfx_draw_quad(draw, &state->drag_texture, quad, 0.9f, 1.0f);
    }
}

static const xd_output_effect_impl expo_output_impl = {
    .name = "Expo",
    .draw = expo_draw,
};

/* ---- showing and hiding ----------------------------------------------------------- */

static void expo_release_captures(expo_state *state) {
    for (int desktop = 0; desktop < MAX_DESKTOPS; desktop++) {
        xd_glfx_destroy_scene_capture(state->captures[desktop]);
        state->captures[desktop] = nullptr;
        state->captured[desktop] = false;
    }
    xd_glfx_destroy_scene_capture(state->drag_capture);
    state->drag_capture = nullptr;
    state->drag_captured = false;
}

static void expo_hide(expo_state *state) {
    state->phase = EXPO_HIDDEN;
    state->pressed = state->dragging = false;
    state->drag_view = nullptr;
    if (state->effect != nullptr)
        xd_glfx_end_output(state->effect);
    state->effect = nullptr;
    expo_release_captures(state);
}

static void expo_show(expo_state *state) {
    xd_server *server = state->server;
    if (state->phase == EXPO_SHOWN || state->phase == EXPO_ZOOMING_OUT ||
        xd_desktops_get_count(server) < 2)
        return;
    if (state->effect == nullptr) {
        state->effect = xd_glfx_begin_output(server, &expo_output_impl, state);
        if (state->effect == nullptr)
            return;
        state->progress = 0;
    }
    state->focus = xd_desktops_get_current(server);
    state->hover = -1;
    state->phase = EXPO_ZOOMING_OUT;
    state->phase_start = expo_get_now() - state->progress * state->options.zoom_seconds;
    wl_event_source_timer_update(state->timer, TICK_MS);
    expo_schedule_frames(server);
}

/* Zooms in to `desktop`, which shows (now, under the picture). */
static void expo_zoom_in(expo_state *state, int desktop) {
    xd_server *server = state->server;
    state->focus = desktop;
    state->quiet_switch = true;
    xd_desktops_switch_to(server, desktop, nullptr);
    state->quiet_switch = false;
    state->phase = EXPO_ZOOMING_IN;
    state->phase_start = expo_get_now() - (1.0 - state->progress) * state->options.zoom_seconds;
    wl_event_source_timer_update(state->timer, TICK_MS);
}

static int expo_tick(void *data) {
    expo_state *state = data;
    if (state->phase == EXPO_HIDDEN)
        return 0;
    double seconds = state->options.zoom_seconds;
    double time = seconds > 0 ? (expo_get_now() - state->phase_start) / seconds : 1.0;
    if (state->phase == EXPO_ZOOMING_OUT) {
        state->progress = xd_wall_ease(time);
        if (time >= 1.0) {
            state->progress = 1.0;
            state->phase = EXPO_SHOWN;
        }
    } else if (state->phase == EXPO_ZOOMING_IN) {
        state->progress = 1.0 - xd_wall_ease(time);
        if (time >= 1.0) {
            expo_hide(state);
            return 0;
        }
    }
    /* Shown, the desktops stay live. */
    expo_schedule_frames(state->server);
    wl_event_source_timer_update(state->timer, TICK_MS);
    return 0;
}

static bool expo_animate([[maybe_unused]] xd_server *server, void *data,
                         [[maybe_unused]] int from, [[maybe_unused]] int to,
                         [[maybe_unused]] xd_view *carried) {
    const expo_state *state = data;
    return state->quiet_switch;
}

/* ---- the pointer ------------------------------------------------------------------ */

/* The desktop under the pointer on the wall, and where in it. */
static int expo_find_desktop(expo_state *state, double *layout_x, double *layout_y) {
    xd_server *server = state->server;
    xd_output *output = xd_output_at(server, server->cursor->x, server->cursor->y);
    if (output == nullptr)
        return -1;
    struct wlr_box box = expo_get_output_box(server, output);
    xd_expo_layout layout = expo_get_layout(state, box);
    double across = 0, down = 0;
    int desktop = xd_expo_find_desktop(&layout, server->cursor->x - box.x,
                                       server->cursor->y - box.y, &across, &down);
    if (desktop >= expo_count_desktops(state))
        return -1;
    *layout_x = box.x + across * box.width;
    *layout_y = box.y + down * box.height;
    return desktop;
}

/* The window on `desktop` at that layout point, front first. */
static xd_view *expo_find_view(expo_state *state, int desktop, double x, double y) {
    XD_LIST_FOR_EACH(view, &state->server->views, xd_view, link) {
        if (!view->mapped || view->minimized || (!view->sticky && view->desktop != desktop))
            continue;
        struct wlr_box box = xd_view_outer_box(view);
        if (x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height)
            return view;
    }
    return nullptr;
}

static void expo_drop(expo_state *state) {
    xd_view *view = state->drag_view;
    double x = 0, y = 0;
    int desktop = expo_find_desktop(state, &x, &y);
    state->dragging = false;
    state->drag_view = nullptr;
    if (view == nullptr || !view->mapped || desktop < 0)
        return;
    xd_view_set_position(view, (int)lround(x - state->grab_x), (int)lround(y - state->grab_y));
    if (view->sticky || view->desktop != desktop)
        xd_desktops_move_view(view, desktop);
}

static bool expo_handle_button(xd_server *server, void *data, uint32_t button, bool pressed,
                               [[maybe_unused]] uint32_t modifiers) {
    expo_state *state = data;
    if (state->phase == EXPO_HIDDEN)
        return false;
    if (state->phase != EXPO_SHOWN || button != BTN_LEFT)
        return true; /* the wall has the pointer */
    double x = 0, y = 0;
    if (pressed) {
        state->pressed = true;
        state->press_x = server->cursor->x;
        state->press_y = server->cursor->y;
        state->press_desktop = expo_find_desktop(state, &x, &y);
        state->drag_view = state->press_desktop >= 0
                               ? expo_find_view(state, state->press_desktop, x, y)
                               : nullptr;
        if (state->drag_view != nullptr) {
            struct wlr_box outer = xd_view_outer_box(state->drag_view);
            state->grab_x = x - outer.x;
            state->grab_y = y - outer.y;
        }
        return true;
    }
    if (!state->pressed)
        return true;
    state->pressed = false;
    if (state->dragging) {
        expo_drop(state);
        return true;
    }
    state->drag_view = nullptr;
    /* A click on a desktop: there. */
    int desktop = expo_find_desktop(state, &x, &y);
    if (desktop >= 0 && desktop == state->press_desktop)
        expo_zoom_in(state, desktop);
    return true;
}

static bool expo_handle_motion(xd_server *server, void *data,
                               [[maybe_unused]] uint32_t time_msec) {
    expo_state *state = data;
    if (state->phase == EXPO_HIDDEN) {
        /* The corner (as Compiz's edges, once per visit). */
        xd_output *output = xd_output_at(server, server->cursor->x, server->cursor->y);
        bool at_edge = false;
        if (output != nullptr && state->options.edges != 0 &&
            server->mode == XD_CURSOR_PASSTHROUGH && server->seat->drag == nullptr) {
            struct wlr_box box = expo_get_output_box(server, output);
            bool left = server->cursor->x < box.x + 1, top = server->cursor->y < box.y + 1;
            bool right = server->cursor->x >= box.x + box.width - 1;
            bool bottom = server->cursor->y >= box.y + box.height - 1;
            uint32_t edges = state->options.edges;
            at_edge = ((edges & XD_SCREEN_EDGE_TOP_LEFT) && top && left) ||
                      ((edges & XD_SCREEN_EDGE_TOP_RIGHT) && top && right) ||
                      ((edges & XD_SCREEN_EDGE_BOTTOM_LEFT) && bottom && left) ||
                      ((edges & XD_SCREEN_EDGE_BOTTOM_RIGHT) && bottom && right) ||
                      ((edges & XD_SCREEN_EDGE_LEFT) && left) ||
                      ((edges & XD_SCREEN_EDGE_RIGHT) && right) ||
                      ((edges & XD_SCREEN_EDGE_TOP) && top) ||
                      ((edges & XD_SCREEN_EDGE_BOTTOM) && bottom);
        }
        if (at_edge && !state->at_edge)
            expo_show(state);
        state->at_edge = at_edge;
        return false;
    }
    double x = 0, y = 0;
    state->hover = expo_find_desktop(state, &x, &y);
    if (state->pressed && !state->dragging && state->drag_view != nullptr &&
        hypot(server->cursor->x - state->press_x, server->cursor->y - state->press_y) >=
            DRAG_THRESHOLD)
        state->dragging = true;
    expo_schedule_frames(server);
    return true;
}

static bool expo_handle_key(xd_server *server, void *data, uint32_t modifiers, uint32_t keysym) {
    expo_state *state = data;
    bool shown = state->phase == EXPO_SHOWN || state->phase == EXPO_ZOOMING_OUT;
    if (xd_effects_binding_matches_key(&state->options.key, modifiers, keysym)) {
        /* With one desktop there's no wall: the key is the program's. */
        if (!shown && xd_desktops_get_count(server) < 2)
            return false;
        if (shown)
            expo_zoom_in(state, state->focus);
        else
            expo_show(state);
        return true;
    }
    if (shown && keysym == XKB_KEY_Escape) {
        state->dragging = state->pressed = false;
        state->drag_view = nullptr;
        expo_zoom_in(state, state->focus);
        return true;
    }
    return false;
}

static void expo_handle_frame([[maybe_unused]] xd_server *server, void *data, xd_output *output,
                              [[maybe_unused]] double now) {
    expo_state *state = data;
    if (state->phase == EXPO_HIDDEN || state->effect == nullptr)
        return;
    expo_capture(state, output);
    xd_glfx_draw_output(state->effect, output);
}

static void expo_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                     const xd_window_event *event) {
    expo_state *state = data;
    if ((event->kind == XD_WINDOW_EVENT_UNMAP || event->kind == XD_WINDOW_EVENT_DESTROY) &&
        event->view == state->drag_view) {
        state->drag_view = nullptr;
        state->dragging = false;
    }
}

/* ---- the effect ------------------------------------------------------------------- */

static void *expo_enable(xd_server *server) {
    expo_state *state = XD_NEW(expo_state);
    state->server = server;
    state->hover = -1;
    state->timer = wl_event_loop_add_timer(server->loop, expo_tick, state);
    xd_desktops_add_animator(server, EXPO_PRIORITY, expo_animate, state);
    return state;
}

static void expo_disable(xd_server *server, void *data) {
    expo_state *state = data;
    xd_desktops_remove_animator(server, expo_animate, state);
    expo_hide(state);
    wl_event_source_remove(state->timer);
    xd_free(state);
}

static void expo_configure([[maybe_unused]] xd_server *server, void *data,
                           const xd_effects_settings *settings, size_t effect) {
    expo_state *state = data;
    expo_options *options = &state->options;
    options->key = xd_effects_get_binding(settings, effect, "ExpoKey");
    options->edges = xd_effects_get_edges(settings, effect, "ExpoEdge");
    options->zoom_seconds = xd_effects_get_double(settings, effect, "ZoomTime");
    options->spacing = xd_effects_get_int(settings, effect, "Spacing");
}

const xd_effect_impl xd_effect_impl_expo = {
    .id = "Expo",
    .enable = expo_enable,
    .disable = expo_disable,
    .configure = expo_configure,
    .window_event = expo_handle_window_event,
    .frame = expo_handle_frame,
    .key = expo_handle_key,
    .button = expo_handle_button,
    .motion = expo_handle_motion,
};
