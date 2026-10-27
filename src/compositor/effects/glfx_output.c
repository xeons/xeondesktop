/* SPDX-License-Identifier: MIT
 * The OpenGL path's whole-output side (glfx.h, xd-compositor-fx only):
 * the scene drawn into textures (a desktop that isn't showing, a window
 * on its own), effects that draw whole outputs in place of the scene, and
 * 3D drawing for them.
 */
#include "glfx.h"

#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/util/log.h>

#include "../../core/alloc.h"

/* As glfx.c's: device px no buffer of ours exceeds, and buffers in whole
 * steps of px (llvmpipe and udmabuf). */
static constexpr int MAX_PX = 8192;
static constexpr int BUFFER_STEP = 64;
enum { MAX_OUTPUTS = 16, SYNCED_MAX = 32 };

static int output_round_up(double value) {
    double rounded = ceil(value);
    return rounded < 1 ? 1 : rounded > MAX_PX ? MAX_PX : (int)rounded;
}

/* Drawn into and sampled, never scanned out, as glfx.c's. */
static struct wlr_swapchain *output_create_chain(xd_server *server, int width, int height) {
    uint64_t modifiers[] = {DRM_FORMAT_MOD_INVALID};
    struct wlr_drm_format format = {
        .format = DRM_FORMAT_ARGB8888, .len = 1, .capacity = 1, .modifiers = modifiers};
    width = XD_MIN((width + BUFFER_STEP - 1) / BUFFER_STEP * BUFFER_STEP, MAX_PX);
    height = XD_MIN((height + BUFFER_STEP - 1) / BUFFER_STEP * BUFFER_STEP, MAX_PX);
    struct wlr_swapchain *chain = wlr_swapchain_create(server->allocator, width, height, &format);
    if (chain == nullptr)
        wlr_log(WLR_ERROR, "effects: cannot allocate a %dx%d buffer", width, height);
    return chain;
}

/* A swapchain for width x height px, made anew when it doesn't fit. */
static bool output_ensure_chain(xd_server *server, struct wlr_swapchain **chain, int width,
                                int height) {
    if (*chain != nullptr && (*chain)->width >= width && (*chain)->height >= height &&
        (*chain)->width < width + 2 * BUFFER_STEP && (*chain)->height < height + 2 * BUFFER_STEP)
        return true;
    if (*chain != nullptr)
        wlr_swapchain_destroy(*chain);
    *chain = output_create_chain(server, width, height);
    return *chain != nullptr;
}

/* ---- the scene as a texture ------------------------------------------------------ */

/* A texture made from a buffer that isn't a surface's (a window's frame,
 * an effect's picture), kept while that buffer is the same. */
typedef struct cached_texture {
    struct wlr_buffer *buffer; /* locked */
    struct wlr_texture *texture;
    bool used; /* in the last capture */
} cached_texture;

struct xd_glfx_scene_capture {
    struct wl_list link; /* captures */
    xd_server *server;
    struct wlr_swapchain *chain;
    struct wlr_buffer *buffer; /* the last capture's, locked */
    struct wlr_texture *texture;
    cached_texture *cache;
    size_t cache_count, cache_capacity;
};

/* Every capture object, and the windows whose frames keep their pixels
 * for them. */
static struct wl_list captures = {&captures, &captures};
static xd_view **kept_views;
static size_t kept_count, kept_capacity;

static void capture_keep_frame(xd_view *view) {
    for (size_t i = 0; i < kept_count; i++) {
        if (kept_views[i] == view)
            return;
    }
    if (kept_count == kept_capacity) {
        kept_capacity = XD_MAX(kept_capacity * 2, (size_t)16);
        kept_views = xd_realloc_array(kept_views, kept_capacity, sizeof(*kept_views));
    }
    kept_views[kept_count++] = view;
    xd_frame_keep_pixels(view, true);
}

void xd_glfx_capture_forget_view(xd_view *view) {
    for (size_t i = 0; i < kept_count; i++) {
        if (kept_views[i] == view) {
            xd_frame_keep_pixels(view, false);
            kept_views[i] = kept_views[--kept_count];
            return;
        }
    }
}

static void capture_release_frames(void) {
    for (size_t i = 0; i < kept_count; i++)
        xd_frame_keep_pixels(kept_views[i], false);
    XD_FREE(&kept_views);
    kept_count = kept_capacity = 0;
}

xd_glfx_scene_capture *xd_glfx_create_scene_capture(xd_server *server) {
    xd_glfx_scene_capture *capture = XD_NEW(xd_glfx_scene_capture);
    capture->server = server;
    wl_list_insert(&captures, &capture->link);
    return capture;
}

static void capture_release_texture(xd_glfx_scene_capture *capture) {
    if (capture->texture != nullptr)
        wlr_texture_destroy(capture->texture);
    if (capture->buffer != nullptr)
        wlr_buffer_unlock(capture->buffer);
    capture->texture = nullptr;
    capture->buffer = nullptr;
}

void xd_glfx_destroy_scene_capture(xd_glfx_scene_capture *capture) {
    if (capture == nullptr)
        return;
    capture_release_texture(capture);
    for (size_t i = 0; i < capture->cache_count; i++) {
        wlr_texture_destroy(capture->cache[i].texture);
        wlr_buffer_unlock(capture->cache[i].buffer);
    }
    xd_free(capture->cache);
    if (capture->chain != nullptr)
        wlr_swapchain_destroy(capture->chain);
    wl_list_remove(&capture->link);
    xd_free(capture);
    if (wl_list_empty(&captures))
        capture_release_frames();
}

static struct wlr_texture *capture_get_cached(xd_glfx_scene_capture *capture,
                                              struct wlr_buffer *buffer) {
    for (size_t i = 0; i < capture->cache_count; i++) {
        if (capture->cache[i].buffer == buffer) {
            capture->cache[i].used = true;
            return capture->cache[i].texture;
        }
    }
    struct wlr_texture *texture = wlr_texture_from_buffer(capture->server->renderer, buffer);
    if (texture == nullptr)
        return nullptr;
    if (capture->cache_count == capture->cache_capacity) {
        capture->cache_capacity = XD_MAX(capture->cache_capacity * 2, (size_t)8);
        capture->cache =
            xd_realloc_array(capture->cache, capture->cache_capacity, sizeof(*capture->cache));
    }
    /* Locked, so its address can't come back as another buffer's. */
    capture->cache[capture->cache_count++] =
        (cached_texture){wlr_buffer_lock(buffer), texture, true};
    return texture;
}

/* Textures of buffers no longer drawn go. */
static void capture_trim_cache(xd_glfx_scene_capture *capture) {
    size_t kept = 0;
    for (size_t i = 0; i < capture->cache_count; i++) {
        cached_texture entry = capture->cache[i];
        if (!entry.used) {
            wlr_texture_destroy(entry.texture);
            wlr_buffer_unlock(entry.buffer);
            continue;
        }
        entry.used = false;
        capture->cache[kept++] = entry;
    }
    capture->cache_count = kept;
}

typedef struct capture_walk {
    xd_glfx_scene_capture *capture;
    struct wlr_render_pass *pass;
    struct wlr_box box;
    double scale;
    bool layers;
    xd_glfx_view_filter include_view;
    void *data;
    struct wlr_linux_drm_syncobj_surface_v1_state *synced[SYNCED_MAX];
    size_t synced_count;
} capture_walk;

/* A box in layout units to the target's px. */
static struct wlr_box capture_to_target(const capture_walk *walk, int x, int y, int width,
                                        int height) {
    double left = (x - walk->box.x) * walk->scale, top = (y - walk->box.y) * walk->scale;
    double right = (x + width - walk->box.x) * walk->scale;
    double bottom = (y + height - walk->box.y) * walk->scale;
    return (struct wlr_box){(int)lround(left), (int)lround(top),
                            (int)lround(right) - (int)lround(left),
                            (int)lround(bottom) - (int)lround(top)};
}

static void capture_draw_buffer(capture_walk *walk, struct wlr_scene_buffer *buffer, int x,
                                int y) {
    if (buffer->opacity <= 0.0f)
        return;
    xd_view *view = xd_view_from_node(&buffer->node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    struct wlr_texture *texture = nullptr;
    struct wlr_linux_drm_syncobj_surface_v1_state *sync = nullptr;
    struct wlr_buffer *pixels = buffer->buffer;
    if (scene_surface != nullptr) {
        texture = wlr_surface_get_texture(scene_surface->surface);
        sync = wlr_linux_drm_syncobj_v1_get_surface_state(scene_surface->surface);
        if (sync != nullptr && walk->synced_count == SYNCED_MAX)
            return;
        if (sync != nullptr)
            walk->synced[walk->synced_count++] = sync;
    } else {
        if (view != nullptr && buffer == view->frame) {
            capture_keep_frame(view);
            pixels = view->kept_frame;
        }
        texture = pixels != nullptr ? capture_get_cached(walk->capture, pixels) : nullptr;
    }
    if (texture == nullptr)
        return;
    int width = buffer->dst_width > 0 ? buffer->dst_width : (int)texture->width;
    int height = buffer->dst_height > 0 ? buffer->dst_height : (int)texture->height;
    if (buffer->dst_width <= 0 && (buffer->transform & WL_OUTPUT_TRANSFORM_90)) {
        int swap = width;
        width = height;
        height = swap;
    }
    struct wlr_box target = capture_to_target(walk, x, y, width, height);
    if (target.width <= 0 || target.height <= 0)
        return;
    wlr_render_pass_add_texture(walk->pass,
                                &(struct wlr_render_texture_options){
                                    .texture = texture,
                                    .src_box = buffer->src_box,
                                    .dst_box = target,
                                    .transform = buffer->transform,
                                    .alpha = &buffer->opacity,
                                    .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                                    .wait_timeline = sync != nullptr ? sync->acquire_timeline
                                                                     : nullptr,
                                    .wait_point = sync != nullptr ? sync->acquire_point : 0,
                                });
}

/* Whether a tree (of the walk's root) is left out. */
static bool capture_skips_tree(const capture_walk *walk, const struct wlr_scene_tree *tree) {
    xd_server *server = walk->capture->server;
    if (tree == server->layer_drag || tree == server->layer_lock)
        return true;
    if (walk->layers)
        return false;
    for (size_t i = 0; i < XD_ARRAY_LEN(server->layers); i++) {
        if (tree == server->layers[i])
            return true;
    }
    return tree == server->layer_background;
}

static void capture_walk_tree(capture_walk *walk, struct wlr_scene_tree *tree, int x, int y,
                              int depth);

static void capture_walk_node(capture_walk *walk, struct wlr_scene_node *node, int x, int y,
                              int depth) {
    xd_node_tag *tag = node->data;
    bool view_root = false;
    if (tag != nullptr && tag->kind == XD_NODE_VIEW) {
        xd_view *view = XD_CONTAINER_OF(tag, xd_view, tag);
        view_root = &view->tree->node == node;
        if (view_root) {
            bool include = walk->include_view != nullptr
                               ? view->mapped && walk->include_view(view, walk->data)
                               : node->enabled;
            if (!include)
                return;
        }
    }
    if (!view_root && !node->enabled)
        return;
    x += node->x;
    y += node->y;
    switch (node->type) {
    case WLR_SCENE_NODE_TREE: {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        if (!capture_skips_tree(walk, tree))
            capture_walk_tree(walk, tree, x, y, depth + 1);
        break;
    }
    case WLR_SCENE_NODE_RECT: {
        if (!walk->layers)
            break; /* a window alone: no desktop colour, no outlines */
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
        struct wlr_box target = capture_to_target(walk, x, y, rect->width, rect->height);
        float alpha = rect->color[3];
        wlr_render_pass_add_rect(walk->pass,
                                 &(struct wlr_render_rect_options){
                                     .box = target,
                                     .color = {rect->color[0] * alpha, rect->color[1] * alpha,
                                               rect->color[2] * alpha, alpha},
                                 });
        break;
    }
    case WLR_SCENE_NODE_BUFFER:
        capture_draw_buffer(walk, wlr_scene_buffer_from_node(node), x, y);
        break;
    case WLR_SCENE_NODE_SHADOW:
    case WLR_SCENE_NODE_OPTIMIZED_BLUR:
    case WLR_SCENE_NODE_BLUR:
        break; /* Aero's glass and shadows aren't drawn into pictures */
    }
}

static void capture_walk_tree(capture_walk *walk, struct wlr_scene_tree *tree, int x, int y,
                              int depth) {
    if (depth > 64)
        return;
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link) {
        /* Whole-output effects' own trees carry this tag. */
        if (child->data == &captures)
            continue;
        capture_walk_node(walk, child, x, y, depth);
    }
}

bool xd_glfx_capture_scene(xd_glfx_scene_capture *capture, struct wlr_box box, double scale,
                           bool layers, xd_glfx_view_filter include_view, void *data,
                           xd_glfx_texture *out) {
    xd_server *server = capture->server;
    int width_px = output_round_up(box.width * scale);
    int height_px = output_round_up(box.height * scale);
    if (box.width <= 0 || box.height <= 0 ||
        !output_ensure_chain(server, &capture->chain, width_px, height_px))
        return false;
    struct wlr_buffer *target = wlr_swapchain_acquire(capture->chain);
    if (target == nullptr)
        return false;
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
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer, target,
                                                                  &options);
    if (pass == nullptr) {
        wlr_buffer_unlock(target);
        return false;
    }
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                       .box = {0, 0, target->width, target->height},
                                       .color = {0, 0, 0, 0},
                                       .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                                   });
    capture_walk walk = {
        .capture = capture,
        .pass = pass,
        .box = box,
        .scale = scale,
        .layers = layers,
        .include_view = include_view,
        .data = data,
    };
    capture_walk_tree(&walk, &server->scene->tree, 0, 0, 0);
    bool ok = wlr_render_pass_submit(pass);
    for (size_t i = 0; ok && options.signal_timeline != nullptr && i < walk.synced_count; i++)
        wlr_linux_drm_syncobj_v1_state_add_release_point(walk.synced[i], options.signal_timeline,
                                                         options.signal_point, server->loop);
    capture_trim_cache(capture);
    struct wlr_texture *texture = ok ? wlr_texture_from_buffer(server->renderer, target) : nullptr;
    if (texture == nullptr) {
        wlr_buffer_unlock(target);
        return false;
    }
    capture_release_texture(capture);
    capture->buffer = target;
    capture->texture = texture;
    struct fx_texture_attribs attribs = {0};
    fx_texture_get_attribs(texture, &attribs);
    *out = (xd_glfx_texture){
        .id = attribs.tex,
        .target = attribs.target,
        .width = width_px,
        .height = height_px,
        .u_scale = (float)width_px / (float)target->width,
        .v_scale = (float)height_px / (float)target->height,
        .box = {box.x, box.y, box.width, box.height},
    };
    return true;
}

/* ---- whole-output effects --------------------------------------------------------- */

typedef struct output_picture {
    struct wlr_output *wlr_output; /* nullptr: free */
    struct wlr_scene_buffer *node;
    struct wlr_swapchain *chain;
} output_picture;

struct xd_output_effect_private {
    struct wlr_scene_tree *tree;
    output_picture pictures[MAX_OUTPUTS];
};

static bool output_reject_input([[maybe_unused]] struct wlr_scene_buffer *buffer,
                                [[maybe_unused]] double *sx, [[maybe_unused]] double *sy) {
    return false;
}

xd_output_effect *xd_glfx_begin_output(xd_server *server, const xd_output_effect_impl *impl,
                                       void *data) {
    if (server->glfx == nullptr)
        return nullptr;
    xd_output_effect *effect = XD_NEW(xd_output_effect);
    effect->impl = impl;
    effect->server = server;
    effect->data = data;
    effect->private_state = XD_NEW(struct xd_output_effect_private);
    struct wlr_scene_tree *tree = wlr_scene_tree_create(&server->scene->tree);
    /* Under the lock screen and the dragged things; captures skip it. */
    wlr_scene_node_place_below(&tree->node, &server->layer_lock->node);
    tree->node.data = &captures;
    effect->private_state->tree = tree;
    return effect;
}

void xd_glfx_end_output(xd_output_effect *effect) {
    if (effect == nullptr)
        return;
    if (effect->impl->done != nullptr)
        effect->impl->done(effect);
    struct xd_output_effect_private *private_state = effect->private_state;
    /* The nodes go with the tree. */
    wlr_scene_node_destroy(&private_state->tree->node);
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
        if (private_state->pictures[i].chain != nullptr)
            wlr_swapchain_destroy(private_state->pictures[i].chain);
    }
    xd_free(private_state);
    xd_free(effect);
}

/* The picture for the output, made the first time; those of outputs
 * gone are freed. */
static output_picture *output_get_picture(xd_output_effect *effect, xd_output *output) {
    struct xd_output_effect_private *private_state = effect->private_state;
    output_picture *free_slot = nullptr, *found = nullptr;
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
        output_picture *picture = &private_state->pictures[i];
        if (picture->wlr_output == nullptr) {
            free_slot = free_slot != nullptr ? free_slot : picture;
            continue;
        }
        bool exists = false;
        XD_LIST_FOR_EACH(other, &effect->server->outputs, xd_output, link) {
            exists = exists || other->wlr_output == picture->wlr_output;
        }
        if (!exists) {
            wlr_scene_node_destroy(&picture->node->node);
            if (picture->chain != nullptr)
                wlr_swapchain_destroy(picture->chain);
            *picture = (output_picture){0};
            free_slot = free_slot != nullptr ? free_slot : picture;
        } else if (picture->wlr_output == output->wlr_output) {
            found = picture;
        }
    }
    if (found != nullptr || free_slot == nullptr)
        return found;
    free_slot->wlr_output = output->wlr_output;
    free_slot->node = wlr_scene_buffer_create(private_state->tree, nullptr);
    free_slot->node->point_accepts_input = output_reject_input;
    return free_slot;
}

void xd_glfx_draw_output(xd_output_effect *effect, xd_output *output) {
    xd_server *server = effect->server;
    output_picture *picture = output_get_picture(effect, output);
    if (picture == nullptr || !output->wlr_output->enabled)
        return;
    struct wlr_box bounds = {0};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &bounds);
    if (bounds.width <= 0 || bounds.height <= 0)
        return;
    double scale = (double)output->wlr_output->scale;
    int width_px = output_round_up(bounds.width * scale);
    int height_px = output_round_up(bounds.height * scale);
    if (!output_ensure_chain(server, &picture->chain, width_px, height_px))
        return;
    struct wlr_buffer *target = wlr_swapchain_acquire(picture->chain);
    struct wlr_render_pass *pass =
        target != nullptr ? wlr_renderer_begin_buffer_pass(server->renderer, target,
                                                           &(struct wlr_buffer_pass_options){0})
                          : nullptr;
    if (pass == nullptr) {
        if (target != nullptr)
            wlr_buffer_unlock(target);
        return;
    }
    /* The pass made SceneFX's context current with the target bound. */
    glViewport(0, 0, width_px, height_px);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    xd_glfx_draw draw = {.bounds = bounds, .scale = scale, .glfx = server->glfx};
    xd_glfx_set_ortho(draw.projection, bounds);
    effect->impl->draw(effect, output, &draw);
    wlr_render_pass_submit(pass);

    struct wlr_scene_buffer *node = picture->node;
    wlr_scene_buffer_set_buffer(node, target);
    wlr_buffer_unlock(target); /* the scene holds its own lock */
    wlr_scene_buffer_set_source_box(node, &(struct wlr_fbox){0, 0, width_px, height_px});
    wlr_scene_buffer_set_dest_size(node, bounds.width, bounds.height);
    wlr_scene_node_set_position(&node->node, bounds.x, bounds.y);
    /* Opaque: what's under it needn't be drawn. */
    pixman_region32_t opaque;
    pixman_region32_init_rect(&opaque, 0, 0, (unsigned)bounds.width, (unsigned)bounds.height);
    wlr_scene_buffer_set_opaque_region(node, &opaque);
    pixman_region32_fini(&opaque);
}

/* ---- 3D drawing ------------------------------------------------------------------- */

static const char quad_vertex[] = "attribute vec4 position;\n"
                                  "attribute vec2 texcoord;\n"
                                  "uniform vec2 texcoord_scale;\n"
                                  "varying vec2 v_texcoord;\n"
                                  "void main() {\n"
                                  "    gl_Position = position;\n"
                                  "    v_texcoord = texcoord * texcoord_scale;\n"
                                  "}\n";

/* Textures are premultiplied: fading and darkening scale the channels. */
static const char quad_fragment[] = "precision mediump float;\n"
                                    "uniform XD_SAMPLER texture0;\n"
                                    "uniform float alpha;\n"
                                    "uniform float brightness;\n"
                                    "varying vec2 v_texcoord;\n"
                                    "void main() {\n"
                                    "    vec4 color = texture2D(texture0, v_texcoord);\n"
                                    "    gl_FragColor = vec4(color.rgb * brightness, color.a) * "
                                    "alpha;\n"
                                    "}\n";

static const char fill_vertex[] = "attribute vec4 position;\n"
                                  "void main() {\n"
                                  "    gl_Position = position;\n"
                                  "}\n";

static const char fill_fragment[] = "precision mediump float;\n"
                                    "uniform vec4 color;\n"
                                    "void main() {\n"
                                    "    gl_FragColor = color;\n"
                                    "}\n";

/* Compiled the first time (a pass is running: the context is current);
 * they go with the renderer's context. */
static struct {
    bool tried;
    xd_glfx_shader quad_2d, quad_external, fill;
    GLint brightness_2d, brightness_external, fill_color;
} shaders;

static void output_init_shaders(void) {
    if (shaders.tried)
        return;
    shaders.tried = true;
    if (xd_glfx_init_shader(&shaders.quad_2d, quad_vertex, quad_fragment, GL_TEXTURE_2D))
        shaders.brightness_2d = glGetUniformLocation(shaders.quad_2d.program, "brightness");
    if (xd_glfx_init_shader(&shaders.quad_external, quad_vertex, quad_fragment,
                            GL_TEXTURE_EXTERNAL_OES))
        shaders.brightness_external =
            glGetUniformLocation(shaders.quad_external.program, "brightness");
    if (xd_glfx_init_shader(&shaders.fill, fill_vertex, fill_fragment, GL_TEXTURE_2D))
        shaders.fill_color = glGetUniformLocation(shaders.fill.program, "color");
}

void xd_glfx_draw_quad([[maybe_unused]] const xd_glfx_draw *draw, const xd_glfx_texture *texture,
                       const float corners[4][4], float alpha, float brightness) {
    output_init_shaders();
    bool external = texture->target == GL_TEXTURE_EXTERNAL_OES;
    const xd_glfx_shader *shader = external ? &shaders.quad_external : &shaders.quad_2d;
    GLint brightness_uniform = external ? shaders.brightness_external : shaders.brightness_2d;
    if (shader->program == 0 || texture->id == 0)
        return;
    static const float texcoords[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    float positions[16];
    memcpy(positions, corners, sizeof(positions));
    glUseProgram(shader->program);
    if (shader->alpha >= 0)
        glUniform1f(shader->alpha, alpha);
    if (brightness_uniform >= 0)
        glUniform1f(brightness_uniform, brightness);
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
        glVertexAttribPointer((GLuint)shader->position, 4, GL_FLOAT, GL_FALSE, 0, positions);
    }
    if (shader->texcoord >= 0) {
        glEnableVertexAttribArray((GLuint)shader->texcoord);
        glVertexAttribPointer((GLuint)shader->texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    if (shader->position >= 0)
        glDisableVertexAttribArray((GLuint)shader->position);
    if (shader->texcoord >= 0)
        glDisableVertexAttribArray((GLuint)shader->texcoord);
    glBindTexture(texture->target, 0);
    glUseProgram(0);
}

void xd_glfx_fill_polygon([[maybe_unused]] const xd_glfx_draw *draw, const float (*corners)[4],
                          size_t count, const float color[4]) {
    output_init_shaders();
    if (shaders.fill.program == 0 || count < 3)
        return;
    glUseProgram(shaders.fill.program);
    if (shaders.fill_color >= 0)
        glUniform4f(shaders.fill_color, color[0] * color[3], color[1] * color[3],
                    color[2] * color[3], color[3]);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (shaders.fill.position >= 0) {
        glEnableVertexAttribArray((GLuint)shaders.fill.position);
        glVertexAttribPointer((GLuint)shaders.fill.position, 4, GL_FLOAT, GL_FALSE, 0,
                              &corners[0][0]);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)count);
    if (shaders.fill.position >= 0)
        glDisableVertexAttribArray((GLuint)shaders.fill.position);
    glUseProgram(0);
}
