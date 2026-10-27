/* SPDX-License-Identifier: MIT
 * Enhanced Zoom Desktop (both builds): Compiz's ezoom. The Windows key
 * and the wheel zoom a monitor in and out around the pointer, smoothly
 * (zoom_model.h), and the view follows the pointer.
 *
 * Neither scene graph scales an output's view, so each frame of a zoomed
 * monitor is drawn twice: the scene as usual into buffers of our own (a
 * swapchain the size of the monitor's), then that picture, the part
 * around the pointer stretched over the whole monitor, by a scene buffer
 * above everything, which the monitor's own frame then shows. The second
 * frame is drawn only when the first changed or the view moved, so a
 * still screen costs nothing. Input isn't moved: the zoom keeps what's
 * under the pointer under it, so clicks land where they look.
 */
#include <math.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output.h>

#include "../../core/alloc.h"
#include "effects.h"
#include "zoom_model.h"

/* Below this the zoom is over. */
static constexpr double NO_ZOOM = 1.0;
static constexpr double MAX_FRAME_SECONDS = 0.1;

typedef struct zoom_options {
    double factor;
    double maximum;
    double speed;
    xd_effect_binding zoom_in, zoom_out;
} zoom_options;

typedef struct zoom_state zoom_state;

/* A monitor zoomed in, or zooming back out. */
typedef struct zoom_output {
    struct wl_list link; /* zoom_state.outputs */
    zoom_state *state;
    struct wlr_output *output;
    double level, target;
    double last_time;
    xd_effect_rect view; /* what was shown last */
    struct wlr_swapchain *swapchain;
    struct wlr_scene_buffer *node;
    struct wl_listener destroy;
} zoom_output;

struct zoom_state {
    xd_server *server;
    zoom_options options;
    struct wlr_scene_tree *tree; /* above everything, the lock screen too */
    struct wl_list outputs;      /* zoom_output.link */
};

static void zoom_free_output(zoom_output *zoom) {
    wlr_output_lock_attach_render(zoom->output, false);
    wlr_scene_node_destroy(&zoom->node->node);
    if (zoom->swapchain != nullptr)
        wlr_swapchain_destroy(zoom->swapchain);
    xd_unlisten(&zoom->destroy);
    wl_list_remove(&zoom->link);
    xd_free(zoom);
}

static void zoom_handle_output_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    zoom_free_output(XD_CONTAINER_OF(listener, zoom_output, destroy));
}

static zoom_output *zoom_find_output(zoom_state *state, const struct wlr_output *output) {
    zoom_output *zoom;
    wl_list_for_each(zoom, &state->outputs, link) {
        if (zoom->output == output)
            return zoom;
    }
    return nullptr;
}

static zoom_output *zoom_add_output(zoom_state *state, struct wlr_output *output) {
    zoom_output *zoom = XD_NEW(zoom_output);
    zoom->state = state;
    zoom->output = output;
    zoom->level = zoom->target = NO_ZOOM;
    zoom->node = wlr_scene_buffer_create(state->tree, nullptr);
    wlr_scene_buffer_set_filter_mode(zoom->node, WLR_SCALE_FILTER_BILINEAR);
    wlr_scene_node_set_enabled(&zoom->node->node, false);
    XD_LISTEN(&output->events.destroy, zoom, destroy, zoom_handle_output_destroy);
    /* The second drawing needs the first in our buffers, never a
     * program's buffer scanned out directly. */
    wlr_output_lock_attach_render(output, true);
    wl_list_insert(&state->outputs, &zoom->link);
    return zoom;
}

/* A swapchain of the monitor's size and format, made again when those
 * change. */
static bool zoom_ensure_swapchain(zoom_output *zoom) {
    struct wlr_output *output = zoom->output;
    if (output->swapchain == nullptr)
        return false;
    struct wlr_swapchain *swapchain = zoom->swapchain;
    if (swapchain != nullptr &&
        (swapchain->width != output->width || swapchain->height != output->height ||
         swapchain->format.format != output->swapchain->format.format)) {
        wlr_swapchain_destroy(swapchain);
        zoom->swapchain = nullptr;
    }
    if (zoom->swapchain == nullptr)
        zoom->swapchain = wlr_swapchain_create(zoom->state->server->allocator, output->width,
                                               output->height, &output->swapchain->format);
    return zoom->swapchain != nullptr;
}

/* The scene drawn as usual into our own buffer, then shown stretched. */
static void zoom_draw(zoom_output *zoom, struct wlr_scene_output *scene_output, struct wlr_box box,
                      xd_effect_rect view) {
    if (!zoom_ensure_swapchain(zoom))
        return;
    wlr_scene_node_set_enabled(&zoom->node->node, false);
    struct wlr_output_state output_state;
    wlr_output_state_init(&output_state);
    struct wlr_scene_output_state_options options = {.swapchain = zoom->swapchain};
    if (wlr_scene_output_build_state(scene_output, &output_state, &options) &&
        output_state.buffer != nullptr) {
        double scale = (double)zoom->output->scale;
        struct wlr_fbox source = {
            .x = (view.x - box.x) * scale,
            .y = (view.y - box.y) * scale,
            .width = view.width * scale,
            .height = view.height * scale,
        };
        wlr_scene_buffer_set_buffer(zoom->node, output_state.buffer);
        wlr_scene_buffer_set_source_box(zoom->node, &source);
        wlr_scene_buffer_set_dest_size(zoom->node, box.width, box.height);
        wlr_scene_node_set_position(&zoom->node->node, box.x, box.y);
    }
    wlr_output_state_finish(&output_state);
    wlr_scene_node_set_enabled(&zoom->node->node, true);
}

static void zoom_handle_frame(xd_server *server, void *data, xd_output *output, double now) {
    zoom_state *state = data;
    zoom_output *zoom = zoom_find_output(state, output->wlr_output);
    if (zoom == nullptr)
        return;
    double seconds = zoom->last_time > 0 ? XD_MIN(now - zoom->last_time, MAX_FRAME_SECONDS) : 0;
    zoom->last_time = now;
    zoom->level = xd_zoom_step_level(zoom->level, zoom->target, seconds, state->options.speed);
    if (zoom->level <= NO_ZOOM && zoom->target <= NO_ZOOM) {
        zoom_free_output(zoom); /* the scene shows itself again */
        xd_effects_schedule_frames(server);
        return;
    }
    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(server->scene, output->wlr_output);
    if (scene_output == nullptr)
        return;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
    /* The pointer on another monitor leaves this one's view where it was. */
    xd_effect_rect view = zoom->view;
    if (wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y) ==
            output->wlr_output ||
        view.width <= 0)
        view = xd_zoom_get_view((xd_effect_rect){box.x, box.y, box.width, box.height},
                                server->cursor->x, server->cursor->y, zoom->level);
    bool moved = view.x != zoom->view.x || view.y != zoom->view.y ||
                 view.width != zoom->view.width || view.height != zoom->view.height;
    zoom->view = view;
    if (moved || wlr_scene_output_needs_frame(scene_output))
        zoom_draw(zoom, scene_output, box, view);
    if (zoom->level != zoom->target)
        wlr_output_schedule_frame(output->wlr_output);
    else
        zoom->last_time = 0;
}

static bool zoom_handle_input(xd_server *server, void *data, const xd_effect_input *input) {
    zoom_state *state = data;
    if (input->kind == XD_EFFECT_INPUT_MOTION) {
        /* The view follows the pointer. */
        if (!wl_list_empty(&state->outputs))
            xd_effects_schedule_frames(server);
        return false;
    }
    if (input->kind != XD_EFFECT_INPUT_AXIS && input->kind != XD_EFFECT_INPUT_BUTTON)
        return false;
    bool zoom_in = xd_effects_binding_matches(&state->options.zoom_in, input);
    bool zoom_out = !zoom_in && xd_effects_binding_matches(&state->options.zoom_out, input);
    if (!zoom_in && !zoom_out)
        return false;
    xd_output *output = xd_output_at(server, input->x, input->y);
    if (output == nullptr)
        return true;
    zoom_output *zoom = zoom_find_output(state, output->wlr_output);
    if (zoom == nullptr) {
        if (zoom_out)
            return true;
        zoom = zoom_add_output(state, output->wlr_output);
    }
    int steps = input->kind == XD_EFFECT_INPUT_AXIS ? input->steps : 1;
    double factor = pow(state->options.factor, zoom_in ? steps : -steps);
    zoom->target = XD_CLAMP(zoom->target * factor, NO_ZOOM, state->options.maximum);
    wlr_output_schedule_frame(output->wlr_output);
    return true;
}

static void *zoom_enable(xd_server *server) {
    zoom_state *state = XD_NEW(zoom_state);
    state->server = server;
    state->tree = wlr_scene_tree_create(&server->scene->tree);
    wl_list_init(&state->outputs);
    return state;
}

static void zoom_disable(xd_server *server, void *data) {
    zoom_state *state = data;
    zoom_output *zoom, *next;
    wl_list_for_each_safe(zoom, next, &state->outputs, link) {
        zoom_free_output(zoom);
    }
    wlr_scene_node_destroy(&state->tree->node);
    xd_free(state);
    xd_effects_schedule_frames(server);
}

static void zoom_configure([[maybe_unused]] xd_server *server, void *data,
                           const xd_effects_settings *settings, size_t effect) {
    zoom_state *state = data;
    zoom_options *options = &state->options;
    options->factor = xd_effects_get_double(settings, effect, "ZoomFactor");
    options->maximum = xd_effects_get_int(settings, effect, "MaximumZoom");
    options->speed = xd_effects_get_double(settings, effect, "Speed");
    xd_effects_get_binding(settings, effect, "ZoomInButton", &options->zoom_in);
    xd_effects_get_binding(settings, effect, "ZoomOutButton", &options->zoom_out);
}

const xd_effect_impl xd_effect_impl_enhanced_zoom = {
    .id = "EnhancedZoom",
    .enable = zoom_enable,
    .disable = zoom_disable,
    .configure = zoom_configure,
    .frame = zoom_handle_frame,
    .input = zoom_handle_input,
};
