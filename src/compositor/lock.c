/* SPDX-License-Identifier: MIT
 * ext-session-lock: screen lockers (swaylock, and later our own "Lock
 * Computer"). While locked, a black cover sits above everything, only the
 * locker's surfaces get input, and grabs and window bindings are off. If
 * the locker dies without unlocking, the screen stays locked until a new
 * locker takes over, as the protocol requires; then only the one we start
 * again may (another program could take the lock, and unlock it).
 *
 * The locker hears it's locked once every monitor has shown the cover
 * (a frame with it committed), not when it's asked for: a computer told
 * it may sleep then would show the desktop again on waking until the
 * cover's frame came. The shell hears too (xd-window-arranger-v1).
 */
#include <time.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

/* Monitors that never show a frame (switched off) don't hold the lock up
 * for longer than this. */
static constexpr int SHOWN_TIMEOUT_MS = 1000;

typedef struct xd_lock {
    xd_server *server;
    struct wlr_session_lock_v1 *lock;
    bool told; /* the locker was sent `locked` */
    struct wl_listener new_surface;
    struct wl_listener unlock;
    struct wl_listener destroy;
} xd_lock;

typedef struct xd_lock_surface {
    struct wl_list link; /* xd_server.lock_surfaces */
    xd_server *server;
    struct wlr_session_lock_surface_v1 *surface;
    struct wlr_scene_tree *tree;
    uint32_t width, height; /* the size last asked of the locker */
    struct wl_listener map;
    struct wl_listener destroy;
} xd_lock_surface;

/* Covers the whole layout. */
static void update_cover(xd_server *server) {
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, nullptr, &box);
    if (server->lock_cover == nullptr) {
        static const float black[4] = {0, 0, 0, 1};
        server->lock_cover = wlr_scene_rect_create(server->layer_lock, box.width, box.height, black);
    } else {
        wlr_scene_rect_set_size(server->lock_cover, box.width, box.height);
    }
    wlr_scene_node_set_position(&server->lock_cover->node, box.x, box.y);
    wlr_scene_node_lower_to_bottom(&server->lock_cover->node);
}

/* Puts a locker surface over its output, at the output's size. */
static void place_surface(xd_lock_surface *ls) {
    if (ls->surface->output == nullptr)
        return;
    struct wlr_box box;
    wlr_output_layout_get_box(ls->server->output_layout, ls->surface->output, &box);
    if (wlr_box_empty(&box))
        return; /* off (not in the layout): nothing to cover there */
    wlr_scene_node_set_position(&ls->tree->node, box.x, box.y);
    if (ls->width != (uint32_t)box.width || ls->height != (uint32_t)box.height) {
        ls->width = (uint32_t)box.width;
        ls->height = (uint32_t)box.height;
        wlr_session_lock_surface_v1_configure(ls->surface, ls->width, ls->height);
    }
}

/* Monitors were moved, resized or rescaled while locked: every locker
 * surface follows its own monitor, and the cover the whole layout. */
void xd_lock_layout_changed(xd_server *server) {
    if (!server->locked)
        return;
    update_cover(server);
    xd_lock_surface *ls;
    wl_list_for_each(ls, &server->lock_surfaces, link) {
        place_surface(ls);
    }
}

static void handle_surface_map(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_lock_surface *ls = XD_CONTAINER_OF(listener, xd_lock_surface, map);
    xd_server *server = ls->server;
    /* The locker on the output under the pointer gets the keyboard. */
    xd_output *out = xd_output_at(server, server->cursor->x, server->cursor->y);
    if (server->lock_focus == nullptr || (out != nullptr && out->wlr_output == ls->surface->output))
        server->lock_focus = ls->surface->surface;
    xd_keyboard_refocus(server);
}

static void handle_surface_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_lock_surface *ls = XD_CONTAINER_OF(listener, xd_lock_surface, destroy);
    xd_server *server = ls->server;
    if (server->lock_focus == ls->surface->surface) {
        server->lock_focus = nullptr;
        xd_keyboard_refocus(server);
    }
    wl_list_remove(&ls->link);
    xd_unlisten(&ls->map);
    xd_unlisten(&ls->destroy);
    /* The role goes before the wl_surface (and with the lock), so the
     * surface's tree would otherwise stay in layer_lock until the client
     * destroys the surface, and twice over if it locks again with it. */
    wlr_scene_node_destroy(&ls->tree->node);
    xd_free(ls);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
    xd_lock *lk = XD_CONTAINER_OF(listener, xd_lock, new_surface);
    xd_server *server = lk->server;
    struct wlr_session_lock_surface_v1 *surface = data;
    xd_lock_surface *ls = XD_NEW(xd_lock_surface);
    ls->server = server;
    ls->surface = surface;
    ls->tree = wlr_scene_subsurface_tree_create(server->layer_lock, surface->surface);
    wl_list_insert(&server->lock_surfaces, &ls->link);
    place_surface(ls);
    XD_LISTEN(&surface->surface->events.map, ls, map, handle_surface_map);
    XD_LISTEN(&surface->events.destroy, ls, destroy, handle_surface_destroy);
}

static void stop_waiting(xd_server *server) {
    server->lock_waiting = false;
    if (server->lock_shown_timer != nullptr) {
        wl_event_source_remove(server->lock_shown_timer);
        server->lock_shown_timer = nullptr;
    }
}

/* The cover is on every monitor: the locker, and the shell, hear so. */
static void tell_locked(xd_server *server) {
    stop_waiting(server);
    if (server->lock != nullptr && !server->lock->told) {
        server->lock->told = true;
        wlr_session_lock_v1_send_locked(server->lock->lock);
    }
    xd_arrange_session_locked(server, true);
}

static int shown_timeout(void *data) {
    xd_server *server = data;
    server->lock_shown_timer = nullptr;
    if (server->lock_waiting) {
        wlr_log(WLR_INFO, "lock: a monitor showed no frame; locked anyway");
        tell_locked(server);
    }
    return 0;
}

void xd_lock_output_shown(xd_output *out) {
    xd_server *server = out->server;
    if (!server->lock_waiting)
        return;
    out->lock_shown = true;
    XD_LIST_FOR_EACH(other, &server->outputs, xd_output, link) {
        if (other->wlr_output->enabled && !other->lock_shown)
            return;
    }
    tell_locked(server);
}

static void end_lock_client(xd_lock *lk) {
    xd_server *server = lk->server;
    xd_unlisten(&lk->new_surface);
    xd_unlisten(&lk->unlock);
    xd_unlisten(&lk->destroy);
    if (server->lock == lk)
        server->lock = nullptr;
    xd_free(lk);
}

static void handle_unlock(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_lock *lk = XD_CONTAINER_OF(listener, xd_lock, unlock);
    xd_server *server = lk->server;
    stop_waiting(server);
    server->locked = false;
    server->lock_focus = nullptr;
    xd_ime_lock_changed(server);
    if (server->lock_cover != nullptr) {
        wlr_scene_node_destroy(&server->lock_cover->node);
        server->lock_cover = nullptr;
    }
    end_lock_client(lk);
    wlr_log(WLR_INFO, "session unlocked");
    xd_keyboard_refocus(server);
    xd_arrange_session_locked(server, false);
}

/* A locker that dies leaves the screen locked with nothing to unlock it
 * from, so start ours again; a few times at most, in case it keeps
 * crashing (Ctrl+Alt+Backspace still ends the session). */
static void restart_locker(xd_server *server) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - server->locker_restart_window.tv_sec > 60) {
        server->locker_restart_window = now;
        server->locker_restarts = 0;
    }
    if (server->locker_restarts >= 5) {
        wlr_log(WLR_ERROR, "screen locker keeps failing; not restarting it");
        return;
    }
    server->locker_restarts++;
    /* On a connection of its own: only it may take the lock over. */
    char sh[] = "/bin/sh", flag[] = "-c";
    char *command = xd_str_dup_cstr(xd_str_from_cstr(server->config.locker));
    char *argv[] = {sh, flag, command, nullptr};
    if (server->config.locker != nullptr && server->config.locker[0] != '\0')
        xd_trust_spawn(server, argv, &server->locker_client);
    xd_free(command);
}

static void handle_lock_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_lock *lk = XD_CONTAINER_OF(listener, xd_lock, destroy);
    xd_server *server = lk->server;
    end_lock_client(lk);
    /* Gone without unlocking: stay locked behind the cover. */
    if (server->locked && !server->terminating) {
        wlr_log(WLR_ERROR, "screen locker exited without unlocking; staying locked");
        restart_locker(server);
    }
}

static void handle_new_lock(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_lock);
    struct wlr_session_lock_v1 *lock = data;
    if (server->lock != nullptr) {
        /* One locker at a time. */
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    /* Locked, its locker gone: only the one we started again takes over
     * (any program could otherwise lock, and so unlock). */
    struct wl_client *client = wl_resource_get_client(lock->resource);
    if (server->locked && !xd_trust_is(&server->locker_client, client)) {
        wlr_log(WLR_ERROR, "lock: refused a locker we didn't start while locked");
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    xd_lock *lk = XD_NEW(xd_lock);
    lk->server = server;
    lk->lock = lock;
    XD_LISTEN(&lock->events.new_surface, lk, new_surface, handle_new_surface);
    XD_LISTEN(&lock->events.unlock, lk, unlock, handle_unlock);
    XD_LISTEN(&lock->events.destroy, lk, destroy, handle_lock_destroy);
    server->lock = lk;

    bool fresh = !server->locked;
    if (!server->locked) {
        server->locked = true;
        /* Menus and drags end; nothing but the locker gets input. */
        wlr_seat_pointer_end_grab(server->seat);
        wlr_seat_keyboard_end_grab(server->seat);
        xd_cursor_cancel_grab(server);
        wlr_seat_pointer_clear_focus(server->seat);
        /* Alt+Tab's window (drawn above the lock) goes, with its titles. */
        server->cycle_view = nullptr;
        xd_switcher_hide(server);
        xd_outline_hide(server);
        xd_ime_lock_changed(server);
        wlr_log(WLR_INFO, "session locked");
    }
    update_cover(server);
    xd_keyboard_refocus(server);
    if (!fresh) {
        /* A locker taking over: the cover has been up all along. */
        tell_locked(server);
        return;
    }
    /* Locked once every monitor has shown the cover. */
    server->lock_waiting = true;
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        out->lock_shown = false;
        wlr_output_schedule_frame(out->wlr_output);
    }
    if (server->lock_shown_timer == nullptr)
        server->lock_shown_timer = wl_event_loop_add_timer(server->loop, shown_timeout, server);
    wl_event_source_timer_update(server->lock_shown_timer, SHOWN_TIMEOUT_MS);
    if (wl_list_empty(&server->outputs))
        tell_locked(server);
}

void xd_lock_init(xd_server *server) {
    wl_list_init(&server->lock_surfaces);
    struct wlr_session_lock_manager_v1 *mgr = wlr_session_lock_manager_v1_create(server->display);
    xd_trust_privileged(server, mgr->global);
    XD_LISTEN(&mgr->events.new_lock, server, new_lock, handle_new_lock);
}
