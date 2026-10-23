/* SPDX-License-Identifier: MIT
 * Keyboard, pointer and seat handling; window-manager key bindings; frame
 * interaction (move, resize, caption buttons). Touchscreens, drawing
 * tablets and touchpad gestures are touch.c's, tablet.c's and
 * gestures.c's; they work the pointer here where a program can't take
 * them.
 */
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "../core/alloc.h"
#include "grabkeys.h"
#include "server.h"

pid_t xd_spawn_argv(char *const argv[]) { return xd_spawn_with(argv, nullptr, environ); }

pid_t xd_spawn_with(char *const argv[], const posix_spawn_file_actions_t *actions,
                    char *const envp[]) {
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
    int err = posix_spawnp(&pid, argv[0], actions, &attr, argv, envp);
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

/* Restarts at most 5 times in 30 seconds, so a program that crashes on
 * startup doesn't spin. */
static bool may_start(struct timespec *window, uint32_t *starts) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - window->tv_sec > 30) {
        *window = now;
        *starts = 0;
    }
    return ++*starts <= 5;
}

void xd_shell_start(xd_server *server) {
    if (server->config.shell == nullptr || server->terminating)
        return;
    if (!may_start(&server->shell_restart_window, &server->shell_restarts)) {
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
    /* On a connection of its own, by which it's known (trust.c). */
    server->shell_pid = xd_trust_spawn(server, argv, &server->shell_client);
    xd_free(scheme);
    xd_free(style);
    xd_free(shell);
}

void xd_polkit_start(xd_server *server) {
    if (server->config.polkit == nullptr || server->terminating)
        return;
    if (!may_start(&server->polkit_restart_window, &server->polkit_restarts)) {
        wlr_log(WLR_ERROR, "polkit agent keeps crashing; not restarting it");
        return;
    }
    char *agent = xd_str_dup_cstr(xd_str_from_cstr(server->config.polkit));
    char *argv[] = {agent, nullptr};
    server->polkit_pid = xd_spawn_argv(argv);
    xd_free(agent);
}

/* ---- keyboard --------------------------------------------------------------- */

static void finish_cycle(xd_server *server) {
    xd_view *target = server->cycle_view;
    server->cycle_view = nullptr;
    xd_switcher_hide(server);
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
        xd_view *view = XD_CONTAINER_OF(link, xd_view, link);
        if (view->mapped) {
            server->cycle_view = view;
            /* Preview the selection by raising it without changing focus. */
            if (!view->minimized)
                wlr_scene_node_raise_to_top(&view->tree->node);
            xd_switcher_show(server);
            return;
        }
    } while (link != start);
}

/* The same from a touchpad's three-finger swipe (gestures.c). */
void xd_input_cycle_step(xd_server *server, bool backwards) { cycle_step(server, backwards); }
void xd_input_cycle_finish(xd_server *server) { finish_cycle(server); }
void xd_input_cycle_cancel(xd_server *server) {
    server->cycle_view = nullptr;
    xd_switcher_hide(server);
}

/* GTK sets the app ID from the GApplication ID. */
#define XD_TASKMGR_APP_ID "org.xeondesktop.TaskManager"
#define XD_SECURITY_APP_ID "org.xeondesktop.Security"

/* Brings the program's window forward if it's open, else starts it. */
static void raise_or_start(xd_server *server, const char *app_id, const char *program) {
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        const char *id = xd_view_app_id(view);
        if (view->mapped && id != nullptr && strcmp(id, app_id) == 0) {
            if (view->minimized)
                xd_view_restore(view);
            xd_view_focus(view);
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
    /* Ctrl+Alt+F1..F12 (the keymap's XF86Switch_VT_n): another virtual
     * terminal, also from the lock screen. Nested sessions have none. */
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12 &&
        server->session != nullptr) {
        wlr_session_change_vt(server->session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
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
        /* On the desktop it's the shell's: Shut Down, as in Windows. */
        if (focused == nullptr && server->desktop_focus != nullptr)
            return false;
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
    /* Print Screen copies the screen to the clipboard and Alt+Print
     * Screen the active window (Alt makes it Sys_Req), as in Windows;
     * Win+Shift+S starts a snip, as in Windows 10. */
    if (!ctrl && !logo && (sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req)) {
        bool window = alt || sym == XKB_KEY_Sys_Req;
        char *argv[] = {(char *)(uintptr_t)server->config.snip,
                        (char *)(uintptr_t)(window ? "--copy-window" : "--copy-screen"), nullptr};
        xd_spawn_argv(argv);
        return true;
    }
    if (logo && shift && !ctrl && !alt && (sym == XKB_KEY_s || sym == XKB_KEY_S)) {
        char *argv[] = {(char *)(uintptr_t)server->config.snip, (char *)(uintptr_t)"--new",
                        nullptr};
        xd_spawn_argv(argv);
        return true;
    }
    /* Screen recording: Win+Shift+R an area (Windows 11's Snipping Tool),
     * Win+Alt+R the active window (the Game Bar's); again, it stops. */
    if (logo && !ctrl && (shift != alt) && (sym == XKB_KEY_r || sym == XKB_KEY_R)) {
        char *argv[] = {(char *)(uintptr_t)server->config.snip,
                        (char *)(uintptr_t)(shift ? "--record=area" : "--record"), nullptr};
        xd_spawn_argv(argv);
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
            xd_window_menu_show(focused, x, y, true);
        }
        return true;
    }
    /* Windows 7's Win+Shift+Up (as tall as the screen) and
     * Win+Shift+Left/Right (to the next monitor), Aero only. */
    if (logo && shift && server->config.style == XD_STYLE_AERO &&
        (sym == XKB_KEY_Up || sym == XKB_KEY_Left || sym == XKB_KEY_Right)) {
        if (focused != nullptr && sym == XKB_KEY_Up)
            xd_snap_tall(focused);
        else if (focused != nullptr)
            xd_snap_to_monitor(focused, sym == XKB_KEY_Left ? -1 : 1);
        return true;
    }
    if (logo && sym == XKB_KEY_Up) {
        if (focused != nullptr)
            xd_view_set_maximized(focused, true);
        return true;
    }
    /* Win+Space: Aero Peek while the Windows key is held (Aero only). */
    if (logo && sym == XKB_KEY_space && server->config.style == XD_STYLE_AERO) {
        xd_view_peek(server, true);
        return true;
    }
    /* Aero Snap from the keyboard (Aero only). */
    if (logo && (sym == XKB_KEY_Left || sym == XKB_KEY_Right) &&
        server->config.style == XD_STYLE_AERO) {
        if (focused != nullptr)
            xd_snap_key(focused, sym == XKB_KEY_Left ? XD_SNAP_LEFT : XD_SNAP_RIGHT);
        return true;
    }
    if (logo && sym == XKB_KEY_Down) {
        if (focused != nullptr && focused->snapped != XD_SNAP_NONE) {
            /* A snapped window gets its size back first. */
            struct wlr_box box = focused->snap_restore;
            focused->snapped = XD_SNAP_NONE;
            xd_view_set_outer_box(focused, box);
            return true;
        }
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

/* Puts every keyboard but `except` on layout `group` (those whose keymap
 * has it), so the layout is the session's, not each device's. */
static void sync_group(xd_server *server, uint32_t group, const xd_keyboard *except) {
    server->kb_group_syncing = true;
    XD_LIST_FOR_EACH(kb, &server->keyboards, xd_keyboard, link) {
        struct wlr_keyboard *keyboard = kb->wlr_keyboard;
        if (kb == except || keyboard->keymap == nullptr ||
            group >= xkb_keymap_num_layouts(keyboard->keymap) || keyboard->modifiers.group == group)
            continue;
        wlr_keyboard_notify_modifiers(keyboard, keyboard->modifiers.depressed,
                                      keyboard->modifiers.latched, keyboard->modifiers.locked,
                                      group);
    }
    server->kb_group_syncing = false;
}

static uint32_t layouts_of(const struct wlr_keyboard *keyboard) {
    return keyboard->keymap != nullptr ? xkb_keymap_num_layouts(keyboard->keymap) : 0;
}

static struct xkb_keymap *configured_keymap(const xd_keyboard_settings *keyboard);

/* How many layouts the session has: [Keyboard] Layout's (or a keyboard's
 * own, if it brought more). */
static uint32_t session_layouts(xd_server *server) {
    if (server->kb_layouts == 0) {
        struct xkb_keymap *keymap = configured_keymap(&server->config.keyboard);
        server->kb_layouts = keymap != nullptr ? xkb_keymap_num_layouts(keymap) : 1;
        xkb_keymap_unref(keymap);
    }
    uint32_t count = server->kb_layouts;
    XD_LIST_FOR_EACH(kb, &server->keyboards, xd_keyboard, link) {
        count = XD_MAX(count, layouts_of(kb->wlr_keyboard));
    }
    return count;
}

/* Every keyboard that has it switches to `group`; the focused client and
 * the shell hear of it. */
static void switch_group(xd_server *server, uint32_t group, const xd_keyboard *except) {
    sync_group(server, group, except);
    struct wlr_keyboard *seat_kb = wlr_seat_get_keyboard(server->seat);
    if (seat_kb != nullptr)
        wlr_seat_keyboard_notify_modifiers(server->seat, &seat_kb->modifiers);
    bool changed = group != server->kb_group;
    if (changed)
        wlr_log(WLR_DEBUG, "keyboard layout %u", group);
    server->kb_group = group;
    if (changed)
        xd_arrange_keyboard_layout(server);
}

void xd_input_set_keyboard_layout(xd_server *server, uint32_t group) {
    if (group < session_layouts(server))
        switch_group(server, group, nullptr);
}

/* A keyboard came, or the keymap changed: everything takes the session's
 * layout, or the first one if the keymap no longer has it. */
static void adopt_group(xd_server *server) {
    uint32_t group = server->kb_group < session_layouts(server) ? server->kb_group : 0;
    server->kb_group = UINT32_MAX; /* tell the shell in any case */
    switch_group(server, group, nullptr);
}

static void handle_modifiers(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_keyboard *kb = XD_CONTAINER_OF(listener, xd_keyboard, modifiers);
    xd_server *server = kb->server;
    /* Our own layout change on a keyboard not in use: nothing to tell. */
    if (server->kb_group_syncing)
        return;
    if (server->cycle_view != nullptr &&
        !(wlr_keyboard_get_modifiers(kb->wlr_keyboard) & WLR_MODIFIER_ALT))
        finish_cycle(server);
    /* Win+Space's peek ends with the Windows key. */
    if (server->peeking && !(wlr_keyboard_get_modifiers(kb->wlr_keyboard) & WLR_MODIFIER_LOGO))
        xd_view_peek(server, false);
    /* While the input method has the keyboard, it passes them on. */
    if (!xd_ime_modifiers(kb)) {
        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        wlr_seat_keyboard_notify_modifiers(server->seat, &kb->wlr_keyboard->modifiers);
    }
    /* wlroots' drag grab swallows modifiers, but the client being dragged
     * over picks copy, move or shortcut by Ctrl, Shift and Alt (Windows'
     * rules): it still gets them. */
    if (server->seat->drag != nullptr)
        wlr_seat_keyboard_send_modifiers(server->seat, &kb->wlr_keyboard->modifiers);
    /* The switching keys ([Keyboard] Options' grp:, Left Alt+Shift in
     * XP) changed the layout: the other keyboards follow. */
    /* (A keyboard with one layout, such as a test's virtual one, has no
     * say.) */
    uint32_t group = kb->wlr_keyboard->modifiers.group;
    if (layouts_of(kb->wlr_keyboard) > 1 && group != server->kb_group)
        switch_group(server, group, kb);
}

/* ---- keyboard move and size (window menu) -------------------------------------- */

static bool keyboard_grab(const xd_server *server) {
    return server->mode == XD_CURSOR_KEY_MOVE || server->mode == XD_CURSOR_KEY_SIZE;
}

static const char *cursor_for_edges(uint32_t edges) {
    bool left = edges & WLR_EDGE_LEFT, right = edges & WLR_EDGE_RIGHT, top = edges & WLR_EDGE_TOP,
         bottom = edges & WLR_EDGE_BOTTOM;
    if (top && left)
        return "nw-resize";
    if (top && right)
        return "ne-resize";
    if (bottom && left)
        return "sw-resize";
    if (bottom && right)
        return "se-resize";
    return top ? "n-resize" : bottom ? "s-resize" : left ? "w-resize" : right ? "e-resize" : "move";
}

/* The pointer sits on the chosen edges (or the caption while moving), as
 * in Windows. */
static void warp_to_handle(xd_server *server) {
    struct wlr_box box = server->key_box;
    double x = box.x + box.width / 2.0, y = box.y + box.height / 2.0;
    if (server->mode == XD_CURSOR_KEY_MOVE) {
        y = box.y + xd_view_insets(server->grab_view).top / 2.0;
    } else {
        uint32_t edges = server->key_edges;
        if (edges & WLR_EDGE_LEFT)
            x = box.x;
        if (edges & WLR_EDGE_RIGHT)
            x = box.x + box.width - 1;
        if (edges & WLR_EDGE_TOP)
            y = box.y;
        if (edges & WLR_EDGE_BOTTOM)
            y = box.y + box.height - 1;
    }
    wlr_cursor_warp(server->cursor, nullptr, x, y);
    server->grab_x = server->cursor->x - box.x;
    server->grab_y = server->cursor->y - box.y;
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

/* Applies key_box, keeping a minimum client size; with outline dragging,
 * only its outline moves until Enter (`done`). */
static void apply_key_box_now(xd_server *server, bool done) {
    xd_view *view = server->grab_view;
    struct wlr_box *box = &server->key_box;
    bool outline = !done && xd_outline_dragging(server);
    if (server->mode == XD_CURSOR_KEY_MOVE) {
        if (outline)
            xd_outline_show(server, *box);
        else
            xd_view_set_position(view, box->x, box->y);
        return;
    }
    xd_insets in = xd_view_insets(view);
    int min_w = in.left + in.right + XD_MIN_CLIENT_W;
    int min_h = in.top + in.bottom + XD_MIN_CLIENT_H;
    if (box->width < min_w) {
        if (server->key_edges & WLR_EDGE_LEFT)
            box->x -= min_w - box->width;
        box->width = min_w;
    }
    if (box->height < min_h) {
        if (server->key_edges & WLR_EDGE_TOP)
            box->y -= min_h - box->height;
        box->height = min_h;
    }
    if (outline)
        xd_outline_show(server, *box);
    else
        xd_view_resize_anchored(view, *box, server->key_edges);
}

static void apply_key_box(xd_server *server) { apply_key_box_now(server, false); }

static void end_keyboard(xd_server *server, bool keep) {
    xd_view *view = server->grab_view;
    xd_cursor_mode mode = server->mode;
    /* Enter after dragging an outline: the window goes there now. */
    if (keep && view != nullptr && server->outline != nullptr)
        apply_key_box_now(server, true);
    xd_outline_hide(server);
    server->key_box = server->grab_box;
    if (!keep && view != nullptr)
        apply_key_box_now(server, true); /* Esc: back where it was */
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
    struct wlr_box *box = &server->key_box;
    if (server->mode == XD_CURSOR_KEY_MOVE) {
        box->x += dx * step;
        box->y += dy * step;
    } else {
        uint32_t *edges = &server->key_edges;
        /* The first arrow on an axis picks that edge; later ones move it. */
        if (dx != 0 && !(*edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT))) {
            *edges |= dx < 0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
        } else if (dy != 0 && !(*edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM))) {
            *edges |= dy < 0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
        } else if (dx != 0) {
            if (*edges & WLR_EDGE_LEFT) {
                box->x += dx * step;
                box->width -= dx * step;
            } else {
                box->width += dx * step;
            }
        } else {
            if (*edges & WLR_EDGE_TOP) {
                box->y += dy * step;
                box->height -= dy * step;
            } else {
                box->height += dy * step;
            }
        }
    }
    apply_key_box(server);
    warp_to_handle(server);
}

/* The mouse drags the window, or the chosen edges, until a click. */
static void keyboard_grab_motion(xd_server *server) {
    struct wlr_box *box = &server->key_box;
    double cx = server->cursor->x, cy = server->cursor->y;
    if (server->mode == XD_CURSOR_KEY_MOVE) {
        box->x = (int)(cx - server->grab_x);
        box->y = (int)(cy - server->grab_y);
    } else {
        uint32_t edges = server->key_edges;
        int right = box->x + box->width, bottom = box->y + box->height;
        if (edges & WLR_EDGE_LEFT) {
            box->x = (int)cx;
            box->width = right - box->x;
        }
        if (edges & WLR_EDGE_RIGHT)
            box->width = (int)cx - box->x + 1;
        if (edges & WLR_EDGE_TOP) {
            box->y = (int)cy;
            box->height = bottom - box->y;
        }
        if (edges & WLR_EDGE_BOTTOM)
            box->height = (int)cy - box->y + 1;
    }
    apply_key_box(server);
}

/* ---- StickyKeys and BounceKeys ([Accessibility]; the others are in access.c) ---- */

/* The xkb modifier a modifier key sets, or 0. */
static uint32_t modifier_of(struct xkb_keymap *keymap, xkb_keysym_t sym) {
    const char *name = nullptr;
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
        name = XKB_MOD_NAME_SHIFT;
        break;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
        name = XKB_MOD_NAME_CTRL;
        break;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
        name = XKB_MOD_NAME_ALT;
        break;
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
        name = XKB_MOD_NAME_LOGO;
        break;
    case XKB_KEY_ISO_Level3_Shift: /* AltGr */
        name = "Mod5";
        break;
    default:
        return 0;
    }
    xkb_mod_index_t i =
        keymap != nullptr ? xkb_keymap_mod_get_index(keymap, name) : XKB_MOD_INVALID;
    return i != XKB_MOD_INVALID && i < 32 ? 1u << i : 0;
}

/* Puts StickyKeys' modifiers into the keyboard's state, next to what xkb
 * tracks itself (Caps Lock, held keys); clients get them as latched and
 * locked modifiers. */
static void sticky_update(xd_keyboard *kb, uint32_t latched, uint32_t locked) {
    struct wlr_keyboard *keyboard = kb->wlr_keyboard;
    if (keyboard->xkb_state == nullptr)
        return;
    uint32_t own_latched = keyboard->modifiers.latched & ~kb->sticky_latched;
    uint32_t own_locked = keyboard->modifiers.locked & ~kb->sticky_locked;
    kb->sticky_latched = latched;
    kb->sticky_locked = locked;
    wlr_keyboard_notify_modifiers(keyboard, keyboard->modifiers.depressed, own_latched | latched,
                                  own_locked | locked, keyboard->modifiers.group);
}

/* StickyKeys, as in Windows: a modifier pressed and released on its own
 * applies to the next key; pressed twice it stays on (if StickyKeysLock)
 * until pressed once more. Called for every key event. */
static void sticky_key(xd_keyboard *kb, const xkb_keysym_t *syms, int nsyms, bool pressed) {
    const xd_access_settings *access_settings = &kb->server->config.access;
    if (!access_settings->sticky_keys)
        return;
    uint32_t mod = 0;
    for (int i = 0; i < nsyms; i++)
        mod |= modifier_of(kb->wlr_keyboard->keymap, syms[i]);
    if (mod == 0) {
        /* Another key: a modifier held with it is a normal chord. */
        kb->sticky_alone = 0;
        return;
    }
    if (pressed) {
        kb->sticky_alone = mod;
        return;
    }
    if (kb->sticky_alone != mod)
        return;
    kb->sticky_alone = 0;
    uint32_t latched = kb->sticky_latched, locked = kb->sticky_locked;
    if (locked & mod) {
        locked &= ~mod;
    } else if (latched & mod) {
        latched &= ~mod;
        if (access_settings->sticky_lock)
            locked |= mod;
    } else {
        latched |= mod;
    }
    sticky_update(kb, latched, locked);
}

/* FilterKeys' BounceKeys: true if this event repeats a key released less
 * than bounce_ms ago, and is to be ignored (its release too). */
static bool bounced(xd_keyboard *kb, const struct wlr_keyboard_key_event *keyboard_key_event) {
    const xd_access_settings *access_settings = &kb->server->config.access;
    if (keyboard_key_event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (kb->bounced_keycode == keyboard_key_event->keycode + 1) {
            kb->bounced_keycode = 0;
            return true;
        }
        kb->bounce_keycode = keyboard_key_event->keycode + 1; /* 0 = none */
        kb->bounce_msec = keyboard_key_event->time_msec;
        return false;
    }
    if (access_settings->filter_keys && !access_settings->slow_keys &&
        kb->bounce_keycode == keyboard_key_event->keycode + 1 &&
        keyboard_key_event->time_msec - kb->bounce_msec < (uint32_t)access_settings->bounce_ms) {
        kb->bounced_keycode = keyboard_key_event->keycode + 1;
        return true;
    }
    return false;
}

static void apply_repeat(xd_server *server, struct wlr_keyboard *keyboard);

void xd_input_apply_access(xd_server *server) {
    XD_LIST_FOR_EACH(kb, &server->keyboards, xd_keyboard, link) {
        kb->sticky_alone = 0;
        kb->bounced_keycode = 0;
        if (kb->sticky_latched != 0 || kb->sticky_locked != 0)
            sticky_update(kb, 0, 0);
        apply_repeat(server, kb->wlr_keyboard);
    }
    xd_access_apply(server);
    const xd_access_settings *access_settings = &server->config.access;
    wlr_log(WLR_INFO,
            "accessibility: StickyKeys %s%s, FilterKeys %s (%s, %d ms), ToggleKeys %s, "
            "MouseKeys %s (%d px/s)",
            access_settings->sticky_keys ? "on" : "off",
            access_settings->sticky_keys && access_settings->sticky_lock ? " (twice locks)" : "",
            access_settings->filter_keys ? "on" : "off",
            access_settings->slow_keys ? "SlowKeys" : "BounceKeys",
            access_settings->slow_keys ? access_settings->slow_ms : access_settings->bounce_ms,
            access_settings->toggle_keys ? "on" : "off", access_settings->mouse_keys ? "on" : "off",
            access_settings->mouse_keys_speed);
}

static void handle_key(struct wl_listener *listener, void *data) {
    xd_keyboard *kb = XD_CONTAINER_OF(listener, xd_keyboard, key);
    const struct wlr_keyboard_key_event *event = data;
    xd_idle_activity(kb->server);
    /* The input method's keyboard sends keys that went through the
     * filters already, on their way to it. */
    if (!xd_ime_own_keyboard(kb) && (bounced(kb, event) || xd_access_key(kb, event)))
        return;
    xd_input_key(kb, event);
}

/* XF86PowerOff, XF86Sleep or XF86Suspend: told to the shell (on press)
 * and kept from windows. Without a shell to hear them they go on as any
 * key. */
static bool power_key(xd_server *server, const xkb_keysym_t *syms, int nsyms, bool pressed) {
    for (int i = 0; i < nsyms; i++) {
        xd_power_event power_event;
        if (syms[i] == XKB_KEY_XF86PowerOff)
            power_event = XD_POWER_EVENT_POWER;
        else if (syms[i] == XKB_KEY_XF86Sleep || syms[i] == XKB_KEY_XF86Suspend)
            power_event = XD_POWER_EVENT_SLEEP;
        else
            continue;
        if (!pressed)
            return server->arranger_resource != nullptr;
        return xd_arrange_power_button(server, power_event);
    }
    return false;
}

void xd_input_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *event) {
    xd_server *server = kb->server;
    /* libinput keycodes are offset by 8 from XKB keycodes. */
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
    bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    sticky_key(kb, syms, nsyms, pressed);
    /* A latched modifier is used up by the next key, once it's been sent
     * (with the modifier) below. */
    bool unlatch = pressed && kb->sticky_latched != 0 && kb->sticky_alone == 0;
    for (int i = 0; unlatch && i < nsyms; i++)
        unlatch = modifier_of(kb->wlr_keyboard->keymap, syms[i]) == 0;

    /* The power and sleep buttons are the shell's (Power Options), even
     * mid-grab or with the screen locked, as logind's would be. */
    if (power_key(server, syms, nsyms, pressed))
        return;
    if (!xd_ime_own_keyboard(kb))
        xd_constraints_key(server, syms, nsyms, pressed);

    /* Keyboard move/size takes every key until it ends. */
    if (keyboard_grab(server)) {
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < nsyms; i++)
                keyboard_grab_key(server, mods, syms[i]);
        }
        return;
    }

    /* Keys the input method passes back saw the shortcuts (and the
     * Windows key) on their way to it; the rest go to it while it has the
     * keyboard. */
    bool from_ime = xd_ime_own_keyboard(kb);
    /* A program taking the shortcuts (a virtual machine, a remote
     * desktop) gets the Windows key and all but grabkeys.h's keys. */
    bool inhibited = xd_shortcuts_inhibited(server);

    /* The Windows key on its own (pressed and released with nothing in
     * between) opens the Start menu; with another key it's a modifier. */
    if (!from_ime && !inhibited) {
        bool super_key = false;
        for (int i = 0; i < nsyms; i++)
            super_key = super_key || syms[i] == XKB_KEY_Super_L || syms[i] == XKB_KEY_Super_R;
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
            server->super_alone = super_key;
        else if (super_key && server->super_alone && !server->locked) {
            server->super_alone = false;
            xd_constraints_release(server);
            xd_start_menu_toggle(server);
        }
    }

    bool handled = false;
    /* Esc ends the VirtualBox host's drag, as it ends any drag. */
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && !from_ime) {
        for (int i = 0; i < nsyms; i++)
            if (syms[i] == XKB_KEY_Escape && xd_relay_cancel(server))
                return;
    }
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && !from_ime) {
        xd_grab_mods held = {
            .ctrl = mods & WLR_MODIFIER_CTRL,
            .alt = mods & WLR_MODIFIER_ALT,
            .shift = mods & WLR_MODIFIER_SHIFT,
            .logo = mods & WLR_MODIFIER_LOGO,
        };
        for (int i = 0; i < nsyms; i++) {
            if (!inhibited || xd_grab_key_kept(held, syms[i]))
                handled = handle_binding(server, mods, syms[i]) || handled;
        }
        /* One of our shortcuts: a captured pointer is the desktop's again. */
        if (handled)
            xd_constraints_release(server);
    }
    if (!handled && !from_ime)
        handled = xd_ime_key(kb, event);
    if (!handled) {
        if (wlr_seat_get_keyboard(server->seat) != kb->wlr_keyboard || kb->resend_layout) {
            wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
            /* The client starts a keymap it's sent afresh, on the first
             * layout: tell it the session's, which this keyboard is on. */
            wlr_seat_keyboard_notify_modifiers(server->seat, &kb->wlr_keyboard->modifiers);
            kb->resend_layout = false;
        }
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode,
                                     event->state);
    }
    if (unlatch)
        sticky_update(kb, 0, kb->sticky_locked);
}

/* A keyboard's keymap arrived (a virtual keyboard sends its own) or
 * changed: it's put on the session's layout, if it has it. Clients get
 * the keymap after us (from the seat), so they hear of the layout with
 * the next key. */
static void handle_keymap(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_keyboard *kb = XD_CONTAINER_OF(listener, xd_keyboard, keymap);
    if (!kb->server->kb_group_syncing)
        sync_group(kb->server, kb->server->kb_group, nullptr);
    kb->resend_layout = true;
}

static void handle_keyboard_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_keyboard *kb = XD_CONTAINER_OF(listener, xd_keyboard, destroy);
    xd_access_keyboard_gone(kb);
    xd_unlisten(&kb->modifiers);
    xd_unlisten(&kb->keymap);
    xd_unlisten(&kb->key);
    xd_unlisten(&kb->destroy);
    wl_list_remove(&kb->link);
    xd_free(kb);
}

static xd_keyboard *register_keyboard(xd_server *server, struct wlr_keyboard *wlr_kb,
                                      struct wlr_input_device *device);

/* The keymap [Keyboard] asks for; empty names fall back to XKB_DEFAULT_*
 * and then the system default. A bad layout falls back the same way. */
static struct xkb_keymap *configured_keymap(const xd_keyboard_settings *keyboard) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (ctx == nullptr)
        return nullptr;
    struct xkb_rule_names names = {
        .layout = keyboard->layout[0] != '\0' ? keyboard->layout : nullptr,
        .variant = keyboard->variant[0] != '\0' ? keyboard->variant : nullptr,
        .options = keyboard->options[0] != '\0' ? keyboard->options : nullptr,
        .model = keyboard->model[0] != '\0' ? keyboard->model : nullptr,
    };
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == nullptr) {
        wlr_log(WLR_ERROR,
                "cannot compile keymap (layout '%s', variant '%s', options '%s'); "
                "using the default",
                keyboard->layout, keyboard->variant, keyboard->options);
        keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    xkb_context_unref(ctx);
    return keymap;
}

/* [Keyboard]'s repeat, or SlowKeys' own (none, or slower). Clients
 * repeat keys themselves, at the rate we tell them. */
static void apply_repeat(xd_server *server, struct wlr_keyboard *keyboard) {
    const xd_access_settings *access_settings = &server->config.access;
    int rate = server->config.keyboard.repeat_rate, delay = server->config.keyboard.repeat_delay;
    if (access_settings->filter_keys && access_settings->slow_keys) {
        rate = access_settings->filter_no_repeat
                   ? 0
                   : XD_MAX(1, 1000 / access_settings->filter_repeat_ms);
        delay = access_settings->filter_repeat_delay;
    }
    wlr_keyboard_set_repeat_info(keyboard, rate, delay);
}

static void configure_keyboard(xd_server *server, struct wlr_keyboard *wlr_kb,
                               struct xkb_keymap *keymap) {
    if (keymap != nullptr)
        wlr_keyboard_set_keymap(wlr_kb, keymap);
    apply_repeat(server, wlr_kb);
}

void xd_input_apply_keyboard(xd_server *server) {
    const xd_keyboard_settings *keyboard = &server->config.keyboard;
    struct xkb_keymap *keymap = configured_keymap(keyboard);
    wlr_log(WLR_INFO,
            "keyboard settings: layout '%s', variant '%s', options '%s', model '%s', "
            "repeat %d/s after %d ms",
            keyboard->layout, keyboard->variant, keyboard->options, keyboard->model,
            keyboard->repeat_rate, keyboard->repeat_delay);
    server->kb_layouts = keymap != nullptr ? xkb_keymap_num_layouts(keymap) : 1;
    XD_LIST_FOR_EACH(kb, &server->keyboards, xd_keyboard, link) {
        if (kb->physical)
            configure_keyboard(server, kb->wlr_keyboard, keymap);
    }
    xkb_keymap_unref(keymap);
    adopt_group(server);
}

static void add_keyboard(xd_server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);
    struct xkb_keymap *keymap = configured_keymap(&server->config.keyboard);
    if (keymap == nullptr) {
        wlr_log(WLR_ERROR, "cannot compile keymap for %s", device->name);
        return;
    }
    configure_keyboard(server, wlr_kb, keymap);
    xkb_keymap_unref(keymap);
    register_keyboard(server, wlr_kb, device)->physical = true;
    adopt_group(server);
}

static xd_keyboard *register_keyboard(xd_server *server, struct wlr_keyboard *wlr_kb,
                                      struct wlr_input_device *device) {
    xd_keyboard *kb = XD_NEW(xd_keyboard);
    kb->server = server;
    kb->wlr_keyboard = wlr_kb;
    XD_LISTEN(&wlr_kb->events.modifiers, kb, modifiers, handle_modifiers);
    XD_LISTEN(&wlr_kb->events.key, kb, key, handle_key);
    XD_LISTEN(&wlr_kb->events.keymap, kb, keymap, handle_keymap);
    XD_LISTEN(&device->events.destroy, kb, destroy, handle_keyboard_destroy);
    wlr_seat_set_keyboard(server->seat, wlr_kb);
    wl_list_insert(&server->keyboards, &kb->link);
    return kb;
}

/* ---- pointers ([Mouse]) ------------------------------------------------------ */

/* A touchpad: libinput's devices that can be tapped. */
static bool is_touchpad(struct libinput_device *li) {
    return libinput_device_config_tap_get_finger_count(li) > 0;
}

static void set_speed(struct libinput_device *li, int speed, bool acceleration) {
    if (!libinput_device_config_accel_is_available(li))
        return;
    libinput_device_config_accel_set_speed(li, speed / 10.0);
    /* Windows' "Enhance pointer precision" is its acceleration curve;
     * without it the pointer moves in proportion to the mouse. */
    enum libinput_config_accel_profile want =
        acceleration ? LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE : LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
    if (libinput_device_config_accel_get_profiles(li) & (uint32_t)want)
        libinput_device_config_accel_set_profile(li, want);
}

/* [Touchpad]: each option only where the touchpad has it (a clickpad has
 * no edge scrolling to speak of, an old touchpad no clickfinger). */
static void configure_touchpad(struct libinput_device *li, const xd_touchpad_settings *touchpad) {
    libinput_device_config_tap_set_enabled(li, touchpad->tap ? LIBINPUT_CONFIG_TAP_ENABLED
                                                             : LIBINPUT_CONFIG_TAP_DISABLED);
    /* Two fingers tapped are the right button, three the middle, as on
     * Windows' precision touchpads. */
    libinput_device_config_tap_set_button_map(li, LIBINPUT_CONFIG_TAP_MAP_LRM);
    libinput_device_config_tap_set_drag_enabled(
        li, touchpad->tap_drag ? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
    /* ENABLED: the older name for libinput 1.27's ENABLED_TIMEOUT, which
     * older distributions' libinput lacks. */
    libinput_device_config_tap_set_drag_lock_enabled(li, touchpad->drag_lock
                                                             ? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED
                                                             : LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
    uint32_t methods = libinput_device_config_scroll_get_methods(li);
    enum libinput_config_scroll_method scroll =
        touchpad->scroll == XD_TOUCHPAD_SCROLL_TWO_FINGER ? LIBINPUT_CONFIG_SCROLL_2FG
        : touchpad->scroll == XD_TOUCHPAD_SCROLL_EDGE     ? LIBINPUT_CONFIG_SCROLL_EDGE
                                                          : LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
    /* Two fingers where the touchpad can't: its edge, if it can that. */
    if (scroll == LIBINPUT_CONFIG_SCROLL_2FG && !(methods & (uint32_t)scroll))
        scroll = LIBINPUT_CONFIG_SCROLL_EDGE;
    if (scroll == LIBINPUT_CONFIG_SCROLL_NO_SCROLL || (methods & (uint32_t)scroll))
        libinput_device_config_scroll_set_method(li, scroll);
    if (libinput_device_config_scroll_has_natural_scroll(li))
        libinput_device_config_scroll_set_natural_scroll_enabled(li, touchpad->natural_scroll);
    uint32_t clicks = libinput_device_config_click_get_methods(li);
    enum libinput_config_click_method click =
        touchpad->click == XD_TOUCHPAD_CLICK_AREAS ? LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
        : touchpad->click == XD_TOUCHPAD_CLICK_FINGERS
            ? LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
            : libinput_device_config_click_get_default_method(li);
    if (click == LIBINPUT_CONFIG_CLICK_METHOD_NONE || (clicks & (uint32_t)click))
        libinput_device_config_click_set_method(li, click);
    if (libinput_device_config_dwt_is_available(li))
        libinput_device_config_dwt_set_enabled(li, touchpad->disable_typing
                                                       ? LIBINPUT_CONFIG_DWT_ENABLED
                                                       : LIBINPUT_CONFIG_DWT_DISABLED);
    uint32_t modes = libinput_device_config_send_events_get_modes(li);
    if (touchpad->disable_with_mouse &&
        (modes & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE))
        libinput_device_config_send_events_set_mode(
            li, LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE);
    else
        libinput_device_config_send_events_set_mode(li, LIBINPUT_CONFIG_SEND_EVENTS_ENABLED);
    set_speed(li, touchpad->speed, touchpad->acceleration);
}

/* Only libinput devices can be configured: not a nested session's
 * pointer, nor virtual ones. Touchpads take [Touchpad]'s speed and
 * options, mice [Mouse]'s; both its buttons. */
static void configure_pointer(struct wlr_input_device *device, const xd_mouse_settings *mouse) {
    if (!wlr_input_device_is_libinput(device))
        return;
    struct libinput_device *li = wlr_libinput_get_device_handle(device);
    if (libinput_device_config_left_handed_is_available(li))
        libinput_device_config_left_handed_set(li, mouse->left_handed);
    if (is_touchpad(li)) {
        const xd_touchpad_settings *touchpad = &mouse->touchpad;
        configure_touchpad(li, touchpad);
        wlr_log(WLR_INFO, "touchpad '%s': tap %s, %s scrolling%s, speed %d", device->name,
                touchpad->tap ? "on" : "off",
                touchpad->scroll == XD_TOUCHPAD_SCROLL_TWO_FINGER ? "two-finger"
                : touchpad->scroll == XD_TOUCHPAD_SCROLL_EDGE     ? "edge"
                                                                  : "no",
                touchpad->natural_scroll ? " (natural)" : "", touchpad->speed);
        return;
    }
    set_speed(li, mouse->speed, mouse->acceleration);
    wlr_log(WLR_INFO, "pointer '%s': %s-handed, speed %d, %s", device->name,
            mouse->left_handed ? "left" : "right", mouse->speed,
            mouse->acceleration ? "acceleration" : "no acceleration");
}

void xd_input_apply_mouse(xd_server *server) {
    const xd_mouse_settings *mouse = &server->config.mouse;
    wlr_log(WLR_INFO, "mouse settings: %s-handed, speed %d, %s, double-click %d ms, wheel lines %d",
            mouse->left_handed ? "left" : "right", mouse->speed,
            mouse->acceleration ? "acceleration" : "no acceleration", mouse->double_click_ms,
            mouse->wheel_lines);
    XD_LIST_FOR_EACH(pointer, &server->pointers, xd_pointer, link) {
        configure_pointer(pointer->device, mouse);
    }
    xd_access_apply(server); /* ClickLock may have been turned off */
}

static void handle_pointer_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_pointer *pointer = XD_CONTAINER_OF(listener, xd_pointer, destroy);
    xd_unlisten(&pointer->destroy);
    wl_list_remove(&pointer->link);
    xd_free(pointer);
}

static void add_pointer(xd_server *server, struct wlr_input_device *device) {
    configure_pointer(device, &server->config.mouse);
    xd_pointer *pointer = XD_NEW(xd_pointer);
    pointer->device = device;
    XD_LISTEN(&device->events.destroy, pointer, destroy, handle_pointer_destroy);
    wl_list_insert(&server->pointers, &pointer->link);
    wlr_cursor_attach_input_device(server->cursor, device);
}

/* ---- the lid ---------------------------------------------------------------- */

/* A switch device (libinput's): the lid is the one we act on; tablet
 * mode and the rest go nowhere. */
typedef struct xd_switch {
    xd_server *server;
    struct wl_listener toggle;
    struct wl_listener destroy;
} xd_switch;

void xd_input_lid(xd_server *server, bool closed) {
    /* Opening the lid wakes the screen, as a key does. */
    if (!closed)
        xd_idle_activity(server);
    xd_arrange_power_button(server,
                            closed ? XD_POWER_EVENT_LID_CLOSED : XD_POWER_EVENT_LID_OPENED);
}

static void handle_switch_toggle(struct wl_listener *listener, void *data) {
    xd_switch *sw = XD_CONTAINER_OF(listener, xd_switch, toggle);
    const struct wlr_switch_toggle_event *switch_toggle_event = data;
    if (switch_toggle_event->switch_type == WLR_SWITCH_TYPE_LID)
        xd_input_lid(sw->server, switch_toggle_event->switch_state == WLR_SWITCH_STATE_ON);
}

static void handle_switch_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_switch *sw = XD_CONTAINER_OF(listener, xd_switch, destroy);
    xd_unlisten(&sw->toggle);
    xd_unlisten(&sw->destroy);
    xd_free(sw);
}

static void add_switch(xd_server *server, struct wlr_input_device *device) {
    xd_switch *sw = XD_NEW(xd_switch);
    sw->server = server;
    XD_LISTEN(&wlr_switch_from_input_device(device)->events.toggle, sw, toggle,
              handle_switch_toggle);
    XD_LISTEN(&device->events.destroy, sw, destroy, handle_switch_destroy);
}

static void update_seat_caps(xd_server *server) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (xd_touch_present(server))
        caps |= WL_SEAT_CAPABILITY_TOUCH;
    wlr_seat_set_capabilities(server->seat, caps);
}

void xd_input_update_caps(xd_server *server) { update_seat_caps(server); }

/* Touchscreens go to touch.c, tablets and their pads to tablet.c. */
void xd_input_add_device(xd_server *server, struct wlr_input_device *device) {
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        add_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        add_pointer(server, device);
        break;
    case WLR_INPUT_DEVICE_SWITCH:
        add_switch(server, device);
        break;
    case WLR_INPUT_DEVICE_TOUCH:
        xd_touch_add(server, device);
        break;
    case WLR_INPUT_DEVICE_TABLET:
        xd_tablet_add(server, device);
        break;
    case WLR_INPUT_DEVICE_TABLET_PAD:
        xd_tablet_add_pad(server, device);
        break;
    }
    update_seat_caps(server);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_input);
    xd_input_add_device(server, data);
}

/* ---- virtual input (testing only) ------------------------------------------ */

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *vk = data;
    /* The client supplies its own keymap. The configured repeat, so a
     * held key (xd-inject's press) repeats as a real one would. */
    apply_repeat(server, &vk->keyboard);
    register_keyboard(server, &vk->keyboard, &vk->keyboard.base);
    adopt_group(server);
    update_seat_caps(server);
    wlr_log(WLR_INFO, "virtual keyboard connected");
}

static void handle_new_virtual_pointer(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_virtual_pointer);
    const struct wlr_virtual_pointer_v1_new_pointer_event *virtual_pointer_v1_new_pointer_event =
        data;
    wlr_cursor_attach_input_device(
        server->cursor, &virtual_pointer_v1_new_pointer_event->new_pointer->pointer.base);
    update_seat_caps(server);
    wlr_log(WLR_INFO, "virtual pointer connected (testing)");
}

void xd_input_enable_virtual(xd_server *server) {
    /* Virtual keyboards are always there (for the input method); the
     * global filter now lets everyone have them. */
    struct wlr_virtual_pointer_manager_v1 *pm =
        wlr_virtual_pointer_manager_v1_create(server->display);
    XD_LISTEN(&pm->events.new_virtual_pointer, server, new_virtual_pointer,
              handle_new_virtual_pointer);
    /* Touch, pens and gestures for xd-inject, which the headless
     * backend has no devices for. */
    xd_test_input_init(server);
    wlr_log(WLR_INFO, "virtual input enabled: any client can inject input (testing only)");
}

/* ---- pointer -------------------------------------------------------------- */

/* The theme and size [Mouse] asks for; nullptr is libxcursor's default
 * (XCURSOR_THEME, then "default"). */
static struct wlr_xcursor_manager *cursor_manager(const xd_mouse_settings *mouse) {
    return wlr_xcursor_manager_create(mouse->cursor_theme[0] != '\0' ? mouse->cursor_theme
                                                                     : nullptr,
                                      (uint32_t)mouse->cursor_size);
}

/* Programs started from now on (and those they start) use the same
 * cursors: XCursor and libwayland-cursor read these, GTK and Qt too. */
static void export_cursor_theme(const xd_mouse_settings *mouse) {
    if (mouse->cursor_theme[0] != '\0')
        setenv("XCURSOR_THEME", mouse->cursor_theme, true);
    else
        unsetenv("XCURSOR_THEME");
    char size[16];
    snprintf(size, sizeof(size), "%d", mouse->cursor_size);
    setenv("XCURSOR_SIZE", size, true);
}

void xd_input_apply_cursor_theme(xd_server *server) {
    const xd_mouse_settings *mouse = &server->config.mouse;
    struct wlr_xcursor_manager *mgr = cursor_manager(mouse);
    /* The cursor keeps a pointer to its manager (to reload on a scale
     * change), so move it to the new one before freeing the old. */
    if (server->cursor_name != nullptr)
        wlr_cursor_set_xcursor(server->cursor, mgr, server->cursor_name);
    wlr_xcursor_manager_destroy(server->xcursor);
    server->xcursor = mgr;
    export_cursor_theme(mouse);
    wlr_log(WLR_INFO, "cursor theme '%s', size %d",
            mouse->cursor_theme[0] != '\0' ? mouse->cursor_theme : "(default)", mouse->cursor_size);
}

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
    xd_layer_surface *layer; /* under the pointer instead of a window */
    struct wlr_surface *surface;
    double sx, sy;
    xd_frame_part part;
} pick;

/* Windows that draw their own decorations and have no resize margin of
 * their own (Firefox, for one) get an invisible one from us, just outside
 * their edges. */
static constexpr double CSD_MARGIN = 6;
static constexpr double CSD_CORNER = 16;

static xd_frame_part csd_margin_part(const xd_view *view, double lx, double ly) {
    struct wlr_box box = xd_view_outer_box(view);
    double x0 = box.x, y0 = box.y, x1 = box.x + box.width, y1 = box.y + box.height;
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
    pick picked = {.part = XD_PART_NONE};
    if (server->locked) {
        /* Only the screen locker's surfaces get the pointer. */
        struct wlr_scene_node *scene_node =
            wlr_scene_node_at(&server->layer_lock->node, lx, ly, &picked.sx, &picked.sy);
        if (scene_node != nullptr && scene_node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_surface *ss =
                wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(scene_node));
            if (ss != nullptr) {
                picked.surface = ss->surface;
                picked.part = XD_PART_CLIENT;
            }
        }
        return picked;
    }
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, &picked.sx, &picked.sy);
    xd_view *under = node != nullptr ? xd_view_from_node(node) : nullptr;
    /* Resize margins of client-decorated windows stacked above what's under
     * the cursor (views are kept front to back). */
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (view == under)
            break;
        if (!view->mapped || view->minimized || xd_view_framed(view) || view->maximized ||
            view->fullscreen)
            continue;
        xd_frame_part part = csd_margin_part(view, lx, ly);
        if (part != XD_PART_NONE)
            return (pick){.view = view, .part = part};
    }
    if (node == nullptr)
        return picked;
    picked.view = under;
    if (under == nullptr)
        picked.layer = xd_layer_from_node(node);
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_surface *ss =
            wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        if (ss != nullptr) {
            picked.surface = ss->surface;
            picked.part = XD_PART_CLIENT;
            return picked;
        }
    }
    if (picked.view != nullptr)
        picked.part = xd_view_hit(picked.view, lx, ly);
    return picked;
}

xd_input_target xd_input_target_at(xd_server *server, double lx, double ly) {
    pick picked = pick_at(server, lx, ly);
    return (xd_input_target){.view = picked.view,
                             .layer = picked.layer,
                             .surface = picked.surface,
                             .sx = picked.sx,
                             .sy = picked.sy,
                             .part = picked.part};
}

static void set_hover(xd_server *server, xd_view *view, xd_frame_part part) {
    xd_frame_part hot = xd_frame_part_is_button(part) ? part : XD_PART_NONE;
    if (server->hover_view != nullptr && server->hover_view != view)
        xd_view_set_hover(server->hover_view, XD_PART_NONE, XD_PART_NONE);
    server->hover_view = hot != XD_PART_NONE ? view : nullptr;
    if (view != nullptr)
        xd_view_set_hover(view, hot, view->pressed);
}

/* The box a resize has reached, and the edges it moves. */
static struct wlr_box resize_box(xd_server *server, uint32_t *moving_out) {
    xd_view *view = server->grab_view;
    xd_insets in = xd_view_insets(view);
    struct wlr_box box = server->grab_box;
    int dx = (int)(server->cursor->x - server->grab_x);
    int dy = (int)(server->cursor->y - server->grab_y);
    int min_w = in.left + in.right + XD_MIN_CLIENT_W;
    int min_h = in.top + in.bottom + XD_MIN_CLIENT_H;
    xd_frame_part part = server->grab_part;

    int left = box.x;
    int top = box.y;
    int right = box.x + box.width;
    int bottom = box.y + box.height;
    if (part == XD_PART_LEFT || part == XD_PART_TOP_LEFT || part == XD_PART_BOTTOM_LEFT)
        left = XD_MIN(left + dx, right - min_w);
    if (part == XD_PART_RIGHT || part == XD_PART_TOP_RIGHT || part == XD_PART_BOTTOM_RIGHT)
        right = XD_MAX(right + dx, left + min_w);
    if (part == XD_PART_TOP || part == XD_PART_TOP_LEFT || part == XD_PART_TOP_RIGHT)
        top = XD_MIN(top + dy, bottom - min_h);
    if (part == XD_PART_BOTTOM || part == XD_PART_BOTTOM_LEFT || part == XD_PART_BOTTOM_RIGHT)
        bottom = XD_MAX(bottom + dy, top + min_h);

    uint32_t moving = 0;
    if (part == XD_PART_LEFT || part == XD_PART_TOP_LEFT || part == XD_PART_BOTTOM_LEFT)
        moving |= WLR_EDGE_LEFT;
    if (part == XD_PART_TOP || part == XD_PART_TOP_LEFT || part == XD_PART_TOP_RIGHT)
        moving |= WLR_EDGE_TOP;
    *moving_out = moving;
    return (struct wlr_box){left, top, right - left, bottom - top};
}

static void process_resize(xd_server *server) {
    uint32_t moving = 0;
    struct wlr_box box = resize_box(server, &moving);
    if (xd_outline_dragging(server))
        xd_outline_show(server, box);
    else
        xd_view_resize_anchored(server->grab_view, box, moving);
}

/* Where a move has the window (its outer box's corner). */
static void move_position(const xd_server *server, int *window_x, int *window_y) {
    *window_x = (int)(server->cursor->x - server->grab_x);
    *window_y = (int)(server->cursor->y - server->grab_y);
}

static void place_drag_icon(xd_server *server);

/* Where a surface is in the layout, found in the scene (it may have moved
 * since the button went down: a taskbar dragged to another edge). */
typedef struct find_surface {
    struct wlr_surface *surface;
    int x, y;
    bool found;
} find_surface;

static void find_surface_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    find_surface *finder = data;
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
    if (!finder->found && ss != nullptr && ss->surface == finder->surface) {
        finder->found = true;
        finder->x = sx;
        finder->y = sy;
    }
}

/* The implicit grab: while buttons are held after a press in a client,
 * motion goes to that surface even outside it (selecting text past a
 * window's edge, dragging a slider, dragging the taskbar). */
static bool implicit_grab_motion(xd_server *server, uint32_t time_msec) {
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *focus = seat->pointer_state.focused_surface;
    if (seat->pointer_state.button_count == 0 || focus == nullptr ||
        wlr_seat_pointer_has_grab(seat))
        return false; /* drags and popup grabs route themselves */
    find_surface finder = {.surface = focus};
    wlr_scene_node_for_each_buffer(&server->scene->tree.node, find_surface_buffer, &finder);
    if (!finder.found)
        return false;
    wlr_seat_pointer_notify_motion(seat, time_msec, server->cursor->x - finder.x,
                                   server->cursor->y - finder.y);
    return true;
}

static void process_motion(xd_server *server, uint32_t time_msec) {
    place_drag_icon(server);
    switch (server->mode) {
    case XD_CURSOR_MOVE: {
        xd_view *view = server->grab_view;
        xd_snap_motion(server, view);
        /* A maximized window stays put until it's dragged far enough. */
        if (view->maximized)
            return;
        int x, y;
        move_position(server, &x, &y);
        if (xd_outline_dragging(server)) {
            struct wlr_box box = xd_view_outer_box(view);
            xd_outline_show(server, (struct wlr_box){x, y, box.width, box.height});
        } else {
            xd_view_set_position(view, x, y);
        }
        return;
    }
    case XD_CURSOR_RESIZE:
        process_resize(server);
        return;
    case XD_CURSOR_BUTTON: {
        /* The button looks pressed only while the pointer is over it. */
        xd_view *view = server->grab_view;
        xd_frame_part part = xd_view_hit(view, server->cursor->x, server->cursor->y);
        xd_view_set_hover(view, part == server->grab_part ? part : XD_PART_NONE, server->grab_part);
        return;
    }
    case XD_CURSOR_KEY_MOVE:
    case XD_CURSOR_KEY_SIZE:
        keyboard_grab_motion(server);
        return;
    case XD_CURSOR_PASSTHROUGH:
        break;
    }
    if (implicit_grab_motion(server, time_msec))
        return;

    pick picked = pick_at(server, server->cursor->x, server->cursor->y);
    set_hover(server, picked.view, picked.part);
    if (picked.surface != nullptr) {
        /* The client sets its own cursor once it has pointer focus. */
        if (server->seat->pointer_state.focused_surface != picked.surface)
            server->cursor_name = nullptr;
        wlr_seat_pointer_notify_enter(server->seat, picked.surface, picked.sx, picked.sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, picked.sx, picked.sy);
    } else {
        wlr_seat_pointer_clear_focus(server->seat);
        xd_cursor_set(server, cursor_for_part(picked.part));
    }
}

/* A motion by (dx, dy) through pointer-constraints (constraints.c): the
 * program hears it as relative motion; a locked pointer stays put (and the
 * program gets no wl_pointer motion), a confined one goes only so far. */
static void constrained_motion(xd_server *server, struct wlr_input_device *device, double dx,
                               double dy, double raw_dx, double raw_dy, uint32_t time_msec) {
    xd_idle_activity(server);
    if (!xd_constraints_motion(server, time_msec, &dx, &dy, raw_dx, raw_dy))
        return;
    wlr_cursor_move(server->cursor, device, dx, dy);
    process_motion(server, time_msec);
    xd_constraints_update(server);
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_motion);
    const struct wlr_pointer_motion_event *pointer_motion_event = data;
    constrained_motion(server, &pointer_motion_event->pointer->base, pointer_motion_event->delta_x,
                       pointer_motion_event->delta_y, pointer_motion_event->unaccel_dx,
                       pointer_motion_event->unaccel_dy, pointer_motion_event->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_motion_absolute);
    const struct wlr_pointer_motion_absolute_event *pointer_motion_absolute_event = data;
    /* A nested session's pointer, a virtual one: as a motion from where
     * the cursor is, so constraints hold for it too (as in sway). */
    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(
        server->cursor, &pointer_motion_absolute_event->pointer->base,
        pointer_motion_absolute_event->x, pointer_motion_absolute_event->y, &lx, &ly);
    double dx = lx - server->cursor->x, dy = ly - server->cursor->y;
    constrained_motion(server, &pointer_motion_absolute_event->pointer->base, dx, dy, dx, dy,
                       pointer_motion_absolute_event->time_msec);
}

static bool is_double_click(xd_server *server, xd_view *view, xd_frame_part part,
                            uint32_t time_msec) {
    bool dbl = server->last_click_view == view && server->last_click_part == part &&
               time_msec - server->last_click_msec <=
                   (uint32_t)server->config.mouse.double_click_ms;
    server->last_click_view = dbl ? nullptr : view;
    server->last_click_part = part;
    server->last_click_msec = time_msec;
    return dbl;
}

static void begin_frame_action(xd_server *server, pick *picked, uint32_t button,
                               uint32_t time_msec) {
    xd_view *view = picked->view;
    xd_view_focus(view);
    if (button == BTN_RIGHT) {
        /* Right-click on the frame: the window menu at the pointer. */
        if (picked->part == XD_PART_CAPTION || picked->part == XD_PART_SYSMENU)
            xd_window_menu_show(view, server->cursor->x, server->cursor->y, false);
        return;
    }
    if (button != BTN_LEFT)
        return;

    if (xd_frame_part_is_button(picked->part)) {
        server->mode = XD_CURSOR_BUTTON;
        server->grab_view = view;
        server->grab_part = picked->part;
        xd_view_set_hover(view, picked->part, picked->part);
        return;
    }
    if (picked->part == XD_PART_SYSMENU) {
        /* A click opens the window menu below the icon; the double-click
         * that closes the window is caught while the menu has the grab
         * (see handle_cursor_button). */
        if (is_double_click(server, view, picked->part, time_msec)) {
            xd_view_close(view);
            return;
        }
        double x, y;
        xd_view_menu_anchor(view, &x, &y);
        xd_window_menu_show(view, x, y, false);
        return;
    }
    if (picked->part == XD_PART_CAPTION) {
        if (is_double_click(server, view, picked->part, time_msec)) {
            xd_view_set_maximized(view, !view->maximized);
            return;
        }
        /* A maximized window moves only in Aero (it's restored as it's
         * dragged). */
        if (!xd_snap_begin(server, view))
            return;
        server->mode = XD_CURSOR_MOVE;
        server->grab_view = view;
        server->grab_x = server->cursor->x - view->x;
        server->grab_y = server->cursor->y - view->y;
        server->grab_press_x = server->cursor->x;
        server->grab_press_y = server->cursor->y;
        return;
    }
    if (xd_frame_part_is_border(picked->part) && !view->maximized) {
        server->mode = XD_CURSOR_RESIZE;
        server->grab_view = view;
        server->grab_part = picked->part;
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
    /* An outline was dragged: the window goes where it ended. */
    bool outline = server->outline != nullptr;
    xd_outline_hide(server);
    if (view == nullptr)
        return;

    if (mode == XD_CURSOR_MOVE) {
        if (outline && !view->maximized) {
            int x, y;
            move_position(server, &x, &y);
            xd_view_set_position(view, x, y);
        }
        xd_snap_end(server, view);
    } else if (mode == XD_CURSOR_RESIZE) {
        if (outline) {
            uint32_t moving = 0;
            struct wlr_box box = resize_box(server, &moving);
            xd_view_resize_anchored(view, box, moving);
        }
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

void xd_cursor_cancel_grab(xd_server *server) {
    xd_view *view = server->grab_view;
    xd_cursor_mode mode = server->mode;
    xd_outline_hide(server); /* the window stays where it was */
    server->mode = XD_CURSOR_PASSTHROUGH;
    server->grab_view = nullptr;
    server->cursor_name = nullptr;
    if (mode == XD_CURSOR_MOVE)
        xd_snap_cancel(server);
    if (view == nullptr)
        return;
    if (mode == XD_CURSOR_RESIZE || mode == XD_CURSOR_KEY_SIZE)
        xd_view_set_resizing(view, false);
    else if (mode == XD_CURSOR_BUTTON)
        xd_view_set_hover(view, XD_PART_NONE, XD_PART_NONE);
}

/* Whether the seat delivered a press of `button` that's still held. */
static bool seat_has_button(const struct wlr_seat *seat, uint32_t button) {
    for (size_t i = 0; i < seat->pointer_state.button_count; i++) {
        if (seat->pointer_state.buttons[i].button == button)
            return true;
    }
    return false;
}

/* MouseKeys moves the pointer as a mouse would. */
void xd_input_move_pointer(xd_server *server, double dx, double dy, uint32_t time_msec) {
    constrained_motion(server, nullptr, dx, dy, dx, dy, time_msec);
    wlr_seat_pointer_notify_frame(server->seat);
}

/* A finger or a pen working the pointer: there at once, as an absolute
 * pointer (a graphics tablet in mouse mode) goes. */
void xd_input_warp_pointer(xd_server *server, double lx, double ly, uint32_t time_msec) {
    wlr_cursor_warp(server->cursor, nullptr, lx, ly);
    xd_idle_activity(server);
    process_motion(server, time_msec);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void process_button(xd_server *server,
                           const struct wlr_pointer_button_event *pointer_button_event);

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_button);
    const struct wlr_pointer_button_event *pointer_button_event = data;
    xd_idle_activity(server);
    if (xd_access_button(server, pointer_button_event->button,
                         pointer_button_event->state == WL_POINTER_BUTTON_STATE_PRESSED,
                         pointer_button_event->time_msec))
        return;
    /* A click while the VirtualBox host's drag holds the pointer ends it
     * (and goes no further). */
    if (pointer_button_event->state == WL_POINTER_BUTTON_STATE_PRESSED && xd_relay_cancel(server))
        return;
    process_button(server, pointer_button_event);
    if (pointer_button_event->state == WL_POINTER_BUTTON_STATE_PRESSED)
        xd_constraints_click(server);
}

void xd_input_button(xd_server *server, uint32_t time_msec, uint32_t button,
                     enum wl_pointer_button_state state) {
    xd_idle_activity(server);
    struct wlr_pointer_button_event pointer_button_event = {
        .time_msec = time_msec, .button = button, .state = state};
    process_button(server, &pointer_button_event);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void process_button(xd_server *server,
                           const struct wlr_pointer_button_event *pointer_button_event) {
    if (server->locked) {
        wlr_seat_pointer_notify_button(server->seat, pointer_button_event->time_msec,
                                       pointer_button_event->button, pointer_button_event->state);
        return;
    }

    /* A click ends keyboard move/size where the window is. */
    if (keyboard_grab(server)) {
        if (pointer_button_event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            end_keyboard(server, true);
            server->ignore_release = true;
        }
        return;
    }
    if (pointer_button_event->state == WL_POINTER_BUTTON_STATE_RELEASED && server->ignore_release) {
        server->ignore_release = false;
        process_motion(server, pointer_button_event->time_msec);
        return;
    }
    if (pointer_button_event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->mode != XD_CURSOR_PASSTHROUGH) {
            end_grab(server);
            /* A client that asked for the move or resize had the press:
             * it must get the release too, or it keeps waiting for it and
             * ignores later clicks (Firefox's tab strip and close button
             * after dragging the window by it). */
            if (seat_has_button(server->seat, pointer_button_event->button))
                wlr_seat_pointer_notify_button(server->seat, pointer_button_event->time_msec,
                                               pointer_button_event->button,
                                               pointer_button_event->state);
            process_motion(server, pointer_button_event->time_msec);
            return;
        }
        wlr_seat_pointer_notify_button(server->seat, pointer_button_event->time_msec,
                                       pointer_button_event->button, pointer_button_event->state);
        /* The implicit grab ends: focus what's under the pointer now. */
        if (server->seat->pointer_state.button_count == 0)
            process_motion(server, pointer_button_event->time_msec);
        return;
    }

    if (server->mode != XD_CURSOR_PASSTHROUGH)
        return;
    /* An open popup (menu) owns the pointer: a click outside it must reach
     * the grab so the menu closes, not start a window drag. */
    if (wlr_seat_pointer_has_grab(server->seat)) {
        /* Double-clicking the caption icon closes the window even though
         * its first click opened the window menu. */
        pick picked = pick_at(server, server->cursor->x, server->cursor->y);
        if (picked.view != nullptr && picked.part == XD_PART_SYSMENU &&
            pointer_button_event->button == BTN_LEFT &&
            is_double_click(server, picked.view, picked.part, pointer_button_event->time_msec))
            xd_view_close(picked.view);
        wlr_seat_pointer_notify_button(server->seat, pointer_button_event->time_msec,
                                       pointer_button_event->button, pointer_button_event->state);
        return;
    }
    pick picked = pick_at(server, server->cursor->x, server->cursor->y);
    if (picked.surface != nullptr) {
        if (picked.view != nullptr)
            xd_view_focus(picked.view);
        else if (picked.layer != nullptr)
            xd_layer_click_focus(picked.layer);
        wlr_seat_pointer_notify_button(server->seat, pointer_button_event->time_msec,
                                       pointer_button_event->button, pointer_button_event->state);
    } else if (picked.view != nullptr) {
        begin_frame_action(server, &picked, pointer_button_event->button,
                           pointer_button_event->time_msec);
    }
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_axis);
    const struct wlr_pointer_axis_event *pointer_axis_event = data;
    xd_idle_activity(server);
    double delta = pointer_axis_event->delta;
    int32_t discrete = pointer_axis_event->delta_discrete;
    /* [Mouse] WheelLines: clients scroll about 3 lines a notch, so scale
     * the wheel's steps (not touchpads') by lines / 3. Clients add up
     * the 120ths, so a third of a step still counts. */
    int lines = server->config.mouse.wheel_lines;
    if (pointer_axis_event->source == WL_POINTER_AXIS_SOURCE_WHEEL && lines != 3) {
        delta = delta * lines / 3.0;
        discrete = (int32_t)((int64_t)discrete * lines / 3);
    }
    /* [Touchpad] ScrollSpeed: fingers on a touchpad (no steps to count). */
    int percent = server->config.mouse.touchpad.scroll_speed;
    if (pointer_axis_event->source == WL_POINTER_AXIS_SOURCE_FINGER && percent > 0 &&
        percent != 100)
        delta = delta * percent / 100.0;
    wlr_seat_pointer_notify_axis(
        server->seat, pointer_axis_event->time_msec, pointer_axis_event->orientation, delta,
        discrete, pointer_axis_event->source, pointer_axis_event->relative_direction);
}

static void handle_cursor_frame(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void handle_request_cursor(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_cursor);
    const struct wlr_seat_pointer_request_set_cursor_event *seat_pointer_request_set_cursor_event =
        data;
    if (server->mode == XD_CURSOR_PASSTHROUGH &&
        server->seat->pointer_state.focused_client ==
            seat_pointer_request_set_cursor_event->seat_client) {
        server->cursor_name = nullptr;
        wlr_cursor_set_surface(server->cursor, seat_pointer_request_set_cursor_event->surface,
                               seat_pointer_request_set_cursor_event->hotspot_x,
                               seat_pointer_request_set_cursor_event->hotspot_y);
    }
}

static void handle_request_cursor_shape(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_cursor_shape);
    const struct wlr_cursor_shape_manager_v1_request_set_shape_event
        *cursor_shape_manager_v1_request_set_shape_event = data;
    if (server->mode == XD_CURSOR_PASSTHROUGH &&
        server->seat->pointer_state.focused_client ==
            cursor_shape_manager_v1_request_set_shape_event->seat_client)
        xd_cursor_set(server, wlr_cursor_shape_v1_name(
                                  cursor_shape_manager_v1_request_set_shape_event->shape));
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_set_selection);
    const struct wlr_seat_request_set_selection_event *seat_request_set_selection_event = data;
    wlr_seat_set_selection(server->seat, seat_request_set_selection_event->source,
                           seat_request_set_selection_event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_set_primary_selection);
    const struct wlr_seat_request_set_primary_selection_event
        *seat_request_set_primary_selection_event = data;
    wlr_seat_set_primary_selection(server->seat, seat_request_set_primary_selection_event->source,
                                   seat_request_set_primary_selection_event->serial);
}

/* ---- drag and drop -------------------------------------------------------------- */

static void handle_request_start_drag(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_start_drag);
    const struct wlr_seat_request_start_drag_event *seat_request_start_drag_event = data;
    /* VirtualBox's relay, whose drags the host drives (dragrelay.c). */
    if (!server->locked && server->mode == XD_CURSOR_PASSTHROUGH &&
        xd_relay_start_drag(server, seat_request_start_drag_event->drag))
        return;
    /* Only for a button held over the client that asks. */
    if (!server->locked && server->mode == XD_CURSOR_PASSTHROUGH &&
        wlr_seat_validate_pointer_grab_serial(server->seat, seat_request_start_drag_event->origin,
                                              seat_request_start_drag_event->serial))
        wlr_seat_start_pointer_drag(server->seat, seat_request_start_drag_event->drag,
                                    seat_request_start_drag_event->serial);
    else
        wlr_data_source_destroy(seat_request_start_drag_event->drag->source);
}

static void place_drag_icon(xd_server *server) {
    if (server->drag_icon != nullptr)
        wlr_scene_node_set_position(&server->drag_icon->node, (int)server->cursor->x,
                                    (int)server->cursor->y);
}

void xd_input_pointer_moved(xd_server *server) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    process_motion(server, (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000));
}

static void handle_drag_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, drag_destroy);
    xd_unlisten(&server->drag_destroy);
    xd_relay_drag_ended(server);
    /* wlroots removes the icon's scene tree with the icon. */
    server->drag_icon = nullptr;
    /* The drop took the pointer: give it back to what's under it. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    process_motion(server, (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000));
}

static void handle_start_drag(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, start_drag);
    struct wlr_drag *drag = data;
    if (drag->icon != nullptr) {
        server->drag_icon = wlr_scene_drag_icon_create(server->layer_drag, drag->icon);
        place_drag_icon(server);
    }
    XD_LISTEN(&drag->events.destroy, server, drag_destroy, handle_drag_destroy);
    xd_relay_drag_started(server, drag);
}

static int handle_sigchld([[maybe_unused]] int sig, void *data) {
    xd_server *server = data;
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (xd_ime_child_exited(server, pid, status) || xd_relay_child_exited(server, pid, status))
            continue;
        if (pid == server->polkit_pid && pid > 0) {
            server->polkit_pid = 0;
            /* Only a crash: it exits with an error when another agent
             * already serves the session (nested) or polkit isn't there,
             * and starting it again wouldn't change that. */
            if (WIFSIGNALED(status)) {
                wlr_log(WLR_ERROR, "polkit agent killed by signal %d; restarting",
                        WTERMSIG(status));
                xd_polkit_start(server);
            }
            continue;
        }
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
    wl_list_init(&server->pointers);

    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->xcursor = cursor_manager(&server->config.mouse);
    export_cursor_theme(&server->config.mouse);
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
    XD_LISTEN(&server->seat->events.request_set_primary_selection, server,
              request_set_primary_selection, handle_request_set_primary_selection);
    XD_LISTEN(&server->seat->events.request_start_drag, server, request_start_drag,
              handle_request_start_drag);
    XD_LISTEN(&server->seat->events.start_drag, server, start_drag, handle_start_drag);

    struct wlr_cursor_shape_manager_v1 *shapes =
        wlr_cursor_shape_manager_v1_create(server->display, 1);
    XD_LISTEN(&shapes->events.request_set_shape, server, request_cursor_shape,
              handle_request_cursor_shape);

    server->sigchld = wl_event_loop_add_signal(server->loop, SIGCHLD, handle_sigchld, server);

    /* The input method passes back keys it doesn't use through a virtual
     * keyboard; only it sees the global (ime.c), unless
     * --allow-virtual-input. */
    server->virtual_keyboards = wlr_virtual_keyboard_manager_v1_create(server->display);
    XD_LISTEN(&server->virtual_keyboards->events.new_virtual_keyboard, server,
              new_virtual_keyboard, handle_new_virtual_keyboard);

    xd_touch_init(server);
    xd_tablet_init(server);
    xd_gestures_init(server);
}

void xd_input_finish(xd_server *server) {
    xd_gestures_finish(server);
    xd_tablet_finish(server);
    xd_touch_finish(server);
}
