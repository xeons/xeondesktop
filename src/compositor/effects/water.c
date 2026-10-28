/* SPDX-License-Identifier: MIT
 * Water Effect (xd-compositor-fx): an output filter (glfx.h) drawing the
 * whole screen as seen through rippling water. The waves are a height
 * field over the output layout (water_model.h), a cell every few layout
 * units, stepped 60 times a second; its slopes go to the GPU as a small
 * texture each frame, and a shader shifts and shades the screen's picture
 * by them. Idle (and free) while the water is still and it isn't raining.
 */
#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/wlr_texture.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../../core/alloc.h"
#include "glfx.h"
#include "water_model.h"

/* The field's cells: at least this many layout units, and at most this
 * many across (big layouts get bigger cells; llvmpipe steps it too). */
static constexpr double MIN_CELL = 4.0;
static constexpr int MAX_COLUMNS = 400;
static constexpr double STEPS_PER_SECOND = 60.0;
static constexpr int MAX_STEPS_PER_FRAME = 4;
/* How far a slope of 1 shifts the picture at an offset scale of 1. */
static constexpr double SHIFT_PX = 64.0;
/* Drops: a click's, a raindrop's, the pointer's wake, a title's wave
 * (radius in layout units, depth). */
static constexpr double RIPPLE_RADIUS = 28.0;
static constexpr double RIPPLE_DEPTH = 1.6;
static constexpr double RAIN_RADIUS = 14.0;
static constexpr double RAIN_DEPTH = 0.8;
static constexpr double WAKE_RADIUS = 12.0;
static constexpr double WAKE_DEPTH = 0.25;
static constexpr double TITLE_RADIUS = 10.0;
static constexpr double TITLE_DEPTH = 0.6;

static const char water_vertex[] = "uniform mat4 projection;\n"
                                   "uniform vec2 texcoord_scale;\n"
                                   "attribute vec2 position;\n"
                                   "attribute vec2 texcoord;\n"
                                   "varying vec2 v_texcoord;\n"
                                   "void main() {\n"
                                   "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                   "    v_texcoord = texcoord * texcoord_scale;\n"
                                   "}\n";

/* `slopes` holds each cell's slope across and down (red, green) and its
 * light (blue), 0.5 for none. */
static const char water_fragment[] =
    "precision mediump float;\n"
    "uniform XD_SAMPLER texture0;\n"
    "uniform sampler2D slopes;\n"
    "uniform float alpha;\n"
    "uniform vec2 field_offset;\n"
    "uniform vec2 field_scale;\n"
    "uniform vec2 shift;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    vec4 slope = texture2D(slopes, field_offset + v_texcoord * field_scale);\n"
    "    vec2 offset = (slope.rg - vec2(0.5)) * 2.0;\n"
    "    vec4 color = texture2D(texture0, v_texcoord + offset * shift);\n"
    "    float light = slope.b - 0.5;\n"
    "    gl_FragColor = vec4(color.rgb + light * color.a, color.a) * alpha;\n"
    "}\n";

/* Compiled once, for 2D and external textures;
 * xd_glfx_finish destroys them, as glfx's own. */
typedef struct water_shader {
    bool tried;
    xd_glfx_shader shader;
    GLint slopes, field_offset, field_scale, shift;
} water_shader;

static water_shader shaders[2];

typedef struct water_options {
    double offset_scale;
    int rain_delay_ms;
    bool title_wave, pointer_wake;
    xd_effect_binding ripple, toggle_rain;
} water_options;

typedef struct water_state {
    xd_server *server;
    water_options options;
    xd_output_filter *filter;
    xd_water water;
    struct wlr_box layout; /* what the field covers */
    double cell;           /* layout units a cell */
    double steps_owed, rain_waited;
    bool raining, waking;
    bool still; /* flat since its last step */
    double wake_x, wake_y; /* the pointer, when its wake last moved */
    uint64_t seed;
    uint8_t *pixels;              /* the slopes, RGBA a cell */
    struct wlr_texture *slopes;   /* made of them, for this frame */
} water_state;

static const water_shader *water_get_shader(GLenum target) {
    water_shader *entry = &shaders[target == GL_TEXTURE_EXTERNAL_OES ? 1 : 0];
    if (!entry->tried) {
        entry->tried = true;
        if (xd_glfx_init_shader(&entry->shader, water_vertex, water_fragment, target)) {
            GLuint program = entry->shader.program;
            entry->slopes = glGetUniformLocation(program, "slopes");
            entry->field_offset = glGetUniformLocation(program, "field_offset");
            entry->field_scale = glGetUniformLocation(program, "field_scale");
            entry->shift = glGetUniformLocation(program, "shift");
        }
    }
    return entry->shader.program != 0 ? entry : nullptr;
}

static double water_get_random(uint64_t *seed) {
    uint64_t value = *seed;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *seed = value;
    return (double)((value * 0x2545F4914F6CDD1DULL) >> 11) / 9007199254740992.0;
}

/* ---- the field ------------------------------------------------------------------ */

/* The field covers the output layout: made anew (flat) when that
 * changed. False without one. */
static bool water_ensure_field(water_state *state) {
    struct wlr_box layout = {0};
    wlr_output_layout_get_box(state->server->output_layout, nullptr, &layout);
    if (layout.width <= 0 || layout.height <= 0)
        return false;
    if (state->water.heights != nullptr && layout.x == state->layout.x &&
        layout.y == state->layout.y && layout.width == state->layout.width &&
        layout.height == state->layout.height)
        return true;
    xd_water_free(&state->water);
    xd_free(state->pixels);
    state->pixels = nullptr;
    state->layout = layout;
    state->cell = fmax(MIN_CELL, ceil((double)layout.width / MAX_COLUMNS));
    double cells_across = ceil(layout.width / state->cell);
    double cells_down = ceil(layout.height / state->cell);
    int columns = (int)cells_across + 2;
    int rows = (int)cells_down + 2;
    if (!xd_water_init(&state->water, columns, rows))
        return false;
    state->pixels = XD_NEW_ARRAY(uint8_t, (size_t)columns * (size_t)rows * 4);
    return true;
}

/* Something disturbed the water, or will: the filter runs again. */
static void water_wake(water_state *state) {
    state->still = false;
    if (state->filter != nullptr)
        xd_glfx_wake_output_filter(state->filter);
}

/* A drop at a layout point, `radius` layout units across. */
static void water_drop(water_state *state, double x, double y, double radius, double depth) {
    if (!water_ensure_field(state))
        return;
    xd_water_add_drop(&state->water, (x - state->layout.x) / state->cell,
                      (y - state->layout.y) / state->cell, radius / state->cell, depth);
    water_wake(state);
}

static uint8_t water_encode(double value) {
    double byte = 128 + value * 127;
    return (uint8_t)(byte < 0 ? 0 : byte > 255 ? 255 : byte);
}

/* The slopes as a texture, for this frame's draws. */
static void water_upload_slopes(water_state *state) {
    const xd_water *water = &state->water;
    for (int row = 0; row < water->rows; row++) {
        for (int column = 0; column < water->columns; column++) {
            double across = 0, down = 0;
            xd_water_get_slope(water, column, row, &across, &down);
            uint8_t *pixel = &state->pixels[((size_t)row * (size_t)water->columns +
                                             (size_t)column) * 4];
            pixel[0] = water_encode(across);
            pixel[1] = water_encode(down);
            /* Lit from the top left. */
            pixel[2] = water_encode(-(across + down) * 0.7);
            pixel[3] = 255;
        }
    }
    if (state->slopes != nullptr)
        wlr_texture_destroy(state->slopes);
    state->slopes = wlr_texture_from_pixels(state->server->renderer, DRM_FORMAT_ABGR8888,
                                            (uint32_t)water->columns * 4,
                                            (uint32_t)water->columns, (uint32_t)water->rows,
                                            state->pixels);
}

/* ---- the output filter ---------------------------------------------------------- */

static bool water_is_waking(const water_state *state) {
    if (!state->options.pointer_wake)
        return false;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(state->server->seat);
    uint32_t modifiers = keyboard != nullptr ? wlr_keyboard_get_modifiers(keyboard) : 0;
    return (modifiers & WLR_MODIFIER_CTRL) && (modifiers & WLR_MODIFIER_LOGO);
}

static bool water_update(xd_output_filter *filter, double seconds) {
    water_state *state = filter->data;
    if (!water_ensure_field(state))
        return false;
    if (state->raining) {
        state->rain_waited += seconds * 1000.0;
        double delay = XD_MAX(state->options.rain_delay_ms, 1);
        for (int drops = 0; state->rain_waited >= delay && drops < 64; drops++) {
            state->rain_waited -= delay;
            double x = state->layout.x + water_get_random(&state->seed) * state->layout.width;
            double y = state->layout.y + water_get_random(&state->seed) * state->layout.height;
            xd_water_add_drop(&state->water, (x - state->layout.x) / state->cell,
                              (y - state->layout.y) / state->cell, RAIN_RADIUS / state->cell,
                              RAIN_DEPTH);
            state->still = false;
        }
        state->rain_waited = fmin(state->rain_waited, delay);
    }
    const struct wlr_cursor *cursor = state->server->cursor;
    state->waking = water_is_waking(state);
    if (state->waking && (cursor->x != state->wake_x || cursor->y != state->wake_y)) {
        double cell = state->cell;
        xd_water_add_line(&state->water, (state->wake_x - state->layout.x) / cell,
                          (state->wake_y - state->layout.y) / cell,
                          (cursor->x - state->layout.x) / cell,
                          (cursor->y - state->layout.y) / cell, WAKE_RADIUS / cell, WAKE_DEPTH);
        state->still = false;
        /* The Windows key was used: letting go of it opens no Start menu. */
        state->server->super_alone = false;
    }
    state->wake_x = cursor->x;
    state->wake_y = cursor->y;
    state->steps_owed = fmin(state->steps_owed + seconds * STEPS_PER_SECOND, MAX_STEPS_PER_FRAME);
    /* The first frame after waking steps once, so new drops show. */
    if (seconds == 0)
        state->steps_owed = 1;
    for (; state->steps_owed >= 1; state->steps_owed -= 1)
        state->still = !xd_water_step(&state->water);
    bool active = !state->still || state->raining || state->waking;
    if (active)
        water_upload_slopes(state);
    return active;
}

static void water_draw(xd_output_filter *filter, [[maybe_unused]] xd_output *output,
                       const xd_glfx_texture *scene, const xd_glfx_draw *draw) {
    water_state *state = filter->data;
    xd_glfx_mesh mesh;
    if (!xd_glfx_init_mesh(&mesh, 1, 1))
        return;
    xd_glfx_set_mesh_rect(&mesh, (struct wlr_fbox){draw->bounds.x, draw->bounds.y,
                                                    draw->bounds.width, draw->bounds.height});
    const water_shader *entry = water_get_shader(scene->target);
    if (entry == nullptr || state->slopes == nullptr) {
        xd_glfx_draw_mesh(draw, &mesh, scene, 1.0f);
        xd_glfx_free_mesh(&mesh);
        return;
    }
    struct fx_texture_attribs attribs = {0};
    fx_texture_get_attribs(state->slopes, &attribs);
    double field_width = state->water.columns * state->cell;
    double field_height = state->water.rows * state->cell;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(attribs.target, attribs.tex);
    glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(entry->shader.program);
    glUniform1i(entry->slopes, 1);
    glUniform2f(entry->field_offset,
                (float)((draw->bounds.x - state->layout.x) / field_width),
                (float)((draw->bounds.y - state->layout.y) / field_height));
    glUniform2f(entry->field_scale, (float)(draw->bounds.width / field_width),
                (float)(draw->bounds.height / field_height));
    double shift = state->options.offset_scale * SHIFT_PX;
    glUniform2f(entry->shift, (float)(shift / XD_MAX(draw->bounds.width, 1)),
                (float)(shift / XD_MAX(draw->bounds.height, 1)));
    xd_glfx_draw_mesh_with(draw, &mesh, scene, &entry->shader, 1.0f);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(attribs.target, 0);
    glActiveTexture(GL_TEXTURE0);
    xd_glfx_free_mesh(&mesh);
}

static const xd_output_filter_impl filter_impl = {
    .name = "Water Effect",
    .update = water_update,
    .draw = water_draw,
};

/* ---- the effect ----------------------------------------------------------------- */

static void *water_enable(xd_server *server) {
    water_state *state = XD_NEW(water_state);
    state->server = server;
    state->seed = 0x9E3779B97F4A7C15ULL;
    state->wake_x = server->cursor->x;
    state->wake_y = server->cursor->y;
    state->filter = xd_glfx_add_output_filter(server, &filter_impl, state);
    return state;
}

static void water_disable([[maybe_unused]] xd_server *server, void *data) {
    water_state *state = data;
    if (state->filter != nullptr)
        xd_glfx_remove_output_filter(state->filter);
    if (state->slopes != nullptr)
        wlr_texture_destroy(state->slopes);
    xd_water_free(&state->water);
    xd_free(state->pixels);
    xd_free(state);
}

static void water_configure([[maybe_unused]] xd_server *server, void *data,
                            const xd_effects_settings *settings, size_t effect) {
    water_state *state = data;
    water_options *options = &state->options;
    options->offset_scale = xd_effects_get_double(settings, effect, "OffsetScale");
    options->rain_delay_ms = xd_effects_get_int(settings, effect, "RainDelay");
    options->title_wave = xd_effects_get_bool(settings, effect, "TitleWave");
    options->pointer_wake = xd_effects_get_bool(settings, effect, "PointerWake");
    xd_effects_get_binding(settings, effect, "Ripple", &options->ripple);
    xd_effects_get_binding(settings, effect, "ToggleRain", &options->toggle_rain);
}

static void water_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                      const xd_window_event *event) {
    water_state *state = data;
    if (event->kind != XD_WINDOW_EVENT_TITLE || !state->options.title_wave ||
        event->view->minimized || !water_ensure_field(state))
        return;
    /* Along the middle of its title bar. */
    struct wlr_box outer = xd_view_outer_box(event->view);
    double cell = state->cell;
    double y = (outer.y + 12 - state->layout.y) / cell;
    xd_water_add_line(&state->water, (outer.x + 8 - state->layout.x) / cell, y,
                      (outer.x + outer.width - 8 - state->layout.x) / cell, y,
                      TITLE_RADIUS / cell, TITLE_DEPTH);
    water_wake(state);
}

static bool water_handle_key(water_state *state, const xd_effect_input *input) {
    if (xd_effects_binding_matches(&state->options.toggle_rain, input)) {
        state->raining = !state->raining;
        state->rain_waited = 0;
        water_wake(state);
        return true;
    }
    /* Ctrl or the Windows key going down may start a wake: the filter
     * looks while they're held. */
    uint32_t keysym = input->keysym;
    bool modifier = keysym == XKB_KEY_Control_L || keysym == XKB_KEY_Control_R ||
                    keysym == XKB_KEY_Super_L || keysym == XKB_KEY_Super_R;
    if (input->pressed && modifier && state->options.pointer_wake) {
        state->wake_x = state->server->cursor->x;
        state->wake_y = state->server->cursor->y;
        water_wake(state);
    }
    return false;
}

static bool water_handle_input(xd_server *server, void *data, const xd_effect_input *input) {
    water_state *state = data;
    switch (input->kind) {
    case XD_EFFECT_INPUT_KEY:
        return water_handle_key(state, input);
    case XD_EFFECT_INPUT_BUTTON:
        /* A shortcut: a program taking them (a virtual machine) has the
         * click. */
        if (xd_shortcuts_inhibited(server) ||
            !xd_effects_binding_matches(&state->options.ripple, input))
            return false;
        water_drop(state, input->x, input->y, RIPPLE_RADIUS, RIPPLE_DEPTH);
        return true;
    case XD_EFFECT_INPUT_AXIS:
    case XD_EFFECT_INPUT_MOTION:
    case XD_EFFECT_INPUT_MODIFIERS:
        return false;
    }
    return false;
}

const xd_effect_impl xd_effect_impl_water = {
    .id = "Water",
    .enable = water_enable,
    .disable = water_disable,
    .configure = water_configure,
    .window_event = water_handle_window_event,
    .input = water_handle_input,
};
