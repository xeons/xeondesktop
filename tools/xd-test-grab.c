/* SPDX-License-Identifier: MIT
 * xd-test-grab: a program that captures the mouse and keyboard, for the UI
 * tests, as games, virtual machines and remote desktops do. A 400x300 blue
 * window that, as asked, locks or confines the pointer
 * (pointer-constraints), hears relative motion (relative-pointer) and
 * takes the compositor's shortcuts (keyboard-shortcuts-inhibit). What it
 * hears goes, a line each, to $XD_TEST_GRAB_LOG (else stderr), positions
 * and deltas in whole pixels (rounded towards zero).
 *
 *   xd-test-grab [--lock | --confine] [--region X,Y,W,H] [--hint X,Y]
 *                [--oneshot] [--inhibit] [--title TITLE]
 *   xd-test-grab --probe   which of these globals the client is offered
 *                          (and Xwayland's keyboard grab, which it mustn't
 *                          be), then exit
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "../src/ui/protocols.h"

enum { WIDTH = 400, HEIGHT = 300 };

typedef struct options {
    bool lock, confine, oneshot, inhibit, has_region, has_hint;
    int rx, ry, rw, rh;
    int hx, hy;
    const char *title;
} options;

typedef struct grab {
    options opt;
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm_base;
    struct zwp_pointer_constraints_v1 *constraints;
    struct zwp_relative_pointer_manager_v1 *relative;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit;
    bool x11_grab; /* offered Xwayland's keyboard grab */
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    bool configured;
    bool constrained; /* the lock or confinement asked for */
    bool closed;
    FILE *log;
} grab;

[[gnu::format(printf, 2, 3)]] static void say(grab *client, const char *fmt, ...) {
    FILE *file = client->log != nullptr ? client->log : stderr;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(file, fmt, ap);
    va_end(ap);
    fputc('\n', file);
    fflush(file);
}

/* ---- registry --------------------------------------------------------------- */

/* Listened to from the bind on: the seat says what it has at once. */
static const struct wl_seat_listener seat_listener;

static void global_add(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                       uint32_t version) {
    grab *client = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        client->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        client->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0 && client->seat == nullptr) {
        client->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(client->seat, &seat_listener, client);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        client->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    else if (strcmp(iface, zwp_pointer_constraints_v1_interface.name) == 0)
        client->constraints = wl_registry_bind(reg, name, &zwp_pointer_constraints_v1_interface, 1);
    else if (strcmp(iface, zwp_relative_pointer_manager_v1_interface.name) == 0)
        client->relative =
            wl_registry_bind(reg, name, &zwp_relative_pointer_manager_v1_interface, 1);
    else if (strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0)
        client->inhibit =
            wl_registry_bind(reg, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    else if (strcmp(iface, zwp_xwayland_keyboard_grab_manager_v1_interface.name) == 0)
        client->x11_grab = true;
}

static void global_remove([[maybe_unused]] void *data, [[maybe_unused]] struct wl_registry *reg,
                          [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global_add, global_remove};

static void wm_ping([[maybe_unused]] void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {wm_ping};

/* ---- the window ---------------------------------------------------------------- */

static struct wl_buffer *blue_buffer(grab *client) {
    int stride = WIDTH * 4, size = stride * HEIGHT;
    int fd = memfd_create("xd-test-grab", MFD_CLOEXEC);
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
        px[i] = 0xff2050c0;
    munmap(px, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    grab *client = data;
    xdg_surface_ack_configure(xs, serial);
    if (client->configured)
        return;
    client->configured = true;
    client->buffer = blue_buffer(client);
    wl_surface_attach(client->surface, client->buffer, 0, 0);
    wl_surface_damage(client->surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(client->surface);
    say(client, "ready");
}

static const struct xdg_surface_listener surface_listener = {surface_configure};

static void toplevel_configure([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_toplevel *toplevel,
                               [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height,
                               [[maybe_unused]] struct wl_array *states) {}

static void toplevel_close(void *data, [[maybe_unused]] struct xdg_toplevel *toplevel) {
    ((grab *)data)->closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

/* ---- pointer ----------------------------------------------------------------- */

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    say(data, "pointer enter %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void pointer_leave(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface) {
    say(data, "pointer leave");
}

static void pointer_motion(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                           [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    say(data, "motion %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                           [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                           uint32_t button, uint32_t state) {
    say(data, "button %u %s", button,
        state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released");
}

static void pointer_axis([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *pointer,
                         [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis,
                         [[maybe_unused]] wl_fixed_t value) {}
static void pointer_frame([[maybe_unused]] void *data,
                          [[maybe_unused]] struct wl_pointer *pointer) {}
static void pointer_axis_source([[maybe_unused]] void *data,
                                [[maybe_unused]] struct wl_pointer *pointer,
                                [[maybe_unused]] uint32_t source) {}
static void pointer_axis_stop([[maybe_unused]] void *data,
                              [[maybe_unused]] struct wl_pointer *pointer,
                              [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis) {}
static void pointer_axis_discrete([[maybe_unused]] void *data,
                                  [[maybe_unused]] struct wl_pointer *pointer,
                                  [[maybe_unused]] uint32_t axis,
                                  [[maybe_unused]] int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void relative_motion(void *data,
                            [[maybe_unused]] struct zwp_relative_pointer_v1 *relative_pointer,
                            [[maybe_unused]] uint32_t hi, [[maybe_unused]] uint32_t lo,
                            wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t ux, wl_fixed_t uy) {
    say(data, "relative %d %d %d %d", wl_fixed_to_int(dx), wl_fixed_to_int(dy),
        wl_fixed_to_int(ux), wl_fixed_to_int(uy));
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {relative_motion};

static void locked(void *data, [[maybe_unused]] struct zwp_locked_pointer_v1 *locked_pointer) {
    say(data, "locked");
}

static void unlocked(void *data, [[maybe_unused]] struct zwp_locked_pointer_v1 *locked_pointer) {
    say(data, "unlocked");
}

static const struct zwp_locked_pointer_v1_listener locked_listener = {locked, unlocked};

static void confined(void *data,
                     [[maybe_unused]] struct zwp_confined_pointer_v1 *confined_pointer) {
    say(data, "confined");
}

static void unconfined(void *data,
                       [[maybe_unused]] struct zwp_confined_pointer_v1 *confined_pointer) {
    say(data, "unconfined");
}

static const struct zwp_confined_pointer_v1_listener confined_listener = {confined, unconfined};

/* ---- keyboard ------------------------------------------------------------------ */

static void kb_keymap([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                      [[maybe_unused]] uint32_t format, int32_t fd,
                      [[maybe_unused]] uint32_t size) {
    close(fd);
}

static void kb_enter(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                     [[maybe_unused]] uint32_t serial, [[maybe_unused]] struct wl_surface *surface,
                     [[maybe_unused]] struct wl_array *keys) {
    say(data, "keyboard enter");
}

static void kb_leave(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                     [[maybe_unused]] uint32_t serial,
                     [[maybe_unused]] struct wl_surface *surface) {
    say(data, "keyboard leave");
}

static void kb_key(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                   [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time, uint32_t key,
                   uint32_t state) {
    say(data, "key %u %s", key, state == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released");
}

static void kb_modifiers([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                         [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t depressed,
                         [[maybe_unused]] uint32_t latched, [[maybe_unused]] uint32_t locked_mods,
                         [[maybe_unused]] uint32_t group) {}

static void kb_repeat([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                      [[maybe_unused]] int32_t rate, [[maybe_unused]] int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat};

static void inhibitor_active(
    void *data, [[maybe_unused]] struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor) {
    say(data, "inhibitor active");
}

static void inhibitor_inactive(
    void *data, [[maybe_unused]] struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor) {
    say(data, "inhibitor inactive");
}

static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
    inhibitor_active, inhibitor_inactive};

/* ---- the seat -------------------------------------------------------------------- */

/* The lock or confinement, once there's a pointer. */
static void constrain(grab *client) {
    const options *config = &client->opt;
    if (client->constrained || client->surface == nullptr || client->pointer == nullptr ||
        !(config->lock || config->confine))
        return;
    client->constrained = true;
    struct wl_region *region = nullptr;
    if (config->has_region) {
        region = wl_compositor_create_region(client->compositor);
        wl_region_add(region, config->rx, config->ry, config->rw, config->rh);
    }
    uint32_t lifetime = config->oneshot ? ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT
                                        : ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT;
    if (config->lock) {
        struct zwp_locked_pointer_v1 *locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
            client->constraints, client->surface, client->pointer, region, lifetime);
        zwp_locked_pointer_v1_add_listener(locked_pointer, &locked_listener, client);
        if (config->has_hint)
            zwp_locked_pointer_v1_set_cursor_position_hint(
                locked_pointer, wl_fixed_from_int(config->hx), wl_fixed_from_int(config->hy));
    } else {
        struct zwp_confined_pointer_v1 *confined_pointer =
            zwp_pointer_constraints_v1_confine_pointer(client->constraints, client->surface,
                                                       client->pointer, region, lifetime);
        zwp_confined_pointer_v1_add_listener(confined_pointer, &confined_listener, client);
    }
    if (region != nullptr)
        wl_region_destroy(region);
    /* The hint is the surface's state, applied on commit. */
    wl_surface_commit(client->surface);
}

/* A test session's seat has a pointer once xd-inject has connected, and a
 * keyboard while one of its runs lasts. */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    grab *client = data;
    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && client->keyboard != nullptr) {
        wl_keyboard_release(client->keyboard);
        client->keyboard = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && client->pointer == nullptr &&
        client->relative != nullptr) {
        client->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(client->pointer, &pointer_listener, client);
        struct zwp_relative_pointer_v1 *rel =
            zwp_relative_pointer_manager_v1_get_relative_pointer(client->relative, client->pointer);
        zwp_relative_pointer_v1_add_listener(rel, &relative_listener, client);
        constrain(client);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && client->keyboard == nullptr) {
        client->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    }
}

static void seat_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_seat *seat,
                      [[maybe_unused]] const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- main ---------------------------------------------------------------------- */

static bool parse(int argc, char **argv, options *config) {
    config->title = "Grab test";
    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (strcmp(argument, "--lock") == 0) {
            config->lock = true;
        } else if (strcmp(argument, "--confine") == 0) {
            config->confine = true;
        } else if (strcmp(argument, "--oneshot") == 0) {
            config->oneshot = true;
        } else if (strcmp(argument, "--inhibit") == 0) {
            config->inhibit = true;
        } else if (strcmp(argument, "--region") == 0 && value != nullptr &&
                   sscanf(value, "%d,%d,%d,%d", &config->rx, &config->ry, &config->rw,
                          &config->rh) == 4) {
            config->has_region = true;
            i++;
        } else if (strcmp(argument, "--hint") == 0 && value != nullptr &&
                   sscanf(value, "%d,%d", &config->hx, &config->hy) == 2) {
            config->has_hint = true;
            i++;
        } else if (strcmp(argument, "--title") == 0 && value != nullptr) {
            config->title = value;
            i++;
        } else {
            return false;
        }
    }
    return !(config->lock && config->confine);
}

int main(int argc, char **argv) {
    bool probe = argc > 1 && strcmp(argv[1], "--probe") == 0;
    grab client = {0};
    if (!probe && !parse(argc, argv, &client.opt)) {
        fprintf(stderr, "usage: xd-test-grab [--lock | --confine] [--region X,Y,W,H] "
                        "[--hint X,Y] [--oneshot] [--inhibit] [--title TITLE] | --probe\n");
        return 2;
    }
    client.display = wl_display_connect(nullptr);
    if (client.display == nullptr) {
        fprintf(stderr, "xd-test-grab: cannot connect to the Wayland display\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(client.display);
    wl_registry_add_listener(reg, &registry_listener, &client);
    wl_display_roundtrip(client.display);
    if (probe) {
        printf("pointer-constraints %s\nrelative-pointer %s\nshortcuts-inhibit %s\n"
               "xwayland-keyboard-grab %s\n",
               client.constraints != nullptr ? "yes" : "no",
               client.relative != nullptr ? "yes" : "no", client.inhibit != nullptr ? "yes" : "no",
               client.x11_grab ? "yes" : "no");
        wl_display_disconnect(client.display);
        return EXIT_SUCCESS;
    }
    const char *log = getenv("XD_TEST_GRAB_LOG");
    if (log != nullptr)
        client.log = fopen(log, "ae");
    if (client.compositor == nullptr || client.shm == nullptr || client.seat == nullptr ||
        client.wm_base == nullptr || client.constraints == nullptr || client.relative == nullptr ||
        client.inhibit == nullptr) {
        fprintf(stderr, "xd-test-grab: the compositor lacks a protocol this needs\n");
        return EXIT_FAILURE;
    }
    xdg_wm_base_add_listener(client.wm_base, &wm_listener, &client);
    client.surface = wl_compositor_create_surface(client.compositor);
    client.xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base, client.surface);
    xdg_surface_add_listener(client.xdg_surface, &surface_listener, &client);
    client.toplevel = xdg_surface_get_toplevel(client.xdg_surface);
    xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
    xdg_toplevel_set_title(client.toplevel, client.opt.title);
    xdg_toplevel_set_app_id(client.toplevel, "org.xeondesktop.TestGrab");
    if (client.opt.inhibit) {
        struct zwp_keyboard_shortcuts_inhibitor_v1 *in =
            zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
                client.inhibit, client.surface, client.seat);
        zwp_keyboard_shortcuts_inhibitor_v1_add_listener(in, &inhibitor_listener, &client);
    }
    wl_surface_commit(client.surface);
    constrain(&client); /* if the pointer came first */
    while (!client.closed && wl_display_dispatch(client.display) != -1) {
    }
    if (client.log != nullptr)
        fclose(client.log);
    wl_display_disconnect(client.display);
    return EXIT_SUCCESS;
}
