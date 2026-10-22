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

static bool may_begin(const xd_gestures *gestures) {
    const xd_server *server = gestures->server;
    return server->mode == XD_CURSOR_PASSTHROUGH &&
           server->seat->pointer_state.focused_surface != nullptr;
}

static void handle_swipe_begin(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, swipe_begin);
    const struct wlr_pointer_swipe_begin_event *pointer_swipe_begin_event = data;
    xd_idle_activity(gestures->server);
    gestures->swiping = may_begin(gestures);
    if (gestures->swiping)
        wlr_pointer_gestures_v1_send_swipe_begin(gestures->manager, gestures->server->seat,
                                                 pointer_swipe_begin_event->time_msec,
                                                 pointer_swipe_begin_event->fingers);
}

static void handle_swipe_update(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, swipe_update);
    const struct wlr_pointer_swipe_update_event *pointer_swipe_update_event = data;
    xd_idle_activity(gestures->server);
    if (gestures->swiping)
        wlr_pointer_gestures_v1_send_swipe_update(
            gestures->manager, gestures->server->seat, pointer_swipe_update_event->time_msec,
            pointer_swipe_update_event->dx, pointer_swipe_update_event->dy);
}

static void handle_swipe_end(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, swipe_end);
    const struct wlr_pointer_swipe_end_event *pointer_swipe_end_event = data;
    if (gestures->swiping)
        wlr_pointer_gestures_v1_send_swipe_end(gestures->manager, gestures->server->seat,
                                               pointer_swipe_end_event->time_msec,
                                               pointer_swipe_end_event->cancelled);
    gestures->swiping = false;
}

static void handle_pinch_begin(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, pinch_begin);
    const struct wlr_pointer_pinch_begin_event *pointer_pinch_begin_event = data;
    xd_idle_activity(gestures->server);
    gestures->pinching = may_begin(gestures);
    if (gestures->pinching)
        wlr_pointer_gestures_v1_send_pinch_begin(gestures->manager, gestures->server->seat,
                                                 pointer_pinch_begin_event->time_msec,
                                                 pointer_pinch_begin_event->fingers);
}

static void handle_pinch_update(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, pinch_update);
    const struct wlr_pointer_pinch_update_event *pointer_pinch_update_event = data;
    xd_idle_activity(gestures->server);
    if (gestures->pinching)
        wlr_pointer_gestures_v1_send_pinch_update(
            gestures->manager, gestures->server->seat, pointer_pinch_update_event->time_msec,
            pointer_pinch_update_event->dx, pointer_pinch_update_event->dy,
            pointer_pinch_update_event->scale, pointer_pinch_update_event->rotation);
}

static void handle_pinch_end(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, pinch_end);
    const struct wlr_pointer_pinch_end_event *pointer_pinch_end_event = data;
    if (gestures->pinching)
        wlr_pointer_gestures_v1_send_pinch_end(gestures->manager, gestures->server->seat,
                                               pointer_pinch_end_event->time_msec,
                                               pointer_pinch_end_event->cancelled);
    gestures->pinching = false;
}

static void handle_hold_begin(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, hold_begin);
    const struct wlr_pointer_hold_begin_event *pointer_hold_begin_event = data;
    xd_idle_activity(gestures->server);
    gestures->holding = may_begin(gestures);
    if (gestures->holding)
        wlr_pointer_gestures_v1_send_hold_begin(gestures->manager, gestures->server->seat,
                                                pointer_hold_begin_event->time_msec,
                                                pointer_hold_begin_event->fingers);
}

static void handle_hold_end(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, hold_end);
    const struct wlr_pointer_hold_end_event *pointer_hold_end_event = data;
    if (gestures->holding)
        wlr_pointer_gestures_v1_send_hold_end(gestures->manager, gestures->server->seat,
                                              pointer_hold_end_event->time_msec,
                                              pointer_hold_end_event->cancelled);
    gestures->holding = false;
}

void xd_gestures_init(xd_server *server) {
    xd_gestures *gestures = XD_NEW(xd_gestures);
    gestures->server = server;
    gestures->manager = wlr_pointer_gestures_v1_create(server->display);
    server->gestures = gestures;
    struct wlr_cursor *cursor = server->cursor;
    XD_LISTEN(&cursor->events.swipe_begin, gestures, swipe_begin, handle_swipe_begin);
    XD_LISTEN(&cursor->events.swipe_update, gestures, swipe_update, handle_swipe_update);
    XD_LISTEN(&cursor->events.swipe_end, gestures, swipe_end, handle_swipe_end);
    XD_LISTEN(&cursor->events.pinch_begin, gestures, pinch_begin, handle_pinch_begin);
    XD_LISTEN(&cursor->events.pinch_update, gestures, pinch_update, handle_pinch_update);
    XD_LISTEN(&cursor->events.pinch_end, gestures, pinch_end, handle_pinch_end);
    XD_LISTEN(&cursor->events.hold_begin, gestures, hold_begin, handle_hold_begin);
    XD_LISTEN(&cursor->events.hold_end, gestures, hold_end, handle_hold_end);
}

void xd_gestures_finish(xd_server *server) {
    xd_gestures *gestures = server->gestures;
    if (gestures == nullptr)
        return;
    struct wl_listener *listeners[] = {
        &gestures->swipe_begin, &gestures->swipe_update, &gestures->swipe_end,
        &gestures->pinch_begin, &gestures->pinch_update, &gestures->pinch_end,
        &gestures->hold_begin,  &gestures->hold_end,
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(listeners); i++)
        xd_unlisten(listeners[i]);
    xd_free(gestures);
    server->gestures = nullptr;
}
