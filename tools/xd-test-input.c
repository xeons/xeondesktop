/* SPDX-License-Identifier: MIT
 * xd-test-input: a window for the UI tests that tells what input reaches
 * it, a line each: the pointer's buttons, touches (wl_touch), the pen and
 * pad (tablet-v2) and touchpad gestures (pointer-gestures).
 *
 *   xd-test-input LOG [--title TITLE] [--no-touch] [--no-tablet] [--drag]
 *
 * --no-touch and --no-tablet leave out wl_touch or tablet-v2, as programs
 * that know nothing of them do: the compositor then works their pointer.
 * --drag moves the window when a finger lands on it, as a header bar
 * does (xdg_toplevel.move with the touch's serial).
 * The window is a plain orange 360x240 box with the compositor's frame;
 * closing it (its Close button, Alt+F4) logs "close" and exits.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "../src/ui/protocols.h"

enum { WIDTH = 360, HEIGHT = 240 };

typedef struct app {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct zwp_tablet_manager_v2 *tablet_manager;
    struct zwp_pointer_gestures_v1 *gestures;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct zwp_pointer_gesture_swipe_v1 *swipe;
    struct zwp_pointer_gesture_pinch_v1 *pinch;
    struct zwp_pointer_gesture_hold_v1 *hold;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    FILE *log;
    bool no_touch, no_tablet, drag;
    bool quit;
} app;

[[gnu::format(printf, 2, 3)]] static void say(app *client, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(client->log, fmt, ap);
    va_end(ap);
    fputc('\n', client->log);
    fflush(client->log);
}

/* ---- the pointer and its gestures ----------------------------------------------- */

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    say(data, "pointer enter %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
}
static void pointer_leave([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *pointer,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface) {}
static void pointer_motion([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *pointer,
                           [[maybe_unused]] uint32_t time, [[maybe_unused]] wl_fixed_t x,
                           [[maybe_unused]] wl_fixed_t y) {}
static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                           [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                           uint32_t button, uint32_t state) {
    say(data, "pointer button %u %s", button, state ? "pressed" : "released");
}
static void pointer_axis([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *pointer,
                         [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis,
                         [[maybe_unused]] wl_fixed_t value) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void swipe_begin(void *data, [[maybe_unused]] struct zwp_pointer_gesture_swipe_v1 *swipe,
                        [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                        [[maybe_unused]] struct wl_surface *surface, uint32_t fingers) {
    say(data, "swipe begin %u", fingers);
}
static void swipe_update(void *data, [[maybe_unused]] struct zwp_pointer_gesture_swipe_v1 *swipe,
                         [[maybe_unused]] uint32_t time, wl_fixed_t dx, wl_fixed_t dy) {
    say(data, "swipe update %.1f %.1f", wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}
static void swipe_end(void *data, [[maybe_unused]] struct zwp_pointer_gesture_swipe_v1 *swipe,
                      [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                      int32_t cancelled) {
    say(data, "swipe end%s", cancelled ? " cancelled" : "");
}
static const struct zwp_pointer_gesture_swipe_v1_listener swipe_listener = {
    swipe_begin, swipe_update, swipe_end};

static void pinch_begin(void *data, [[maybe_unused]] struct zwp_pointer_gesture_pinch_v1 *pinch,
                        [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                        [[maybe_unused]] struct wl_surface *surface, uint32_t fingers) {
    say(data, "pinch begin %u", fingers);
}
static void pinch_update(void *data, [[maybe_unused]] struct zwp_pointer_gesture_pinch_v1 *pinch,
                         [[maybe_unused]] uint32_t time, [[maybe_unused]] wl_fixed_t dx,
                         [[maybe_unused]] wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation) {
    say(data, "pinch update scale %.2f rotation %.1f", wl_fixed_to_double(scale),
        wl_fixed_to_double(rotation));
}
static void pinch_end(void *data, [[maybe_unused]] struct zwp_pointer_gesture_pinch_v1 *pinch,
                      [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                      int32_t cancelled) {
    say(data, "pinch end%s", cancelled ? " cancelled" : "");
}
static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
    pinch_begin, pinch_update, pinch_end};

static void hold_begin(void *data, [[maybe_unused]] struct zwp_pointer_gesture_hold_v1 *hold,
                       [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                       [[maybe_unused]] struct wl_surface *surface, uint32_t fingers) {
    say(data, "hold begin %u", fingers);
}
static void hold_end(void *data, [[maybe_unused]] struct zwp_pointer_gesture_hold_v1 *hold,
                     [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                     int32_t cancelled) {
    say(data, "hold end%s", cancelled ? " cancelled" : "");
}
static const struct zwp_pointer_gesture_hold_v1_listener hold_listener = {hold_begin, hold_end};

/* ---- touch -------------------------------------------------------------------------- */

static void touch_down(void *data, [[maybe_unused]] struct wl_touch *touch, uint32_t serial,
                       [[maybe_unused]] uint32_t time, [[maybe_unused]] struct wl_surface *surface,
                       int32_t id, wl_fixed_t x, wl_fixed_t y) {
    app *client = data;
    say(client, "touch down %d %d %d", id, wl_fixed_to_int(x), wl_fixed_to_int(y));
    if (client->drag)
        xdg_toplevel_move(client->toplevel, client->seat, serial);
}
static void touch_up(void *data, [[maybe_unused]] struct wl_touch *touch,
                     [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time, int32_t id) {
    say(data, "touch up %d", id);
}
static void touch_motion(void *data, [[maybe_unused]] struct wl_touch *touch,
                         [[maybe_unused]] uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    say(data, "touch motion %d %d %d", id, wl_fixed_to_int(x), wl_fixed_to_int(y));
}
static void touch_frame(void *data, [[maybe_unused]] struct wl_touch *touch) {
    say(data, "touch frame");
}
static void touch_cancel(void *data, [[maybe_unused]] struct wl_touch *touch) {
    say(data, "touch cancel");
}

static const struct wl_touch_listener touch_listener = {
    .down = touch_down,
    .up = touch_up,
    .motion = touch_motion,
    .frame = touch_frame,
    .cancel = touch_cancel,
};

/* ---- the seat --------------------------------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    app *client = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && client->pointer == nullptr) {
        client->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(client->pointer, &pointer_listener, client);
        if (client->gestures != nullptr) {
            client->swipe =
                zwp_pointer_gestures_v1_get_swipe_gesture(client->gestures, client->pointer);
            zwp_pointer_gesture_swipe_v1_add_listener(client->swipe, &swipe_listener, client);
            client->pinch =
                zwp_pointer_gestures_v1_get_pinch_gesture(client->gestures, client->pointer);
            zwp_pointer_gesture_pinch_v1_add_listener(client->pinch, &pinch_listener, client);
            if (zwp_pointer_gestures_v1_get_version(client->gestures) >=
                ZWP_POINTER_GESTURES_V1_GET_HOLD_GESTURE_SINCE_VERSION) {
                client->hold =
                    zwp_pointer_gestures_v1_get_hold_gesture(client->gestures, client->pointer);
                zwp_pointer_gesture_hold_v1_add_listener(client->hold, &hold_listener, client);
            }
        }
    }
    /* Touchscreens come and go (the tests' does with each xd-inject). */
    bool touch = (caps & WL_SEAT_CAPABILITY_TOUCH) && !client->no_touch;
    if (touch && client->touch == nullptr) {
        client->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(client->touch, &touch_listener, client);
        say(client, "touchscreen");
    } else if (!touch && client->touch != nullptr) {
        wl_touch_release(client->touch);
        client->touch = nullptr;
    }
}

static void seat_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_seat *seat,
                      [[maybe_unused]] const char *name) {}

static const struct wl_seat_listener seat_listener = {seat_capabilities, seat_name};

/* ---- the pen ------------------------------------------------------------------------------ */

static void tool_type([[maybe_unused]] void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                      [[maybe_unused]] uint32_t type) {}
static void tool_serial([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                        [[maybe_unused]] uint32_t hi, [[maybe_unused]] uint32_t lo) {}
static void tool_capability([[maybe_unused]] void *data,
                            [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                            [[maybe_unused]] uint32_t capability) {}
static void tool_done([[maybe_unused]] void *data,
                      [[maybe_unused]] struct zwp_tablet_tool_v2 *tool) {}
static void tool_removed([[maybe_unused]] void *data, struct zwp_tablet_tool_v2 *tool) {
    zwp_tablet_tool_v2_destroy(tool);
}
static void tool_proximity_in(void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                              [[maybe_unused]] uint32_t serial,
                              [[maybe_unused]] struct zwp_tablet_v2 *tablet,
                              [[maybe_unused]] struct wl_surface *surface) {
    say(data, "tool proximity in");
}
static void tool_proximity_out(void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool) {
    say(data, "tool proximity out");
}
static void tool_down(void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                      [[maybe_unused]] uint32_t serial) {
    say(data, "tool down");
}
static void tool_up(void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool) {
    say(data, "tool up");
}
static void tool_motion(void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool, wl_fixed_t x,
                        wl_fixed_t y) {
    say(data, "tool motion %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
}
static void tool_pressure(void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                          uint32_t pressure) {
    say(data, "tool pressure %u", pressure);
}
static void tool_distance([[maybe_unused]] void *data,
                          [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                          [[maybe_unused]] uint32_t distance) {}
static void tool_tilt(void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool, wl_fixed_t x,
                      wl_fixed_t y) {
    say(data, "tool tilt %.1f %.1f", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void tool_rotation([[maybe_unused]] void *data,
                          [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                          [[maybe_unused]] wl_fixed_t degrees) {}
static void tool_slider([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                        [[maybe_unused]] int32_t position) {}
static void tool_wheel([[maybe_unused]] void *data,
                       [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                       [[maybe_unused]] wl_fixed_t degrees, [[maybe_unused]] int32_t clicks) {}
static void tool_button(void *data, [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                        [[maybe_unused]] uint32_t serial, uint32_t button, uint32_t state) {
    say(data, "tool button %u %s", button, state ? "pressed" : "released");
}
static void tool_frame([[maybe_unused]] void *data,
                       [[maybe_unused]] struct zwp_tablet_tool_v2 *tool,
                       [[maybe_unused]] uint32_t time) {}

static const struct zwp_tablet_tool_v2_listener tool_listener = {
    .type = tool_type,
    .hardware_serial = tool_serial,
    .hardware_id_wacom = tool_serial,
    .capability = tool_capability,
    .done = tool_done,
    .removed = tool_removed,
    .proximity_in = tool_proximity_in,
    .proximity_out = tool_proximity_out,
    .down = tool_down,
    .up = tool_up,
    .motion = tool_motion,
    .pressure = tool_pressure,
    .distance = tool_distance,
    .tilt = tool_tilt,
    .rotation = tool_rotation,
    .slider = tool_slider,
    .wheel = tool_wheel,
    .button = tool_button,
    .frame = tool_frame,
};

/* ---- the pad ------------------------------------------------------------------------------ */

static void ring_source([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwp_tablet_pad_ring_v2 *ring,
                        [[maybe_unused]] uint32_t source) {}
static void ring_angle(void *data, [[maybe_unused]] struct zwp_tablet_pad_ring_v2 *ring,
                       wl_fixed_t degrees) {
    say(data, "pad ring %.1f", wl_fixed_to_double(degrees));
}
static void ring_stop([[maybe_unused]] void *data,
                      [[maybe_unused]] struct zwp_tablet_pad_ring_v2 *ring) {}
static void ring_frame([[maybe_unused]] void *data,
                       [[maybe_unused]] struct zwp_tablet_pad_ring_v2 *ring,
                       [[maybe_unused]] uint32_t time) {}
static const struct zwp_tablet_pad_ring_v2_listener ring_listener = {
    ring_source, ring_angle, ring_stop, ring_frame};

static void strip_source([[maybe_unused]] void *data,
                         [[maybe_unused]] struct zwp_tablet_pad_strip_v2 *strip,
                         [[maybe_unused]] uint32_t source) {}
static void strip_position(void *data, [[maybe_unused]] struct zwp_tablet_pad_strip_v2 *strip,
                           uint32_t position) {
    say(data, "pad strip %u", position);
}
static void strip_stop([[maybe_unused]] void *data,
                       [[maybe_unused]] struct zwp_tablet_pad_strip_v2 *strip) {}
static void strip_frame([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwp_tablet_pad_strip_v2 *strip,
                        [[maybe_unused]] uint32_t time) {}
static const struct zwp_tablet_pad_strip_v2_listener strip_listener = {
    strip_source, strip_position, strip_stop, strip_frame};

static void group_buttons([[maybe_unused]] void *data,
                          [[maybe_unused]] struct zwp_tablet_pad_group_v2 *group,
                          [[maybe_unused]] struct wl_array *buttons) {}
static void group_ring(void *data, [[maybe_unused]] struct zwp_tablet_pad_group_v2 *group,
                       struct zwp_tablet_pad_ring_v2 *ring) {
    zwp_tablet_pad_ring_v2_add_listener(ring, &ring_listener, data);
}
static void group_strip(void *data, [[maybe_unused]] struct zwp_tablet_pad_group_v2 *group,
                        struct zwp_tablet_pad_strip_v2 *strip) {
    zwp_tablet_pad_strip_v2_add_listener(strip, &strip_listener, data);
}
static void group_modes([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwp_tablet_pad_group_v2 *group,
                        [[maybe_unused]] uint32_t modes) {}
static void group_done([[maybe_unused]] void *data,
                       [[maybe_unused]] struct zwp_tablet_pad_group_v2 *group) {}
static void group_mode_switch([[maybe_unused]] void *data,
                              [[maybe_unused]] struct zwp_tablet_pad_group_v2 *group,
                              [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t serial,
                              [[maybe_unused]] uint32_t mode) {}
static const struct zwp_tablet_pad_group_v2_listener group_listener = {
    .buttons = group_buttons,
    .ring = group_ring,
    .strip = group_strip,
    .modes = group_modes,
    .done = group_done,
    .mode_switch = group_mode_switch,
};

static void pad_group(void *data, [[maybe_unused]] struct zwp_tablet_pad_v2 *pad,
                      struct zwp_tablet_pad_group_v2 *group) {
    zwp_tablet_pad_group_v2_add_listener(group, &group_listener, data);
}
static void pad_path([[maybe_unused]] void *data, [[maybe_unused]] struct zwp_tablet_pad_v2 *pad,
                     [[maybe_unused]] const char *path) {}
static void pad_buttons([[maybe_unused]] void *data, [[maybe_unused]] struct zwp_tablet_pad_v2 *pad,
                        [[maybe_unused]] uint32_t buttons) {}
static void pad_done([[maybe_unused]] void *data, [[maybe_unused]] struct zwp_tablet_pad_v2 *pad) {}
static void pad_button(void *data, [[maybe_unused]] struct zwp_tablet_pad_v2 *pad,
                       [[maybe_unused]] uint32_t time, uint32_t button, uint32_t state) {
    say(data, "pad button %u %s", button, state ? "pressed" : "released");
}
static void pad_enter(void *data, [[maybe_unused]] struct zwp_tablet_pad_v2 *pad,
                      [[maybe_unused]] uint32_t serial,
                      [[maybe_unused]] struct zwp_tablet_v2 *tablet,
                      [[maybe_unused]] struct wl_surface *surface) {
    say(data, "pad enter");
}
static void pad_leave(void *data, [[maybe_unused]] struct zwp_tablet_pad_v2 *pad,
                      [[maybe_unused]] uint32_t serial,
                      [[maybe_unused]] struct wl_surface *surface) {
    say(data, "pad leave");
}
static void pad_removed([[maybe_unused]] void *data, struct zwp_tablet_pad_v2 *pad) {
    zwp_tablet_pad_v2_destroy(pad);
}
static const struct zwp_tablet_pad_v2_listener pad_listener = {
    .group = pad_group,
    .path = pad_path,
    .buttons = pad_buttons,
    .done = pad_done,
    .button = pad_button,
    .enter = pad_enter,
    .leave = pad_leave,
    .removed = pad_removed,
};

static void tablet_name([[maybe_unused]] void *data, [[maybe_unused]] struct zwp_tablet_v2 *tablet,
                        [[maybe_unused]] const char *name) {}
static void tablet_id([[maybe_unused]] void *data, [[maybe_unused]] struct zwp_tablet_v2 *tablet,
                      [[maybe_unused]] uint32_t vid, [[maybe_unused]] uint32_t pid) {}
static void tablet_path([[maybe_unused]] void *data, [[maybe_unused]] struct zwp_tablet_v2 *tablet,
                        [[maybe_unused]] const char *path) {}
static void tablet_done(void *data, [[maybe_unused]] struct zwp_tablet_v2 *tablet) {
    say(data, "tablet");
}
static void tablet_removed([[maybe_unused]] void *data, struct zwp_tablet_v2 *tablet) {
    zwp_tablet_v2_destroy(tablet);
}
static const struct zwp_tablet_v2_listener tablet_listener = {
    .name = tablet_name,
    .id = tablet_id,
    .path = tablet_path,
    .done = tablet_done,
    .removed = tablet_removed,
};

static void seat_tablet_added(void *data, [[maybe_unused]] struct zwp_tablet_seat_v2 *tablet_seat,
                              struct zwp_tablet_v2 *tablet) {
    zwp_tablet_v2_add_listener(tablet, &tablet_listener, data);
}
static void seat_tool_added(void *data, [[maybe_unused]] struct zwp_tablet_seat_v2 *tablet_seat,
                            struct zwp_tablet_tool_v2 *tool) {
    zwp_tablet_tool_v2_add_listener(tool, &tool_listener, data);
}
static void seat_pad_added(void *data, [[maybe_unused]] struct zwp_tablet_seat_v2 *tablet_seat,
                           struct zwp_tablet_pad_v2 *pad) {
    zwp_tablet_pad_v2_add_listener(pad, &pad_listener, data);
}
static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
    seat_tablet_added, seat_tool_added, seat_pad_added};

/* ---- the window ---------------------------------------------------------------------------- */

static void wm_ping([[maybe_unused]] void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void xdg_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    app *client = data;
    xdg_surface_ack_configure(xs, serial);
    wl_surface_attach(client->surface, client->buffer, 0, 0);
    wl_surface_damage(client->surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(client->surface);
}
static const struct xdg_surface_listener xdg_surface_listener = {xdg_configure};

static void toplevel_configure([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_toplevel *toplevel,
                               [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height,
                               [[maybe_unused]] struct wl_array *states) {}
static void toplevel_close(void *data, [[maybe_unused]] struct xdg_toplevel *toplevel) {
    app *client = data;
    say(client, "close");
    client->quit = true;
}
static void toplevel_bounds([[maybe_unused]] void *data,
                            [[maybe_unused]] struct xdg_toplevel *toplevel,
                            [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}
static void toplevel_caps([[maybe_unused]] void *data,
                          [[maybe_unused]] struct xdg_toplevel *toplevel,
                          [[maybe_unused]] struct wl_array *caps) {}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_caps,
};

static struct wl_buffer *orange_buffer(app *client) {
    int stride = WIDTH * 4, size = stride * HEIGHT;
    int fd = memfd_create("xd-test-input", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0) {
        if (fd >= 0)
            close(fd);
        return nullptr;
    }
    uint32_t *px = mmap(nullptr, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    for (int i = 0; i < WIDTH * HEIGHT; i++)
        px[i] = 0xffff8000;
    munmap(px, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void global_add(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                       uint32_t version) {
    app *client = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        client->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        client->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        client->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0 && client->seat == nullptr)
        client->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 3 ? version : 3);
    else if (strcmp(iface, zwp_tablet_manager_v2_interface.name) == 0 && !client->no_tablet)
        client->tablet_manager = wl_registry_bind(reg, name, &zwp_tablet_manager_v2_interface, 1);
    else if (strcmp(iface, zwp_pointer_gestures_v1_interface.name) == 0)
        client->gestures = wl_registry_bind(reg, name, &zwp_pointer_gestures_v1_interface,
                                            version < 3 ? version : 3);
}

static void global_remove([[maybe_unused]] void *data, [[maybe_unused]] struct wl_registry *reg,
                          [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global_add, global_remove};

int main(int argc, char **argv) {
    app client = {0};
    const char *title = "Input test";
    const char *log = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-touch") == 0)
            client.no_touch = true;
        else if (strcmp(argv[i], "--no-tablet") == 0)
            client.no_tablet = true;
        else if (strcmp(argv[i], "--drag") == 0)
            client.drag = true;
        else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc)
            title = argv[++i];
        else if (log == nullptr && argv[i][0] != '-')
            log = argv[i];
        else {
            fprintf(stderr, "usage: xd-test-input LOG [--title TITLE] [--no-touch] "
                            "[--no-tablet] [--drag]\n");
            return EXIT_FAILURE;
        }
    }
    if (log == nullptr) {
        fprintf(stderr, "usage: xd-test-input LOG [--title TITLE] [--no-touch] [--no-tablet] "
                        "[--drag]\n");
        return EXIT_FAILURE;
    }
    client.log = fopen(log, "ae");
    if (client.log == nullptr) {
        fprintf(stderr, "xd-test-input: %s: %s\n", log, strerror(errno));
        return EXIT_FAILURE;
    }
    client.display = wl_display_connect(nullptr);
    if (client.display == nullptr) {
        fprintf(stderr, "xd-test-input: cannot connect to the Wayland display\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(client.display);
    wl_registry_add_listener(reg, &registry_listener, &client);
    wl_display_roundtrip(client.display);
    if (client.compositor == nullptr || client.shm == nullptr || client.wm_base == nullptr ||
        client.seat == nullptr) {
        fprintf(stderr, "xd-test-input: the compositor lacks a core global\n");
        return EXIT_FAILURE;
    }
    xdg_wm_base_add_listener(client.wm_base, &wm_listener, &client);
    wl_seat_add_listener(client.seat, &seat_listener, &client);
    if (client.tablet_manager != nullptr) {
        struct zwp_tablet_seat_v2 *ts =
            zwp_tablet_manager_v2_get_tablet_seat(client.tablet_manager, client.seat);
        zwp_tablet_seat_v2_add_listener(ts, &tablet_seat_listener, &client);
    }
    client.buffer = orange_buffer(&client);
    if (client.buffer == nullptr) {
        fprintf(stderr, "xd-test-input: cannot make a buffer\n");
        return EXIT_FAILURE;
    }
    client.surface = wl_compositor_create_surface(client.compositor);
    client.xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base, client.surface);
    xdg_surface_add_listener(client.xdg_surface, &xdg_surface_listener, &client);
    client.toplevel = xdg_surface_get_toplevel(client.xdg_surface);
    xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
    xdg_toplevel_set_title(client.toplevel, title);
    xdg_toplevel_set_app_id(client.toplevel, "org.xeondesktop.TestInput");
    wl_surface_commit(client.surface);
    say(&client, "ready");
    while (!client.quit && wl_display_dispatch(client.display) != -1) {
    }
    fclose(client.log);
    wl_display_disconnect(client.display);
    return EXIT_SUCCESS;
}
