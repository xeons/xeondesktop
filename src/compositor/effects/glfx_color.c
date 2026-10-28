/* SPDX-License-Identifier: MIT
 * Window colours (glfx.h, xd-compositor-fx only): Window Opacity's
 * brightness and saturation, which the scene graph can't draw. A window
 * whose colours change is captured (as a window effect's is) and drawn
 * once more through a shader into a picture, which a scene buffer node at
 * the top of its tree shows at the window's opacity, its own buffers
 * drawn fully transparent. Unlike a window effect's, the picture is drawn
 * again only when the window changes: a commit of one of its surfaces,
 * its frame redrawn, a popup, a move within it, another scale, other
 * colours. Its blur and shadow stay, as they are.
 *
 * A window effect on the window (Wobbly, Magic Lamp, Burn, Explode...)
 * draws it meanwhile, from a capture this file colours as well, so the
 * window keeps its colours while it wobbles or burns; the picture's node
 * is left out of that capture and off till the effect is over, then
 * drawn afresh.
 */
#include "glfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "../../core/alloc.h"
#include "opacity_model.h"

/* Our pictures are a whole number of these px across and down, as
 * glfx.c's captures (llvmpipe and wlroots' udmabuf allocator). */
static constexpr int PICTURE_STEP = 64;
/* XD_EFFECTS_TIMING: what drawing the pictures costs, logged over this
 * long. */
static constexpr double TIMING_SECONDS = 2.0;

typedef struct color_window {
    xd_view *view;
    float opacity, brightness, saturation;
    /* What glfx_capture_window draws the window with: its view, our node
     * (left out), its swapchain and the frame's texture. Never in glfx's
     * list of effects. */
    xd_window_effect context;
    struct wlr_scene_buffer *node; /* the picture, in the window's tree */
    struct wlr_swapchain *picture_chain;
    struct wlr_swapchain *adjust_chain; /* window effects' captures, coloured */
    uint64_t signature;                 /* what the picture shows */
    bool drawn;                         /* the node has a picture */
    bool stale; /* drawn before a window effect: draw it again */
} color_window;

typedef struct color_shader {
    xd_glfx_shader base;
    GLint brightness, saturation;
} color_shader;

static struct {
    color_window **windows;
    size_t count, capacity;
    color_shader shader_2d, shader_external;
    bool shaders_tried;
    /* XD_EFFECTS_TIMING */
    int timing; /* 0: not checked yet, 1: off, 2: on */
    double timing_start, draw_total;
    int pictures;
} colors;

/* glfx.h's draw_mesh names, and the window's colours: greyed towards its
 * luminance (Compiz's weights, opacity_model.h's) by saturation, then
 * darkened by brightness. Premultiplied, so alpha needn't change. */
static const char color_vertex[] = "uniform mat4 projection;\n"
                                   "uniform vec2 texcoord_scale;\n"
                                   "attribute vec2 position;\n"
                                   "attribute vec2 texcoord;\n"
                                   "varying vec2 v_texcoord;\n"
                                   "void main() {\n"
                                   "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                   "    v_texcoord = texcoord * texcoord_scale;\n"
                                   "}\n";

static const char color_fragment[] =
    "precision mediump float;\n"
    "uniform XD_SAMPLER texture0;\n"
    "uniform float alpha;\n"
    "uniform float brightness;\n"
    "uniform float saturation;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    vec4 color = texture2D(texture0, v_texcoord);\n"
    "    float grey = dot(color.rgb, vec3(0.30, 0.59, 0.11));\n"
    "    color.rgb = mix(vec3(grey), color.rgb, saturation) * brightness;\n"
    "    gl_FragColor = color * alpha;\n"
    "}\n";

static double color_get_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static bool color_is_timing(void) {
    if (colors.timing == 0)
        colors.timing = getenv("XD_EFFECTS_TIMING") != nullptr ? 2 : 1;
    return colors.timing == 2;
}

/* A picture drawn in `seconds`: the averages, every TIMING_SECONDS. */
static void color_count_picture(double now, double seconds) {
    if (colors.pictures == 0 && colors.draw_total == 0)
        colors.timing_start = now;
    colors.draw_total += seconds;
    colors.pictures++;
    if (now - colors.timing_start < TIMING_SECONDS)
        return;
    wlr_log(WLR_INFO,
            "effects: window colours: %d pictures in %.1f s, %.2f ms each (capture and colours)",
            colors.pictures, now - colors.timing_start,
            colors.draw_total / colors.pictures * 1000.0);
    colors.pictures = 0;
    colors.draw_total = 0;
}

/* ---- shaders and drawing ---------------------------------------------------------- */

static bool color_init_shader(color_shader *shader, GLenum target) {
    if (!xd_glfx_init_shader(&shader->base, color_vertex, color_fragment, target))
        return false;
    shader->brightness = glGetUniformLocation(shader->base.program, "brightness");
    shader->saturation = glGetUniformLocation(shader->base.program, "saturation");
    return true;
}

/* Compiled the first time (a pass is running: the context is current). */
static const color_shader *color_get_shader(GLenum target) {
    if (!colors.shaders_tried) {
        colors.shaders_tried = true;
        if (!color_init_shader(&colors.shader_2d, GL_TEXTURE_2D))
            wlr_log(WLR_ERROR, "effects: no shader for window colours");
        /* Not every driver has external textures; ours are 2D. */
        if (!color_init_shader(&colors.shader_external, GL_TEXTURE_EXTERNAL_OES))
            wlr_log(WLR_INFO, "effects: no shader for window colours of external textures");
    }
    const color_shader *shader =
        target == GL_TEXTURE_EXTERNAL_OES ? &colors.shader_external : &colors.shader_2d;
    return shader->base.program != 0 ? shader : nullptr;
}

/* `source`'s top left width x height px drawn with the colours into a
 * buffer from `chain`, px for px: the buffer (locked), or nullptr. */
static struct wlr_buffer *color_draw(xd_server *server, struct wlr_swapchain **chain,
                                     const xd_glfx_texture *source, int width, int height,
                                     float brightness, float saturation) {
    if (!glfx_ensure_chain(server, chain, width, height, PICTURE_STEP))
        return nullptr;
    struct wlr_buffer *target = wlr_swapchain_acquire(*chain);
    if (target == nullptr)
        return nullptr;
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
        server->renderer, target, &(struct wlr_buffer_pass_options){0});
    if (pass == nullptr) {
        wlr_buffer_unlock(target);
        return nullptr;
    }
    /* The pass made SceneFX's context current with the target bound. */
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    const color_shader *shader = color_get_shader(source->target);
    xd_glfx_mesh mesh;
    if (shader != nullptr && xd_glfx_init_mesh(&mesh, 1, 1)) {
        xd_glfx_draw draw = {.bounds = {0, 0, width, height}, .scale = 1.0};
        xd_glfx_set_ortho(draw.projection, draw.bounds);
        xd_glfx_set_mesh_rect(&mesh, (struct wlr_fbox){0, 0, width, height});
        glUseProgram(shader->base.program);
        glUniform1f(shader->brightness, brightness);
        glUniform1f(shader->saturation, saturation);
        xd_glfx_draw_mesh_with(&draw, &mesh, source, &shader->base, 1.0f);
        xd_glfx_free_mesh(&mesh);
    }
    if (!wlr_render_pass_submit(pass) || shader == nullptr) {
        wlr_buffer_unlock(target);
        return nullptr;
    }
    return target;
}

/* A capture's size in px: what of its buffer it fills. */
static void color_get_capture_size(const xd_window_effect *effect, int *width, int *height) {
    const struct wlr_buffer *buffer = effect->capture_buffer;
    *width = XD_MAX((int)lroundf(effect->texture.u_scale * (float)buffer->width), 1);
    *height = XD_MAX((int)lroundf(effect->texture.v_scale * (float)buffer->height), 1);
}

/* ---- the windows ------------------------------------------------------------------ */

static color_window *color_find(const xd_view *view) {
    for (size_t i = 0; view != nullptr && i < colors.count; i++) {
        if (colors.windows[i]->view == view)
            return colors.windows[i];
    }
    return nullptr;
}

static bool color_is_context(const xd_window_effect *effect) {
    for (size_t i = 0; i < colors.count; i++) {
        if (&colors.windows[i]->context == effect)
            return true;
    }
    return false;
}

/* Input goes to the window under the picture, where it really is. */
static bool color_reject_input([[maybe_unused]] struct wlr_scene_buffer *buffer,
                               [[maybe_unused]] double *sx, [[maybe_unused]] double *sy) {
    return false;
}

static color_window *color_add(xd_view *view) {
    if (colors.count == colors.capacity) {
        colors.capacity = XD_MAX(colors.capacity * 2, (size_t)8);
        colors.windows =
            xd_realloc_array(colors.windows, colors.capacity, sizeof(*colors.windows));
    }
    color_window *window = XD_NEW(color_window);
    window->view = view;
    window->context.view = view;
    /* Its frame's pixels, for the captures (the scene graph lets go of
     * them). */
    xd_frame_keep_pixels(view, true);
    colors.windows[colors.count++] = window;
    return window;
}

/* Whether Aero Peek shows the window (all of them when it isn't on), as
 * glfx.c's glfx_show_window has it. */
static bool color_is_peek_shown(const xd_view *view) {
    const xd_server *server = view->server;
    const xd_view *peeked = server->peek_view;
    return !server->peeking ||
           (peeked != nullptr && (view == peeked || xd_view_get_owner(view) == peeked));
}

typedef struct opacity_change {
    struct wlr_scene_buffer *skip;
    float opacity;
} opacity_change;

static void color_set_opacity(struct wlr_scene_buffer *buffer, [[maybe_unused]] int sx,
                              [[maybe_unused]] int sy, void *data) {
    const opacity_change *change = data;
    if (buffer != change->skip)
        wlr_scene_buffer_set_opacity(buffer, change->opacity);
}

/* The picture in the window's place. While Aero Peek hides the window,
 * what it set stays (the picture's content hidden, the frame an outline);
 * while it shows it, the picture shows as it set it. */
static void color_show(color_window *window) {
    xd_view *view = window->view;
    if (!color_is_peek_shown(view))
        return;
    wlr_scene_node_set_enabled(&window->node->node, true);
    opacity_change change = {window->node, 0.0f};
    wlr_scene_node_for_each_buffer(&view->tree->node, color_set_opacity, &change);
    if (!view->server->peeking)
        wlr_scene_buffer_set_opacity(window->node, window->opacity);
}

static void color_drop(color_window *window) {
    for (size_t i = 0; i < colors.count; i++) {
        if (colors.windows[i] == window) {
            colors.windows[i] = colors.windows[--colors.count];
            break;
        }
    }
    xd_view *view = window->view;
    if (window->node != nullptr)
        wlr_scene_node_destroy(&window->node->node);
    /* Its buffers as they were before: Window Opacity's, or Aero Peek's
     * while it shows the window. */
    if (window->drawn && color_is_peek_shown(view)) {
        opacity_change change = {nullptr, view->server->peeking ? 1.0f : window->opacity};
        wlr_scene_node_for_each_buffer(&view->tree->node, color_set_opacity, &change);
    }
    xd_window_effect *context = &window->context;
    glfx_release_capture(context);
    if (context->frame_texture != nullptr)
        wlr_texture_destroy(context->frame_texture);
    if (context->frame_buffer != nullptr)
        wlr_buffer_unlock(context->frame_buffer);
    wlr_swapchain_destroy(context->capture_chain);
    wlr_swapchain_destroy(window->picture_chain);
    wlr_swapchain_destroy(window->adjust_chain);
    xd_frame_keep_pixels(view, false);
    xd_free(window);
}

/* ---- what the picture shows ---------------------------------------------------------- */

static constexpr uint64_t FNV_OFFSET = 14695981039346656037u;
static constexpr uint64_t FNV_PRIME = 1099511628211u;

static void color_mix(uint64_t *hash, uint64_t value) {
    for (size_t i = 0; i < sizeof(value); i++) {
        *hash ^= (value >> (i * 8)) & 0xff;
        *hash *= FNV_PRIME;
    }
}

static void color_mix_double(uint64_t *hash, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    color_mix(hash, bits);
}

typedef struct signature_walk {
    const color_window *window;
    int origin_x, origin_y; /* the window's tree, layout */
    uint64_t hash;
} signature_walk;

/* Each buffer as the capture would draw it: which, its pixels (a
 * surface's change with each commit, not always their buffer), where in
 * the window, how big and which part. */
static void color_hash_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    signature_walk *walk = data;
    const xd_view *view = walk->window->view;
    if (buffer == walk->window->node)
        return;
    struct wlr_buffer *pixels = buffer == view->frame ? view->kept_frame : buffer->buffer;
    uint64_t *hash = &walk->hash;
    color_mix(hash, (uintptr_t)buffer);
    color_mix(hash, (uintptr_t)pixels);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    color_mix(hash, scene_surface != nullptr ? scene_surface->surface->current.seq : 0);
    color_mix(hash, (uint64_t)(int64_t)(sx - walk->origin_x));
    color_mix(hash, (uint64_t)(int64_t)(sy - walk->origin_y));
    color_mix(hash, (uint64_t)(int64_t)buffer->dst_width);
    color_mix(hash, (uint64_t)(int64_t)buffer->dst_height);
    color_mix(hash, (uint64_t)buffer->transform);
    color_mix_double(hash, buffer->src_box.x);
    color_mix_double(hash, buffer->src_box.y);
    color_mix_double(hash, buffer->src_box.width);
    color_mix_double(hash, buffer->src_box.height);
}

static uint64_t color_get_signature(const color_window *window) {
    signature_walk walk = {.window = window, .hash = FNV_OFFSET};
    xd_view *view = window->view;
    wlr_scene_node_coords(&view->tree->node, &walk.origin_x, &walk.origin_y);
    wlr_scene_node_for_each_buffer(&view->tree->node, color_hash_buffer, &walk);
    color_mix_double(&walk.hash, xd_view_scale(view));
    color_mix_double(&walk.hash, (double)window->brightness);
    color_mix_double(&walk.hash, (double)window->saturation);
    return walk.hash;
}

/* Captures the window and draws its picture with the colours. */
static bool color_redraw(color_window *window) {
    xd_view *view = window->view;
    xd_server *server = view->server;
    if (window->node == nullptr) {
        window->node = wlr_scene_buffer_create(view->tree, nullptr);
        window->node->point_accepts_input = color_reject_input;
        wlr_scene_node_set_enabled(&window->node->node, false);
        window->context.node = window->node;
    }
    double started = color_is_timing() ? color_get_now() : 0;
    xd_window_effect *context = &window->context;
    if (!glfx_capture_window(context))
        return false;
    int width = 0, height = 0;
    color_get_capture_size(context, &width, &height);
    struct wlr_buffer *picture = color_draw(server, &window->picture_chain, &context->texture,
                                            width, height, window->brightness,
                                            window->saturation);
    struct wlr_fbox box = context->texture.box;
    glfx_release_capture(context);
    if (picture == nullptr)
        return false;
    if (color_is_timing()) {
        double now = color_get_now();
        color_count_picture(now, now - started);
    }
    struct wlr_scene_buffer *node = window->node;
    wlr_scene_buffer_set_buffer(node, picture);
    wlr_buffer_unlock(picture); /* the scene holds its own lock */
    wlr_scene_buffer_set_source_box(node, &(struct wlr_fbox){0, 0, width, height});
    wlr_scene_buffer_set_dest_size(node, (int)box.width, (int)box.height);
    int origin_x = 0, origin_y = 0;
    wlr_scene_node_coords(&view->tree->node, &origin_x, &origin_y);
    wlr_scene_node_set_position(&node->node, (int)box.x - origin_x, (int)box.y - origin_y);
    wlr_scene_node_raise_to_top(&node->node);
    return true;
}

/* ---- glfx.h ------------------------------------------------------------------------ */

bool xd_glfx_set_window_color(xd_view *view, float opacity, float brightness,
                              float saturation) {
    if (view->server->glfx == nullptr)
        return false;
    color_window *window = color_find(view);
    if (!xd_opacity_is_colored(brightness, saturation)) {
        if (window != nullptr)
            color_drop(window);
        return false;
    }
    if (window == nullptr)
        window = color_add(view);
    window->opacity = opacity;
    window->brightness = brightness;
    window->saturation = saturation;
    /* Not drawn yet (this frame's glfx_color_frame will), or an effect's. */
    if (!window->drawn || xd_glfx_find_window(view, nullptr) != nullptr)
        return false;
    color_show(window);
    return true;
}

void glfx_color_prepare_capture(xd_window_effect *effect) {
    color_window *window = color_find(effect->view);
    if (window == nullptr || &window->context == effect || window->node == nullptr)
        return;
    /* Left out of the effect's picture, which has the colours anyway;
     * off till the effect is over (the effect draws the window). */
    wlr_scene_node_set_enabled(&window->node->node, false);
    window->stale = true;
}

void glfx_color_adjust_capture(xd_window_effect *effect) {
    color_window *window = color_find(effect->view);
    if (window == nullptr || color_is_context(effect) || effect->capture_buffer == nullptr)
        return;
    xd_server *server = window->view->server;
    int width = 0, height = 0;
    color_get_capture_size(effect, &width, &height);
    struct wlr_buffer *picture = color_draw(server, &window->adjust_chain, &effect->texture,
                                            width, height, window->brightness,
                                            window->saturation);
    struct wlr_texture *texture =
        picture != nullptr ? wlr_texture_from_buffer(server->renderer, picture) : nullptr;
    if (texture == nullptr) {
        if (picture != nullptr)
            wlr_buffer_unlock(picture);
        return; /* drawn as it is */
    }
    /* The effect keeps ours in place of its capture: a closing effect for
     * as long as it runs, past the window and the chain. */
    wlr_texture_destroy(effect->capture_texture);
    wlr_buffer_unlock(effect->capture_buffer);
    struct fx_texture_attribs attribs = {0};
    fx_texture_get_attribs(texture, &attribs);
    effect->capture_texture = texture;
    effect->capture_buffer = picture;
    effect->texture.id = attribs.tex;
    effect->texture.target = attribs.target;
    effect->texture.u_scale = (float)width / (float)picture->width;
    effect->texture.v_scale = (float)height / (float)picture->height;
}

void glfx_color_frame([[maybe_unused]] xd_server *server) {
    for (size_t i = 0; i < colors.count; i++) {
        color_window *window = colors.windows[i];
        xd_view *view = window->view;
        if (!view->mapped)
            continue;
        if (xd_glfx_find_window(view, nullptr) != nullptr) {
            window->stale = true;
            continue;
        }
        /* Every output's frame, but drawn again only when it changed: the
         * same moment's other frames find it as it is. */
        uint64_t signature = color_get_signature(window);
        if (!window->drawn || window->stale || signature != window->signature) {
            if (color_redraw(window)) {
                window->signature = signature;
                window->drawn = true;
                window->stale = false;
            }
        }
        if (window->drawn)
            color_show(window);
    }
}

void glfx_color_forget_view(xd_view *view) {
    color_window *window = color_find(view);
    if (window != nullptr)
        color_drop(window);
}

void glfx_color_finish([[maybe_unused]] xd_server *server) {
    while (colors.count > 0)
        color_drop(colors.windows[colors.count - 1]);
    XD_FREE(&colors.windows);
    colors.capacity = 0;
    /* The shaders go with glfx's (xd_glfx_finish). */
    colors.shaders_tried = false;
}
