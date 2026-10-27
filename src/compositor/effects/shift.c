/* SPDX-License-Identifier: MIT
 * Shift Switcher (xd-compositor-fx): Compiz's shift plugin. Its key shows
 * the desktop's windows (minimized ones too), most recently used first,
 * as live pictures in perspective over the dimmed desktop: a cover flow,
 * the chosen one facing the viewer and the others turned to the sides,
 * or a stack flipping over (viewport_model.h's xd_shift_get_slot,
 * unit-tested). The key again, or the arrows, choose the next or the one
 * before; letting go of the keys (or Enter, or a click) activates it,
 * Escape changes nothing.
 *
 * A whole-output effect (glfx.h): the desktop behind is a scene capture,
 * and each window one of its own, at about the size it's drawn.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <time.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../../core/alloc.h"
#include "glfx.h"
#include "viewport_model.h"

static constexpr int TICK_MS = 16;
enum { MAX_WINDOWS = 32 };
/* Fading in, this long. */
static constexpr double APPEAR_SECONDS = 0.15;
/* The reflection's strength, and its gap below the window. */
static constexpr float REFLECTION_ALPHA = 0.25f;

typedef struct shift_options {
    xd_effect_binding initiate;
    xd_shift_mode mode;
    double size;       /* 0..1 of the screen */
    double brightness; /* the background's, 0..1 */
    bool reflection;
    double animation_seconds;
} shift_options;

typedef struct shift_state {
    xd_server *server;
    shift_options options;
    bool active;
    xd_output_effect *effect;
    struct wl_event_source *timer;
    xd_view *views[MAX_WINDOWS];
    size_t count;
    size_t selected;
    double position; /* the place in front, moving towards `selected` */
    double start_time, last_time;
    double appear; /* 0..1 */
    xd_glfx_scene_capture *background_capture;
    xd_glfx_texture background;
    bool background_captured;
    xd_glfx_scene_capture *captures[MAX_WINDOWS];
    xd_glfx_texture textures[MAX_WINDOWS];
    bool captured[MAX_WINDOWS];
} shift_state;

static double shift_get_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void shift_schedule_frames(xd_server *server) {
    XD_LIST_FOR_EACH(output, &server->outputs, xd_output, link) {
        if (output->wlr_output->enabled)
            wlr_output_schedule_frame(output->wlr_output);
    }
}

/* ---- drawing ---------------------------------------------------------------------- */

static bool shift_shows_in_background(const xd_view *view, [[maybe_unused]] void *data) {
    return !view->minimized && xd_desktops_is_view_shown(view);
}

static bool shift_is_view(const xd_view *view, void *data) {
    return view == data;
}

static double shift_get_scale(const shift_state *state, const xd_view *view,
                              struct wlr_box box) {
    struct wlr_box outer = xd_view_outer_box(view);
    return xd_shift_get_scale(outer.width, outer.height, box.width, box.height,
                              state->options.size);
}

static void shift_capture(shift_state *state, xd_output *output) {
    xd_server *server = state->server;
    struct wlr_box box = {0};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
    double output_scale = (double)output->wlr_output->scale;
    if (state->background_capture == nullptr)
        state->background_capture = xd_glfx_create_scene_capture(server);
    /* The background is dimmed: half size will do. */
    state->background_captured =
        xd_glfx_capture_scene(state->background_capture, box, output_scale * 0.5, true,
                              shift_shows_in_background, nullptr, &state->background);
    for (size_t i = 0; i < state->count; i++) {
        state->captured[i] = false;
        xd_view *view = state->views[i];
        double position = (double)i - state->position;
        if (view == nullptr || xd_shift_get_slot(state->options.mode, position, box.width,
                                                 box.height)
                                       .alpha <= 0)
            continue;
        if (state->captures[i] == nullptr)
            state->captures[i] = xd_glfx_create_scene_capture(server);
        state->captured[i] = xd_glfx_capture_scene(
            state->captures[i], xd_view_outer_box(view),
            output_scale * shift_get_scale(state, view, box), false, shift_is_view, view,
            &state->textures[i]);
    }
}

static void shift_project(const xd_camera *camera, const xd_vec3 corners[4], float clip[4][4]) {
    for (int i = 0; i < 4; i++)
        xd_camera_project(camera, corners[i], clip[i]);
}

static void shift_draw(xd_output_effect *effect, [[maybe_unused]] xd_output *output,
                       const xd_glfx_draw *draw) {
    shift_state *state = effect->data;
    struct wlr_box box = {0, 0, draw->bounds.width, draw->bounds.height};
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    float appear = (float)state->appear;
    if (state->background_captured) {
        const float whole[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}};
        float brightness = 1.0f - (1.0f - (float)state->options.brightness) * appear;
        xd_glfx_draw_quad(draw, &state->background, whole, 1.0f, brightness);
    }
    xd_camera camera;
    xd_camera_init(&camera, box.width, box.height);
    /* The farthest first, the one in front last. */
    size_t order[MAX_WINDOWS];
    for (size_t i = 0; i < state->count; i++)
        order[i] = i;
    for (size_t i = 1; i < state->count; i++) {
        for (size_t j = i; j > 0 && fabs((double)order[j - 1] - state->position) <
                                        fabs((double)order[j] - state->position);
             j--) {
            size_t swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }
    for (size_t k = 0; k < state->count; k++) {
        size_t i = order[k];
        xd_view *view = state->views[i];
        if (view == nullptr || !state->captured[i])
            continue;
        xd_shift_slot slot = xd_shift_get_slot(state->options.mode, (double)i - state->position,
                                               box.width, box.height);
        struct wlr_box outer = xd_view_outer_box(view);
        double scale = shift_get_scale(state, view, box);
        double width = outer.width * scale, height = outer.height * scale;
        xd_vec3 corners[4];
        xd_shift_get_corners(&slot, width, height, corners);
        float clip[4][4];
        float alpha = (float)slot.alpha * appear;
        if (state->options.reflection) {
            /* Mirrored below: its top edge on the window's bottom one, the
             * picture upside down. */
            xd_vec3 mirrored[4] = {
                {corners[3].x, corners[3].y + height, corners[3].z},
                {corners[2].x, corners[2].y + height, corners[2].z},
                corners[2],
                corners[3],
            };
            shift_project(&camera, mirrored, clip);
            xd_glfx_draw_quad(draw, &state->textures[i], clip, alpha * REFLECTION_ALPHA, 1.0f);
        }
        shift_project(&camera, corners, clip);
        xd_glfx_draw_quad(draw, &state->textures[i], clip, alpha, 1.0f);
    }
}

static const xd_output_effect_impl shift_output_impl = {
    .name = "Shift Switcher",
    .draw = shift_draw,
};

/* ---- switching -------------------------------------------------------------------- */

static void shift_release(shift_state *state) {
    xd_glfx_destroy_scene_capture(state->background_capture);
    state->background_capture = nullptr;
    state->background_captured = false;
    for (size_t i = 0; i < MAX_WINDOWS; i++) {
        xd_glfx_destroy_scene_capture(state->captures[i]);
        state->captures[i] = nullptr;
        state->captured[i] = false;
    }
}

/* Put away; `activate` the window chosen. */
static void shift_finish(shift_state *state, bool activate) {
    if (!state->active)
        return;
    state->active = false;
    xd_view *chosen = state->selected < state->count ? state->views[state->selected] : nullptr;
    if (state->effect != nullptr)
        xd_glfx_end_output(state->effect);
    state->effect = nullptr;
    shift_release(state);
    state->count = 0;
    if (activate && chosen != nullptr && chosen->mapped)
        xd_view_focus(chosen);
}

static bool shift_start(shift_state *state) {
    xd_server *server = state->server;
    state->count = 0;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (view->mapped && xd_desktops_is_view_shown(view) && state->count < MAX_WINDOWS)
            state->views[state->count++] = view;
    }
    if (state->count == 0)
        return false;
    state->effect = xd_glfx_begin_output(server, &shift_output_impl, state);
    if (state->effect == nullptr)
        return false;
    state->active = true;
    state->selected = 0;
    state->position = 0;
    state->appear = 0;
    state->start_time = state->last_time = shift_get_now();
    wl_event_source_timer_update(state->timer, TICK_MS);
    shift_schedule_frames(server);
    return true;
}

static void shift_step(shift_state *state, bool forwards) {
    if (state->count == 0)
        return;
    state->selected = forwards ? (state->selected + 1) % state->count
                               : (state->selected + state->count - 1) % state->count;
    shift_schedule_frames(state->server);
}

static int shift_tick(void *data) {
    shift_state *state = data;
    if (!state->active)
        return 0;
    double now = shift_get_now();
    double seconds = now - state->last_time;
    state->last_time = now;
    state->appear = fmin((now - state->start_time) / APPEAR_SECONDS, 1.0);
    /* Towards the chosen one, slowing as it nears: the last of the gap
     * goes in about the animation time. */
    double rate = fmin(seconds / fmax(state->options.animation_seconds / 4.0, 0.01), 1.0);
    state->position += ((double)state->selected - state->position) * rate;
    if (fabs((double)state->selected - state->position) < 0.001)
        state->position = (double)state->selected;
    shift_schedule_frames(state->server);
    wl_event_source_timer_update(state->timer, TICK_MS);
    return 0;
}

static bool shift_handle_key(shift_state *state, const xd_effect_input *input) {
    if (!input->pressed)
        return false;
    bool initiate = xd_effects_binding_matches(&state->options.initiate, input);
    if (!state->active) {
        if (!initiate)
            return false;
        /* At once the next window, as Alt+Tab. */
        if (shift_start(state) && state->count > 1)
            shift_step(state, true);
        return true;
    }
    switch (input->keysym) {
    case XKB_KEY_Right:
    case XKB_KEY_Down:
        shift_step(state, true);
        return true;
    case XKB_KEY_Left:
    case XKB_KEY_Up:
        shift_step(state, false);
        return true;
    case XKB_KEY_Escape:
        shift_finish(state, false);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        shift_finish(state, true);
        return true;
    default:
        break;
    }
    if (initiate) {
        shift_step(state, true);
        return true;
    }
    return false;
}

static bool shift_handle_input([[maybe_unused]] xd_server *server, void *data,
                               const xd_effect_input *input) {
    shift_state *state = data;
    switch (input->kind) {
    case XD_EFFECT_INPUT_KEY:
        return shift_handle_key(state, input);
    case XD_EFFECT_INPUT_MODIFIERS:
        /* Let go of the binding's modifiers: to the window in front. */
        if (state->active && !xd_effects_is_binding_held(&state->options.initiate, input))
            shift_finish(state, true);
        return false;
    case XD_EFFECT_INPUT_BUTTON:
        if (!state->active)
            return false;
        if (input->button == BTN_LEFT && !input->pressed)
            shift_finish(state, true);
        return true;
    case XD_EFFECT_INPUT_MOTION:
        /* The pointer is its while it shows. */
        return state->active;
    case XD_EFFECT_INPUT_AXIS:
        return false;
    }
    return false;
}

static void shift_handle_frame([[maybe_unused]] xd_server *server, void *data, xd_output *output,
                               [[maybe_unused]] double now) {
    shift_state *state = data;
    if (!state->active || state->effect == nullptr)
        return;
    shift_capture(state, output);
    xd_glfx_draw_output(state->effect, output);
}

static void shift_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                      const xd_window_event *event) {
    shift_state *state = data;
    if (!state->active ||
        (event->kind != XD_WINDOW_EVENT_UNMAP && event->kind != XD_WINDOW_EVENT_DESTROY))
        return;
    /* A window gone stays a gap no more: the ones after move up. */
    for (size_t i = 0; i < state->count; i++) {
        if (state->views[i] != event->view)
            continue;
        xd_glfx_destroy_scene_capture(state->captures[i]);
        for (size_t j = i; j + 1 < state->count; j++) {
            state->views[j] = state->views[j + 1];
            state->captures[j] = state->captures[j + 1];
            state->textures[j] = state->textures[j + 1];
            state->captured[j] = state->captured[j + 1];
        }
        state->count--;
        state->captures[state->count] = nullptr;
        state->captured[state->count] = false;
        if (state->selected > i || state->selected >= state->count)
            state->selected = state->selected > 0 ? state->selected - 1 : 0;
        break;
    }
    if (state->count == 0)
        shift_finish(state, false);
}

/* ---- the effect ------------------------------------------------------------------- */

static void *shift_enable(xd_server *server) {
    shift_state *state = XD_NEW(shift_state);
    state->server = server;
    state->timer = wl_event_loop_add_timer(server->loop, shift_tick, state);
    return state;
}

static void shift_disable([[maybe_unused]] xd_server *server, void *data) {
    shift_state *state = data;
    shift_finish(state, false);
    wl_event_source_remove(state->timer);
    xd_free(state);
}

static void shift_configure([[maybe_unused]] xd_server *server, void *data,
                            const xd_effects_settings *settings, size_t effect) {
    shift_state *state = data;
    shift_options *options = &state->options;
    xd_effects_get_binding(settings, effect, "Initiate", &options->initiate);
    options->mode = xd_effects_get_choice(settings, effect, "Mode") == 1 ? XD_SHIFT_FLIP
                                                                          : XD_SHIFT_COVER;
    options->size = xd_effects_get_int(settings, effect, "Size") / 100.0;
    options->brightness = xd_effects_get_int(settings, effect, "BackgroundBrightness") / 100.0;
    options->reflection = xd_effects_get_bool(settings, effect, "Reflection");
    options->animation_seconds = xd_effects_get_double(settings, effect, "AnimationTime");
}

const xd_effect_impl xd_effect_impl_shift_switcher = {
    .id = "ShiftSwitcher",
    .enable = shift_enable,
    .disable = shift_disable,
    .configure = shift_configure,
    .window_event = shift_handle_window_event,
    .frame = shift_handle_frame,
    .input = shift_handle_input,
};
