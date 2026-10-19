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

[[gnu::format(printf, 2, 3)]] static void say(grab *g, const char *fmt, ...) {
    FILE *f = g->log != nullptr ? g->log : stderr;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

/* ---- registry --------------------------------------------------------------- */

/* Listened to from the bind on: the seat says what it has at once. */
static const struct wl_seat_listener seat_listener;

static void global_add(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                       uint32_t version) {
    grab *g = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        g->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        g->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0 && g->seat == nullptr) {
        g->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(g->seat, &seat_listener, g);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        g->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    else if (strcmp(iface, zwp_pointer_constraints_v1_interface.name) == 0)
        g->constraints = wl_registry_bind(reg, name, &zwp_pointer_constraints_v1_interface, 1);
    else if (strcmp(iface, zwp_relative_pointer_manager_v1_interface.name) == 0)
        g->relative = wl_registry_bind(reg, name, &zwp_relative_pointer_manager_v1_interface, 1);
    else if (strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0)
        g->inhibit =
            wl_registry_bind(reg, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    else if (strcmp(iface, zwp_xwayland_keyboard_grab_manager_v1_interface.name) == 0)
        g->x11_grab = true;
}

static void global_remove([[maybe_unused]] void *data, [[maybe_unused]] struct wl_registry *reg,
                          [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global_add, global_remove};

static void wm_ping([[maybe_unused]] void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {wm_ping};

/* ---- the window ---------------------------------------------------------------- */

static struct wl_buffer *blue_buffer(grab *g) {
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
    struct wl_shm_pool *pool = wl_shm_create_pool(g->shm, fd, size);
    struct wl_buffer *b =
        wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static void surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    grab *g = data;
    xdg_surface_ack_configure(xs, serial);
    if (g->configured)
        return;
    g->configured = true;
    g->buffer = blue_buffer(g);
    wl_surface_attach(g->surface, g->buffer, 0, 0);
    wl_surface_damage(g->surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(g->surface);
    say(g, "ready");
}

static const struct xdg_surface_listener surface_listener = {surface_configure};

static void toplevel_configure([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_toplevel *t,
                               [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h,
                               [[maybe_unused]] struct wl_array *states) {}

static void toplevel_close(void *data, [[maybe_unused]] struct xdg_toplevel *t) {
    ((grab *)data)->closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

/* ---- pointer ----------------------------------------------------------------- */

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *p,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    say(data, "pointer enter %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void pointer_leave(void *data, [[maybe_unused]] struct wl_pointer *p,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *s) {
    say(data, "pointer leave");
}

static void pointer_motion(void *data, [[maybe_unused]] struct wl_pointer *p,
                           [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    say(data, "motion %d %d", wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *p,
                           [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                           uint32_t button, uint32_t state) {
    say(data, "button %u %s", button,
        state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released");
}

static void pointer_axis([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *p,
                         [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis,
                         [[maybe_unused]] wl_fixed_t value) {}
static void pointer_frame([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *p) {}
static void pointer_axis_source([[maybe_unused]] void *data,
                                [[maybe_unused]] struct wl_pointer *p,
                                [[maybe_unused]] uint32_t source) {}
static void pointer_axis_stop([[maybe_unused]] void *data, [[maybe_unused]] struct wl_pointer *p,
                              [[maybe_unused]] uint32_t time, [[maybe_unused]] uint32_t axis) {}
static void pointer_axis_discrete([[maybe_unused]] void *data,
                                  [[maybe_unused]] struct wl_pointer *p,
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

static void relative_motion(void *data, [[maybe_unused]] struct zwp_relative_pointer_v1 *r,
                            [[maybe_unused]] uint32_t hi, [[maybe_unused]] uint32_t lo,
                            wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t ux, wl_fixed_t uy) {
    say(data, "relative %d %d %d %d", wl_fixed_to_int(dx), wl_fixed_to_int(dy),
        wl_fixed_to_int(ux), wl_fixed_to_int(uy));
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {relative_motion};

static void locked(void *data, [[maybe_unused]] struct zwp_locked_pointer_v1 *l) {
    say(data, "locked");
}

static void unlocked(void *data, [[maybe_unused]] struct zwp_locked_pointer_v1 *l) {
    say(data, "unlocked");
}

static const struct zwp_locked_pointer_v1_listener locked_listener = {locked, unlocked};

static void confined(void *data, [[maybe_unused]] struct zwp_confined_pointer_v1 *c) {
    say(data, "confined");
}

static void unconfined(void *data, [[maybe_unused]] struct zwp_confined_pointer_v1 *c) {
    say(data, "unconfined");
}

static const struct zwp_confined_pointer_v1_listener confined_listener = {confined, unconfined};

/* ---- keyboard ------------------------------------------------------------------ */

static void kb_keymap([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *k,
                      [[maybe_unused]] uint32_t format, int32_t fd,
                      [[maybe_unused]] uint32_t size) {
    close(fd);
}

static void kb_enter(void *data, [[maybe_unused]] struct wl_keyboard *k,
                     [[maybe_unused]] uint32_t serial, [[maybe_unused]] struct wl_surface *s,
                     [[maybe_unused]] struct wl_array *keys) {
    say(data, "keyboard enter");
}

static void kb_leave(void *data, [[maybe_unused]] struct wl_keyboard *k,
                     [[maybe_unused]] uint32_t serial, [[maybe_unused]] struct wl_surface *s) {
    say(data, "keyboard leave");
}

static void kb_key(void *data, [[maybe_unused]] struct wl_keyboard *k,
                   [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                   uint32_t key, uint32_t state) {
    say(data, "key %u %s", key, state == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released");
}

static void kb_modifiers([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *k,
                         [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t depressed,
                         [[maybe_unused]] uint32_t latched, [[maybe_unused]] uint32_t locked_mods,
                         [[maybe_unused]] uint32_t group) {}

static void kb_repeat([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *k,
                      [[maybe_unused]] int32_t rate, [[maybe_unused]] int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat};

static void inhibitor_active(void *data,
                             [[maybe_unused]] struct zwp_keyboard_shortcuts_inhibitor_v1 *i) {
    say(data, "inhibitor active");
}

static void inhibitor_inactive(void *data,
                               [[maybe_unused]] struct zwp_keyboard_shortcuts_inhibitor_v1 *i) {
    say(data, "inhibitor inactive");
}

static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
    inhibitor_active, inhibitor_inactive};

/* ---- the seat -------------------------------------------------------------------- */

/* The lock or confinement, once there's a pointer. */
static void constrain(grab *g) {
    const options *o = &g->opt;
    if (g->constrained || g->surface == nullptr || g->pointer == nullptr ||
        !(o->lock || o->confine))
        return;
    g->constrained = true;
    struct wl_region *region = nullptr;
    if (o->has_region) {
        region = wl_compositor_create_region(g->compositor);
        wl_region_add(region, o->rx, o->ry, o->rw, o->rh);
    }
    uint32_t lifetime = o->oneshot ? ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT
                                   : ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT;
    if (o->lock) {
        struct zwp_locked_pointer_v1 *l = zwp_pointer_constraints_v1_lock_pointer(
            g->constraints, g->surface, g->pointer, region, lifetime);
        zwp_locked_pointer_v1_add_listener(l, &locked_listener, g);
        if (o->has_hint)
            zwp_locked_pointer_v1_set_cursor_position_hint(l, wl_fixed_from_int(o->hx),
                                                           wl_fixed_from_int(o->hy));
    } else {
        struct zwp_confined_pointer_v1 *c = zwp_pointer_constraints_v1_confine_pointer(
            g->constraints, g->surface, g->pointer, region, lifetime);
        zwp_confined_pointer_v1_add_listener(c, &confined_listener, g);
    }
    if (region != nullptr)
        wl_region_destroy(region);
    /* The hint is the surface's state, applied on commit. */
    wl_surface_commit(g->surface);
}

/* A test session's seat has a pointer once xd-inject has connected, and a
 * keyboard while one of its runs lasts. */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    grab *g = data;
    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && g->keyboard != nullptr) {
        wl_keyboard_release(g->keyboard);
        g->keyboard = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && g->pointer == nullptr && g->relative != nullptr) {
        g->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g->pointer, &pointer_listener, g);
        struct zwp_relative_pointer_v1 *rel =
            zwp_relative_pointer_manager_v1_get_relative_pointer(g->relative, g->pointer);
        zwp_relative_pointer_v1_add_listener(rel, &relative_listener, g);
        constrain(g);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && g->keyboard == nullptr) {
        g->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g->keyboard, &keyboard_listener, g);
    }
}

static void seat_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_seat *seat,
                      [[maybe_unused]] const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- main ---------------------------------------------------------------------- */

static bool parse(int argc, char **argv, options *o) {
    o->title = "Grab test";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (strcmp(a, "--lock") == 0) {
            o->lock = true;
        } else if (strcmp(a, "--confine") == 0) {
            o->confine = true;
        } else if (strcmp(a, "--oneshot") == 0) {
            o->oneshot = true;
        } else if (strcmp(a, "--inhibit") == 0) {
            o->inhibit = true;
        } else if (strcmp(a, "--region") == 0 && v != nullptr &&
                   sscanf(v, "%d,%d,%d,%d", &o->rx, &o->ry, &o->rw, &o->rh) == 4) {
            o->has_region = true;
            i++;
        } else if (strcmp(a, "--hint") == 0 && v != nullptr &&
                   sscanf(v, "%d,%d", &o->hx, &o->hy) == 2) {
            o->has_hint = true;
            i++;
        } else if (strcmp(a, "--title") == 0 && v != nullptr) {
            o->title = v;
            i++;
        } else {
            return false;
        }
    }
    return !(o->lock && o->confine);
}

int main(int argc, char **argv) {
    bool probe = argc > 1 && strcmp(argv[1], "--probe") == 0;
    grab g = {0};
    if (!probe && !parse(argc, argv, &g.opt)) {
        fprintf(stderr, "usage: xd-test-grab [--lock | --confine] [--region X,Y,W,H] "
                        "[--hint X,Y] [--oneshot] [--inhibit] [--title TITLE] | --probe\n");
        return 2;
    }
    g.display = wl_display_connect(nullptr);
    if (g.display == nullptr) {
        fprintf(stderr, "xd-test-grab: cannot connect to the Wayland display\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(g.display);
    wl_registry_add_listener(reg, &registry_listener, &g);
    wl_display_roundtrip(g.display);
    if (probe) {
        printf("pointer-constraints %s\nrelative-pointer %s\nshortcuts-inhibit %s\n"
               "xwayland-keyboard-grab %s\n",
               g.constraints != nullptr ? "yes" : "no", g.relative != nullptr ? "yes" : "no",
               g.inhibit != nullptr ? "yes" : "no", g.x11_grab ? "yes" : "no");
        wl_display_disconnect(g.display);
        return EXIT_SUCCESS;
    }
    const char *log = getenv("XD_TEST_GRAB_LOG");
    if (log != nullptr)
        g.log = fopen(log, "ae");
    if (g.compositor == nullptr || g.shm == nullptr || g.seat == nullptr ||
        g.wm_base == nullptr || g.constraints == nullptr || g.relative == nullptr ||
        g.inhibit == nullptr) {
        fprintf(stderr, "xd-test-grab: the compositor lacks a protocol this needs\n");
        return EXIT_FAILURE;
    }
    xdg_wm_base_add_listener(g.wm_base, &wm_listener, &g);
    g.surface = wl_compositor_create_surface(g.compositor);
    g.xdg_surface = xdg_wm_base_get_xdg_surface(g.wm_base, g.surface);
    xdg_surface_add_listener(g.xdg_surface, &surface_listener, &g);
    g.toplevel = xdg_surface_get_toplevel(g.xdg_surface);
    xdg_toplevel_add_listener(g.toplevel, &toplevel_listener, &g);
    xdg_toplevel_set_title(g.toplevel, g.opt.title);
    xdg_toplevel_set_app_id(g.toplevel, "org.xeondesktop.TestGrab");
    if (g.opt.inhibit) {
        struct zwp_keyboard_shortcuts_inhibitor_v1 *in =
            zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(g.inhibit, g.surface,
                                                                       g.seat);
        zwp_keyboard_shortcuts_inhibitor_v1_add_listener(in, &inhibitor_listener, &g);
    }
    wl_surface_commit(g.surface);
    constrain(&g); /* if the pointer came first */
    while (!g.closed && wl_display_dispatch(g.display) != -1) {
    }
    if (g.log != nullptr)
        fclose(g.log);
    wl_display_disconnect(g.display);
    return EXIT_SUCCESS;
}
