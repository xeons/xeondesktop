/* SPDX-License-Identifier: MIT
 * Ring Switcher (both builds): Compiz's ring plugin. Its key (Super+Tab)
 * shows the windows' live thumbnails (window mirrors, mirror.h) on an
 * ellipse over the darkened monitor under the pointer, the one picked at
 * the front, larger and brighter, with its title below; pressed again,
 * the ring turns to the next. Letting go of the modifiers (or Enter)
 * switches to that window; Esc goes back without switching. Alt+Tab
 * (switcher.c) stays as it was: this is another way, off by default.
 */
#include <math.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "../../core/alloc.h"
#include "effects.h"
#include "mirror.h"
#include "ring_model.h"
#include "scale_model.h"

static constexpr float DARKEN_ALPHA = 0.5f;
static constexpr double MAX_FRAME_MS = 100.0;
/* The title's box: space round the text, and below the front thumbnail. */
static constexpr int TITLE_PADDING = 8;
static constexpr int TITLE_GAP = 12;

typedef struct ring_options {
    double speed, timestep;
    char windows[512];
    bool darken, show_title;
    double ring_width, ring_height; /* shares of the monitor */
    int thumb_width, thumb_height;
    double min_scale, min_brightness;
    xd_effect_binding next, previous;
} ring_options;

typedef struct ring_window {
    xd_view *view;
    xd_mirror *mirror;
} ring_window;

typedef struct ring_state {
    xd_server *server;
    ring_options options;
    bool active;
    ring_window *windows; /* in focus order, the active window first */
    size_t count;
    long position;           /* the window picked, counted on round the ring */
    xd_scale_motion turning; /* x: the ring's turn, in degrees */
    double last_time;
    /* The monitor it shows on, and its destroy: unplugged, the switcher
     * ends (output.c frees its xd_output then). */
    struct wlr_output *output;
    struct wl_listener output_destroy;
    struct wlr_scene_tree *overlay;
    struct wlr_scene_rect *darken;
    struct wlr_scene_tree *thumbnail_tree;
    struct wlr_scene_buffer *title;
    xd_view *title_view; /* whose title the buffer shows */
    uint32_t held;       /* the modifiers that keep it open */
} ring_state;

static size_t ring_get_selected(const ring_state *state) {
    return xd_ring_step_index(0, (int)(state->position % (long)state->count), state->count);
}

/* The current desktop's windows, as Compiz's ring showed the current
 * viewport's. */
static bool ring_is_candidate(const ring_state *state, const xd_view *view) {
    return view->mapped && xd_desktops_is_view_shown(view) &&
           xd_effects_match_view(state->options.windows, view);
}

static void ring_close(ring_state *state) {
    for (size_t i = 0; i < state->count; i++)
        xd_mirror_destroy(state->windows[i].mirror);
    XD_FREE(&state->windows);
    state->count = 0;
    if (state->overlay != nullptr)
        wlr_scene_node_destroy(&state->overlay->node);
    state->overlay = nullptr;
    state->title = nullptr;
    state->title_view = nullptr;
    xd_unlisten(&state->output_destroy);
    state->output = nullptr;
    state->active = false;
}

static void ring_finish(ring_state *state, bool switch_to) {
    xd_view *picked = state->count > 0 ? state->windows[ring_get_selected(state)].view : nullptr;
    ring_close(state);
    if (switch_to && picked != nullptr && picked->mapped)
        xd_view_focus(picked);
    xd_effects_schedule_frames(state->server);
}

static void ring_handle_output_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    ring_state *state = XD_CONTAINER_OF(listener, ring_state, output_destroy);
    ring_finish(state, false);
}

static void ring_begin(ring_state *state, int step, uint32_t held) {
    xd_server *server = state->server;
    xd_output *output = xd_output_at(server, server->cursor->x, server->cursor->y);
    if (output == nullptr || server->locked)
        return;
    size_t count = 0;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        count += ring_is_candidate(state, view);
    }
    if (count == 0)
        return;
    state->windows = XD_NEW_ARRAY(ring_window, count);
    state->overlay = xd_effects_create_overlay(server);
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
    const float color[4] = {0, 0, 0, state->options.darken ? DARKEN_ALPHA : 0.0f};
    state->darken = wlr_scene_rect_create(state->overlay, box.width, box.height, color);
    wlr_scene_node_set_position(&state->darken->node, box.x, box.y);
    state->thumbnail_tree = wlr_scene_tree_create(state->overlay);
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (!ring_is_candidate(state, view))
            continue;
        ring_window *window = &state->windows[state->count++];
        window->view = view;
        window->mirror = xd_mirror_create(view, state->thumbnail_tree);
    }
    state->output = output->wlr_output;
    XD_LISTEN(&state->output->events.destroy, state, output_destroy, ring_handle_output_destroy);
    state->active = true;
    state->held = held;
    /* The ring starts on the active window and turns from there. */
    state->position = 0;
    state->turning = (xd_scale_motion){.scale = 1.0};
    state->position += step;
    state->last_time = 0;
    xd_effects_schedule_frames(server);
}

static void ring_step(ring_state *state, int step) {
    state->position += step;
    xd_effects_schedule_frames(state->server);
}

/* The picked window's title in a dark box, drawn when the pick changes. */
static void ring_draw_title(ring_state *state, xd_view *view) {
    xd_server *server = state->server;
    if (state->title_view == view && state->title != nullptr)
        return;
    state->title_view = view;
    const char *title = xd_view_title(view);
    if (title == nullptr || title[0] == '\0')
        title = xd_view_app_id(view);
    if (title == nullptr)
        title = "";
    double scale = (double)state->output->scale;
    xd_theme theme;
    xd_theme_init(&theme, server->config.style, &server->config.scheme, scale);
    xd_text_style style =
        xd_text(&theme, XD_FONT_CAPTION, (xd_rgba){1, 1, 1, 1}, XD_ALIGN_CENTER);
    style.ellipsize = true;
    /* Measured on a scratch surface first. */
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *measure = cairo_create(scratch);
    int text_width = xd_text_width(measure, title, &style);
    cairo_destroy(measure);
    cairo_surface_destroy(scratch);
    int padding = xd_px(scale, TITLE_PADDING);
    int max_width = xd_px(scale, state->options.thumb_width * 1.5);
    int width = XD_MIN(text_width, max_width) + 2 * padding;
    int height = xd_px(scale, 18) + 2 * padding;
    cairo_surface_t *surface = nullptr;
    struct wlr_buffer *buffer = xd_cairo_buffer_new(width, height, &surface);
    if (buffer == nullptr)
        return;
    cairo_t *cairo = cairo_create(surface);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0.75);
    xd_path_rounded(cairo, 0, 0, width, height, padding, padding, padding, padding);
    cairo_fill(cairo);
    xd_draw_text(cairo, (xd_rect){padding, padding, width - 2 * padding, height - 2 * padding},
                 title, &style);
    cairo_destroy(cairo);
    cairo_surface_flush(surface);
    if (state->title == nullptr)
        state->title = wlr_scene_buffer_create(state->overlay, nullptr);
    wlr_scene_buffer_set_buffer(state->title, buffer);
    double logical_width = ceil(width / scale), logical_height = ceil(height / scale);
    wlr_scene_buffer_set_dest_size(state->title, (int)logical_width, (int)logical_height);
    wlr_buffer_drop(buffer); /* the scene holds its own lock */
}

static void ring_handle_frame(xd_server *server, void *data, [[maybe_unused]] xd_output *output,
                              double now) {
    ring_state *state = data;
    if (!state->active)
        return;
    /* Its monitor turned off: nowhere to show it. */
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, state->output, &box);
    if (!state->output->enabled || wlr_box_empty(&box)) {
        ring_finish(state, false);
        return;
    }
    double milliseconds = state->last_time > 0 ? (now - state->last_time) * 1000.0 : 16.0;
    if (milliseconds <= 0)
        return;
    state->last_time = now;
    milliseconds = XD_MIN(milliseconds, MAX_FRAME_MS);
    size_t count = state->count;
    double degrees_each = 360.0 / (double)count;
    xd_scale_slot target = {.x = (double)state->position * degrees_each, .scale = 1.0};
    bool moving = xd_scale_step_motion(&state->turning, target, milliseconds, state->options.speed,
                                       state->options.timestep);

    const ring_options *options = &state->options;
    double ring_width = XD_MAX(box.width * options->ring_width - options->thumb_width, 0.0);
    double ring_height = XD_MAX(box.height * options->ring_height - options->thumb_height, 0.0);
    xd_effect_rect ring = {box.x + (box.width - ring_width) / 2,
                           box.y + (box.height - ring_height) / 2, ring_width, ring_height};
    xd_ring_item *items = XD_NEW_ARRAY(xd_ring_item, count);
    xd_ring_layout(count, state->turning.x / degrees_each, ring, options->min_scale,
                   options->min_brightness, items);
    /* Back to front, so the front thumbnail is drawn last. */
    size_t *order = XD_NEW_ARRAY(size_t, count);
    for (size_t i = 0; i < count; i++)
        order[i] = i;
    for (size_t i = 1; i < count; i++) {
        for (size_t j = i; j > 0 && items[order[j - 1]].depth > items[order[j]].depth; j--) {
            size_t swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }
    for (size_t k = 0; k < count; k++) {
        size_t i = order[k];
        ring_window *window = &state->windows[i];
        xd_mirror_update(window->mirror);
        struct wlr_box window_box = xd_mirror_get_box(window->mirror);
        double fit = XD_MIN(XD_MIN((double)options->thumb_width / XD_MAX(window_box.width, 1),
                                   (double)options->thumb_height / XD_MAX(window_box.height, 1)),
                            1.0);
        double scale = fit * items[i].scale;
        xd_mirror_place(window->mirror, items[i].x - window_box.width * scale / 2,
                        items[i].y - window_box.height * scale / 2, scale, scale);
        xd_mirror_set_opacity(window->mirror, (float)items[i].brightness);
        wlr_scene_node_raise_to_top(&xd_mirror_get_tree(window->mirror)->node);
    }
    size_t selected = ring_get_selected(state);
    if (options->show_title) {
        ring_draw_title(state, state->windows[selected].view);
        if (state->title != nullptr) {
            int title_x = (int)lround(ring.x + ring.width / 2 - state->title->dst_width / 2.0);
            int title_y = (int)lround(ring.y + ring.height + options->thumb_height / 2.0 + TITLE_GAP);
            title_y = XD_MIN(title_y, box.y + box.height - state->title->dst_height);
            wlr_scene_node_set_position(&state->title->node, title_x, title_y);
            wlr_scene_node_raise_to_top(&state->title->node);
        }
    }
    xd_free(items);
    xd_free(order);
    if (moving)
        xd_effects_schedule_frames(server);
    else
        state->last_time = 0;
}

static bool ring_handle_key(ring_state *state, const xd_effect_input *input) {
    bool next = xd_effects_binding_matches(&state->options.next, input);
    bool previous = !next && xd_effects_binding_matches(&state->options.previous, input);
    if (!state->active) {
        if (!next && !previous)
            return false;
        const xd_effect_binding *binding = next ? &state->options.next : &state->options.previous;
        /* What keeps it open: the binding's modifiers but Shift (for going
         * backwards, as Shift+Alt+Tab does). */
        uint32_t held = binding->modifiers & ~(uint32_t)XD_BINDING_SHIFT;
        ring_begin(state, next ? 1 : -1, held != 0 ? held : binding->modifiers);
        return state->active;
    }
    if (!input->pressed)
        return true;
    if (next || previous) {
        ring_step(state, next ? 1 : -1);
        return true;
    }
    switch (input->keysym) {
    case XKB_KEY_Escape:
        ring_finish(state, false);
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        ring_finish(state, true);
        break;
    case XKB_KEY_Right:
        ring_step(state, 1);
        break;
    case XKB_KEY_Left:
        ring_step(state, -1);
        break;
    case XKB_KEY_Tab: /* Shift+Tab too (effects.h) */
        ring_step(state, (input->modifiers & XD_BINDING_SHIFT) ? -1 : 1);
        break;
    default:
        break;
    }
    return true;
}

static bool ring_handle_input([[maybe_unused]] xd_server *server, void *data,
                              const xd_effect_input *input) {
    ring_state *state = data;
    switch (input->kind) {
    case XD_EFFECT_INPUT_KEY:
        return ring_handle_key(state, input);
    case XD_EFFECT_INPUT_MODIFIERS:
        /* Let go of the modifiers: to the window picked. */
        if (state->active && (input->modifiers & state->held) == 0)
            ring_finish(state, true);
        return false;
    case XD_EFFECT_INPUT_BUTTON:
        if (!state->active)
            return false;
        if (input->pressed && input->button == BTN_LEFT)
            ring_finish(state, true);
        return true;
    case XD_EFFECT_INPUT_AXIS:
        if (!state->active)
            return false;
        ring_step(state, input->delta > 0 ? 1 : -1);
        return true;
    case XD_EFFECT_INPUT_MOTION:
        return false;
    }
    return false;
}

static void ring_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                     const xd_window_event *event) {
    ring_state *state = data;
    if (!state->active ||
        (event->kind != XD_WINDOW_EVENT_UNMAP && event->kind != XD_WINDOW_EVENT_DESTROY))
        return;
    for (size_t i = 0; i < state->count; i++) {
        if (state->windows[i].view != event->view)
            continue;
        xd_mirror_destroy(state->windows[i].mirror);
        state->count--;
        for (size_t j = i; j < state->count; j++)
            state->windows[j] = state->windows[j + 1];
        if (state->count == 0)
            ring_finish(state, false);
        else
            state->title_view = nullptr;
        xd_effects_schedule_frames(state->server);
        return;
    }
}

static void *ring_enable(xd_server *server) {
    ring_state *state = XD_NEW(ring_state);
    state->server = server;
    return state;
}

static void ring_disable([[maybe_unused]] xd_server *server, void *data) {
    ring_state *state = data;
    if (state->active)
        ring_close(state);
    xd_free(state);
}

static void ring_configure([[maybe_unused]] xd_server *server, void *data,
                           const xd_effects_settings *settings, size_t effect) {
    ring_state *state = data;
    ring_options *options = &state->options;
    options->speed = xd_effects_get_double(settings, effect, "Speed");
    options->timestep = xd_effects_get_double(settings, effect, "Timestep");
    snprintf(options->windows, sizeof(options->windows), "%s",
             xd_effects_get_text(settings, effect, "Windows"));
    options->darken = xd_effects_get_bool(settings, effect, "DarkenBack");
    options->show_title = xd_effects_get_bool(settings, effect, "ShowTitle");
    options->ring_width = xd_effects_get_int(settings, effect, "RingWidth") / 100.0;
    options->ring_height = xd_effects_get_int(settings, effect, "RingHeight") / 100.0;
    options->thumb_width = xd_effects_get_int(settings, effect, "ThumbWidth");
    options->thumb_height = xd_effects_get_int(settings, effect, "ThumbHeight");
    options->min_scale = xd_effects_get_double(settings, effect, "MinScale");
    options->min_brightness = xd_effects_get_double(settings, effect, "MinBrightness");
    xd_effects_get_binding(settings, effect, "NextKey", &options->next);
    xd_effects_get_binding(settings, effect, "PreviousKey", &options->previous);
}

const xd_effect_impl xd_effect_impl_ring_switcher = {
    .id = "RingSwitcher",
    .enable = ring_enable,
    .disable = ring_disable,
    .configure = ring_configure,
    .window_event = ring_handle_window_event,
    .frame = ring_handle_frame,
    .input = ring_handle_input,
};
