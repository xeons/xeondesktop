/* SPDX-License-Identifier: MIT
 * xd-shell: connects to the compositor, puts a taskbar on every output and
 * keeps it in sync with the window list and the clock.
 */
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
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
#include "../settings/soundplay.h"
#include "../settings/themes.h"
#include "../ui/image.h"
#include "gpu.h"
#include "shell.h"

/* ---- clock ------------------------------------------------------------------ */

/* Updates shell->clock; returns true if the text changed.
 * XD_CLOCK_FIXED=TEXT shows TEXT instead and never ticks, for the UI
 * tests, whose screenshots would otherwise differ at each new minute. */
static bool update_clock(xd_shell *shell) {
    const char *fixed = getenv("XD_CLOCK_FIXED");
    if (fixed != nullptr && fixed[0] != '\0') {
        if (strcmp(fixed, shell->clock) == 0)
            return false;
        snprintf(shell->clock, sizeof(shell->clock), "%s", fixed);
        return true;
    }
    /* The precise clock: time() reads a coarse one that can still say :59
     * when the minute timer fires, leaving the clock a minute behind. Half
     * a second's margin for timer slack the other way. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t now = ts.tv_sec + (ts.tv_nsec >= 500000000 ? 1 : 0);
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

xd_taskbar *xd_taskbar_for_surface(xd_shell *shell, struct wl_surface *surface) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        if (bar->surface == surface)
            return bar;
    }
    return nullptr;
}

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    xd_shell *shell = data;
    shell->pointer_bar = xd_taskbar_for_surface(shell, surface);
    shell->pointer_menu = xd_menu_for_surface(shell, surface);
    shell->pointer_start7 = xd_start7_owns(shell, surface);
    shell->pointer_flyout = xd_volume_flyout_owns(shell, surface);
    shell->pointer_desktop = xd_desktop_for_surface(shell, surface);
    shell->pointer_thumbs = xd_thumbs_owns(shell, surface);
    shell->pointer_balloon = xd_balloon_owns(shell, surface);
    shell->enter_serial = serial;
    shell->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (shell->cursor_shape != nullptr)
        wp_cursor_shape_device_v1_set_shape(shell->cursor_shape, serial,
                                            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    double lx = wl_fixed_to_double(x);
    double ly = wl_fixed_to_double(y);
    if (shell->pointer_bar != nullptr) {
        xd_taskbar_reveal(shell->pointer_bar); /* Auto-hide */
        xd_taskbar_pointer_motion(shell->pointer_bar, lx, ly);
    }
    if (shell->pointer_menu != nullptr)
        xd_menu_pointer_motion(shell->pointer_menu, lx, ly);
    if (shell->pointer_start7)
        xd_start7_pointer_motion(shell, lx, ly);
    if (shell->pointer_flyout) {
        shell->flyout_x = lx;
        shell->flyout_y = ly;
    }
    if (shell->pointer_desktop != nullptr)
        xd_deskicons_motion(shell, shell->pointer_desktop, lx, ly);
    if (shell->pointer_thumbs)
        xd_thumbs_pointer_motion(shell, lx, ly);
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
    if (shell->pointer_thumbs)
        xd_thumbs_pointer_leave(shell);
    shell->pointer_thumbs = false;
    shell->pointer_balloon = false;
    shell->pointer_bar = nullptr;
    shell->pointer_menu = nullptr;
    shell->pointer_start7 = false;
    shell->pointer_flyout = false;
    shell->pointer_desktop = nullptr;
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
    if (shell->pointer_flyout) {
        shell->flyout_x = lx;
        shell->flyout_y = ly;
        xd_volume_flyout_motion(shell, lx, ly);
    }
    if (shell->pointer_desktop != nullptr)
        xd_deskicons_motion(shell, shell->pointer_desktop, lx, ly);
    if (shell->pointer_thumbs)
        xd_thumbs_pointer_motion(shell, lx, ly);
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    xd_shell *shell = data;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (pressed)
        shell->button_serial = serial;
    /* The compositor only dismisses the flyout's grab for clicks in other
     * programs; the desktop and taskbar are ours. */
    if (pressed && !shell->pointer_flyout)
        xd_volume_flyout_dismiss(shell);
    /* Nor menus for clicks on the desktop: a click there closes the Start
     * menu or a context menu, and (as in Windows) does nothing else. */
    if (pressed && shell->pointer_desktop != nullptr &&
        (shell->start_menu != nullptr || shell->start7 != nullptr)) {
        xd_menu_close_all(shell);
        shell->eat_release = true;
        return;
    }
    if (!pressed && shell->eat_release) {
        shell->eat_release = false;
        return;
    }
    if (shell->pointer_balloon)
        xd_balloon_button(shell, pressed);
    else if (shell->pointer_thumbs)
        xd_thumbs_pointer_button(shell, button, pressed);
    else if (shell->pointer_flyout)
        xd_volume_flyout_button(shell, button, pressed, shell->flyout_x, shell->flyout_y);
    else if (shell->pointer_menu != nullptr)
        xd_menu_pointer_button(shell->pointer_menu, button, pressed);
    else if (shell->pointer_start7)
        xd_start7_pointer_button(shell, button, pressed);
    else if (shell->pointer_bar != nullptr)
        xd_taskbar_pointer_button(shell->pointer_bar, button, pressed, serial);
    else if (shell->pointer_desktop != nullptr)
        xd_deskicons_button(shell, shell->pointer_desktop, button, pressed, time, serial);
}

static void pointer_axis(void *data, [[maybe_unused]] struct wl_pointer *p,
                         [[maybe_unused]] uint32_t time, uint32_t axis, wl_fixed_t value) {
    xd_shell *shell = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && shell->pointer_start7) {
        xd_start7_pointer_axis(shell, wl_fixed_to_double(value));
    } else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && shell->pointer_bar != nullptr) {
        /* The wheel pages through task buttons; one notch (10) a page,
         * touchpads' small steps added up. */
        xd_taskbar *bar = shell->pointer_bar;
        bar->wheel += wl_fixed_to_double(value);
        if (bar->wheel <= -10 || bar->wheel >= 10) {
            int step = bar->wheel < 0 ? -1 : 1;
            bar->wheel = 0;
            /* Over a notification area icon: its own (the volume). */
            size_t slot = (size_t)(XD_HIT_TRAY_ICON - bar->hot);
            xd_tray_item *item = bar->hot <= XD_HIT_TRAY_ICON && slot < bar->tray_count
                                     ? xd_tray_get(shell, bar->tray_index[slot])
                                     : nullptr;
            if (item != nullptr)
                xd_tray_item_scroll(shell, item, step);
            else
                xd_taskbar_page(bar, step);
        }
    }
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

static void repeat_arm(xd_shell *shell, int first_ms, int every_ms) {
    if (shell->repeat_timer_fd < 0)
        return;
    struct itimerspec spec = {
        .it_value = {first_ms / 1000, (long)(first_ms % 1000) * 1000000L},
        .it_interval = {every_ms / 1000, (long)(every_ms % 1000) * 1000000L},
    };
    timerfd_settime(shell->repeat_timer_fd, 0, &spec, nullptr);
}

static void repeat_stop(xd_shell *shell) {
    shell->repeat_key = 0;
    repeat_arm(shell, 0, 0);
}

static void keyboard_enter(void *data, [[maybe_unused]] struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *surface, [[maybe_unused]] struct wl_array *keys) {
    xd_shell *shell = data;
    shell->kb_surface = surface;
    shell->kb_serial = serial;
    xd_deskicons_relayout(shell); /* the focus rectangle */
    xd_shellbus_keyboard_entered(shell, surface, serial);
}

static void keyboard_leave(void *data, [[maybe_unused]] struct wl_keyboard *k,
                           [[maybe_unused]] uint32_t serial, struct wl_surface *surface) {
    xd_shell *shell = data;
    repeat_stop(shell);
    if (shell->kb_surface == surface)
        shell->kb_surface = nullptr;
    xd_deskicons_relayout(shell);
}

static void handle_key(xd_shell *shell, uint32_t key, uint32_t serial) {
    xkb_keysym_t sym = xkb_state_key_get_one_sym(shell->xkb_state, key + 8);
    /* The keyboard used in a menu: it underlines its access keys from now
     * on, as Windows' menus do when they hide them until then. */
    if (shell->start7 != nullptr || shell->start_menu != nullptr)
        xd_menu_show_cues(shell);
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
    if (xd_volume_flyout_key(shell, sym))
        return;
    /* An open menu first: its popup's keys arrive at the surface it hangs
     * off, and the desktop's right-click menu hangs off the desktop. */
    if (shell->start_menu != nullptr) {
        xd_menu_key(shell, sym);
        return;
    }
    /* The desktop's icons, when it has the keyboard. */
    if (shell->kb_surface != nullptr && xd_desktop_for_surface(shell, shell->kb_surface) != nullptr) {
        char utf8[16] = {0};
        xkb_state_key_get_utf8(shell->xkb_state, key + 8, utf8, sizeof(utf8));
        if (xd_deskicons_key(shell, sym, utf8, serial))
            return;
    }
    xd_menu_key(shell, sym);
}

static void keyboard_key(void *data, [[maybe_unused]] struct wl_keyboard *k, uint32_t serial,
                         [[maybe_unused]] uint32_t time, uint32_t key, uint32_t state) {
    xd_shell *shell = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (key + 1 == shell->repeat_key)
            repeat_stop(shell);
        return;
    }
    if (shell->xkb_state == nullptr)
        return;
    handle_key(shell, key, serial);
    /* Held keys repeat, as the keymap says (not modifiers). */
    if (shell->repeat_rate > 0 && shell->keymap != nullptr &&
        xkb_keymap_key_repeats(shell->keymap, key + 8)) {
        shell->repeat_key = key + 1;
        shell->repeat_serial = serial;
        repeat_arm(shell, shell->repeat_delay, XD_MAX(1000 / shell->repeat_rate, 1));
    } else {
        repeat_stop(shell);
    }
}

static void repeat_fired(xd_shell *shell) {
    if (shell->repeat_key != 0 && shell->xkb_state != nullptr)
        handle_key(shell, shell->repeat_key - 1, shell->repeat_serial);
}

static void keyboard_modifiers(void *data, [[maybe_unused]] struct wl_keyboard *k,
                               [[maybe_unused]] uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group) {
    xd_shell *shell = data;
    if (shell->xkb_state != nullptr)
        xkb_state_update_mask(shell->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, [[maybe_unused]] struct wl_keyboard *k, int32_t rate,
                                 int32_t delay) {
    xd_shell *shell = data;
    shell->repeat_rate = XD_CLAMP(rate, 0, 1000);
    shell->repeat_delay = XD_CLAMP(delay, 1, 10000);
}

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
        if (shell->started) {
            xd_taskbar_create(shell, output, name);
            xd_primary_track(shell, output);
        }
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
    } else if (xd_str_eq(iface, xd_str_from_cstr(xd_window_arranger_v1_interface.name))) {
        shell->arranger = bind(reg, name, &xd_window_arranger_v1_interface, version, 10);
        xd_taskmenu_bind(shell);
    } else if (xd_str_eq(iface, xd_str_from_cstr(zxdg_output_manager_v1_interface.name))) {
        xd_primary_bind(shell, reg, name, version);
    } else if (xd_str_eq(iface, xd_str_from_cstr(ext_idle_notifier_v1_interface.name))) {
        shell->idle_notifier = bind(reg, name, &ext_idle_notifier_v1_interface, version, 1);
    } else if (xd_str_eq(iface,
                         xd_str_from_cstr(zwlr_output_power_manager_v1_interface.name))) {
        shell->power_manager =
            bind(reg, name, &zwlr_output_power_manager_v1_interface, version, 1);
    } else if (xd_str_eq(iface,
                         xd_str_from_cstr(wp_fractional_scale_manager_v1_interface.name))) {
        shell->fractional_manager =
            bind(reg, name, &wp_fractional_scale_manager_v1_interface, version, 1);
    } else if (xd_str_eq(iface, xd_str_from_cstr(xdg_wm_base_interface.name))) {
        shell->wm_base = bind(reg, name, &xdg_wm_base_interface, version, 3);
        xdg_wm_base_add_listener(shell->wm_base, &wm_base_listener, shell);
    } else if (xd_str_eq(iface, xd_str_from_cstr(wl_data_device_manager_interface.name))) {
        shell->data_device_manager = bind(reg, name, &wl_data_device_manager_interface, version, 3);
    } else if (xd_str_eq(iface, xd_str_from_cstr(xdg_activation_v1_interface.name))) {
        shell->activation = bind(reg, name, &xdg_activation_v1_interface, version, 1);
    } else if (xd_str_eq(iface, xd_str_from_cstr(wp_viewporter_interface.name))) {
        shell->viewporter = bind(reg, name, &wp_viewporter_interface, version, 1);
    } else if (xd_str_eq(iface, xd_str_from_cstr(wp_cursor_shape_manager_v1_interface.name))) {
        shell->cursor_shape_manager =
            bind(reg, name, &wp_cursor_shape_manager_v1_interface, version, 1);
    } else {
        xd_thumbs_bind(shell, reg, name, interface, version);
    }
}

static void registry_global_remove(void *data, [[maybe_unused]] struct wl_registry *reg,
                                   uint32_t name) {
    xd_shell *shell = data;
    XD_LIST_FOR_EACH_SAFE(bar, tmp, &shell->taskbars, xd_taskbar, link) {
        if (bar->output_global == name) {
            xd_primary_forget(shell, bar->output);
            xd_taskbar_destroy(bar);
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- launching ----------------------------------------------------------------- */

static void token_done(void *data, [[maybe_unused]] struct xdg_activation_token_v1 *token,
                       const char *name) {
    *(char **)data = g_strdup(name);
}

static const struct xdg_activation_token_v1_listener token_listener = {.done = token_done};

/* An xdg-activation token from the shell, so the program (or the running
 * instance it hands over to) may bring its window forward. nullptr if
 * there's none. g_free the result. */
static char *activation_token(xd_shell *shell) {
    if (shell->activation == nullptr || wl_list_empty(&shell->taskbars))
        return nullptr;
    xd_taskbar *bar = XD_CONTAINER_OF(shell->taskbars.next, xd_taskbar, link);
    /* A private queue, so waiting for it dispatches nothing else. */
    struct wl_event_queue *queue = wl_display_create_queue(shell->display);
    struct xdg_activation_v1 *wrapper = wl_proxy_create_wrapper(shell->activation);
    wl_proxy_set_queue((struct wl_proxy *)wrapper, queue);
    struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(wrapper);
    char *name = nullptr;
    xdg_activation_token_v1_add_listener(token, &token_listener, &name);
    xdg_activation_token_v1_set_surface(token, bar->surface);
    xdg_activation_token_v1_commit(token);
    while (name == nullptr && wl_display_dispatch_queue(shell->display, queue) >= 0) {
    }
    xdg_activation_token_v1_destroy(token);
    wl_proxy_wrapper_destroy(wrapper);
    wl_event_queue_destroy(queue);
    return name;
}

int xd_shell_screen_height(const xd_shell *shell) {
    int h = 0;
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        if (bar->desktop != nullptr && bar->desktop->configured)
            h = XD_MAX(h, bar->desktop->height);
    }
    return h > 0 ? h : 768;
}

pid_t xd_shell_launch(xd_shell *shell, const char *command) {
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0)
        return 0;
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
    char **env = g_get_environ();
    char *token = activation_token(shell);
    if (token != nullptr) {
        env = g_environ_setenv(env, "XDG_ACTIVATION_TOKEN", token, true);
        env = g_environ_setenv(env, "DESKTOP_STARTUP_ID", token, true);
    }
    pid_t pid = 0;
    int err = posix_spawn(&pid, sh, nullptr, &attr, argv, env);
    g_strfreev(env);
    g_free(token);
    if (err != 0) {
        fprintf(stderr, "xd-shell: cannot run '%s': %s\n", command, strerror(err));
        pid = 0;
    }
    xd_free(cmd);
    posix_spawnattr_destroy(&attr);
    return pid;
}

void xd_shell_start_menu_closed(xd_shell *shell) {
    xd_taskbar *bar = shell->start_bar;
    shell->start_bar = nullptr;
    shell->context_menu = false;
    shell->menu_cues = false;
    xd_start_menu_reload_later(shell); /* programs installed while it was open */
    if (bar == nullptr)
        return;
    if (bar->layer != nullptr)
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    bar->dirty = true;
    xd_taskbars_schedule_tuck(shell);
}

/* ---- settings ------------------------------------------------------------------- */

static void load_wallpaper(xd_shell *shell) {
    if (shell->wallpaper != nullptr)
        cairo_surface_destroy(shell->wallpaper);
    /* Big enough for a 4K output without keeping a 5K image in memory. */
    shell->wallpaper = shell->settings.wallpaper[0] != '\0' && !shell->taskbar_only
                           ? xd_image_load(shell->settings.wallpaper, 4096, 4096)
                           : nullptr;
}

/* GTK3 on Wayland, and with it Qt's GTK platform theme, takes the icon
 * theme from GSettings (org.gnome.desktop.interface), never from our
 * settings: in a real session the icon theme in use goes there too, as
 * Plasma does with its own. Not in a nested or test session, whose
 * GSettings are the host desktop's, nor under another compositor, whose
 * desktop they belong to. GTK_THEME covers the GTK theme. */
static void export_icon_theme(const xd_shell *shell, const xd_settings *s) {
    const char *nested = getenv("XEONDESKTOP_NESTED");
    if (shell->standalone || nested == nullptr || strcmp(nested, "0") != 0)
        return;
    const char *theme = xd_settings_icon_theme(s);
    char **dirs = xd_icon_theme_dirs();
    bool installed = false;
    for (char **d = dirs; *d != nullptr && !installed; d++) {
        char *index = g_build_filename(*d, theme, "index.theme", nullptr);
        installed = g_file_test(index, G_FILE_TEST_IS_REGULAR);
        g_free(index);
    }
    g_strfreev(dirs);
    if (installed)
        xd_gtk_settings_export(g_get_user_config_dir(), nullptr, nullptr, theme);
}

/* Applies desktop.ini; command-line overrides only win at startup. */
static void apply_settings(xd_shell *shell, bool startup) {
    char old_wallpaper[sizeof(shell->settings.wallpaper)];
    memcpy(old_wallpaper, shell->settings.wallpaper, sizeof(old_wallpaper));
    char old_icons[sizeof(shell->settings.icon_theme)];
    memcpy(old_icons, shell->settings.icon_theme, sizeof(old_icons));
    xd_font_smoothing old_smoothing = shell->settings.font_smoothing;
    xd_settings_load(&shell->settings);
    /* A new icon theme: everything is drawn from the cache afresh below
     * (nothing keeps its surfaces between draws). */
    if (startup || strcmp(old_icons, shell->settings.icon_theme) != 0) {
        xd_icons_destroy(shell->icons);
        shell->icons = xd_icons_create(xd_settings_icon_theme(&shell->settings));
        export_icon_theme(shell, &shell->settings);
    }
    /* Font smoothing for other programs too, under the same rule. */
    if (startup || memcmp(&old_smoothing, &shell->settings.font_smoothing,
                          sizeof(old_smoothing)) != 0) {
        const char *nested = getenv("XEONDESKTOP_NESTED");
        if (!shell->standalone && nested != nullptr && strcmp(nested, "0") == 0)
            xd_font_settings_export(g_get_user_config_dir(), nullptr,
                                    &shell->settings.font_smoothing);
    }
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
    shell->background_gen++; /* the colour or wallpaper mode may have changed */
    xd_idle_update(shell);
    xd_sleep_update(shell);
    xd_buttons_update(shell);
    /* Programs started from the shell get Mouse Properties' cursors (the
     * compositor sets these too, but only for what it starts). */
    const xd_mouse_settings *m = &shell->settings.mouse;
    if (m->cursor_theme[0] != '\0')
        setenv("XCURSOR_THEME", m->cursor_theme, true);
    else
        unsetenv("XCURSOR_THEME");
    char cursor_size[16];
    snprintf(cursor_size, sizeof(cursor_size), "%d", m->cursor_size);
    setenv("XCURSOR_SIZE", cursor_size, true);
    /* And Display Properties' GTK theme, as the compositor gives it. */
    setenv("GTK_THEME", xd_settings_gtk_theme(&shell->settings), true);
    /* And Qt's style; Qt programs already running keep theirs. */
    xd_settings_qt_env(&shell->settings);
    if (startup)
        return;
    xd_tray_changed(shell); /* the volume and power icons may come or go */
    xd_deskicons_settings_changed(shell);

    /* Re-theme: menus are rebuilt (their commands carry overrides). */
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->menu_tree);
    xd_menu_tree_init(&shell->menu_tree);
    xd_start_menu_build(shell);
    xd_taskbars_update(shell);
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
    fprintf(f, "usage: xd-shell [--style classic|luna|whistler] [--scheme FILE] [--taskbar-only]\n"
               "       xd-shell --toggle-start-menu\n"
               "\n"
               "  --taskbar-only       taskbars, Start menu and notification area, no desktop\n"
               "                       (wallpaper, icons): for another compositor's session\n"
               "  --toggle-start-menu  open or close the running shell's Start menu (for a\n"
               "                       compositor's key binding)\n");
}

static bool parse_args(int argc, char **argv, xd_shell *shell) {
    for (int i = 1; i < argc; i++) {
        xd_str arg = xd_str_from_cstr(argv[i]);
        if (xd_str_eq(arg, XD_STR("-h")) || xd_str_eq(arg, XD_STR("--help"))) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        }
        if (xd_str_eq(arg, XD_STR("--toggle-start-menu")))
            exit(xd_shellbus_call_toggle());
        if (xd_str_eq(arg, XD_STR("--taskbar-only"))) {
            shell->taskbar_only = true;
            continue;
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

/* The connection to the compositor ended. A protocol error is ours: a
 * failure, which xd-compositor or systemd restarts us after; the
 * compositor going away ends the session, and the shell cleanly. */
static int display_lost(struct wl_display *display) {
    if (wl_display_get_error(display) == EPROTO) {
        fprintf(stderr, "xd-shell: Wayland protocol error\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "xd-shell: the compositor went away\n");
    return EXIT_SUCCESS;
}

/* Our own descriptors, then GLib's (D-Bus for the notification area),
 * polled together: GLib's main context runs inside our loop. */
enum { OWN_FDS = 10 };

static int run(xd_shell *shell, int timer_fd, int signal_fd) {
    struct wl_display *d = shell->display;
    GMainContext *ctx = g_main_context_default();
    if (!g_main_context_acquire(ctx))
        return EXIT_FAILURE;
    GPollFD *gfds = nullptr;
    struct pollfd *fds = nullptr;
    gint gcap = 0;
    int rc = EXIT_SUCCESS;
    while (shell->running) {
        debug_open_menu(shell);
        xd_taskbars_render_dirty(shell);
        xd_menus_render_dirty(shell);
        xd_start7_render_dirty(shell);
        xd_tooltip_render_dirty(shell);
        xd_balloon_render_dirty(shell);
        xd_thumbs_render_dirty(shell);
        xd_volume_flyout_render_dirty(shell);
        while (wl_display_prepare_read(d) != 0)
            wl_display_dispatch_pending(d);
        if (!flush_display(d)) {
            wl_display_cancel_read(d);
            return EXIT_FAILURE;
        }

        gint prio = 0, timeout = -1;
        g_main_context_prepare(ctx, &prio);
        gint ng;
        while ((ng = g_main_context_query(ctx, prio, &timeout, gfds, gcap)) > gcap) {
            gcap = ng;
            gfds = xd_realloc_array(gfds, (size_t)gcap, sizeof(*gfds));
            fds = xd_realloc_array(fds, (size_t)gcap + OWN_FDS, sizeof(*fds));
        }
        if (fds == nullptr)
            fds = xd_realloc_array(fds, OWN_FDS, sizeof(*fds));
        const int own[OWN_FDS] = {
            wl_display_get_fd(d),          timer_fd,
            signal_fd,                     shell->menu_timer_fd,
            shell->settings_fd,            shell->quicklaunch.watch_fd,
            shell->flash_timer_fd,         shell->tooltip_timer_fd,
            shell->autohide_timer_fd,      shell->repeat_timer_fd,
        };
        for (int i = 0; i < OWN_FDS; i++)
            fds[i] = (struct pollfd){.fd = own[i], .events = POLLIN};
        for (gint i = 0; i < ng; i++)
            fds[OWN_FDS + i] = (struct pollfd){.fd = gfds[i].fd, .events = (short)gfds[i].events};
        int pr = poll(fds, (nfds_t)(OWN_FDS + ng), timeout);
        for (gint i = 0; i < ng; i++)
            gfds[i].revents = (gushort)(pr > 0 ? fds[OWN_FDS + i].revents : 0);
        gboolean glib_ready = g_main_context_check(ctx, prio, gfds, ng);
        if (pr < 0) {
            wl_display_cancel_read(d);
            if (errno == EINTR)
                continue;
            rc = EXIT_FAILURE;
            break;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(d) < 0) {
                rc = display_lost(d);
                break;
            }
        } else {
            wl_display_cancel_read(d);
        }
        /* POLLHUP: the compositor went away. */
        if ((fds[0].revents & (POLLERR | POLLHUP)) || wl_display_dispatch_pending(d) < 0) {
            rc = display_lost(d);
            break;
        }
        /* GLib's callbacks (D-Bus, pipes) run once the Wayland read is
         * done: they may wait on the display themselves (an activation
         * token for a launch), which would deadlock while it's prepared. */
        if (glib_ready)
            g_main_context_dispatch(ctx);

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
        if (shell->flash_timer_fd >= 0 && (fds[6].revents & POLLIN)) {
            uint64_t expirations;
            (void)!read(shell->flash_timer_fd, &expirations, sizeof(expirations));
            xd_toplevels_flash_tick(shell);
        }
        if (shell->tooltip_timer_fd >= 0 && (fds[7].revents & POLLIN)) {
            uint64_t expirations;
            (void)!read(shell->tooltip_timer_fd, &expirations, sizeof(expirations));
            xd_taskbar_tooltip_timer(shell);
        }
        if (shell->repeat_timer_fd >= 0 && (fds[9].revents & POLLIN)) {
            uint64_t expirations;
            (void)!read(shell->repeat_timer_fd, &expirations, sizeof(expirations));
            repeat_fired(shell);
        }
        if (shell->autohide_timer_fd >= 0 && (fds[8].revents & POLLIN)) {
            uint64_t expirations;
            (void)!read(shell->autohide_timer_fd, &expirations, sizeof(expirations));
            xd_taskbars_tuck_timer(shell);
        }
    }
    xd_free(gfds);
    xd_free(fds);
    g_main_context_release(ctx);
    return rc;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    static xd_shell shell;
    shell.style = XD_STYLE_CLASSIC;
    const char *term = getenv("TERMINAL");
    shell.terminal = term != nullptr && term[0] != '\0' ? term : "alacritty";
    shell.menu_timer_fd = -1;
    shell.flash_timer_fd = -1;
    shell.tooltip_timer_fd = -1;
    shell.autohide_timer_fd = -1;
    shell.repeat_timer_fd = -1;
    shell.repeat_rate = 25; /* until the compositor says */
    shell.repeat_delay = 600;
    shell.saver_pidfd = -1;
    shell.settings_fd = -1;
    shell.quicklaunch.watch_fd = -1;
    xd_scheme_init_default(&shell.scheme);
    wl_list_init(&shell.taskbars);
    wl_list_init(&shell.toplevels);
    if (!parse_args(argc, argv, &shell)) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    /* SIGTERM and SIGINT end the loop (through a signalfd) and exit 0, so
     * neither xd-compositor nor systemd restarts us. Blocked before GLib
     * starts its threads (D-Bus), which inherit the mask: a thread that
     * didn't block them would take the signal and die of it. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    shell.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    shell.display = wl_display_connect(nullptr);
    if (shell.display == nullptr) {
        fprintf(stderr, "xd-shell: cannot connect to a Wayland compositor\n");
        return EXIT_FAILURE;
    }
    xd_thumbs_init(&shell); /* binds its globals as they come */
    shell.registry = wl_display_get_registry(shell.display);
    wl_registry_add_listener(shell.registry, &registry_listener, &shell);
    wl_display_roundtrip(shell.display);

    if (shell.compositor == nullptr || shell.shm == nullptr || shell.layer_shell == nullptr ||
        shell.wm_base == nullptr) {
        fprintf(stderr, "xd-shell: compositor lacks wl_shm, xdg-shell or wlr-layer-shell\n");
        return EXIT_FAILURE;
    }
    pid_t compositor = xd_session_compositor_pid(shell.display);
    xd_session_detect(&shell, compositor);

    /* Launched apps are reaped automatically. */
    signal(SIGCHLD, SIG_IGN);
    apply_settings(&shell, true); /* also loads the icon theme */
    shell.settings_fd = xd_settings_watch();
    xd_quicklaunch_init(&shell);
    xd_superbar_init(&shell);
    xd_apps_load(&shell.apps);
    xd_apps_load_control_panel(&shell.control_apps);
    xd_menu_tree_init(&shell.menu_tree);
    xd_menu_tree_init(&shell.context_tree);
    xd_menu_tree_init(&shell.docs_tree);
    shell.last_arrange = -1;
    xd_start_menu_build(&shell);
    xd_start_menu_watch_programs(&shell);
    shell.menu_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    shell.flash_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    shell.tooltip_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    shell.autohide_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    shell.repeat_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

    update_clock(&shell);
    xd_toplevels_init(&shell);
    xd_tray_init(&shell);
    xd_dnd_init(&shell, shell.data_device_manager);
    xd_sys_bluetooth_init(&shell);
    xd_sys_network_init(&shell);
    xd_sys_volume_init(&shell);
    if (!shell.taskbar_only)
        xd_deskicons_init(&shell);
    xd_sys_battery_init(&shell);
    xd_sys_access_init(&shell);
    xd_sleep_init(&shell);
    xd_buttons_init(&shell);
    xd_account_init(&shell);
    xd_housekeeping_init(&shell);
    xd_gpu_watch();
    xd_shell_sounds_init();
    xd_shellbus_init(&shell);
    xd_session_logon_sound(compositor);
    /* From here on, new outputs get a taskbar as soon as they appear. */
    shell.started = true;
    for (size_t i = 0; i < shell.early_count; i++) {
        xd_taskbar_create(&shell, shell.early_outputs[i].output, shell.early_outputs[i].name);
        xd_primary_track(&shell, shell.early_outputs[i].output);
    }
    shell.early_count = 0;

    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    int timer_fd = clock_timer_create();

    shell.running = true;
    int rc = run(&shell, timer_fd, signal_fd);

    xd_menu_close_all(&shell);
    xd_identify_hide(&shell);

    xd_primary_finish(&shell);
    xd_langbar_finish(&shell);
    XD_LIST_FOR_EACH_SAFE(bar, tmp, &shell.taskbars, xd_taskbar, link) {
        xd_taskbar_destroy(bar);
    }
    XD_LIST_FOR_EACH_SAFE(t, tmp, &shell.toplevels, xd_toplevel, link) {
        zwlr_foreign_toplevel_handle_v1_destroy(t->handle);
        XD_FREE(&t->title);
        XD_FREE(&t->app_id);
        g_free(t->icon);
        g_free(t->app_name);
        g_free(t->desktop_id);
        wl_list_remove(&t->link);
        xd_free(t);
    }
    xd_shellbus_finish(&shell);
    xd_tray_finish(&shell);
    xd_sleep_finish(&shell);
    xd_buttons_finish(&shell);
    xd_account_finish(&shell);
    xd_housekeeping_finish(&shell);
    xd_gpu_unwatch();
    xd_sound_finish();
    xd_deskicons_finish(&shell);
    xd_dnd_finish(&shell);
    xd_menu_tree_free(&shell.menu_tree);
    if (shell.own_commands != nullptr)
        g_hash_table_destroy(shell.own_commands);
    xd_menu_tree_free(&shell.context_tree);
    xd_menu_tree_free(&shell.docs_tree);
    xd_start_menu_unwatch_programs(&shell);
    xd_apps_free(&shell.apps);
    xd_apps_free(&shell.control_apps);
    xd_quicklaunch_finish(&shell);
    xd_thumbs_finish(&shell);
    xd_superbar_finish(&shell);
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
