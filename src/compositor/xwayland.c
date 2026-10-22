/* SPDX-License-Identifier: MIT
 * X11 applications through Xwayland.
 *
 * Xwayland starts on demand (when an X11 client first connects); main.c
 * sets DISPLAY for everything we launch. Normal X11 windows become views
 * like any other (our frame unless their Motif hints ask for none,
 * maximize, minimize, fullscreen, move and resize). Override-redirect
 * windows (menus, tooltips, drop-downs) are shown where the application
 * puts them, above normal windows, without a frame or a taskbar button.
 */
#include <wlr/util/log.h>
#include <xcb/xcb_icccm.h>

#include "../core/alloc.h"
#include "server.h"

/* ---- override-redirect windows ------------------------------------------------- */

typedef struct xd_unmanaged {
    xd_server *server;
    struct wlr_xwayland_surface *xsurface;
    struct wlr_scene_tree *tree;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener set_geometry;
    struct wl_listener request_configure;
    struct wl_listener destroy;
} xd_unmanaged;

static void unmanaged_place(xd_unmanaged *unmanaged) {
    if (unmanaged->tree != nullptr)
        wlr_scene_node_set_position(&unmanaged->tree->node, unmanaged->xsurface->x,
                                    unmanaged->xsurface->y);
}

static void handle_um_map(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_unmanaged *unmanaged = XD_CONTAINER_OF(listener, xd_unmanaged, map);
    unmanaged->tree = wlr_scene_subsurface_tree_create(unmanaged->server->layer_x11_popups,
                                                       unmanaged->xsurface->surface);
    unmanaged_place(unmanaged);
    /* Drop-down menus and the like may want the keyboard; never while the
     * screen is locked, when it belongs to the lock dialog (else an X11
     * program could read the password typed into it). */
    if (!unmanaged->server->locked &&
        wlr_xwayland_surface_override_redirect_wants_focus(unmanaged->xsurface)) {
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(unmanaged->server->seat);
        if (kb != nullptr)
            wlr_seat_keyboard_notify_enter(unmanaged->server->seat, unmanaged->xsurface->surface,
                                           kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
}

static void handle_um_unmap(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_unmanaged *unmanaged = XD_CONTAINER_OF(listener, xd_unmanaged, unmap);
    if (unmanaged->tree != nullptr) {
        wlr_scene_node_destroy(&unmanaged->tree->node);
        unmanaged->tree = nullptr;
    }
    xd_keyboard_refocus(unmanaged->server);
}

static void handle_um_associate(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_unmanaged *unmanaged = XD_CONTAINER_OF(listener, xd_unmanaged, associate);
    XD_LISTEN(&unmanaged->xsurface->surface->events.map, unmanaged, map, handle_um_map);
    XD_LISTEN(&unmanaged->xsurface->surface->events.unmap, unmanaged, unmap, handle_um_unmap);
}

static void handle_um_dissociate(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_unmanaged *unmanaged = XD_CONTAINER_OF(listener, xd_unmanaged, dissociate);
    xd_unlisten(&unmanaged->map);
    xd_unlisten(&unmanaged->unmap);
}

static void handle_um_geometry(struct wl_listener *listener, [[maybe_unused]] void *data) {
    unmanaged_place(XD_CONTAINER_OF(listener, xd_unmanaged, set_geometry));
}

static void handle_um_configure(struct wl_listener *listener, void *data) {
    xd_unmanaged *unmanaged = XD_CONTAINER_OF(listener, xd_unmanaged, request_configure);
    const struct wlr_xwayland_surface_configure_event *xwayland_surface_configure_event = data;
    wlr_xwayland_surface_configure(unmanaged->xsurface, xwayland_surface_configure_event->x,
                                   xwayland_surface_configure_event->y,
                                   xwayland_surface_configure_event->width,
                                   xwayland_surface_configure_event->height);
    unmanaged_place(unmanaged);
}

static void handle_um_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_unmanaged *unmanaged = XD_CONTAINER_OF(listener, xd_unmanaged, destroy);
    if (unmanaged->tree != nullptr)
        wlr_scene_node_destroy(&unmanaged->tree->node);
    xd_unlisten(&unmanaged->associate);
    xd_unlisten(&unmanaged->dissociate);
    xd_unlisten(&unmanaged->map);
    xd_unlisten(&unmanaged->unmap);
    xd_unlisten(&unmanaged->set_geometry);
    xd_unlisten(&unmanaged->request_configure);
    xd_unlisten(&unmanaged->destroy);
    xd_free(unmanaged);
}

static void new_unmanaged(xd_server *server, struct wlr_xwayland_surface *xs) {
    xd_unmanaged *unmanaged = XD_NEW(xd_unmanaged);
    unmanaged->server = server;
    unmanaged->xsurface = xs;
    XD_LISTEN(&xs->events.associate, unmanaged, associate, handle_um_associate);
    XD_LISTEN(&xs->events.dissociate, unmanaged, dissociate, handle_um_dissociate);
    XD_LISTEN(&xs->events.set_geometry, unmanaged, set_geometry, handle_um_geometry);
    XD_LISTEN(&xs->events.request_configure, unmanaged, request_configure, handle_um_configure);
    XD_LISTEN(&xs->events.destroy, unmanaged, destroy, handle_um_destroy);
}

/* ---- managed windows ------------------------------------------------------------ */

static void update_pings(xd_view *view);

static void handle_x_map(struct wl_listener *listener, [[maybe_unused]] void *data) {
    update_pings(XD_CONTAINER_OF(listener, xd_view, map));
    xd_view_on_map(XD_CONTAINER_OF(listener, xd_view, map));
}

static void handle_x_unmap(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_on_unmap(XD_CONTAINER_OF(listener, xd_view, unmap));
}

static void handle_x_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_on_commit(XD_CONTAINER_OF(listener, xd_view, commit));
}

/* The surface's scene tree goes away by itself when the surface is
 * destroyed, which can happen before Xwayland dissociates the window. */
static void handle_x_content_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, content_destroy);
    xd_unlisten(&view->content_destroy);
    view->content = nullptr;
}

static void handle_x_associate(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, associate);
    struct wlr_surface *surface = view->xsurface->surface;
    view->content = wlr_scene_subsurface_tree_create(view->tree, surface);
    view->content->node.data = nullptr;
    XD_LISTEN(&view->content->node.events.destroy, view, content_destroy, handle_x_content_destroy);
    XD_LISTEN(&surface->events.map, view, map, handle_x_map);
    XD_LISTEN(&surface->events.unmap, view, unmap, handle_x_unmap);
    XD_LISTEN(&surface->events.commit, view, commit, handle_x_commit);
}

static void handle_x_dissociate(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, dissociate);
    xd_unlisten(&view->map);
    xd_unlisten(&view->unmap);
    xd_unlisten(&view->commit);
    if (view->content != nullptr)
        wlr_scene_node_destroy(&view->content->node); /* clears view->content */
}

static void handle_x_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, destroy);
    view->xsurface->data = nullptr;
    xd_view_on_destroy(view);
}

/* Before mapping, X11 windows pick their own size and place; after, they
 * may only resize (we place them). */
static void handle_x_configure(struct wl_listener *listener, void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_configure);
    const struct wlr_xwayland_surface_configure_event *xwayland_surface_configure_event = data;
    if (!view->mapped) {
        wlr_xwayland_surface_configure(view->xsurface, xwayland_surface_configure_event->x,
                                       xwayland_surface_configure_event->y,
                                       xwayland_surface_configure_event->width,
                                       xwayland_surface_configure_event->height);
        return;
    }
    if (view->maximized || view->fullscreen) {
        xd_view_x11_sync(view);
        return;
    }
    xd_insets in = xd_view_insets(view);
    xd_view_set_outer_box(
        view, (struct wlr_box){view->x, view->y,
                               xwayland_surface_configure_event->width + in.left + in.right,
                               xwayland_surface_configure_event->height + in.top + in.bottom});
}

static void handle_x_move(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_move);
    xd_server *server = view->server;
    /* Never while locked: the pointer is the lock dialog's. */
    if (server->locked || server->mode != XD_CURSOR_PASSTHROUGH || !xd_snap_begin(server, view))
        return;
    server->mode = XD_CURSOR_MOVE;
    server->grab_view = view;
    server->grab_x = server->cursor->x - view->x;
    server->grab_y = server->cursor->y - view->y;
    server->grab_press_x = server->cursor->x;
    server->grab_press_y = server->cursor->y;
}

static xd_frame_part part_from_x11_edges(uint32_t edges) {
    bool top = edges & WLR_EDGE_TOP, bottom = edges & WLR_EDGE_BOTTOM, left = edges & WLR_EDGE_LEFT,
         right = edges & WLR_EDGE_RIGHT;
    return top && left       ? XD_PART_TOP_LEFT
           : top && right    ? XD_PART_TOP_RIGHT
           : bottom && left  ? XD_PART_BOTTOM_LEFT
           : bottom && right ? XD_PART_BOTTOM_RIGHT
           : top             ? XD_PART_TOP
           : bottom          ? XD_PART_BOTTOM
           : left            ? XD_PART_LEFT
           : right           ? XD_PART_RIGHT
                             : XD_PART_NONE;
}

static void handle_x_resize(struct wl_listener *listener, void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_resize);
    const struct wlr_xwayland_resize_event *xwayland_resize_event = data;
    xd_server *server = view->server;
    xd_frame_part part = part_from_x11_edges(xwayland_resize_event->edges);
    if (server->locked || view->maximized || part == XD_PART_NONE ||
        server->mode != XD_CURSOR_PASSTHROUGH)
        return;
    server->mode = XD_CURSOR_RESIZE;
    server->grab_view = view;
    server->grab_part = part;
    server->grab_x = server->cursor->x;
    server->grab_y = server->cursor->y;
    server->grab_box = xd_view_outer_box(view);
}

static void handle_x_maximize(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_maximize);
    xd_view_set_maximized(view, view->xsurface->maximized_vert && view->xsurface->maximized_horz);
}

static void handle_x_minimize(struct wl_listener *listener, void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_minimize);
    const struct wlr_xwayland_minimize_event *xwayland_minimize_event = data;
    if (xwayland_minimize_event->minimize)
        xd_view_minimize(view);
    else
        xd_view_focus(view);
}

static void handle_x_fullscreen(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_fullscreen);
    if (view->mapped)
        xd_view_set_fullscreen(view, view->xsurface->fullscreen);
}

/* _NET_ACTIVE_WINDOW: an X11 app may move the focus between its own
 * windows (or take it when nothing has it); otherwise it only flashes. */
static void handle_x_activate(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_activate);
    if (!view->mapped || view->server->locked)
        return;
    xd_view *focused = xd_view_focused(view->server);
    if (focused == nullptr ||
        (focused->kind == XD_VIEW_X11 && focused->xsurface->pid == view->xsurface->pid))
        xd_view_focus(view);
    else if (focused != view)
        xd_view_attention(view);
}

/* The ICCCM urgency hint: flash the taskbar button. */
static void handle_x_hints(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, set_hints);
    const xcb_icccm_wm_hints_t *hints = view->xsurface->hints;
    if (view->mapped && hints != nullptr && (hints->flags & XCB_ICCCM_WM_HINT_X_URGENCY) &&
        view != xd_view_focused(view->server))
        xd_view_attention(view);
}

static void handle_x_close(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_close(XD_CONTAINER_OF(listener, xd_view, request_close));
}

static void handle_x_title(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_title_changed(XD_CONTAINER_OF(listener, xd_view, set_title));
}

/* Only programs that list _NET_WM_PING answer pings; others would look
 * hung. */
static void update_pings(xd_view *view) {
    const struct wlr_xwayland_surface *xs = view->xsurface;
    view->pings = false;
    for (size_t i = 0; view->server->net_wm_ping != 0 && i < xs->protocols_len; i++)
        if (xs->protocols[i] == view->server->net_wm_ping)
            view->pings = true;
}

static void handle_x_ping_timeout(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_ping_timed_out(XD_CONTAINER_OF(listener, xd_view, ping_timeout));
}

static void handle_x_class(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, set_app_id);
    if (view->ftl != nullptr && view->xsurface->class != nullptr)
        wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, view->xsurface->class);
}

static void handle_x_decorations(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_update_decorations(XD_CONTAINER_OF(listener, xd_view, set_decorations));
}

static void new_managed(xd_server *server, struct wlr_xwayland_surface *xs) {
    xd_view *view = XD_NEW(xd_view);
    view->tag.kind = XD_NODE_VIEW;
    view->kind = XD_VIEW_X11;
    view->server = server;
    view->xsurface = xs;
    xs->data = view;
    view->tree = wlr_scene_tree_create(server->layer_views);
    view->tree->node.data = &view->tag;
    view->frame = wlr_scene_buffer_create(view->tree, nullptr);
    wl_list_insert(server->views.prev, &view->link);

    XD_LISTEN(&xs->events.associate, view, associate, handle_x_associate);
    XD_LISTEN(&xs->events.dissociate, view, dissociate, handle_x_dissociate);
    XD_LISTEN(&xs->events.destroy, view, destroy, handle_x_destroy);
    XD_LISTEN(&xs->events.request_configure, view, request_configure, handle_x_configure);
    XD_LISTEN(&xs->events.request_move, view, request_move, handle_x_move);
    XD_LISTEN(&xs->events.request_resize, view, request_resize, handle_x_resize);
    XD_LISTEN(&xs->events.request_maximize, view, request_maximize, handle_x_maximize);
    XD_LISTEN(&xs->events.request_minimize, view, request_minimize, handle_x_minimize);
    XD_LISTEN(&xs->events.request_fullscreen, view, request_fullscreen, handle_x_fullscreen);
    XD_LISTEN(&xs->events.request_activate, view, request_activate, handle_x_activate);
    XD_LISTEN(&xs->events.request_close, view, request_close, handle_x_close);
    XD_LISTEN(&xs->events.set_title, view, set_title, handle_x_title);
    XD_LISTEN(&xs->events.set_class, view, set_app_id, handle_x_class);
    XD_LISTEN(&xs->events.set_decorations, view, set_decorations, handle_x_decorations);
    XD_LISTEN(&xs->events.set_hints, view, set_hints, handle_x_hints);
    XD_LISTEN(&xs->events.ping_timeout, view, ping_timeout, handle_x_ping_timeout);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, xwayland_new_surface);
    struct wlr_xwayland_surface *xs = data;
    if (xs->override_redirect)
        new_unmanaged(server, xs);
    else
        new_managed(server, xs);
}

static void handle_ready(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, xwayland_ready);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
    xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (conn != nullptr) {
        static const char name[] = "_NET_WM_PING";
        xcb_intern_atom_reply_t *atom_reply =
            xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, sizeof(name) - 1, name), nullptr);
        if (atom_reply != nullptr) {
            server->net_wm_ping = atom_reply->atom;
            free(atom_reply);
        }
    }
    wlr_log(WLR_INFO, "Xwayland ready on DISPLAY=%s", server->xwayland->display_name);
}

void xd_xwayland_init(xd_server *server) {
    /* Lazy: Xwayland only starts when an X11 client connects. */
    server->xwayland = wlr_xwayland_create(server->display, server->compositor, true);
    if (server->xwayland == nullptr) {
        wlr_log(WLR_ERROR, "Xwayland is not available; X11 applications won't run");
        return;
    }
    XD_LISTEN(&server->xwayland->events.ready, server, xwayland_ready, handle_ready);
    XD_LISTEN(&server->xwayland->events.new_surface, server, xwayland_new_surface,
              handle_new_surface);
    wlr_log(WLR_INFO, "X11 applications use DISPLAY=%s", server->xwayland->display_name);
}

void xd_xwayland_finish(xd_server *server) {
    if (server->xwayland == nullptr)
        return;
    xd_unlisten(&server->xwayland_ready);
    xd_unlisten(&server->xwayland_new_surface);
    wlr_xwayland_destroy(server->xwayland);
    server->xwayland = nullptr;
}
