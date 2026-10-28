/* SPDX-License-Identifier: MIT
 * Outputs (monitors): mode and scale selection, frame scheduling, desktop
 * background, and work areas.
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/backend/drm.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "../settings/monitors.h"
#include "effects/effects.h"
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
    xd_view_rescue_offscreen(server);
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        xd_output_arrange(out); /* also refits maximized windows */
    }
    /* Windows may now sit on an output with a different scale. */
    xd_view_update_all_frames(server);
    xd_lock_layout_changed(server);
    xd_output_config_changed(server);
}

static void handle_frame(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_output *out = XD_CONTAINER_OF(listener, xd_output, frame);
    struct wlr_scene_output *so = wlr_scene_get_scene_output(out->server->scene, out->wlr_output);
    if (so == nullptr)
        return;
    /* A frame, in this order (effects/effects.h):
     * 1. the effects update what the scene shows: their frame hooks
     *    (whole-output effects draw their picture of the output into a
     *    scene node from captures of the scene; Zoom, last, draws the
     *    scene again magnified), then the OpenGL window effects;
     * 2. the scene graph draws the output, and output filters (Water)
     *    each draw it again from the one before;
     * 3. our gamma ramp (gamma.c) joins the same commit when it changed,
     *    a colour transform the display applies to whatever was drawn,
     *    and adaptive sync and tearing (present.c). */
    xd_effects_output_frame(out);
    if (wlr_scene_output_needs_frame(so) || xd_gamma_pending(out) || xd_present_pending(out) ||
        xd_effects_is_output_filtered(out)) {
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        if (xd_effects_build_output_state(out, so, &state)) {
            xd_gamma_frame(out, &state);
            xd_present_frame(out, &state);
            if (wlr_output_commit_state(out->wlr_output, &state))
                xd_lock_output_shown(out);
        }
        wlr_output_state_finish(&state);
    }
    xd_effects_output_frame_done(out);
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
    xd_gamma_output_removed(out);
    xd_unlisten(&out->frame);
    xd_unlisten(&out->request_state);
    xd_unlisten(&out->destroy);
    if (out->background != nullptr)
        wlr_scene_node_destroy(&out->background->node);
    out->background = nullptr;
    xd_server *server = out->server;
    /* Removing it from the layout emits the layout's change, whose handler
     * walks server->outputs: this one must be gone from there first. */
    wl_list_remove(&out->link);
    wlr_output_layout_remove(server->output_layout, out->wlr_output);
    xd_free(out);
    /* The others close up from 0,0. */
    xd_output_place_all(server);
    xd_output_config_changed(server);
}

void xd_output_key(const struct wlr_output *output, char *out, size_t cap) {
    xd_monitor_key(out, cap, output->make, output->model, output->serial, output->name);
}

/* The mode of that size, the requested refresh rate if there's one, else
 * the fastest. */
static struct wlr_output_mode *find_mode(struct wlr_output *output, int width, int height,
                                         int refresh) {
    struct wlr_output_mode *best = nullptr;
    struct wlr_output_mode *output_mode;
    wl_list_for_each(output_mode, &output->modes, link) {
        if (output_mode->width != width || output_mode->height != height)
            continue;
        if (refresh > 0 && abs(output_mode->refresh - refresh) < 500)
            return output_mode;
        if (best == nullptr || output_mode->refresh > best->refresh)
            best = output_mode;
    }
    return best;
}

static bool other_output_enabled(xd_server *server, const struct wlr_output *self) {
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (out->wlr_output != self && out->wlr_output->enabled)
            return true;
    }
    return false;
}

/* Fills st from the remembered settings (or defaults); false if the output
 * stays off. */
static bool initial_state(xd_server *server, struct wlr_output *output,
                          const xd_monitor_config *cfg, struct wlr_output_state *st) {
    bool enable = cfg == nullptr || cfg->enabled || !other_output_enabled(server, output);
    wlr_output_state_set_enabled(st, enable);
    if (!enable)
        return false;
    struct wlr_output_mode *mode = nullptr;
    int width = output->width;
    if (cfg != nullptr && cfg->width > 0) {
        mode = find_mode(output, cfg->width, cfg->height, cfg->refresh_mhz);
        if (mode == nullptr && wl_list_empty(&output->modes)) {
            /* Nested and headless outputs take any size. */
            wlr_output_state_set_custom_mode(st, cfg->width, cfg->height, cfg->refresh_mhz);
            width = cfg->width;
        }
    }
    if (mode == nullptr && width == output->width)
        mode = wlr_output_preferred_mode(output);
    if (mode != nullptr) {
        wlr_output_state_set_mode(st, mode);
        width = mode->width;
    }
    double scale = server->config.forced_scale > 0    ? server->config.forced_scale
                   : cfg != nullptr && cfg->scale > 0 ? cfg->scale
                                                      : (double)auto_scale(output, width);
    wlr_output_state_set_scale(st, (float)scale);
    wlr_output_state_set_transform(
        st, cfg != nullptr ? (enum wl_output_transform)cfg->transform : WL_OUTPUT_TRANSFORM_NORMAL);
    return true;
}

static void handle_new_output(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_output);
    struct wlr_output *wlr_output = data;

    if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
        wlr_log(WLR_ERROR, "cannot init rendering on output %s", wlr_output->name);
        return;
    }
    if (server->software_cursors && wlr_output_is_drm(wlr_output))
        wlr_output_lock_software_cursors(wlr_output, true);

    char key[192];
    xd_output_key(wlr_output, key, sizeof(key));
    xd_monitor_config cfg;
    bool remembered = xd_monitors_lookup(key, &cfg);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    bool enabled = initial_state(server, wlr_output, remembered ? &cfg : nullptr, &state);
    bool ok = wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
    if (!ok && remembered) {
        wlr_log(WLR_ERROR, "output %s: remembered settings failed, using defaults",
                wlr_output->name);
        remembered = false;
        wlr_output_state_init(&state);
        enabled = initial_state(server, wlr_output, nullptr, &state);
        ok = wlr_output_commit_state(wlr_output, &state);
        wlr_output_state_finish(&state);
    }
    if (!ok) {
        wlr_log(WLR_ERROR, "cannot enable output %s", wlr_output->name);
        return;
    }
    wlr_log(WLR_INFO, "output %s (%s): %dx%d, scale %.2f%s", wlr_output->name, key,
            wlr_output->width, wlr_output->height, (double)wlr_output->scale,
            !enabled ? ", off" : remembered ? ", remembered" : "");

    xd_output *out = XD_NEW(xd_output);
    out->server = server;
    out->wlr_output = wlr_output;
    wl_list_init(&out->layer_surfaces);
    XD_LISTEN(&wlr_output->events.frame, out, frame, handle_frame);
    XD_LISTEN(&wlr_output->events.request_state, out, request_state, handle_request_state);
    XD_LISTEN(&wlr_output->events.destroy, out, destroy, handle_output_destroy);
    wl_list_insert(server->outputs.prev, &out->link);
    xd_gamma_output_added(out);

    struct wlr_scene_output *so = wlr_scene_output_create(server->scene, wlr_output);
    if (remembered) {
        out->has_position = true;
        out->want_x = cfg.x;
        out->want_y = cfg.y;
        out->vrr = cfg.vrr; /* present.c turns it on when it's wanted */
    }
    if (enabled) {
        struct wlr_output_layout_output *lo =
            wlr_output_layout_add_auto(server->output_layout, wlr_output);
        wlr_scene_output_layout_add_output(server->scene_layout, lo, so);
        xd_output_place_all(server);
    }
    xd_output_update_backgrounds(server);
    xd_output_arrange(out);
}

void xd_output_place_all(xd_server *server) {
    /* The remembered arrangement, minus monitors that aren't here, moved
     * so the desktop starts at 0,0: a missing left-hand monitor mustn't
     * leave everything offset. */
    int minx = INT32_MAX;
    int miny = INT32_MAX;
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (!out->has_position || !wlr_output_layout_get(server->output_layout, out->wlr_output))
            continue;
        minx = XD_MIN(minx, out->want_x);
        miny = XD_MIN(miny, out->want_y);
    }
    if (minx == INT32_MAX)
        return;
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (out->has_position && wlr_output_layout_get(server->output_layout, out->wlr_output))
            wlr_output_layout_add(server->output_layout, out->wlr_output, out->want_x - minx,
                                  out->want_y - miny);
    }
}

void xd_output_save_config(xd_server *server) {
    size_t count = (size_t)wl_list_length(&server->outputs);
    if (count == 0)
        return;
    xd_monitor_config *cfgs = xd_alloc_array(count, sizeof(*cfgs));
    size_t i = 0;
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        struct wlr_output *output = out->wlr_output;
        xd_monitor_config *monitor_config = &cfgs[i++];
        *monitor_config = (xd_monitor_config){
            .enabled = output->enabled,
            .width = output->width,
            .height = output->height,
            .refresh_mhz = output->refresh,
            .scale = (double)output->scale,
            .transform = (int)output->transform,
            .vrr = out->vrr,
        };
        xd_output_key(output, monitor_config->key, sizeof(monitor_config->key));
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, output, &box);
        if (!wlr_box_empty(&box)) {
            monitor_config->x = box.x;
            monitor_config->y = box.y;
        }
    }
    xd_monitors_store(cfgs, count);
    xd_free(cfgs);
}

xd_output *xd_output_from_wlr(xd_server *server, struct wlr_output *wlr_output) {
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        if (out->wlr_output == wlr_output)
            return out;
    }
    return nullptr;
}

static struct wlr_output *output_at(xd_server *server, double lx, double ly) {
    struct wlr_output *output = wlr_output_layout_output_at(server->output_layout, lx, ly);
    if (output == nullptr)
        output = wlr_output_layout_get_center_output(server->output_layout);
    return output;
}

double xd_output_scale_at(xd_server *server, double lx, double ly) {
    struct wlr_output *output = output_at(server, lx, ly);
    return output != nullptr && output->scale > 0 ? (double)output->scale : 1.0;
}

xd_output *xd_output_at(xd_server *server, double lx, double ly) {
    struct wlr_output *output = output_at(server, lx, ly);
    return output != nullptr ? xd_output_from_wlr(server, output) : nullptr;
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
