/* SPDX-License-Identifier: MIT
 * libxdui: connection, input, clipboard and the event loop.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "ui_internal.h"

static const char *const text_mimes[] = {"text/plain;charset=utf-8", "text/plain",
                                         "UTF8_STRING"};

/* ---- pointer ----------------------------------------------------------------- */

void xd_ui_set_cursor_text(xd_ui *ui, bool text) {
    if (ui->cursor_shape != nullptr)
        wp_cursor_shape_device_v1_set_shape(ui->cursor_shape, ui->pointer_serial,
                                            text ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT
                                                 : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    xd_ui *ui = data;
    ui->pointer_serial = serial;
    ui->pointer_window = xd_window_for_surface(ui, surface);
    ui->pointer_popup = xd_popup_for_surface(ui, surface);
    ui->pointer_x = wl_fixed_to_double(x);
    ui->pointer_y = wl_fixed_to_double(y);
    xd_ui_set_cursor_text(ui, false);
    if (ui->pointer_window != nullptr)
        xd_window_pointer_motion(ui->pointer_window, ui->pointer_x, ui->pointer_y);
    if (ui->pointer_popup != nullptr)
        xd_popup_pointer_motion(ui->pointer_popup, ui->pointer_x, ui->pointer_y);
}

static void pointer_leave(void *data, [[maybe_unused]] struct wl_pointer *p,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface) {
    xd_ui *ui = data;
    if (ui->pointer_window != nullptr)
        xd_window_pointer_leave(ui->pointer_window);
    ui->pointer_window = nullptr;
    ui->pointer_popup = nullptr;
}

static void pointer_motion(void *data, [[maybe_unused]] struct wl_pointer *p,
                           [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    xd_ui *ui = data;
    ui->pointer_x = wl_fixed_to_double(x);
    ui->pointer_y = wl_fixed_to_double(y);
    if (ui->pointer_window != nullptr)
        xd_window_pointer_motion(ui->pointer_window, ui->pointer_x, ui->pointer_y);
    if (ui->pointer_popup != nullptr)
        xd_popup_pointer_motion(ui->pointer_popup, ui->pointer_x, ui->pointer_y);
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *p, uint32_t serial,
                           [[maybe_unused]] uint32_t time, uint32_t button, uint32_t state) {
    xd_ui *ui = data;
    ui->pointer_serial = serial;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (ui->pointer_popup != nullptr)
        xd_popup_pointer_button(ui->pointer_popup, button, pressed);
    else if (ui->pointer_window != nullptr)
        xd_window_pointer_button(ui->pointer_window, button, pressed);
}

static void pointer_axis(void *data, [[maybe_unused]] struct wl_pointer *p,
                         [[maybe_unused]] uint32_t time, uint32_t axis, wl_fixed_t value) {
    xd_ui *ui = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && ui->pointer_popup != nullptr)
        xd_popup_pointer_axis(ui->pointer_popup, wl_fixed_to_double(value));
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

/* ---- keyboard ------------------------------------------------------------------ */

static uint32_t current_mods(xd_ui *ui) {
    uint32_t m = 0;
    if (ui->xkb_state == nullptr)
        return m;
    if (xkb_state_mod_name_is_active(ui->xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) >
        0)
        m |= XD_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(ui->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) >
        0)
        m |= XD_MOD_CTRL;
    if (xkb_state_mod_name_is_active(ui->xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        m |= XD_MOD_ALT;
    return m;
}

static void dispatch_key(xd_ui *ui, uint32_t key) {
    if (ui->keyboard_window == nullptr || ui->xkb_state == nullptr)
        return;
    xkb_keycode_t code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(ui->xkb_state, code);
    char utf8[16] = {0};
    xkb_state_key_get_utf8(ui->xkb_state, code, utf8, sizeof(utf8));
    xd_window_key(ui->keyboard_window, sym, utf8, current_mods(ui));
}

static void stop_repeat(xd_ui *ui) {
    ui->repeat_key = 0;
    if (ui->repeat_fd >= 0) {
        struct itimerspec off = {0};
        timerfd_settime(ui->repeat_fd, 0, &off, nullptr);
    }
}

static void keyboard_keymap(void *data, [[maybe_unused]] struct wl_keyboard *k, uint32_t format,
                            int32_t fd, uint32_t size) {
    xd_ui *ui = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || size > (16u << 20)) {
        close(fd);
        return;
    }
    char *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;
    struct xkb_keymap *keymap = nullptr;
    if (memchr(map, '\0', size) != nullptr)
        keymap = xkb_keymap_new_from_string(ui->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (keymap == nullptr)
        return;
    xkb_state_unref(ui->xkb_state);
    xkb_keymap_unref(ui->keymap);
    ui->keymap = keymap;
    ui->xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter(void *data, [[maybe_unused]] struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *surface, [[maybe_unused]] struct wl_array *keys) {
    xd_ui *ui = data;
    ui->key_serial = serial;
    ui->keyboard_window = xd_window_for_surface(ui, surface);
    if (ui->keyboard_window != nullptr) {
        ui->keyboard_window->active = true;
        ui->keyboard_window->dirty = true;
    }
}

static void keyboard_leave(void *data, [[maybe_unused]] struct wl_keyboard *k,
                           [[maybe_unused]] uint32_t serial,
                           [[maybe_unused]] struct wl_surface *surface) {
    xd_ui *ui = data;
    stop_repeat(ui);
    if (ui->keyboard_window != nullptr) {
        ui->keyboard_window->active = false;
        ui->keyboard_window->dirty = true;
    }
    ui->keyboard_window = nullptr;
}

static void keyboard_key(void *data, [[maybe_unused]] struct wl_keyboard *k, uint32_t serial,
                         [[maybe_unused]] uint32_t time, uint32_t key, uint32_t state) {
    xd_ui *ui = data;
    ui->key_serial = serial;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (key == ui->repeat_key)
            stop_repeat(ui);
        return;
    }
    dispatch_key(ui, key);
    if (ui->keymap != nullptr && xkb_keymap_key_repeats(ui->keymap, key + 8) &&
        ui->repeat_rate > 0 && ui->repeat_fd >= 0) {
        ui->repeat_key = key;
        long delay_ns = (long)ui->repeat_delay * 1000000L;
        long period_ns = 1000000000L / ui->repeat_rate;
        struct itimerspec spec = {
            .it_value = {.tv_sec = delay_ns / 1000000000L, .tv_nsec = delay_ns % 1000000000L},
            .it_interval = {.tv_sec = period_ns / 1000000000L,
                            .tv_nsec = period_ns % 1000000000L},
        };
        timerfd_settime(ui->repeat_fd, 0, &spec, nullptr);
    }
}

static void keyboard_modifiers(void *data, [[maybe_unused]] struct wl_keyboard *k,
                               [[maybe_unused]] uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group) {
    xd_ui *ui = data;
    if (ui->xkb_state != nullptr)
        xkb_state_update_mask(ui->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, [[maybe_unused]] struct wl_keyboard *k,
                                 int32_t rate, int32_t delay) {
    xd_ui *ui = data;
    ui->repeat_rate = XD_CLAMP(rate, 0, 100);
    ui->repeat_delay = XD_CLAMP(delay, 100, 2000);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ---- clipboard ------------------------------------------------------------------ */

static bool is_text_mime(const char *mime) {
    for (size_t i = 0; i < XD_ARRAY_LEN(text_mimes); i++) {
        if (strcmp(mime, text_mimes[i]) == 0)
            return true;
    }
    return false;
}

static void offer_offer(void *data, struct wl_data_offer *offer, const char *mime) {
    xd_ui *ui = data;
    if (offer == ui->pending_offer && is_text_mime(mime))
        ui->pending_offer_text = true;
}

static void offer_source_actions([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_data_offer *o,
                                 [[maybe_unused]] uint32_t actions) {}
static void offer_action([[maybe_unused]] void *data, [[maybe_unused]] struct wl_data_offer *o,
                         [[maybe_unused]] uint32_t action) {}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void device_data_offer(void *data, [[maybe_unused]] struct wl_data_device *d,
                              struct wl_data_offer *offer) {
    xd_ui *ui = data;
    if (ui->pending_offer != nullptr && ui->pending_offer != ui->selection)
        wl_data_offer_destroy(ui->pending_offer);
    ui->pending_offer = offer;
    ui->pending_offer_text = false;
    wl_data_offer_add_listener(offer, &offer_listener, ui);
}

static void device_selection(void *data, [[maybe_unused]] struct wl_data_device *d,
                             struct wl_data_offer *offer) {
    xd_ui *ui = data;
    if (ui->selection != nullptr && ui->selection != offer)
        wl_data_offer_destroy(ui->selection);
    ui->selection = offer;
    ui->selection_text = offer != nullptr && offer == ui->pending_offer && ui->pending_offer_text;
    if (offer == ui->pending_offer)
        ui->pending_offer = nullptr;
}

/* Drag and drop isn't supported in dialogs; release offers right away. */
static void device_enter(void *data, [[maybe_unused]] struct wl_data_device *d,
                         [[maybe_unused]] uint32_t serial,
                         [[maybe_unused]] struct wl_surface *surface,
                         [[maybe_unused]] wl_fixed_t x, [[maybe_unused]] wl_fixed_t y,
                         struct wl_data_offer *offer) {
    xd_ui *ui = data;
    if (offer != nullptr) {
        if (offer == ui->pending_offer)
            ui->pending_offer = nullptr;
        wl_data_offer_destroy(offer);
    }
}
static void device_leave([[maybe_unused]] void *data,
                         [[maybe_unused]] struct wl_data_device *d) {}
static void device_motion([[maybe_unused]] void *data, [[maybe_unused]] struct wl_data_device *d,
                          [[maybe_unused]] uint32_t time, [[maybe_unused]] wl_fixed_t x,
                          [[maybe_unused]] wl_fixed_t y) {}
static void device_drop([[maybe_unused]] void *data, [[maybe_unused]] struct wl_data_device *d) {}

static const struct wl_data_device_listener device_listener = {
    .data_offer = device_data_offer,
    .enter = device_enter,
    .leave = device_leave,
    .motion = device_motion,
    .drop = device_drop,
    .selection = device_selection,
};

static void source_target([[maybe_unused]] void *data, [[maybe_unused]] struct wl_data_source *s,
                          [[maybe_unused]] const char *mime) {}

static void source_send(void *data, [[maybe_unused]] struct wl_data_source *s,
                        [[maybe_unused]] const char *mime, int32_t fd) {
    xd_ui *ui = data;
    /* Small text; a blocking write is fine. A reader that hangs up gives
     * EPIPE (SIGPIPE is ignored, see xd_ui_connect). */
    if (ui->copied != nullptr) {
        size_t len = strlen(ui->copied);
        size_t off = 0;
        while (off < len) {
            ssize_t n = write(fd, ui->copied + off, len - off);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            off += (size_t)n;
        }
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *s) {
    xd_ui *ui = data;
    if (ui->source == s) {
        ui->source = nullptr;
        XD_FREE(&ui->copied);
    }
    wl_data_source_destroy(s);
}

static void source_dnd_drop_performed([[maybe_unused]] void *data,
                                      [[maybe_unused]] struct wl_data_source *s) {}
static void source_dnd_finished([[maybe_unused]] void *data,
                                [[maybe_unused]] struct wl_data_source *s) {}
static void source_action([[maybe_unused]] void *data, [[maybe_unused]] struct wl_data_source *s,
                          [[maybe_unused]] uint32_t action) {}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_dnd_drop_performed,
    .dnd_finished = source_dnd_finished,
    .action = source_action,
};

void xd_ui_clipboard_copy(xd_ui *ui, xd_str text) {
    if (ui->data_device_manager == nullptr || ui->data_device == nullptr || text.len == 0)
        return;
    if (ui->source != nullptr) {
        wl_data_source_destroy(ui->source);
        ui->source = nullptr;
    }
    XD_FREE(&ui->copied);
    ui->copied = xd_str_dup_cstr(text);
    ui->source = wl_data_device_manager_create_data_source(ui->data_device_manager);
    wl_data_source_add_listener(ui->source, &source_listener, ui);
    for (size_t i = 0; i < XD_ARRAY_LEN(text_mimes); i++)
        wl_data_source_offer(ui->source, text_mimes[i]);
    wl_data_device_set_selection(ui->data_device, ui->source, ui->key_serial);
}

/* Keeps the first line of pasted text, as a single-line field would. */
static char *first_line(xd_str s) {
    size_t nl = s.len;
    for (size_t i = 0; i < s.len; i++) {
        if (s.ptr[i] == '\n' || s.ptr[i] == '\r') {
            nl = i;
            break;
        }
    }
    return xd_str_dup_cstr(xd_str_sub(s, 0, nl));
}

char *xd_ui_clipboard_paste(xd_ui *ui) {
    /* Our own selection: answer from memory instead of piping to
     * ourselves, which would deadlock this single-threaded loop. */
    if (ui->source != nullptr && ui->copied != nullptr)
        return first_line(xd_str_from_cstr(ui->copied));
    if (ui->selection == nullptr || !ui->selection_text)
        return nullptr;

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return nullptr;
    wl_data_offer_receive(ui->selection, text_mimes[0], fds[1]);
    close(fds[1]);
    wl_display_flush(ui->display);

    /* Read up to 64 KiB, giving the owner a second to answer. */
    xd_strbuf buf = {0};
    char chunk[4096];
    while (buf.len < 65536) {
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        int r = poll(&pfd, 1, 1000);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        ssize_t n = read(fds[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        xd_strbuf_append(&buf, (xd_str){chunk, (size_t)n});
    }
    close(fds[0]);
    char *line = first_line(xd_strbuf_view(&buf));
    xd_strbuf_free(&buf);
    return line;
}

/* ---- seat and globals --------------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    xd_ui *ui = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && ui->pointer == nullptr) {
        ui->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ui->pointer, &pointer_listener, ui);
        if (ui->cursor_shape_manager != nullptr)
            ui->cursor_shape =
                wp_cursor_shape_manager_v1_get_pointer(ui->cursor_shape_manager, ui->pointer);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && ui->keyboard == nullptr) {
        ui->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(ui->keyboard, &keyboard_listener, ui);
    }
}

static void seat_name([[maybe_unused]] void *data, [[maybe_unused]] struct wl_seat *seat,
                      [[maybe_unused]] const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void wm_base_ping([[maybe_unused]] void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

static void *bind(struct wl_registry *reg, uint32_t name, const struct wl_interface *iface,
                  uint32_t have, uint32_t want) {
    return wl_registry_bind(reg, name, iface, XD_MIN(have, want));
}

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
    xd_ui *ui = data;
    xd_str iface = xd_str_from_cstr(interface);
#define IS(i) xd_str_eq(iface, xd_str_from_cstr((i).name))
    if (IS(wl_compositor_interface)) {
        ui->compositor = bind(reg, name, &wl_compositor_interface, version, 4);
    } else if (IS(wl_shm_interface)) {
        ui->shm = bind(reg, name, &wl_shm_interface, version, 1);
    } else if (IS(wl_seat_interface) && ui->seat == nullptr) {
        ui->seat = bind(reg, name, &wl_seat_interface, version, 5);
        wl_seat_add_listener(ui->seat, &seat_listener, ui);
    } else if (IS(xdg_wm_base_interface)) {
        ui->wm_base = bind(reg, name, &xdg_wm_base_interface, version, 3);
        xdg_wm_base_add_listener(ui->wm_base, &wm_base_listener, ui);
    } else if (IS(wp_fractional_scale_manager_v1_interface)) {
        ui->fractional_manager =
            bind(reg, name, &wp_fractional_scale_manager_v1_interface, version, 1);
    } else if (IS(wp_viewporter_interface)) {
        ui->viewporter = bind(reg, name, &wp_viewporter_interface, version, 1);
    } else if (IS(wp_cursor_shape_manager_v1_interface)) {
        ui->cursor_shape_manager =
            bind(reg, name, &wp_cursor_shape_manager_v1_interface, version, 1);
    } else if (IS(wl_data_device_manager_interface)) {
        ui->data_device_manager = bind(reg, name, &wl_data_device_manager_interface, version, 3);
    }
#undef IS
}

static void registry_global_remove([[maybe_unused]] void *data,
                                   [[maybe_unused]] struct wl_registry *reg,
                                   [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- lifecycle ----------------------------------------------------------------- */

xd_ui *xd_ui_connect(int argc, char **argv) {
    xd_ui *ui = XD_NEW(xd_ui);
    ui->style = XD_STYLE_CLASSIC;
    xd_scheme_init_default(&ui->scheme);
    ui->repeat_rate = 30;
    ui->repeat_delay = 500;
    ui->repeat_fd = -1;
    wl_list_init(&ui->windows);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i + 1 < argc; i++) {
        xd_str arg = xd_str_from_cstr(argv[i]);
        if (xd_str_eq(arg, XD_STR("--style"))) {
            if (!xd_visual_style_from_name(xd_str_from_cstr(argv[i + 1]), &ui->style))
                fprintf(stderr, "%s: unknown style %s\n", argv[0], argv[i + 1]);
            i++;
        } else if (xd_str_eq(arg, XD_STR("--scheme"))) {
            xd_strbuf err = {0};
            if (!xd_scheme_load(argv[i + 1], &ui->scheme, &err))
                fprintf(stderr, "%s: %s: %s\n", argv[0], argv[i + 1], xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
            i++;
        }
    }

    ui->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    ui->display = wl_display_connect(nullptr);
    if (ui->display == nullptr) {
        fprintf(stderr, "%s: cannot connect to a Wayland compositor\n", argc > 0 ? argv[0] : "");
        xd_ui_destroy(ui);
        return nullptr;
    }
    ui->registry = wl_display_get_registry(ui->display);
    wl_registry_add_listener(ui->registry, &registry_listener, ui);
    wl_display_roundtrip(ui->display);
    if (ui->compositor == nullptr || ui->shm == nullptr || ui->wm_base == nullptr) {
        fprintf(stderr, "%s: compositor lacks wl_shm or xdg-shell\n", argc > 0 ? argv[0] : "");
        xd_ui_destroy(ui);
        return nullptr;
    }
    if (ui->data_device_manager != nullptr && ui->seat != nullptr) {
        ui->data_device = wl_data_device_manager_get_data_device(ui->data_device_manager, ui->seat);
        wl_data_device_add_listener(ui->data_device, &device_listener, ui);
    }
    /* Second roundtrip: seat capabilities and keymap. */
    wl_display_roundtrip(ui->display);

    ui->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    const char *icon_theme = getenv("XD_ICON_THEME");
    ui->icons = xd_icons_create(icon_theme != nullptr ? icon_theme : "breeze");
    return ui;
}

xd_icons *xd_ui_icons(xd_ui *ui) { return ui->icons; }

void xd_ui_quit(xd_ui *ui) { ui->running = false; }

static bool flush(struct wl_display *d) {
    while (wl_display_flush(d) < 0) {
        if (errno != EAGAIN)
            return false;
        struct pollfd pfd = {.fd = wl_display_get_fd(d), .events = POLLOUT};
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
            return false;
    }
    return true;
}

static void render_dirty(xd_ui *ui) {
    XD_LIST_FOR_EACH(win, &ui->windows, xd_window, link) {
        if (win->dirty && win->configured)
            xd_window_render(win);
        if (win->popup != nullptr && win->popup->dirty && win->popup->configured)
            xd_popup_render(win->popup);
    }
}

void xd_ui_run(xd_ui *ui) {
    struct wl_display *d = ui->display;
    ui->running = !wl_list_empty(&ui->windows);
    while (ui->running) {
        render_dirty(ui);
        while (wl_display_prepare_read(d) != 0)
            wl_display_dispatch_pending(d);
        if (!flush(d)) {
            wl_display_cancel_read(d);
            break;
        }
        struct pollfd fds[2] = {
            {.fd = wl_display_get_fd(d), .events = POLLIN},
            {.fd = ui->repeat_fd, .events = POLLIN},
        };
        if (poll(fds, XD_ARRAY_LEN(fds), -1) < 0) {
            wl_display_cancel_read(d);
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(d) < 0)
                break;
        } else {
            wl_display_cancel_read(d);
        }
        if (fds[0].revents & (POLLERR | POLLHUP))
            break;
        if (wl_display_dispatch_pending(d) < 0)
            break;
        if (ui->repeat_fd >= 0 && (fds[1].revents & POLLIN)) {
            uint64_t n;
            (void)!read(ui->repeat_fd, &n, sizeof(n));
            if (ui->repeat_key != 0)
                dispatch_key(ui, ui->repeat_key);
        }
        if (wl_list_empty(&ui->windows))
            ui->running = false;
    }
    flush(d);
}

void xd_ui_destroy(xd_ui *ui) {
    if (ui == nullptr)
        return;
    XD_LIST_FOR_EACH_SAFE(win, tmp, &ui->windows, xd_window, link) {
        xd_window_close(win);
    }
    if (ui->source != nullptr)
        wl_data_source_destroy(ui->source);
    XD_FREE(&ui->copied);
    if (ui->selection != nullptr)
        wl_data_offer_destroy(ui->selection);
    if (ui->pending_offer != nullptr)
        wl_data_offer_destroy(ui->pending_offer);
    if (ui->data_device != nullptr)
        wl_data_device_release(ui->data_device);
    if (ui->cursor_shape != nullptr)
        wp_cursor_shape_device_v1_destroy(ui->cursor_shape);
    if (ui->pointer != nullptr)
        wl_pointer_release(ui->pointer);
    if (ui->keyboard != nullptr)
        wl_keyboard_release(ui->keyboard);
    xkb_state_unref(ui->xkb_state);
    xkb_keymap_unref(ui->keymap);
    xkb_context_unref(ui->xkb);
    if (ui->repeat_fd >= 0)
        close(ui->repeat_fd);
    xd_icons_destroy(ui->icons);
    if (ui->display != nullptr)
        wl_display_disconnect(ui->display);
    xd_free(ui);
}
