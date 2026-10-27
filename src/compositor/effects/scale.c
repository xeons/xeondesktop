/* SPDX-License-Identifier: MIT
 * Scale (both builds): Compiz's window picker. On its key or screen
 * corner the windows glide from where they are into a grid of live
 * thumbnails (window mirrors, mirror.h) over the darkened desktop; the
 * one under the pointer is highlighted, and a click or Enter picks it:
 * every thumbnail glides back and the one picked comes to the front. Esc,
 * the key again or a click on no window puts them back as they were.
 *
 * The windows themselves are hidden meanwhile (their programs still get
 * frame events, so the thumbnails stay live), and the effect has the
 * keyboard and the pointer.
 */
#include <math.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "../../core/alloc.h"
#include "effects.h"
#include "mirror.h"
#include "scale_model.h"

/* The darkened background's opacity, and how long it takes to get there. */
static constexpr float DARKEN_ALPHA = 0.5f;
static constexpr double DARKEN_SECONDS = 0.25;
/* The longest step a frame takes, so a stall doesn't make it jump. */
static constexpr double MAX_FRAME_MS = 100.0;
/* The highlight round the thumbnail under the pointer. */
static constexpr int HIGHLIGHT_MARGIN = 4;
static constexpr size_t MAX_OUTPUTS = 16;

typedef struct scale_options {
    int spacing;
    double speed, timestep;
    float opacity; /* of the thumbnails not under the pointer */
    bool darken, all_outputs;
    char windows[512];
    xd_effect_binding initiate;
    uint32_t edges;
} scale_options;

typedef struct scale_thumbnail {
    xd_view *view; /* nullptr once it's gone */
    xd_mirror *mirror;
    xd_effect_rect box; /* the window's outer box, where it glides back to */
    xd_scale_slot slot;
    xd_scale_motion motion;
} scale_thumbnail;

typedef enum scale_phase : uint8_t {
    SCALE_PHASE_OFF,
    SCALE_PHASE_IN,  /* gliding into the grid, or there */
    SCALE_PHASE_OUT, /* gliding back */
} scale_phase;

typedef struct scale_state {
    xd_server *server;
    scale_options options;
    scale_phase phase;
    scale_thumbnail *thumbnails; /* bottom first, as drawn */
    size_t count;
    struct wlr_scene_tree *overlay;
    struct wlr_scene_rect *darken[MAX_OUTPUTS];
    size_t darken_count;
    double darken_level; /* 0..1 of DARKEN_ALPHA */
    struct wlr_scene_rect *highlight;
    struct wlr_scene_tree *thumbnail_tree;
    int hovered;     /* the thumbnail under the pointer, or -1 */
    size_t selected; /* the keyboard's, picked by Enter */
    xd_view *picked; /* comes to the front once back */
    double last_time;
    bool edge_armed; /* the pointer left the corner since it last fired */
} scale_state;

static bool scale_is_candidate(const scale_state *state, const xd_view *view) {
    return view->mapped && !view->minimized && xd_effects_match_view(state->options.windows, view);
}

static xd_effect_rect scale_get_rect(struct wlr_box box) {
    return (xd_effect_rect){box.x, box.y, box.width, box.height};
}

/* Lays out the thumbnails of the windows on one output in its work area. */
static void scale_layout_output(scale_state *state, xd_output *output) {
    struct wlr_box area = output->usable;
    size_t count = 0;
    for (size_t i = 0; i < state->count; i++)
        count += xd_output_at(state->server, state->thumbnails[i].box.x +
                                                 state->thumbnails[i].box.width / 2,
                              state->thumbnails[i].box.y + state->thumbnails[i].box.height / 2) ==
                 output;
    if (count == 0)
        return;
    xd_effect_rect *windows = XD_NEW_ARRAY(xd_effect_rect, count);
    xd_scale_slot *slots = XD_NEW_ARRAY(xd_scale_slot, count);
    size_t *indices = XD_NEW_ARRAY(size_t, count);
    size_t made = 0;
    /* Top window first: it gets the slot nearest it. */
    for (size_t i = state->count; i-- > 0;) {
        const scale_thumbnail *thumbnail = &state->thumbnails[i];
        if (xd_output_at(state->server, thumbnail->box.x + thumbnail->box.width / 2,
                         thumbnail->box.y + thumbnail->box.height / 2) != output)
            continue;
        windows[made] = thumbnail->box;
        indices[made++] = i;
    }
    xd_scale_layout(windows, count, scale_get_rect(area), state->options.spacing, slots);
    for (size_t i = 0; i < count; i++)
        state->thumbnails[indices[i]].slot = slots[i];
    xd_free(windows);
    xd_free(slots);
    xd_free(indices);
}

static void scale_set_darken(scale_state *state) {
    float alpha = state->options.darken ? DARKEN_ALPHA * (float)state->darken_level : 0.0f;
    const float color[4] = {0.0f, 0.0f, 0.0f, alpha};
    for (size_t i = 0; i < state->darken_count; i++)
        wlr_scene_rect_set_color(state->darken[i], color);
}

static void scale_begin(scale_state *state) {
    xd_server *server = state->server;
    if (state->phase != SCALE_PHASE_OFF || server->locked)
        return;
    xd_output *current = xd_output_at(server, server->cursor->x, server->cursor->y);
    if (current == nullptr)
        return;
    size_t count = 0;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        count += scale_is_candidate(state, view);
    }
    if (count == 0)
        return;
    state->thumbnails = XD_NEW_ARRAY(scale_thumbnail, count);
    state->overlay = xd_effects_create_overlay(server);
    XD_LIST_FOR_EACH(output, &server->outputs, xd_output, link) {
        if (state->darken_count == MAX_OUTPUTS || (!state->options.all_outputs && output != current))
            continue;
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
        const float clear[4] = {0};
        struct wlr_scene_rect *rect =
            wlr_scene_rect_create(state->overlay, box.width, box.height, clear);
        wlr_scene_node_set_position(&rect->node, box.x, box.y);
        state->darken[state->darken_count++] = rect;
    }
    const float highlight[4] = {0.25f, 0.25f, 0.25f, 0.25f};
    state->highlight = wlr_scene_rect_create(state->overlay, 1, 1, highlight);
    wlr_scene_node_set_enabled(&state->highlight->node, false);
    state->thumbnail_tree = wlr_scene_tree_create(state->overlay);

    /* The views are in focus order, front first: drawn the other way. */
    struct wl_list *link;
    for (link = server->views.prev; link != &server->views; link = link->prev) {
        xd_view *view = XD_CONTAINER_OF(link, xd_view, link);
        if (!scale_is_candidate(state, view))
            continue;
        struct wlr_box box = xd_view_outer_box(view);
        xd_output *output = xd_output_at(server, box.x + box.width / 2.0, box.y + box.height / 2.0);
        if (!state->options.all_outputs && output != current)
            continue;
        scale_thumbnail *thumbnail = &state->thumbnails[state->count++];
        thumbnail->view = view;
        thumbnail->box = scale_get_rect(box);
        thumbnail->mirror = xd_mirror_create(view, state->thumbnail_tree);
        thumbnail->motion = (xd_scale_motion){.x = box.x, .y = box.y, .scale = 1.0};
    }
    if (state->count == 0) {
        wlr_scene_node_destroy(&state->overlay->node);
        state->overlay = nullptr;
        state->darken_count = 0;
        XD_FREE(&state->thumbnails);
        return;
    }
    XD_LIST_FOR_EACH(output, &server->outputs, xd_output, link) {
        if (state->options.all_outputs || output == current)
            scale_layout_output(state, output);
    }
    for (size_t i = 0; i < state->count; i++)
        wlr_scene_node_set_enabled(&state->thumbnails[i].view->tree->node, false);
    state->phase = SCALE_PHASE_IN;
    state->hovered = -1;
    state->selected = state->count - 1; /* the front window */
    state->picked = nullptr;
    state->darken_level = 0;
    state->last_time = 0;
    /* The picker has the pointer: no program's cursor or hover. */
    wlr_seat_pointer_clear_focus(server->seat);
    xd_cursor_set(server, "default");
    xd_effects_schedule_frames(server);
}

/* Back where they were: the windows are shown again, the one picked in
 * front. */
static void scale_finish(scale_state *state) {
    xd_server *server = state->server;
    for (size_t i = 0; i < state->count; i++) {
        scale_thumbnail *thumbnail = &state->thumbnails[i];
        if (thumbnail->view != nullptr && thumbnail->view->mapped && !thumbnail->view->minimized)
            wlr_scene_node_set_enabled(&thumbnail->view->tree->node, true);
        xd_mirror_destroy(thumbnail->mirror);
    }
    XD_FREE(&state->thumbnails);
    state->count = 0;
    if (state->overlay != nullptr)
        wlr_scene_node_destroy(&state->overlay->node);
    state->overlay = nullptr;
    state->darken_count = 0;
    state->phase = SCALE_PHASE_OFF;
    xd_view *picked = state->picked;
    state->picked = nullptr;
    if (picked != nullptr && picked->mapped)
        xd_view_focus(picked);
    xd_input_pointer_moved(server);
}

static void scale_end(scale_state *state, xd_view *picked) {
    if (state->phase != SCALE_PHASE_IN)
        return;
    state->phase = SCALE_PHASE_OUT;
    state->picked = picked;
    wlr_scene_node_set_enabled(&state->highlight->node, false);
    for (size_t i = 0; i < state->count; i++) {
        if (state->thumbnails[i].view == picked && picked != nullptr)
            wlr_scene_node_raise_to_top(&xd_mirror_get_tree(state->thumbnails[i].mirror)->node);
    }
    xd_effects_schedule_frames(state->server);
}

static void scale_pick(scale_state *state, size_t index) {
    if (index < state->count)
        scale_end(state, state->thumbnails[index].view);
}

static void scale_show_highlight(scale_state *state) {
    int shown = state->hovered;
    if (shown < 0 || state->phase != SCALE_PHASE_IN) {
        wlr_scene_node_set_enabled(&state->highlight->node, false);
        return;
    }
    const scale_thumbnail *thumbnail = &state->thumbnails[shown];
    const xd_scale_motion *motion = &thumbnail->motion;
    wlr_scene_node_set_enabled(&state->highlight->node, true);
    wlr_scene_node_set_position(&state->highlight->node,
                                (int)lround(motion->x) - HIGHLIGHT_MARGIN,
                                (int)lround(motion->y) - HIGHLIGHT_MARGIN);
    wlr_scene_rect_set_size(state->highlight,
                            (int)lround(thumbnail->box.width * motion->scale) + 2 * HIGHLIGHT_MARGIN,
                            (int)lround(thumbnail->box.height * motion->scale) +
                                2 * HIGHLIGHT_MARGIN);
}

static void scale_update_hovered(scale_state *state, double x, double y) {
    if (state->count == 0)
        return;
    xd_effect_rect *windows = XD_NEW_ARRAY(xd_effect_rect, state->count);
    xd_scale_motion *motions = XD_NEW_ARRAY(xd_scale_motion, state->count);
    for (size_t i = 0; i < state->count; i++) {
        windows[i] = state->thumbnails[i].box;
        motions[i] = state->thumbnails[i].motion;
    }
    int hovered = xd_scale_find_thumbnail_at(windows, motions, state->count, x, y);
    xd_free(windows);
    xd_free(motions);
    if (hovered >= 0)
        state->selected = (size_t)hovered;
    if (hovered != state->hovered) {
        state->hovered = hovered;
        xd_effects_schedule_frames(state->server);
    }
}

/* The arrow keys move the keyboard's selection, as Compiz's did. */
static void scale_move_selection(scale_state *state, int dx, int dy) {
    xd_effect_rect *windows = XD_NEW_ARRAY(xd_effect_rect, state->count);
    xd_scale_slot *slots = XD_NEW_ARRAY(xd_scale_slot, state->count);
    for (size_t i = 0; i < state->count; i++) {
        windows[i] = state->thumbnails[i].box;
        slots[i] = state->thumbnails[i].slot;
    }
    state->selected = xd_scale_find_neighbour(windows, slots, state->count, state->selected, dx, dy);
    state->hovered = (int)state->selected;
    xd_free(windows);
    xd_free(slots);
    xd_effects_schedule_frames(state->server);
}

static void scale_handle_frame(xd_server *server, void *data, [[maybe_unused]] xd_output *output,
                               double now) {
    scale_state *state = data;
    if (state->phase == SCALE_PHASE_OFF)
        return;
    double milliseconds = state->last_time > 0 ? (now - state->last_time) * 1000.0 : 16.0;
    if (milliseconds <= 0)
        return; /* the other outputs' frames at the same moment */
    milliseconds = XD_MIN(milliseconds, MAX_FRAME_MS);
    state->last_time = now;

    bool moving = false;
    for (size_t i = 0; i < state->count; i++) {
        scale_thumbnail *thumbnail = &state->thumbnails[i];
        xd_scale_slot target = thumbnail->slot;
        if (state->phase == SCALE_PHASE_OUT)
            target = (xd_scale_slot){thumbnail->box.x, thumbnail->box.y, 1.0};
        moving = xd_scale_step_motion(&thumbnail->motion, target, milliseconds,
                                      state->options.speed, state->options.timestep) ||
                 moving;
        xd_mirror_update(thumbnail->mirror);
        const xd_scale_motion *motion = &thumbnail->motion;
        xd_mirror_place(thumbnail->mirror, motion->x, motion->y, motion->scale, motion->scale);
        bool full = state->phase == SCALE_PHASE_OUT || (int)i == state->hovered;
        xd_mirror_set_opacity(thumbnail->mirror, full ? 1.0f : state->options.opacity);
    }
    double darken_step = milliseconds / 1000.0 / DARKEN_SECONDS;
    double darken_goal = state->phase == SCALE_PHASE_IN ? 1.0 : 0.0;
    if (state->darken_level != darken_goal) {
        state->darken_level = darken_goal > state->darken_level
                                  ? XD_MIN(state->darken_level + darken_step, 1.0)
                                  : XD_MAX(state->darken_level - darken_step, 0.0);
        moving = true;
    }
    scale_set_darken(state);
    scale_show_highlight(state);

    /* The windows are hidden: their programs hear of the frame from us. */
    struct timespec when;
    clock_gettime(CLOCK_MONOTONIC, &when);
    for (size_t i = 0; i < state->count; i++)
        xd_mirror_send_frame_done(state->thumbnails[i].mirror, &when);

    if (state->phase == SCALE_PHASE_OUT && !moving) {
        scale_finish(state);
        return;
    }
    if (moving)
        xd_effects_schedule_frames(server);
    else
        state->last_time = 0; /* idle: the next frame starts afresh */
}

/* The screen edges and corners the pointer at (x, y) is pushed into. */
static uint32_t scale_get_edges_at(xd_server *server, double x, double y) {
    xd_output *output = xd_output_at(server, x, y);
    if (output == nullptr)
        return 0;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
    bool left = x < box.x + 1, right = x >= box.x + box.width - 1;
    bool top = y < box.y + 1, bottom = y >= box.y + box.height - 1;
    if (top && left)
        return XD_SCREEN_EDGE_TOP_LEFT;
    if (top && right)
        return XD_SCREEN_EDGE_TOP_RIGHT;
    if (bottom && left)
        return XD_SCREEN_EDGE_BOTTOM_LEFT;
    if (bottom && right)
        return XD_SCREEN_EDGE_BOTTOM_RIGHT;
    return (left ? XD_SCREEN_EDGE_LEFT : 0u) | (right ? XD_SCREEN_EDGE_RIGHT : 0u) |
           (top ? XD_SCREEN_EDGE_TOP : 0u) | (bottom ? XD_SCREEN_EDGE_BOTTOM : 0u);
}

static bool scale_handle_key(scale_state *state, const xd_effect_input *input) {
    if (state->phase == SCALE_PHASE_OFF) {
        if (!xd_effects_binding_matches(&state->options.initiate, input))
            return false;
        scale_begin(state);
        return state->phase != SCALE_PHASE_OFF;
    }
    /* While it shows, the picker has the keyboard. */
    if (!input->pressed || state->phase != SCALE_PHASE_IN)
        return true;
    if (xd_effects_binding_matches(&state->options.initiate, input) ||
        input->keysym == XKB_KEY_Escape) {
        scale_end(state, nullptr);
        return true;
    }
    switch (input->keysym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        scale_pick(state, state->selected);
        break;
    case XKB_KEY_Left:
        scale_move_selection(state, -1, 0);
        break;
    case XKB_KEY_Right:
        scale_move_selection(state, 1, 0);
        break;
    case XKB_KEY_Up:
        scale_move_selection(state, 0, -1);
        break;
    case XKB_KEY_Down:
        scale_move_selection(state, 0, 1);
        break;
    default:
        break;
    }
    return true;
}

static bool scale_handle_input(xd_server *server, void *data, const xd_effect_input *input) {
    scale_state *state = data;
    switch (input->kind) {
    case XD_EFFECT_INPUT_KEY:
        return scale_handle_key(state, input);
    case XD_EFFECT_INPUT_MOTION: {
        if (state->phase != SCALE_PHASE_OFF) {
            scale_update_hovered(state, input->x, input->y);
            return true;
        }
        bool at_edge = (scale_get_edges_at(server, input->x, input->y) & state->options.edges) != 0;
        if (at_edge && state->edge_armed && server->mode == XD_CURSOR_PASSTHROUGH &&
            server->seat->pointer_state.button_count == 0) {
            state->edge_armed = false;
            scale_begin(state);
        } else if (!at_edge) {
            state->edge_armed = true;
        }
        return false;
    }
    case XD_EFFECT_INPUT_BUTTON:
        if (state->phase == SCALE_PHASE_OFF)
            return false;
        if (input->pressed && input->button == BTN_LEFT && state->phase == SCALE_PHASE_IN) {
            scale_update_hovered(state, input->x, input->y);
            if (state->hovered >= 0)
                scale_pick(state, (size_t)state->hovered);
            else
                scale_end(state, nullptr);
        }
        return true;
    case XD_EFFECT_INPUT_AXIS:
        return state->phase != SCALE_PHASE_OFF;
    case XD_EFFECT_INPUT_MODIFIERS:
        return false;
    }
    return false;
}

static void scale_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                      const xd_window_event *event) {
    scale_state *state = data;
    if (event->kind != XD_WINDOW_EVENT_UNMAP && event->kind != XD_WINDOW_EVENT_DESTROY &&
        event->kind != XD_WINDOW_EVENT_MINIMIZE)
        return;
    /* A window closing meanwhile leaves the picker. */
    for (size_t i = 0; i < state->count; i++) {
        scale_thumbnail *thumbnail = &state->thumbnails[i];
        if (thumbnail->view != event->view)
            continue;
        xd_mirror_destroy(thumbnail->mirror);
        if (state->picked == thumbnail->view)
            state->picked = nullptr;
        state->count--;
        for (size_t j = i; j < state->count; j++)
            state->thumbnails[j] = state->thumbnails[j + 1];
        state->hovered = -1;
        state->selected = state->count > 0 ? state->count - 1 : 0;
        if (state->count == 0)
            scale_finish(state);
        xd_effects_schedule_frames(state->server);
        return;
    }
}

static void *scale_enable(xd_server *server) {
    scale_state *state = XD_NEW(scale_state);
    state->server = server;
    state->hovered = -1;
    state->edge_armed = true;
    return state;
}

static void scale_disable([[maybe_unused]] xd_server *server, void *data) {
    scale_state *state = data;
    if (state->phase != SCALE_PHASE_OFF) {
        state->picked = nullptr;
        scale_finish(state);
    }
    xd_free(state);
}

static void scale_configure([[maybe_unused]] xd_server *server, void *data,
                            const xd_effects_settings *settings, size_t effect) {
    scale_state *state = data;
    scale_options *options = &state->options;
    options->spacing = xd_effects_get_int(settings, effect, "Spacing");
    options->speed = xd_effects_get_double(settings, effect, "Speed");
    options->timestep = xd_effects_get_double(settings, effect, "Timestep");
    int opacity = xd_effects_get_int(settings, effect, "Opacity");
    options->opacity = (float)opacity / 100.0f;
    options->darken = xd_effects_get_bool(settings, effect, "DarkenBack");
    options->all_outputs = xd_effects_get_choice(settings, effect, "Outputs") == 1;
    snprintf(options->windows, sizeof(options->windows), "%s",
             xd_effects_get_text(settings, effect, "Windows"));
    xd_effects_get_binding(settings, effect, "InitiateKey", &options->initiate);
    options->edges = xd_effects_get_edges(settings, effect, "InitiateEdge");
}

const xd_effect_impl xd_effect_impl_scale = {
    .id = "Scale",
    .enable = scale_enable,
    .disable = scale_disable,
    .configure = scale_configure,
    .window_event = scale_handle_window_event,
    .frame = scale_handle_frame,
    .input = scale_handle_input,
};
