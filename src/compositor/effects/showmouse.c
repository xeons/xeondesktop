/* SPDX-License-Identifier: MIT
 * Show Mouse (both builds): Compiz's showmouse plugin, simplified. Its
 * key (Windows+K) switches on a ring of glowing dots that turns round the
 * pointer and follows it, to find it on a large or busy screen; Compiz
 * threw particles from turning emitters, which this draws as the
 * emitters alone. A small cairo picture, drawn again each frame while it
 * shows, in an overlay that takes no input.
 */
#include <math.h>

#include "../../core/alloc.h"
#include "effects.h"

/* Each dot's glow, as a share of the ring's radius. */
static constexpr double GLOW_SHARE = 0.3;

typedef struct show_mouse_options {
    int radius, dots;
    double turns_per_second;
    uint32_t color; /* 0xRRGGBBAA */
    bool at_startup;
    xd_effect_binding toggle;
} show_mouse_options;

typedef struct show_mouse_state {
    xd_server *server;
    show_mouse_options options;
    bool configured;
    bool shown;
    struct wlr_scene_tree *overlay;
    struct wlr_scene_buffer *node;
} show_mouse_state;

static bool show_mouse_reject_input([[maybe_unused]] struct wlr_scene_buffer *buffer,
                                    [[maybe_unused]] double *sx, [[maybe_unused]] double *sy) {
    return false;
}

static void show_mouse_set_shown(show_mouse_state *state, bool shown) {
    state->shown = shown;
    if (!shown && state->overlay != nullptr) {
        wlr_scene_node_destroy(&state->overlay->node);
        state->overlay = nullptr;
        state->node = nullptr;
    } else if (shown && state->overlay == nullptr) {
        state->overlay = xd_effects_create_overlay(state->server);
        state->node = wlr_scene_buffer_create(state->overlay, nullptr);
        state->node->point_accepts_input = show_mouse_reject_input;
    }
    xd_effects_schedule_frames(state->server);
}

/* The dots at this moment, round the middle of a picture `size` px
 * across. */
static void show_mouse_draw(const show_mouse_options *options, cairo_t *cairo, double size,
                            double scale, double now) {
    double radius = options->radius * scale, glow = options->radius * GLOW_SHARE * scale;
    double red = (options->color >> 24 & 0xFF) / 255.0, green = (options->color >> 16 & 0xFF) / 255.0;
    double blue = (options->color >> 8 & 0xFF) / 255.0, alpha = (options->color & 0xFF) / 255.0;
    double turn = fmod(now * options->turns_per_second, 1.0) * 2.0 * XD_PI;
    double centre = size / 2;
    /* A faint ring they run along. */
    cairo_set_source_rgba(cairo, red, green, blue, alpha * 0.35);
    cairo_set_line_width(cairo, XD_MAX(scale * 2.0, 1.0));
    cairo_arc(cairo, centre, centre, radius, 0, 2 * XD_PI);
    cairo_stroke(cairo);
    for (int i = 0; i < options->dots; i++) {
        double angle = turn + 2.0 * XD_PI * i / options->dots;
        double x = centre + radius * cos(angle), y = centre + radius * sin(angle);
        cairo_pattern_t *pattern = cairo_pattern_create_radial(x, y, 0, x, y, glow);
        cairo_pattern_add_color_stop_rgba(pattern, 0.0, 1, 1, 1, alpha);
        cairo_pattern_add_color_stop_rgba(pattern, 0.25, red, green, blue, alpha);
        cairo_pattern_add_color_stop_rgba(pattern, 1.0, red, green, blue, 0);
        cairo_set_source(cairo, pattern);
        cairo_arc(cairo, x, y, glow, 0, 2 * XD_PI);
        cairo_fill(cairo);
        cairo_pattern_destroy(pattern);
    }
}

static void show_mouse_handle_frame(xd_server *server, void *data,
                                    [[maybe_unused]] xd_output *output, double now) {
    show_mouse_state *state = data;
    if (!state->shown || state->node == nullptr)
        return;
    double x = server->cursor->x, y = server->cursor->y;
    double scale = xd_output_scale_at(server, x, y);
    const show_mouse_options *options = &state->options;
    /* Logical units across, whole, with room for the glow. */
    double across = ceil(2.0 * options->radius * (1.0 + GLOW_SHARE) + 2.0);
    double size = ceil(across * scale);
    cairo_surface_t *surface = nullptr;
    struct wlr_buffer *buffer = xd_cairo_buffer_new((int)size, (int)size, &surface);
    if (buffer == nullptr)
        return;
    cairo_t *cairo = cairo_create(surface);
    show_mouse_draw(options, cairo, size, scale, now);
    cairo_destroy(cairo);
    cairo_surface_flush(surface);
    wlr_scene_buffer_set_buffer(state->node, buffer);
    wlr_buffer_drop(buffer); /* the scene holds its own lock */
    wlr_scene_buffer_set_dest_size(state->node, (int)across, (int)across);
    wlr_scene_node_set_position(&state->node->node, (int)lround(x - across / 2),
                                (int)lround(y - across / 2));
    /* It turns all the time it shows. */
    xd_effects_schedule_frames(server);
}

static bool show_mouse_handle_input([[maybe_unused]] xd_server *server, void *data,
                                    const xd_effect_input *input) {
    show_mouse_state *state = data;
    if (input->kind == XD_EFFECT_INPUT_KEY &&
        xd_effects_binding_matches(&state->options.toggle, input)) {
        show_mouse_set_shown(state, !state->shown);
        return true;
    }
    return false;
}

static void *show_mouse_enable(xd_server *server) {
    show_mouse_state *state = XD_NEW(show_mouse_state);
    state->server = server;
    return state;
}

static void show_mouse_disable([[maybe_unused]] xd_server *server, void *data) {
    show_mouse_state *state = data;
    show_mouse_set_shown(state, false);
    xd_free(state);
}

static void show_mouse_configure([[maybe_unused]] xd_server *server, void *data,
                                 const xd_effects_settings *settings, size_t effect) {
    show_mouse_state *state = data;
    show_mouse_options *options = &state->options;
    options->radius = xd_effects_get_int(settings, effect, "Radius");
    options->dots = xd_effects_get_int(settings, effect, "Dots");
    options->turns_per_second = xd_effects_get_double(settings, effect, "RotationSpeed");
    options->color = xd_effects_get_color(settings, effect, "Color");
    options->at_startup = xd_effects_get_bool(settings, effect, "ActivateAtStartup");
    xd_effects_get_binding(settings, effect, "ToggleKey", &options->toggle);
    /* Shown from the start: once, when it's turned on. */
    if (!state->configured && options->at_startup)
        show_mouse_set_shown(state, true);
    state->configured = true;
}

const xd_effect_impl xd_effect_impl_show_mouse = {
    .id = "ShowMouse",
    .enable = show_mouse_enable,
    .disable = show_mouse_disable,
    .configure = show_mouse_configure,
    .frame = show_mouse_handle_frame,
    .input = show_mouse_handle_input,
};
