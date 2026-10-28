/* SPDX-License-Identifier: MIT
 * Window Opacity: Compiz's Opacity, Brightness and Saturation. Each
 * window has an opacity, a brightness and a saturation: the first rule's
 * it matches for each (or unchanged), as the bindings over it last set
 * them, and inactive windows no more than the inactive opacity and
 * brightness when they're dimmed.
 *
 * The scene graph has an opacity per buffer, in wlroots and SceneFX
 * alike, but no brightness or saturation: those are xd-compositor-fx's
 * alone, which draws a window whose colours change with OpenGL (glfx.h's
 * xd_glfx_set_window_color), its picture taking the opacity in place of
 * its buffers. The values are set again before each frame (unchanged
 * ones cost nothing), which also puts them back after Aero Peek or an
 * OpenGL effect set their own; Aero Peek's are left alone while it shows.
 */

#include "../../core/alloc.h"
#include "effects.h"
#include "opacity_model.h"
#ifdef XD_SCENEFX
#include "glfx.h"
#endif

/* Brightness and saturation are drawn with OpenGL, by xd-compositor-fx. */
#ifdef XD_SCENEFX
static constexpr size_t PROPERTIES = XD_OPACITY_PROPERTY_COUNT;
#else
static constexpr size_t PROPERTIES = XD_OPACITY_PROPERTY_OPACITY + 1;
#endif

typedef struct opacity_options {
    float steps[XD_OPACITY_PROPERTY_COUNT];
    xd_effect_binding increase[XD_OPACITY_PROPERTY_COUNT];
    xd_effect_binding decrease[XD_OPACITY_PROPERTY_COUNT];
    bool dim_inactive;
    /* What dimming lowers inactive windows to, per property. */
    float inactive[XD_OPACITY_PROPERTY_COUNT];
    xd_opacity_rule rules[XD_OPACITY_PROPERTY_COUNT][XD_OPACITY_RULES];
} opacity_options;

/* A window whose values the bindings set. */
typedef struct opacity_window {
    xd_view *view;
    float values[XD_OPACITY_PROPERTY_COUNT];
    bool set[XD_OPACITY_PROPERTY_COUNT];
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

/* A value before dimming: the bindings', else its rule's. */
static float opacity_get_base(opacity_state *state, const xd_view *view,
                              xd_opacity_property property) {
    const opacity_window *window = opacity_find_window(state, view);
    if (window != nullptr && window->set[property])
        return window->values[property];
    return xd_opacity_choose_rule(state->options.rules[property], XD_OPACITY_RULES,
                                  xd_view_app_id(view), xd_view_title(view), view->dialog);
}

static void opacity_set_buffer(struct wlr_scene_buffer *buffer, [[maybe_unused]] int sx,
                               [[maybe_unused]] int sy, void *data) {
    wlr_scene_buffer_set_opacity(buffer, *(const float *)data);
}

static void opacity_choose(opacity_state *state, xd_view *view,
                           float values[XD_OPACITY_PROPERTY_COUNT]) {
    bool dimmed = state->options.dim_inactive && xd_view_focused(state->server) != view;
    for (size_t property = 0; property < XD_OPACITY_PROPERTY_COUNT; property++) {
        values[property] = 1.0f;
        if (property >= PROPERTIES)
            continue;
        values[property] = opacity_get_base(state, view, (xd_opacity_property)property);
        if (dimmed)
            values[property] = XD_MIN(values[property], state->options.inactive[property]);
    }
}

/* Every window as the options say, or (`plain`) as itself. */
static void opacity_apply(opacity_state *state, bool plain) {
    xd_server *server = state->server;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (!view->mapped)
            continue;
        float values[XD_OPACITY_PROPERTY_COUNT] = {1.0f, 1.0f, 1.0f};
        if (!plain)
            opacity_choose(state, view, values);
#ifdef XD_SCENEFX
        /* Drawn by glfx, which gives its picture the opacity. */
        if (xd_glfx_set_window_color(view, values[XD_OPACITY_PROPERTY_OPACITY],
                                     values[XD_OPACITY_PROPERTY_BRIGHTNESS],
                                     values[XD_OPACITY_PROPERTY_SATURATION]))
            continue;
#endif
        wlr_scene_node_for_each_buffer(&view->tree->node, opacity_set_buffer,
                                       &values[XD_OPACITY_PROPERTY_OPACITY]);
    }
}

static void opacity_handle_frame(xd_server *server, void *data,
                                 [[maybe_unused]] xd_output *output,
                                 [[maybe_unused]] double now) {
    opacity_state *state = data;
    if (!server->peeking)
        opacity_apply(state, false);
}

/* The property whose binding the input is, and which way. */
static bool opacity_find_binding(const opacity_options *options, const xd_effect_input *input,
                                 xd_opacity_property *property, bool *more) {
    for (size_t i = 0; i < PROPERTIES; i++) {
        *property = (xd_opacity_property)i;
        *more = xd_effects_binding_matches(&options->increase[i], input);
        if (*more || xd_effects_binding_matches(&options->decrease[i], input))
            return true;
    }
    return false;
}

static bool opacity_handle_input(xd_server *server, void *data, const xd_effect_input *input) {
    opacity_state *state = data;
    if (input->kind != XD_EFFECT_INPUT_AXIS && input->kind != XD_EFFECT_INPUT_BUTTON)
        return false;
    xd_opacity_property property = XD_OPACITY_PROPERTY_OPACITY;
    bool more = false;
    if (!opacity_find_binding(&state->options, input, &property, &more))
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
        *window = (opacity_window){.view = view};
    }
    if (!window->set[property]) {
        window->values[property] = opacity_get_base(state, view, property);
        window->set[property] = true;
    }
    float step = more ? state->options.steps[property] : -state->options.steps[property];
    int steps = input->kind == XD_EFFECT_INPUT_AXIS ? input->steps : 1;
    window->values[property] =
        xd_opacity_step_value(property, window->values[property], step, steps);
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
#ifdef XD_SCENEFX
    /* Aero Peek's opacities stay while it shows, but no window is drawn
     * by glfx any more. */
    if (server->peeking) {
        XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
            xd_glfx_set_window_color(view, 1.0f, 1.0f, 1.0f);
        }
    }
#endif
    if (!server->peeking)
        opacity_apply(state, true);
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
    /* Each property's keys: the opacity ones are the older, plainer names. */
    static const struct {
        const char *step, *increase, *decrease, *rule_prefix, *rule_value;
    } keys[XD_OPACITY_PROPERTY_COUNT] = {
        [XD_OPACITY_PROPERTY_OPACITY] = {"Step", "IncreaseButton", "DecreaseButton", "",
                                         "Opacity"},
        [XD_OPACITY_PROPERTY_BRIGHTNESS] = {"BrightnessStep", "BrightnessIncreaseButton",
                                            "BrightnessDecreaseButton", "Brightness", "Value"},
        [XD_OPACITY_PROPERTY_SATURATION] = {"SaturationStep", "SaturationIncreaseButton",
                                            "SaturationDecreaseButton", "Saturation", "Value"},
    };
    for (size_t property = 0; property < XD_OPACITY_PROPERTY_COUNT; property++) {
        options->steps[property] = opacity_get_percent(settings, effect, keys[property].step);
        xd_effects_get_binding(settings, effect, keys[property].increase,
                               &options->increase[property]);
        xd_effects_get_binding(settings, effect, keys[property].decrease,
                               &options->decrease[property]);
        for (size_t i = 0; i < XD_OPACITY_RULES; i++) {
            xd_opacity_rule *rule = &options->rules[property][i];
            char key[64];
            snprintf(key, sizeof(key), "%sRule%zuWindows", keys[property].rule_prefix, i + 1);
            snprintf(rule->windows, sizeof(rule->windows), "%s",
                     xd_effects_get_text(settings, effect, key));
            snprintf(key, sizeof(key), "%sRule%zu%s", keys[property].rule_prefix, i + 1,
                     keys[property].rule_value);
            rule->value = opacity_get_percent(settings, effect, key);
        }
    }
    options->dim_inactive = xd_effects_get_bool(settings, effect, "DimInactive");
    options->inactive[XD_OPACITY_PROPERTY_OPACITY] =
        opacity_get_percent(settings, effect, "InactiveOpacity");
    options->inactive[XD_OPACITY_PROPERTY_BRIGHTNESS] =
        opacity_get_percent(settings, effect, "InactiveBrightness");
    /* Dimming leaves the saturation alone. */
    options->inactive[XD_OPACITY_PROPERTY_SATURATION] = 1.0f;
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
