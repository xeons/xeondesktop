/* SPDX-License-Identifier: MIT
 * Touchscreens, and which monitor a touchscreen or drawing tablet covers.
 *
 * A finger on a program that reads touch (wl_touch: GTK, Qt, Firefox,
 * Xwayland's programs) goes to it as touch. Everywhere else it works the
 * mouse, as Windows 7 promotes touch to mouse messages for programs that
 * don't ask for WM_TOUCH: our frames (a tap on a caption button, a drag of
 * the caption or a border, Aero Snap), the shell's taskbar and menus
 * (libxdui reads only the pointer) and the screen locker. That is one
 * finger at a time: the first to land on such a place holds the pointer
 * until it lifts, and others landing meanwhile are ignored. A program
 * that reads touch may still move or size its own window from a touch
 * (a GTK header bar dragged): that finger then drives the move.
 *
 * libinput applies a touchscreen's calibration (the udev
 * LIBINPUT_CALIBRATION_MATRIX) itself; wlr_cursor applies the monitor's
 * rotation.
 */
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

/* ---- which monitor a device covers ------------------------------------------- */

/* A touchscreen or drawing tablet, mapped to a monitor (or, for a tablet
 * that isn't a screen, the whole desktop). */
typedef struct mapped_device {
    struct wl_list link; /* xd_touch.mapped */
    xd_touch *owner;
    struct wlr_input_device *device;
    char *output_name; /* the device's own (udev's WL_OUTPUT), or nullptr */
    bool screen;       /* it's on a screen: a touchscreen or pen display */
    struct wlr_output *output; /* where it's mapped now, or nullptr */
    struct wl_listener destroy;
} mapped_device;

typedef enum point_kind : uint8_t {
    POINT_CLIENT,  /* the program it landed on reads touch */
    POINT_POINTER, /* it works the pointer */
    POINT_GRAB,    /* it moves or sizes a window the program asked to */
    POINT_IGNORED, /* another finger has the pointer */
} point_kind;

typedef struct touch_point {
    struct wl_list link; /* xd_touch.points */
    int32_t id;
    struct wlr_touch *touch; /* the device it's on */
    point_kind kind;
    double lx, ly;       /* where it is (layout coordinates) */
    double off_x, off_y; /* POINT_CLIENT: its surface's origin when it landed */
} touch_point;

typedef struct touch_device {
    struct wl_list link; /* xd_touch.devices */
    xd_touch *owner;
    struct wlr_input_device *device;
    struct wl_listener destroy;
} touch_device;

struct xd_touch {
    xd_server *server;
    struct wl_list devices; /* touch_device */
    struct wl_list points;  /* touch_point */
    struct wl_list mapped;  /* mapped_device */
    struct wl_listener down;
    struct wl_listener up;
    struct wl_listener motion;
    struct wl_listener cancel;
    struct wl_listener frame;
    struct wl_listener layout_change;
};

/* A laptop's or tablet's own panel, where its touchscreen and pen are. */
static bool is_builtin_panel(const char *name) {
    return strncmp(name, "eDP", 3) == 0 || strncmp(name, "LVDS", 4) == 0 ||
           strncmp(name, "DSI", 3) == 0;
}

/* The monitor a screen device belongs to: the one udev names for it, else
 * the built-in panel, else the first monitor. */
static struct wlr_output *screen_output(xd_server *server, const char *want) {
    struct wlr_output *builtin = nullptr, *first = nullptr;
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        struct wlr_output *o = out->wlr_output;
        if (wlr_output_layout_get(server->output_layout, o) == nullptr)
            continue; /* turned off */
        if (want != nullptr && strcmp(o->name, want) == 0)
            return o;
        if (builtin == nullptr && is_builtin_panel(o->name))
            builtin = o;
        if (first == nullptr)
            first = o;
    }
    return builtin != nullptr ? builtin : first;
}

static void apply_mapping(mapped_device *m) {
    xd_server *server = m->owner->server;
    struct wlr_output *o = m->screen ? screen_output(server, m->output_name) : nullptr;
    if (o != m->output)
        wlr_log(WLR_INFO, "'%s' mapped to %s", m->device->name != nullptr ? m->device->name : "?",
                o != nullptr ? o->name : "the whole desktop");
    m->output = o;
    wlr_cursor_map_input_to_output(server->cursor, m->device, o);
}

static void handle_mapped_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    mapped_device *m = XD_CONTAINER_OF(listener, mapped_device, destroy);
    xd_unlisten(&m->destroy);
    wl_list_remove(&m->link);
    xd_free(m->output_name);
    xd_free(m);
}

void xd_input_map_device(xd_server *server, struct wlr_input_device *device,
                         const char *output_name, bool screen) {
    mapped_device *m = XD_NEW(mapped_device);
    m->owner = server->touch;
    m->device = device;
    m->screen = screen;
    if (output_name != nullptr)
        m->output_name = xd_str_dup_cstr(xd_str_from_cstr(output_name));
    XD_LISTEN(&device->events.destroy, m, destroy, handle_mapped_destroy);
    wl_list_insert(&server->touch->mapped, &m->link);
    apply_mapping(m);
}

struct wlr_box xd_input_device_area(xd_server *server, const struct wlr_input_device *device) {
    struct wlr_box box = {0};
    XD_LIST_FOR_EACH(m, &server->touch->mapped, mapped_device, link) {
        if (m->device == device && m->output != nullptr) {
            wlr_output_layout_get_box(server->output_layout, m->output, &box);
            return box;
        }
    }
    wlr_output_layout_get_box(server->output_layout, nullptr, &box);
    return box;
}

/* A monitor came, went or moved: the mapping follows (wlr_cursor would
 * keep a monitor that's gone). */
static void handle_layout_change(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_touch *t = XD_CONTAINER_OF(listener, xd_touch, layout_change);
    XD_LIST_FOR_EACH(m, &t->mapped, mapped_device, link) {
        apply_mapping(m);
    }
}

/* ---- touch points --------------------------------------------------------------- */

static touch_point *find_point(xd_touch *t, int32_t id) {
    XD_LIST_FOR_EACH(p, &t->points, touch_point, link) {
        if (p->id == id)
            return p;
    }
    return nullptr;
}

static void drop_point(touch_point *p) {
    wl_list_remove(&p->link);
    xd_free(p);
}

static bool pointer_held(const xd_touch *t) {
    XD_LIST_FOR_EACH(p, &t->points, touch_point, link) {
        if (p->kind == POINT_POINTER || p->kind == POINT_GRAB)
            return true;
    }
    return false;
}

/* Whether the pointer is free for a finger: no mouse button held, no
 * move or size under way (the window menu's keyboard Move and Size end
 * with a tap, as with a click). */
static bool pointer_free(const xd_server *server) {
    return server->seat->pointer_state.button_count == 0 &&
           (server->mode == XD_CURSOR_PASSTHROUGH || server->mode == XD_CURSOR_KEY_MOVE ||
            server->mode == XD_CURSOR_KEY_SIZE);
}

static void touch_position(xd_server *server, struct wlr_touch *touch, double x, double y,
                           double *lx, double *ly) {
    wlr_cursor_absolute_to_layout_coords(server->cursor, &touch->base, x, y, lx, ly);
}

static void handle_down(struct wl_listener *listener, void *data) {
    xd_touch *t = XD_CONTAINER_OF(listener, xd_touch, down);
    xd_server *server = t->server;
    const struct wlr_touch_down_event *e = data;
    xd_idle_activity(server);
    if (find_point(t, e->touch_id) != nullptr)
        return; /* a device that repeats itself */
    touch_point *p = XD_NEW(touch_point);
    p->id = e->touch_id;
    p->touch = e->touch;
    touch_position(server, e->touch, e->x, e->y, &p->lx, &p->ly);
    wl_list_insert(&t->points, &p->link);

    xd_input_target target = xd_input_target_at(server, p->lx, p->ly);
    if (target.surface != nullptr && server->mode == XD_CURSOR_PASSTHROUGH &&
        wlr_surface_accepts_touch(target.surface, server->seat)) {
        /* Focus as a click would; an open menu's grab sees the touch
         * first instead (and closes the menu if it's outside). */
        if (!server->locked && !wlr_seat_pointer_has_grab(server->seat) &&
            !wlr_seat_touch_has_grab(server->seat)) {
            if (target.view != nullptr)
                xd_view_focus(target.view);
            else if (target.layer != nullptr)
                xd_layer_click_focus(target.layer);
        }
        p->kind = POINT_CLIENT;
        p->off_x = p->lx - target.sx;
        p->off_y = p->ly - target.sy;
        wlr_seat_touch_notify_down(server->seat, target.surface, e->time_msec, e->touch_id,
                                   target.sx, target.sy);
        return;
    }
    if (pointer_held(t) || !pointer_free(server)) {
        p->kind = POINT_IGNORED;
        return;
    }
    p->kind = POINT_POINTER;
    xd_input_warp_pointer(server, p->lx, p->ly, e->time_msec);
    xd_input_button(server, e->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
}

static bool window_grab(const xd_server *server) {
    return server->mode == XD_CURSOR_MOVE || server->mode == XD_CURSOR_RESIZE;
}

static void handle_motion(struct wl_listener *listener, void *data) {
    xd_touch *t = XD_CONTAINER_OF(listener, xd_touch, motion);
    xd_server *server = t->server;
    const struct wlr_touch_motion_event *e = data;
    xd_idle_activity(server);
    touch_point *p = find_point(t, e->touch_id);
    if (p == nullptr)
        return;
    touch_position(server, e->touch, e->x, e->y, &p->lx, &p->ly);
    switch (p->kind) {
    case POINT_CLIENT:
        /* Coordinates stay relative to where the surface was when the
         * finger landed, as a pointer's implicit grab has them. */
        wlr_seat_touch_notify_motion(server->seat, e->time_msec, e->touch_id, p->lx - p->off_x,
                                     p->ly - p->off_y);
        break;
    case POINT_GRAB:
        if (window_grab(server))
            xd_input_warp_pointer(server, p->lx, p->ly, e->time_msec);
        break;
    case POINT_POINTER:
        xd_input_warp_pointer(server, p->lx, p->ly, e->time_msec);
        break;
    case POINT_IGNORED:
        break;
    }
}

static void handle_up(struct wl_listener *listener, void *data) {
    xd_touch *t = XD_CONTAINER_OF(listener, xd_touch, up);
    xd_server *server = t->server;
    const struct wlr_touch_up_event *e = data;
    xd_idle_activity(server);
    touch_point *p = find_point(t, e->touch_id);
    if (p == nullptr)
        return;
    point_kind kind = p->kind;
    drop_point(p);
    switch (kind) {
    case POINT_CLIENT:
        wlr_seat_touch_notify_up(server->seat, e->time_msec, e->touch_id);
        break;
    case POINT_GRAB:
        /* The program had the finger's down: it gets the up too. */
        wlr_seat_touch_notify_up(server->seat, e->time_msec, e->touch_id);
        if (window_grab(server))
            xd_input_button(server, e->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
        break;
    case POINT_POINTER:
        xd_input_button(server, e->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
        break;
    case POINT_IGNORED:
        break;
    }
}

/* Nothing a touch did counts (the device gave up on it, a palm say, or
 * was unplugged). */
static void cancel_point(xd_touch *t, touch_point *p, uint32_t time_msec) {
    xd_server *server = t->server;
    point_kind kind = p->kind;
    int32_t id = p->id;
    drop_point(p);
    if (kind == POINT_CLIENT || kind == POINT_GRAB) {
        struct wlr_touch_point *point = wlr_seat_touch_get_point(server->seat, id);
        if (point != nullptr && point->client != nullptr)
            wlr_seat_touch_notify_cancel(server->seat, point->client);
    }
    if ((kind == POINT_POINTER || kind == POINT_GRAB) && server->mode != XD_CURSOR_PASSTHROUGH)
        xd_cursor_cancel_grab(server);
    /* A program that had the pointer's press gets its release. */
    if (kind == POINT_POINTER && server->seat->pointer_state.button_count > 0)
        xd_input_button(server, time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
}

static void handle_cancel(struct wl_listener *listener, void *data) {
    xd_touch *t = XD_CONTAINER_OF(listener, xd_touch, cancel);
    const struct wlr_touch_cancel_event *e = data;
    touch_point *p = find_point(t, e->touch_id);
    if (p != nullptr)
        cancel_point(t, p, e->time_msec);
}

static void handle_frame(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_touch *t = XD_CONTAINER_OF(listener, xd_touch, frame);
    wlr_seat_touch_notify_frame(t->server->seat);
}

bool xd_touch_claim_grab(xd_server *server, struct wlr_surface *surface, uint32_t serial) {
    struct wlr_touch_point *point = nullptr;
    if (server->touch == nullptr || surface == nullptr ||
        !wlr_seat_validate_touch_grab_serial(server->seat, surface, serial, &point) ||
        point == nullptr)
        return false;
    touch_point *p = find_point(server->touch, point->touch_id);
    if (p == nullptr || p->kind != POINT_CLIENT || pointer_held(server->touch))
        return false;
    /* The move or size starts from the finger, and follows it. */
    p->kind = POINT_GRAB;
    wlr_cursor_warp(server->cursor, nullptr, p->lx, p->ly);
    return true;
}

/* ---- devices ---------------------------------------------------------------- */

static uint32_t now_msec(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void handle_device_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    touch_device *d = XD_CONTAINER_OF(listener, touch_device, destroy);
    xd_server *server = d->owner->server;
    struct wlr_touch *touch = wlr_touch_from_input_device(d->device);
    XD_LIST_FOR_EACH_SAFE(p, tmp, &d->owner->points, touch_point, link) {
        if (p->touch == touch)
            cancel_point(d->owner, p, now_msec());
    }
    xd_unlisten(&d->destroy);
    wl_list_remove(&d->link);
    xd_free(d);
    xd_input_update_caps(server);
}

void xd_touch_add(xd_server *server, struct wlr_input_device *device) {
    struct wlr_touch *touch = wlr_touch_from_input_device(device);
    touch_device *d = XD_NEW(touch_device);
    d->owner = server->touch;
    d->device = device;
    XD_LISTEN(&device->events.destroy, d, destroy, handle_device_destroy);
    wl_list_insert(&server->touch->devices, &d->link);
    wlr_cursor_attach_input_device(server->cursor, device);
    xd_input_map_device(server, device, touch->output_name, true);
}

bool xd_touch_present(const xd_server *server) {
    return server->touch != nullptr && !wl_list_empty(&server->touch->devices);
}

void xd_touch_init(xd_server *server) {
    xd_touch *t = XD_NEW(xd_touch);
    t->server = server;
    wl_list_init(&t->devices);
    wl_list_init(&t->points);
    wl_list_init(&t->mapped);
    server->touch = t;
    XD_LISTEN(&server->cursor->events.touch_down, t, down, handle_down);
    XD_LISTEN(&server->cursor->events.touch_motion, t, motion, handle_motion);
    XD_LISTEN(&server->cursor->events.touch_up, t, up, handle_up);
    XD_LISTEN(&server->cursor->events.touch_cancel, t, cancel, handle_cancel);
    XD_LISTEN(&server->cursor->events.touch_frame, t, frame, handle_frame);
    XD_LISTEN(&server->output_layout->events.change, t, layout_change, handle_layout_change);
}

void xd_touch_finish(xd_server *server) {
    xd_touch *t = server->touch;
    if (t == nullptr)
        return;
    xd_unlisten(&t->down);
    xd_unlisten(&t->motion);
    xd_unlisten(&t->up);
    xd_unlisten(&t->cancel);
    xd_unlisten(&t->frame);
    xd_unlisten(&t->layout_change);
    XD_LIST_FOR_EACH_SAFE(p, ptmp, &t->points, touch_point, link) {
        drop_point(p);
    }
    XD_LIST_FOR_EACH_SAFE(d, dtmp, &t->devices, touch_device, link) {
        xd_unlisten(&d->destroy);
        wl_list_remove(&d->link);
        xd_free(d);
    }
    XD_LIST_FOR_EACH_SAFE(m, mtmp, &t->mapped, mapped_device, link) {
        handle_mapped_destroy(&m->destroy, nullptr);
    }
    xd_free(t);
    server->touch = nullptr;
}
