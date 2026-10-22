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
#include "../settings/settings.h"
#include "../settings/themepkg.h"
#include "ui_internal.h"

static const char *const text_mimes[] = {"text/plain;charset=utf-8", "text/plain",
                                         "UTF8_STRING"};

/* ---- pointer ----------------------------------------------------------------- */

/* Called on every motion: only a change of shape goes to the compositor. */
void xd_ui_set_cursor_text(xd_ui *ui, bool text) {
    if (ui->cursor_shape == nullptr ||
        (ui->cursor_set && ui->cursor_text == text && ui->cursor_busy == ui->busy))
        return;
    /* Busy: Windows' "working in background" arrow, over text fields too. */
    wp_cursor_shape_device_v1_set_shape(ui->cursor_shape, ui->pointer_serial,
                                        ui->busy ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS
                                        : text   ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT
                                                 : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    ui->cursor_set = true;
    ui->cursor_text = text;
    ui->cursor_busy = ui->busy;
}

void xd_ui_set_busy(xd_ui *ui, bool busy) {
    ui->busy = busy;
    /* Now, if the pointer is over one of our windows; else on its next enter. */
    if (ui->cursor_set)
        xd_ui_set_cursor_text(ui, ui->cursor_text);
}

static void pointer_enter(void *data, [[maybe_unused]] struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    xd_ui *ui = data;
    ui->pointer_serial = serial;
    ui->cursor_set = false; /* each enter needs its shape set again */
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

static void pointer_leave(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                          [[maybe_unused]] uint32_t serial,
                          [[maybe_unused]] struct wl_surface *surface) {
    xd_ui *ui = data;
    if (ui->pointer_window != nullptr)
        xd_window_pointer_leave(ui->pointer_window);
    ui->pointer_window = nullptr;
    ui->pointer_popup = nullptr;
}

static void pointer_motion(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                           [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    xd_ui *ui = data;
    ui->pointer_x = wl_fixed_to_double(x);
    ui->pointer_y = wl_fixed_to_double(y);
    if (ui->pointer_window != nullptr)
        xd_window_pointer_motion(ui->pointer_window, ui->pointer_x, ui->pointer_y);
    if (ui->pointer_popup != nullptr)
        xd_popup_pointer_motion(ui->pointer_popup, ui->pointer_x, ui->pointer_y);
}

static void pointer_button(void *data, [[maybe_unused]] struct wl_pointer *pointer, uint32_t serial,
                           [[maybe_unused]] uint32_t time, uint32_t button, uint32_t state) {
    xd_ui *ui = data;
    ui->pointer_serial = serial;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (ui->pointer_popup != nullptr)
        xd_popup_pointer_button(ui->pointer_popup, button, pressed);
    else if (ui->pointer_window != nullptr)
        xd_window_pointer_button(ui->pointer_window, button, pressed);
}

static void pointer_axis(void *data, [[maybe_unused]] struct wl_pointer *pointer,
                         [[maybe_unused]] uint32_t time, uint32_t axis, wl_fixed_t value) {
    xd_ui *ui = data;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;
    if (ui->pointer_popup != nullptr)
        xd_popup_pointer_axis(ui->pointer_popup, wl_fixed_to_double(value));
    else if (ui->pointer_window != nullptr)
        xd_window_pointer_axis(ui->pointer_window, wl_fixed_to_double(value));
}

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

/* ---- keyboard ------------------------------------------------------------------ */

static uint32_t current_mods(xd_ui *ui) {
    uint32_t mods = 0;
    if (ui->xkb_state == nullptr)
        return mods;
    if (xkb_state_mod_name_is_active(ui->xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) >
        0)
        mods |= XD_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(ui->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) >
        0)
        mods |= XD_MOD_CTRL;
    if (xkb_state_mod_name_is_active(ui->xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= XD_MOD_ALT;
    return mods;
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

static void keyboard_keymap(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
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

static void keyboard_enter(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           [[maybe_unused]] struct wl_array *keys) {
    xd_ui *ui = data;
    ui->key_serial = serial;
    ui->keyboard_window = xd_window_for_surface(ui, surface);
    if (ui->keyboard_window != nullptr) {
        ui->keyboard_window->active = true;
        ui->keyboard_window->dirty = true;
    }
}

static void keyboard_leave(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
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

static void keyboard_key(void *data, [[maybe_unused]] struct wl_keyboard *keyboard, uint32_t serial,
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

static void keyboard_modifiers(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
                               [[maybe_unused]] uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group) {
    xd_ui *ui = data;
    if (ui->xkb_state != nullptr)
        xkb_state_update_mask(ui->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, [[maybe_unused]] struct wl_keyboard *keyboard,
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
                                 [[maybe_unused]] struct wl_data_offer *data_offer,
                                 [[maybe_unused]] uint32_t actions) {}
static void offer_action([[maybe_unused]] void *data,
                         [[maybe_unused]] struct wl_data_offer *data_offer,
                         [[maybe_unused]] uint32_t action) {}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void device_data_offer(void *data, [[maybe_unused]] struct wl_data_device *data_device,
                              struct wl_data_offer *offer) {
    xd_ui *ui = data;
    if (ui->pending_offer != nullptr && ui->pending_offer != ui->selection)
        wl_data_offer_destroy(ui->pending_offer);
    ui->pending_offer = offer;
    ui->pending_offer_text = false;
    wl_data_offer_add_listener(offer, &offer_listener, ui);
}

static void device_selection(void *data, [[maybe_unused]] struct wl_data_device *data_device,
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
static void device_enter(void *data, [[maybe_unused]] struct wl_data_device *data_device,
                         [[maybe_unused]] uint32_t serial,
                         [[maybe_unused]] struct wl_surface *surface, [[maybe_unused]] wl_fixed_t x,
                         [[maybe_unused]] wl_fixed_t y, struct wl_data_offer *offer) {
    xd_ui *ui = data;
    if (offer != nullptr) {
        if (offer == ui->pending_offer)
            ui->pending_offer = nullptr;
        wl_data_offer_destroy(offer);
    }
}
static void device_leave([[maybe_unused]] void *data,
                         [[maybe_unused]] struct wl_data_device *data_device) {}
static void device_motion([[maybe_unused]] void *data,
                          [[maybe_unused]] struct wl_data_device *data_device,
                          [[maybe_unused]] uint32_t time, [[maybe_unused]] wl_fixed_t x,
                          [[maybe_unused]] wl_fixed_t y) {}
static void device_drop([[maybe_unused]] void *data,
                        [[maybe_unused]] struct wl_data_device *data_device) {}

static const struct wl_data_device_listener device_listener = {
    .data_offer = device_data_offer,
    .enter = device_enter,
    .leave = device_leave,
    .motion = device_motion,
    .drop = device_drop,
    .selection = device_selection,
};

static void source_target([[maybe_unused]] void *data,
                          [[maybe_unused]] struct wl_data_source *data_source,
                          [[maybe_unused]] const char *mime) {}

static void source_send(void *data, [[maybe_unused]] struct wl_data_source *data_source,
                        [[maybe_unused]] const char *mime, int32_t fd) {
    xd_ui *ui = data;
    /* Small text; a blocking write is fine. A reader that hangs up gives
     * EPIPE (SIGPIPE is ignored, see xd_ui_connect). */
    if (ui->copied != nullptr) {
        size_t len = strlen(ui->copied);
        size_t off = 0;
        while (off < len) {
            ssize_t written = write(fd, ui->copied + off, len - off);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            off += (size_t)written;
        }
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *data_source) {
    xd_ui *ui = data;
    if (ui->source == data_source) {
        ui->source = nullptr;
        XD_FREE(&ui->copied);
    }
    wl_data_source_destroy(data_source);
}

static void source_dnd_drop_performed([[maybe_unused]] void *data,
                                      [[maybe_unused]] struct wl_data_source *data_source) {}
static void source_dnd_finished([[maybe_unused]] void *data,
                                [[maybe_unused]] struct wl_data_source *data_source) {}
static void source_action([[maybe_unused]] void *data,
                          [[maybe_unused]] struct wl_data_source *data_source,
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
static char *first_line(xd_str text) {
    size_t nl = text.len;
    for (size_t i = 0; i < text.len; i++) {
        if (text.ptr[i] == '\n' || text.ptr[i] == '\r') {
            nl = i;
            break;
        }
    }
    return xd_str_dup_cstr(xd_str_sub(text, 0, nl));
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
        int ready = poll(&pfd, 1, 1000);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        ssize_t bytes_read = read(fds[0], chunk, sizeof(chunk));
        if (bytes_read < 0 && errno == EINTR)
            continue;
        if (bytes_read <= 0)
            break;
        xd_strbuf_append(&buf, (xd_str){chunk, (size_t)bytes_read});
    }
    close(fds[0]);
    char *line = first_line(xd_strbuf_view(&buf));
    xd_strbuf_free(&buf);
    return line;
}

/* ---- outputs ------------------------------------------------------------------ */

static void copy_str_into(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src != nullptr ? src : "");
}

static xd_ui_output *find_output(xd_ui *ui, const void *proxy) {
    for (size_t i = 0; i < ui->output_count; i++) {
        if ((const void *)ui->outputs[i].output == proxy ||
            (const void *)ui->outputs[i].xdg_output == proxy)
            return &ui->outputs[i];
    }
    return nullptr;
}

static void out_geometry(void *data, struct wl_output *output, [[maybe_unused]] int32_t x,
                         [[maybe_unused]] int32_t y, int32_t pw, int32_t ph,
                         [[maybe_unused]] int32_t subpixel, const char *make, const char *model,
                         [[maybe_unused]] int32_t transform) {
    xd_ui_output *out = find_output(data, output);
    if (out == nullptr)
        return;
    out->info.phys_w_mm = pw;
    out->info.phys_h_mm = ph;
    if (out->info.description[0] == '\0')
        snprintf(out->info.description, sizeof(out->info.description), "%s %s",
                 make != nullptr ? make : "", model != nullptr ? model : "");
}

static void out_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width,
                     int32_t height, int32_t refresh) {
    xd_ui_output *out = find_output(data, output);
    if (out == nullptr || !(flags & WL_OUTPUT_MODE_CURRENT))
        return;
    out->info.mode_w = width;
    out->info.mode_h = height;
    out->info.refresh_mhz = refresh;
}

static void out_done([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *output) {}
static void out_scale([[maybe_unused]] void *data, [[maybe_unused]] struct wl_output *output,
                      [[maybe_unused]] int32_t factor) {}

static void out_name(void *data, struct wl_output *output, const char *name) {
    xd_ui_output *out = find_output(data, output);
    if (out != nullptr)
        copy_str_into(out->info.name, sizeof(out->info.name), name);
}

static void out_description(void *data, struct wl_output *output, const char *desc) {
    xd_ui_output *out = find_output(data, output);
    if (out != nullptr)
        copy_str_into(out->info.description, sizeof(out->info.description), desc);
}

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_description,
};

static void xdg_out_position(void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
    xd_ui_output *out = find_output(data, xdg_output);
    if (out != nullptr) {
        out->info.x = x;
        out->info.y = y;
    }
}

static void xdg_out_size(void *data, struct zxdg_output_v1 *xdg_output, int32_t width,
                         int32_t height) {
    xd_ui_output *out = find_output(data, xdg_output);
    if (out != nullptr) {
        out->info.logical_w = width;
        out->info.logical_h = height;
    }
}

static void xdg_out_done([[maybe_unused]] void *data,
                         [[maybe_unused]] struct zxdg_output_v1 *xdg_output) {}
static void xdg_out_name([[maybe_unused]] void *data,
                         [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
                         [[maybe_unused]] const char *name) {}
static void xdg_out_description([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
                                [[maybe_unused]] const char *description) {}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_out_position,
    .logical_size = xdg_out_size,
    .done = xdg_out_done,
    .name = xdg_out_name,
    .description = xdg_out_description,
};

static void add_xdg_output(xd_ui *ui, xd_ui_output *out) {
    if (ui->xdg_output_manager == nullptr || out->xdg_output != nullptr)
        return;
    out->xdg_output = zxdg_output_manager_v1_get_xdg_output(ui->xdg_output_manager, out->output);
    zxdg_output_v1_add_listener(out->xdg_output, &xdg_output_listener, ui);
}

size_t xd_ui_outputs(xd_ui *ui, const xd_output_info **out) {
    ui->output_infos = xd_realloc_array(ui->output_infos, XD_MAX(ui->output_count, 1),
                                        sizeof(*ui->output_infos));
    for (size_t i = 0; i < ui->output_count; i++)
        ui->output_infos[i] = ui->outputs[i].info;
    *out = ui->output_infos;
    return ui->output_count;
}

void xd_ui_set_theme(xd_ui *ui, xd_visual_style style, const xd_scheme *scheme) {
    ui->style = style;
    ui->scheme = *scheme;
    XD_LIST_FOR_EACH(win, &ui->windows, xd_window, link) {
        win->dirty = true;
    }
}

/* desktop.ini was saved: the style, scheme and theme package as it now
 * says (overriding --style and --scheme, as the shell does). */
static void settings_changed(void *user) {
    xd_ui *ui = user;
    if (!xd_settings_watch_changed(ui->settings_fd))
        return;
    xd_settings settings;
    xd_settings_load(&settings);
    xd_scheme scheme;
    xd_settings_scheme(&settings, &scheme);
    /* Windows draw with a theme made afresh each time: none holds the old
     * package once they're marked for redrawing. */
    xd_pkg *old = xd_settings_use_package(&settings);
    xd_ui_set_theme(ui, settings.style, &scheme);
    xd_pkg_free(old);
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
    /* The last keyboard or mouse unplugged: these objects go dead; new
     * ones are made when one comes back. */
    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && ui->keyboard != nullptr) {
        stop_repeat(ui);
        wl_keyboard_release(ui->keyboard);
        ui->keyboard = nullptr;
        ui->keyboard_window = nullptr;
    }
    if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ui->pointer != nullptr) {
        if (ui->cursor_shape != nullptr) {
            wp_cursor_shape_device_v1_destroy(ui->cursor_shape);
            ui->cursor_shape = nullptr;
        }
        wl_pointer_release(ui->pointer);
        ui->pointer = nullptr;
        ui->pointer_window = nullptr;
        ui->pointer_popup = nullptr;
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
    } else if (IS(wl_output_interface)) {
        if (ui->output_count == ui->output_cap) {
            ui->output_cap = ui->output_cap == 0 ? 4 : ui->output_cap * 2;
            ui->outputs = xd_realloc_array(ui->outputs, ui->output_cap, sizeof(*ui->outputs));
        }
        xd_ui_output *out = &ui->outputs[ui->output_count++];
        *out = (xd_ui_output){.global = name};
        out->output = bind(reg, name, &wl_output_interface, version, 4);
        wl_output_add_listener(out->output, &output_listener, ui);
        add_xdg_output(ui, out);
    } else if (IS(zxdg_output_manager_v1_interface)) {
        ui->xdg_output_manager = bind(reg, name, &zxdg_output_manager_v1_interface, version, 3);
        for (size_t i = 0; i < ui->output_count; i++)
            add_xdg_output(ui, &ui->outputs[i]);
    } else if (IS(wl_data_device_manager_interface)) {
        ui->data_device_manager = bind(reg, name, &wl_data_device_manager_interface, version, 3);
    } else if (IS(zwlr_output_manager_v1_interface)) {
        xd_display_bind(ui, reg, name, version);
    }
#undef IS
}

static void registry_global_remove(void *data, [[maybe_unused]] struct wl_registry *reg,
                                   uint32_t name) {
    xd_ui *ui = data;
    for (size_t i = 0; i < ui->output_count; i++) {
        if (ui->outputs[i].global != name)
            continue;
        if (ui->outputs[i].xdg_output != nullptr)
            zxdg_output_v1_destroy(ui->outputs[i].xdg_output);
        wl_output_release(ui->outputs[i].output);
        ui->outputs[i] = ui->outputs[--ui->output_count];
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- lifecycle ----------------------------------------------------------------- */

xd_ui *xd_ui_connect(int argc, char **argv) {
    xd_ui *ui = XD_NEW(xd_ui);
    /* desktop.ini first; --style/--scheme from the command line win. */
    xd_settings settings;
    xd_settings_load(&settings);
    ui->style = settings.style;
    xd_settings_scheme(&settings, &ui->scheme);
    xd_pkg_free(xd_settings_use_package(&settings)); /* none before it */
    ui->repeat_rate = 30;
    ui->repeat_delay = 500;
    ui->repeat_fd = -1;
    wl_list_init(&ui->windows);
    wl_list_init(&ui->sources);
    wl_list_init(&ui->heads);
    ui->settings_fd = xd_settings_watch();
    if (ui->settings_fd >= 0)
        xd_ui_add_fd(ui, ui->settings_fd, settings_changed, ui);
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
    /* Taken at start: these programs are short-lived dialogs. */
    ui->icons = xd_icons_create(xd_settings_icon_theme(&settings));
    return ui;
}

xd_icons *xd_ui_icons(xd_ui *ui) { return ui->icons; }

xd_visual_style xd_ui_style(const xd_ui *ui) { return ui->style; }

/* ---- timers and file descriptors -------------------------------------------- */

static xd_ui_source *add_source(xd_ui *ui, int fd, xd_ui_source_cb cb, void *user) {
    xd_ui_source *src = XD_NEW(xd_ui_source);
    src->fd = fd;
    src->cb = cb;
    src->user = user;
    wl_list_insert(ui->sources.prev, &src->link);
    return src;
}

xd_ui_source *xd_ui_add_timer(xd_ui *ui, int interval_ms, bool repeat, xd_ui_source_cb cb,
                              void *user) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0)
        return nullptr;
    long ms = XD_MAX(interval_ms, 1);
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    struct itimerspec spec = {.it_value = ts};
    if (repeat)
        spec.it_interval = ts;
    timerfd_settime(fd, 0, &spec, nullptr);
    xd_ui_source *src = add_source(ui, fd, cb, user);
    src->timer = true;
    src->repeat = repeat;
    return src;
}

xd_ui_source *xd_ui_add_fd(xd_ui *ui, int fd, xd_ui_source_cb cb, void *user) {
    return add_source(ui, fd, cb, user);
}

void xd_ui_remove_source(xd_ui_source *src) {
    if (src != nullptr)
        src->dead = true;
}

static void free_source(xd_ui_source *src) {
    wl_list_remove(&src->link);
    if (src->timer)
        close(src->fd);
    xd_free(src);
}

static void sweep_sources(xd_ui *ui) {
    XD_LIST_FOR_EACH_SAFE(src, tmp, &ui->sources, xd_ui_source, link) {
        if (src->dead)
            free_source(src);
    }
}

void xd_ui_quit(xd_ui *ui) { ui->running = false; }

static bool flush(struct wl_display *display) {
    while (wl_display_flush(display) < 0) {
        if (errno != EAGAIN)
            return false;
        struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLOUT};
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
        if (win->tip != nullptr && win->tip->dirty && win->tip->configured)
            xd_tooltip_render(win->tip);
    }
}

void xd_ui_run(xd_ui *ui) {
    struct wl_display *display = ui->display;
    ui->running = !wl_list_empty(&ui->windows);
    while (ui->running) {
        render_dirty(ui);
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        if (!flush(display)) {
            wl_display_cancel_read(display);
            break;
        }
        sweep_sources(ui);
        size_t nsrc = (size_t)wl_list_length(&ui->sources);
        struct pollfd *fds = xd_alloc_array(2 + nsrc, sizeof(*fds));
        fds[0] = (struct pollfd){.fd = wl_display_get_fd(display), .events = POLLIN};
        fds[1] = (struct pollfd){.fd = ui->repeat_fd, .events = POLLIN};
        size_t k = 2;
        XD_LIST_FOR_EACH(src, &ui->sources, xd_ui_source, link) {
            fds[k++] = (struct pollfd){.fd = src->fd, .events = POLLIN};
        }
        if (poll(fds, 2 + nsrc, -1) < 0) {
            xd_free(fds);
            wl_display_cancel_read(display);
            if (errno == EINTR)
                continue;
            break;
        }
        bool wayland_ok = true;
        if (fds[0].revents & POLLIN)
            wayland_ok = wl_display_read_events(display) >= 0;
        else
            wl_display_cancel_read(display);
        if (!wayland_ok || (fds[0].revents & (POLLERR | POLLHUP)) ||
            wl_display_dispatch_pending(display) < 0) {
            xd_free(fds);
            break;
        }
        /* Quit from a window's event: nothing else of this round runs (a
         * timer's callback could find its program's state freed). */
        if (!ui->running) {
            xd_free(fds);
            break;
        }
        if (ui->repeat_fd >= 0 && (fds[1].revents & POLLIN)) {
            uint64_t expirations;
            (void)!read(ui->repeat_fd, &expirations, sizeof(expirations));
            if (ui->repeat_key != 0)
                dispatch_key(ui, ui->repeat_key);
        }
        /* Sources in poll order; ones added by callbacks wait a round. */
        k = 2;
        XD_LIST_FOR_EACH(src, &ui->sources, xd_ui_source, link) {
            if (k >= 2 + nsrc)
                break;
            short rev = fds[k++].revents;
            if (!ui->running)
                break;
            if (src->dead || rev == 0)
                continue;
            if (src->timer) {
                uint64_t expirations;
                (void)!read(src->fd, &expirations, sizeof(expirations));
                if (!src->repeat)
                    src->dead = true;
            }
            src->cb(src->user);
        }
        xd_free(fds);
        if (wl_list_empty(&ui->windows))
            ui->running = false;
    }
    flush(display);
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
    if (ui->settings_fd >= 0)
        close(ui->settings_fd);
    if (ui->sources.next != nullptr) {
        XD_LIST_FOR_EACH_SAFE(src, tmp, &ui->sources, xd_ui_source, link) {
            free_source(src);
        }
    }
    xd_display_destroy(ui);
    xd_icons_destroy(ui->icons);
    for (size_t i = 0; i < ui->output_count; i++) {
        if (ui->outputs[i].xdg_output != nullptr)
            zxdg_output_v1_destroy(ui->outputs[i].xdg_output);
        wl_output_release(ui->outputs[i].output);
    }
    xd_free(ui->outputs);
    xd_free(ui->output_infos);
    if (ui->display != nullptr)
        wl_display_disconnect(ui->display);
    xd_free(ui);
}
