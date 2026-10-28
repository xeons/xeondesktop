/* SPDX-License-Identifier: MIT
 * The OpenGL effect path (glfx.h): windows captured into textures and
 * drawn by effects into buffers the scene graph shows in their place.
 */
#include "glfx.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "../../core/alloc.h"

/* Device px: no texture or buffer of ours is bigger (GLES2 guarantees far
 * less, but every renderer SceneFX runs on has this). */
static constexpr int MAX_PX = 8192;
/* Buffers are a whole number of these px across and down, the part used
 * drawn from the top left: llvmpipe, drawing into or reading a buffer from
 * wlroots' udmabuf allocator (no padding), runs past the end of one that
 * isn't, and crashes. The target grows in bigger steps, so a wobbling
 * window's changing bounds don't mean a new buffer each frame. */
static constexpr int CAPTURE_STEP = 64;
static constexpr int TARGET_STEP = 256;
/* Frames of two outputs closer than this are one frame for the effects. */
static constexpr double SAME_FRAME_SECONDS = 0.002;
/* XD_EFFECTS_TIMING: frame times logged over this long. */
static constexpr double TIMING_SECONDS = 2.0;

typedef struct xd_glfx {
    xd_server *server;
    /* Every shader made (xd_glfx_init_shader) and not destroyed yet. */
    xd_glfx_shader **shaders;
    size_t shader_count, shader_capacity;
    struct wl_list windows; /* xd_window_effect.link */
    struct wl_list filters; /* xd_output_filter.link */
    struct wl_list filtered_outputs; /* filtered_output.link */
    double last_filter_frame;
    xd_glfx_shader shader_2d, shader_external;
    bool shaders_tried;
    double last_frame;
    /* XD_EFFECTS_TIMING */
    bool timing;
    double timing_start, last_done, frame_start;
    double draw_total, interval_total, interval_max, frame_total;
    int frames;
} xd_glfx;

static bool glfx_has_active_filters(const xd_glfx *glfx);
static void glfx_update_filters(xd_glfx *glfx, double now);

/* Rounded down or up to whole units, clamped to what our buffers hold. */
static int glfx_round_down(double value) {
    double floored = floor(value);
    return floored < -MAX_PX ? -MAX_PX : floored > MAX_PX ? MAX_PX : (int)floored;
}

static int glfx_round_up(double value) {
    double ceiled = ceil(value);
    return ceiled < -MAX_PX ? -MAX_PX : ceiled > MAX_PX ? MAX_PX : (int)ceiled;
}

static double glfx_get_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* ---- shaders -------------------------------------------------------------------- */

static const char default_vertex[] = "uniform mat4 projection;\n"
                                     "uniform vec2 texcoord_scale;\n"
                                     "attribute vec2 position;\n"
                                     "attribute vec2 texcoord;\n"
                                     "varying vec2 v_texcoord;\n"
                                     "void main() {\n"
                                     "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                     "    v_texcoord = texcoord * texcoord_scale;\n"
                                     "}\n";

/* Textures are premultiplied: fading scales every channel. */
static const char default_fragment[] =
    "precision mediump float;\n"
    "uniform XD_SAMPLER texture0;\n"
    "uniform float alpha;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(texture0, v_texcoord) * alpha;\n"
    "}\n";

static GLuint glfx_compile_stage(GLenum stage, const char *prelude, const char *source) {
    GLuint shader = glCreateShader(stage);
    const char *parts[2] = {prelude, source};
    glShaderSource(shader, 2, parts, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = "";
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), nullptr, log);
        wlr_log(WLR_ERROR, "effects: cannot compile a %s shader: %s",
                stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* The effects' shaders are made the first time a pass needs them, by
 * whichever effect, mostly into static storage: the framework keeps them
 * to destroy with the renderer's context current at the end (glfx.h). */
static xd_glfx *owner;

static void glfx_own_shader(xd_glfx_shader *shader) {
    if (owner == nullptr)
        return;
    if (owner->shader_count == owner->shader_capacity) {
        owner->shader_capacity = XD_MAX(owner->shader_capacity * 2, (size_t)16);
        owner->shaders =
            xd_realloc_array(owner->shaders, owner->shader_capacity, sizeof(*owner->shaders));
    }
    owner->shaders[owner->shader_count++] = shader;
}

bool xd_glfx_init_shader(xd_glfx_shader *shader, const char *vertex, const char *fragment,
                         GLenum target) {
    *shader = (xd_glfx_shader){0};
    const char *prelude = target == GL_TEXTURE_EXTERNAL_OES
                              ? "#extension GL_OES_EGL_image_external : require\n"
                                "#define XD_SAMPLER samplerExternalOES\n"
                              : "#define XD_SAMPLER sampler2D\n";
    GLuint vertex_shader = glfx_compile_stage(GL_VERTEX_SHADER, "", vertex);
    GLuint fragment_shader = glfx_compile_stage(GL_FRAGMENT_SHADER, prelude, fragment);
    if (vertex_shader == 0 || fragment_shader == 0) {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    glDetachShader(program, vertex_shader);
    glDetachShader(program, fragment_shader);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = "";
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), nullptr, log);
        wlr_log(WLR_ERROR, "effects: cannot link a shader: %s", log);
        glDeleteProgram(program);
        return false;
    }
    shader->program = program;
    glfx_own_shader(shader);
    shader->position = glGetAttribLocation(program, "position");
    shader->texcoord = glGetAttribLocation(program, "texcoord");
    shader->projection = glGetUniformLocation(program, "projection");
    shader->texcoord_scale = glGetUniformLocation(program, "texcoord_scale");
    shader->texture = glGetUniformLocation(program, "texture0");
    shader->alpha = glGetUniformLocation(program, "alpha");
    return true;
}

void xd_glfx_destroy_shader(xd_glfx_shader *shader) {
    if (shader->program == 0)
        return;
    glDeleteProgram(shader->program);
    for (size_t i = 0; owner != nullptr && i < owner->shader_count; i++) {
        if (owner->shaders[i] == shader) {
            owner->shaders[i] = owner->shaders[--owner->shader_count];
            break;
        }
    }
    *shader = (xd_glfx_shader){0};
}

/* ---- meshes --------------------------------------------------------------------- */

bool xd_glfx_init_mesh(xd_glfx_mesh *mesh, int columns, int rows) {
    *mesh = (xd_glfx_mesh){0};
    size_t vertices = 0, cells = 0, indices = 0;
    if (columns < 1 || rows < 1 ||
        ckd_mul(&vertices, (size_t)columns + 1, (size_t)rows + 1) || vertices > UINT16_MAX ||
        ckd_mul(&cells, (size_t)columns, (size_t)rows) || ckd_mul(&indices, cells, 6))
        return false;
    mesh->columns = columns;
    mesh->rows = rows;
    mesh->vertex_count = vertices;
    mesh->index_count = indices;
    mesh->positions = XD_NEW_ARRAY(float, vertices * 2);
    mesh->texcoords = XD_NEW_ARRAY(float, vertices * 2);
    mesh->indices = XD_NEW_ARRAY(uint16_t, indices);
    for (int row = 0; row <= rows; row++) {
        for (int column = 0; column <= columns; column++) {
            size_t vertex = xd_glfx_get_mesh_vertex(mesh, column, row);
            mesh->texcoords[vertex * 2] = (float)column / (float)columns;
            mesh->texcoords[vertex * 2 + 1] = (float)row / (float)rows;
        }
    }
    size_t next = 0;
    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            uint16_t top_left = (uint16_t)xd_glfx_get_mesh_vertex(mesh, column, row);
            uint16_t top_right = (uint16_t)xd_glfx_get_mesh_vertex(mesh, column + 1, row);
            uint16_t bottom_left = (uint16_t)xd_glfx_get_mesh_vertex(mesh, column, row + 1);
            uint16_t bottom_right = (uint16_t)xd_glfx_get_mesh_vertex(mesh, column + 1, row + 1);
            const uint16_t cell[6] = {top_left,  top_right,    bottom_left,
                                      top_right, bottom_right, bottom_left};
            memcpy(&mesh->indices[next], cell, sizeof(cell));
            next += 6;
        }
    }
    return true;
}

void xd_glfx_free_mesh(xd_glfx_mesh *mesh) {
    xd_free(mesh->positions);
    xd_free(mesh->texcoords);
    xd_free(mesh->indices);
    *mesh = (xd_glfx_mesh){0};
}

void xd_glfx_set_mesh_rect(xd_glfx_mesh *mesh, struct wlr_fbox box) {
    for (size_t vertex = 0; vertex < mesh->vertex_count; vertex++) {
        double across = (double)mesh->texcoords[vertex * 2];
        double down = (double)mesh->texcoords[vertex * 2 + 1];
        mesh->positions[vertex * 2] = (float)(box.x + box.width * across);
        mesh->positions[vertex * 2 + 1] = (float)(box.y + box.height * down);
    }
}

struct wlr_fbox xd_glfx_get_mesh_bounds(const xd_glfx_mesh *mesh) {
    if (mesh->vertex_count == 0)
        return (struct wlr_fbox){0};
    double min_x = HUGE_VAL, min_y = HUGE_VAL, max_x = -HUGE_VAL, max_y = -HUGE_VAL;
    for (size_t vertex = 0; vertex < mesh->vertex_count; vertex++) {
        double vertex_x = (double)mesh->positions[vertex * 2];
        double vertex_y = (double)mesh->positions[vertex * 2 + 1];
        min_x = XD_MIN(min_x, vertex_x);
        min_y = XD_MIN(min_y, vertex_y);
        max_x = XD_MAX(max_x, vertex_x);
        max_y = XD_MAX(max_y, vertex_y);
    }
    return (struct wlr_fbox){min_x, min_y, max_x - min_x, max_y - min_y};
}

/* ---- matrices ------------------------------------------------------------------- */

void xd_glfx_set_ortho(float out[16], struct wlr_box box) {
    /* The buffer's first row is its top (as wlroots draws buffers), and a
     * framebuffer's first row is GL's y = -1: so top maps to -1. */
    float width = (float)XD_MAX(box.width, 1), height = (float)XD_MAX(box.height, 1);
    const float ortho[16] = {
        2.0f / width, 0, 0, 0,
        0, 2.0f / height, 0, 0,
        0, 0, 1, 0,
        -1.0f - 2.0f * (float)box.x / width, -1.0f - 2.0f * (float)box.y / height, 0, 1,
    };
    memcpy(out, ortho, sizeof(ortho));
}

/* ---- drawing -------------------------------------------------------------------- */

/* The built-in shader for the target, compiled the first time (a pass
 * is running: the context is current). */
static const xd_glfx_shader *glfx_get_default_shader(xd_glfx *glfx, GLenum target) {
    if (!glfx->shaders_tried) {
        glfx->shaders_tried = true;
        if (!xd_glfx_init_shader(&glfx->shader_2d, default_vertex, default_fragment,
                                 GL_TEXTURE_2D))
            wlr_log(WLR_ERROR, "effects: no shader for textures");
        /* Not every driver has external textures; none is needed then. */
        if (!xd_glfx_init_shader(&glfx->shader_external, default_vertex, default_fragment,
                                 GL_TEXTURE_EXTERNAL_OES))
            wlr_log(WLR_INFO, "effects: no shader for external textures");
    }
    const xd_glfx_shader *shader =
        target == GL_TEXTURE_EXTERNAL_OES ? &glfx->shader_external : &glfx->shader_2d;
    return shader->program != 0 ? shader : nullptr;
}

void xd_glfx_draw_mesh_with(const xd_glfx_draw *draw, const xd_glfx_mesh *mesh,
                            const xd_glfx_texture *texture, const xd_glfx_shader *shader,
                            float alpha) {
    if (shader == nullptr || shader->program == 0 || mesh->index_count == 0)
        return;
    glUseProgram(shader->program);
    if (shader->projection >= 0)
        glUniformMatrix4fv(shader->projection, 1, GL_FALSE, draw->projection);
    if (shader->alpha >= 0)
        glUniform1f(shader->alpha, alpha);
    if (shader->texcoord_scale >= 0)
        glUniform2f(shader->texcoord_scale, texture->u_scale, texture->v_scale);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(texture->target, texture->id);
    glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(texture->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(texture->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (shader->texture >= 0)
        glUniform1i(shader->texture, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (shader->position >= 0) {
        glEnableVertexAttribArray((GLuint)shader->position);
        glVertexAttribPointer((GLuint)shader->position, 2, GL_FLOAT, GL_FALSE, 0,
                              mesh->positions);
    }
    if (shader->texcoord >= 0) {
        glEnableVertexAttribArray((GLuint)shader->texcoord);
        glVertexAttribPointer((GLuint)shader->texcoord, 2, GL_FLOAT, GL_FALSE, 0,
                              mesh->texcoords);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawElements(GL_TRIANGLES, (GLsizei)mesh->index_count, GL_UNSIGNED_SHORT, mesh->indices);
    if (shader->position >= 0)
        glDisableVertexAttribArray((GLuint)shader->position);
    if (shader->texcoord >= 0)
        glDisableVertexAttribArray((GLuint)shader->texcoord);
    glBindTexture(texture->target, 0);
    glUseProgram(0);
}

void xd_glfx_draw_mesh(const xd_glfx_draw *draw, const xd_glfx_mesh *mesh,
                       const xd_glfx_texture *texture, float alpha) {
    const xd_glfx_shader *shader = glfx_get_default_shader(draw->glfx, texture->target);
    xd_glfx_draw_mesh_with(draw, mesh, texture, shader, alpha);
}

/* ---- buffers -------------------------------------------------------------------- */

/* Our buffers are drawn into and sampled, never scanned out. */
static struct wlr_swapchain *glfx_create_chain(xd_server *server, int width, int height) {
    return wlr_swapchain_create(server->allocator, width, height,
                                xd_server_get_offscreen_format(server));
}

/* A swapchain of buffers at least width x height, whole steps across and
 * down, made anew when it's too small or far too big. */
bool glfx_ensure_chain(xd_server *server, struct wlr_swapchain **chain, int width, int height,
                       int step) {
    struct wlr_swapchain *current = *chain;
    if (current != nullptr) {
        if (current->width >= width && current->height >= height &&
            current->width <= 2 * width + step && current->height <= 2 * height + step)
            return true;
        wlr_swapchain_destroy(current);
        *chain = nullptr;
    }
    width = XD_MIN((width + step - 1) / step * step, MAX_PX);
    height = XD_MIN((height + step - 1) / step * step, MAX_PX);
    *chain = glfx_create_chain(server, width, height);
    if (*chain == nullptr)
        wlr_log(WLR_ERROR, "effects: cannot allocate a %dx%d buffer", width, height);
    return *chain != nullptr;
}

/* ---- capturing a window ------------------------------------------------------------ */

typedef struct capture_entry {
    struct wlr_scene_buffer *buffer;
    /* Its pixels: the scene graph lets go of a buffer that isn't a
     * surface's once it has made a texture of it, so the frame's are the
     * ones frame.c keeps for us. */
    struct wlr_buffer *pixels;
    int x, y; /* layout */
} capture_entry;

typedef struct capture_list {
    struct wlr_scene_buffer *skip; /* the effect's own node */
    xd_view *view;
    capture_entry *entries;
    size_t count, capacity;
} capture_list;

static void glfx_collect_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    capture_list *list = data;
    struct wlr_buffer *pixels =
        buffer == list->view->frame ? list->view->kept_frame : buffer->buffer;
    if (buffer == list->skip || pixels == nullptr)
        return;
    if (list->count == list->capacity) {
        size_t capacity = XD_MAX(list->capacity * 2, (size_t)8);
        capture_entry *grown = XD_NEW_ARRAY(capture_entry, capacity);
        if (list->count > 0)
            memcpy(grown, list->entries, list->count * sizeof(*grown));
        xd_free(list->entries);
        list->entries = grown;
        list->capacity = capacity;
    }
    list->entries[list->count++] = (capture_entry){buffer, pixels, sx, sy};
}

/* A scene buffer's size in layout units, as the scene graph draws it. */
static void glfx_get_buffer_size(const capture_entry *entry, int *width, int *height) {
    const struct wlr_scene_buffer *buffer = entry->buffer;
    *width = buffer->dst_width > 0 ? buffer->dst_width : entry->pixels->width;
    *height = buffer->dst_height > 0 ? buffer->dst_height : entry->pixels->height;
    if (buffer->dst_width <= 0 && (buffer->transform & WL_OUTPUT_TRANSFORM_90)) {
        int swap = *width;
        *width = *height;
        *height = swap;
    }
}

/* The texture for a buffer that isn't a surface's (our frame): kept while
 * the frame doesn't change. */
static struct wlr_texture *glfx_get_frame_texture(xd_window_effect *effect,
                                                  struct wlr_buffer *buffer) {
    if (effect->frame_buffer == buffer && effect->frame_texture != nullptr)
        return effect->frame_texture;
    if (effect->frame_texture != nullptr)
        wlr_texture_destroy(effect->frame_texture);
    if (effect->frame_buffer != nullptr)
        wlr_buffer_unlock(effect->frame_buffer);
    /* Locked, so its address can't come back as another buffer's. */
    effect->frame_buffer = wlr_buffer_lock(buffer);
    effect->frame_texture = wlr_texture_from_buffer(effect->view->server->renderer, buffer);
    return effect->frame_texture;
}

/* Draws the window's buffers (frame, surfaces, subsurfaces, popups) into
 * effect->texture. */
bool glfx_capture_window(xd_window_effect *effect) {
    xd_view *view = effect->view;
    xd_server *server = view->server;
    glfx_color_prepare_capture(effect);
    capture_list list = {.skip = effect->node, .view = view};
    wlr_scene_node_for_each_buffer(&view->tree->node, glfx_collect_buffer, &list);
    if (list.count == 0) {
        xd_free(list.entries);
        return false;
    }
    int left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
    for (size_t i = 0; i < list.count; i++) {
        int width = 0, height = 0;
        glfx_get_buffer_size(&list.entries[i], &width, &height);
        left = XD_MIN(left, list.entries[i].x);
        top = XD_MIN(top, list.entries[i].y);
        right = XD_MAX(right, list.entries[i].x + width);
        bottom = XD_MAX(bottom, list.entries[i].y + height);
    }
    double scale = xd_view_scale(view);
    int width_px = XD_CLAMP(glfx_round_up((right - left) * scale), 1, MAX_PX);
    int height_px = XD_CLAMP(glfx_round_up((bottom - top) * scale), 1, MAX_PX);
    if (!glfx_ensure_chain(server, &effect->capture_chain, width_px, height_px, CAPTURE_STEP)) {
        xd_free(list.entries);
        return false;
    }
    struct wlr_buffer *target = wlr_swapchain_acquire(effect->capture_chain);
    if (target == nullptr) {
        xd_free(list.entries);
        return false;
    }
    /* With explicit sync, the pass signals a point of capture.c's
     * timeline once done, which releases the programs' buffers it read. */
    struct wlr_buffer_pass_options options = {0};
    if (server->syncobj != nullptr && server->capture_timeline == nullptr)
        server->capture_timeline =
            wlr_drm_syncobj_timeline_create(wlr_renderer_get_drm_fd(server->renderer));
    if (server->capture_timeline != nullptr) {
        options.signal_timeline = server->capture_timeline;
        options.signal_point = ++server->capture_point;
    }
    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(server->renderer, target, &options);
    if (pass == nullptr) {
        wlr_buffer_unlock(target);
        xd_free(list.entries);
        return false;
    }
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                       .box = {0, 0, target->width, target->height},
                                       .color = {0, 0, 0, 0},
                                       .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                                   });
    /* At most one a buffer. */
    struct wlr_linux_drm_syncobj_surface_v1_state **synced =
        XD_NEW_ARRAY(struct wlr_linux_drm_syncobj_surface_v1_state *, list.count);
    size_t synced_count = 0;
    for (size_t i = 0; i < list.count; i++) {
        struct wlr_scene_buffer *buffer = list.entries[i].buffer;
        struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
        struct wlr_texture *texture = nullptr;
        struct wlr_linux_drm_syncobj_surface_v1_state *sync = nullptr;
        if (scene_surface != nullptr) {
            texture = wlr_surface_get_texture(scene_surface->surface);
            /* Unmapped by a commit without a buffer, the surface has let
             * go of its texture; the scene graph still shows the last. */
            struct wlr_client_buffer *client_buffer =
                texture == nullptr ? wlr_client_buffer_get(buffer->buffer) : nullptr;
            if (client_buffer != nullptr)
                texture = client_buffer->texture;
            /* The program's GPU may still be drawing it: wait for it, as
             * the scene graph does. */
            sync = wlr_linux_drm_syncobj_v1_get_surface_state(scene_surface->surface);
            if (sync != nullptr)
                synced[synced_count++] = sync;
        } else {
            texture = glfx_get_frame_texture(effect, list.entries[i].pixels);
        }
        if (texture == nullptr)
            continue;
        int width = 0, height = 0;
        glfx_get_buffer_size(&list.entries[i], &width, &height);
        int offset_x = list.entries[i].x - left, offset_y = list.entries[i].y - top;
        struct wlr_box box = {
            .x = (int)(offset_x * scale),
            .y = (int)(offset_y * scale),
            .width = (int)((offset_x + width) * scale) - (int)(offset_x * scale),
            .height = (int)((offset_y + height) * scale) - (int)(offset_y * scale),
        };
        if (box.width <= 0 || box.height <= 0)
            continue;
        /* Never the buffer's opacity: we keep the window's at 0. */
        wlr_render_pass_add_texture(
            pass, &(struct wlr_render_texture_options){
                      .texture = texture,
                      .src_box = buffer->src_box,
                      .dst_box = box,
                      .transform = buffer->transform,
                      .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                      .wait_timeline = sync != nullptr ? sync->acquire_timeline : nullptr,
                      .wait_point = sync != nullptr ? sync->acquire_point : 0,
                  });
    }
    xd_free(list.entries);
    bool ok = wlr_render_pass_submit(pass);
    for (size_t i = 0; ok && options.signal_timeline != nullptr && i < synced_count; i++)
        wlr_linux_drm_syncobj_v1_state_add_release_point(synced[i], options.signal_timeline,
                                                         options.signal_point, server->loop);
    xd_free(synced);
    struct wlr_texture *texture = ok ? wlr_texture_from_buffer(server->renderer, target) : nullptr;
    if (texture == nullptr) {
        wlr_buffer_unlock(target);
        return false;
    }
    struct fx_texture_attribs attribs = {0};
    fx_texture_get_attribs(texture, &attribs);
    effect->capture_texture = texture;
    effect->capture_buffer = target; /* locked till the effect has drawn it */
    effect->texture = (xd_glfx_texture){
        .id = attribs.tex,
        .target = attribs.target,
        .u_scale = (float)width_px / (float)target->width,
        .v_scale = (float)height_px / (float)target->height,
        .box = {left, top, right - left, bottom - top},
    };
    glfx_color_adjust_capture(effect);
    return true;
}

void glfx_release_capture(xd_window_effect *effect) {
    if (effect->capture_texture != nullptr)
        wlr_texture_destroy(effect->capture_texture);
    if (effect->capture_buffer != nullptr)
        wlr_buffer_unlock(effect->capture_buffer);
    effect->capture_texture = nullptr;
    effect->capture_buffer = nullptr;
    effect->texture.id = 0;
}

/* ---- hiding the window itself ------------------------------------------------------ */

typedef struct opacity_change {
    struct wlr_scene_buffer *skip;
    float opacity;
} opacity_change;

static void glfx_set_opacity(struct wlr_scene_buffer *buffer, [[maybe_unused]] int sx,
                        [[maybe_unused]] int sy, void *data) {
    const opacity_change *change = data;
    if (buffer != change->skip)
        wlr_scene_buffer_set_opacity(buffer, change->opacity);
}

/* Each frame: subsurfaces and popups that appeared since are hidden too. */
static void glfx_hide_window(xd_window_effect *effect) {
    xd_view *view = effect->view;
    opacity_change change = {effect->node, 0.0f};
    wlr_scene_node_for_each_buffer(&view->tree->node, glfx_set_opacity, &change);
    if (view->blur != nullptr)
        wlr_scene_node_set_enabled(&view->blur->node, false);
    if (view->shadow != nullptr)
        wlr_scene_node_set_enabled(&view->shadow->node, false);
}

/* Shown as Aero Peek has it (view.c's apply_peek): while it peeks at
 * another window or shows the desktop, the content stays invisible and the
 * frame a faint outline. Window Opacity sets its own from the next frame. */
static void glfx_show_window(xd_window_effect *effect) {
    xd_view *view = effect->view;
    const xd_server *server = view->server;
    const xd_view *peeked = server->peek_view;
    bool shown = !server->peeking ||
                 (peeked != nullptr && (view == peeked || xd_view_get_owner(view) == peeked));
    opacity_change change = {effect->node, shown ? 1.0f : 0.0f};
    wlr_scene_node_for_each_buffer(&view->tree->node, glfx_set_opacity, &change);
    wlr_scene_buffer_set_opacity(view->frame, shown ? 1.0f : 0.25f);
    /* Its glass and shadow, as frame.c has them. */
    xd_frame_render(view, true);
}

/* ---- window effects ------------------------------------------------------------- */

static bool glfx_reject_input([[maybe_unused]] struct wlr_scene_buffer *buffer,
                                [[maybe_unused]] double *sx, [[maybe_unused]] double *sy) {
    return false;
}

static void glfx_schedule_frames(xd_server *server) {
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (out->wlr_output->enabled)
            wlr_output_schedule_frame(out->wlr_output);
    }
}

xd_window_effect *xd_glfx_find_window(xd_view *view, const xd_window_effect_impl *impl) {
    xd_glfx *glfx = view->server->glfx;
    if (glfx == nullptr)
        return nullptr;
    xd_window_effect *effect;
    wl_list_for_each(effect, &glfx->windows, link) {
        if (effect->view == view && (impl == nullptr || effect->impl == impl))
            return effect;
    }
    return nullptr;
}

xd_window_effect *xd_glfx_begin_window(xd_view *view, const xd_window_effect_impl *impl,
                                       void *data) {
    xd_glfx *glfx = view->server->glfx;
    if (glfx == nullptr || !view->mapped)
        return nullptr;
    xd_window_effect *other = xd_glfx_find_window(view, nullptr);
    if (other != nullptr)
        xd_glfx_end_window(other);
    xd_window_effect *effect = XD_NEW(xd_window_effect);
    effect->impl = impl;
    effect->view = view;
    effect->data = data;
    xd_frame_keep_pixels(view, true);
    /* Empty until its first frame: the window shows as itself till then. */
    effect->node = wlr_scene_buffer_create(view->tree, nullptr);
    effect->node->point_accepts_input = glfx_reject_input;
    wl_list_insert(&glfx->windows, &effect->link);
    glfx_schedule_frames(view->server);
    return effect;
}

void xd_glfx_end_window(xd_window_effect *effect) {
    wl_list_remove(&effect->link);
    if (effect->impl->done != nullptr)
        effect->impl->done(effect);
    struct wlr_scene_buffer *node = effect->node;
    effect->node = nullptr;
    wlr_scene_node_destroy(&node->node);
    if (!effect->held && effect->view->mapped)
        glfx_show_window(effect);
    glfx_release_capture(effect);
    if (effect->frame_texture != nullptr)
        wlr_texture_destroy(effect->frame_texture);
    if (effect->frame_buffer != nullptr)
        wlr_buffer_unlock(effect->frame_buffer);
    if (!effect->held)
        xd_frame_keep_pixels(effect->view, false);
    wlr_swapchain_destroy(effect->capture_chain);
    wlr_swapchain_destroy(effect->target_chain);
    xd_free(effect);
}

/* A live (not closing) effect on the window, or nullptr. */
static xd_window_effect *glfx_find_live_window(xd_view *view) {
    xd_glfx *glfx = view->server->glfx;
    xd_window_effect *effect;
    wl_list_for_each(effect, &glfx->windows, link) {
        if (effect->view == view && !effect->held)
            return effect;
    }
    return nullptr;
}

void xd_glfx_forget_view(xd_view *view) {
    if (view->server->glfx == nullptr)
        return;
    xd_window_effect *effect = nullptr;
    while ((effect = glfx_find_live_window(view)) != nullptr)
        xd_glfx_end_window(effect);
    glfx_color_forget_view(view);
}

void xd_glfx_drop_view(xd_view *view) {
    xd_glfx *glfx = view->server->glfx;
    if (glfx == nullptr)
        return;
    xd_window_effect *effect;
    wl_list_for_each(effect, &glfx->windows, link) {
        if (effect->view == view)
            effect->view = nullptr;
    }
}

/* A program closing its window unmaps its surface, and the scene graph,
 * told first, hides the surface's tree: shown again just for the
 * picture. */
typedef struct hidden_trees {
    struct wlr_scene_node **nodes;
    size_t count, capacity;
} hidden_trees;

static void glfx_show_hidden_trees(struct wlr_scene_tree *tree, hidden_trees *hidden) {
    if (!tree->node.enabled) {
        if (hidden->count == hidden->capacity) {
            hidden->capacity = XD_MAX(hidden->capacity * 2, (size_t)8);
            hidden->nodes =
                xd_realloc_array(hidden->nodes, hidden->capacity, sizeof(*hidden->nodes));
        }
        wlr_scene_node_set_enabled(&tree->node, true);
        hidden->nodes[hidden->count++] = &tree->node;
    }
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link) {
        if (child->type == WLR_SCENE_NODE_TREE)
            glfx_show_hidden_trees(wlr_scene_tree_from_node(child), hidden);
    }
}

xd_window_effect *xd_glfx_begin_closing_window(xd_view *view, const xd_window_effect_impl *impl,
                                               void *data) {
    xd_glfx *glfx = view->server->glfx;
    if (glfx == nullptr || !view->mapped || view->minimized || !view->tree->node.enabled ||
        view->tree->node.parent == nullptr)
        return nullptr;
    /* One closing effect a window (Burn and Explode both on): the first
     * to ask. */
    xd_window_effect *other;
    wl_list_for_each(other, &glfx->windows, link) {
        if (other->view == view && other->held)
            return nullptr;
    }
    /* Their nodes are in the window's tree, and would be in its picture. */
    xd_window_effect *live = nullptr;
    while ((live = glfx_find_live_window(view)) != nullptr)
        xd_glfx_end_window(live);
    xd_window_effect *effect = XD_NEW(xd_window_effect);
    effect->impl = impl;
    effect->view = view;
    effect->data = data;
    effect->held = true;
    effect->held_scale = xd_view_scale(view);
    hidden_trees hidden = {0};
    if (view->content != nullptr)
        glfx_show_hidden_trees(view->content, &hidden);
    xd_frame_keep_pixels(view, true);
    bool captured = glfx_capture_window(effect);
    xd_frame_keep_pixels(view, false);
    for (size_t i = 0; i < hidden.count; i++)
        wlr_scene_node_set_enabled(hidden.nodes[i], false);
    xd_free(hidden.nodes);
    /* The picture is drawn: what it was drawn from can go. */
    if (effect->frame_texture != nullptr)
        wlr_texture_destroy(effect->frame_texture);
    if (effect->frame_buffer != nullptr)
        wlr_buffer_unlock(effect->frame_buffer);
    effect->frame_texture = nullptr;
    effect->frame_buffer = nullptr;
    if (!captured) {
        wlr_swapchain_destroy(effect->capture_chain);
        xd_free(effect);
        return nullptr;
    }
    /* Beside the window's tree, so it stays when that is hidden or freed. */
    effect->node = wlr_scene_buffer_create(view->tree->node.parent, nullptr);
    wlr_scene_node_place_above(&effect->node->node, &view->tree->node);
    effect->node->point_accepts_input = glfx_reject_input;
    wl_list_insert(&glfx->windows, &effect->link);
    glfx_schedule_frames(view->server);
    return effect;
}

/* One frame of one effect (it may end). */
static void glfx_draw_window_effect(xd_glfx *glfx, xd_window_effect *effect, double now) {
    xd_server *server = glfx->server;
    xd_view *view = effect->view;
    if (!effect->held && (!view->mapped || !glfx_capture_window(effect))) {
        xd_glfx_end_window(effect);
        return;
    }
    double seconds = 0;
    if (!effect->begun) {
        effect->begun = true;
        if (effect->impl->begin != nullptr)
            effect->impl->begin(effect);
    } else {
        seconds = XD_MAX(now - effect->last_time, 0.0);
    }
    effect->last_time = now;
    if (!effect->impl->update(effect, seconds)) {
        xd_glfx_end_window(effect);
        return;
    }
    struct wlr_fbox fbounds = effect->impl->get_bounds(effect);
    double scale = effect->held ? effect->held_scale : xd_view_scale(view);
    struct wlr_box bounds = {
        .x = glfx_round_down(fbounds.x),
        .y = glfx_round_down(fbounds.y),
    };
    bounds.width = XD_MAX(glfx_round_up(fbounds.x + fbounds.width) - bounds.x, 1);
    bounds.height = XD_MAX(glfx_round_up(fbounds.y + fbounds.height) - bounds.y, 1);
    int width_px = XD_CLAMP(glfx_round_up(bounds.width * scale), 1, MAX_PX);
    int height_px = XD_CLAMP(glfx_round_up(bounds.height * scale), 1, MAX_PX);
    if (!glfx_ensure_chain(server, &effect->target_chain, width_px, height_px, TARGET_STEP)) {
        xd_glfx_end_window(effect);
        return;
    }
    struct wlr_buffer *target = wlr_swapchain_acquire(effect->target_chain);
    struct wlr_render_pass *pass =
        target != nullptr ? wlr_renderer_begin_buffer_pass(server->renderer, target,
                                                           &(struct wlr_buffer_pass_options){0})
                          : nullptr;
    if (pass == nullptr) {
        if (target != nullptr)
            wlr_buffer_unlock(target);
        if (!effect->held)
            glfx_release_capture(effect);
        return; /* another go next frame */
    }
    double started = glfx->timing ? glfx_get_now() : 0;
    /* The pass made SceneFX's context current with the target bound. */
    glViewport(0, 0, width_px, height_px);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    xd_glfx_draw draw = {.bounds = bounds, .scale = scale, .glfx = glfx};
    xd_glfx_set_ortho(draw.projection, bounds);
    effect->impl->draw(effect, &draw);
    if (glfx->timing)
        glFinish();
    wlr_render_pass_submit(pass);
    if (glfx->timing)
        glfx->draw_total += glfx_get_now() - started;

    struct wlr_scene_buffer *node = effect->node;
    wlr_scene_buffer_set_buffer(node, target);
    wlr_buffer_unlock(target); /* the scene holds its own lock */
    wlr_scene_buffer_set_source_box(node, &(struct wlr_fbox){0, 0, width_px, height_px});
    wlr_scene_buffer_set_dest_size(node, bounds.width, bounds.height);
    if (effect->held) {
        int parent_x = 0, parent_y = 0;
        wlr_scene_node_coords(&node->node.parent->node, &parent_x, &parent_y);
        wlr_scene_node_set_position(&node->node, bounds.x - parent_x, bounds.y - parent_y);
        return;
    }
    glfx_release_capture(effect);
    wlr_scene_node_set_position(&node->node, bounds.x - view->tree->node.x,
                                bounds.y - view->tree->node.y);
    wlr_scene_node_raise_to_top(&node->node);
    glfx_hide_window(effect);
}

/* Our node can't be a plane of its own, nor the window it hides: no
 * direct scanout on the output while window effects run. */
static void glfx_lock_scanout(xd_output *output, bool lock) {
    if (output->effects_scanout_locked == lock)
        return;
    output->effects_scanout_locked = lock;
    wlr_output_lock_attach_render(output->wlr_output, lock);
}

void xd_glfx_frame(xd_server *server, xd_output *output, double now) {
    xd_glfx *glfx = server->glfx;
    if (glfx == nullptr)
        return;
    if (glfx->timing)
        glfx->frame_start = glfx_get_now();
    if (!wl_list_empty(&glfx->filters))
        glfx_update_filters(glfx, now);
    glfx_lock_scanout(output, !wl_list_empty(&glfx->windows) || glfx_has_active_filters(glfx));
    if (wl_list_empty(&glfx->windows)) {
        glfx_color_frame(server);
        return;
    }
    /* Another output's frame of the same moment: drawn already. The
     * windows are hidden again, though: Window Opacity's frame hook has
     * just set their opacity anew. */
    if (now - glfx->last_frame < SAME_FRAME_SECONDS) {
        xd_window_effect *effect;
        wl_list_for_each(effect, &glfx->windows, link) {
            if (!effect->held && effect->begun && effect->view->mapped)
                glfx_hide_window(effect);
        }
        glfx_color_frame(server);
        return;
    }
    glfx->last_frame = now;
    xd_window_effect *effect, *next;
    wl_list_for_each_safe(effect, next, &glfx->windows, link) {
        glfx_draw_window_effect(glfx, effect, now);
    }
    /* After them: a window whose effect has just ended is drawn with its
     * colours this frame. */
    glfx_color_frame(server);
}

void xd_glfx_frame_done(xd_server *server, [[maybe_unused]] xd_output *output) {
    xd_glfx *glfx = server->glfx;
    if (glfx == nullptr)
        return;
    if (wl_list_empty(&glfx->windows) && !glfx_has_active_filters(glfx)) {
        glfx->last_done = 0;
        return;
    }
    /* Frames keep coming while an effect runs, at the outputs' pace. */
    glfx_schedule_frames(server);
    if (!glfx->timing)
        return;
    double now = glfx_get_now();
    if (glfx->last_done > 0) {
        double interval = now - glfx->last_done;
        glfx->interval_total += interval;
        glfx->interval_max = XD_MAX(glfx->interval_max, interval);
        glfx->frame_total += now - glfx->frame_start;
        glfx->frames++;
    } else {
        glfx->timing_start = now;
    }
    glfx->last_done = now;
    if (glfx->frames > 0 && now - glfx->timing_start >= TIMING_SECONDS) {
        wlr_log(WLR_INFO,
                "effects: %d frames, %.1f ms apart (at most %.1f), %.2f ms to draw a frame, "
                "%.2f ms of it the effects'",
                glfx->frames, glfx->interval_total / glfx->frames * 1000.0,
                glfx->interval_max * 1000.0, glfx->frame_total / glfx->frames * 1000.0,
                glfx->draw_total / glfx->frames * 1000.0);
        glfx->frames = 0;
        glfx->interval_total = glfx->interval_max = glfx->frame_total = glfx->draw_total = 0;
        glfx->timing_start = now;
    }
}

/* ---- output filters --------------------------------------------------------------- */

/* An output's buffers for its filters: the scene graph's picture, and
 * one between two filters. */
typedef struct filtered_output {
    struct wl_list link; /* xd_glfx.filtered_outputs */
    struct wlr_output *wlr_output;
    struct wlr_swapchain *scene_chain, *between_chain;
    struct wl_listener destroy;
} filtered_output;

static void glfx_free_filtered_output(filtered_output *filtered) {
    wl_list_remove(&filtered->link);
    wl_list_remove(&filtered->destroy.link);
    wlr_swapchain_destroy(filtered->scene_chain);
    wlr_swapchain_destroy(filtered->between_chain);
    xd_free(filtered);
}

static void glfx_free_filtered_outputs(xd_glfx *glfx) {
    filtered_output *filtered, *next;
    wl_list_for_each_safe(filtered, next, &glfx->filtered_outputs, link) {
        glfx_free_filtered_output(filtered);
    }
}

static void glfx_handle_filtered_output_destroy(struct wl_listener *listener,
                                                [[maybe_unused]] void *data) {
    glfx_free_filtered_output(XD_CONTAINER_OF(listener, filtered_output, destroy));
}

static filtered_output *glfx_get_filtered_output(xd_glfx *glfx, struct wlr_output *wlr_output) {
    filtered_output *filtered;
    wl_list_for_each(filtered, &glfx->filtered_outputs, link) {
        if (filtered->wlr_output == wlr_output)
            return filtered;
    }
    filtered = XD_NEW(filtered_output);
    filtered->wlr_output = wlr_output;
    filtered->destroy.notify = glfx_handle_filtered_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &filtered->destroy);
    wl_list_insert(&glfx->filtered_outputs, &filtered->link);
    return filtered;
}

/* A swapchain of exactly width x height (the scene graph wants its
 * output's size). */
static bool glfx_ensure_exact_chain(xd_server *server, struct wlr_swapchain **chain, int width,
                                    int height) {
    if (*chain != nullptr && (*chain)->width == width && (*chain)->height == height)
        return true;
    wlr_swapchain_destroy(*chain);
    *chain = glfx_create_chain(server, width, height);
    return *chain != nullptr;
}

static bool glfx_has_active_filters(const xd_glfx *glfx) {
    const xd_output_filter *filter;
    wl_list_for_each(filter, &glfx->filters, link) {
        if (filter->active)
            return true;
    }
    return false;
}

/* The scene graph's picture of what a filter drew over is stale: it
 * repaints the whole output (moving it there and back is the public way
 * to say so). */
static void glfx_repaint_outputs(xd_server *server) {
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        struct wlr_scene_output *scene_output =
            wlr_scene_get_scene_output(server->scene, out->wlr_output);
        if (scene_output == nullptr)
            continue;
        int x = scene_output->x, y = scene_output->y;
        wlr_scene_output_set_position(scene_output, x + 1, y);
        wlr_scene_output_set_position(scene_output, x, y);
    }
}

xd_output_filter *xd_glfx_add_output_filter(xd_server *server, const xd_output_filter_impl *impl,
                                            void *data) {
    xd_glfx *glfx = server->glfx;
    if (glfx == nullptr)
        return nullptr;
    xd_output_filter *filter = XD_NEW(xd_output_filter);
    filter->impl = impl;
    filter->server = server;
    filter->data = data;
    filter->active = true;
    wl_list_insert(glfx->filters.prev, &filter->link);
    glfx_schedule_frames(server);
    return filter;
}

void xd_glfx_remove_output_filter(xd_output_filter *filter) {
    xd_glfx *glfx = filter->server->glfx;
    bool was_active = filter->active;
    wl_list_remove(&filter->link);
    if (filter->impl->done != nullptr)
        filter->impl->done(filter);
    if (was_active)
        glfx_repaint_outputs(filter->server);
    xd_free(filter);
    /* The last one gone: the outputs' full-size buffers for them too. */
    if (wl_list_empty(&glfx->filters))
        glfx_free_filtered_outputs(glfx);
}

void xd_glfx_wake_output_filter(xd_output_filter *filter) {
    if (!filter->active) {
        filter->active = true;
        filter->last_time = 0;
    }
    glfx_schedule_frames(filter->server);
}

/* Once per frame moment: each filter says whether it's active. */
static void glfx_update_filters(xd_glfx *glfx, double now) {
    if (now - glfx->last_filter_frame < SAME_FRAME_SECONDS)
        return;
    glfx->last_filter_frame = now;
    bool went_idle = false;
    xd_output_filter *filter;
    wl_list_for_each(filter, &glfx->filters, link) {
        if (!filter->active)
            continue;
        double seconds = filter->last_time > 0 ? XD_MAX(now - filter->last_time, 0.0) : 0;
        filter->last_time = now;
        filter->active = filter->impl->update(filter, seconds);
        went_idle = went_idle || !filter->active;
    }
    if (went_idle)
        glfx_repaint_outputs(glfx->server);
}

bool xd_glfx_is_output_filtered(const xd_output *output) {
    const xd_glfx *glfx = output->server->glfx;
    return glfx != nullptr && glfx_has_active_filters(glfx) &&
           output->wlr_output->transform == WL_OUTPUT_TRANSFORM_NORMAL;
}

/* Runs one filter from `source` into `target`. */
static bool glfx_run_filter(xd_glfx *glfx, xd_output_filter *filter, xd_output *output,
                            struct wlr_buffer *source, struct wlr_buffer *target,
                            struct wlr_box box) {
    xd_server *server = glfx->server;
    struct wlr_texture *texture = wlr_texture_from_buffer(server->renderer, source);
    if (texture == nullptr)
        return false;
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
        server->renderer, target, &(struct wlr_buffer_pass_options){0});
    if (pass == nullptr) {
        wlr_texture_destroy(texture);
        return false;
    }
    double started = glfx->timing ? glfx_get_now() : 0;
    glViewport(0, 0, target->width, target->height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    xd_glfx_draw draw = {.bounds = box, .scale = (double)output->wlr_output->scale, .glfx = glfx};
    xd_glfx_set_ortho(draw.projection, box);
    struct fx_texture_attribs attribs = {0};
    fx_texture_get_attribs(texture, &attribs);
    xd_glfx_texture scene = {
        .id = attribs.tex,
        .target = attribs.target,
        .u_scale = 1.0f,
        .v_scale = 1.0f,
        .box = {box.x, box.y, box.width, box.height},
    };
    filter->impl->draw(filter, output, &scene, &draw);
    if (glfx->timing)
        glFinish();
    bool ok = wlr_render_pass_submit(pass);
    if (glfx->timing)
        glfx->draw_total += glfx_get_now() - started;
    wlr_texture_destroy(texture);
    return ok;
}

bool xd_glfx_build_output_state(xd_output *output, struct wlr_scene_output *scene_output,
                                struct wlr_output_state *state) {
    xd_glfx *glfx = output->server->glfx;
    if (!xd_glfx_is_output_filtered(output))
        return wlr_scene_output_build_state(scene_output, state, nullptr);
    struct wlr_output *wlr_output = output->wlr_output;
    xd_server *server = output->server;
    filtered_output *filtered = glfx_get_filtered_output(glfx, wlr_output);
    if (!glfx_ensure_exact_chain(server, &filtered->scene_chain, wlr_output->width,
                                 wlr_output->height))
        return wlr_scene_output_build_state(scene_output, state, nullptr);
    /* The scene graph draws into our buffer, with its own damage tracking
     * for it; what it would commit besides is dropped. */
    struct wlr_output_state scene_state;
    wlr_output_state_init(&scene_state);
    bool built = wlr_scene_output_build_state(
        scene_output, &scene_state,
        &(struct wlr_scene_output_state_options){.swapchain = filtered->scene_chain});
    if (!built || !(scene_state.committed & WLR_OUTPUT_STATE_BUFFER) ||
        scene_state.buffer == nullptr ||
        !wlr_output_configure_primary_swapchain(wlr_output, state, &wlr_output->swapchain)) {
        wlr_output_state_finish(&scene_state);
        return false;
    }
    struct wlr_box box = {0};
    wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
    struct wlr_buffer *source = wlr_buffer_lock(scene_state.buffer);
    wlr_output_state_finish(&scene_state);
    xd_output_filter *filter, *last = nullptr;
    wl_list_for_each(filter, &glfx->filters, link) {
        if (filter->active)
            last = filter;
    }
    bool ok = true;
    wl_list_for_each(filter, &glfx->filters, link) {
        if (!ok || !filter->active)
            continue;
        struct wlr_swapchain *chain = wlr_output->swapchain;
        if (filter != last) {
            ok = glfx_ensure_exact_chain(server, &filtered->between_chain, wlr_output->width,
                                         wlr_output->height);
            chain = filtered->between_chain;
        }
        struct wlr_buffer *target = ok ? wlr_swapchain_acquire(chain) : nullptr;
        ok = target != nullptr && glfx_run_filter(glfx, filter, output, source, target, box);
        wlr_buffer_unlock(source);
        source = target;
    }
    /* No damage given: the whole output. */
    if (ok && source != nullptr)
        wlr_output_state_set_buffer(state, source);
    if (source != nullptr)
        wlr_buffer_unlock(source);
    return ok;
}

/* A pass on a scratch buffer, outside the outputs' frames: GL calls need
 * the renderer's context, which a pass makes current. */
typedef struct scratch_pass {
    struct wlr_swapchain *chain;
    struct wlr_buffer *buffer;
    struct wlr_render_pass *pass;
} scratch_pass;

static bool glfx_begin_scratch_pass(xd_server *server, scratch_pass *scratch) {
    *scratch = (scratch_pass){0};
    scratch->chain = glfx_create_chain(server, CAPTURE_STEP, CAPTURE_STEP);
    scratch->buffer = scratch->chain != nullptr ? wlr_swapchain_acquire(scratch->chain) : nullptr;
    scratch->pass = scratch->buffer != nullptr
                        ? wlr_renderer_begin_buffer_pass(server->renderer, scratch->buffer,
                                                         &(struct wlr_buffer_pass_options){0})
                        : nullptr;
    return scratch->pass != nullptr;
}

static void glfx_end_scratch_pass(scratch_pass *scratch) {
    if (scratch->pass != nullptr)
        wlr_render_pass_submit(scratch->pass);
    if (scratch->buffer != nullptr)
        wlr_buffer_unlock(scratch->buffer);
    if (scratch->chain != nullptr)
        wlr_swapchain_destroy(scratch->chain);
    *scratch = (scratch_pass){0};
}

bool xd_glfx_init(xd_server *server, xd_effects_support *support) {
    xd_glfx *glfx = XD_NEW(xd_glfx);
    glfx->server = server;
    wl_list_init(&glfx->windows);
    wl_list_init(&glfx->filters);
    wl_list_init(&glfx->filtered_outputs);
    glfx->timing = getenv("XD_EFFECTS_TIMING") != nullptr;
    server->glfx = glfx;
    owner = glfx;
    /* The renderer's name, for the log and Desktop Effects. */
    scratch_pass scratch;
    bool ok = glfx_begin_scratch_pass(server, &scratch);
    if (ok) {
        const GLubyte *renderer = glGetString(GL_RENDERER);
        snprintf(support->renderer, sizeof(support->renderer), "%s",
                 renderer != nullptr ? (const char *)renderer : "OpenGL ES");
    } else {
        snprintf(support->reason, sizeof(support->reason), "%s",
                 "The compositor cannot draw with OpenGL ES on this computer.");
    }
    glfx_end_scratch_pass(&scratch);
    return ok;
}

void xd_glfx_finish(xd_server *server) {
    xd_glfx *glfx = server->glfx;
    if (glfx == nullptr)
        return;
    xd_window_effect *effect, *next;
    wl_list_for_each_safe(effect, next, &glfx->windows, link) {
        xd_glfx_end_window(effect);
    }
    xd_output_filter *filter, *next_filter;
    wl_list_for_each_safe(filter, next_filter, &glfx->filters, link) {
        xd_glfx_remove_output_filter(filter);
    }
    glfx_free_filtered_outputs(glfx);
    glfx_color_finish(server);
    /* Every shader made, ours and the effects', while the renderer and its
     * context are still there. They aren't made again: every effect is off
     * by now, and glfx goes with the compositor. */
    scratch_pass scratch = {0};
    if (glfx->shader_count > 0 && glfx_begin_scratch_pass(server, &scratch)) {
        while (glfx->shader_count > 0)
            xd_glfx_destroy_shader(glfx->shaders[glfx->shader_count - 1]);
    }
    glfx_end_scratch_pass(&scratch);
    xd_free(glfx->shaders);
    owner = nullptr;
    server->glfx = nullptr;
    xd_free(glfx);
}
