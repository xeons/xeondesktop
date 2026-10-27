/* SPDX-License-Identifier: MIT
 * Desktop Wall (both builds): a switch between virtual desktops slides
 * the old desktop's windows out and the new one's in, the way the view
 * moves (viewport_model.h's xd_wall_get_offset), by moving their scene
 * nodes for a moment; their windows stay where they are. A small map of
 * the desktops, the new one lit, shows meanwhile, as Compiz's Wall drew
 * it. Edge flip move: a window dragged against the screen's edge for a
 * while is carried to the desktop that way, the pointer coming in at the
 * other side.
 */
#include <cairo.h>
#include <math.h>
#include <time.h>
#include <wlr/util/log.h>

#include "../../core/alloc.h"
#include "../desktops_grid.h"
#include "effects.h"
#include "viewport_model.h"

/* Below Desktop Cube's: the cube turns when it's on, and Wall slides
 * what it leaves (switches up and down). */
static constexpr int WALL_PRIORITY = 10;
static constexpr int TICK_MS = 10;
/* The map's desktops (logical px tall, the screen's shape), their gaps
 * and its margin. */
static constexpr double MAP_CELL_HEIGHT = 40;
static constexpr double MAP_GAP = 6;
static constexpr double MAP_MARGIN = 10;
/* Edge flip: how close to the edge counts, and how far from it the
 * pointer comes in on the other side. */
static constexpr double EDGE_BAND = 1.0;
static constexpr double EDGE_ENTRY = 20.0;

typedef struct wall_options {
    double slide_seconds;
    bool show_switcher;
    double preview_seconds;
    bool edge_flip_move;
    int edge_flip_delay;
} wall_options;

typedef struct wall_state {
    xd_server *server;
    wall_options options;
    struct wl_event_source *timer;
    /* The slide. */
    bool sliding;
    int from, to;
    int columns, rows; /* whole desktops the view moves */
    xd_view *carried;
    double start;
    /* The map, until preview_until. */
    struct wlr_scene_buffer *map;
    double preview_until;
    /* Edge flip: the edge the pointer is at during a move, since when. */
    int edge_columns, edge_rows;
    double edge_since;
    struct wl_event_source *edge_timer;
} wall_state;

static double wall_get_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int wall_round_up(double value) {
    double rounded = ceil(value);
    return (int)rounded;
}

static struct wlr_box wall_get_layout_box(xd_server *server) {
    struct wlr_box box = {0};
    wlr_output_layout_get_box(server->output_layout, nullptr, &box);
    return box;
}

/* ---- the map ---------------------------------------------------------------------- */

static void wall_hide_map(wall_state *state) {
    if (state->map != nullptr)
        wlr_scene_node_destroy(&state->map->node);
    state->map = nullptr;
}

static void wall_fill_rounded(cairo_t *cairo, double x, double y, double width, double height,
                              double radius) {
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, x + width - radius, y + radius, radius, -M_PI / 2.0, 0);
    cairo_arc(cairo, x + width - radius, y + height - radius, radius, 0, M_PI / 2.0);
    cairo_arc(cairo, x + radius, y + height - radius, radius, M_PI / 2.0, M_PI);
    cairo_arc(cairo, x + radius, y + radius, radius, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cairo);
}

/* Compiz Wall's: a light grey panel with a dark outline, the desktops dark
 * and the current one light, centred on the monitor under the pointer. */
static void wall_show_map(wall_state *state) {
    xd_server *server = state->server;
    const xd_desktop_grid *grid = xd_desktops_get_grid(server);
    double cursor_x = server->cursor->x, cursor_y = server->cursor->y;
    xd_output *output = xd_output_at(server, cursor_x, cursor_y);
    if (output == nullptr)
        return;
    struct wlr_box area = {0};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &area);
    double scale = (double)output->wlr_output->scale;
    double aspect = area.height > 0 ? (double)area.width / area.height : 4.0 / 3.0;
    double cell_height = MAP_CELL_HEIGHT, cell_width = MAP_CELL_HEIGHT * aspect;
    double width = MAP_MARGIN * 2 + grid->columns * cell_width + (grid->columns - 1) * MAP_GAP;
    double height = MAP_MARGIN * 2 + grid->rows * cell_height + (grid->rows - 1) * MAP_GAP;
    int width_px = wall_round_up(width * scale), height_px = wall_round_up(height * scale);
    cairo_surface_t *surface = nullptr;
    struct wlr_buffer *buffer = xd_cairo_buffer_new(width_px, height_px, &surface);
    if (buffer == nullptr)
        return;
    cairo_t *cairo = cairo_create(surface);
    cairo_scale(cairo, scale, scale);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    wall_fill_rounded(cairo, 0.5, 0.5, width - 1, height - 1, 5);
    cairo_set_source_rgba(cairo, 0.8, 0.8, 0.8, 0.9);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgba(cairo, 0.2, 0.2, 0.2, 0.85);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
    int current = xd_desktops_get_current(server);
    for (int row = 0; row < grid->rows; row++) {
        for (int column = 0; column < grid->columns; column++) {
            double x = MAP_MARGIN + column * (cell_width + MAP_GAP);
            double y = MAP_MARGIN + row * (cell_height + MAP_GAP);
            bool lit = xd_desktop_grid_get_index(grid, column, row) == current;
            wall_fill_rounded(cairo, x, y, cell_width, cell_height, 3);
            if (lit)
                cairo_set_source_rgba(cairo, 1.0, 1.0, 1.0, 0.9);
            else
                cairo_set_source_rgba(cairo, 0.2, 0.2, 0.2, 0.55);
            cairo_fill_preserve(cairo);
            cairo_set_source_rgba(cairo, 0.2, 0.2, 0.2, 0.85);
            cairo_stroke(cairo);
        }
    }
    cairo_destroy(cairo);
    cairo_surface_flush(surface);
    if (state->map == nullptr)
        state->map = wlr_scene_buffer_create(server->layer_drag, nullptr);
    wlr_scene_buffer_set_buffer(state->map, buffer);
    wlr_buffer_drop(buffer); /* the scene holds its own lock */
    int logical_width = wall_round_up(width), logical_height = wall_round_up(height);
    wlr_scene_buffer_set_dest_size(state->map, logical_width, logical_height);
    wlr_scene_node_set_position(&state->map->node, area.x + (area.width - logical_width) / 2,
                                area.y + (area.height - logical_height) / 2);
    wlr_scene_node_raise_to_top(&state->map->node);
}

/* ---- the slide -------------------------------------------------------------------- */

static bool wall_slides_view(const wall_state *state, const xd_view *view) {
    return view->mapped && !view->minimized && !view->sticky && view != state->carried &&
           (view->desktop == state->from || view->desktop == state->to);
}

static void wall_place_views(wall_state *state, double progress) {
    xd_server *server = state->server;
    struct wlr_box box = wall_get_layout_box(server);
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (!wall_slides_view(state, view))
            continue;
        double offset_x = 0, offset_y = 0;
        xd_wall_get_offset(state->columns, state->rows, progress, view->desktop == state->to,
                           box.width, box.height, &offset_x, &offset_y);
        wlr_scene_node_set_enabled(&view->tree->node, true);
        wlr_scene_node_set_position(&view->tree->node, view->x + (int)lround(offset_x),
                                    view->y + (int)lround(offset_y));
    }
}

static void wall_end_slide(wall_state *state) {
    if (!state->sliding)
        return;
    state->sliding = false;
    state->carried = nullptr;
    /* Every window where it is again, the old desktop's hidden. */
    xd_desktops_apply(state->server);
}

static int wall_tick(void *data) {
    wall_state *state = data;
    double now = wall_get_now();
    if (state->sliding) {
        double time = state->options.slide_seconds > 0
                          ? (now - state->start) / state->options.slide_seconds
                          : 1.0;
        if (time >= 1.0)
            wall_end_slide(state);
        else
            wall_place_views(state, xd_wall_ease(time));
    }
    if (state->map != nullptr && !state->sliding && now >= state->preview_until)
        wall_hide_map(state);
    if (state->sliding || state->map != nullptr)
        wl_event_source_timer_update(state->timer, TICK_MS);
    return 0;
}

static bool wall_animate(xd_server *server, void *data, int from, int to, xd_view *carried) {
    wall_state *state = data;
    if (state->options.slide_seconds <= 0 && !state->options.show_switcher)
        return false;
    wall_end_slide(state);
    xd_desktop_grid_get_direction(xd_desktops_get_grid(server), from, to, false,
                                  &state->columns, &state->rows);
    state->from = from;
    state->to = to;
    state->carried = carried;
    state->start = wall_get_now();
    state->sliding = state->options.slide_seconds > 0;
    if (state->sliding)
        wall_place_views(state, 0.0);
    if (state->options.show_switcher) {
        wall_show_map(state);
        state->preview_until = state->start + XD_MAX(state->options.slide_seconds, 0.0) +
                               state->options.preview_seconds;
    }
    wl_event_source_timer_update(state->timer, TICK_MS);
    return true;
}

/* ---- edge flip -------------------------------------------------------------------- */

/* Which edge of the screen the pointer is against, as whole desktops
 * that way; false for none (or no desktop beyond it). */
static bool wall_find_edge(wall_state *state, int *columns, int *rows) {
    xd_server *server = state->server;
    struct wlr_box box = wall_get_layout_box(server);
    double x = server->cursor->x, y = server->cursor->y;
    *columns = x <= box.x + EDGE_BAND ? -1 : x >= box.x + box.width - 1 - EDGE_BAND ? 1 : 0;
    *rows = y <= box.y + EDGE_BAND ? -1 : y >= box.y + box.height - 1 - EDGE_BAND ? 1 : 0;
    if (*columns == 0 && *rows == 0)
        return false;
    return xd_desktop_grid_find_neighbour(xd_desktops_get_grid(server),
                                          xd_desktops_get_current(server), *columns, *rows,
                                          false) >= 0;
}

static bool wall_is_moving(const xd_server *server) {
    return server->mode == XD_CURSOR_MOVE && server->grab_view != nullptr &&
           !server->grab_view->maximized;
}

static int wall_flip_edge(void *data) {
    wall_state *state = data;
    xd_server *server = state->server;
    int columns = 0, rows = 0;
    if (!wall_is_moving(server) || !wall_find_edge(state, &columns, &rows) ||
        columns != state->edge_columns || rows != state->edge_rows)
        return 0;
    state->edge_columns = state->edge_rows = 0;
    if (!xd_desktops_switch_by(server, columns, rows, server->grab_view))
        return 0;
    /* The pointer, and the window it holds, come in from the other side. */
    struct wlr_box box = wall_get_layout_box(server);
    double x = server->cursor->x - columns * (box.width - 2 * EDGE_ENTRY);
    double y = server->cursor->y - rows * (box.height - 2 * EDGE_ENTRY);
    wlr_cursor_warp(server->cursor, nullptr, x, y);
    xd_input_pointer_moved(server);
    return 0;
}

static bool wall_handle_input([[maybe_unused]] xd_server *server, void *data,
                              const xd_effect_input *input) {
    wall_state *state = data;
    if (input->kind != XD_EFFECT_INPUT_MOTION)
        return false;
    int columns = 0, rows = 0;
    if (!state->options.edge_flip_move || !wall_is_moving(state->server) ||
        !wall_find_edge(state, &columns, &rows)) {
        state->edge_columns = state->edge_rows = 0;
        return false;
    }
    if (columns != state->edge_columns || rows != state->edge_rows) {
        state->edge_columns = columns;
        state->edge_rows = rows;
        wl_event_source_timer_update(state->edge_timer, XD_MAX(state->options.edge_flip_delay, 1));
    }
    return false;
}

/* ---- the effect ------------------------------------------------------------------- */

static void *wall_enable(xd_server *server) {
    wall_state *state = XD_NEW(wall_state);
    state->server = server;
    state->timer = wl_event_loop_add_timer(server->loop, wall_tick, state);
    state->edge_timer = wl_event_loop_add_timer(server->loop, wall_flip_edge, state);
    xd_desktops_add_animator(server, WALL_PRIORITY, wall_animate, state);
    return state;
}

static void wall_disable(xd_server *server, void *data) {
    wall_state *state = data;
    xd_desktops_remove_animator(server, wall_animate, state);
    wall_end_slide(state);
    wall_hide_map(state);
    wl_event_source_remove(state->timer);
    wl_event_source_remove(state->edge_timer);
    xd_free(state);
}

static void wall_configure([[maybe_unused]] xd_server *server, void *data,
                           const xd_effects_settings *settings, size_t effect) {
    wall_state *state = data;
    wall_options *options = &state->options;
    options->slide_seconds = xd_effects_get_double(settings, effect, "SlideDuration");
    options->show_switcher = xd_effects_get_bool(settings, effect, "ShowSwitcher");
    options->preview_seconds = xd_effects_get_double(settings, effect, "PreviewTimeout");
    options->edge_flip_move = xd_effects_get_bool(settings, effect, "EdgeFlipMove");
    options->edge_flip_delay = xd_effects_get_int(settings, effect, "EdgeFlipDelay");
}

static void wall_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                     const xd_window_event *event) {
    wall_state *state = data;
    if ((event->kind == XD_WINDOW_EVENT_UNMAP || event->kind == XD_WINDOW_EVENT_DESTROY) &&
        event->view == state->carried)
        state->carried = nullptr;
}

const xd_effect_impl xd_effect_impl_desktop_wall = {
    .id = "DesktopWall",
    .enable = wall_enable,
    .disable = wall_disable,
    .configure = wall_configure,
    .window_event = wall_handle_window_event,
    .input = wall_handle_input,
};
