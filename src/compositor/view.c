/* SPDX-License-Identifier: MIT
 * Toplevel windows ("views"): lifecycle, focus, placement, maximize and
 * minimize, server-side decorations, and popups.
 */
#include <math.h>
#include <stdlib.h>

#include "../core/alloc.h"
#include "server.h"

static xd_frame view_frame_desc(const xd_view *view) {
    return (xd_frame){
        .title = "",
        .active = true,
        .maximized = view->maximized,
        .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE,
    };
}

xd_insets xd_view_insets(const xd_view *view) {
    xd_frame f = view_frame_desc(view);
    return xd_theme_frame_insets(&view->server->logical_theme, &f);
}

static struct wlr_box client_geometry(const xd_view *view) {
    return view->toplevel->base->geometry;
}

struct wlr_box xd_view_outer_box(const xd_view *view) {
    xd_insets in = xd_view_insets(view);
    struct wlr_box geo = client_geometry(view);
    return (struct wlr_box){
        .x = view->x,
        .y = view->y,
        .width = geo.width + in.left + in.right,
        .height = geo.height + in.top + in.bottom,
    };
}

xd_view *xd_view_from_node(struct wlr_scene_node *node) {
    for (; node != nullptr; node = node->parent != nullptr ? &node->parent->node : nullptr) {
        xd_node_tag *tag = node->data;
        if (tag != nullptr && tag->kind == XD_NODE_VIEW)
            return XD_CONTAINER_OF(tag, xd_view, tag);
    }
    return nullptr;
}

xd_view *xd_view_focused(xd_server *server) {
    if (wl_list_empty(&server->views))
        return nullptr;
    xd_view *front = XD_CONTAINER_OF(server->views.next, xd_view, link);
    return front->mapped && !front->minimized ? front : nullptr;
}

/* Places the client surfaces inside the frame. */
static void update_content_position(xd_view *view) {
    xd_insets in = xd_view_insets(view);
    struct wlr_box geo = client_geometry(view);
    wlr_scene_node_set_position(&view->content->node, in.left - geo.x, in.top - geo.y);
}

/* Tells the taskbar which output the window is on (by its centre). */
static void update_ftl_output(xd_view *view) {
    if (view->ftl == nullptr)
        return;
    struct wlr_box b = xd_view_outer_box(view);
    xd_output *out = xd_output_at(view->server, b.x + b.width / 2.0, b.y + b.height / 2.0);
    struct wlr_output *o = out != nullptr ? out->wlr_output : nullptr;
    if (o == view->ftl_output)
        return;
    if (view->ftl_output != nullptr)
        wlr_foreign_toplevel_handle_v1_output_leave(view->ftl, view->ftl_output);
    if (o != nullptr)
        wlr_foreign_toplevel_handle_v1_output_enter(view->ftl, o);
    view->ftl_output = o;
}

void xd_view_set_position(xd_view *view, int x, int y) {
    view->x = x;
    view->y = y;
    wlr_scene_node_set_position(&view->tree->node, x, y);
    update_ftl_output(view);
    /* The frame's pixel scale follows the output it's on. */
    xd_frame_render(view, false);
}

static void send_client_size(xd_view *view, int outer_w, int outer_h) {
    xd_insets in = xd_view_insets(view);
    int w = XD_MAX(outer_w - in.left - in.right, XD_MIN_CLIENT_W);
    int h = XD_MAX(outer_h - in.top - in.bottom, XD_MIN_CLIENT_H);
    wlr_xdg_toplevel_set_size(view->toplevel, w, h);
}

void xd_view_set_outer_box(xd_view *view, struct wlr_box outer) {
    xd_view_set_position(view, outer.x, outer.y);
    send_client_size(view, outer.width, outer.height);
}

static int floor_int(double v) {
    double f = floor(v);
    XD_ASSERT(f > -32768.0 && f < 32768.0);
    return (int)f;
}

xd_frame_part xd_view_hit(const xd_view *view, double lx, double ly) {
    struct wlr_box outer = xd_view_outer_box(view);
    double scale = view->drawn.scale > 0 ? view->drawn.scale : 1.0;
    xd_theme theme;
    xd_theme_init(&theme, view->server->config.style, &view->server->config.scheme, scale);
    xd_frame f = view_frame_desc(view);
    xd_rect dev = {0, 0, -floor_int(-outer.width * scale), -floor_int(-outer.height * scale)};
    int px = floor_int((lx - outer.x) * scale);
    int py = floor_int((ly - outer.y) * scale);
    return xd_theme_frame_hit(&theme, dev, &f, px, py);
}

void xd_view_set_hover(xd_view *view, xd_frame_part hot, xd_frame_part pressed) {
    if (view->hot == hot && view->pressed == pressed)
        return;
    view->hot = hot;
    view->pressed = pressed;
    xd_frame_render(view, false);
}

void xd_view_update_all_frames(xd_server *server) {
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        xd_frame_render(view, false);
    }
}

static void set_activated(xd_view *view, bool activated) {
    wlr_xdg_toplevel_set_activated(view->toplevel, activated);
    if (view->ftl != nullptr)
        wlr_foreign_toplevel_handle_v1_set_activated(view->ftl, activated);
    xd_frame_render(view, false);
}

void xd_view_focus(xd_view *view) {
    xd_server *server = view->server;
    struct wlr_surface *surface = view->toplevel->base->surface;
    xd_view *prev = xd_view_focused(server);

    if (view->minimized) {
        view->minimized = false;
        wlr_scene_node_set_enabled(&view->tree->node, true);
        if (view->ftl != nullptr)
            wlr_foreign_toplevel_handle_v1_set_minimized(view->ftl, false);
    }

    wlr_scene_node_raise_to_top(&view->tree->node);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);

    if (prev != nullptr && prev != view)
        set_activated(prev, false);
    set_activated(view, true);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    if (kb != nullptr)
        wlr_seat_keyboard_notify_enter(server->seat, surface, kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
}

/* Focuses the most recently used visible window, or clears focus. */
void xd_view_focus_next(xd_server *server) {
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (view->mapped && !view->minimized) {
            xd_view_focus(view);
            return;
        }
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
}

void xd_view_close(xd_view *view) { wlr_xdg_toplevel_send_close(view->toplevel); }

void xd_view_set_maximized(xd_view *view, bool maximized) {
    if (!view->toplevel->base->initialized)
        return;
    if (view->maximized == maximized) {
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
        return;
    }
    if (maximized) {
        struct wlr_box outer = xd_view_outer_box(view);
        view->restore = outer;
        struct wlr_box area = xd_output_work_area_at(view->server, outer.x + outer.width / 2.0,
                                                     outer.y + outer.height / 2.0);
        view->maximized = true;
        wlr_xdg_toplevel_set_maximized(view->toplevel, true);
        if (view->ftl != nullptr)
            wlr_foreign_toplevel_handle_v1_set_maximized(view->ftl, true);
        xd_view_set_outer_box(view, area);
    } else {
        view->maximized = false;
        wlr_xdg_toplevel_set_maximized(view->toplevel, false);
        if (view->ftl != nullptr)
            wlr_foreign_toplevel_handle_v1_set_maximized(view->ftl, false);
        xd_view_set_outer_box(view, view->restore);
    }
}

void xd_view_minimize(xd_view *view) {
    if (view->minimized || !view->mapped)
        return;
    bool was_focused = xd_view_focused(view->server) == view;
    view->minimized = true;
    wlr_scene_node_set_enabled(&view->tree->node, false);
    if (view->ftl != nullptr)
        wlr_foreign_toplevel_handle_v1_set_minimized(view->ftl, true);
    set_activated(view, false);
    if (was_focused) {
        /* Send it to the back of the focus order, like Windows does. */
        wl_list_remove(&view->link);
        wl_list_insert(view->server->views.prev, &view->link);
        xd_view_focus_next(view->server);
    }
}

void xd_view_restore(xd_view *view) {
    if (view->minimized)
        xd_view_focus(view);
    else if (view->maximized)
        xd_view_set_maximized(view, false);
}

/* Windows-style cascade: each new window opens one caption height lower
 * and to the right of the last, wrapping when it would leave the screen. */
static void place_new_view(xd_view *view) {
    xd_server *server = view->server;
    struct wlr_box area =
        xd_output_work_area_at(server, server->cursor->x, server->cursor->y);
    struct wlr_box outer = xd_view_outer_box(view);
    xd_insets in = xd_view_insets(view);
    int step = in.top;

    for (int attempt = 0; attempt < 2; attempt++) {
        int x = area.x + step / 2 + server->cascade * step;
        int y = area.y + step / 2 + server->cascade * step;
        if (x + outer.width <= area.x + area.width && y + outer.height <= area.y + area.height) {
            server->cascade++;
            xd_view_set_position(view, x, y);
            return;
        }
        server->cascade = 0;
    }
    /* Larger than the work area: shrink it to fit so the caption and the
     * caption buttons stay reachable. */
    if (outer.width > area.width || outer.height > area.height) {
        struct wlr_box fit = {area.x, area.y, XD_MIN(outer.width, area.width),
                              XD_MIN(outer.height, area.height)};
        xd_view_set_outer_box(view, fit);
        return;
    }
    /* Fits, but not at the cascade position: centre it. */
    xd_view_set_position(view, area.x + (area.width - outer.width) / 2,
                         area.y + (area.height - outer.height) / 2);
}

/* ---- taskbar handle (wlr-foreign-toplevel-management) ---------------------- */

static void handle_ftl_activate(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, ftl_activate);
    xd_view_focus(view);
}

static void handle_ftl_minimize(struct wl_listener *listener, void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, ftl_minimize);
    const struct wlr_foreign_toplevel_handle_v1_minimized_event *e = data;
    if (e->minimized)
        xd_view_minimize(view);
    else
        xd_view_focus(view);
}

static void handle_ftl_maximize(struct wl_listener *listener, void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, ftl_maximize);
    const struct wlr_foreign_toplevel_handle_v1_maximized_event *e = data;
    xd_view_set_maximized(view, e->maximized);
}

static void handle_ftl_close(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, ftl_close);
    xd_view_close(view);
}

static void ftl_create(xd_view *view) {
    view->ftl = wlr_foreign_toplevel_handle_v1_create(view->server->ftl_manager);
    if (view->toplevel->title != nullptr)
        wlr_foreign_toplevel_handle_v1_set_title(view->ftl, view->toplevel->title);
    if (view->toplevel->app_id != nullptr)
        wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, view->toplevel->app_id);
    wlr_foreign_toplevel_handle_v1_set_maximized(view->ftl, view->maximized);
    XD_LISTEN(&view->ftl->events.request_activate, view, ftl_activate, handle_ftl_activate);
    XD_LISTEN(&view->ftl->events.request_minimize, view, ftl_minimize, handle_ftl_minimize);
    XD_LISTEN(&view->ftl->events.request_maximize, view, ftl_maximize, handle_ftl_maximize);
    XD_LISTEN(&view->ftl->events.request_close, view, ftl_close, handle_ftl_close);
}

static void ftl_destroy(xd_view *view) {
    if (view->ftl == nullptr)
        return;
    xd_unlisten(&view->ftl_activate);
    xd_unlisten(&view->ftl_minimize);
    xd_unlisten(&view->ftl_maximize);
    xd_unlisten(&view->ftl_close);
    wlr_foreign_toplevel_handle_v1_destroy(view->ftl);
    view->ftl = nullptr;
    view->ftl_output = nullptr;
}

void xd_view_refit_maximized(xd_server *server, xd_output *out) {
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (!view->mapped || !view->maximized)
            continue;
        struct wlr_box b = xd_view_outer_box(view);
        if (xd_output_at(server, b.x + b.width / 2.0, b.y + b.height / 2.0) == out)
            xd_view_set_outer_box(view, out->usable);
    }
}

void xd_view_output_gone(xd_server *server, struct wlr_output *wlr_output) {
    /* wlroots drops the output from each handle itself. */
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (view->ftl_output == wlr_output)
            view->ftl_output = nullptr;
    }
}

static void handle_map(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, map);
    view->mapped = true;
    ftl_create(view);
    update_content_position(view);
    place_new_view(view);
    xd_view_focus(view);
    xd_frame_render(view, true);
}

static void handle_unmap(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, unmap);
    xd_server *server = view->server;
    bool was_focused = xd_view_focused(server) == view;
    view->mapped = false;
    view->minimized = false;
    wlr_scene_node_set_enabled(&view->tree->node, true);
    ftl_destroy(view);

    if (server->grab_view == view) {
        server->mode = XD_CURSOR_PASSTHROUGH;
        server->grab_view = nullptr;
    }
    if (server->hover_view == view)
        server->hover_view = nullptr;
    if (server->last_click_view == view)
        server->last_click_view = nullptr;
    if (server->cycle_view == view)
        server->cycle_view = nullptr;

    /* Move to the back so focus_next picks a different window. */
    wl_list_remove(&view->link);
    wl_list_insert(server->views.prev, &view->link);
    if (was_focused)
        xd_view_focus_next(server);
}

static void handle_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, commit);
    struct wlr_xdg_surface *xdg = view->toplevel->base;

    if (xdg->initial_commit) {
        /* Let the client pick its size, and tell it we draw the frame. */
        wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
        if (view->decoration != nullptr)
            wlr_xdg_toplevel_decoration_v1_set_mode(
                view->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        return;
    }
    if (!view->mapped)
        return;
    update_content_position(view);
    xd_frame_render(view, false);
}

static void handle_request_move(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_move);
    xd_server *server = view->server;
    if (view->maximized || server->mode != XD_CURSOR_PASSTHROUGH)
        return;
    server->mode = XD_CURSOR_MOVE;
    server->grab_view = view;
    server->grab_x = server->cursor->x - view->x;
    server->grab_y = server->cursor->y - view->y;
}

static xd_frame_part part_from_edges(uint32_t edges) {
    bool top = edges & WLR_EDGE_TOP;
    bool bottom = edges & WLR_EDGE_BOTTOM;
    bool left = edges & WLR_EDGE_LEFT;
    bool right = edges & WLR_EDGE_RIGHT;
    if (top && left)
        return XD_PART_TOP_LEFT;
    if (top && right)
        return XD_PART_TOP_RIGHT;
    if (bottom && left)
        return XD_PART_BOTTOM_LEFT;
    if (bottom && right)
        return XD_PART_BOTTOM_RIGHT;
    if (top)
        return XD_PART_TOP;
    if (bottom)
        return XD_PART_BOTTOM;
    if (left)
        return XD_PART_LEFT;
    if (right)
        return XD_PART_RIGHT;
    return XD_PART_NONE;
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_resize);
    const struct wlr_xdg_toplevel_resize_event *event = data;
    xd_server *server = view->server;
    xd_frame_part part = part_from_edges(event->edges);
    if (view->maximized || part == XD_PART_NONE || server->mode != XD_CURSOR_PASSTHROUGH)
        return;
    server->mode = XD_CURSOR_RESIZE;
    server->grab_view = view;
    server->grab_part = part;
    server->grab_x = server->cursor->x;
    server->grab_y = server->cursor->y;
    server->grab_box = xd_view_outer_box(view);
    wlr_xdg_toplevel_set_resizing(view->toplevel, true);
}

static void handle_request_maximize(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_maximize);
    xd_view_set_maximized(view, view->toplevel->requested.maximized);
}

static void handle_request_minimize(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_minimize);
    xd_view_minimize(view);
}

static void handle_request_fullscreen(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_fullscreen);
    /* Not supported yet; the protocol still requires a configure reply. */
    if (view->toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
}

static void handle_set_title(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, set_title);
    if (view->ftl != nullptr && view->toplevel->title != nullptr)
        wlr_foreign_toplevel_handle_v1_set_title(view->ftl, view->toplevel->title);
    view->drawn.title_dirty = true;
    xd_frame_render(view, false);
}

static void handle_set_app_id(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, set_app_id);
    if (view->ftl != nullptr && view->toplevel->app_id != nullptr)
        wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, view->toplevel->app_id);
}

static void handle_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, destroy);
    ftl_destroy(view);
    xd_server *server = view->server;
    if (server->grab_view == view) {
        server->grab_view = nullptr;
        server->mode = XD_CURSOR_PASSTHROUGH;
    }
    if (server->hover_view == view)
        server->hover_view = nullptr;
    if (server->last_click_view == view)
        server->last_click_view = nullptr;
    if (server->cycle_view == view)
        server->cycle_view = nullptr;

    xd_unlisten(&view->map);
    xd_unlisten(&view->unmap);
    xd_unlisten(&view->commit);
    xd_unlisten(&view->destroy);
    xd_unlisten(&view->request_move);
    xd_unlisten(&view->request_resize);
    xd_unlisten(&view->request_maximize);
    xd_unlisten(&view->request_minimize);
    xd_unlisten(&view->request_fullscreen);
    xd_unlisten(&view->set_title);
    xd_unlisten(&view->set_app_id);
    xd_unlisten(&view->decoration_mode);
    xd_unlisten(&view->decoration_destroy);
    wl_list_remove(&view->link);
    /* Takes the frame buffer and the client's scene subtree with it. */
    wlr_scene_node_destroy(&view->tree->node);
    xd_free(view);
}

static void handle_new_toplevel(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;

    xd_view *view = XD_NEW(xd_view);
    view->tag.kind = XD_NODE_VIEW;
    view->server = server;
    view->toplevel = toplevel;
    view->tree = wlr_scene_tree_create(server->layer_views);
    view->tree->node.data = &view->tag;
    view->frame = wlr_scene_buffer_create(view->tree, nullptr);
    view->content = wlr_scene_xdg_surface_create(view->tree, toplevel->base);
    /* Popups find their parent's scene tree through this. */
    toplevel->base->data = view->content;
    wl_list_insert(server->views.prev, &view->link);

    XD_LISTEN(&toplevel->base->surface->events.map, view, map, handle_map);
    XD_LISTEN(&toplevel->base->surface->events.unmap, view, unmap, handle_unmap);
    XD_LISTEN(&toplevel->base->surface->events.commit, view, commit, handle_commit);
    XD_LISTEN(&toplevel->events.destroy, view, destroy, handle_destroy);
    XD_LISTEN(&toplevel->events.request_move, view, request_move, handle_request_move);
    XD_LISTEN(&toplevel->events.request_resize, view, request_resize, handle_request_resize);
    XD_LISTEN(&toplevel->events.request_maximize, view, request_maximize,
              handle_request_maximize);
    XD_LISTEN(&toplevel->events.request_minimize, view, request_minimize,
              handle_request_minimize);
    XD_LISTEN(&toplevel->events.request_fullscreen, view, request_fullscreen,
              handle_request_fullscreen);
    XD_LISTEN(&toplevel->events.set_title, view, set_title, handle_set_title);
    XD_LISTEN(&toplevel->events.set_app_id, view, set_app_id, handle_set_app_id);
}

/* ---- decorations --------------------------------------------------------- */

static void handle_decoration_mode(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, decoration_mode);
    /* Every window gets a themed frame, whatever the client asks for. */
    if (view->toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
                                                WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void handle_decoration_destroy(struct wl_listener *listener,
                                      [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, decoration_destroy);
    xd_unlisten(&view->decoration_mode);
    xd_unlisten(&view->decoration_destroy);
    view->decoration = nullptr;
}

static void handle_new_decoration([[maybe_unused]] struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    struct wlr_scene_tree *content = deco->toplevel->base->data;
    xd_view *view = content != nullptr ? xd_view_from_node(&content->node) : nullptr;
    if (view == nullptr)
        return;
    view->decoration = deco;
    XD_LISTEN(&deco->events.request_mode, view, decoration_mode, handle_decoration_mode);
    XD_LISTEN(&deco->events.destroy, view, decoration_destroy, handle_decoration_destroy);
    handle_decoration_mode(&view->decoration_mode, nullptr);
}

/* ---- popups --------------------------------------------------------------- */

typedef struct xd_popup {
    struct wlr_xdg_popup *popup;
    struct wl_listener commit;
    struct wl_listener destroy;
} xd_popup;

static void handle_popup_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_popup *p = XD_CONTAINER_OF(listener, xd_popup, commit);
    if (!p->popup->base->initial_commit)
        return;

    /* Keep the popup on the output its toplevel is on. */
    struct wlr_scene_tree *tree = p->popup->base->data;
    xd_view *view = tree != nullptr ? xd_view_from_node(&tree->node) : nullptr;
    if (view != nullptr) {
        int cx = 0;
        int cy = 0;
        wlr_scene_node_coords(&view->content->node, &cx, &cy);
        struct wlr_box area = xd_output_work_area_at(view->server, view->x, view->y);
        struct wlr_box box = {area.x - cx, area.y - cy, area.width, area.height};
        wlr_xdg_popup_unconstrain_from_box(p->popup, &box);
    }
    wlr_xdg_surface_schedule_configure(p->popup->base);
}

static void handle_popup_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_popup *p = XD_CONTAINER_OF(listener, xd_popup, destroy);
    xd_unlisten(&p->commit);
    xd_unlisten(&p->destroy);
    xd_free(p);
}

static void handle_new_popup([[maybe_unused]] struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *popup = data;
    struct wlr_xdg_surface *parent =
        popup->parent != nullptr ? wlr_xdg_surface_try_from_wlr_surface(popup->parent) : nullptr;
    if (parent == nullptr || parent->data == nullptr)
        return; /* layer-shell parents come with the shell milestone */

    struct wlr_scene_tree *parent_tree = parent->data;
    popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);

    xd_popup *p = XD_NEW(xd_popup);
    p->popup = popup;
    XD_LISTEN(&popup->base->surface->events.commit, p, commit, handle_popup_commit);
    XD_LISTEN(&popup->events.destroy, p, destroy, handle_popup_destroy);
}

void xd_view_init(xd_server *server) {
    wl_list_init(&server->views);
    server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
    XD_LISTEN(&server->xdg_shell->events.new_toplevel, server, new_toplevel,
              handle_new_toplevel);
    XD_LISTEN(&server->xdg_shell->events.new_popup, server, new_popup, handle_new_popup);

    server->ftl_manager = wlr_foreign_toplevel_manager_v1_create(server->display);
    server->xdg_decoration = wlr_xdg_decoration_manager_v1_create(server->display);
    XD_LISTEN(&server->xdg_decoration->events.new_toplevel_decoration, server, new_decoration,
              handle_new_decoration);
}
