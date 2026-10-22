/* SPDX-License-Identifier: MIT
 * XP's "Animate windows when minimizing and maximizing": a bar in the
 * active caption's colour flies between a window's title bar and its
 * taskbar button (minimize, restore) or between its two title bars
 * (maximize, restore down), as DrawAnimatedRects does.
 */
#include <time.h>

#include "../core/alloc.h"
#include "server.h"

static constexpr int64_t DURATION_NS = 200 * 1000000LL;
static constexpr int FRAME_MS = 15;

typedef struct xd_zoom {
    struct wl_list link; /* xd_server.zooms */
    xd_server *server;
    struct wlr_scene_rect *rect;
    struct wlr_box from, to;
    int64_t start;
    struct wl_event_source *timer;
    xd_view *reveal; /* shown when the bar arrives (restore), or nullptr */
} xd_zoom;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int lerp(int from, int to, double progress) {
    double value = from + (to - from) * progress;
    return (int)(value < 0 ? value - 0.5 : value + 0.5);
}

static void zoom_free(xd_zoom *zoom, bool show) {
    if (show && zoom->reveal != nullptr && zoom->reveal->mapped && !zoom->reveal->minimized)
        wlr_scene_node_set_enabled(&zoom->reveal->tree->node, true);
    wl_list_remove(&zoom->link);
    wl_event_source_remove(zoom->timer);
    wlr_scene_node_destroy(&zoom->rect->node);
    xd_free(zoom);
}

static int zoom_tick(void *data) {
    xd_zoom *zoom = data;
    double progress = (double)(now_ns() - zoom->start) / (double)DURATION_NS;
    if (progress >= 1.0) {
        zoom_free(zoom, true);
        return 0;
    }
    struct wlr_box box = {lerp(zoom->from.x, zoom->to.x, progress),
                          lerp(zoom->from.y, zoom->to.y, progress),
                          lerp(zoom->from.width, zoom->to.width, progress),
                          lerp(zoom->from.height, zoom->to.height, progress)};
    wlr_scene_node_set_position(&zoom->rect->node, box.x, box.y);
    wlr_scene_rect_set_size(zoom->rect, XD_MAX(box.width, 1), XD_MAX(box.height, 1));
    wl_event_source_timer_update(zoom->timer, FRAME_MS);
    return 0;
}

void xd_animate_zoom(xd_server *server, struct wlr_box from, struct wlr_box to,
                     xd_view *reveal) {
    if (!server->config.animate_windows || from.width <= 0 || to.width <= 0) {
        return;
    }
    /* A window being restored again cancels its earlier flight. */
    if (reveal != nullptr)
        xd_animate_forget_view(server, reveal);
    xd_zoom *zoom = XD_NEW(xd_zoom);
    zoom->server = server;
    zoom->from = from;
    zoom->to = to;
    zoom->start = now_ns();
    zoom->reveal = reveal;
    xd_rgb title_color = server->config.scheme.colors[XD_COLOR_ACTIVE_TITLE];
    const float color[4] = {(float)title_color.r / 255.0f, (float)title_color.g / 255.0f,
                            (float)title_color.b / 255.0f, 1.0f};
    zoom->rect = wlr_scene_rect_create(server->layer_drag, XD_MAX(from.width, 1),
                                       XD_MAX(from.height, 1), color);
    wlr_scene_node_set_position(&zoom->rect->node, from.x, from.y);
    if (reveal != nullptr)
        wlr_scene_node_set_enabled(&reveal->tree->node, false);
    zoom->timer =
        wl_event_loop_add_timer(wl_display_get_event_loop(server->display), zoom_tick, zoom);
    wl_event_source_timer_update(zoom->timer, FRAME_MS);
    wl_list_insert(&server->zooms, &zoom->link);
}

void xd_animate_forget_view(xd_server *server, xd_view *view) {
    xd_zoom *zoom, *tmp;
    wl_list_for_each_safe(zoom, tmp, &server->zooms, link) {
        if (zoom->reveal == view) {
            zoom->reveal = nullptr;
            zoom_free(zoom, false);
        }
    }
}

void xd_animate_finish(xd_server *server) {
    xd_zoom *zoom, *tmp;
    wl_list_for_each_safe(zoom, tmp, &server->zooms, link) zoom_free(zoom, false);
}
