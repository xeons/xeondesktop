/* SPDX-License-Identifier: MIT
 * xd-shell: connects to the compositor, puts a taskbar on every output and
 * keeps it in sync with the window list and the clock.
 */
#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../ui/image.h"
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
    shell->pointer_menu = xd_menu_for_surface(shell, surface);
    shell->pointer_start7 = xd_start7_owns(shell, surface);
    if (shell->cursor_shape != nullptr)
        wp_cursor_shape_device_v1_set_shape(shell->cursor_shape, serial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    double lx = wl_fixed_to_double(x);
    double ly = wl_fixed_to_double(y);
    if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_motion(shell->pointer_bar, lx, ly);
    if (shell->pointer_menu != nullptr)
        xd_menu_pointer_motion(shell->pointer_menu, lx, ly);
    if (shell->pointer_start7)
        xd_start7_pointer_motion(shell, lx, ly);
}

static void pointer_leave(void *data, [[maybe_unused]] struct wl_pointer *p,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface) {
    xd_shell *shell = data;
    if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_leave(shell->pointer_bar);
    if (shell->pointer_menu != nullptr)
        xd_menu_pointer_leave(shell->pointer_menu);
    if (shell->pointer_start7)
        xd_start7_pointer_leave(shell);
    shell->pointer_bar = nullptr;
    shell->pointer_menu = nullptr;
    shell->pointer_start7 = false;
}

static void pointer_motion(void *data, [[maybe_unused]] struct wl_pointer *p,
                           [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    xd_shell *shell = data;
    double lx = wl_fixed_to_double(x);
    double ly = wl_fixed_to_double(y);
    if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_motion(shell->pointer_bar, lx, ly);
    if (shell->pointer_menu != nullptr)
        xd_menu_pointer_motion(shell->pointer_menu, lx, ly);
    if (shell->pointer_start7)
        xd_start7_pointer_motion(shell, lx, ly);
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *p, uint32_t serial,
                           [[maybe_unused]] uint32_t time, uint32_t button, uint32_t state) {
    xd_shell *shell = data;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (shell->pointer_menu != nullptr)
        xd_menu_pointer_button(shell->pointer_menu, button, pressed);
    else if (shell->pointer_start7)
        xd_start7_pointer_button(shell, button, pressed);
    else if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_button(shell->pointer_bar, button, pressed, serial);
}

static void pointer_axis(void *data, [[maybe_unused]] struct wl_pointer *p,
                         [[maybe_unused]] uint32_t time, uint32_t axis, wl_fixed_t value) {
    xd_shell *shell = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && shell->pointer_start7)
        xd_start7_pointer_axis(shell, wl_fixed_to_double(value));
}
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

/* ---- keyboard (menu navigation) --------------------------------------------- */

static void keyboard_keymap(void *data, [[maybe_unused]] struct wl_keyboard *k, uint32_t format,
                            int32_t fd, uint32_t size) {
    xd_shell *shell = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || size > (16u << 20)) {
        close(fd);
        return;
    }
    char *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;
    /* The keymap must be NUL-terminated within `size`; don't trust it. */
    struct xkb_keymap *keymap = nullptr;
    if (memchr(map, '\0', size) != nullptr)
        keymap = xkb_keymap_new_from_string(shell->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (keymap == nullptr)
        return;
    xkb_state_unref(shell->xkb_state);
    xkb_keymap_unref(shell->keymap);
    shell->keymap = keymap;
    shell->xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *k,
                           [[maybe_unused]] uint32_t serial,
                           [[maybe_unused]] struct wl_surface *surface,
                           [[maybe_unused]] struct wl_array *keys) {}

static void keyboard_leave([[maybe_unused]] void *data, [[maybe_unused]] struct wl_keyboard *k,
                           [[maybe_unused]] uint32_t serial,
                           [[maybe_unused]] struct wl_surface *surface) {}

static void keyboard_key(void *data, [[maybe_unused]] struct wl_keyboard *k,
                         [[maybe_unused]] uint32_t serial, [[maybe_unused]] uint32_t time,
                         uint32_t key, uint32_t state) {
    xd_shell *shell = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || shell->xkb_state == nullptr)
        return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(shell->xkb_state, key + 8);
    if (shell->start7 != nullptr) {
        char utf8[16] = {0};
        xkb_state_key_get_utf8(shell->xkb_state, key + 8, utf8, sizeof(utf8));
        bool ctrl_alt =
            xkb_state_mod_name_is_active(shell->xkb_state, XKB_MOD_NAME_CTRL,
                                         XKB_STATE_MODS_EFFECTIVE) > 0 ||
            xkb_state_mod_name_is_active(shell->xkb_state, XKB_MOD_NAME_ALT,
                                         XKB_STATE_MODS_EFFECTIVE) > 0;
        xd_start7_key(shell, sym, utf8, ctrl_alt);
        return;
    }
    xd_menu_key(shell, sym);
}

static void keyboard_modifiers(void *data, [[maybe_unused]] struct wl_keyboard *k,
                               [[maybe_unused]] uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group) {
    xd_shell *shell = data;
    if (shell->xkb_state != nullptr)
        xkb_state_update_mask(shell->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_keyboard *k,
                                 [[maybe_unused]] int32_t rate,
                                 [[maybe_unused]] int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void wm_base_ping([[maybe_unused]] void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    xd_shell *shell = data;
    bool has_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_keyboard && shell->keyboard == nullptr) {
        shell->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(shell->keyboard, &keyboard_listener, shell);
    } else if (!has_keyboard && shell->keyboard != nullptr) {
        wl_keyboard_release(shell->keyboard);
        shell->keyboard = nullptr;
    }
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
        shell->pointer_menu = nullptr;
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
    } else if (xd_str_eq(iface, xd_str_from_cstr(xdg_wm_base_interface.name))) {
        shell->wm_base = bind(reg, name, &xdg_wm_base_interface, version, 3);
        xdg_wm_base_add_listener(shell->wm_base, &wm_base_listener, shell);
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

/* ---- launching ----------------------------------------------------------------- */

void xd_shell_launch([[maybe_unused]] xd_shell *shell, const char *command) {
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0)
        return;
    /* Children start with default signal handling and an empty mask, in
     * their own session so closing the shell doesn't take them down. */
    sigset_t none;
    sigset_t all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                        POSIX_SPAWN_SETSID);
    char sh[] = "/bin/sh";
    char flag[] = "-c";
    char *cmd = xd_str_dup_cstr(xd_str_from_cstr(command));
    char *argv[] = {sh, flag, cmd, nullptr};
    pid_t pid;
    int err = posix_spawn(&pid, sh, nullptr, &attr, argv, environ);
    if (err != 0)
        fprintf(stderr, "xd-shell: cannot run '%s': %s\n", command, strerror(err));
    xd_free(cmd);
    posix_spawnattr_destroy(&attr);
}

void xd_shell_start_menu_closed(xd_shell *shell) {
    xd_taskbar *bar = shell->start_bar;
    shell->start_bar = nullptr;
    if (bar == nullptr)
        return;
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    bar->dirty = true;
}

/* ---- settings ------------------------------------------------------------------- */

static void load_wallpaper(xd_shell *shell) {
    if (shell->wallpaper != nullptr)
        cairo_surface_destroy(shell->wallpaper);
    /* Big enough for a 4K output without keeping a 5K image in memory. */
    shell->wallpaper = shell->settings.wallpaper[0] != '\0'
                           ? xd_image_load(shell->settings.wallpaper, 4096, 4096)
                           : nullptr;
}

/* Applies desktop.ini; command-line overrides only win at startup. */
static void apply_settings(xd_shell *shell, bool startup) {
    char old_wallpaper[sizeof(shell->settings.wallpaper)];
    memcpy(old_wallpaper, shell->settings.wallpaper, sizeof(old_wallpaper));
    xd_settings_load(&shell->settings);
    if (!startup) {
        shell->cli_style = false;
        shell->scheme_path = nullptr;
    }
    if (!shell->cli_style)
        shell->style = shell->settings.style;
    if (shell->scheme_path == nullptr)
        xd_settings_scheme(&shell->settings, &shell->scheme);
    if (startup || strcmp(old_wallpaper, shell->settings.wallpaper) != 0)
        load_wallpaper(shell);
    if (startup)
        return;

    /* Re-theme: menus are rebuilt (their commands carry overrides). */
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->menu_tree);
    xd_menu_tree_init(&shell->menu_tree);
    xd_start_menu_build(shell);
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        xd_taskbar_retheme(bar);
    }
}

/* XD_SHELL_OPEN_MENU="Programs/Accessories" opens the Start menu and that
 * submenu path at startup, one level per configured menu, without a grab.
 * For screenshots and testing without an input device. */
static void debug_open_menu(xd_shell *shell) {
    static const char *path;
    static bool started;
    if (!started) {
        path = getenv("XD_SHELL_OPEN_MENU");
        if (path == nullptr || wl_list_empty(&shell->taskbars))
            return;
        xd_taskbar *bar = XD_CONTAINER_OF(shell->taskbars.next, xd_taskbar, link);
        if (!bar->configured)
            return;
        started = true;
        xd_taskbar_open_start_menu(bar, 0);
        return;
    }
    if (path == nullptr || path[0] == '\0' || shell->start_menu == nullptr)
        return;
    xd_menu *menu = shell->start_menu;
    while (menu->child != nullptr)
        menu = menu->child;
    if (!menu->shown)
        return;

    xd_str rest = xd_str_from_cstr(path);
    size_t slash = rest.len;
    bool more = xd_str_find_char(rest, '/', &slash);
    xd_str label = xd_str_sub(rest, 0, slash);
    path = more ? path + slash + 1 : "";
    for (size_t i = 0; i < menu->model->count; i++) {
        if (xd_str_eq(xd_str_from_cstr(menu->model->items[i].label), label)) {
            menu->hot = (int)i;
            xd_menu *child = xd_menu_open_child(menu, (int)i);
            if (child != nullptr && path[0] == '\0')
                child->hot = 0;
            return;
        }
    }
    path = "";
}

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
            shell->cli_style = true;
            if (!xd_visual_style_from_name(xd_str_from_cstr(val), &shell->style)) {
                fprintf(stderr, "xd-shell: unknown style: %s\n", val);
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--scheme"))) {
            xd_strbuf err = {0};
            bool ok = xd_scheme_load(val, &shell->scheme, &err);
            shell->scheme_path = val;
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
        debug_open_menu(shell);
        xd_taskbars_render_dirty(shell);
        xd_menus_render_dirty(shell);
        xd_start7_render_dirty(shell);
        while (wl_display_prepare_read(d) != 0)
            wl_display_dispatch_pending(d);
        if (!flush_display(d)) {
            wl_display_cancel_read(d);
            return EXIT_FAILURE;
        }

        struct pollfd fds[6] = {
            {.fd = wl_display_get_fd(d), .events = POLLIN},
            {.fd = timer_fd, .events = POLLIN},
            {.fd = signal_fd, .events = POLLIN},
            {.fd = shell->menu_timer_fd, .events = POLLIN},
            {.fd = shell->settings_fd, .events = POLLIN},
            {.fd = shell->quicklaunch.watch_fd, .events = POLLIN},
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
        if (shell->quicklaunch.watch_fd >= 0 && (fds[5].revents & POLLIN) &&
            xd_quicklaunch_watch_changed(shell)) {
            xd_quicklaunch_reload(shell);
            xd_taskbars_forget_layout(shell);
        }
        if (shell->settings_fd >= 0 && (fds[4].revents & POLLIN) &&
            xd_settings_watch_changed(shell->settings_fd))
            apply_settings(shell, false);
        if (shell->menu_timer_fd >= 0 && (fds[3].revents & POLLIN)) {
            uint64_t expirations;
            (void)!read(shell->menu_timer_fd, &expirations, sizeof(expirations));
            xd_menu_timer_fired(shell);
        }
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    static xd_shell shell;
    shell.style = XD_STYLE_CLASSIC;
    const char *term = getenv("TERMINAL");
    shell.terminal = term != nullptr && term[0] != '\0' ? term : "alacritty";
    shell.menu_timer_fd = -1;
    shell.settings_fd = -1;
    shell.quicklaunch.watch_fd = -1;
    xd_scheme_init_default(&shell.scheme);
    wl_list_init(&shell.taskbars);
    wl_list_init(&shell.toplevels);
    if (!parse_args(argc, argv, &shell)) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    shell.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    shell.display = wl_display_connect(nullptr);
    if (shell.display == nullptr) {
        fprintf(stderr, "xd-shell: cannot connect to a Wayland compositor\n");
        return EXIT_FAILURE;
    }
    shell.registry = wl_display_get_registry(shell.display);
    wl_registry_add_listener(shell.registry, &registry_listener, &shell);
    wl_display_roundtrip(shell.display);

    if (shell.compositor == nullptr || shell.shm == nullptr || shell.layer_shell == nullptr ||
        shell.wm_base == nullptr) {
        fprintf(stderr, "xd-shell: compositor lacks wl_shm, xdg-shell or wlr-layer-shell\n");
        return EXIT_FAILURE;
    }

    /* Launched apps are reaped automatically. */
    signal(SIGCHLD, SIG_IGN);
    const char *icon_theme = getenv("XD_ICON_THEME");
    shell.icons = xd_icons_create(icon_theme != nullptr ? icon_theme : "breeze");
    apply_settings(&shell, true);
    shell.settings_fd = xd_settings_watch();
    xd_quicklaunch_init(&shell);
    xd_apps_load(&shell.apps, "XeonDesktop");
    xd_menu_tree_init(&shell.menu_tree);
    xd_start_menu_build(&shell);
    shell.menu_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

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

    xd_menu_close_all(&shell);

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
    xd_menu_tree_free(&shell.menu_tree);
    xd_apps_free(&shell.apps);
    xd_quicklaunch_finish(&shell);
    xd_free(shell.desktop_hidden);
    xd_icons_destroy(shell.icons);
    if (shell.keyboard != nullptr)
        wl_keyboard_release(shell.keyboard);
    xkb_state_unref(shell.xkb_state);
    xkb_keymap_unref(shell.keymap);
    xkb_context_unref(shell.xkb);
    if (shell.menu_timer_fd >= 0)
        close(shell.menu_timer_fd);
    if (shell.settings_fd >= 0)
        close(shell.settings_fd);
    if (shell.wallpaper != nullptr)
        cairo_surface_destroy(shell.wallpaper);
    if (timer_fd >= 0)
        close(timer_fd);
    if (signal_fd >= 0)
        close(signal_fd);
    wl_display_disconnect(shell.display);
    return rc;
}
