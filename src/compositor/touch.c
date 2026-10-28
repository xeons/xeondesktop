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
#include <wlr/types/wlr_data_device.h>
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
        struct wlr_output *output = out->wlr_output;
        if (wlr_output_layout_get(server->output_layout, output) == nullptr)
            continue; /* turned off */
        if (want != nullptr && strcmp(output->name, want) == 0)
            return output;
        if (builtin == nullptr && is_builtin_panel(output->name))
            builtin = output;
        if (first == nullptr)
            first = output;
    }
    return builtin != nullptr ? builtin : first;
}

static void apply_mapping(mapped_device *mapping) {
    xd_server *server = mapping->owner->server;
    struct wlr_output *output =
        mapping->screen ? screen_output(server, mapping->output_name) : nullptr;
    if (output != mapping->output)
        wlr_log(WLR_INFO, "'%s' mapped to %s",
                mapping->device->name != nullptr ? mapping->device->name : "?",
                output != nullptr ? output->name : "the whole desktop");
    mapping->output = output;
    wlr_cursor_map_input_to_output(server->cursor, mapping->device, output);
}

static void handle_mapped_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    mapped_device *mapping = XD_CONTAINER_OF(listener, mapped_device, destroy);
    xd_unlisten(&mapping->destroy);
    wl_list_remove(&mapping->link);
    xd_free(mapping->output_name);
    xd_free(mapping);
}

void xd_input_map_device(xd_server *server, struct wlr_input_device *device,
                         const char *output_name, bool screen) {
    mapped_device *mapping = XD_NEW(mapped_device);
    mapping->owner = server->touch;
    mapping->device = device;
    mapping->screen = screen;
    if (output_name != nullptr)
        mapping->output_name = xd_str_dup_cstr(xd_str_from_cstr(output_name));
    XD_LISTEN(&device->events.destroy, mapping, destroy, handle_mapped_destroy);
    wl_list_insert(&server->touch->mapped, &mapping->link);
    apply_mapping(mapping);
}

struct wlr_box xd_input_device_area(xd_server *server, const struct wlr_input_device *device) {
    struct wlr_box box = {0};
    XD_LIST_FOR_EACH(mapping, &server->touch->mapped, mapped_device, link) {
        if (mapping->device == device && mapping->output != nullptr) {
            wlr_output_layout_get_box(server->output_layout, mapping->output, &box);
            return box;
        }
    }
    wlr_output_layout_get_box(server->output_layout, nullptr, &box);
    return box;
}

/* A monitor came, went or moved: the mapping follows (wlr_cursor would
 * keep a monitor that's gone). */
static void handle_layout_change(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_touch *touch = XD_CONTAINER_OF(listener, xd_touch, layout_change);
    XD_LIST_FOR_EACH(mapping, &touch->mapped, mapped_device, link) { apply_mapping(mapping); }
}

/* ---- touch points --------------------------------------------------------------- */

/* A contact is its device's: libinput numbers them across the seat, but
 * other backends (a nested session's, the tests') number each device's
 * from 0. */
static touch_point *find_point(xd_touch *touch, const struct wlr_touch *device, int32_t id) {
    XD_LIST_FOR_EACH(contact, &touch->points, touch_point, link) {
        if (contact->touch == device && contact->id == id)
            return contact;
    }
    return nullptr;
}

static void drop_point(touch_point *contact) {
    wl_list_remove(&contact->link);
    xd_free(contact);
}

static bool pointer_held(const xd_touch *touch) {
    XD_LIST_FOR_EACH(contact, &touch->points, touch_point, link) {
        if (contact->kind == POINT_POINTER || contact->kind == POINT_GRAB)
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
    xd_touch *touch = XD_CONTAINER_OF(listener, xd_touch, down);
    xd_server *server = touch->server;
    const struct wlr_touch_down_event *touch_down_event = data;
    xd_idle_activity(server);
    if (find_point(touch, touch_down_event->touch, touch_down_event->touch_id) != nullptr)
        return; /* a device that repeats itself */
    touch_point *contact = XD_NEW(touch_point);
    contact->id = touch_down_event->touch_id;
    contact->touch = touch_down_event->touch;
    touch_position(server, touch_down_event->touch, touch_down_event->x, touch_down_event->y,
                   &contact->lx, &contact->ly);
    wl_list_insert(&touch->points, &contact->link);

    xd_input_target target = xd_input_target_at(server, contact->lx, contact->ly);
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
        contact->kind = POINT_CLIENT;
        contact->off_x = contact->lx - target.sx;
        contact->off_y = contact->ly - target.sy;
        wlr_seat_touch_notify_down(server->seat, target.surface, touch_down_event->time_msec,
                                   touch_down_event->touch_id, target.sx, target.sy);
        return;
    }
    if (pointer_held(touch) || !pointer_free(server)) {
        contact->kind = POINT_IGNORED;
        return;
    }
    contact->kind = POINT_POINTER;
    xd_input_warp_pointer(server, contact->lx, contact->ly, touch_down_event->time_msec);
    xd_input_button(server, touch_down_event->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
}

/* A finger dragging (drag and drop it started): the drag goes to whatever
 * is under it, as wlroots' drag grab expects the compositor to say. */
static bool move_touch_drag(xd_server *server, const touch_point *contact, uint32_t time_msec) {
    const struct wlr_drag *drag = server->seat->drag;
    if (drag == nullptr || drag->grab_type != WLR_DRAG_GRAB_KEYBOARD_TOUCH ||
        drag->touch_id != contact->id)
        return false;
    xd_input_target target = xd_input_target_at(server, contact->lx, contact->ly);
    if (target.surface != nullptr) {
        wlr_seat_touch_point_focus(server->seat, target.surface, time_msec, contact->id,
                                   target.sx, target.sy);
        wlr_seat_touch_notify_motion(server->seat, time_msec, contact->id, target.sx, target.sy);
    } else {
        wlr_seat_touch_point_clear_focus(server->seat, time_msec, contact->id);
    }
    xd_input_place_drag_icon(server, contact->lx, contact->ly);
    return true;
}

bool xd_touch_get_position(const xd_server *server, int32_t id, double *lx, double *ly) {
    if (server->touch == nullptr)
        return false;
    XD_LIST_FOR_EACH(contact, &server->touch->points, touch_point, link) {
        if (contact->id == id && contact->kind == POINT_CLIENT) {
            *lx = contact->lx;
            *ly = contact->ly;
            return true;
        }
    }
    return false;
}

static bool window_grab(const xd_server *server) {
    return server->mode == XD_CURSOR_MOVE || server->mode == XD_CURSOR_RESIZE;
}

static void handle_motion(struct wl_listener *listener, void *data) {
    xd_touch *touch = XD_CONTAINER_OF(listener, xd_touch, motion);
    xd_server *server = touch->server;
    const struct wlr_touch_motion_event *touch_motion_event = data;
    xd_idle_activity(server);
    touch_point *contact = find_point(touch, touch_motion_event->touch, touch_motion_event->touch_id);
    if (contact == nullptr)
        return;
    touch_position(server, touch_motion_event->touch, touch_motion_event->x, touch_motion_event->y,
                   &contact->lx, &contact->ly);
    switch (contact->kind) {
    case POINT_CLIENT:
        if (move_touch_drag(server, contact, touch_motion_event->time_msec))
            break;
        /* Coordinates stay relative to where the surface was when the
         * finger landed, as a pointer's implicit grab has them. */
        wlr_seat_touch_notify_motion(server->seat, touch_motion_event->time_msec,
                                     touch_motion_event->touch_id, contact->lx - contact->off_x,
                                     contact->ly - contact->off_y);
        break;
    case POINT_GRAB:
        if (window_grab(server))
            xd_input_warp_pointer(server, contact->lx, contact->ly, touch_motion_event->time_msec);
        break;
    case POINT_POINTER:
        xd_input_warp_pointer(server, contact->lx, contact->ly, touch_motion_event->time_msec);
        break;
    case POINT_IGNORED:
        break;
    }
}

static void handle_up(struct wl_listener *listener, void *data) {
    xd_touch *touch = XD_CONTAINER_OF(listener, xd_touch, up);
    xd_server *server = touch->server;
    const struct wlr_touch_up_event *touch_up_event = data;
    xd_idle_activity(server);
    touch_point *contact = find_point(touch, touch_up_event->touch, touch_up_event->touch_id);
    if (contact == nullptr)
        return;
    point_kind kind = contact->kind;
    drop_point(contact);
    switch (kind) {
    case POINT_CLIENT:
        wlr_seat_touch_notify_up(server->seat, touch_up_event->time_msec, touch_up_event->touch_id);
        break;
    case POINT_GRAB:
        /* The program had the finger's down: it gets the up too. */
        wlr_seat_touch_notify_up(server->seat, touch_up_event->time_msec, touch_up_event->touch_id);
        if (window_grab(server))
            xd_input_button(server, touch_up_event->time_msec, BTN_LEFT,
                            WL_POINTER_BUTTON_STATE_RELEASED);
        break;
    case POINT_POINTER:
        xd_input_button(server, touch_up_event->time_msec, BTN_LEFT,
                        WL_POINTER_BUTTON_STATE_RELEASED);
        break;
    case POINT_IGNORED:
        break;
    }
}

/* Nothing a touch did counts (the device gave up on it, a palm say, or
 * was unplugged). */
static void cancel_point(xd_touch *touch, touch_point *contact, uint32_t time_msec) {
    xd_server *server = touch->server;
    point_kind kind = contact->kind;
    int32_t id = contact->id;
    bool grabbing = kind == POINT_POINTER || kind == POINT_GRAB;
    drop_point(contact);
    if (kind == POINT_CLIENT || kind == POINT_GRAB) {
        struct wlr_touch_point *point = wlr_seat_touch_get_point(server->seat, id);
        if (point != nullptr && point->client != nullptr) {
            /* wl_touch's cancel ends every touch the program has: its
             * other fingers count for nothing more until they lift. */
            struct wlr_seat_client *client = point->client;
            XD_LIST_FOR_EACH(other, &touch->points, touch_point, link) {
                if (other->kind != POINT_CLIENT && other->kind != POINT_GRAB)
                    continue;
                struct wlr_touch_point *other_point =
                    wlr_seat_touch_get_point(server->seat, other->id);
                if (other_point == nullptr || other_point->client != client)
                    continue;
                if (other->kind == POINT_GRAB)
                    grabbing = true; /* its move or size ends too */
                other->kind = POINT_IGNORED;
            }
            wlr_seat_touch_notify_cancel(server->seat, client);
        }
    }
    if (grabbing && server->mode != XD_CURSOR_PASSTHROUGH)
        xd_cursor_cancel_grab(server);
    /* A program that had the pointer's press gets its release. */
    if (kind == POINT_POINTER && server->seat->pointer_state.button_count > 0)
        xd_input_button(server, time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
}

static void handle_cancel(struct wl_listener *listener, void *data) {
    xd_touch *touch = XD_CONTAINER_OF(listener, xd_touch, cancel);
    const struct wlr_touch_cancel_event *touch_cancel_event = data;
    touch_point *contact = find_point(touch, touch_cancel_event->touch, touch_cancel_event->touch_id);
    if (contact != nullptr)
        cancel_point(touch, contact, touch_cancel_event->time_msec);
}

static void handle_frame(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_touch *touch = XD_CONTAINER_OF(listener, xd_touch, frame);
    wlr_seat_touch_notify_frame(touch->server->seat);
}

bool xd_touch_claim_grab(xd_server *server, struct wlr_surface *surface, uint32_t serial) {
    struct wlr_touch_point *point = nullptr;
    if (server->touch == nullptr || surface == nullptr ||
        !wlr_seat_validate_touch_grab_serial(server->seat, surface, serial, &point) ||
        point == nullptr)
        return false;
    if (pointer_held(server->touch))
        return false;
    /* The seat knows the touch by its id alone. */
    XD_LIST_FOR_EACH(contact, &server->touch->points, touch_point, link) {
        if (contact->id != point->touch_id || contact->kind != POINT_CLIENT)
            continue;
        /* The move or size starts from the finger, and follows it. */
        contact->kind = POINT_GRAB;
        wlr_cursor_warp(server->cursor, nullptr, contact->lx, contact->ly);
        return true;
    }
    return false;
}

void xd_touch_revert_grab(xd_server *server) {
    if (server->touch == nullptr)
        return;
    XD_LIST_FOR_EACH(contact, &server->touch->points, touch_point, link) {
        if (contact->kind == POINT_GRAB)
            contact->kind = POINT_CLIENT;
    }
}

/* ---- devices ---------------------------------------------------------------- */

static uint32_t now_msec(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void handle_device_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    touch_device *touch_input = XD_CONTAINER_OF(listener, touch_device, destroy);
    xd_server *server = touch_input->owner->server;
    struct wlr_touch *touch = wlr_touch_from_input_device(touch_input->device);
    XD_LIST_FOR_EACH_SAFE(contact, tmp, &touch_input->owner->points, touch_point, link) {
        if (contact->touch == touch)
            cancel_point(touch_input->owner, contact, now_msec());
    }
    xd_unlisten(&touch_input->destroy);
    wl_list_remove(&touch_input->link);
    xd_free(touch_input);
    xd_input_update_caps(server);
}

void xd_touch_add(xd_server *server, struct wlr_input_device *device) {
    struct wlr_touch *touch = wlr_touch_from_input_device(device);
    touch_device *touch_input = XD_NEW(touch_device);
    touch_input->owner = server->touch;
    touch_input->device = device;
    XD_LISTEN(&device->events.destroy, touch_input, destroy, handle_device_destroy);
    wl_list_insert(&server->touch->devices, &touch_input->link);
    wlr_cursor_attach_input_device(server->cursor, device);
    xd_input_map_device(server, device, touch->output_name, true);
}

bool xd_touch_present(const xd_server *server) {
    return server->touch != nullptr && !wl_list_empty(&server->touch->devices);
}

void xd_touch_init(xd_server *server) {
    xd_touch *touch = XD_NEW(xd_touch);
    touch->server = server;
    wl_list_init(&touch->devices);
    wl_list_init(&touch->points);
    wl_list_init(&touch->mapped);
    server->touch = touch;
    XD_LISTEN(&server->cursor->events.touch_down, touch, down, handle_down);
    XD_LISTEN(&server->cursor->events.touch_motion, touch, motion, handle_motion);
    XD_LISTEN(&server->cursor->events.touch_up, touch, up, handle_up);
    XD_LISTEN(&server->cursor->events.touch_cancel, touch, cancel, handle_cancel);
    XD_LISTEN(&server->cursor->events.touch_frame, touch, frame, handle_frame);
    XD_LISTEN(&server->output_layout->events.change, touch, layout_change, handle_layout_change);
}

void xd_touch_finish(xd_server *server) {
    xd_touch *touch = server->touch;
    if (touch == nullptr)
        return;
    xd_unlisten(&touch->down);
    xd_unlisten(&touch->motion);
    xd_unlisten(&touch->up);
    xd_unlisten(&touch->cancel);
    xd_unlisten(&touch->frame);
    xd_unlisten(&touch->layout_change);
    XD_LIST_FOR_EACH_SAFE(contact, ptmp, &touch->points, touch_point, link) { drop_point(contact); }
    XD_LIST_FOR_EACH_SAFE(touch_input, dtmp, &touch->devices, touch_device, link) {
        xd_unlisten(&touch_input->destroy);
        wl_list_remove(&touch_input->link);
        xd_free(touch_input);
    }
    XD_LIST_FOR_EACH_SAFE(mapping, mtmp, &touch->mapped, mapped_device, link) {
        handle_mapped_destroy(&mapping->destroy, nullptr);
    }
    xd_free(touch);
    server->touch = nullptr;
}
