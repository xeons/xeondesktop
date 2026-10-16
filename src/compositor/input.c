/* SPDX-License-Identifier: MIT
 * Keyboard, pointer and seat handling; window-manager key bindings; frame
 * interaction (move, resize, caption buttons).
 */
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "../core/alloc.h"
#include "server.h"

/* Windows' default double-click time. */
enum { XD_DOUBLE_CLICK_MS = 500 };

pid_t xd_spawn_argv(char *const argv[]) {
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0)
        return -1;
    /* The event loop blocks SIGCHLD etc. for its signalfd; children must
     * not inherit that mask. */
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSID);

    pid_t pid = -1;
    int err = posix_spawnp(&pid, argv[0], nullptr, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (err != 0) {
        wlr_log(WLR_ERROR, "cannot run %s: %s", argv[0], strerror(err));
        return -1;
    }
    return pid;
}

void xd_spawn(xd_server *server, const char *command) {
    (void)server;
    if (command == nullptr || command[0] == '\0')
        return;
    char sh[] = "/bin/sh";
    char flag[] = "-c";
    char *cmd = xd_str_dup_cstr(xd_str_from_cstr(command));
    char *argv[] = {sh, flag, cmd, nullptr};
    xd_spawn_argv(argv);
    xd_free(cmd);
}

void xd_shell_start(xd_server *server) {
    if (server->config.shell == nullptr || server->terminating)
        return;
    /* Restart at most 5 times in 30 seconds, so a shell that crashes on
     * startup doesn't spin. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - server->shell_restart_window.tv_sec > 30) {
        server->shell_restart_window = now;
        server->shell_restarts = 0;
    }
    if (++server->shell_restarts > 5) {
        wlr_log(WLR_ERROR, "shell keeps exiting; not restarting it");
        return;
    }

    /* The shell reads desktop.ini itself; only command-line overrides are
     * passed on. */
    char *shell = xd_str_dup_cstr(xd_str_from_cstr(server->config.shell));
    char opt_style[] = "--style";
    char *style = xd_str_dup_cstr(xd_str_from_cstr(xd_visual_style_name(server->config.style)));
    char opt_scheme[] = "--scheme";
    char *scheme = server->config.scheme_path != nullptr
                       ? xd_str_dup_cstr(xd_str_from_cstr(server->config.scheme_path))
                       : nullptr;
    char *argv[6] = {shell};
    size_t argc = 1;
    if (server->config.cli_style) {
        argv[argc++] = opt_style;
        argv[argc++] = style;
    }
    if (scheme != nullptr) {
        argv[argc++] = opt_scheme;
        argv[argc++] = scheme;
    }
    argv[argc] = nullptr;
    server->shell_pid = xd_spawn_argv(argv);
    xd_free(scheme);
    xd_free(style);
    xd_free(shell);
}

/* ---- keyboard --------------------------------------------------------------- */

static void finish_cycle(xd_server *server) {
    xd_view *target = server->cycle_view;
    server->cycle_view = nullptr;
    if (target != nullptr)
        xd_view_focus(target);
}

/* Alt+Tab: Tab steps through windows in most-recently-used order while Alt
 * is held; releasing Alt focuses the selection. */
static void cycle_step(xd_server *server, bool backwards) {
    if (wl_list_empty(&server->views))
        return;
    struct wl_list *start =
        server->cycle_view != nullptr ? &server->cycle_view->link : server->views.next;
    struct wl_list *link = start;
    do {
        link = backwards ? link->prev : link->next;
        if (link == &server->views)
            continue;
        xd_view *v = XD_CONTAINER_OF(link, xd_view, link);
        if (v->mapped) {
            server->cycle_view = v;
            /* Preview the selection by raising it without changing focus. */
            if (!v->minimized)
                wlr_scene_node_raise_to_top(&v->tree->node);
            return;
        }
    } while (link != start);
}

/* GTK sets the app ID from the GApplication ID. */
#define XD_TASKMGR_APP_ID "org.xeondesktop.TaskManager"
#define XD_SECURITY_APP_ID "org.xeondesktop.Security"

/* Brings the program's window forward if it's open, else starts it. */
static void raise_or_start(xd_server *server, const char *app_id, const char *program) {
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        const char *id = xd_view_app_id(v);
        if (v->mapped && id != nullptr && strcmp(id, app_id) == 0) {
            if (v->minimized)
                xd_view_restore(v);
            xd_view_focus(v);
            return;
        }
    }
    char *argv[] = {(char *)(uintptr_t)program, nullptr};
    xd_spawn_argv(argv);
}

static bool handle_binding(xd_server *server, uint32_t mods, xkb_keysym_t sym) {
    bool alt = mods & WLR_MODIFIER_ALT;
    bool ctrl = mods & WLR_MODIFIER_CTRL;
    bool shift = mods & WLR_MODIFIER_SHIFT;
    bool logo = mods & WLR_MODIFIER_LOGO;
    xd_view *focused = xd_view_focused(server);

    if (ctrl && alt && sym == XKB_KEY_BackSpace) {
        wl_display_terminate(server->display);
        return true;
    }
    if (server->locked)
        return false; /* the screen locker gets every other key */
    /* Win+L: Lock Computer. */
    if (logo && !ctrl && !alt && (sym == XKB_KEY_l || sym == XKB_KEY_L)) {
        server->super_alone = false;
        xd_spawn(server, server->config.locker);
        return true;
    }
    if (alt && !ctrl && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
        cycle_step(server, shift || sym == XKB_KEY_ISO_Left_Tab);
        return true;
    }
    if (alt && !ctrl && sym == XKB_KEY_F4) {
        if (focused != nullptr)
            xd_view_close(focused);
        return true;
    }
    /* Ctrl+Alt+Delete: Windows 2000's Windows Security dialog in the
     * Classic style; Task Manager in the XP styles, as XP does with the
     * Welcome screen. Ctrl+Shift+Esc: always Task Manager. */
    if (ctrl && alt && (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete)) {
        if (server->config.style == XD_STYLE_CLASSIC)
            raise_or_start(server, XD_SECURITY_APP_ID, server->config.security);
        else
            raise_or_start(server, XD_TASKMGR_APP_ID, server->config.taskmgr);
        return true;
    }
    if (ctrl && shift && !alt && sym == XKB_KEY_Escape) {
        raise_or_start(server, XD_TASKMGR_APP_ID, server->config.taskmgr);
        return true;
    }
    if (ctrl && alt && (sym == XKB_KEY_t || sym == XKB_KEY_T)) {
        xd_spawn(server, server->config.terminal);
        return true;
    }
    /* Ctrl+Esc: the Start menu, as in Windows. */
    if (ctrl && !alt && !shift && sym == XKB_KEY_Escape) {
        xd_start_menu_toggle(server);
        return true;
    }
    /* Alt+Space: the window menu, below the caption icon. */
    if (alt && !ctrl && sym == XKB_KEY_space) {
        if (focused != nullptr) {
            double x, y;
            xd_view_menu_anchor(focused, &x, &y);
            xd_window_menu_show(focused, x, y);
        }
        return true;
    }
    if (logo && sym == XKB_KEY_Up) {
        if (focused != nullptr)
            xd_view_set_maximized(focused, true);
        return true;
    }
    if (logo && sym == XKB_KEY_Down) {
        if (focused != nullptr) {
            if (focused->maximized)
                xd_view_set_maximized(focused, false);
            else
                xd_view_minimize(focused);
        }
        return true;
    }
    return false;
}

static void handle_modifiers(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_keyboard *kb = XD_CONTAINER_OF(listener, xd_keyboard, modifiers);
    xd_server *server = kb->server;
    if (server->cycle_view != nullptr &&
        !(wlr_keyboard_get_modifiers(kb->wlr_keyboard) & WLR_MODIFIER_ALT))
        finish_cycle(server);
    wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(server->seat, &kb->wlr_keyboard->modifiers);
}

/* ---- keyboard move and size (window menu) -------------------------------------- */

static bool keyboard_grab(const xd_server *server) {
    return server->mode == XD_CURSOR_KEY_MOVE || server->mode == XD_CURSOR_KEY_SIZE;
}

static const char *cursor_for_edges(uint32_t e) {
    bool l = e & WLR_EDGE_LEFT, r = e & WLR_EDGE_RIGHT, t = e & WLR_EDGE_TOP,
         b = e & WLR_EDGE_BOTTOM;
    if (t && l)
        return "nw-resize";
    if (t && r)
        return "ne-resize";
    if (b && l)
        return "sw-resize";
    if (b && r)
        return "se-resize";
    return t ? "n-resize" : b ? "s-resize" : l ? "w-resize" : r ? "e-resize" : "move";
}

/* The pointer sits on the chosen edges (or the caption while moving), as
 * in Windows. */
static void warp_to_handle(xd_server *server) {
    struct wlr_box b = server->key_box;
    double x = b.x + b.width / 2.0, y = b.y + b.height / 2.0;
    if (server->mode == XD_CURSOR_KEY_MOVE) {
        y = b.y + xd_view_insets(server->grab_view).top / 2.0;
    } else {
        uint32_t e = server->key_edges;
        if (e & WLR_EDGE_LEFT)
            x = b.x;
        if (e & WLR_EDGE_RIGHT)
            x = b.x + b.width - 1;
        if (e & WLR_EDGE_TOP)
            y = b.y;
        if (e & WLR_EDGE_BOTTOM)
            y = b.y + b.height - 1;
    }
    wlr_cursor_warp(server->cursor, nullptr, x, y);
    server->grab_x = server->cursor->x - b.x;
    server->grab_y = server->cursor->y - b.y;
    xd_cursor_set(server, server->mode == XD_CURSOR_KEY_MOVE ? "move"
                                                             : cursor_for_edges(server->key_edges));
}

static void begin_keyboard(xd_view *view, xd_cursor_mode mode) {
    xd_server *server = view->server;
    if (!view->mapped || view->minimized || view->maximized || server->mode != XD_CURSOR_PASSTHROUGH)
        return;
    if (mode == XD_CURSOR_KEY_SIZE && view->dialog)
        return; /* fixed size */
    xd_view_focus(view);
    server->mode = mode;
    server->grab_view = view;
    server->grab_box = xd_view_outer_box(view);
    server->key_box = server->grab_box;
    server->key_edges = 0;
    wlr_seat_pointer_clear_focus(server->seat);
    if (mode == XD_CURSOR_KEY_SIZE)
        xd_view_set_resizing(view, true);
    warp_to_handle(server);
}

void xd_view_begin_keyboard_move(xd_view *view) { begin_keyboard(view, XD_CURSOR_KEY_MOVE); }
void xd_view_begin_keyboard_size(xd_view *view) { begin_keyboard(view, XD_CURSOR_KEY_SIZE); }

/* Applies key_box, keeping a minimum client size. */
static void apply_key_box(xd_server *server) {
    xd_view *view = server->grab_view;
    struct wlr_box *b = &server->key_box;
    if (server->mode == XD_CURSOR_KEY_MOVE) {
        xd_view_set_position(view, b->x, b->y);
        return;
    }
    xd_insets in = xd_view_insets(view);
    int min_w = in.left + in.right + XD_MIN_CLIENT_W;
    int min_h = in.top + in.bottom + XD_MIN_CLIENT_H;
    if (b->width < min_w) {
        if (server->key_edges & WLR_EDGE_LEFT)
            b->x -= min_w - b->width;
        b->width = min_w;
    }
    if (b->height < min_h) {
        if (server->key_edges & WLR_EDGE_TOP)
            b->y -= min_h - b->height;
        b->height = min_h;
    }
    xd_view_resize_anchored(view, *b, server->key_edges);
}

static void end_keyboard(xd_server *server, bool keep) {
    xd_view *view = server->grab_view;
    xd_cursor_mode mode = server->mode;
    server->key_box = server->grab_box;
    if (!keep && view != nullptr)
        apply_key_box(server); /* Esc: back where it was */
    if (view != nullptr && mode == XD_CURSOR_KEY_SIZE)
        xd_view_set_resizing(view, false);
    server->mode = XD_CURSOR_PASSTHROUGH;
    server->grab_view = nullptr;
    server->cursor_name = nullptr;
}

/* Arrows move the window (or the chosen edges) 8px, 1px with Ctrl. */
static void keyboard_grab_key(xd_server *server, uint32_t mods, xkb_keysym_t sym) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        end_keyboard(server, true);
        return;
    }
    if (sym == XKB_KEY_Escape) {
        end_keyboard(server, false);
        return;
    }
    int dx = 0, dy = 0;
    if (sym == XKB_KEY_Left || sym == XKB_KEY_KP_Left)
        dx = -1;
    else if (sym == XKB_KEY_Right || sym == XKB_KEY_KP_Right)
        dx = 1;
    else if (sym == XKB_KEY_Up || sym == XKB_KEY_KP_Up)
        dy = -1;
    else if (sym == XKB_KEY_Down || sym == XKB_KEY_KP_Down)
        dy = 1;
    else
        return;
    int step = (mods & WLR_MODIFIER_CTRL) ? 1 : 8;
    struct wlr_box *b = &server->key_box;
    if (server->mode == XD_CURSOR_KEY_MOVE) {
        b->x += dx * step;
        b->y += dy * step;
    } else {
        uint32_t *e = &server->key_edges;
        /* The first arrow on an axis picks that edge; later ones move it. */
        if (dx != 0 && !(*e & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT))) {
            *e |= dx < 0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
        } else if (dy != 0 && !(*e & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM))) {
            *e |= dy < 0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
        } else if (dx != 0) {
            if (*e & WLR_EDGE_LEFT) {
                b->x += dx * step;
                b->width -= dx * step;
            } else {
                b->width += dx * step;
            }
        } else {
            if (*e & WLR_EDGE_TOP) {
                b->y += dy * step;
                b->height -= dy * step;
            } else {
                b->height += dy * step;
            }
        }
    }
    apply_key_box(server);
    warp_to_handle(server);
}

/* The mouse drags the window, or the chosen edges, until a click. */
static void keyboard_grab_motion(xd_server *server) {
    struct wlr_box *b = &server->key_box;
    double cx = server->cursor->x, cy = server->cursor->y;
    if (server->mode == XD_CURSOR_KEY_MOVE) {
        b->x = (int)(cx - server->grab_x);
        b->y = (int)(cy - server->grab_y);
    } else {
        uint32_t e = server->key_edges;
        int right = b->x + b->width, bottom = b->y + b->height;
        if (e & WLR_EDGE_LEFT) {
            b->x = (int)cx;
            b->width = right - b->x;
        }
        if (e & WLR_EDGE_RIGHT)
            b->width = (int)cx - b->x + 1;
        if (e & WLR_EDGE_TOP) {
            b->y = (int)cy;
            b->height = bottom - b->y;
        }
        if (e & WLR_EDGE_BOTTOM)
            b->height = (int)cy - b->y + 1;
    }
    apply_key_box(server);
}

static void handle_key(struct wl_listener *listener, void *data) {
    xd_keyboard *kb = XD_CONTAINER_OF(listener, xd_keyboard, key);
    xd_server *server = kb->server;
    const struct wlr_keyboard_key_event *event = data;
    xd_idle_activity(server);

    /* libinput keycodes are offset by 8 from XKB keycodes. */
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

    /* Keyboard move/size takes every key until it ends. */
    if (keyboard_grab(server)) {
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < nsyms; i++)
                keyboard_grab_key(server, mods, syms[i]);
        }
        return;
    }

    /* The Windows key on its own (pressed and released with nothing in
     * between) opens the Start menu; with another key it's a modifier. */
    bool super_key = false;
    for (int i = 0; i < nsyms; i++)
        super_key = super_key || syms[i] == XKB_KEY_Super_L || syms[i] == XKB_KEY_Super_R;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
        server->super_alone = super_key;
    else if (super_key && server->super_alone && !server->locked) {
        server->super_alone = false;
        xd_start_menu_toggle(server);
    }

    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++)
            handled = handle_binding(server, mods, syms[i]) || handled;
    }
    if (!handled) {
        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode,
                                     event->state);
    }
}

static void handle_keyboard_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_keyboard *kb = XD_CONTAINER_OF(listener, xd_keyboard, destroy);
    xd_unlisten(&kb->modifiers);
    xd_unlisten(&kb->key);
    xd_unlisten(&kb->destroy);
    wl_list_remove(&kb->link);
    xd_free(kb);
}

static xd_keyboard *register_keyboard(xd_server *server, struct wlr_keyboard *wlr_kb,
                                      struct wlr_input_device *device);

/* The keymap [Keyboard] asks for; empty names fall back to XKB_DEFAULT_*
 * and then the system default. A bad layout falls back the same way. */
static struct xkb_keymap *configured_keymap(const xd_keyboard_settings *k) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (ctx == nullptr)
        return nullptr;
    struct xkb_rule_names names = {
        .layout = k->layout[0] != '\0' ? k->layout : nullptr,
        .variant = k->variant[0] != '\0' ? k->variant : nullptr,
        .options = k->options[0] != '\0' ? k->options : nullptr,
        .model = k->model[0] != '\0' ? k->model : nullptr,
    };
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == nullptr) {
        wlr_log(WLR_ERROR, "cannot compile keymap (layout '%s', variant '%s', options '%s'); "
                "using the default", k->layout, k->variant, k->options);
        keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    xkb_context_unref(ctx);
    return keymap;
}

static void configure_keyboard(struct wlr_keyboard *wlr_kb, struct xkb_keymap *keymap,
                               const xd_keyboard_settings *k) {
    if (keymap != nullptr)
        wlr_keyboard_set_keymap(wlr_kb, keymap);
    wlr_keyboard_set_repeat_info(wlr_kb, k->repeat_rate, k->repeat_delay);
}

void xd_input_apply_keyboard(xd_server *server) {
    struct xkb_keymap *keymap = configured_keymap(&server->config.keyboard);
    XD_LIST_FOR_EACH(kb, &server->keyboards, xd_keyboard, link) {
        if (kb->physical)
            configure_keyboard(kb->wlr_keyboard, keymap, &server->config.keyboard);
    }
    xkb_keymap_unref(keymap);
}

static void add_keyboard(xd_server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);
    struct xkb_keymap *keymap = configured_keymap(&server->config.keyboard);
    if (keymap == nullptr) {
        wlr_log(WLR_ERROR, "cannot compile keymap for %s", device->name);
        return;
    }
    configure_keyboard(wlr_kb, keymap, &server->config.keyboard);
    xkb_keymap_unref(keymap);
    register_keyboard(server, wlr_kb, device)->physical = true;
}

static xd_keyboard *register_keyboard(xd_server *server, struct wlr_keyboard *wlr_kb,
                                      struct wlr_input_device *device) {
    xd_keyboard *kb = XD_NEW(xd_keyboard);
    kb->server = server;
    kb->wlr_keyboard = wlr_kb;
    XD_LISTEN(&wlr_kb->events.modifiers, kb, modifiers, handle_modifiers);
    XD_LISTEN(&wlr_kb->events.key, kb, key, handle_key);
    XD_LISTEN(&device->events.destroy, kb, destroy, handle_keyboard_destroy);
    wlr_seat_set_keyboard(server->seat, wlr_kb);
    wl_list_insert(&server->keyboards, &kb->link);
    return kb;
}

static void update_seat_caps(xd_server *server) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        add_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
    case WLR_INPUT_DEVICE_TABLET:
    case WLR_INPUT_DEVICE_TABLET_PAD:
    case WLR_INPUT_DEVICE_SWITCH:
        break;
    }
    update_seat_caps(server);
}

/* ---- virtual input (testing only) ------------------------------------------ */

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *vk = data;
    /* The client supplies its own keymap. No repeat: injected keys are
     * pressed and released explicitly. */
    wlr_keyboard_set_repeat_info(&vk->keyboard, 0, 0);
    register_keyboard(server, &vk->keyboard, &vk->keyboard.base);
    update_seat_caps(server);
    wlr_log(WLR_INFO, "virtual keyboard connected (testing)");
}

static void handle_new_virtual_pointer(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_virtual_pointer);
    const struct wlr_virtual_pointer_v1_new_pointer_event *e = data;
    wlr_cursor_attach_input_device(server->cursor, &e->new_pointer->pointer.base);
    update_seat_caps(server);
    wlr_log(WLR_INFO, "virtual pointer connected (testing)");
}

void xd_input_enable_virtual(xd_server *server) {
    struct wlr_virtual_keyboard_manager_v1 *kbm =
        wlr_virtual_keyboard_manager_v1_create(server->display);
    struct wlr_virtual_pointer_manager_v1 *pm =
        wlr_virtual_pointer_manager_v1_create(server->display);
    XD_LISTEN(&kbm->events.new_virtual_keyboard, server, new_virtual_keyboard,
              handle_new_virtual_keyboard);
    XD_LISTEN(&pm->events.new_virtual_pointer, server, new_virtual_pointer,
              handle_new_virtual_pointer);
    wlr_log(WLR_INFO, "virtual input enabled: any client can inject input (testing only)");
}

/* ---- pointer -------------------------------------------------------------- */

void xd_cursor_set(xd_server *server, const char *name) {
    /* Avoid reloading the image on every motion event. */
    if (server->cursor_name == name)
        return;
    server->cursor_name = name;
    wlr_cursor_set_xcursor(server->cursor, server->xcursor, name);
}

static const char *cursor_for_part(xd_frame_part part) {
    switch (part) {
    case XD_PART_TOP:
        return "n-resize";
    case XD_PART_BOTTOM:
        return "s-resize";
    case XD_PART_LEFT:
        return "w-resize";
    case XD_PART_RIGHT:
        return "e-resize";
    case XD_PART_TOP_LEFT:
        return "nw-resize";
    case XD_PART_TOP_RIGHT:
        return "ne-resize";
    case XD_PART_BOTTOM_LEFT:
        return "sw-resize";
    case XD_PART_BOTTOM_RIGHT:
        return "se-resize";
    case XD_PART_NONE:
    case XD_PART_CLIENT:
    case XD_PART_CAPTION:
    case XD_PART_SYSMENU:
    case XD_PART_CLOSE:
    case XD_PART_MAXIMIZE:
    case XD_PART_MINIMIZE:
    case XD_PART_HELP:
        break;
    }
    return "default";
}

/* What's under the cursor: a client surface, a frame, or the desktop. */
typedef struct pick {
    xd_view *view;
    struct wlr_surface *surface;
    double sx, sy;
    xd_frame_part part;
} pick;

/* Windows that draw their own decorations and have no resize margin of
 * their own (Firefox, for one) get an invisible one from us, just outside
 * their edges. */
static constexpr double CSD_MARGIN = 6;
static constexpr double CSD_CORNER = 16;

static xd_frame_part csd_margin_part(const xd_view *v, double lx, double ly) {
    struct wlr_box b = xd_view_outer_box(v);
    double x0 = b.x, y0 = b.y, x1 = b.x + b.width, y1 = b.y + b.height;
    if (lx < x0 - CSD_MARGIN || ly < y0 - CSD_MARGIN || lx >= x1 + CSD_MARGIN ||
        ly >= y1 + CSD_MARGIN)
        return XD_PART_NONE;
    bool left = lx < x0, right = lx >= x1, top = ly < y0, bottom = ly >= y1;
    if (!left && !right && !top && !bottom)
        return XD_PART_NONE; /* inside the window */
    /* Corners extend a little along each edge, as on a real frame. */
    bool near_left = lx < x0 + CSD_CORNER, near_right = lx >= x1 - CSD_CORNER;
    bool near_top = ly < y0 + CSD_CORNER, near_bottom = ly >= y1 - CSD_CORNER;
    if ((top || left) && (top ? near_left : near_top))
        return XD_PART_TOP_LEFT;
    if ((top || right) && (top ? near_right : near_top))
        return XD_PART_TOP_RIGHT;
    if ((bottom || left) && (bottom ? near_left : near_bottom))
        return XD_PART_BOTTOM_LEFT;
    if ((bottom || right) && (bottom ? near_right : near_bottom))
        return XD_PART_BOTTOM_RIGHT;
    return top ? XD_PART_TOP : bottom ? XD_PART_BOTTOM : left ? XD_PART_LEFT : XD_PART_RIGHT;
}

static pick pick_at(xd_server *server, double lx, double ly) {
    pick p = {.part = XD_PART_NONE};
    if (server->locked) {
        /* Only the screen locker's surfaces get the pointer. */
        struct wlr_scene_node *n =
            wlr_scene_node_at(&server->layer_lock->node, lx, ly, &p.sx, &p.sy);
        if (n != nullptr && n->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_surface *ss =
                wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n));
            if (ss != nullptr) {
                p.surface = ss->surface;
                p.part = XD_PART_CLIENT;
            }
        }
        return p;
    }
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, &p.sx, &p.sy);
    xd_view *under = node != nullptr ? xd_view_from_node(node) : nullptr;
    /* Resize margins of client-decorated windows stacked above what's under
     * the cursor (views are kept front to back). */
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (v == under)
            break;
        if (!v->mapped || v->minimized || !v->csd || v->maximized || v->fullscreen)
            continue;
        xd_frame_part part = csd_margin_part(v, lx, ly);
        if (part != XD_PART_NONE)
            return (pick){.view = v, .part = part};
    }
    if (node == nullptr)
        return p;
    p.view = under;
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_surface *ss =
            wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        if (ss != nullptr) {
            p.surface = ss->surface;
            p.part = XD_PART_CLIENT;
            return p;
        }
    }
    if (p.view != nullptr)
        p.part = xd_view_hit(p.view, lx, ly);
    return p;
}

static void set_hover(xd_server *server, xd_view *view, xd_frame_part part) {
    xd_frame_part hot = xd_frame_part_is_button(part) ? part : XD_PART_NONE;
    if (server->hover_view != nullptr && server->hover_view != view)
        xd_view_set_hover(server->hover_view, XD_PART_NONE, XD_PART_NONE);
    server->hover_view = hot != XD_PART_NONE ? view : nullptr;
    if (view != nullptr)
        xd_view_set_hover(view, hot, view->pressed);
}

static void process_resize(xd_server *server) {
    xd_view *view = server->grab_view;
    xd_insets in = xd_view_insets(view);
    struct wlr_box b = server->grab_box;
    int dx = (int)(server->cursor->x - server->grab_x);
    int dy = (int)(server->cursor->y - server->grab_y);
    int min_w = in.left + in.right + XD_MIN_CLIENT_W;
    int min_h = in.top + in.bottom + XD_MIN_CLIENT_H;
    xd_frame_part p = server->grab_part;

    int left = b.x;
    int top = b.y;
    int right = b.x + b.width;
    int bottom = b.y + b.height;
    if (p == XD_PART_LEFT || p == XD_PART_TOP_LEFT || p == XD_PART_BOTTOM_LEFT)
        left = XD_MIN(left + dx, right - min_w);
    if (p == XD_PART_RIGHT || p == XD_PART_TOP_RIGHT || p == XD_PART_BOTTOM_RIGHT)
        right = XD_MAX(right + dx, left + min_w);
    if (p == XD_PART_TOP || p == XD_PART_TOP_LEFT || p == XD_PART_TOP_RIGHT)
        top = XD_MIN(top + dy, bottom - min_h);
    if (p == XD_PART_BOTTOM || p == XD_PART_BOTTOM_LEFT || p == XD_PART_BOTTOM_RIGHT)
        bottom = XD_MAX(bottom + dy, top + min_h);

    uint32_t moving = 0;
    if (p == XD_PART_LEFT || p == XD_PART_TOP_LEFT || p == XD_PART_BOTTOM_LEFT)
        moving |= WLR_EDGE_LEFT;
    if (p == XD_PART_TOP || p == XD_PART_TOP_LEFT || p == XD_PART_TOP_RIGHT)
        moving |= WLR_EDGE_TOP;
    xd_view_resize_anchored(view, (struct wlr_box){left, top, right - left, bottom - top},
                            moving);
}

static void process_motion(xd_server *server, uint32_t time_msec) {
    switch (server->mode) {
    case XD_CURSOR_MOVE: {
        xd_view *v = server->grab_view;
        xd_view_set_position(v, (int)(server->cursor->x - server->grab_x),
                             (int)(server->cursor->y - server->grab_y));
        return;
    }
    case XD_CURSOR_RESIZE:
        process_resize(server);
        return;
    case XD_CURSOR_BUTTON: {
        /* The button looks pressed only while the pointer is over it. */
        xd_view *v = server->grab_view;
        xd_frame_part part = xd_view_hit(v, server->cursor->x, server->cursor->y);
        xd_view_set_hover(v, part == server->grab_part ? part : XD_PART_NONE, server->grab_part);
        return;
    }
    case XD_CURSOR_KEY_MOVE:
    case XD_CURSOR_KEY_SIZE:
        keyboard_grab_motion(server);
        return;
    case XD_CURSOR_PASSTHROUGH:
        break;
    }

    pick p = pick_at(server, server->cursor->x, server->cursor->y);
    set_hover(server, p.view, p.part);
    if (p.surface != nullptr) {
        /* The client sets its own cursor once it has pointer focus. */
        if (server->seat->pointer_state.focused_surface != p.surface)
            server->cursor_name = nullptr;
        wlr_seat_pointer_notify_enter(server->seat, p.surface, p.sx, p.sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, p.sx, p.sy);
    } else {
        wlr_seat_pointer_clear_focus(server->seat);
        xd_cursor_set(server, cursor_for_part(p.part));
    }
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_motion);
    const struct wlr_pointer_motion_event *e = data;
    wlr_cursor_move(server->cursor, &e->pointer->base, e->delta_x, e->delta_y);
    xd_idle_activity(server);
    process_motion(server, e->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_motion_absolute);
    const struct wlr_pointer_motion_absolute_event *e = data;
    wlr_cursor_warp_absolute(server->cursor, &e->pointer->base, e->x, e->y);
    xd_idle_activity(server);
    process_motion(server, e->time_msec);
}

static bool is_double_click(xd_server *server, xd_view *view, xd_frame_part part,
                            uint32_t time_msec) {
    bool dbl = server->last_click_view == view && server->last_click_part == part &&
               time_msec - server->last_click_msec <= XD_DOUBLE_CLICK_MS;
    server->last_click_view = dbl ? nullptr : view;
    server->last_click_part = part;
    server->last_click_msec = time_msec;
    return dbl;
}

static void begin_frame_action(xd_server *server, pick *p, uint32_t button,
                               uint32_t time_msec) {
    xd_view *view = p->view;
    xd_view_focus(view);
    if (button == BTN_RIGHT) {
        /* Right-click on the frame: the window menu at the pointer. */
        if (p->part == XD_PART_CAPTION || p->part == XD_PART_SYSMENU)
            xd_window_menu_show(view, server->cursor->x, server->cursor->y);
        return;
    }
    if (button != BTN_LEFT)
        return;

    if (xd_frame_part_is_button(p->part)) {
        server->mode = XD_CURSOR_BUTTON;
        server->grab_view = view;
        server->grab_part = p->part;
        xd_view_set_hover(view, p->part, p->part);
        return;
    }
    if (p->part == XD_PART_SYSMENU) {
        /* A click opens the window menu below the icon; the double-click
         * that closes the window is caught while the menu has the grab
         * (see handle_cursor_button). */
        if (is_double_click(server, view, p->part, time_msec)) {
            xd_view_close(view);
            return;
        }
        double x, y;
        xd_view_menu_anchor(view, &x, &y);
        xd_window_menu_show(view, x, y);
        return;
    }
    if (p->part == XD_PART_CAPTION) {
        if (is_double_click(server, view, p->part, time_msec)) {
            xd_view_set_maximized(view, !view->maximized);
            return;
        }
        if (view->maximized)
            return;
        server->mode = XD_CURSOR_MOVE;
        server->grab_view = view;
        server->grab_x = server->cursor->x - view->x;
        server->grab_y = server->cursor->y - view->y;
        return;
    }
    if (xd_frame_part_is_border(p->part) && !view->maximized) {
        server->mode = XD_CURSOR_RESIZE;
        server->grab_view = view;
        server->grab_part = p->part;
        server->grab_x = server->cursor->x;
        server->grab_y = server->cursor->y;
        server->grab_box = xd_view_outer_box(view);
        xd_view_set_resizing(view, true);
    }
}

static void end_grab(xd_server *server) {
    xd_view *view = server->grab_view;
    xd_cursor_mode mode = server->mode;
    server->mode = XD_CURSOR_PASSTHROUGH;
    server->grab_view = nullptr;
    if (view == nullptr)
        return;

    if (mode == XD_CURSOR_RESIZE) {
        xd_view_set_resizing(view, false);
    } else if (mode == XD_CURSOR_BUTTON) {
        xd_frame_part part = server->grab_part;
        bool fire = view->hot == part;
        xd_view_set_hover(view, XD_PART_NONE, XD_PART_NONE);
        if (!fire)
            return;
        switch (part) {
        case XD_PART_CLOSE:
            xd_view_close(view);
            break;
        case XD_PART_MAXIMIZE:
            xd_view_set_maximized(view, !view->maximized);
            break;
        case XD_PART_MINIMIZE:
            xd_view_minimize(view);
            break;
        case XD_PART_HELP:
        case XD_PART_NONE:
        case XD_PART_CLIENT:
        case XD_PART_CAPTION:
        case XD_PART_SYSMENU:
        case XD_PART_TOP:
        case XD_PART_BOTTOM:
        case XD_PART_LEFT:
        case XD_PART_RIGHT:
        case XD_PART_TOP_LEFT:
        case XD_PART_TOP_RIGHT:
        case XD_PART_BOTTOM_LEFT:
        case XD_PART_BOTTOM_RIGHT:
            break;
        }
    }
}

/* Whether the seat delivered a press of `button` that's still held. */
static bool seat_has_button(const struct wlr_seat *seat, uint32_t button) {
    for (size_t i = 0; i < seat->pointer_state.button_count; i++) {
        if (seat->pointer_state.buttons[i].button == button)
            return true;
    }
    return false;
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_button);
    const struct wlr_pointer_button_event *e = data;
    xd_idle_activity(server);
    if (server->locked) {
        wlr_seat_pointer_notify_button(server->seat, e->time_msec, e->button, e->state);
        return;
    }

    /* A click ends keyboard move/size where the window is. */
    if (keyboard_grab(server)) {
        if (e->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            end_keyboard(server, true);
            server->ignore_release = true;
        }
        return;
    }
    if (e->state == WL_POINTER_BUTTON_STATE_RELEASED && server->ignore_release) {
        server->ignore_release = false;
        process_motion(server, e->time_msec);
        return;
    }
    if (e->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->mode != XD_CURSOR_PASSTHROUGH) {
            end_grab(server);
            /* A client that asked for the move or resize had the press:
             * it must get the release too, or it keeps waiting for it and
             * ignores later clicks (Firefox's tab strip and close button
             * after dragging the window by it). */
            if (seat_has_button(server->seat, e->button))
                wlr_seat_pointer_notify_button(server->seat, e->time_msec, e->button, e->state);
            process_motion(server, e->time_msec);
            return;
        }
        wlr_seat_pointer_notify_button(server->seat, e->time_msec, e->button, e->state);
        return;
    }

    if (server->mode != XD_CURSOR_PASSTHROUGH)
        return;
    /* An open popup (menu) owns the pointer: a click outside it must reach
     * the grab so the menu closes, not start a window drag. */
    if (wlr_seat_pointer_has_grab(server->seat)) {
        /* Double-clicking the caption icon closes the window even though
         * its first click opened the window menu. */
        pick p = pick_at(server, server->cursor->x, server->cursor->y);
        if (p.view != nullptr && p.part == XD_PART_SYSMENU && e->button == BTN_LEFT &&
            is_double_click(server, p.view, p.part, e->time_msec))
            xd_view_close(p.view);
        wlr_seat_pointer_notify_button(server->seat, e->time_msec, e->button, e->state);
        return;
    }
    pick p = pick_at(server, server->cursor->x, server->cursor->y);
    if (p.surface != nullptr) {
        if (p.view != nullptr)
            xd_view_focus(p.view);
        wlr_seat_pointer_notify_button(server->seat, e->time_msec, e->button, e->state);
    } else if (p.view != nullptr) {
        begin_frame_action(server, &p, e->button, e->time_msec);
    }
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_axis);
    const struct wlr_pointer_axis_event *e = data;
    xd_idle_activity(server);
    wlr_seat_pointer_notify_axis(server->seat, e->time_msec, e->orientation, e->delta,
                                 e->delta_discrete, e->source, e->relative_direction);
}

static void handle_cursor_frame(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void handle_request_cursor(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_cursor);
    const struct wlr_seat_pointer_request_set_cursor_event *e = data;
    if (server->mode == XD_CURSOR_PASSTHROUGH &&
        server->seat->pointer_state.focused_client == e->seat_client) {
        server->cursor_name = nullptr;
        wlr_cursor_set_surface(server->cursor, e->surface, e->hotspot_x, e->hotspot_y);
    }
}

static void handle_request_cursor_shape(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_cursor_shape);
    const struct wlr_cursor_shape_manager_v1_request_set_shape_event *e = data;
    if (server->mode == XD_CURSOR_PASSTHROUGH &&
        server->seat->pointer_state.focused_client == e->seat_client)
        xd_cursor_set(server, wlr_cursor_shape_v1_name(e->shape));
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_set_selection);
    const struct wlr_seat_request_set_selection_event *e = data;
    wlr_seat_set_selection(server->seat, e->source, e->serial);
}

static int handle_sigchld([[maybe_unused]] int sig, void *data) {
    xd_server *server = data;
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid != server->shell_pid)
            continue;
        server->shell_pid = 0;
        if (WIFEXITED(status))
            wlr_log(WLR_INFO, "shell exited with status %d", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            wlr_log(WLR_ERROR, "shell killed by signal %d; restarting", WTERMSIG(status));
        /* Only a crash or kill restarts it; a clean exit was deliberate. */
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            xd_shell_start(server);
    }
    return 0;
}

void xd_input_init(xd_server *server) {
    wl_list_init(&server->keyboards);

    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->xcursor = wlr_xcursor_manager_create(nullptr, 24);
    XD_LISTEN(&server->cursor->events.motion, server, cursor_motion, handle_cursor_motion);
    XD_LISTEN(&server->cursor->events.motion_absolute, server, cursor_motion_absolute,
              handle_cursor_motion_absolute);
    XD_LISTEN(&server->cursor->events.button, server, cursor_button, handle_cursor_button);
    XD_LISTEN(&server->cursor->events.axis, server, cursor_axis, handle_cursor_axis);
    XD_LISTEN(&server->cursor->events.frame, server, cursor_frame, handle_cursor_frame);

    server->seat = wlr_seat_create(server->display, "seat0");
    XD_LISTEN(&server->backend->events.new_input, server, new_input, handle_new_input);
    XD_LISTEN(&server->seat->events.request_set_cursor, server, request_cursor,
              handle_request_cursor);
    XD_LISTEN(&server->seat->events.request_set_selection, server, request_set_selection,
              handle_request_set_selection);

    struct wlr_cursor_shape_manager_v1 *shapes =
        wlr_cursor_shape_manager_v1_create(server->display, 1);
    XD_LISTEN(&shapes->events.request_set_shape, server, request_cursor_shape,
              handle_request_cursor_shape);

    server->sigchld = wl_event_loop_add_signal(server->loop, SIGCHLD, handle_sigchld, server);
}
