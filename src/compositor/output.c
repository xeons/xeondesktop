/* SPDX-License-Identifier: MIT
 * Outputs (monitors): mode and scale selection, frame scheduling, desktop
 * background, and work areas.
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

/* Picks a scale from the panel's physical DPI, rounded to the 25% steps
 * Windows offers (100%, 125%, 150%, ...). */
static float auto_scale(const struct wlr_output *output, int width_px) {
    if (output->phys_width <= 0 || width_px <= 0)
        return 1.0f;
    double dpi = width_px * 25.4 / output->phys_width;
    double scale = round(dpi / 96.0 * 4.0) / 4.0;
    /* Implausible EDID sizes (projectors, broken monitors) stay at 100%. */
    if (!(scale >= 1.0 && scale <= 4.0))
        return 1.0f;
    return (float)scale;
}

void xd_output_update_backgrounds(xd_server *server) {
    xd_rgb bg = server->config.scheme.colors[XD_COLOR_BACKGROUND];
    const float color[4] = {(float)bg.r / 255.0f, (float)bg.g / 255.0f, (float)bg.b / 255.0f,
                            1.0f};
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
        if (out->background == nullptr) {
            out->background =
                wlr_scene_rect_create(server->layer_background, box.width, box.height, color);
        } else {
            wlr_scene_rect_set_size(out->background, box.width, box.height);
            wlr_scene_rect_set_color(out->background, color);
        }
        wlr_scene_node_set_position(&out->background->node, box.x, box.y);
    }
}

static void handle_layout_change(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, layout_change);
    xd_output_update_backgrounds(server);
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        xd_output_arrange(out);
    }
    /* Windows may now sit on an output with a different scale. */
    xd_view_update_all_frames(server);
}

static void handle_frame(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_output *out = XD_CONTAINER_OF(listener, xd_output, frame);
    struct wlr_scene_output *so = wlr_scene_get_scene_output(out->server->scene, out->wlr_output);
    if (so == nullptr)
        return;
    wlr_scene_output_commit(so, nullptr);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(so, &now);
}

static void handle_request_state(struct wl_listener *listener, void *data) {
    /* Nested backends (X11/Wayland windows) ask for this when resized. */
    xd_output *out = XD_CONTAINER_OF(listener, xd_output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(out->wlr_output, event->state);
}

static void handle_output_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_output *out = XD_CONTAINER_OF(listener, xd_output, destroy);
    /* Layer surfaces belong to one output; their clients get `closed`. */
    XD_LIST_FOR_EACH_SAFE(ls, tmp, &out->layer_surfaces, xd_layer_surface, link) {
        wlr_layer_surface_v1_destroy(ls->layer_surface);
    }
    xd_view_output_gone(out->server, out->wlr_output);
    xd_unlisten(&out->frame);
    xd_unlisten(&out->request_state);
    xd_unlisten(&out->destroy);
    if (out->background != nullptr)
        wlr_scene_node_destroy(&out->background->node);
    wl_list_remove(&out->link);
    xd_free(out);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_output);
    struct wlr_output *wlr_output = data;

    if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
        wlr_log(WLR_ERROR, "cannot init rendering on output %s", wlr_output->name);
        return;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    int width = wlr_output->width;
    if (mode != nullptr) {
        wlr_output_state_set_mode(&state, mode);
        width = mode->width;
    }
    float scale = server->config.forced_scale > 0 ? (float)server->config.forced_scale
                                                  : auto_scale(wlr_output, width);
    wlr_output_state_set_scale(&state, scale);
    bool ok = wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
    if (!ok) {
        wlr_log(WLR_ERROR, "cannot enable output %s", wlr_output->name);
        return;
    }
    wlr_log(WLR_INFO, "output %s: %dx%d, scale %.2f", wlr_output->name, wlr_output->width,
            wlr_output->height, (double)wlr_output->scale);

    xd_output *out = XD_NEW(xd_output);
    out->server = server;
    out->wlr_output = wlr_output;
    wl_list_init(&out->layer_surfaces);
    XD_LISTEN(&wlr_output->events.frame, out, frame, handle_frame);
    XD_LISTEN(&wlr_output->events.request_state, out, request_state, handle_request_state);
    XD_LISTEN(&wlr_output->events.destroy, out, destroy, handle_output_destroy);
    wl_list_insert(server->outputs.prev, &out->link);

    struct wlr_output_layout_output *lo =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *so = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, lo, so);
    xd_output_update_backgrounds(server);
    xd_output_arrange(out);
}

xd_output *xd_output_from_wlr(xd_server *server, struct wlr_output *wlr_output) {
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (out->wlr_output == wlr_output)
            return out;
    }
    return nullptr;
}

static struct wlr_output *output_at(xd_server *server, double lx, double ly) {
    struct wlr_output *o = wlr_output_layout_output_at(server->output_layout, lx, ly);
    if (o == nullptr)
        o = wlr_output_layout_get_center_output(server->output_layout);
    return o;
}

double xd_output_scale_at(xd_server *server, double lx, double ly) {
    struct wlr_output *o = output_at(server, lx, ly);
    return o != nullptr && o->scale > 0 ? (double)o->scale : 1.0;
}

xd_output *xd_output_at(xd_server *server, double lx, double ly) {
    struct wlr_output *o = output_at(server, lx, ly);
    return o != nullptr ? xd_output_from_wlr(server, o) : nullptr;
}

struct wlr_box xd_output_work_area_at(xd_server *server, double lx, double ly) {
    xd_output *out = xd_output_at(server, lx, ly);
    if (out == nullptr)
        return (struct wlr_box){0, 0, 800, 600};
    if (!wlr_box_empty(&out->usable))
        return out->usable;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
    return box;
}

void xd_output_init(xd_server *server) {
    wl_list_init(&server->outputs);
    server->output_layout = wlr_output_layout_create(server->display);
    XD_LISTEN(&server->output_layout->events.change, server, layout_change,
              handle_layout_change);
    XD_LISTEN(&server->backend->events.new_output, server, new_output, handle_new_output);
}
