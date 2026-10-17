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

static int lerp(int a, int b, double t) {
    double v = a + (b - a) * t;
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

static void zoom_free(xd_zoom *z, bool show) {
    if (show && z->reveal != nullptr && z->reveal->mapped && !z->reveal->minimized)
        wlr_scene_node_set_enabled(&z->reveal->tree->node, true);
    wl_list_remove(&z->link);
    wl_event_source_remove(z->timer);
    wlr_scene_node_destroy(&z->rect->node);
    xd_free(z);
}

static int zoom_tick(void *data) {
    xd_zoom *z = data;
    double t = (double)(now_ns() - z->start) / (double)DURATION_NS;
    if (t >= 1.0) {
        zoom_free(z, true);
        return 0;
    }
    struct wlr_box b = {lerp(z->from.x, z->to.x, t), lerp(z->from.y, z->to.y, t),
                        lerp(z->from.width, z->to.width, t),
                        lerp(z->from.height, z->to.height, t)};
    wlr_scene_node_set_position(&z->rect->node, b.x, b.y);
    wlr_scene_rect_set_size(z->rect, XD_MAX(b.width, 1), XD_MAX(b.height, 1));
    wl_event_source_timer_update(z->timer, FRAME_MS);
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
    xd_zoom *z = XD_NEW(xd_zoom);
    z->server = server;
    z->from = from;
    z->to = to;
    z->start = now_ns();
    z->reveal = reveal;
    xd_rgb c = server->config.scheme.colors[XD_COLOR_ACTIVE_TITLE];
    const float color[4] = {(float)c.r / 255.0f, (float)c.g / 255.0f, (float)c.b / 255.0f, 1.0f};
    z->rect = wlr_scene_rect_create(server->layer_drag, XD_MAX(from.width, 1),
                                    XD_MAX(from.height, 1), color);
    wlr_scene_node_set_position(&z->rect->node, from.x, from.y);
    if (reveal != nullptr)
        wlr_scene_node_set_enabled(&reveal->tree->node, false);
    z->timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->display), zoom_tick, z);
    wl_event_source_timer_update(z->timer, FRAME_MS);
    wl_list_insert(&server->zooms, &z->link);
}

void xd_animate_forget_view(xd_server *server, xd_view *view) {
    xd_zoom *z, *tmp;
    wl_list_for_each_safe(z, tmp, &server->zooms, link) {
        if (z->reveal == view) {
            z->reveal = nullptr;
            zoom_free(z, false);
        }
    }
}

void xd_animate_finish(xd_server *server) {
    xd_zoom *z, *tmp;
    wl_list_for_each_safe(z, tmp, &server->zooms, link)
        zoom_free(z, false);
}
