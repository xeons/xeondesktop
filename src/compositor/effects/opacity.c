/* SPDX-License-Identifier: MIT
 * Window Opacity (both builds): Compiz's Opacity, Brightness and
 * Saturation, the opacity part. Each window's buffers are drawn at the
 * opacity of the first rule it matches (or opaque), as Alt and the wheel
 * over it last set, and inactive windows no more than the inactive
 * opacity when they're dimmed.
 *
 * The scene graph has an opacity per buffer, in wlroots and SceneFX
 * alike, but no brightness or saturation, so those aren't offered. The
 * opacities are set again before each frame (unchanged ones cost
 * nothing), which also puts them back after Aero Peek or an OpenGL effect
 * set their own; Aero Peek's are left alone while it shows.
 */

#include "../../core/alloc.h"
#include "effects.h"

enum { OPACITY_RULES = 3 };
/* Alt and the wheel go no lower, so a window can't vanish. */
static constexpr float LOWEST_OPACITY = 0.05f;

typedef struct opacity_rule {
    char windows[512]; /* "" is no rule */
    float opacity;
} opacity_rule;

typedef struct opacity_options {
    float step;
    xd_effect_binding increase, decrease;
    bool dim_inactive;
    float inactive;
    opacity_rule rules[OPACITY_RULES];
} opacity_options;

/* A window whose opacity Alt and the wheel set. */
typedef struct opacity_window {
    xd_view *view;
    float opacity;
} opacity_window;

typedef struct opacity_state {
    xd_server *server;
    opacity_options options;
    opacity_window *windows;
    size_t window_count, window_capacity;
} opacity_state;

static opacity_window *opacity_find_window(opacity_state *state, const xd_view *view) {
    for (size_t i = 0; i < state->window_count; i++) {
        if (state->windows[i].view == view)
            return &state->windows[i];
    }
    return nullptr;
}

/* The opacity a window has before dimming: Alt and the wheel's, else its
 * rule's. */
static float opacity_get_base(opacity_state *state, const xd_view *view) {
    const opacity_window *window = opacity_find_window(state, view);
    if (window != nullptr)
        return window->opacity;
    for (size_t i = 0; i < OPACITY_RULES; i++) {
        const opacity_rule *rule = &state->options.rules[i];
        if (rule->windows[0] != '\0' && xd_effects_match_view(rule->windows, view))
            return rule->opacity;
    }
    return 1.0f;
}

static void opacity_set_buffer(struct wlr_scene_buffer *buffer, [[maybe_unused]] int sx,
                               [[maybe_unused]] int sy, void *data) {
    wlr_scene_buffer_set_opacity(buffer, *(const float *)data);
}

static void opacity_apply(opacity_state *state, float (*choose)(opacity_state *, xd_view *)) {
    xd_server *server = state->server;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (!view->mapped)
            continue;
        float opacity = choose(state, view);
        wlr_scene_node_for_each_buffer(&view->tree->node, opacity_set_buffer, &opacity);
    }
}

static float opacity_choose(opacity_state *state, xd_view *view) {
    float opacity = opacity_get_base(state, view);
    if (state->options.dim_inactive && xd_view_focused(state->server) != view)
        opacity = XD_MIN(opacity, state->options.inactive);
    return opacity;
}

static float opacity_choose_opaque([[maybe_unused]] opacity_state *state,
                                   [[maybe_unused]] xd_view *view) {
    return 1.0f;
}

static void opacity_handle_frame(xd_server *server, void *data,
                                 [[maybe_unused]] xd_output *output,
                                 [[maybe_unused]] double now) {
    opacity_state *state = data;
    if (!server->peeking)
        opacity_apply(state, opacity_choose);
}

static bool opacity_handle_input(xd_server *server, void *data, const xd_effect_input *input) {
    opacity_state *state = data;
    if (input->kind != XD_EFFECT_INPUT_AXIS && input->kind != XD_EFFECT_INPUT_BUTTON)
        return false;
    bool more = xd_effects_binding_matches(&state->options.increase, input);
    bool less = !more && xd_effects_binding_matches(&state->options.decrease, input);
    if (!more && !less)
        return false;
    xd_view *view = xd_input_target_at(server, input->x, input->y).view;
    if (view == nullptr)
        return true; /* the binding's, though there's no window to change */
    opacity_window *window = opacity_find_window(state, view);
    if (window == nullptr) {
        if (state->window_count == state->window_capacity) {
            state->window_capacity = XD_MAX(state->window_capacity * 2, 8);
            state->windows = xd_realloc_array(state->windows, state->window_capacity,
                                              sizeof(*state->windows));
        }
        window = &state->windows[state->window_count++];
        *window = (opacity_window){view, opacity_get_base(state, view)};
    }
    float step = (more ? state->options.step : -state->options.step) *
                 (float)(input->kind == XD_EFFECT_INPUT_AXIS ? input->steps : 1);
    window->opacity = XD_CLAMP(window->opacity + step, LOWEST_OPACITY, 1.0f);
    xd_effects_schedule_frames(server);
    return true;
}

static void opacity_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                        const xd_window_event *event) {
    opacity_state *state = data;
    if (event->kind != XD_WINDOW_EVENT_DESTROY)
        return;
    opacity_window *window = opacity_find_window(state, event->view);
    if (window != nullptr)
        *window = state->windows[--state->window_count];
}

static void *opacity_enable(xd_server *server) {
    opacity_state *state = XD_NEW(opacity_state);
    state->server = server;
    return state;
}

static void opacity_disable([[maybe_unused]] xd_server *server, void *data) {
    opacity_state *state = data;
    if (!server->peeking)
        opacity_apply(state, opacity_choose_opaque);
    xd_effects_schedule_frames(server);
    xd_free(state->windows);
    xd_free(state);
}

static float opacity_get_percent(const xd_effects_settings *settings, size_t effect,
                                 const char *key) {
    int percent = xd_effects_get_int(settings, effect, key);
    return (float)percent / 100.0f;
}

static void opacity_configure(xd_server *server, void *data, const xd_effects_settings *settings,
                              size_t effect) {
    opacity_state *state = data;
    opacity_options *options = &state->options;
    options->step = opacity_get_percent(settings, effect, "Step");
    xd_effects_get_binding(settings, effect, "IncreaseButton", &options->increase);
    xd_effects_get_binding(settings, effect, "DecreaseButton", &options->decrease);
    options->dim_inactive = xd_effects_get_bool(settings, effect, "DimInactive");
    options->inactive = opacity_get_percent(settings, effect, "InactiveOpacity");
    for (size_t i = 0; i < OPACITY_RULES; i++) {
        char key[32];
        snprintf(key, sizeof(key), "Rule%zuWindows", i + 1);
        snprintf(options->rules[i].windows, sizeof(options->rules[i].windows), "%s",
                 xd_effects_get_text(settings, effect, key));
        snprintf(key, sizeof(key), "Rule%zuOpacity", i + 1);
        options->rules[i].opacity = opacity_get_percent(settings, effect, key);
    }
    xd_effects_schedule_frames(server);
}

const xd_effect_impl xd_effect_impl_window_opacity = {
    .id = "WindowOpacity",
    .enable = opacity_enable,
    .disable = opacity_disable,
    .configure = opacity_configure,
    .window_event = opacity_handle_window_event,
    .frame = opacity_handle_frame,
    .input = opacity_handle_input,
};
