/* SPDX-License-Identifier: MIT
 * Touchpad gestures (pointer-gestures): swipes, pinches and holds go to
 * the program under the pointer, for pinch to zoom in a browser or an
 * image viewer, swipes back and forward, holds that stop kinetic
 * scrolling.
 *
 * Swipes of three fingers or more are the desktop's, as on Windows'
 * precision touchpads (and before them the touchpad drivers' own), unless
 * [Touchpad] Gestures is off: down shows the desktop, up brings back the
 * windows it hid, sideways switches windows through Alt+Tab's window
 * (each stretch of the swipe a step, the window chosen when the fingers
 * lift). Two-finger swipes and pinches stay the programs'.
 *
 * A gesture that starts during a move or size, or with nothing under the
 * pointer, goes nowhere until it ends; one that started goes on to its
 * end, so a program never sees half a gesture.
 */
#include <math.h>

#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>

#include "../core/alloc.h"
#include "server.h"

/* Swipe distances, in libinput's units (about a pixel of pointer
 * motion): how far before a desktop swipe has a direction, and how far
 * each step through the windows is. */
static constexpr double SWIPE_DECIDE = 60;
static constexpr double SWIPE_STEP = 120;

typedef enum desktop_swipe : uint8_t {
    DESKTOP_SWIPE_NONE, /* not the desktop's */
    DESKTOP_SWIPE_PENDING, /* the desktop's, its direction not yet known */
    DESKTOP_SWIPE_UP,
    DESKTOP_SWIPE_DOWN,
    DESKTOP_SWIPE_WINDOWS, /* sideways: Alt+Tab */
} desktop_swipe;

struct xd_gestures {
    xd_server *server;
    struct wlr_pointer_gestures_v1 *manager;
    bool swiping, pinching, holding; /* begun for a program */
    desktop_swipe desktop;
    double swipe_x, swipe_y; /* how far the desktop's swipe has gone */
    double stepped_x; /* where its last step through the windows was */
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

/* ---- the desktop's swipes ---------------------------------------------------------- */

static bool desktop_takes(const xd_gestures *gestures, uint32_t fingers) {
    const xd_server *server = gestures->server;
    return fingers >= 3 && server->config.mouse.touchpad.gestures && !server->locked &&
           server->mode == XD_CURSOR_PASSTHROUGH;
}

/* Down: the windows showing are minimized, remembered to come back.
 * Marked first, then minimized: minimizing moves a window to the end of
 * the list. */
static void hide_windows(xd_server *server) {
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        view->swiped_away = view->mapped && !view->minimized;
    }
    bool more = true;
    while (more) {
        more = false;
        XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
            if (view->swiped_away && !view->minimized && view->mapped) {
                xd_view_minimize(view);
                more = true;
                break;
            }
        }
    }
}

/* Up: the windows the last swipe down hid, the first of them in front. */
static void restore_windows(xd_server *server) {
    xd_view *front = nullptr;
    XD_LIST_FOR_EACH_SAFE(view, tmp, &server->views, xd_view, link) {
        if (!view->swiped_away)
            continue;
        view->swiped_away = false;
        if (view->mapped && view->minimized) {
            xd_view_restore(view);
            if (front == nullptr)
                front = view;
        }
    }
    if (front != nullptr)
        xd_view_focus(front);
}

static void desktop_swipe_update(xd_gestures *gestures, double dx, double dy) {
    gestures->swipe_x += dx;
    gestures->swipe_y += dy;
    if (gestures->desktop == DESKTOP_SWIPE_PENDING &&
        hypot(gestures->swipe_x, gestures->swipe_y) >= SWIPE_DECIDE) {
        if (fabs(gestures->swipe_x) > fabs(gestures->swipe_y)) {
            gestures->desktop = DESKTOP_SWIPE_WINDOWS;
            gestures->stepped_x = 0;
        } else {
            gestures->desktop = gestures->swipe_y > 0 ? DESKTOP_SWIPE_DOWN : DESKTOP_SWIPE_UP;
        }
    }
    /* Locked meanwhile: no Alt+Tab window (it's drawn above the lock). */
    if (gestures->desktop != DESKTOP_SWIPE_WINDOWS || gestures->server->locked)
        return;
    /* A step for each stretch, either way, the first at once. */
    if (gestures->stepped_x == 0 && gestures->server->cycle_view == nullptr) {
        gestures->stepped_x = gestures->swipe_x;
        xd_input_cycle_step(gestures->server, gestures->swipe_x < 0);
        return;
    }
    while (fabs(gestures->swipe_x - gestures->stepped_x) >= SWIPE_STEP) {
        bool back = gestures->swipe_x < gestures->stepped_x;
        gestures->stepped_x += back ? -SWIPE_STEP : SWIPE_STEP;
        xd_input_cycle_step(gestures->server, back);
    }
}

static void desktop_swipe_end(xd_gestures *gestures, bool cancelled) {
    xd_server *server = gestures->server;
    desktop_swipe swipe = gestures->desktop;
    gestures->desktop = DESKTOP_SWIPE_NONE;
    if (swipe == DESKTOP_SWIPE_WINDOWS) {
        if (cancelled || server->locked)
            xd_input_cycle_cancel(server);
        else
            xd_input_cycle_finish(server);
        return;
    }
    if (cancelled || server->locked)
        return;
    if (swipe == DESKTOP_SWIPE_DOWN)
        hide_windows(server);
    else if (swipe == DESKTOP_SWIPE_UP)
        restore_windows(server);
}

/* ---- the programs' ------------------------------------------------------------------ */

static void handle_swipe_begin(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, swipe_begin);
    const struct wlr_pointer_swipe_begin_event *pointer_swipe_begin_event = data;
    xd_idle_activity(gestures->server);
    if (desktop_takes(gestures, pointer_swipe_begin_event->fingers)) {
        gestures->desktop = DESKTOP_SWIPE_PENDING;
        gestures->swipe_x = gestures->swipe_y = 0;
        gestures->swiping = false;
        return;
    }
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
    if (gestures->desktop != DESKTOP_SWIPE_NONE)
        desktop_swipe_update(gestures, pointer_swipe_update_event->dx,
                             pointer_swipe_update_event->dy);
    else if (gestures->swiping)
        wlr_pointer_gestures_v1_send_swipe_update(
            gestures->manager, gestures->server->seat, pointer_swipe_update_event->time_msec,
            pointer_swipe_update_event->dx, pointer_swipe_update_event->dy);
}

static void handle_swipe_end(struct wl_listener *listener, void *data) {
    xd_gestures *gestures = XD_CONTAINER_OF(listener, xd_gestures, swipe_end);
    const struct wlr_pointer_swipe_end_event *pointer_swipe_end_event = data;
    if (gestures->desktop != DESKTOP_SWIPE_NONE)
        desktop_swipe_end(gestures, pointer_swipe_end_event->cancelled);
    else if (gestures->swiping)
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
