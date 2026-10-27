/* SPDX-License-Identifier: MIT
 * Explode (xd-compositor-fx): a closing window's last picture is cut into
 * pieces (explode_model.h) that fly out, spin and fade, drawn as one mesh
 * of triangle fans. Thickness is faked: each piece's darker copy, pushed
 * the way it's tilted, shows as its edge.
 */
#include <math.h>

#include "../../core/alloc.h"
#include "explode_model.h"
#include "glfx.h"

/* The built-in shader's, darkened by `shade` for the pieces' edges. */
static const char shade_fragment[] = "precision mediump float;\n"
                                     "uniform XD_SAMPLER texture0;\n"
                                     "uniform float alpha;\n"
                                     "uniform float shade;\n"
                                     "varying vec2 v_texcoord;\n"
                                     "void main() {\n"
                                     "    vec4 color = texture2D(texture0, v_texcoord) * alpha;\n"
                                     "    gl_FragColor = vec4(color.rgb * shade, color.a);\n"
                                     "}\n";

static const char shade_vertex[] = "uniform mat4 projection;\n"
                                   "uniform vec2 texcoord_scale;\n"
                                   "attribute vec2 position;\n"
                                   "attribute vec2 texcoord;\n"
                                   "varying vec2 v_texcoord;\n"
                                   "void main() {\n"
                                   "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                   "    v_texcoord = texcoord * texcoord_scale;\n"
                                   "}\n";

/* How dark a piece's edge is. */
static constexpr float EDGE_SHADE = 0.45f;

/* Compiled once, for 2D and external textures; they go with the
 * renderer's context, as glfx's own do. */
typedef struct explode_shader {
    bool tried;
    xd_glfx_shader shader;
    GLint shade;
} explode_shader;

static explode_shader shaders[2];

typedef struct explode_options {
    int duration_ms;
    xd_explode_tessellation tessellation;
    int across, down, spokes, tiers, thickness;
    char windows[512]; /* which windows: xd_effects_match_window */
} explode_options;

typedef struct explode_state {
    xd_server *server;
    explode_options options;
    struct wl_list windows; /* explode_window.link */
    uint64_t seed;
} explode_state;

typedef struct explode_window {
    struct wl_list link; /* explode_state.windows */
    explode_state *state;
    xd_window_effect *effect;
    xd_explode explode;
    double progress;
    xd_glfx_mesh mesh; /* every piece's fan; positions are the faces' */
    float *face_positions, *edge_positions;
} explode_window;

static const xd_window_effect_impl window_impl;

static const explode_shader *explode_get_shader(GLenum target) {
    explode_shader *entry = &shaders[target == GL_TEXTURE_EXTERNAL_OES ? 1 : 0];
    if (!entry->tried) {
        entry->tried = true;
        if (xd_glfx_init_shader(&entry->shader, shade_vertex, shade_fragment, target))
            entry->shade = glGetUniformLocation(entry->shader.program, "shade");
    }
    return entry->shader.program != 0 ? entry : nullptr;
}

/* ---- the window effect ----------------------------------------------------------- */

/* One fan of triangles a piece, its texture coordinates its corners'. */
static void explode_build_mesh(explode_window *window) {
    const xd_explode *explode = &window->explode;
    size_t vertices = 0, indices = 0;
    for (size_t i = 0; i < explode->count; i++) {
        vertices += (size_t)explode->pieces[i].corner_count;
        indices += (size_t)(explode->pieces[i].corner_count - 2) * 3;
    }
    xd_glfx_mesh *mesh = &window->mesh;
    *mesh = (xd_glfx_mesh){.vertex_count = vertices, .index_count = indices};
    mesh->texcoords = XD_NEW_ARRAY(float, vertices * 2);
    mesh->indices = XD_NEW_ARRAY(uint16_t, indices);
    window->face_positions = XD_NEW_ARRAY(float, vertices * 2);
    window->edge_positions = XD_NEW_ARRAY(float, vertices * 2);
    mesh->positions = window->face_positions;
    size_t vertex = 0, index = 0;
    for (size_t i = 0; i < explode->count; i++) {
        const xd_explode_piece *piece = &explode->pieces[i];
        size_t first = vertex;
        for (int corner = 0; corner < piece->corner_count; corner++) {
            mesh->texcoords[vertex * 2] = (float)(piece->corners[corner][0] / explode->width);
            mesh->texcoords[vertex * 2 + 1] =
                (float)(piece->corners[corner][1] / explode->height);
            vertex++;
        }
        for (int corner = 1; corner + 1 < piece->corner_count; corner++) {
            mesh->indices[index++] = (uint16_t)first;
            mesh->indices[index++] = (uint16_t)(first + (size_t)corner);
            mesh->indices[index++] = (uint16_t)(first + (size_t)corner + 1);
        }
    }
}

static void explode_begin(xd_window_effect *effect) {
    explode_window *window = effect->data;
    const explode_options *options = &window->state->options;
    const struct wlr_fbox *box = &effect->texture.box;
    xd_explode_init(&window->explode, options->tessellation, box->width, box->height,
                    options->across, options->down, options->spokes, options->tiers,
                    ++window->state->seed);
    explode_build_mesh(window);
}

static bool explode_update(xd_window_effect *effect, double seconds) {
    explode_window *window = effect->data;
    window->progress += seconds * 1000.0 / XD_MAX(window->state->options.duration_ms, 1);
    /* 16-bit indices: a window cut finer than that just goes. */
    return window->progress < 1 && window->mesh.vertex_count > 0 &&
           window->mesh.vertex_count <= UINT16_MAX;
}

static struct wlr_fbox explode_get_bounds(xd_window_effect *effect) {
    explode_window *window = effect->data;
    const xd_explode *explode = &window->explode;
    const struct wlr_fbox *box = &effect->texture.box;
    double thickness = window->state->options.thickness;
    size_t vertex = 0;
    for (size_t i = 0; i < explode->count; i++) {
        const xd_explode_piece *piece = &explode->pieces[i];
        double normal[3];
        xd_explode_get_normal(piece, window->progress, normal);
        for (int corner = 0; corner < piece->corner_count; corner++) {
            double x = 0, y = 0;
            xd_explode_get_corner(explode, piece, corner, window->progress, &x, &y);
            window->face_positions[vertex * 2] = (float)(box->x + x);
            window->face_positions[vertex * 2 + 1] = (float)(box->y + y);
            window->edge_positions[vertex * 2] = (float)(box->x + x - normal[0] * thickness);
            window->edge_positions[vertex * 2 + 1] =
                (float)(box->y + y - normal[1] * thickness);
            vertex++;
        }
    }
    xd_glfx_mesh *mesh = &window->mesh;
    mesh->positions = window->face_positions;
    struct wlr_fbox faces = xd_glfx_get_mesh_bounds(mesh);
    mesh->positions = window->edge_positions;
    struct wlr_fbox edges = xd_glfx_get_mesh_bounds(mesh);
    mesh->positions = window->face_positions;
    double left = fmin(faces.x, edges.x), top = fmin(faces.y, edges.y);
    double right = fmax(faces.x + faces.width, edges.x + edges.width);
    double bottom = fmax(faces.y + faces.height, edges.y + edges.height);
    return (struct wlr_fbox){left - 1, top - 1, right - left + 2, bottom - top + 2};
}

static void explode_draw(xd_window_effect *effect, const xd_glfx_draw *draw) {
    explode_window *window = effect->data;
    const explode_shader *entry = explode_get_shader(effect->texture.target);
    if (entry == nullptr)
        return;
    float alpha = (float)xd_explode_get_alpha(window->progress);
    xd_glfx_mesh *mesh = &window->mesh;
    if (window->state->options.thickness > 0) {
        mesh->positions = window->edge_positions;
        glUseProgram(entry->shader.program);
        glUniform1f(entry->shade, EDGE_SHADE);
        xd_glfx_draw_mesh_with(draw, mesh, &effect->texture, &entry->shader, alpha);
    }
    mesh->positions = window->face_positions;
    glUseProgram(entry->shader.program);
    glUniform1f(entry->shade, 1.0f);
    xd_glfx_draw_mesh_with(draw, mesh, &effect->texture, &entry->shader, alpha);
}

static void explode_finish(xd_window_effect *effect) {
    explode_window *window = effect->data;
    wl_list_remove(&window->link);
    window->mesh.positions = nullptr; /* one of these */
    xd_glfx_free_mesh(&window->mesh);
    xd_free(window->face_positions);
    xd_free(window->edge_positions);
    xd_explode_free(&window->explode);
    xd_free(window);
}

static const xd_window_effect_impl window_impl = {
    .name = "Explode",
    .begin = explode_begin,
    .update = explode_update,
    .get_bounds = explode_get_bounds,
    .draw = explode_draw,
    .done = explode_finish,
};

/* ---- the effect ----------------------------------------------------------------- */

static void *explode_enable(xd_server *server) {
    explode_state *state = XD_NEW(explode_state);
    state->server = server;
    state->seed = 0x9E3779B97F4A7C15ULL;
    wl_list_init(&state->windows);
    return state;
}

static void explode_disable([[maybe_unused]] xd_server *server, void *data) {
    explode_state *state = data;
    explode_window *window, *next;
    wl_list_for_each_safe(window, next, &state->windows, link) {
        xd_glfx_end_window(window->effect); /* explode_finish frees it */
    }
    xd_free(state);
}

static void explode_configure([[maybe_unused]] xd_server *server, void *data,
                              const xd_effects_settings *settings, size_t effect) {
    explode_state *state = data;
    explode_options *options = &state->options;
    options->duration_ms = xd_effects_get_int(settings, effect, "Duration");
    int tessellation = xd_effects_get_choice(settings, effect, "Tessellation");
    options->tessellation = tessellation == 1   ? XD_EXPLODE_HEXAGONAL
                            : tessellation == 2 ? XD_EXPLODE_GLASS
                                                : XD_EXPLODE_RECTANGULAR;
    options->across = xd_effects_get_int(settings, effect, "PiecesAcross");
    options->down = xd_effects_get_int(settings, effect, "PiecesDown");
    options->spokes = xd_effects_get_int(settings, effect, "Spokes");
    options->tiers = xd_effects_get_int(settings, effect, "Tiers");
    options->thickness = xd_effects_get_int(settings, effect, "Thickness");
    snprintf(options->windows, sizeof(options->windows), "%s",
             xd_effects_get_text(settings, effect, "Windows"));
}

static void explode_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                        const xd_window_event *event) {
    explode_state *state = data;
    xd_view *view = event->view;
    if (event->kind != XD_WINDOW_EVENT_UNMAP || view->fullscreen ||
        !xd_effects_match_window(state->options.windows, xd_view_app_id(view),
                                 xd_view_title(view), view->dialog))
        return;
    explode_window *window = XD_NEW(explode_window);
    window->state = state;
    window->effect = xd_glfx_begin_closing_window(view, &window_impl, window);
    if (window->effect == nullptr) {
        xd_free(window);
        return;
    }
    wl_list_insert(&state->windows, &window->link);
}

const xd_effect_impl xd_effect_impl_explode = {
    .id = "Explode",
    .enable = explode_enable,
    .disable = explode_disable,
    .configure = explode_configure,
    .window_event = explode_handle_window_event,
};
