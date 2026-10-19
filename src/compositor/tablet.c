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
#include <wlr/backend/libinput.h>
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

static void pad_set_focus(pad *p, struct wlr_surface *surface);

static void handle_pad_focus_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    pad *p = XD_CONTAINER_OF(listener, pad, focus_destroy);
    xd_unlisten(&p->focus_destroy);
    p->focus = nullptr;
}

static void pad_set_focus(pad *p, struct wlr_surface *surface) {
    if (p->focus == surface)
        return;
    if (p->focus != nullptr) {
        wlr_tablet_v2_tablet_pad_notify_leave(p->v2, p->focus);
        xd_unlisten(&p->focus_destroy);
        p->focus = nullptr;
    }
    if (surface == nullptr || p->tablet == nullptr ||
        !wlr_surface_accepts_tablet_v2(surface, p->tablet->v2))
        return;
    wlr_tablet_v2_tablet_pad_notify_enter(p->v2, p->tablet->v2, surface);
    p->focus = surface;
    XD_LISTEN(&surface->events.destroy, p, focus_destroy, handle_pad_focus_destroy);
}

/* The pen went to a surface of a program that reads tablets: its tablet's
 * pads go there too. */
static void pads_follow(xd_tablets *ts, const tablet *t, struct wlr_surface *surface) {
    XD_LIST_FOR_EACH(p, &ts->pads, pad, link) {
        if (p->tablet == t)
            pad_set_focus(p, surface);
    }
}

/* A pad is on the same physical tablet as libinput groups them; devices
 * not from libinput (a nested session's, the tests') go with the newest
 * tablet. */
static bool same_device(struct wlr_input_device *a, struct wlr_input_device *b) {
    if (!wlr_input_device_is_libinput(a) || !wlr_input_device_is_libinput(b))
        return true;
    return libinput_device_get_device_group(wlr_libinput_get_device_handle(a)) ==
           libinput_device_get_device_group(wlr_libinput_get_device_handle(b));
}

static void pad_find_tablet(xd_tablets *ts, pad *p) {
    XD_LIST_FOR_EACH(t, &ts->tablets, tablet, link) {
        if (same_device(&t->wlr_tablet->base, &p->wlr_pad->base)) {
            p->tablet = t;
            return;
        }
    }
}

static void handle_pad_button(struct wl_listener *listener, void *data) {
    pad *p = XD_CONTAINER_OF(listener, pad, button);
    const struct wlr_tablet_pad_button_event *e = data;
    xd_idle_activity(p->owner->server);
    /* A mode switch button changes the group's mode first. */
    if (e->group < XD_ARRAY_LEN(p->modes) && p->modes[e->group] != e->mode) {
        p->modes[e->group] = e->mode;
        wlr_tablet_v2_tablet_pad_notify_mode(p->v2, e->group, e->mode, e->time_msec);
    }
    wlr_tablet_v2_tablet_pad_notify_button(p->v2, e->button, e->time_msec,
                                           e->state == WLR_BUTTON_PRESSED
                                               ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                               : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
}

static void handle_pad_ring(struct wl_listener *listener, void *data) {
    pad *p = XD_CONTAINER_OF(listener, pad, ring);
    const struct wlr_tablet_pad_ring_event *e = data;
    xd_idle_activity(p->owner->server);
    wlr_tablet_v2_tablet_pad_notify_ring(p->v2, e->ring, e->position,
                                         e->source == WLR_TABLET_PAD_RING_SOURCE_FINGER,
                                         e->time_msec);
}

static void handle_pad_strip(struct wl_listener *listener, void *data) {
    pad *p = XD_CONTAINER_OF(listener, pad, strip);
    const struct wlr_tablet_pad_strip_event *e = data;
    xd_idle_activity(p->owner->server);
    wlr_tablet_v2_tablet_pad_notify_strip(p->v2, e->strip, e->position,
                                          e->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER,
                                          e->time_msec);
}

static void pad_free(pad *p) {
    xd_unlisten(&p->focus_destroy);
    xd_unlisten(&p->button);
    xd_unlisten(&p->ring);
    xd_unlisten(&p->strip);
    xd_unlisten(&p->destroy);
    wl_list_remove(&p->link);
    xd_free(p);
}

static void handle_pad_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    pad_free(XD_CONTAINER_OF(listener, pad, destroy));
}

void xd_tablet_add_pad(xd_server *server, struct wlr_input_device *device) {
    xd_tablets *ts = server->tablets;
    pad *p = XD_NEW(pad);
    p->owner = ts;
    p->wlr_pad = wlr_tablet_pad_from_input_device(device);
    p->v2 = wlr_tablet_pad_create(ts->manager, server->seat, device);
    XD_LISTEN(&p->wlr_pad->events.button, p, button, handle_pad_button);
    XD_LISTEN(&p->wlr_pad->events.ring, p, ring, handle_pad_ring);
    XD_LISTEN(&p->wlr_pad->events.strip, p, strip, handle_pad_strip);
    XD_LISTEN(&device->events.destroy, p, destroy, handle_pad_destroy);
    wl_list_insert(&ts->pads, &p->link);
    pad_find_tablet(ts, p);
    wlr_log(WLR_INFO, "tablet pad '%s': %zu buttons, %zu rings, %zu strips",
            device->name != nullptr ? device->name : "?", p->wlr_pad->button_count,
            p->wlr_pad->ring_count, p->wlr_pad->strip_count);
}

/* ---- tablets ------------------------------------------------------------------- */

static void handle_tablet_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    tablet *t = XD_CONTAINER_OF(listener, tablet, destroy);
    xd_tablets *ts = t->owner;
    XD_LIST_FOR_EACH(p, &ts->pads, pad, link) {
        if (p->tablet == t) {
            pad_set_focus(p, nullptr);
            p->tablet = nullptr;
        }
    }
    XD_LIST_FOR_EACH(o, &ts->tools, tool, link) {
        if (o->tablet == t)
            o->tablet = nullptr;
    }
    xd_unlisten(&t->destroy);
    wl_list_remove(&t->link);
    xd_free(t);
}

void xd_tablet_add(xd_server *server, struct wlr_input_device *device) {
    xd_tablets *ts = server->tablets;
    tablet *t = XD_NEW(tablet);
    t->owner = ts;
    t->wlr_tablet = wlr_tablet_from_input_device(device);
    t->v2 = wlr_tablet_create(ts->manager, server->seat, device);
    t->wlr_tablet->data = t;
    XD_LISTEN(&device->events.destroy, t, destroy, handle_tablet_destroy);
    wl_list_insert(&ts->tablets, &t->link);
    wlr_cursor_attach_input_device(server->cursor, device);
    /* A pen display (a tablet PC's screen, a Cintiq) is on a monitor;
     * libinput offers a calibration only for those (INPUT_PROP_DIRECT).
     * Other tablets cover the whole desktop, as Windows' tablet drivers
     * map them by default. */
    bool screen = wlr_input_device_is_libinput(device) &&
                  libinput_device_config_calibration_has_matrix(
                      wlr_libinput_get_device_handle(device));
    xd_input_map_device(server, device, nullptr, screen);
    XD_LIST_FOR_EACH(p, &ts->pads, pad, link) {
        if (p->tablet == nullptr && same_device(device, &p->wlr_pad->base))
            p->tablet = t;
    }
    wlr_log(WLR_INFO, "drawing tablet '%s'%s", device->name != nullptr ? device->name : "?",
            screen ? " (on a screen)" : "");
}

/* ---- tools --------------------------------------------------------------------- */

static void tool_free(tool *o) {
    xd_unlisten(&o->tool_destroy);
    xd_unlisten(&o->set_cursor);
    if (o->wlr_tool->data == o)
        o->wlr_tool->data = nullptr;
    wl_list_remove(&o->link);
    xd_free(o);
}

static void handle_tool_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    tool_free(XD_CONTAINER_OF(listener, tool, tool_destroy));
}

/* The program under a tablet-v2 pen sets its cursor. */
static void handle_set_cursor(struct wl_listener *listener, void *data) {
    tool *o = XD_CONTAINER_OF(listener, tool, set_cursor);
    const struct wlr_tablet_v2_event_cursor *e = data;
    xd_server *server = o->owner->server;
    struct wlr_surface *focus = o->v2->focused_surface;
    if (o->emulating || focus == nullptr || e->seat_client == nullptr ||
        e->seat_client->client != wl_resource_get_client(focus->resource) ||
        server->mode != XD_CURSOR_PASSTHROUGH)
        return;
    server->cursor_name = nullptr;
    wlr_cursor_set_surface(server->cursor, e->surface, e->hotspot_x, e->hotspot_y);
}

static tool *get_tool(xd_tablets *ts, struct wlr_tablet_tool *wlr_tool) {
    if (wlr_tool->data != nullptr)
        return wlr_tool->data;
    tool *o = XD_NEW(tool);
    o->owner = ts;
    o->wlr_tool = wlr_tool;
    o->v2 = wlr_tablet_tool_create(ts->manager, ts->server->seat, wlr_tool);
    wlr_tool->data = o;
    XD_LISTEN(&wlr_tool->events.destroy, o, tool_destroy, handle_tool_destroy);
    XD_LISTEN(&o->v2->events.set_cursor, o, set_cursor, handle_set_cursor);
    wl_list_insert(&ts->tools, &o->link);
    return o;
}

static bool pressed_any(const tool *o) {
    return o->tip_down || o->pointer_buttons != 0 || o->v2->num_buttons > 0;
}

/* Moves the cursor to the pen: absolute pens by where they are on the
 * tablet's area, a tablet's mouse and lens by how far they moved. */
static void move_cursor(xd_server *server, tool *o, struct wlr_tablet *wt, uint32_t axes,
                        double x, double y, double dx, double dy) {
    if (o->wlr_tool->type == WLR_TABLET_TOOL_TYPE_MOUSE ||
        o->wlr_tool->type == WLR_TABLET_TOOL_TYPE_LENS) {
        wlr_cursor_move(server->cursor, &wt->base, dx, dy);
        return;
    }
    wlr_cursor_warp_absolute(server->cursor, &wt->base, (axes & WLR_TABLET_TOOL_AXIS_X) ? x : (double)NAN,
                             (axes & WLR_TABLET_TOOL_AXIS_Y) ? y : (double)NAN);
}

/* The pen is at the cursor: it goes to what's under it as a pen, or
 * works the pointer there. */
static void tool_at_cursor(tool *o, uint32_t time_msec) {
    xd_server *server = o->owner->server;
    double lx = server->cursor->x, ly = server->cursor->y;
    if (o->emulating && pressed_any(o)) {
        xd_input_warp_pointer(server, lx, ly, time_msec);
        return;
    }
    if (!o->emulating && wlr_tablet_tool_v2_has_implicit_grab(o->v2)) {
        /* A stroke stays with its surface, wherever the pen goes. */
        wlr_tablet_v2_tablet_tool_notify_motion(o->v2, lx - o->off_x, ly - o->off_y);
        return;
    }
    xd_input_target target = xd_input_target_at(server, lx, ly);
    if (o->tablet != nullptr && target.surface != nullptr &&
        server->mode == XD_CURSOR_PASSTHROUGH &&
        wlr_surface_accepts_tablet_v2(target.surface, o->tablet->v2)) {
        if (o->emulating || o->v2->focused_surface != target.surface) {
            /* The pen, not the mouse, is over it now. */
            wlr_seat_pointer_notify_clear_focus(server->seat);
            xd_cursor_set(server, "default");
        }
        o->emulating = false;
        o->off_x = lx - target.sx;
        o->off_y = ly - target.sy;
        wlr_tablet_v2_tablet_tool_notify_proximity_in(o->v2, o->tablet->v2, target.surface);
        wlr_tablet_v2_tablet_tool_notify_motion(o->v2, target.sx, target.sy);
        pads_follow(o->owner, o->tablet, target.surface);
        return;
    }
    if (!o->emulating && o->v2->focused_surface != nullptr)
        wlr_tablet_v2_tablet_tool_notify_proximity_out(o->v2);
    o->emulating = true;
    xd_input_warp_pointer(server, lx, ly, time_msec);
}

static void handle_proximity(struct wl_listener *listener, void *data) {
    xd_tablets *ts = XD_CONTAINER_OF(listener, xd_tablets, proximity);
    xd_server *server = ts->server;
    const struct wlr_tablet_tool_proximity_event *e = data;
    xd_idle_activity(server);
    tool *o = get_tool(ts, e->tool);
    o->tablet = e->tablet->data;
    if (e->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
        move_cursor(server, o, e->tablet, WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y, e->x,
                    e->y, 0, 0);
        o->emulating = false;
        tool_at_cursor(o, e->time_msec);
        return;
    }
    /* Out of reach: whatever the pen held is let go. */
    if (o->emulating) {
        for (uint32_t b = 0; b < 32; b++) {
            if (o->pointer_buttons & (1u << b))
                xd_input_button(server, e->time_msec, BTN_LEFT + b,
                                WL_POINTER_BUTTON_STATE_RELEASED);
        }
    } else {
        wlr_tablet_v2_tablet_tool_notify_proximity_out(o->v2);
    }
    o->pointer_buttons = 0;
    o->tip_down = false;
    o->emulating = false;
    /* The mouse has the pointer again where the pen left it. */
    xd_input_warp_pointer(server, server->cursor->x, server->cursor->y, e->time_msec);
}

static void handle_axis(struct wl_listener *listener, void *data) {
    xd_tablets *ts = XD_CONTAINER_OF(listener, xd_tablets, axis);
    xd_server *server = ts->server;
    const struct wlr_tablet_tool_axis_event *e = data;
    xd_idle_activity(server);
    tool *o = get_tool(ts, e->tool);
    o->tablet = e->tablet->data;
    if (e->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y)) {
        move_cursor(server, o, e->tablet, e->updated_axes, e->x, e->y, e->dx, e->dy);
        tool_at_cursor(o, e->time_msec);
    }
    if (o->emulating || o->v2->focused_surface == nullptr)
        return; /* a mouse has no pressure */
    struct wlr_tablet_v2_tablet_tool *v2 = o->v2;
    if (e->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE)
        wlr_tablet_v2_tablet_tool_notify_pressure(v2, e->pressure);
    if (e->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE)
        wlr_tablet_v2_tablet_tool_notify_distance(v2, e->distance);
    if (e->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
        wlr_tablet_v2_tablet_tool_notify_tilt(v2, e->tilt_x, e->tilt_y);
    if (e->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION)
        wlr_tablet_v2_tablet_tool_notify_rotation(v2, e->rotation);
    if (e->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER)
        wlr_tablet_v2_tablet_tool_notify_slider(v2, e->slider);
    if (e->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL)
        wlr_tablet_v2_tablet_tool_notify_wheel(v2, e->wheel_delta,
                                              (int32_t)lround(e->wheel_delta / 15.0));
}

/* A pen touching a window activates it, as a click does. */
static void focus_under(xd_server *server) {
    if (server->locked)
        return;
    xd_input_target t = xd_input_target_at(server, server->cursor->x, server->cursor->y);
    if (t.view != nullptr)
        xd_view_focus(t.view);
    else if (t.layer != nullptr)
        xd_layer_click_focus(t.layer);
}

static void emulated_button(tool *o, uint32_t time_msec, uint32_t button, bool pressed) {
    uint32_t bit = 1u << (button - BTN_LEFT);
    if (pressed == ((o->pointer_buttons & bit) != 0))
        return;
    o->pointer_buttons = pressed ? o->pointer_buttons | bit : o->pointer_buttons & ~bit;
    xd_input_button(o->owner->server, time_msec, button,
                    pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
}

static void handle_tip(struct wl_listener *listener, void *data) {
    xd_tablets *ts = XD_CONTAINER_OF(listener, xd_tablets, tip);
    xd_server *server = ts->server;
    const struct wlr_tablet_tool_tip_event *e = data;
    xd_idle_activity(server);
    tool *o = get_tool(ts, e->tool);
    bool down = e->state == WLR_TABLET_TOOL_TIP_DOWN;
    if (down == o->tip_down)
        return;
    o->tip_down = down;
    if (o->emulating || (down && o->v2->focused_surface == nullptr)) {
        o->emulating = true;
        if (down)
            xd_input_warp_pointer(server, server->cursor->x, server->cursor->y, e->time_msec);
        emulated_button(o, e->time_msec, BTN_LEFT, down);
    } else if (down) {
        if (!wlr_seat_pointer_has_grab(server->seat))
            focus_under(server);
        wlr_tablet_v2_tablet_tool_notify_down(o->v2);
        wlr_tablet_tool_v2_start_implicit_grab(o->v2);
    } else {
        wlr_tablet_v2_tablet_tool_notify_up(o->v2);
    }
    /* A stroke over: the pen may be somewhere else now. */
    if (!down && !pressed_any(o))
        tool_at_cursor(o, e->time_msec);
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
    const struct wlr_tablet_tool_button_event *e = data;
    xd_idle_activity(server);
    tool *o = get_tool(ts, e->tool);
    bool pressed = e->state == WLR_BUTTON_PRESSED;
    if (o->emulating || o->v2->focused_surface == nullptr) {
        uint32_t b = pointer_button_for(e->button);
        if (b == 0)
            return;
        o->emulating = true;
        emulated_button(o, e->time_msec, b, pressed);
    } else {
        wlr_tablet_v2_tablet_tool_notify_button(o->v2, e->button,
                                                pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                                        : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
        if (pressed)
            wlr_tablet_tool_v2_start_implicit_grab(o->v2);
    }
    if (!pressed && !pressed_any(o))
        tool_at_cursor(o, e->time_msec);
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
    XD_LIST_FOR_EACH_SAFE(p, ptmp, &ts->pads, pad, link) {
        pad_free(p);
    }
    XD_LIST_FOR_EACH_SAFE(o, otmp, &ts->tools, tool, link) {
        tool_free(o);
    }
    XD_LIST_FOR_EACH_SAFE(t, ttmp, &ts->tablets, tablet, link) {
        t->wlr_tablet->data = nullptr;
        xd_unlisten(&t->destroy);
        wl_list_remove(&t->link);
        xd_free(t);
    }
    xd_free(ts);
    server->tablets = nullptr;
}
