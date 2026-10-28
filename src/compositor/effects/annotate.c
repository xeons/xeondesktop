/* SPDX-License-Identifier: MIT
 * Annotate (both builds): Compiz's annotate plugin. Windows+Alt and the
 * left button drag a line over the screen, in the fill colour with an
 * outline in the stroke colour; the right button rubs out; Windows+Alt+K
 * clears it all. The drawing is a picture per monitor, drawn with cairo,
 * above every window and panel (below the lock screen), which takes no
 * input: the windows keep the keyboard focus and the pointer between
 * strokes.
 *
 * Each monitor keeps the strokes finished so far (`done`) apart from the
 * picture shown, which is that plus the stroke being drawn, drawn whole
 * each time so its outline stays under its own line.
 */
#include <math.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_output.h>

#include "../../core/alloc.h"
#include "effects.h"

typedef struct annotate_options {
    uint32_t fill, stroke; /* 0xRRGGBBAA */
    double line_width, stroke_width, erase_width;
    xd_effect_binding draw, erase, clear;
} annotate_options;

typedef enum annotate_tool : uint8_t {
    ANNOTATE_TOOL_NONE,
    ANNOTATE_TOOL_DRAW,
    ANNOTATE_TOOL_ERASE,
} annotate_tool;

typedef struct annotate_state annotate_state;

/* A monitor's drawing. */
typedef struct annotate_canvas {
    struct wl_list link; /* annotate_state.canvases */
    annotate_state *state;
    struct wlr_output *output;
    struct wlr_box box; /* layout units */
    double scale;       /* device px per unit */
    struct wlr_buffer *buffer; /* shown; we hold it */
    cairo_surface_t *shown;    /* the buffer's pixels */
    cairo_surface_t *done;     /* the strokes finished */
    struct wlr_scene_buffer *node;
    struct wl_listener destroy;
} annotate_canvas;

typedef struct annotate_point {
    double x, y; /* layout units */
} annotate_point;

struct annotate_state {
    xd_server *server;
    annotate_options options;
    struct wlr_scene_tree *overlay;
    struct wl_list canvases; /* annotate_canvas.link */
    annotate_tool tool;
    uint32_t button; /* the one that started the stroke */
    annotate_point *points; /* the stroke being drawn */
    size_t point_count, point_capacity;
};

/* The pictures are only pictures: the pointer reaches what's below. */
static bool annotate_reject_input([[maybe_unused]] struct wlr_scene_buffer *buffer,
                                  [[maybe_unused]] double *sx, [[maybe_unused]] double *sy) {
    return false;
}

static void annotate_free_canvas(annotate_canvas *canvas) {
    wlr_scene_node_destroy(&canvas->node->node);
    wlr_buffer_drop(canvas->buffer);
    cairo_surface_destroy(canvas->done);
    xd_unlisten(&canvas->destroy);
    wl_list_remove(&canvas->link);
    xd_free(canvas);
}

static void annotate_handle_output_destroy(struct wl_listener *listener,
                                           [[maybe_unused]] void *data) {
    annotate_free_canvas(XD_CONTAINER_OF(listener, annotate_canvas, destroy));
}

/* The canvas's pictures at the monitor's box and scale now: made, or made
 * again when its mode, scale or place changed, the strokes so far kept
 * where they were on it. False if there's no memory for them. */
static bool annotate_fit_canvas(annotate_canvas *canvas) {
    struct wlr_box box;
    wlr_output_layout_get_box(canvas->state->server->output_layout, canvas->output, &box);
    double scale = (double)canvas->output->scale;
    if (canvas->buffer != nullptr && wlr_box_equal(&box, &canvas->box) && scale == canvas->scale)
        return true;
    if (wlr_box_empty(&box))
        return canvas->buffer != nullptr; /* turned off: as it was, till it's back */
    double width_px = ceil(box.width * scale), height_px = ceil(box.height * scale);
    int width = (int)width_px, height = (int)height_px;
    cairo_surface_t *shown = nullptr;
    struct wlr_buffer *buffer = xd_cairo_buffer_new(width, height, &shown);
    if (buffer == nullptr)
        return false;
    cairo_surface_t *done = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (canvas->done != nullptr) {
        cairo_t *cairo = cairo_create(done);
        cairo_scale(cairo, scale / canvas->scale, scale / canvas->scale);
        cairo_set_source_surface(cairo, canvas->done, 0, 0);
        cairo_paint(cairo);
        cairo_destroy(cairo);
        cairo_surface_destroy(canvas->done);
    }
    if (canvas->buffer != nullptr)
        wlr_buffer_drop(canvas->buffer);
    canvas->buffer = buffer;
    canvas->shown = shown;
    canvas->done = done;
    canvas->box = box;
    canvas->scale = scale;
    wlr_scene_buffer_set_dest_size(canvas->node, box.width, box.height);
    wlr_scene_node_set_position(&canvas->node->node, box.x, box.y);
    return true;
}

/* The drawing on the monitor at (x, y), made if there's none yet. */
static annotate_canvas *annotate_get_canvas(annotate_state *state, double x, double y) {
    xd_server *server = state->server;
    struct wlr_output *output = wlr_output_layout_output_at(server->output_layout, x, y);
    if (output == nullptr)
        return nullptr;
    annotate_canvas *canvas;
    wl_list_for_each(canvas, &state->canvases, link) {
        if (canvas->output == output)
            return canvas;
    }
    canvas = XD_NEW(annotate_canvas);
    canvas->state = state;
    canvas->output = output;
    if (state->overlay == nullptr)
        state->overlay = xd_effects_create_overlay(server);
    canvas->node = wlr_scene_buffer_create(state->overlay, nullptr);
    canvas->node->point_accepts_input = annotate_reject_input;
    if (!annotate_fit_canvas(canvas)) {
        wlr_scene_node_destroy(&canvas->node->node);
        xd_free(canvas);
        return nullptr;
    }
    XD_LISTEN(&output->events.destroy, canvas, destroy, annotate_handle_output_destroy);
    wl_list_insert(&state->canvases, &canvas->link);
    return canvas;
}

static void annotate_set_color(cairo_t *cairo, uint32_t color) {
    cairo_set_source_rgba(cairo, (color >> 24 & 0xFF) / 255.0, (color >> 16 & 0xFF) / 255.0,
                          (color >> 8 & 0xFF) / 255.0, (color & 0xFF) / 255.0);
}

/* The stroke's points as a path in the canvas's pixels. */
static void annotate_trace(const annotate_state *state, const annotate_canvas *canvas,
                           cairo_t *cairo) {
    cairo_new_path(cairo);
    for (size_t i = 0; i < state->point_count; i++) {
        double x = (state->points[i].x - canvas->box.x) * canvas->scale;
        double y = (state->points[i].y - canvas->box.y) * canvas->scale;
        if (i == 0)
            cairo_move_to(cairo, x, y);
        else
            cairo_line_to(cairo, x, y);
    }
    /* A click alone leaves a dot. */
    if (state->point_count == 1)
        cairo_rel_line_to(cairo, 0, 0);
}

/* What's shown: the strokes finished and the one being drawn. */
static void annotate_redraw(annotate_state *state, annotate_canvas *canvas) {
    const annotate_options *options = &state->options;
    cairo_t *cairo = cairo_create(canvas->shown);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cairo, canvas->done, 0, 0);
    cairo_paint(cairo);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    if (state->tool == ANNOTATE_TOOL_DRAW && state->point_count > 0) {
        cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
        annotate_trace(state, canvas, cairo);
        if (options->stroke_width > 0) {
            cairo_set_line_width(cairo,
                                 (options->line_width + 2 * options->stroke_width) * canvas->scale);
            annotate_set_color(cairo, options->stroke);
            cairo_stroke_preserve(cairo);
        }
        cairo_set_line_width(cairo, options->line_width * canvas->scale);
        annotate_set_color(cairo, options->fill);
        cairo_stroke(cairo);
    }
    cairo_destroy(cairo);
    cairo_surface_flush(canvas->shown);
    wlr_scene_buffer_set_buffer(canvas->node, canvas->buffer);
}

/* Rubs the stroke so far out of the strokes finished. */
static void annotate_erase(annotate_state *state, annotate_canvas *canvas) {
    cairo_t *cairo = cairo_create(canvas->done);
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cairo, state->options.erase_width * canvas->scale);
    annotate_trace(state, canvas, cairo);
    cairo_stroke(cairo);
    cairo_destroy(cairo);
    cairo_surface_flush(canvas->done);
}

/* The stroke so far, onto whichever monitors it crosses. */
static void annotate_update(annotate_state *state) {
    annotate_canvas *canvas;
    wl_list_for_each(canvas, &state->canvases, link) {
        if (state->tool == ANNOTATE_TOOL_ERASE)
            annotate_erase(state, canvas);
        annotate_redraw(state, canvas);
    }
}

/* Before each frame of a monitor: its drawing follows a change of its
 * mode, scale or place. */
static void annotate_handle_frame([[maybe_unused]] xd_server *server, void *data,
                                  xd_output *output, [[maybe_unused]] double now) {
    annotate_state *state = data;
    annotate_canvas *canvas;
    wl_list_for_each(canvas, &state->canvases, link) {
        if (canvas->output != output->wlr_output)
            continue;
        struct wlr_buffer *before = canvas->buffer;
        if (annotate_fit_canvas(canvas) && canvas->buffer != before)
            annotate_redraw(state, canvas);
        return;
    }
}

static void annotate_add_point(annotate_state *state, double x, double y) {
    if (state->point_count == state->point_capacity) {
        state->point_capacity = XD_MAX(state->point_capacity * 2, 64);
        state->points =
            xd_realloc_array(state->points, state->point_capacity, sizeof(*state->points));
    }
    state->points[state->point_count++] = (annotate_point){x, y};
    annotate_get_canvas(state, x, y);
}

static void annotate_end_stroke(annotate_state *state) {
    if (state->tool == ANNOTATE_TOOL_DRAW) {
        /* What's shown is now finished. */
        annotate_canvas *canvas;
        wl_list_for_each(canvas, &state->canvases, link) {
            cairo_t *cairo = cairo_create(canvas->done);
            cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_surface(cairo, canvas->shown, 0, 0);
            cairo_paint(cairo);
            cairo_destroy(cairo);
        }
    }
    state->tool = ANNOTATE_TOOL_NONE;
    state->point_count = 0;
    xd_input_pointer_moved(state->server); /* the windows have the pointer again */
}

static void annotate_clear(annotate_state *state) {
    annotate_canvas *canvas, *next;
    wl_list_for_each_safe(canvas, next, &state->canvases, link) {
        annotate_free_canvas(canvas);
    }
    if (state->overlay != nullptr)
        wlr_scene_node_destroy(&state->overlay->node);
    state->overlay = nullptr;
    state->tool = ANNOTATE_TOOL_NONE;
    state->point_count = 0;
}

static bool annotate_handle_input(xd_server *server, void *data, const xd_effect_input *input) {
    annotate_state *state = data;
    switch (input->kind) {
    case XD_EFFECT_INPUT_KEY:
        if (!xd_effects_binding_matches(&state->options.clear, input))
            return false;
        annotate_clear(state);
        return true;
    case XD_EFFECT_INPUT_BUTTON:
        if (state->tool != ANNOTATE_TOOL_NONE) {
            if (!input->pressed && input->button == state->button)
                annotate_end_stroke(state);
            return true; /* every button while it draws */
        }
        if (xd_effects_binding_matches(&state->options.draw, input))
            state->tool = ANNOTATE_TOOL_DRAW;
        else if (xd_effects_binding_matches(&state->options.erase, input))
            state->tool = ANNOTATE_TOOL_ERASE;
        else
            return false;
        state->button = input->button;
        state->point_count = 0;
        wlr_seat_pointer_clear_focus(server->seat);
        xd_cursor_set(server, "crosshair");
        annotate_add_point(state, input->x, input->y);
        annotate_update(state);
        return true;
    case XD_EFFECT_INPUT_MOTION:
        if (state->tool == ANNOTATE_TOOL_NONE)
            return false;
        annotate_add_point(state, input->x, input->y);
        annotate_update(state);
        return true;
    case XD_EFFECT_INPUT_AXIS:
    case XD_EFFECT_INPUT_MODIFIERS:
        return false;
    }
    return false;
}

static void *annotate_enable(xd_server *server) {
    annotate_state *state = XD_NEW(annotate_state);
    state->server = server;
    wl_list_init(&state->canvases);
    return state;
}

static void annotate_disable([[maybe_unused]] xd_server *server, void *data) {
    annotate_state *state = data;
    annotate_clear(state);
    xd_free(state->points);
    xd_free(state);
}

static void annotate_configure([[maybe_unused]] xd_server *server, void *data,
                               const xd_effects_settings *settings, size_t effect) {
    annotate_state *state = data;
    annotate_options *options = &state->options;
    options->fill = xd_effects_get_color(settings, effect, "FillColor");
    options->stroke = xd_effects_get_color(settings, effect, "StrokeColor");
    options->line_width = xd_effects_get_double(settings, effect, "LineWidth");
    options->stroke_width = xd_effects_get_double(settings, effect, "StrokeWidth");
    options->erase_width = xd_effects_get_double(settings, effect, "EraseWidth");
    xd_effects_get_binding(settings, effect, "DrawButton", &options->draw);
    xd_effects_get_binding(settings, effect, "EraseButton", &options->erase);
    xd_effects_get_binding(settings, effect, "ClearKey", &options->clear);
}

const xd_effect_impl xd_effect_impl_annotate = {
    .id = "Annotate",
    .enable = annotate_enable,
    .disable = annotate_disable,
    .configure = annotate_configure,
    .frame = annotate_handle_frame,
    .input = annotate_handle_input,
};
