/* SPDX-License-Identifier: MIT
 * xd-shell: connects to the compositor, puts a taskbar on every output and
 * keeps it in sync with the window list and the clock.
 */
#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "shell.h"

/* ---- clock ------------------------------------------------------------------ */

/* Updates shell->clock; returns true if the text changed. */
static bool update_clock(xd_shell *shell) {
    time_t now = time(nullptr);
    struct tm tm;
    if (localtime_r(&now, &tm) == nullptr)
        return false;
    char buf[sizeof(shell->clock)];
    /* Windows' short time format: "7:08 PM", or 24-hour where the locale
     * has no AM/PM. */
    char ampm[8];
    size_t has_ampm = strftime(ampm, sizeof(ampm), "%p", &tm);
    size_t n = strftime(buf, sizeof(buf), has_ampm > 0 ? "%I:%M %p" : "%H:%M", &tm);
    if (n == 0)
        return false;
    /* "07:08 PM" -> "7:08 PM" */
    const char *text = (has_ampm > 0 && buf[0] == '0') ? buf + 1 : buf;
    if (strcmp(text, shell->clock) == 0)
        return false;
    snprintf(shell->clock, sizeof(shell->clock), "%s", text);
    return true;
}

/* A timer that fires on every minute boundary. */
static int clock_timer_create(void) {
    int fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0)
        return -1;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct itimerspec spec = {
        .it_interval = {.tv_sec = 60},
        .it_value = {.tv_sec = now.tv_sec - now.tv_sec % 60 + 60},
    };
    /* Re-arms itself if the wall clock jumps, e.g. after suspend. */
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &spec, nullptr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- pointer --------------------------------------------------------------------- */

static xd_taskbar *bar_for_surface(xd_shell *shell, struct wl_surface *surface) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        if (bar->surface == surface)
            return bar;
    }
    return nullptr;
}

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    xd_shell *shell = data;
    shell->pointer_bar = bar_for_surface(shell, surface);
    if (shell->cursor_shape != nullptr)
        wp_cursor_shape_device_v1_set_shape(shell->cursor_shape, serial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_motion(shell->pointer_bar, wl_fixed_to_double(x),
                                  wl_fixed_to_double(y));
}

static void pointer_leave(void *data, [[maybe_unused]] struct wl_pointer *p,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface) {
    xd_shell *shell = data;
    if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_leave(shell->pointer_bar);
    shell->pointer_bar = nullptr;
}

static void pointer_motion(void *data, [[maybe_unused]] struct wl_pointer *p,
                           [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    xd_shell *shell = data;
    if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_motion(shell->pointer_bar, wl_fixed_to_double(x),
                                  wl_fixed_to_double(y));
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *p,
                           [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                           uint32_t button, uint32_t state) {
    xd_shell *shell = data;
    if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_button(shell->pointer_bar, button,
                                  state == WL_POINTER_BUTTON_STATE_PRESSED);
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

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    xd_shell *shell = data;
    bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && shell->pointer == nullptr) {
        shell->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(shell->pointer, &pointer_listener, shell);
        if (shell->cursor_shape_manager != nullptr)
            shell->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(
                shell->cursor_shape_manager, shell->pointer);
    } else if (!has_pointer && shell->pointer != nullptr) {
        if (shell->cursor_shape != nullptr)
            wp_cursor_shape_device_v1_destroy(shell->cursor_shape);
        shell->cursor_shape = nullptr;
        wl_pointer_release(shell->pointer);
        shell->pointer = nullptr;
        shell->pointer_bar = nullptr;
    }
}

static void seat_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_seat *seat,
                      [[maybe_unused]] const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- globals ----------------------------------------------------------------------- */

static void *bind(struct wl_registry *reg, uint32_t name, const struct wl_interface *iface,
                  uint32_t have, uint32_t want) {
    return wl_registry_bind(reg, name, iface, XD_MIN(have, want));
}

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
    xd_shell *shell = data;
    xd_str iface = xd_str_from_cstr(interface);

    if (xd_str_eq(iface, xd_str_from_cstr(wl_compositor_interface.name))) {
        shell->compositor = bind(reg, name, &wl_compositor_interface, version, 4);
    } else if (xd_str_eq(iface, xd_str_from_cstr(wl_shm_interface.name))) {
        shell->shm = bind(reg, name, &wl_shm_interface, version, 1);
    } else if (xd_str_eq(iface, xd_str_from_cstr(wl_seat_interface.name)) &&
               shell->seat == nullptr) {
        shell->seat = bind(reg, name, &wl_seat_interface, version, 5);
        wl_seat_add_listener(shell->seat, &seat_listener, shell);
    } else if (xd_str_eq(iface, xd_str_from_cstr(wl_output_interface.name))) {
        struct wl_output *output = bind(reg, name, &wl_output_interface, version, 3);
        /* Globals arrive in any order; outputs seen before the layer shell
         * get their taskbar after the first roundtrip (see main). */
        if (shell->started)
            xd_taskbar_create(shell, output, name);
        else if (shell->early_count < XD_ARRAY_LEN(shell->early_outputs))
            shell->early_outputs[shell->early_count++] = (xd_early_output){output, name};
        else
            wl_output_release(output);
    } else if (xd_str_eq(iface, xd_str_from_cstr(zwlr_layer_shell_v1_interface.name))) {
        shell->layer_shell = bind(reg, name, &zwlr_layer_shell_v1_interface, version, 4);
    } else if (xd_str_eq(iface,
                         xd_str_from_cstr(zwlr_foreign_toplevel_manager_v1_interface.name))) {
        shell->toplevel_manager =
            bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface, version, 3);
    } else if (xd_str_eq(iface,
                         xd_str_from_cstr(wp_fractional_scale_manager_v1_interface.name))) {
        shell->fractional_manager =
            bind(reg, name, &wp_fractional_scale_manager_v1_interface, version, 1);
    } else if (xd_str_eq(iface, xd_str_from_cstr(wp_viewporter_interface.name))) {
        shell->viewporter = bind(reg, name, &wp_viewporter_interface, version, 1);
    } else if (xd_str_eq(iface, xd_str_from_cstr(wp_cursor_shape_manager_v1_interface.name))) {
        shell->cursor_shape_manager =
            bind(reg, name, &wp_cursor_shape_manager_v1_interface, version, 1);
    }
}

static void registry_global_remove(void *data, [[maybe_unused]] struct wl_registry *reg,
                                   uint32_t name) {
    xd_shell *shell = data;
    XD_LIST_FOR_EACH_SAFE(bar, tmp, &shell->taskbars, xd_taskbar, link) {
        if (bar->output_global == name)
            xd_taskbar_destroy(bar);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- main loop ------------------------------------------------------------------- */

static void usage(FILE *f) {
    fprintf(f, "usage: xd-shell [--style classic|luna|whistler] [--scheme FILE]\n");
}

static bool parse_args(int argc, char **argv, xd_shell *shell) {
    for (int i = 1; i < argc; i++) {
        xd_str arg = xd_str_from_cstr(argv[i]);
        if (xd_str_eq(arg, XD_STR("-h")) || xd_str_eq(arg, XD_STR("--help"))) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "xd-shell: missing value for %s\n", argv[i]);
            return false;
        }
        const char *val = argv[++i];
        if (xd_str_eq(arg, XD_STR("--style"))) {
            if (!xd_visual_style_from_name(xd_str_from_cstr(val), &shell->style)) {
                fprintf(stderr, "xd-shell: unknown style: %s\n", val);
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--scheme"))) {
            xd_strbuf err = {0};
            bool ok = xd_scheme_load(val, &shell->scheme, &err);
            if (!ok)
                fprintf(stderr, "xd-shell: %s: %s\n", val, xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
            if (!ok)
                return false;
        } else {
            fprintf(stderr, "xd-shell: unknown option: %s\n", argv[i - 1]);
            return false;
        }
    }
    return true;
}

/* Flushes requests, waiting for the socket if the compositor is slow. */
static bool flush_display(struct wl_display *display) {
    while (wl_display_flush(display) < 0) {
        if (errno != EAGAIN)
            return false;
        struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLOUT};
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
            return false;
    }
    return true;
}

static int run(xd_shell *shell, int timer_fd, int signal_fd) {
    struct wl_display *d = shell->display;
    while (shell->running) {
        xd_taskbars_render_dirty(shell);
        while (wl_display_prepare_read(d) != 0)
            wl_display_dispatch_pending(d);
        if (!flush_display(d)) {
            wl_display_cancel_read(d);
            return EXIT_FAILURE;
        }

        struct pollfd fds[3] = {
            {.fd = wl_display_get_fd(d), .events = POLLIN},
            {.fd = timer_fd, .events = POLLIN},
            {.fd = signal_fd, .events = POLLIN},
        };
        if (poll(fds, XD_ARRAY_LEN(fds), -1) < 0) {
            wl_display_cancel_read(d);
            if (errno == EINTR)
                continue;
            return EXIT_FAILURE;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(d) < 0)
                return EXIT_FAILURE;
        } else {
            wl_display_cancel_read(d);
        }
        if (fds[0].revents & (POLLERR | POLLHUP))
            return EXIT_FAILURE; /* compositor went away */
        if (wl_display_dispatch_pending(d) < 0)
            return EXIT_FAILURE;

        if (timer_fd >= 0 && (fds[1].revents & POLLIN)) {
            uint64_t expirations;
            /* ECANCELED means the wall clock jumped; just re-read it. */
            (void)!read(timer_fd, &expirations, sizeof(expirations));
            if (update_clock(shell))
                xd_taskbars_mark_dirty(shell);
        }
        if (signal_fd >= 0 && (fds[2].revents & POLLIN))
            shell->running = false;
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    static xd_shell shell;
    shell.style = XD_STYLE_CLASSIC;
    xd_scheme_init_default(&shell.scheme);
    wl_list_init(&shell.taskbars);
    wl_list_init(&shell.toplevels);
    if (!parse_args(argc, argv, &shell)) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    shell.display = wl_display_connect(nullptr);
    if (shell.display == nullptr) {
        fprintf(stderr, "xd-shell: cannot connect to a Wayland compositor\n");
        return EXIT_FAILURE;
    }
    shell.registry = wl_display_get_registry(shell.display);
    wl_registry_add_listener(shell.registry, &registry_listener, &shell);
    wl_display_roundtrip(shell.display);

    if (shell.compositor == nullptr || shell.shm == nullptr || shell.layer_shell == nullptr) {
        fprintf(stderr, "xd-shell: compositor lacks wl_shm or wlr-layer-shell\n");
        return EXIT_FAILURE;
    }

    update_clock(&shell);
    xd_toplevels_init(&shell);
    /* From here on, new outputs get a taskbar as soon as they appear. */
    shell.started = true;
    for (size_t i = 0; i < shell.early_count; i++)
        xd_taskbar_create(&shell, shell.early_outputs[i].output, shell.early_outputs[i].name);
    shell.early_count = 0;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    int timer_fd = clock_timer_create();

    shell.running = true;
    int rc = run(&shell, timer_fd, signal_fd);

    XD_LIST_FOR_EACH_SAFE(bar, tmp, &shell.taskbars, xd_taskbar, link) {
        xd_taskbar_destroy(bar);
    }
    XD_LIST_FOR_EACH_SAFE(t, tmp, &shell.toplevels, xd_toplevel, link) {
        zwlr_foreign_toplevel_handle_v1_destroy(t->handle);
        XD_FREE(&t->title);
        XD_FREE(&t->app_id);
        wl_list_remove(&t->link);
        xd_free(t);
    }
    if (timer_fd >= 0)
        close(timer_fd);
    if (signal_fd >= 0)
        close(signal_fd);
    wl_display_disconnect(shell.display);
    return rc;
}
