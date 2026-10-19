/* SPDX-License-Identifier: MIT
 * Touchpad gestures (pointer-gestures): swipes, pinches and holds go to
 * the program under the pointer, for pinch to zoom in a browser or an
 * image viewer, swipes back and forward, holds that stop kinetic
 * scrolling. The compositor keeps none for itself: Windows XP and 7 had
 * no touchpad gestures of their own (the touchpad's driver had some).
 *
 * A gesture that starts during a move or size, or with nothing under the
 * pointer, goes nowhere until it ends; one that started goes on to its
 * end, so a program never sees half a gesture.
 */
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>

#include "../core/alloc.h"
#include "server.h"

struct xd_gestures {
    xd_server *server;
    struct wlr_pointer_gestures_v1 *manager;
    bool swiping, pinching, holding; /* begun for a program */
    struct wl_listener swipe_begin;
    struct wl_listener swipe_update;
    struct wl_listener swipe_end;
    struct wl_listener pinch_begin;
    struct wl_listener pinch_update;
    struct wl_listener pinch_end;
    struct wl_listener hold_begin;
    struct wl_listener hold_end;
};

static bool may_begin(const xd_gestures *g) {
    const xd_server *server = g->server;
    return server->mode == XD_CURSOR_PASSTHROUGH &&
           server->seat->pointer_state.focused_surface != nullptr;
}

static void handle_swipe_begin(struct wl_listener *listener, void *data) {
    xd_gestures *g = XD_CONTAINER_OF(listener, xd_gestures, swipe_begin);
    const struct wlr_pointer_swipe_begin_event *e = data;
    xd_idle_activity(g->server);
    g->swiping = may_begin(g);
    if (g->swiping)
        wlr_pointer_gestures_v1_send_swipe_begin(g->manager, g->server->seat, e->time_msec,
                                                 e->fingers);
}

static void handle_swipe_update(struct wl_listener *listener, void *data) {
    xd_gestures *g = XD_CONTAINER_OF(listener, xd_gestures, swipe_update);
    const struct wlr_pointer_swipe_update_event *e = data;
    xd_idle_activity(g->server);
    if (g->swiping)
        wlr_pointer_gestures_v1_send_swipe_update(g->manager, g->server->seat, e->time_msec,
                                                  e->dx, e->dy);
}

static void handle_swipe_end(struct wl_listener *listener, void *data) {
    xd_gestures *g = XD_CONTAINER_OF(listener, xd_gestures, swipe_end);
    const struct wlr_pointer_swipe_end_event *e = data;
    if (g->swiping)
        wlr_pointer_gestures_v1_send_swipe_end(g->manager, g->server->seat, e->time_msec,
                                               e->cancelled);
    g->swiping = false;
}

static void handle_pinch_begin(struct wl_listener *listener, void *data) {
    xd_gestures *g = XD_CONTAINER_OF(listener, xd_gestures, pinch_begin);
    const struct wlr_pointer_pinch_begin_event *e = data;
    xd_idle_activity(g->server);
    g->pinching = may_begin(g);
    if (g->pinching)
        wlr_pointer_gestures_v1_send_pinch_begin(g->manager, g->server->seat, e->time_msec,
                                                 e->fingers);
}

static void handle_pinch_update(struct wl_listener *listener, void *data) {
    xd_gestures *g = XD_CONTAINER_OF(listener, xd_gestures, pinch_update);
    const struct wlr_pointer_pinch_update_event *e = data;
    xd_idle_activity(g->server);
    if (g->pinching)
        wlr_pointer_gestures_v1_send_pinch_update(g->manager, g->server->seat, e->time_msec,
                                                  e->dx, e->dy, e->scale, e->rotation);
}

static void handle_pinch_end(struct wl_listener *listener, void *data) {
    xd_gestures *g = XD_CONTAINER_OF(listener, xd_gestures, pinch_end);
    const struct wlr_pointer_pinch_end_event *e = data;
    if (g->pinching)
        wlr_pointer_gestures_v1_send_pinch_end(g->manager, g->server->seat, e->time_msec,
                                               e->cancelled);
    g->pinching = false;
}

static void handle_hold_begin(struct wl_listener *listener, void *data) {
    xd_gestures *g = XD_CONTAINER_OF(listener, xd_gestures, hold_begin);
    const struct wlr_pointer_hold_begin_event *e = data;
    xd_idle_activity(g->server);
    g->holding = may_begin(g);
    if (g->holding)
        wlr_pointer_gestures_v1_send_hold_begin(g->manager, g->server->seat, e->time_msec,
                                                e->fingers);
}

static void handle_hold_end(struct wl_listener *listener, void *data) {
    xd_gestures *g = XD_CONTAINER_OF(listener, xd_gestures, hold_end);
    const struct wlr_pointer_hold_end_event *e = data;
    if (g->holding)
        wlr_pointer_gestures_v1_send_hold_end(g->manager, g->server->seat, e->time_msec,
                                              e->cancelled);
    g->holding = false;
}

void xd_gestures_init(xd_server *server) {
    xd_gestures *g = XD_NEW(xd_gestures);
    g->server = server;
    g->manager = wlr_pointer_gestures_v1_create(server->display);
    server->gestures = g;
    struct wlr_cursor *c = server->cursor;
    XD_LISTEN(&c->events.swipe_begin, g, swipe_begin, handle_swipe_begin);
    XD_LISTEN(&c->events.swipe_update, g, swipe_update, handle_swipe_update);
    XD_LISTEN(&c->events.swipe_end, g, swipe_end, handle_swipe_end);
    XD_LISTEN(&c->events.pinch_begin, g, pinch_begin, handle_pinch_begin);
    XD_LISTEN(&c->events.pinch_update, g, pinch_update, handle_pinch_update);
    XD_LISTEN(&c->events.pinch_end, g, pinch_end, handle_pinch_end);
    XD_LISTEN(&c->events.hold_begin, g, hold_begin, handle_hold_begin);
    XD_LISTEN(&c->events.hold_end, g, hold_end, handle_hold_end);
}

void xd_gestures_finish(xd_server *server) {
    xd_gestures *g = server->gestures;
    if (g == nullptr)
        return;
    struct wl_listener *listeners[] = {
        &g->swipe_begin, &g->swipe_update, &g->swipe_end, &g->pinch_begin,
        &g->pinch_update, &g->pinch_end, &g->hold_begin, &g->hold_end,
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(listeners); i++)
        xd_unlisten(listeners[i]);
    xd_free(g);
    server->gestures = nullptr;
}
