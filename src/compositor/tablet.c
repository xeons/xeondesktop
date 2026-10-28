/* SPDX-License-Identifier: MIT
 * Drawing tablets (tablet-v2): pens, with their pressure, tilt and
 * buttons, and the tablets' pads (buttons, rings, strips).
 *
 * A program that reads tablets on our seat (GTK, Krita, Inkscape, GIMP)
 * gets the pen as a pen. Everywhere else the pen works the pointer, as a
 * pen did in Windows for programs that knew nothing of it: the tip is the
 * left button, the barrel button the right one (the second, the middle),
 * so a pen still clicks our frames' buttons, drags captions, opens the
 * Start menu. Which way a pen goes is decided as it hovers and holds
 * while the tip or a button is down, so a stroke never changes halfway.
 *
 * The pad follows the pen: it's focused on the surface the pen last went
 * to, when that program reads tablets. Pads of programs that don't do
 * nothing (Windows had its tablet driver turn them into keys).
 *
 * The pen moves the cursor wherever it goes, so it's seen; a tablet
 * program sets the cursor it wants for the pen.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <time.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

typedef struct tablet {
    struct wl_list link; /* xd_tablets.tablets */
    xd_tablets *owner;
    struct wlr_tablet *wlr_tablet;
    struct wlr_tablet_v2_tablet *v2;
    struct wl_listener destroy;
} tablet;

typedef struct tool {
    struct wl_list link; /* xd_tablets.tools */
    xd_tablets *owner;
    struct wlr_tablet_tool *wlr_tool;
    struct wlr_tablet_v2_tablet_tool *v2;
    tablet *tablet; /* the one it was last used on */
    bool emulating; /* working the pointer, not tablet-v2 */
    bool tip_down;
    uint32_t pointer_buttons; /* emulated buttons held (BTN_* bits from BTN_LEFT) */
    double off_x, off_y; /* tablet-v2: its surface's origin, for strokes */
    struct wl_listener tool_destroy;
    struct wl_listener set_cursor;
} tool;

typedef struct pad {
    struct wl_list link; /* xd_tablets.pads */
    xd_tablets *owner;
    struct wlr_tablet_pad *wlr_pad;
    struct wlr_tablet_v2_tablet_pad *v2;
    tablet *tablet;
    struct wlr_surface *focus;
    unsigned int modes[8]; /* each group's mode, as last told */
    struct wl_listener focus_destroy;
    struct wl_listener button;
    struct wl_listener ring;
    struct wl_listener strip;
    struct wl_listener destroy;
} pad;

struct xd_tablets {
    xd_server *server;
    struct wlr_tablet_manager_v2 *manager;
    struct wl_list tablets; /* tablet */
    struct wl_list tools;   /* tool */
    struct wl_list pads;    /* pad */
    struct wl_listener axis;
    struct wl_listener proximity;
    struct wl_listener tip;
    struct wl_listener button;
};

/* ---- pads ---------------------------------------------------------------------- */

static void pad_set_focus(pad *tablet_pad, struct wlr_surface *surface);

static void handle_pad_focus_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    pad *tablet_pad = XD_CONTAINER_OF(listener, pad, focus_destroy);
    xd_unlisten(&tablet_pad->focus_destroy);
    tablet_pad->focus = nullptr;
}

static void pad_set_focus(pad *tablet_pad, struct wlr_surface *surface) {
    if (tablet_pad->focus == surface)
        return;
    if (tablet_pad->focus != nullptr) {
        wlr_tablet_v2_tablet_pad_notify_leave(tablet_pad->v2, tablet_pad->focus);
        xd_unlisten(&tablet_pad->focus_destroy);
        tablet_pad->focus = nullptr;
    }
    if (surface == nullptr || tablet_pad->tablet == nullptr ||
        !wlr_surface_accepts_tablet_v2(surface, tablet_pad->tablet->v2))
        return;
    wlr_tablet_v2_tablet_pad_notify_enter(tablet_pad->v2, tablet_pad->tablet->v2, surface);
    tablet_pad->focus = surface;
    XD_LISTEN(&surface->events.destroy, tablet_pad, focus_destroy, handle_pad_focus_destroy);
}

/* The pen went to a surface of a program that reads tablets: its tablet's
 * pads go there too. */
static void pads_follow(xd_tablets *ts, const tablet *tablet_device, struct wlr_surface *surface) {
    XD_LIST_FOR_EACH(tablet_pad, &ts->pads, pad, link) {
        if (tablet_pad->tablet == tablet_device)
            pad_set_focus(tablet_pad, surface);
    }
}

/* A pad is on the same physical tablet as libinput groups them; devices
 * not from libinput (a nested session's, the tests') go with the newest
 * tablet. */
static bool same_device(struct wlr_input_device *first_device,
                        struct wlr_input_device *second_device) {
    if (!wlr_input_device_is_libinput(first_device) || !wlr_input_device_is_libinput(second_device))
        return true;
    return libinput_device_get_device_group(wlr_libinput_get_device_handle(first_device)) ==
           libinput_device_get_device_group(wlr_libinput_get_device_handle(second_device));
}

static void pad_find_tablet(xd_tablets *ts, pad *tablet_pad) {
    XD_LIST_FOR_EACH(tablet_device, &ts->tablets, tablet, link) {
        if (same_device(&tablet_device->wlr_tablet->base, &tablet_pad->wlr_pad->base)) {
            tablet_pad->tablet = tablet_device;
            return;
        }
    }
}

/* While the session is locked the pad is the locker's alone, as the
 * keyboard is: a program behind the lock gets nothing from it. */
static bool is_pad_blocked(const pad *tablet_pad) {
    return tablet_pad->owner->server->locked &&
           (tablet_pad->focus == nullptr ||
            wlr_session_lock_surface_v1_try_from_wlr_surface(tablet_pad->focus) == nullptr);
}

void xd_tablet_lock_changed(xd_server *server) {
    xd_tablets *ts = server->tablets;
    if (ts == nullptr || !server->locked)
        return;
    XD_LIST_FOR_EACH(tablet_pad, &ts->pads, pad, link) {
        if (is_pad_blocked(tablet_pad))
            pad_set_focus(tablet_pad, nullptr);
    }
}

static void handle_pad_button(struct wl_listener *listener, void *data) {
    pad *tablet_pad = XD_CONTAINER_OF(listener, pad, button);
    const struct wlr_tablet_pad_button_event *tablet_pad_button_event = data;
    xd_idle_activity(tablet_pad->owner->server);
    if (is_pad_blocked(tablet_pad))
        return;
    /* A mode switch button changes the group's mode first. */
    if (tablet_pad_button_event->group < XD_ARRAY_LEN(tablet_pad->modes) &&
        tablet_pad->modes[tablet_pad_button_event->group] != tablet_pad_button_event->mode) {
        tablet_pad->modes[tablet_pad_button_event->group] = tablet_pad_button_event->mode;
        wlr_tablet_v2_tablet_pad_notify_mode(tablet_pad->v2, tablet_pad_button_event->group,
                                             tablet_pad_button_event->mode,
                                             tablet_pad_button_event->time_msec);
    }
    wlr_tablet_v2_tablet_pad_notify_button(tablet_pad->v2, tablet_pad_button_event->button,
                                           tablet_pad_button_event->time_msec,
                                           tablet_pad_button_event->state == WLR_BUTTON_PRESSED
                                               ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                               : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
}

static void handle_pad_ring(struct wl_listener *listener, void *data) {
    pad *tablet_pad = XD_CONTAINER_OF(listener, pad, ring);
    const struct wlr_tablet_pad_ring_event *tablet_pad_ring_event = data;
    xd_idle_activity(tablet_pad->owner->server);
    if (is_pad_blocked(tablet_pad))
        return;
    wlr_tablet_v2_tablet_pad_notify_ring(
        tablet_pad->v2, tablet_pad_ring_event->ring, tablet_pad_ring_event->position,
        tablet_pad_ring_event->source == WLR_TABLET_PAD_RING_SOURCE_FINGER,
        tablet_pad_ring_event->time_msec);
}

static void handle_pad_strip(struct wl_listener *listener, void *data) {
    pad *tablet_pad = XD_CONTAINER_OF(listener, pad, strip);
    const struct wlr_tablet_pad_strip_event *tablet_pad_strip_event = data;
    xd_idle_activity(tablet_pad->owner->server);
    if (is_pad_blocked(tablet_pad))
        return;
    wlr_tablet_v2_tablet_pad_notify_strip(
        tablet_pad->v2, tablet_pad_strip_event->strip, tablet_pad_strip_event->position,
        tablet_pad_strip_event->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER,
        tablet_pad_strip_event->time_msec);
}

static void pad_free(pad *tablet_pad) {
    xd_unlisten(&tablet_pad->focus_destroy);
    xd_unlisten(&tablet_pad->button);
    xd_unlisten(&tablet_pad->ring);
    xd_unlisten(&tablet_pad->strip);
    xd_unlisten(&tablet_pad->destroy);
    wl_list_remove(&tablet_pad->link);
    xd_free(tablet_pad);
}

static void handle_pad_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    pad_free(XD_CONTAINER_OF(listener, pad, destroy));
}

void xd_tablet_add_pad(xd_server *server, struct wlr_input_device *device) {
    xd_tablets *ts = server->tablets;
    pad *tablet_pad = XD_NEW(pad);
    tablet_pad->owner = ts;
    tablet_pad->wlr_pad = wlr_tablet_pad_from_input_device(device);
    tablet_pad->v2 = wlr_tablet_pad_create(ts->manager, server->seat, device);
    XD_LISTEN(&tablet_pad->wlr_pad->events.button, tablet_pad, button, handle_pad_button);
    XD_LISTEN(&tablet_pad->wlr_pad->events.ring, tablet_pad, ring, handle_pad_ring);
    XD_LISTEN(&tablet_pad->wlr_pad->events.strip, tablet_pad, strip, handle_pad_strip);
    XD_LISTEN(&device->events.destroy, tablet_pad, destroy, handle_pad_destroy);
    wl_list_insert(&ts->pads, &tablet_pad->link);
    pad_find_tablet(ts, tablet_pad);
    wlr_log(WLR_INFO, "tablet pad '%s': %zu buttons, %zu rings, %zu strips",
            device->name != nullptr ? device->name : "?", tablet_pad->wlr_pad->button_count,
            tablet_pad->wlr_pad->ring_count, tablet_pad->wlr_pad->strip_count);
}

/* ---- the pointer buttons a pen holds ------------------------------------------ */

static uint32_t now_msec(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}

/* The pen is gone (out of reach, or unplugged with its tablet): whatever
 * it held as the pointer is let go, else a drag or a move it started
 * would never end. */
static void release_pointer_buttons(tool *tablet_tool, uint32_t time_msec) {
    uint32_t held = tablet_tool->pointer_buttons;
    tablet_tool->pointer_buttons = 0;
    if (!tablet_tool->emulating)
        return;
    for (uint32_t bit = 0; bit < 32; bit++) {
        if (held & (1u << bit))
            xd_input_button(tablet_tool->owner->server, time_msec, BTN_LEFT + bit,
                            WL_POINTER_BUTTON_STATE_RELEASED);
    }
}

/* ---- tablets ------------------------------------------------------------------- */

static void handle_tablet_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    tablet *tablet_device = XD_CONTAINER_OF(listener, tablet, destroy);
    xd_tablets *ts = tablet_device->owner;
    /* Its pens go with it. */
    XD_LIST_FOR_EACH(tablet_tool, &ts->tools, tool, link) {
        if (tablet_tool->tablet == tablet_device) {
            release_pointer_buttons(tablet_tool, now_msec());
            tablet_tool->tip_down = false;
        }
    }
    XD_LIST_FOR_EACH(tablet_pad, &ts->pads, pad, link) {
        if (tablet_pad->tablet == tablet_device) {
            pad_set_focus(tablet_pad, nullptr);
            tablet_pad->tablet = nullptr;
        }
    }
    XD_LIST_FOR_EACH(tablet_tool, &ts->tools, tool, link) {
        if (tablet_tool->tablet == tablet_device)
            tablet_tool->tablet = nullptr;
    }
    xd_unlisten(&tablet_device->destroy);
    wl_list_remove(&tablet_device->link);
    xd_free(tablet_device);
}

void xd_tablet_add(xd_server *server, struct wlr_input_device *device) {
    xd_tablets *ts = server->tablets;
    tablet *tablet_device = XD_NEW(tablet);
    tablet_device->owner = ts;
    tablet_device->wlr_tablet = wlr_tablet_from_input_device(device);
    tablet_device->v2 = wlr_tablet_create(ts->manager, server->seat, device);
    tablet_device->wlr_tablet->data = tablet_device;
    XD_LISTEN(&device->events.destroy, tablet_device, destroy, handle_tablet_destroy);
    wl_list_insert(&ts->tablets, &tablet_device->link);
    wlr_cursor_attach_input_device(server->cursor, device);
    /* A pen display (a tablet PC's screen, a Cintiq) is on a monitor;
     * libinput offers a calibration only for those (INPUT_PROP_DIRECT).
     * Other tablets cover the whole desktop, as Windows' tablet drivers
     * map them by default. */
    bool screen = wlr_input_device_is_libinput(device) &&
                  libinput_device_config_calibration_has_matrix(
                      wlr_libinput_get_device_handle(device));
    xd_input_map_device(server, device, nullptr, screen);
    XD_LIST_FOR_EACH(tablet_pad, &ts->pads, pad, link) {
        if (tablet_pad->tablet == nullptr && same_device(device, &tablet_pad->wlr_pad->base))
            tablet_pad->tablet = tablet_device;
    }
    wlr_log(WLR_INFO, "drawing tablet '%s'%s", device->name != nullptr ? device->name : "?",
            screen ? " (on a screen)" : "");
}

/* ---- tools --------------------------------------------------------------------- */

static void tool_free(tool *tablet_tool) {
    xd_unlisten(&tablet_tool->tool_destroy);
    xd_unlisten(&tablet_tool->set_cursor);
    if (tablet_tool->wlr_tool->data == tablet_tool)
        tablet_tool->wlr_tool->data = nullptr;
    wl_list_remove(&tablet_tool->link);
    xd_free(tablet_tool);
}

static void handle_tool_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    tool *tablet_tool = XD_CONTAINER_OF(listener, tool, tool_destroy);
    release_pointer_buttons(tablet_tool, now_msec());
    tool_free(tablet_tool);
}

/* The program under a tablet-v2 pen sets its cursor. */
static void handle_set_cursor(struct wl_listener *listener, void *data) {
    tool *tablet_tool = XD_CONTAINER_OF(listener, tool, set_cursor);
    const struct wlr_tablet_v2_event_cursor *tablet_v2_event_cursor = data;
    xd_server *server = tablet_tool->owner->server;
    struct wlr_surface *focus = tablet_tool->v2->focused_surface;
    if (tablet_tool->emulating || focus == nullptr ||
        tablet_v2_event_cursor->seat_client == nullptr ||
        tablet_v2_event_cursor->seat_client->client != wl_resource_get_client(focus->resource) ||
        server->mode != XD_CURSOR_PASSTHROUGH)
        return;
    server->cursor_name = nullptr;
    wlr_cursor_set_surface(server->cursor, tablet_v2_event_cursor->surface,
                           tablet_v2_event_cursor->hotspot_x, tablet_v2_event_cursor->hotspot_y);
}

static tool *get_tool(xd_tablets *ts, struct wlr_tablet_tool *wlr_tool) {
    if (wlr_tool->data != nullptr)
        return wlr_tool->data;
    tool *tablet_tool = XD_NEW(tool);
    tablet_tool->owner = ts;
    tablet_tool->wlr_tool = wlr_tool;
    tablet_tool->v2 = wlr_tablet_tool_create(ts->manager, ts->server->seat, wlr_tool);
    wlr_tool->data = tablet_tool;
    XD_LISTEN(&wlr_tool->events.destroy, tablet_tool, tool_destroy, handle_tool_destroy);
    XD_LISTEN(&tablet_tool->v2->events.set_cursor, tablet_tool, set_cursor, handle_set_cursor);
    wl_list_insert(&ts->tools, &tablet_tool->link);
    return tablet_tool;
}

static bool pressed_any(const tool *tablet_tool) {
    return tablet_tool->tip_down || tablet_tool->pointer_buttons != 0 ||
           tablet_tool->v2->num_buttons > 0;
}

/* Moves the cursor to the pen: absolute pens by where they are on the
 * tablet's area, a tablet's mouse and lens by how far they moved. */
static void move_cursor(xd_server *server, tool *tablet_tool, struct wlr_tablet *wt, uint32_t axes,
                        double x, double y, double dx, double dy) {
    if (tablet_tool->wlr_tool->type == WLR_TABLET_TOOL_TYPE_MOUSE ||
        tablet_tool->wlr_tool->type == WLR_TABLET_TOOL_TYPE_LENS) {
        wlr_cursor_move(server->cursor, &wt->base, dx, dy);
        return;
    }
    wlr_cursor_warp_absolute(server->cursor, &wt->base, (axes & WLR_TABLET_TOOL_AXIS_X) ? x : (double)NAN,
                             (axes & WLR_TABLET_TOOL_AXIS_Y) ? y : (double)NAN);
}

/* The pen is at the cursor: it goes to what's under it as a pen, or
 * works the pointer there. */
static void tool_at_cursor(tool *tablet_tool, uint32_t time_msec) {
    xd_server *server = tablet_tool->owner->server;
    double lx = server->cursor->x, ly = server->cursor->y;
    if (tablet_tool->emulating && pressed_any(tablet_tool)) {
        xd_input_warp_pointer(server, lx, ly, time_msec);
        return;
    }
    if (!tablet_tool->emulating && wlr_tablet_tool_v2_has_implicit_grab(tablet_tool->v2)) {
        /* A stroke stays with its surface, wherever the pen goes. */
        wlr_tablet_v2_tablet_tool_notify_motion(tablet_tool->v2, lx - tablet_tool->off_x,
                                                ly - tablet_tool->off_y);
        return;
    }
    xd_input_target target = xd_input_target_at(server, lx, ly);
    if (tablet_tool->tablet != nullptr && target.surface != nullptr &&
        server->mode == XD_CURSOR_PASSTHROUGH &&
        wlr_surface_accepts_tablet_v2(target.surface, tablet_tool->tablet->v2)) {
        if (tablet_tool->emulating || tablet_tool->v2->focused_surface != target.surface) {
            /* The pen, not the mouse, is over it now. */
            wlr_seat_pointer_notify_clear_focus(server->seat);
            xd_cursor_set(server, "default");
        }
        tablet_tool->emulating = false;
        tablet_tool->off_x = lx - target.sx;
        tablet_tool->off_y = ly - target.sy;
        wlr_tablet_v2_tablet_tool_notify_proximity_in(tablet_tool->v2, tablet_tool->tablet->v2,
                                                      target.surface);
        wlr_tablet_v2_tablet_tool_notify_motion(tablet_tool->v2, target.sx, target.sy);
        pads_follow(tablet_tool->owner, tablet_tool->tablet, target.surface);
        return;
    }
    if (!tablet_tool->emulating && tablet_tool->v2->focused_surface != nullptr)
        wlr_tablet_v2_tablet_tool_notify_proximity_out(tablet_tool->v2);
    tablet_tool->emulating = true;
    xd_input_warp_pointer(server, lx, ly, time_msec);
}

static void handle_proximity(struct wl_listener *listener, void *data) {
    xd_tablets *ts = XD_CONTAINER_OF(listener, xd_tablets, proximity);
    xd_server *server = ts->server;
    const struct wlr_tablet_tool_proximity_event *tablet_tool_proximity_event = data;
    xd_idle_activity(server);
    tool *tablet_tool = get_tool(ts, tablet_tool_proximity_event->tool);
    tablet_tool->tablet = tablet_tool_proximity_event->tablet->data;
    if (tablet_tool_proximity_event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
        move_cursor(server, tablet_tool, tablet_tool_proximity_event->tablet,
                    WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y, tablet_tool_proximity_event->x,
                    tablet_tool_proximity_event->y, 0, 0);
        tablet_tool->emulating = false;
        tool_at_cursor(tablet_tool, tablet_tool_proximity_event->time_msec);
        return;
    }
    /* Out of reach: whatever the pen held is let go. */
    if (!tablet_tool->emulating)
        wlr_tablet_v2_tablet_tool_notify_proximity_out(tablet_tool->v2);
    release_pointer_buttons(tablet_tool, tablet_tool_proximity_event->time_msec);
    tablet_tool->tip_down = false;
    tablet_tool->emulating = false;
    /* The mouse has the pointer again where the pen left it. */
    xd_input_warp_pointer(server, server->cursor->x, server->cursor->y,
                          tablet_tool_proximity_event->time_msec);
}

static void handle_axis(struct wl_listener *listener, void *data) {
    xd_tablets *ts = XD_CONTAINER_OF(listener, xd_tablets, axis);
    xd_server *server = ts->server;
    const struct wlr_tablet_tool_axis_event *tablet_tool_axis_event = data;
    xd_idle_activity(server);
    tool *tablet_tool = get_tool(ts, tablet_tool_axis_event->tool);
    tablet_tool->tablet = tablet_tool_axis_event->tablet->data;
    if (tablet_tool_axis_event->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y)) {
        move_cursor(server, tablet_tool, tablet_tool_axis_event->tablet,
                    tablet_tool_axis_event->updated_axes, tablet_tool_axis_event->x,
                    tablet_tool_axis_event->y, tablet_tool_axis_event->dx,
                    tablet_tool_axis_event->dy);
        tool_at_cursor(tablet_tool, tablet_tool_axis_event->time_msec);
    }
    if (tablet_tool->emulating || tablet_tool->v2->focused_surface == nullptr)
        return; /* a mouse has no pressure */
    struct wlr_tablet_v2_tablet_tool *v2 = tablet_tool->v2;
    if (tablet_tool_axis_event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE)
        wlr_tablet_v2_tablet_tool_notify_pressure(v2, tablet_tool_axis_event->pressure);
    if (tablet_tool_axis_event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE)
        wlr_tablet_v2_tablet_tool_notify_distance(v2, tablet_tool_axis_event->distance);
    if (tablet_tool_axis_event->updated_axes &
        (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
        wlr_tablet_v2_tablet_tool_notify_tilt(v2, tablet_tool_axis_event->tilt_x,
                                              tablet_tool_axis_event->tilt_y);
    if (tablet_tool_axis_event->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION)
        wlr_tablet_v2_tablet_tool_notify_rotation(v2, tablet_tool_axis_event->rotation);
    if (tablet_tool_axis_event->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER)
        wlr_tablet_v2_tablet_tool_notify_slider(v2, tablet_tool_axis_event->slider);
    if (tablet_tool_axis_event->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL)
        wlr_tablet_v2_tablet_tool_notify_wheel(
            v2, tablet_tool_axis_event->wheel_delta,
            (int32_t)lround(tablet_tool_axis_event->wheel_delta / 15.0));
}

/* A pen touching a window activates it, as a click does. */
static void focus_under(xd_server *server) {
    if (server->locked)
        return;
    xd_input_target input_target = xd_input_target_at(server, server->cursor->x, server->cursor->y);
    if (input_target.view != nullptr)
        xd_view_focus(input_target.view);
    else if (input_target.layer != nullptr)
        xd_layer_click_focus(input_target.layer);
}

static void emulated_button(tool *tablet_tool, uint32_t time_msec, uint32_t button, bool pressed) {
    uint32_t bit = 1u << (button - BTN_LEFT);
    if (pressed == ((tablet_tool->pointer_buttons & bit) != 0))
        return;
    tablet_tool->pointer_buttons =
        pressed ? tablet_tool->pointer_buttons | bit : tablet_tool->pointer_buttons & ~bit;
    xd_input_button(tablet_tool->owner->server, time_msec, button,
                    pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
}

static void handle_tip(struct wl_listener *listener, void *data) {
    xd_tablets *ts = XD_CONTAINER_OF(listener, xd_tablets, tip);
    xd_server *server = ts->server;
    const struct wlr_tablet_tool_tip_event *tablet_tool_tip_event = data;
    xd_idle_activity(server);
    tool *tablet_tool = get_tool(ts, tablet_tool_tip_event->tool);
    bool down = tablet_tool_tip_event->state == WLR_TABLET_TOOL_TIP_DOWN;
    if (down == tablet_tool->tip_down)
        return;
    tablet_tool->tip_down = down;
    if (tablet_tool->emulating || (down && tablet_tool->v2->focused_surface == nullptr)) {
        tablet_tool->emulating = true;
        if (down)
            xd_input_warp_pointer(server, server->cursor->x, server->cursor->y,
                                  tablet_tool_tip_event->time_msec);
        emulated_button(tablet_tool, tablet_tool_tip_event->time_msec, BTN_LEFT, down);
    } else if (down) {
        if (!wlr_seat_pointer_has_grab(server->seat))
            focus_under(server);
        wlr_tablet_v2_tablet_tool_notify_down(tablet_tool->v2);
        wlr_tablet_tool_v2_start_implicit_grab(tablet_tool->v2);
    } else {
        wlr_tablet_v2_tablet_tool_notify_up(tablet_tool->v2);
    }
    /* A stroke over: the pen may be somewhere else now. */
    if (!down && !pressed_any(tablet_tool))
        tool_at_cursor(tablet_tool, tablet_tool_tip_event->time_msec);
}

/* The barrel buttons as a mouse's: the first is the right button (as in
 * Windows), the second the middle one. */
static uint32_t pointer_button_for(uint32_t button) {
    switch (button) {
    case BTN_STYLUS:
        return BTN_RIGHT;
    case BTN_STYLUS2:
        return BTN_MIDDLE;
    default:
        return 0;
    }
}

static void handle_button(struct wl_listener *listener, void *data) {
    xd_tablets *ts = XD_CONTAINER_OF(listener, xd_tablets, button);
    xd_server *server = ts->server;
    const struct wlr_tablet_tool_button_event *tablet_tool_button_event = data;
    xd_idle_activity(server);
    tool *tablet_tool = get_tool(ts, tablet_tool_button_event->tool);
    bool pressed = tablet_tool_button_event->state == WLR_BUTTON_PRESSED;
    if (tablet_tool->emulating || tablet_tool->v2->focused_surface == nullptr) {
        uint32_t pointer_button = pointer_button_for(tablet_tool_button_event->button);
        if (pointer_button == 0)
            return;
        tablet_tool->emulating = true;
        emulated_button(tablet_tool, tablet_tool_button_event->time_msec, pointer_button, pressed);
    } else {
        wlr_tablet_v2_tablet_tool_notify_button(tablet_tool->v2, tablet_tool_button_event->button,
                                                pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                                        : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
        if (pressed)
            wlr_tablet_tool_v2_start_implicit_grab(tablet_tool->v2);
    }
    if (!pressed && !pressed_any(tablet_tool))
        tool_at_cursor(tablet_tool, tablet_tool_button_event->time_msec);
}

/* ---- setup ----------------------------------------------------------------------- */

void xd_tablet_init(xd_server *server) {
    xd_tablets *ts = XD_NEW(xd_tablets);
    ts->server = server;
    ts->manager = wlr_tablet_v2_create(server->display);
    wl_list_init(&ts->tablets);
    wl_list_init(&ts->tools);
    wl_list_init(&ts->pads);
    server->tablets = ts;
    XD_LISTEN(&server->cursor->events.tablet_tool_axis, ts, axis, handle_axis);
    XD_LISTEN(&server->cursor->events.tablet_tool_proximity, ts, proximity, handle_proximity);
    XD_LISTEN(&server->cursor->events.tablet_tool_tip, ts, tip, handle_tip);
    XD_LISTEN(&server->cursor->events.tablet_tool_button, ts, button, handle_button);
}

void xd_tablet_finish(xd_server *server) {
    xd_tablets *ts = server->tablets;
    if (ts == nullptr)
        return;
    xd_unlisten(&ts->axis);
    xd_unlisten(&ts->proximity);
    xd_unlisten(&ts->tip);
    xd_unlisten(&ts->button);
    XD_LIST_FOR_EACH_SAFE(tablet_pad, ptmp, &ts->pads, pad, link) { pad_free(tablet_pad); }
    XD_LIST_FOR_EACH_SAFE(tablet_tool, otmp, &ts->tools, tool, link) { tool_free(tablet_tool); }
    XD_LIST_FOR_EACH_SAFE(tablet_device, ttmp, &ts->tablets, tablet, link) {
        tablet_device->wlr_tablet->data = nullptr;
        xd_unlisten(&tablet_device->destroy);
        wl_list_remove(&tablet_device->link);
        xd_free(tablet_device);
    }
    xd_free(ts);
    server->tablets = nullptr;
}
