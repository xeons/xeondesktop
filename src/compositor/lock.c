/* SPDX-License-Identifier: MIT
 * ext-session-lock: screen lockers (swaylock, and later our own "Lock
 * Computer"). While locked, a black cover sits above everything, only the
 * locker's surfaces get input, and grabs and window bindings are off. If
 * the locker dies without unlocking, the screen stays locked until a new
 * locker takes over, as the protocol requires.
 */
#include <time.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

typedef struct xd_lock {
    xd_server *server;
    struct wlr_session_lock_v1 *lock;
    struct wl_listener new_surface;
    struct wl_listener unlock;
    struct wl_listener destroy;
} xd_lock;

typedef struct xd_lock_surface {
    xd_server *server;
    struct wlr_session_lock_surface_v1 *surface;
    struct wlr_scene_tree *tree;
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

void xd_lock_layout_changed(xd_server *server) {
    if (server->locked)
        update_cover(server);
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
    xd_unlisten(&ls->map);
    xd_unlisten(&ls->destroy);
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
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, surface->output, &box);
    wlr_scene_node_set_position(&ls->tree->node, box.x, box.y);
    wlr_session_lock_surface_v1_configure(surface, (uint32_t)box.width, (uint32_t)box.height);
    XD_LISTEN(&surface->surface->events.map, ls, map, handle_surface_map);
    XD_LISTEN(&surface->events.destroy, ls, destroy, handle_surface_destroy);
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
    server->locked = false;
    server->lock_focus = nullptr;
    if (server->lock_cover != nullptr) {
        wlr_scene_node_destroy(&server->lock_cover->node);
        server->lock_cover = nullptr;
    }
    end_lock_client(lk);
    wlr_log(WLR_INFO, "session unlocked");
    xd_keyboard_refocus(server);
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
    xd_spawn(server, server->config.locker);
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
    xd_lock *lk = XD_NEW(xd_lock);
    lk->server = server;
    lk->lock = lock;
    XD_LISTEN(&lock->events.new_surface, lk, new_surface, handle_new_surface);
    XD_LISTEN(&lock->events.unlock, lk, unlock, handle_unlock);
    XD_LISTEN(&lock->events.destroy, lk, destroy, handle_lock_destroy);
    server->lock = lk;

    if (!server->locked) {
        server->locked = true;
        /* Menus and drags end; nothing but the locker gets input. */
        wlr_seat_pointer_end_grab(server->seat);
        wlr_seat_keyboard_end_grab(server->seat);
        server->mode = XD_CURSOR_PASSTHROUGH;
        server->grab_view = nullptr;
        wlr_seat_pointer_clear_focus(server->seat);
        wlr_log(WLR_INFO, "session locked");
    }
    update_cover(server);
    xd_keyboard_refocus(server);
    /* The cover is already up, so the screen is locked as of now. */
    wlr_session_lock_v1_send_locked(lock);
}

void xd_lock_init(xd_server *server) {
    struct wlr_session_lock_manager_v1 *mgr = wlr_session_lock_manager_v1_create(server->display);
    XD_LISTEN(&mgr->events.new_lock, server, new_lock, handle_new_lock);
}
