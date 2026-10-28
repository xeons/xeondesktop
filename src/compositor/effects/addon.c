/* SPDX-License-Identifier: MIT
 * Animations Add-On (xd-compositor-fx): Compiz Fusion's animationaddon
 * effects for Animations' events. Animations' page picks an event's
 * effect, duration and windows; when that's one of ours (Airplane, Beam
 * Up, Domino, Fold, Glide 3, Leaf Spread, Razr, Skewer) and Animations is
 * on, we take the window before Animations does (impl_list.h's order) and
 * draw it, else Animations zooms it.
 *
 * The window's picture is cut into pieces (addon_model.h) drawn as one
 * mesh per frame, back to front: each piece a slab, its face (the window,
 * or seen from behind its mirror image, darker), and the sides facing the
 * viewer, in perspective with the texture kept true (each vertex's
 * texture coordinates times its growth, divided back per pixel). Beam Up
 * adds its beams, glowing streaks drawn on top.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../core/alloc.h"
#include "addon_model.h"
#include "animations_model.h"
#include "glfx.h"

/* x, y (layout units); u, v times grow, grow; shade, alpha, glow. */
enum { PIECE_FLOATS = 8, BEAM_FLOATS = 8, BATCH_VERTICES = 65535 };

static const char piece_vertex[] = "uniform mat4 projection;\n"
                                   "uniform vec2 texcoord_scale;\n"
                                   "attribute vec2 position;\n"
                                   "attribute vec3 texcoord;\n"
                                   "attribute vec3 tint;\n"
                                   "varying vec3 v_texcoord;\n"
                                   "varying vec3 v_tint;\n"
                                   "void main() {\n"
                                   "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                   "    v_texcoord = vec3(texcoord.xy * texcoord_scale, "
                                   "texcoord.z);\n"
                                   "    v_tint = tint;\n"
                                   "}\n";

/* Textures are premultiplied: the glow mixes toward the light as much of
 * it as the pixel covers. */
static const char piece_fragment[] =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform XD_SAMPLER texture0;\n"
    "uniform vec3 glow_color;\n"
    "varying vec3 v_texcoord;\n"
    "varying vec3 v_tint;\n"
    "void main() {\n"
    "    vec4 color = texture2D(texture0, v_texcoord.xy / v_texcoord.z);\n"
    "    color.rgb *= v_tint.x;\n"
    "    color.rgb = mix(color.rgb, glow_color * color.a, v_tint.z);\n"
    "    gl_FragColor = color * v_tint.y;\n"
    "}\n";

static const char beam_vertex[] = "uniform mat4 projection;\n"
                                  "attribute vec2 position;\n"
                                  "attribute vec2 place;\n"
                                  "attribute vec4 color;\n"
                                  "varying vec2 v_place;\n"
                                  "varying vec4 v_color;\n"
                                  "void main() {\n"
                                  "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                  "    v_place = place;\n"
                                  "    v_color = color;\n"
                                  "}\n";

/* Soft across, brightest a little above its foot. */
static const char beam_fragment[] = "precision mediump float;\n"
                                    "varying vec2 v_place;\n"
                                    "varying vec4 v_color;\n"
                                    "void main() {\n"
                                    "    float across = 1.0 - v_place.x * v_place.x;\n"
                                    "    float along = 4.0 * v_place.y * (1.0 - v_place.y);\n"
                                    "    gl_FragColor = v_color * across * across * along;\n"
                                    "}\n";

/* The face seen from behind, and the sides: darker. */
static constexpr double BACK_SHADE = 0.7;
static constexpr double SIDE_SHADE = 0.45;

typedef struct addon_piece_shader {
    bool tried;
    xd_glfx_shader shader;
    GLint tint, glow_color;
} addon_piece_shader;

/* Compiled once, for 2D and external textures; they go with the
 * renderer's context, as glfx's own do. */
static addon_piece_shader piece_shaders[2];
static bool beam_tried;
static xd_glfx_shader beam_shader;
static GLint beam_place = -1, beam_color = -1;

typedef enum addon_event : uint8_t {
    ADDON_EVENT_OPEN,
    ADDON_EVENT_CLOSE,
    ADDON_EVENT_MINIMIZE,
    ADDON_EVENT_UNMINIMIZE,
    ADDON_EVENT_COUNT,
} addon_event;

static const char *const addon_event_prefixes[ADDON_EVENT_COUNT] = {"Open", "Close", "Minimize",
                                                                    "Unminimize"};

/* Animations' settings for an event. */
typedef struct addon_event_options {
    xd_animation_effect effect;
    int duration_ms;
    char windows[512];
} addon_event_options;

typedef struct addon_state {
    xd_server *server;
    xd_addon_options options;
    float beam_color[3];
    bool animations_on;
    addon_event_options events[ADDON_EVENT_COUNT];
    struct wl_list windows; /* addon_window.link */
    uint64_t seed;
} addon_state;

/* A piece shown this frame, and how far it is from the eye (squared). */
typedef struct addon_depth {
    double distance;
    size_t index;
} addon_depth;

/* A run of vertices drawn with 16-bit indices. */
typedef struct addon_batch {
    size_t first_vertex, first_index, index_count;
} addon_batch;

typedef struct addon_window {
    struct wl_list link; /* addon_state.windows */
    addon_state *state;
    xd_window_effect *effect;
    xd_animation_effect kind;
    int duration_ms;
    double progress;
    bool appearing; /* progress runs back to 0 */
    bool has_target;
    struct wlr_box target; /* the taskbar button, layout units */
    xd_addon model;
    bool built;
    /* This frame's, from get_bounds for draw. */
    xd_addon_pose *poses;
    addon_depth *order;
    float *vertices;
    uint16_t *indices;
    size_t vertex_count, index_count, vertex_capacity, index_capacity;
    addon_batch *batches;
    size_t batch_count, batch_capacity;
    float *beam_vertices;
    uint16_t *beam_indices;
    size_t beam_vertex_count;
} addon_window;

static const xd_window_effect_impl window_impl;

/* ---- shaders -------------------------------------------------------------------- */

static const addon_piece_shader *addon_get_piece_shader(GLenum target) {
    addon_piece_shader *entry = &piece_shaders[target == GL_TEXTURE_EXTERNAL_OES ? 1 : 0];
    if (!entry->tried) {
        entry->tried = true;
        if (xd_glfx_init_shader(&entry->shader, piece_vertex, piece_fragment, target)) {
            entry->tint = glGetAttribLocation(entry->shader.program, "tint");
            entry->glow_color = glGetUniformLocation(entry->shader.program, "glow_color");
        }
    }
    const xd_glfx_shader *shader = &entry->shader;
    return shader->program != 0 && shader->position >= 0 && shader->texcoord >= 0 &&
                   entry->tint >= 0
               ? entry
               : nullptr;
}

static const xd_glfx_shader *addon_get_beam_shader(void) {
    if (!beam_tried) {
        beam_tried = true;
        if (xd_glfx_init_shader(&beam_shader, beam_vertex, beam_fragment, GL_TEXTURE_2D)) {
            beam_place = glGetAttribLocation(beam_shader.program, "place");
            beam_color = glGetAttribLocation(beam_shader.program, "color");
        }
    }
    return beam_shader.program != 0 && beam_shader.position >= 0 && beam_place >= 0 &&
                   beam_color >= 0
               ? &beam_shader
               : nullptr;
}

/* ---- building the frame's mesh ----------------------------------------------------- */

static void addon_add_batch(addon_window *window) {
    if (window->batch_count == window->batch_capacity) {
        size_t capacity = XD_MAX(window->batch_capacity * 2, (size_t)4);
        addon_batch *grown = XD_NEW_ARRAY(addon_batch, capacity);
        for (size_t i = 0; i < window->batch_count; i++)
            grown[i] = window->batches[i];
        xd_free(window->batches);
        window->batches = grown;
        window->batch_capacity = capacity;
    }
    window->batches[window->batch_count++] = (addon_batch){
        .first_vertex = window->vertex_count,
        .first_index = window->index_count,
    };
}

/* Room for a polygon of `corners` in the current batch (a new one when the
 * indices would overflow). */
static addon_batch *addon_reserve(addon_window *window, int corners) {
    addon_batch *batch = &window->batches[window->batch_count - 1];
    if (window->vertex_count - batch->first_vertex + (size_t)corners > BATCH_VERTICES) {
        addon_add_batch(window);
        batch = &window->batches[window->batch_count - 1];
    }
    return batch;
}

/* One vertex: `point` (window px, 3D) in perspective, showing the window
 * at `rest`. */
static void addon_emit_vertex(addon_window *window, const double point[3], const double rest[2],
                              double shade, double alpha, double glow, struct wlr_fbox *bounds) {
    const xd_addon *model = &window->model;
    const struct wlr_fbox *box = &window->effect->texture.box;
    double x = 0, y = 0, grow = 1;
    xd_addon_project(model, point, &x, &y, &grow);
    float *out = &window->vertices[window->vertex_count * PIECE_FLOATS];
    out[0] = (float)(box->x + x);
    out[1] = (float)(box->y + y);
    out[2] = (float)(rest[0] / model->width * grow);
    out[3] = (float)(rest[1] / model->height * grow);
    out[4] = (float)grow;
    out[5] = (float)shade;
    out[6] = (float)XD_CLAMP(alpha, 0.0, 1.0);
    out[7] = (float)glow;
    window->vertex_count++;
    double left = fmin(bounds->x, box->x + x), top = fmin(bounds->y, box->y + y);
    double right = fmax(bounds->x + bounds->width, box->x + x);
    double bottom = fmax(bounds->y + bounds->height, box->y + y);
    *bounds = (struct wlr_fbox){left, top, right - left, bottom - top};
}

/* A fan over the last `corners` vertices. */
static void addon_emit_fan(addon_window *window, const addon_batch *batch, int corners) {
    size_t first = window->vertex_count - (size_t)corners - batch->first_vertex;
    for (int corner = 1; corner + 1 < corners; corner++) {
        window->indices[window->index_count++] = (uint16_t)first;
        window->indices[window->index_count++] = (uint16_t)(first + (size_t)corner);
        window->indices[window->index_count++] = (uint16_t)(first + (size_t)corner + 1);
    }
    window->batches[window->batch_count - 1].index_count =
        window->index_count - window->batches[window->batch_count - 1].first_index;
}

static double addon_dot(const double left[3], const double right[3]) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

/* A piece as a slab: the sides that face the eye, then the face that
 * does (its back, mirrored and darker, once it has turned away). */
static void addon_emit_piece(addon_window *window, size_t index, struct wlr_fbox *bounds) {
    const xd_addon *model = &window->model;
    const xd_addon_piece *piece = &model->pieces[index];
    const xd_addon_pose *pose = &window->poses[index];
    int count = piece->corner_count;
    double thickness = model->thickness;
    double middle[3] = {0, 0, 0};
    double back[XD_ADDON_MAX_CORNERS][3];
    for (int i = 0; i < count; i++) {
        for (int axis = 0; axis < 3; axis++) {
            back[i][axis] = pose->corners[i][axis] - pose->normal[axis] * thickness;
            middle[axis] += (pose->corners[i][axis] + back[i][axis]) / (2.0 * count);
        }
    }
    double to_eye[3] = {model->eye[0] - middle[0], model->eye[1] - middle[1],
                        model->eye[2] - middle[2]};
    bool facing = addon_dot(pose->normal, to_eye) > 0;
    /* Lit from the front: turned away from it, darker. */
    double light = 0.55 + 0.45 * fabs(pose->normal[2]);
    if (thickness > 0.5) {
        for (int i = 0; i < count; i++) {
            int next = (i + 1) % count;
            double edge[3], side[3], side_middle[3], eye_way[3], outward[3];
            for (int axis = 0; axis < 3; axis++) {
                edge[axis] = pose->corners[next][axis] - pose->corners[i][axis];
                side_middle[axis] = (pose->corners[i][axis] + pose->corners[next][axis] +
                                     back[i][axis] + back[next][axis]) /
                                    4;
                eye_way[axis] = model->eye[axis] - side_middle[axis];
                outward[axis] = side_middle[axis] - middle[axis];
            }
            side[0] = edge[1] * pose->normal[2] - edge[2] * pose->normal[1];
            side[1] = edge[2] * pose->normal[0] - edge[0] * pose->normal[2];
            side[2] = edge[0] * pose->normal[1] - edge[1] * pose->normal[0];
            if (addon_dot(side, outward) < 0) {
                for (int axis = 0; axis < 3; axis++)
                    side[axis] = -side[axis];
            }
            if (addon_dot(side, eye_way) <= 0)
                continue;
            const addon_batch *batch = addon_reserve(window, 4);
            double shade = SIDE_SHADE * light;
            addon_emit_vertex(window, pose->corners[i], piece->corners[i], shade, pose->alpha[i],
                              pose->glow, bounds);
            addon_emit_vertex(window, pose->corners[next], piece->corners[next], shade,
                              pose->alpha[next], pose->glow, bounds);
            addon_emit_vertex(window, back[next], piece->corners[next], shade, pose->alpha[next],
                              pose->glow, bounds);
            addon_emit_vertex(window, back[i], piece->corners[i], shade, pose->alpha[i],
                              pose->glow, bounds);
            addon_emit_fan(window, batch, 4);
        }
    }
    const addon_batch *batch = addon_reserve(window, count);
    for (int i = 0; i < count; i++)
        addon_emit_vertex(window, facing ? pose->corners[i] : back[i], piece->corners[i],
                          facing ? light : BACK_SHADE * light, pose->alpha[i], pose->glow,
                          bounds);
    addon_emit_fan(window, batch, count);
}

static int addon_compare_depth(const void *left, const void *right) {
    double first = ((const addon_depth *)left)->distance;
    double second = ((const addon_depth *)right)->distance;
    return first < second ? 1 : first > second ? -1 : 0;
}

static void addon_emit_beams(addon_window *window, struct wlr_fbox *bounds) {
    const xd_addon *model = &window->model;
    const struct wlr_fbox *box = &window->effect->texture.box;
    const float *color = window->state->beam_color;
    window->beam_vertex_count = 0;
    for (size_t i = 0; i < model->beam_count; i++) {
        xd_addon_beam_state beam;
        if (!xd_addon_get_beam(model, i, window->progress, &beam) || beam.brightness <= 0.002)
            continue;
        double left = box->x + beam.x - beam.width / 2, right = left + beam.width;
        double top = box->y + beam.top, bottom = top + beam.height;
        const double corners[4][4] = {{left, top, -1, 0},
                                      {right, top, 1, 0},
                                      {right, bottom, 1, 1},
                                      {left, bottom, -1, 1}};
        float brightness = (float)beam.brightness;
        for (int corner = 0; corner < 4; corner++) {
            float *out = &window->beam_vertices[window->beam_vertex_count * BEAM_FLOATS];
            for (int value = 0; value < 4; value++)
                out[value] = (float)corners[corner][value];
            out[4] = color[0] * brightness;
            out[5] = color[1] * brightness;
            out[6] = color[2] * brightness;
            out[7] = brightness;
            window->beam_vertex_count++;
        }
        double from_x = fmin(bounds->x, left), from_y = fmin(bounds->y, top);
        double to_x = fmax(bounds->x + bounds->width, right);
        double to_y = fmax(bounds->y + bounds->height, bottom);
        *bounds = (struct wlr_fbox){from_x, from_y, to_x - from_x, to_y - from_y};
    }
}

/* ---- the window effect ----------------------------------------------------------- */

static xd_addon_effect addon_get_model_effect(xd_animation_effect effect) {
    switch (effect) {
    case XD_ANIMATION_AIRPLANE:
        return XD_ADDON_AIRPLANE;
    case XD_ANIMATION_BEAM_UP:
        return XD_ADDON_BEAM_UP;
    case XD_ANIMATION_DOMINO:
        return XD_ADDON_DOMINO;
    case XD_ANIMATION_FOLD:
        return XD_ADDON_FOLD;
    case XD_ANIMATION_GLIDE_3:
        return XD_ADDON_GLIDE_3;
    case XD_ANIMATION_LEAF_SPREAD:
        return XD_ADDON_LEAF_SPREAD;
    case XD_ANIMATION_RAZR:
        return XD_ADDON_RAZR;
    case XD_ANIMATION_SKEWER:
    case XD_ANIMATION_NONE:
    case XD_ANIMATION_FADE:
    case XD_ANIMATION_ZOOM:
    case XD_ANIMATION_GLIDE_1:
    case XD_ANIMATION_GLIDE_2:
    case XD_ANIMATION_BURN:
    case XD_ANIMATION_EXPLODE:
    case XD_ANIMATION_EFFECT_COUNT:
        break;
    }
    return XD_ADDON_SKEWER;
}

static void addon_begin(xd_window_effect *effect) {
    addon_window *window = effect->data;
    addon_state *state = window->state;
    const struct wlr_fbox *box = &effect->texture.box;
    xd_effect_rect target = {0};
    if (window->has_target)
        target = (xd_effect_rect){window->target.x - box->x, window->target.y - box->y,
                                  window->target.width, window->target.height};
    window->built = xd_addon_init(&window->model, addon_get_model_effect(window->kind),
                                  box->width, box->height, &state->options,
                                  window->has_target ? &target : nullptr, ++state->seed);
    if (!window->built)
        return;
    /* Compiz's eye: in front of the middle of the screen the window is
     * on. */
    struct wlr_output *output = wlr_output_layout_output_at(
        state->server->output_layout, box->x + box->width / 2, box->y + box->height / 2);
    if (output != nullptr) {
        struct wlr_box screen;
        wlr_output_layout_get_box(state->server->output_layout, output, &screen);
        xd_addon_set_eye(&window->model, screen.x + screen.width / 2.0 - box->x,
                         screen.y + screen.height / 2.0 - box->y,
                         xd_addon_get_eye_distance(screen.height));
    }
    size_t count = window->model.count;
    size_t vertices = 0, indices = 0;
    for (size_t i = 0; i < count; i++) {
        size_t corners = (size_t)window->model.pieces[i].corner_count;
        vertices += corners * 5;
        indices += (corners - 2) * 3 + corners * 6;
    }
    window->poses = XD_NEW_ARRAY(xd_addon_pose, count);
    window->order = XD_NEW_ARRAY(addon_depth, count);
    window->vertices = XD_NEW_ARRAY(float, vertices * PIECE_FLOATS);
    window->indices = XD_NEW_ARRAY(uint16_t, indices);
    window->vertex_capacity = vertices;
    window->index_capacity = indices;
    size_t beams = window->model.beam_count;
    if (beams > 0) {
        window->beam_vertices = XD_NEW_ARRAY(float, beams * 4 * BEAM_FLOATS);
        window->beam_indices = XD_NEW_ARRAY(uint16_t, XD_MIN(beams, (size_t)16383) * 6);
        for (size_t i = 0; i < XD_MIN(beams, (size_t)16383); i++) {
            const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
            for (size_t corner = 0; corner < 6; corner++)
                window->beam_indices[i * 6 + corner] = (uint16_t)(i * 4 + quad[corner]);
        }
    }
}

static bool addon_update(xd_window_effect *effect, double seconds) {
    addon_window *window = effect->data;
    if (!window->built)
        return false;
    double step = seconds * 1000.0 / XD_MAX(window->duration_ms, 1);
    window->progress += window->appearing ? -step : step;
    window->progress = XD_CLAMP(window->progress, 0.0, 1.0);
    return window->appearing ? window->progress > 0 : window->progress < 1;
}

static struct wlr_fbox addon_get_bounds(xd_window_effect *effect) {
    addon_window *window = effect->data;
    const xd_addon *model = &window->model;
    const struct wlr_fbox *box = &effect->texture.box;
    struct wlr_fbox bounds = {box->x + box->width / 2, box->y + box->height / 2, 0, 0};
    window->vertex_count = 0;
    window->index_count = 0;
    window->batch_count = 0;
    addon_add_batch(window);
    size_t shown = 0;
    for (size_t i = 0; i < model->count; i++) {
        xd_addon_pose *pose = &window->poses[i];
        xd_addon_get_pose(model, i, window->progress, pose);
        double alpha = 0, middle[3] = {0, 0, 0};
        int count = model->pieces[i].corner_count;
        for (int corner = 0; corner < count; corner++) {
            alpha = fmax(alpha, pose->alpha[corner]);
            for (int axis = 0; axis < 3; axis++)
                middle[axis] += pose->corners[corner][axis] / count;
        }
        if (alpha <= 0.002)
            continue;
        double away[3] = {middle[0] - model->eye[0], middle[1] - model->eye[1],
                          middle[2] - model->eye[2]};
        window->order[shown++] = (addon_depth){addon_dot(away, away), i};
    }
    /* Back to front: the farthest first. */
    qsort(window->order, shown, sizeof(addon_depth), addon_compare_depth);
    for (size_t i = 0; i < shown; i++)
        addon_emit_piece(window, window->order[i].index, &bounds);
    XD_ASSERT(window->vertex_count <= window->vertex_capacity &&
              window->index_count <= window->index_capacity);
    if (model->beam_count > 0)
        addon_emit_beams(window, &bounds);
    return (struct wlr_fbox){bounds.x - 1, bounds.y - 1, bounds.width + 2, bounds.height + 2};
}

static void addon_draw_pieces(addon_window *window, const xd_glfx_draw *draw) {
    const xd_glfx_texture *texture = &window->effect->texture;
    const addon_piece_shader *entry = addon_get_piece_shader(texture->target);
    if (entry == nullptr || window->index_count == 0)
        return;
    const xd_glfx_shader *shader = &entry->shader;
    glUseProgram(shader->program);
    glUniformMatrix4fv(shader->projection, 1, GL_FALSE, draw->projection);
    if (shader->texcoord_scale >= 0)
        glUniform2f(shader->texcoord_scale, texture->u_scale, texture->v_scale);
    const float *glow = window->state->beam_color;
    glUniform3f(entry->glow_color, glow[0], glow[1], glow[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(texture->target, texture->id);
    glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(texture->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(texture->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (shader->texture >= 0)
        glUniform1i(shader->texture, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLuint position = (GLuint)shader->position, texcoord = (GLuint)shader->texcoord,
           tint = (GLuint)entry->tint;
    glEnableVertexAttribArray(position);
    glEnableVertexAttribArray(texcoord);
    glEnableVertexAttribArray(tint);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    GLsizei stride = (GLsizei)(PIECE_FLOATS * sizeof(float));
    for (size_t i = 0; i < window->batch_count; i++) {
        const addon_batch *batch = &window->batches[i];
        if (batch->index_count == 0)
            continue;
        const float *base = &window->vertices[batch->first_vertex * PIECE_FLOATS];
        glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, stride, base);
        glVertexAttribPointer(texcoord, 3, GL_FLOAT, GL_FALSE, stride, base + 2);
        glVertexAttribPointer(tint, 3, GL_FLOAT, GL_FALSE, stride, base + 5);
        glDrawElements(GL_TRIANGLES, (GLsizei)batch->index_count, GL_UNSIGNED_SHORT,
                       &window->indices[batch->first_index]);
    }
    glDisableVertexAttribArray(position);
    glDisableVertexAttribArray(texcoord);
    glDisableVertexAttribArray(tint);
    glBindTexture(texture->target, 0);
    glUseProgram(0);
}

static void addon_draw_beams(addon_window *window, const xd_glfx_draw *draw) {
    const xd_glfx_shader *shader = addon_get_beam_shader();
    size_t quads = XD_MIN(window->beam_vertex_count / 4, (size_t)16383);
    if (shader == nullptr || quads == 0)
        return;
    glUseProgram(shader->program);
    glUniformMatrix4fv(shader->projection, 1, GL_FALSE, draw->projection);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLuint position = (GLuint)shader->position, place = (GLuint)beam_place,
           color = (GLuint)beam_color;
    GLsizei stride = (GLsizei)(BEAM_FLOATS * sizeof(float));
    glEnableVertexAttribArray(position);
    glEnableVertexAttribArray(place);
    glEnableVertexAttribArray(color);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, stride, window->beam_vertices);
    glVertexAttribPointer(place, 2, GL_FLOAT, GL_FALSE, stride, window->beam_vertices + 2);
    glVertexAttribPointer(color, 4, GL_FLOAT, GL_FALSE, stride, window->beam_vertices + 4);
    /* Light adds up. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDrawElements(GL_TRIANGLES, (GLsizei)(quads * 6), GL_UNSIGNED_SHORT, window->beam_indices);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisableVertexAttribArray(position);
    glDisableVertexAttribArray(place);
    glDisableVertexAttribArray(color);
    glUseProgram(0);
}

static void addon_draw(xd_window_effect *effect, const xd_glfx_draw *draw) {
    addon_window *window = effect->data;
    addon_draw_pieces(window, draw);
    addon_draw_beams(window, draw);
}

static void addon_finish(xd_window_effect *effect) {
    addon_window *window = effect->data;
    wl_list_remove(&window->link);
    xd_addon_free(&window->model);
    xd_free(window->poses);
    xd_free(window->order);
    xd_free(window->vertices);
    xd_free(window->indices);
    xd_free(window->batches);
    xd_free(window->beam_vertices);
    xd_free(window->beam_indices);
    xd_free(window);
}

static const xd_window_effect_impl window_impl = {
    .name = "Animations Add-On",
    .begin = addon_begin,
    .update = addon_update,
    .get_bounds = addon_get_bounds,
    .draw = addon_draw,
    .done = addon_finish,
};

/* ---- the effect ----------------------------------------------------------------- */

static bool addon_get_event(xd_window_event_kind kind, addon_event *event) {
    switch (kind) {
    case XD_WINDOW_EVENT_MAP:
        *event = ADDON_EVENT_OPEN;
        return true;
    case XD_WINDOW_EVENT_UNMAP:
        *event = ADDON_EVENT_CLOSE;
        return true;
    case XD_WINDOW_EVENT_MINIMIZE:
        *event = ADDON_EVENT_MINIMIZE;
        return true;
    case XD_WINDOW_EVENT_UNMINIMIZE:
        *event = ADDON_EVENT_UNMINIMIZE;
        return true;
    case XD_WINDOW_EVENT_DESTROY:
    case XD_WINDOW_EVENT_FOCUS:
    case XD_WINDOW_EVENT_MAXIMIZE:
    case XD_WINDOW_EVENT_TITLE:
        return false;
    }
    return false;
}

/* Whether Animations has one of our effects for this window's event. */
static bool addon_is_wanted(const addon_state *state, const xd_view *view,
                            xd_window_event_kind kind) {
    addon_event event;
    if (!state->animations_on || !addon_get_event(kind, &event) || view->fullscreen)
        return false;
    const addon_event_options *options = &state->events[event];
    return xd_animation_is_addon(options->effect) &&
           xd_effects_match_view(options->windows, view);
}

static void addon_start(addon_state *state, xd_view *view, addon_event event) {
    const addon_event_options *options = &state->events[event];
    bool closing = event == ADDON_EVENT_CLOSE || event == ADDON_EVENT_MINIMIZE;
    bool minimizing = event == ADDON_EVENT_MINIMIZE || event == ADDON_EVENT_UNMINIMIZE;
    addon_window *window = XD_NEW(addon_window);
    window->state = state;
    window->kind = options->effect;
    window->duration_ms = options->duration_ms;
    window->appearing = !closing;
    window->progress = closing ? 0.0 : 1.0;
    /* Turned round midway (restored while it's going, minimized again
     * while it's coming back): from where the one before had got to. */
    xd_window_effect *before = xd_glfx_find_window(view, &window_impl);
    if (before != nullptr) {
        const addon_window *previous = before->data;
        if (previous->kind == window->kind && previous->built)
            window->progress = previous->progress;
    }
    if (minimizing && window->kind == XD_ANIMATION_AIRPLANE &&
        state->options.airplane_fly_to_taskbar && view->has_button_box) {
        window->has_target = true;
        window->target = view->button_box;
    }
    window->effect = closing ? xd_glfx_begin_closing_window(view, &window_impl, window)
                             : xd_glfx_begin_window(view, &window_impl, window);
    if (window->effect == nullptr) {
        xd_free(window);
        return;
    }
    wl_list_insert(&state->windows, &window->link);
}

static void addon_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                      const xd_window_event *event) {
    addon_state *state = data;
    xd_view *view = event->view;
    addon_event kind;
    if (!addon_get_event(event->kind, &kind) || !addon_is_wanted(state, view, event->kind))
        return;
    /* Magic Lamp, Burn and Explode, before us, keep the windows they take. */
    if (xd_effects_is_animated_before(&xd_effect_impl_animations_addon, view, event->kind))
        return;
    addon_start(state, view, kind);
}

static bool addon_animates([[maybe_unused]] xd_server *server, void *data, const xd_view *view,
                           xd_window_event_kind kind) {
    return addon_is_wanted(data, view, kind);
}

static void *addon_enable(xd_server *server) {
    addon_state *state = XD_NEW(addon_state);
    state->server = server;
    state->seed = 0xD1B54A32D192ED03ULL;
    wl_list_init(&state->windows);
    xd_addon_get_default_options(&state->options);
    return state;
}

static void addon_disable([[maybe_unused]] xd_server *server, void *data) {
    addon_state *state = data;
    addon_window *window, *next;
    wl_list_for_each_safe(window, next, &state->windows, link) {
        xd_glfx_end_window(window->effect); /* addon_finish frees it */
    }
    xd_free(state);
}

static xd_addon_direction addon_get_direction(int choice, bool skewer) {
    /* The pages' orders: Up, Down, Left, Right, Random; Skewer's Left,
     * Right, Up, Down, In, Out, Left and right, Up and down, In and out,
     * Random. */
    static const xd_addon_direction dominoes[] = {XD_ADDON_UP, XD_ADDON_DOWN, XD_ADDON_LEFT,
                                                  XD_ADDON_RIGHT, XD_ADDON_RANDOM};
    static const xd_addon_direction skewers[] = {
        XD_ADDON_LEFT, XD_ADDON_RIGHT,      XD_ADDON_UP,      XD_ADDON_DOWN,   XD_ADDON_IN,
        XD_ADDON_OUT,  XD_ADDON_LEFT_RIGHT, XD_ADDON_UP_DOWN, XD_ADDON_IN_OUT, XD_ADDON_RANDOM};
    if (skewer)
        return choice >= 0 && (size_t)choice < XD_ARRAY_LEN(skewers) ? skewers[choice]
                                                                      : XD_ADDON_LEFT_RIGHT;
    return choice >= 0 && (size_t)choice < XD_ARRAY_LEN(dominoes) ? dominoes[choice]
                                                                   : XD_ADDON_DOWN;
}

static void addon_configure([[maybe_unused]] xd_server *server, void *data,
                            const xd_effects_settings *settings, size_t effect) {
    addon_state *state = data;
    xd_addon_options *options = &state->options;
    options->airplane_path_length = xd_effects_get_double(settings, effect, "AirplanePathLength");
    options->airplane_fly_to_taskbar =
        xd_effects_get_bool(settings, effect, "AirplaneFlyToTaskbar");
    options->beam_size = xd_effects_get_double(settings, effect, "BeamSize");
    options->beam_spacing = xd_effects_get_int(settings, effect, "BeamSpacing");
    options->beam_life = xd_effects_get_double(settings, effect, "BeamLife");
    options->beam_slowdown = xd_effects_get_double(settings, effect, "BeamSlowdown");
    uint32_t color = xd_effects_get_color(settings, effect, "BeamColor");
    state->beam_color[0] = (float)((color >> 24) & 0xFF) / 255.0f;
    state->beam_color[1] = (float)((color >> 16) & 0xFF) / 255.0f;
    state->beam_color[2] = (float)((color >> 8) & 0xFF) / 255.0f;
    options->domino_direction =
        addon_get_direction(xd_effects_get_choice(settings, effect, "DominoDirection"), false);
    options->domino_piece_size = xd_effects_get_int(settings, effect, "DominoPieceSize");
    options->razr_direction =
        addon_get_direction(xd_effects_get_choice(settings, effect, "RazrDirection"), false);
    options->razr_piece_size = xd_effects_get_int(settings, effect, "RazrPieceSize");
    options->fold_columns = xd_effects_get_int(settings, effect, "FoldGridWidth");
    options->fold_rows = xd_effects_get_int(settings, effect, "FoldGridHeight");
    options->fold_count = xd_effects_get_int(settings, effect, "FoldCount");
    options->fold_out = xd_effects_get_choice(settings, effect, "FoldDirection") == 1;
    options->glide_away_position = xd_effects_get_double(settings, effect, "Glide3AwayPosition");
    options->glide_away_angle = xd_effects_get_int(settings, effect, "Glide3AwayAngle");
    options->glide_thickness = xd_effects_get_int(settings, effect, "Glide3Thickness");
    options->leaf_spread = xd_effects_get_double(settings, effect, "LeafSpread");
    options->skewer_direction =
        addon_get_direction(xd_effects_get_choice(settings, effect, "SkewerDirection"), true);
    options->skewer_tessellation =
        xd_effects_get_choice(settings, effect, "SkewerTessellation") == 1
            ? XD_EXPLODE_HEXAGONAL
            : XD_EXPLODE_RECTANGULAR;
    options->skewer_across = xd_effects_get_int(settings, effect, "SkewerPiecesAcross");
    options->skewer_down = xd_effects_get_int(settings, effect, "SkewerPiecesDown");
    options->skewer_thickness = xd_effects_get_int(settings, effect, "SkewerThickness");
    options->skewer_rotation = xd_effects_get_int(settings, effect, "SkewerRotation");
    options->skewer_depth = xd_effects_get_double(settings, effect, "SkewerDepth");
    xd_addon_clamp_options(options);

    /* Animations' events say which windows are ours: its settings come
     * with ours (every effect is told of every change). */
    size_t animations = 0;
    state->animations_on = xd_effects_find(settings, "Animations", &animations) &&
                           xd_effects_is_enabled(settings, animations);
    for (size_t i = 0; i < ADDON_EVENT_COUNT; i++) {
        addon_event_options *event = &state->events[i];
        *event = (addon_event_options){0};
        if (!state->animations_on)
            continue;
        char key[32];
        snprintf(key, sizeof(key), "%sEffect", addon_event_prefixes[i]);
        event->effect =
            xd_animation_find_effect(xd_effects_get_choice_name(settings, animations, key));
        snprintf(key, sizeof(key), "%sDuration", addon_event_prefixes[i]);
        event->duration_ms = xd_effects_get_int(settings, animations, key);
        snprintf(key, sizeof(key), "%sWindows", addon_event_prefixes[i]);
        snprintf(event->windows, sizeof(event->windows), "%s",
                 xd_effects_get_text(settings, animations, key));
    }
}

const xd_effect_impl xd_effect_impl_animations_addon = {
    .id = "AnimationsAddOn",
    .enable = addon_enable,
    .disable = addon_disable,
    .configure = addon_configure,
    .window_event = addon_handle_window_event,
    .animates = addon_animates,
};
