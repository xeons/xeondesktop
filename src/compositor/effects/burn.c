/* SPDX-License-Identifier: MIT
 * Burn (xd-compositor-fx): a closing window's last picture burns away, a
 * ragged flame front crossing it (burn_model.h; a shader chars the band
 * behind it and drops what's burnt), giving off sparks drawn as glowing
 * points that rise, slow and fade.
 */
#include <math.h>

#include "../../core/alloc.h"
#include "burn_model.h"
#include "glfx.h"

static const char window_vertex[] = "uniform mat4 projection;\n"
                                    "uniform vec2 texcoord_scale;\n"
                                    "attribute vec2 position;\n"
                                    "attribute vec2 texcoord;\n"
                                    "varying vec2 v_texcoord;\n"
                                    "varying vec2 v_window;\n"
                                    "void main() {\n"
                                    "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                    "    v_texcoord = texcoord * texcoord_scale;\n"
                                    "    v_window = texcoord;\n"
                                    "}\n";

/* The front as burn_model.c has it, made ragged by value noise; what's
 * behind it chars, glows in the fire's colour near the edge, and goes.
 * Textures are premultiplied. */
static const char window_fragment[] =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform XD_SAMPLER texture0;\n"
    "uniform float alpha;\n"
    "uniform vec3 along_axis;\n"
    "uniform float front;\n"
    "uniform float edge;\n"
    "uniform vec3 fire;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec2 v_window;\n"
    "float hash(vec2 point) {\n"
    "    return fract(sin(dot(point, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    "float noise(vec2 point) {\n"
    "    vec2 cell = floor(point);\n"
    "    vec2 inside = fract(point);\n"
    "    inside = inside * inside * (3.0 - 2.0 * inside);\n"
    "    return mix(mix(hash(cell), hash(cell + vec2(1.0, 0.0)), inside.x),\n"
    "               mix(hash(cell + vec2(0.0, 1.0)), hash(cell + vec2(1.0, 1.0)), inside.x),\n"
    "               inside.y);\n"
    "}\n"
    "void main() {\n"
    "    float ragged = noise(v_window * 16.0) * 0.7 + noise(v_window * 41.0) * 0.3;\n"
    "    float along = dot(vec3(v_window, 1.0), along_axis) + (ragged - 0.5) * 0.08;\n"
    "    float burnt = (front - along) / edge;\n"
    "    if (burnt >= 1.0)\n"
    "        discard;\n"
    "    vec4 color = texture2D(texture0, v_texcoord) * alpha;\n"
    "    if (burnt > 0.0) {\n"
    "        color.rgb *= 1.0 - 0.85 * clamp(burnt * 1.6, 0.0, 1.0);\n"
    "        color.rgb += fire * smoothstep(0.5, 0.92, burnt) * color.a;\n"
    "        color *= 1.0 - smoothstep(0.88, 1.0, burnt);\n"
    "    }\n"
    "    gl_FragColor = color;\n"
    "}\n";

static const char spark_vertex[] = "uniform mat4 projection;\n"
                                   "uniform float point_scale;\n"
                                   "attribute vec2 position;\n"
                                   "attribute vec4 color;\n"
                                   "attribute float size;\n"
                                   "varying vec4 v_color;\n"
                                   "void main() {\n"
                                   "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                   "    gl_PointSize = size * point_scale;\n"
                                   "    v_color = color;\n"
                                   "}\n";

/* A soft round glow. */
static const char spark_fragment[] = "precision mediump float;\n"
                                     "varying vec4 v_color;\n"
                                     "void main() {\n"
                                     "    vec2 offset = gl_PointCoord - vec2(0.5);\n"
                                     "    float glow = max(1.0 - dot(offset, offset) * 4.0, 0.0);\n"
                                     "    gl_FragColor = v_color * glow * glow;\n"
                                     "}\n";

/* Compiled once, the first time a pass needs them, for 2D and external
 * textures; they go with the renderer's context, as glfx's own do. */
typedef struct burn_window_shader {
    bool tried;
    xd_glfx_shader shader;
    GLint along_axis, front, edge, fire;
} burn_window_shader;

static burn_window_shader window_shaders[2];
static bool sparks_tried;
static xd_glfx_shader spark_shader;
static GLint spark_color = -1, spark_size = -1, spark_point_scale = -1;

typedef struct burn_options {
    int duration_ms;
    int direction; /* the choice: xd_burn_direction, or 4 for random */
    float fire[3];
    bool mystical;
    int particle_count;
    double particle_size, particle_life, slowdown;
    char windows[512]; /* which windows: xd_effects_match_window */
} burn_options;

typedef struct burn_state {
    xd_server *server;
    burn_options options;
    struct wl_list windows; /* burn_window.link */
    uint64_t seed;
} burn_state;

typedef struct burn_window {
    struct wl_list link; /* burn_state.windows */
    burn_state *state;
    xd_window_effect *effect;
    xd_burn_direction direction;
    double progress; /* 0: the whole window, 1: burnt away */
    double spawn_owed;
    xd_burn_particles particles;
    float *spark_positions, *spark_colors, *spark_sizes;
    xd_glfx_mesh mesh;
} burn_window;

static const xd_window_effect_impl window_impl;

/* ---- drawing -------------------------------------------------------------------- */

static const burn_window_shader *burn_get_window_shader(GLenum target) {
    burn_window_shader *entry = &window_shaders[target == GL_TEXTURE_EXTERNAL_OES ? 1 : 0];
    if (!entry->tried) {
        entry->tried = true;
        if (xd_glfx_init_shader(&entry->shader, window_vertex, window_fragment, target)) {
            GLuint program = entry->shader.program;
            entry->along_axis = glGetUniformLocation(program, "along_axis");
            entry->front = glGetUniformLocation(program, "front");
            entry->edge = glGetUniformLocation(program, "edge");
            entry->fire = glGetUniformLocation(program, "fire");
        }
    }
    return entry->shader.program != 0 ? entry : nullptr;
}

static const xd_glfx_shader *burn_get_spark_shader(void) {
    if (!sparks_tried) {
        sparks_tried = true;
        if (xd_glfx_init_shader(&spark_shader, spark_vertex, spark_fragment, GL_TEXTURE_2D)) {
            spark_color = glGetAttribLocation(spark_shader.program, "color");
            spark_size = glGetAttribLocation(spark_shader.program, "size");
            spark_point_scale = glGetUniformLocation(spark_shader.program, "point_scale");
        }
    }
    return spark_shader.program != 0 && spark_shader.position >= 0 && spark_color >= 0 &&
                   spark_size >= 0
               ? &spark_shader
               : nullptr;
}

static void burn_draw_window(burn_window *window, const xd_glfx_draw *draw) {
    xd_window_effect *effect = window->effect;
    const burn_window_shader *entry = burn_get_window_shader(effect->texture.target);
    if (entry == nullptr)
        return;
    double axis[3];
    xd_burn_get_axis(window->direction, axis);
    const float *fire = window->state->options.fire;
    xd_glfx_set_mesh_rect(&window->mesh, effect->texture.box);
    glUseProgram(entry->shader.program);
    glUniform3f(entry->along_axis, (float)axis[0], (float)axis[1], (float)axis[2]);
    glUniform1f(entry->front, (float)xd_burn_get_front(window->progress));
    glUniform1f(entry->edge, (float)xd_burn_get_edge());
    glUniform3f(entry->fire, fire[0], fire[1], fire[2]);
    xd_glfx_draw_mesh_with(draw, &window->mesh, &effect->texture, &entry->shader, 1.0f);
}

static void burn_draw_sparks(burn_window *window, const xd_glfx_draw *draw) {
    const xd_burn_particles *particles = &window->particles;
    const xd_glfx_shader *shader = burn_get_spark_shader();
    if (shader == nullptr || particles->count == 0)
        return;
    double diameter = window->state->options.particle_size * 2;
    for (size_t i = 0; i < particles->count; i++) {
        const xd_burn_particle *particle = &particles->items[i];
        float share = (float)(particle->life / particle->lifetime);
        /* Young sparks are hotter: yellower. */
        float heat = 0.5f * share * share;
        window->spark_positions[i * 2] = (float)particle->x;
        window->spark_positions[i * 2 + 1] = (float)particle->y;
        window->spark_colors[i * 4] = (particle->red + (1.0f - particle->red) * heat) * share;
        window->spark_colors[i * 4 + 1] =
            (particle->green + (0.85f - particle->green) * heat) * share;
        window->spark_colors[i * 4 + 2] = (particle->blue + (0.4f - particle->blue) * heat) * share;
        window->spark_colors[i * 4 + 3] = share;
        window->spark_sizes[i] = (float)diameter;
    }
    glUseProgram(shader->program);
    glUniformMatrix4fv(shader->projection, 1, GL_FALSE, draw->projection);
    glUniform1f(spark_point_scale, (float)draw->scale);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLuint position = (GLuint)shader->position, color = (GLuint)spark_color,
           size = (GLuint)spark_size;
    glEnableVertexAttribArray(position);
    glEnableVertexAttribArray(color);
    glEnableVertexAttribArray(size);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, window->spark_positions);
    glVertexAttribPointer(color, 4, GL_FLOAT, GL_FALSE, 0, window->spark_colors);
    glVertexAttribPointer(size, 1, GL_FLOAT, GL_FALSE, 0, window->spark_sizes);
    /* Glows add up. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDrawArrays(GL_POINTS, 0, (GLsizei)particles->count);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisableVertexAttribArray(position);
    glDisableVertexAttribArray(color);
    glDisableVertexAttribArray(size);
    glUseProgram(0);
}

/* ---- the window effect ----------------------------------------------------------- */

/* A colour of full saturation and brightness, of hue 0..1. */
static void burn_get_hue(double hue, float rgb[3]) {
    double sixth = hue * 6;
    double rising = sixth - floor(sixth);
    float up = (float)rising, down = (float)(1 - rising);
    const float table[6][3] = {{1, up, 0}, {down, 1, 0}, {0, 1, up},
                               {0, down, 1}, {up, 0, 1}, {1, 0, down}};
    int part = (int)sixth % 6;
    rgb[0] = table[part][0];
    rgb[1] = table[part][1];
    rgb[2] = table[part][2];
}

static void burn_spawn(burn_window *window, double seconds) {
    const burn_options *options = &window->state->options;
    xd_burn_particles *particles = &window->particles;
    const struct wlr_fbox *box = &window->effect->texture.box;
    double lifetime = options->particle_life;
    /* Enough a second to keep about ParticleCount alight. */
    window->spawn_owed += (double)options->particle_count / lifetime * seconds;
    double front = xd_burn_get_front(window->progress) - xd_burn_get_edge() * 0.4;
    while (window->spawn_owed >= 1) {
        window->spawn_owed -= 1;
        uint64_t *seed = &particles->seed;
        double across = 0, down = 0;
        xd_burn_get_front_point(window->direction, front, xd_burn_get_random(seed), &across,
                                &down);
        xd_burn_particle particle = {
            .x = box->x + across * box->width + (xd_burn_get_random(seed) - 0.5) * 6,
            .y = box->y + down * box->height + (xd_burn_get_random(seed) - 0.5) * 6,
            .speed_x = (xd_burn_get_random(seed) - 0.5) * 50,
            .speed_y = -20 - xd_burn_get_random(seed) * 50,
            .lifetime = lifetime * (0.5 + 0.5 * xd_burn_get_random(seed)),
            .red = options->fire[0],
            .green = options->fire[1],
            .blue = options->fire[2],
        };
        particle.life = particle.lifetime;
        if (options->mystical) {
            float rgb[3];
            burn_get_hue(xd_burn_get_random(seed), rgb);
            particle.red = rgb[0];
            particle.green = rgb[1];
            particle.blue = rgb[2];
        }
        if (!xd_burn_add_particle(particles, &particle))
            window->spawn_owed = 0;
    }
}

static void burn_begin(xd_window_effect *effect) {
    burn_window *window = effect->data;
    xd_glfx_init_mesh(&window->mesh, 1, 1);
    size_t capacity = (size_t)window->state->options.particle_count;
    xd_burn_init_particles(&window->particles, capacity, ++window->state->seed);
    window->spark_positions = XD_NEW_ARRAY(float, capacity * 2);
    window->spark_colors = XD_NEW_ARRAY(float, capacity * 4);
    window->spark_sizes = XD_NEW_ARRAY(float, capacity);
}

static bool burn_update(xd_window_effect *effect, double seconds) {
    burn_window *window = effect->data;
    const burn_options *options = &window->state->options;
    window->progress += seconds * 1000.0 / XD_MAX(options->duration_ms, 1);
    xd_burn_step_particles(&window->particles, seconds, options->slowdown);
    if (window->progress < 1)
        burn_spawn(window, seconds);
    return window->progress < 1 || window->particles.count > 0;
}

static struct wlr_fbox burn_get_bounds(xd_window_effect *effect) {
    burn_window *window = effect->data;
    const struct wlr_fbox *box = &effect->texture.box;
    double left = box->x, top = box->y, right = box->x + box->width,
           bottom = box->y + box->height;
    double reach = window->state->options.particle_size + 1;
    for (size_t i = 0; i < window->particles.count; i++) {
        const xd_burn_particle *particle = &window->particles.items[i];
        left = fmin(left, particle->x - reach);
        top = fmin(top, particle->y - reach);
        right = fmax(right, particle->x + reach);
        bottom = fmax(bottom, particle->y + reach);
    }
    return (struct wlr_fbox){left - 1, top - 1, right - left + 2, bottom - top + 2};
}

static void burn_draw(xd_window_effect *effect, const xd_glfx_draw *draw) {
    burn_window *window = effect->data;
    if (window->progress < 1)
        burn_draw_window(window, draw);
    burn_draw_sparks(window, draw);
}

static void burn_finish(xd_window_effect *effect) {
    burn_window *window = effect->data;
    wl_list_remove(&window->link);
    xd_glfx_free_mesh(&window->mesh);
    xd_burn_free_particles(&window->particles);
    xd_free(window->spark_positions);
    xd_free(window->spark_colors);
    xd_free(window->spark_sizes);
    xd_free(window);
}

static const xd_window_effect_impl window_impl = {
    .name = "Burn",
    .begin = burn_begin,
    .update = burn_update,
    .get_bounds = burn_get_bounds,
    .draw = burn_draw,
    .done = burn_finish,
};

/* ---- the effect ----------------------------------------------------------------- */

static void *burn_enable(xd_server *server) {
    burn_state *state = XD_NEW(burn_state);
    state->server = server;
    state->seed = 0x2545F4914F6CDD1DULL;
    wl_list_init(&state->windows);
    return state;
}

static void burn_disable([[maybe_unused]] xd_server *server, void *data) {
    burn_state *state = data;
    burn_window *window, *next;
    wl_list_for_each_safe(window, next, &state->windows, link) {
        xd_glfx_end_window(window->effect); /* burn_finish frees it */
    }
    xd_free(state);
}

static void burn_configure([[maybe_unused]] xd_server *server, void *data,
                           const xd_effects_settings *settings, size_t effect) {
    burn_state *state = data;
    burn_options *options = &state->options;
    options->duration_ms = xd_effects_get_int(settings, effect, "Duration");
    options->direction = xd_effects_get_choice(settings, effect, "Direction");
    uint32_t color = xd_effects_get_color(settings, effect, "FireColor");
    options->fire[0] = (float)((color >> 24) & 0xFF) / 255.0f;
    options->fire[1] = (float)((color >> 16) & 0xFF) / 255.0f;
    options->fire[2] = (float)((color >> 8) & 0xFF) / 255.0f;
    options->mystical = xd_effects_get_bool(settings, effect, "MysticalFire");
    options->particle_count = xd_effects_get_int(settings, effect, "ParticleCount");
    options->particle_size = xd_effects_get_double(settings, effect, "ParticleSize");
    options->particle_life = xd_effects_get_double(settings, effect, "ParticleLife");
    options->slowdown = xd_effects_get_double(settings, effect, "Slowdown");
    snprintf(options->windows, sizeof(options->windows), "%s",
             xd_effects_get_text(settings, effect, "Windows"));
}

static void burn_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                     const xd_window_event *event) {
    burn_state *state = data;
    xd_view *view = event->view;
    if (event->kind != XD_WINDOW_EVENT_UNMAP || view->fullscreen ||
        !xd_effects_match_window(state->options.windows, xd_view_app_id(view),
                                 xd_view_title(view), view->dialog))
        return;
    burn_window *window = XD_NEW(burn_window);
    window->state = state;
    int direction = state->options.direction;
    if (direction < XD_BURN_DOWN || direction > XD_BURN_RIGHT)
        direction = (int)(xd_burn_get_random(&state->seed) * 4) % 4;
    window->direction = (xd_burn_direction)direction;
    window->effect = xd_glfx_begin_closing_window(view, &window_impl, window);
    if (window->effect == nullptr) {
        xd_free(window);
        return;
    }
    wl_list_insert(&state->windows, &window->link);
}

const xd_effect_impl xd_effect_impl_burn = {
    .id = "Burn",
    .enable = burn_enable,
    .disable = burn_disable,
    .configure = burn_configure,
    .window_event = burn_handle_window_event,
};
