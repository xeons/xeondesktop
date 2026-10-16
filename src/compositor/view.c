/* SPDX-License-Identifier: MIT
 * Toplevel windows ("views"): lifecycle, focus, placement, maximize and
 * minimize, server-side decorations, and popups.
 */
#include <math.h>
#include <stdlib.h>

#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

/* ---- xdg-shell / X11 ------------------------------------------------------ */

struct wlr_surface *xd_view_surface(const xd_view *view) {
    return view->kind == XD_VIEW_X11 ? view->xsurface->surface : view->toplevel->base->surface;
}

const char *xd_view_title(const xd_view *view) {
    return view->kind == XD_VIEW_X11 ? view->xsurface->title : view->toplevel->title;
}

const char *xd_view_app_id(const xd_view *view) {
    return view->kind == XD_VIEW_X11 ? view->xsurface->class : view->toplevel->app_id;
}

static bool view_initialized(const xd_view *view) {
    return view->kind == XD_VIEW_X11 ? view->xsurface->surface != nullptr
                                     : view->toplevel->base->initialized;
}

static void view_schedule_configure(xd_view *view) {
    if (view->kind == XD_VIEW_XDG && view->toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
}

void xd_view_set_resizing(xd_view *view, bool resizing) {
    if (view->kind == XD_VIEW_XDG)
        wlr_xdg_toplevel_set_resizing(view->toplevel, resizing);
}

static void view_set_maximized_state(xd_view *view, bool on) {
    if (view->kind == XD_VIEW_X11)
        wlr_xwayland_surface_set_maximized(view->xsurface, on, on);
    else
        wlr_xdg_toplevel_set_maximized(view->toplevel, on);
}

static void view_set_fullscreen_state(xd_view *view, bool on) {
    if (view->kind == XD_VIEW_X11)
        wlr_xwayland_surface_set_fullscreen(view->xsurface, on);
    else
        wlr_xdg_toplevel_set_fullscreen(view->toplevel, on);
}

static bool view_wants_fullscreen(const xd_view *view) {
    return view->kind == XD_VIEW_X11 ? view->xsurface->fullscreen
                                     : view->toplevel->requested.fullscreen;
}

static bool view_is_fixed_size(const xd_view *view) {
    if (view->kind == XD_VIEW_X11) {
        const xcb_size_hints_t *h = view->xsurface->size_hints;
        return h != nullptr && (h->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) &&
               (h->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) && h->min_width > 0 &&
               h->min_width == h->max_width && h->min_height == h->max_height;
    }
    const struct wlr_xdg_toplevel_state *st = &view->toplevel->current;
    return st->min_width > 0 && st->min_height > 0 && st->min_width == st->max_width &&
           st->min_height == st->max_height;
}

xd_frame xd_view_frame(const xd_view *view, bool active) {
    const char *title = xd_view_title(view);
    return (xd_frame){
        .title = title != nullptr ? title : "",
        .active = active,
        .maximized = view->maximized,
        .dialog = view->dialog,
        .buttons = view->dialog ? XD_CAPBTN_CLOSE
                                : XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE,
        .hot = view->hot,
        .pressed = view->pressed,
    };
}

xd_insets xd_view_insets(const xd_view *view) {
    if (view->csd || view->fullscreen)
        return (xd_insets){0};
    xd_frame f = xd_view_frame(view, true);
    return xd_theme_frame_insets(&view->server->logical_theme, &f);
}

static struct wlr_box client_geometry(const xd_view *view) {
    if (view->kind == XD_VIEW_X11)
        return (struct wlr_box){0, 0, view->xsurface->width, view->xsurface->height};
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

void xd_view_retheme(xd_view *view) {
    if (!view->mapped)
        return;
    update_content_position(view);
    xd_frame_render(view, true);
}

void xd_view_set_position(xd_view *view, int x, int y) {
    view->x = x;
    view->y = y;
    wlr_scene_node_set_position(&view->tree->node, x, y);
    xd_view_x11_sync(view);
    update_ftl_output(view);
    /* The frame's pixel scale follows the output it's on. */
    xd_frame_render(view, false);
}

void xd_view_x11_sync(xd_view *view) {
    if (view->kind != XD_VIEW_X11 || view->xsurface->surface == nullptr)
        return;
    xd_insets in = xd_view_insets(view);
    wlr_xwayland_surface_configure(view->xsurface, (int16_t)(view->x + in.left),
                                   (int16_t)(view->y + in.top), view->xsurface->width,
                                   view->xsurface->height);
}

static void send_client_size(xd_view *view, int outer_w, int outer_h) {
    xd_insets in = xd_view_insets(view);
    int w = XD_MAX(outer_w - in.left - in.right, XD_MIN_CLIENT_W);
    int h = XD_MAX(outer_h - in.top - in.bottom, XD_MIN_CLIENT_H);
    if (view->kind == XD_VIEW_X11) {
        wlr_xwayland_surface_configure(view->xsurface, (int16_t)(view->x + in.left),
                                       (int16_t)(view->y + in.top), (uint16_t)w, (uint16_t)h);
        return;
    }
    wlr_xdg_toplevel_set_size(view->toplevel, w, h);
}

void xd_view_set_outer_box(xd_view *view, struct wlr_box outer) {
    view->anchor_edges = 0;
    xd_view_set_position(view, outer.x, outer.y);
    send_client_size(view, outer.width, outer.height);
}

void xd_view_resize_anchored(xd_view *view, struct wlr_box outer, uint32_t moving) {
    /* Moving the left or top edge: the right or bottom edge stays; the
     * position follows when the client commits its new size (see
     * handle_commit), so the window never shows at the new place with the
     * old size. */
    view->anchor_edges = moving & (WLR_EDGE_LEFT | WLR_EDGE_TOP);
    view->anchor_right = outer.x + outer.width;
    view->anchor_bottom = outer.y + outer.height;
    struct wlr_box cur = xd_view_outer_box(view);
    xd_view_set_position(view, (moving & WLR_EDGE_LEFT) ? cur.x : outer.x,
                         (moving & WLR_EDGE_TOP) ? cur.y : outer.y);
    send_client_size(view, outer.width, outer.height);
}

static void apply_anchor(xd_view *view) {
    if (view->anchor_edges == 0)
        return;
    struct wlr_box b = xd_view_outer_box(view);
    int x = (view->anchor_edges & WLR_EDGE_LEFT) ? view->anchor_right - b.width : b.x;
    int y = (view->anchor_edges & WLR_EDGE_TOP) ? view->anchor_bottom - b.height : b.y;
    if (x != b.x || y != b.y)
        xd_view_set_position(view, x, y);
}

static int floor_int(double v) {
    double f = floor(v);
    XD_ASSERT(f > -32768.0 && f < 32768.0);
    return (int)f;
}

void xd_view_menu_anchor(const xd_view *view, double *lx, double *ly) {
    struct wlr_box outer = xd_view_outer_box(view);
    *lx = outer.x;
    *ly = outer.y;
    if (view->csd)
        return;
    xd_frame f = xd_view_frame(view, true);
    xd_frame_layout l;
    xd_theme_frame_layout(&view->server->logical_theme,
                          (xd_rect){0, 0, outer.width, outer.height}, &f, &l);
    *lx = outer.x + l.icon.x;
    *ly = outer.y + l.caption.y + l.caption.h;
}

xd_frame_part xd_view_hit(const xd_view *view, double lx, double ly) {
    if (view->csd || view->fullscreen)
        return XD_PART_CLIENT;
    struct wlr_box outer = xd_view_outer_box(view);
    double scale = view->drawn.scale > 0 ? view->drawn.scale : 1.0;
    xd_theme theme;
    xd_theme_init(&theme, view->server->config.style, &view->server->config.scheme, scale);
    xd_frame f = xd_view_frame(view, true);
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
    /* A fullscreen window covers the taskbar only while it's in front. */
    if (view->fullscreen)
        wlr_scene_node_reparent(&view->tree->node, activated ? view->server->layer_fullscreen
                                                             : view->server->layer_views);
    if (view->kind == XD_VIEW_X11) {
        wlr_xwayland_surface_activate(view->xsurface, activated);
        if (activated)
            wlr_xwayland_surface_restack(view->xsurface, nullptr, XCB_STACK_MODE_ABOVE);
    } else {
        wlr_xdg_toplevel_set_activated(view->toplevel, activated);
    }
    if (view->ftl != nullptr)
        wlr_foreign_toplevel_handle_v1_set_activated(view->ftl, activated);
    xd_frame_render(view, false);
}

/* The view of a toplevel (its owner, for dialogs). */
static xd_view *view_of(struct wlr_xdg_toplevel *t) {
    struct wlr_scene_tree *content = t != nullptr ? t->base->data : nullptr;
    return content != nullptr ? xd_view_from_node(&content->node) : nullptr;
}

static xd_view *parent_of(const xd_view *view) {
    if (view->kind == XD_VIEW_X11)
        return view->xsurface->parent != nullptr ? view->xsurface->parent->data : nullptr;
    return view_of(view->toplevel->parent);
}

/* Owned windows (dialogs) stay above their owner: raise the owner's whole
 * family, the owner first. */
static void raise_family(xd_view *view) {
    xd_view *root = view;
    for (int depth = 0; depth < 16 && parent_of(root) != nullptr; depth++)
        root = parent_of(root);
    wlr_scene_node_raise_to_top(&root->tree->node);
    /* Then each owned window, recursively, in focus order (oldest last). */
    xd_view *stack[64];
    size_t n = 0;
    stack[n++] = root;
    for (size_t i = 0; i < n; i++) {
        XD_LIST_FOR_EACH(v, &root->server->views, xd_view, link) {
            if (v->mapped && parent_of(v) == stack[i] && n < XD_ARRAY_LEN(stack)) {
                wlr_scene_node_raise_to_top(&v->tree->node);
                stack[n++] = v;
            }
        }
    }
    /* The focused window itself ends up on top of its siblings. */
    wlr_scene_node_raise_to_top(&view->tree->node);
}

void xd_view_focus(xd_view *view) {
    xd_server *server = view->server;
    xd_view *prev = xd_view_focused(server);

    if (view->minimized) {
        view->minimized = false;
        wlr_scene_node_set_enabled(&view->tree->node, true);
        if (view->ftl != nullptr)
            wlr_foreign_toplevel_handle_v1_set_minimized(view->ftl, false);
    }

    /* Restoring an owner restores the dialogs it owns. */
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (v->minimized && parent_of(v) == view) {
            v->minimized = false;
            wlr_scene_node_set_enabled(&v->tree->node, true);
        }
    }
    /* Deactivate first: a fullscreen window drops back among the others,
     * and must end up under the one being raised. */
    if (prev != nullptr && prev != view)
        set_activated(prev, false);
    raise_family(view);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    set_activated(view, true);

    /* An exclusive layer surface (open Start menu) keeps the keyboard;
     * the window gets it when that ends. */
    xd_keyboard_refocus(server);
}

/* Focuses the most recently used visible window, or clears focus. */
void xd_view_focus_next(xd_server *server) {
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (view->mapped && !view->minimized) {
            xd_view_focus(view);
            return;
        }
    }
    xd_keyboard_refocus(server);
}

void xd_view_close(xd_view *view) {
    if (view->kind == XD_VIEW_X11)
        wlr_xwayland_surface_close(view->xsurface);
    else
        wlr_xdg_toplevel_send_close(view->toplevel);
}

void xd_view_set_maximized(xd_view *view, bool maximized) {
    if (!view_initialized(view))
        return;
    if (view->dialog)
        maximized = false; /* dialogs keep their size */
    if (view->maximized == maximized) {
        view_schedule_configure(view);
        return;
    }
    if (maximized) {
        struct wlr_box outer = xd_view_outer_box(view);
        view->restore = outer;
        struct wlr_box area = xd_output_work_area_at(view->server, outer.x + outer.width / 2.0,
                                                     outer.y + outer.height / 2.0);
        view->maximized = true;
        view_set_maximized_state(view, true);
        if (view->ftl != nullptr)
            wlr_foreign_toplevel_handle_v1_set_maximized(view->ftl, true);
        xd_view_set_outer_box(view, area);
    } else {
        view->maximized = false;
        view_set_maximized_state(view, false);
        if (view->ftl != nullptr)
            wlr_foreign_toplevel_handle_v1_set_maximized(view->ftl, false);
        xd_view_set_outer_box(view, view->restore);
    }
}

void xd_view_set_fullscreen(xd_view *view, bool fullscreen) {
    if (!view_initialized(view))
        return;
    if (view->fullscreen == fullscreen) {
        view_schedule_configure(view);
        return;
    }
    xd_server *server = view->server;
    if (fullscreen) {
        struct wlr_box outer = xd_view_outer_box(view);
        view->fullscreen_restore = outer;
        xd_output *out = xd_output_at(server, outer.x + outer.width / 2.0,
                                      outer.y + outer.height / 2.0);
        if (out == nullptr)
            return;
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
        view->fullscreen = true;
        view_set_fullscreen_state(view, true);
        if (view->ftl != nullptr)
            wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ftl, true);
        update_content_position(view);
        xd_frame_render(view, true);
        xd_view_set_outer_box(view, box); /* the whole output, no frame */
        if (xd_view_focused(server) == view)
            wlr_scene_node_reparent(&view->tree->node, server->layer_fullscreen);
    } else {
        view->fullscreen = false;
        view_set_fullscreen_state(view, false);
        if (view->ftl != nullptr)
            wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ftl, false);
        wlr_scene_node_reparent(&view->tree->node, server->layer_views);
        update_content_position(view);
        xd_frame_render(view, true);
        xd_view_set_outer_box(view, view->fullscreen_restore);
    }
}

void xd_view_minimize(xd_view *view) {
    if (view->minimized || !view->mapped)
        return;
    /* A dialog's owner goes with it, and the owner takes its dialogs. */
    xd_view *owner = parent_of(view);
    if (owner != nullptr && !owner->minimized) {
        xd_view_minimize(owner);
        return;
    }
    XD_LIST_FOR_EACH(v, &view->server->views, xd_view, link) {
        if (parent_of(v) == view && v->mapped && !v->minimized) {
            v->minimized = true;
            wlr_scene_node_set_enabled(&v->tree->node, false);
        }
    }
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
    /* One caption lower each time; client-decorated windows have no frame
     * of ours, so use a standard caption's height. */
    int step = in.top > 0 ? in.top : xd_px(1.0, 26);

    /* Owned windows (dialogs) open centred over their owner. */
    xd_view *owner = parent_of(view);
    if (owner != nullptr && owner->mapped) {
        struct wlr_box o = xd_view_outer_box(owner);
        int x = o.x + (o.width - outer.width) / 2;
        int y = o.y + (o.height - outer.height) / 2;
        x = XD_CLAMP(x, area.x, XD_MAX(area.x + area.width - outer.width, area.x));
        y = XD_CLAMP(y, area.y, XD_MAX(area.y + area.height - outer.height, area.y));
        xd_view_set_position(view, x, y);
        return;
    }

    /* Dialogs open centred, as Windows places them. */
    if (view->dialog && outer.width <= area.width && outer.height <= area.height) {
        xd_view_set_position(view, area.x + (area.width - outer.width) / 2,
                             area.y + (area.height - outer.height) / 2);
        return;
    }

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
    if (xd_view_title(view) != nullptr)
        wlr_foreign_toplevel_handle_v1_set_title(view->ftl, xd_view_title(view));
    if (xd_view_app_id(view) != nullptr)
        wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, xd_view_app_id(view));
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

static int fit_span(int pos, int len, int lo, int span) {
    if (len >= span)
        return lo;
    return XD_MAX(lo, XD_MIN(pos, lo + span - len));
}

void xd_view_rescue_offscreen(xd_server *server) {
    struct wlr_output_layout *layout = server->output_layout;
    if (wl_list_empty(&layout->outputs))
        return;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (!view->mapped)
            continue;
        struct wlr_box b = xd_view_outer_box(view);
        double cx = b.x + b.width / 2.0;
        double cy = b.y + b.height / 2.0;
        if (wlr_output_layout_output_at(layout, cx, cy) != nullptr)
            continue;
        double nx;
        double ny;
        wlr_output_layout_closest_point(layout, nullptr, cx, cy, &nx, &ny);
        struct wlr_output *o = wlr_output_layout_output_at(layout, nx, ny);
        if (o == nullptr)
            continue;
        if (view->fullscreen) {
            struct wlr_box ob;
            wlr_output_layout_get_box(layout, o, &ob);
            view->fullscreen_restore.x += ob.x - b.x;
            view->fullscreen_restore.y += ob.y - b.y;
            xd_view_set_outer_box(view, ob);
            continue;
        }
        struct wlr_box area = xd_output_work_area_at(server, nx, ny);
        int x = fit_span((int)nx - b.width / 2, b.width, area.x, area.width);
        int y = fit_span((int)ny - b.height / 2, b.height, area.y, area.height);
        /* Where it un-maximizes to moves along with it. */
        view->restore.x += x - b.x;
        view->restore.y += y - b.y;
        xd_view_set_position(view, x, y);
    }
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


void xd_view_on_map(xd_view *view) {
    view->mapped = true;
    view->dialog = view_is_fixed_size(view);
    xd_view_update_decorations(view);
    /* Owned windows (dialogs) get no taskbar button, as in Windows. */
    if (parent_of(view) == nullptr)
        ftl_create(view);
    update_content_position(view);
    place_new_view(view);
    xd_view_focus(view);
    xd_frame_render(view, true);
    if (view_wants_fullscreen(view))
        xd_view_set_fullscreen(view, true);
}

static void handle_map(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_on_map(XD_CONTAINER_OF(listener, xd_view, map));
}

void xd_view_on_unmap(xd_view *view) {
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

static void handle_unmap(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_on_unmap(XD_CONTAINER_OF(listener, xd_view, unmap));
}

static void handle_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, commit);
    struct wlr_xdg_surface *xdg = view->toplevel->base;

    if (xdg->initial_commit) {
        /* Let the client pick its size, and settle who draws the frame. */
        wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
        xd_view_update_decorations(view);
        return;
    }
    xd_view_on_commit(view);
}

void xd_view_on_commit(xd_view *view) {
    if (!view->mapped)
        return;
    /* Clients that don't negotiate decorations may still draw their own
     * (shadow margins around the window geometry give them away). */
    xd_view_update_decorations(view);
    apply_anchor(view);
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
    xd_view_set_resizing(view, true);
}

static void handle_request_maximize(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_maximize);
    xd_view_set_maximized(view, view->toplevel->requested.maximized);
}

/* A client-decorated window (e.g. a GTK header bar) asks for the window
 * menu; x, y are relative to its window geometry. */
static void handle_request_window_menu(struct wl_listener *listener, void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_window_menu);
    const struct wlr_xdg_toplevel_show_window_menu_event *e = data;
    if (!view->mapped)
        return;
    struct wlr_box outer = xd_view_outer_box(view);
    xd_insets in = xd_view_insets(view);
    xd_window_menu_show(view, outer.x + in.left + e->x, outer.y + in.top + e->y);
}

static void handle_request_minimize(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_minimize);
    xd_view_minimize(view);
}

static void handle_request_fullscreen(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, request_fullscreen);
    if (!view->mapped) {
        /* Before mapping, just answer; it's applied once the window shows. */
        view_schedule_configure(view);
        return;
    }
    xd_view_set_fullscreen(view, view->toplevel->requested.fullscreen);
}

/* Gaining or losing an owner adds or removes the taskbar button. */
static void handle_set_parent(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, set_parent);
    if (!view->mapped)
        return;
    if (parent_of(view) != nullptr)
        ftl_destroy(view);
    else if (view->ftl == nullptr)
        ftl_create(view);
}

static void handle_set_title(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, set_title);
    if (view->ftl != nullptr && xd_view_title(view) != nullptr)
        wlr_foreign_toplevel_handle_v1_set_title(view->ftl, xd_view_title(view));
    view->drawn.title_dirty = true;
    xd_frame_render(view, false);
}

static void handle_set_app_id(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, set_app_id);
    if (view->ftl != nullptr && xd_view_app_id(view) != nullptr)
        wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, xd_view_app_id(view));
}

static void handle_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view_on_destroy(XD_CONTAINER_OF(listener, xd_view, destroy));
}

void xd_view_on_destroy(xd_view *view) {
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
    xd_unlisten(&view->request_window_menu);
    xd_unlisten(&view->request_fullscreen);
    xd_unlisten(&view->set_parent);
    xd_unlisten(&view->associate);
    xd_unlisten(&view->dissociate);
    xd_unlisten(&view->request_configure);
    xd_unlisten(&view->request_activate);
    xd_unlisten(&view->request_close);
    xd_unlisten(&view->set_decorations);
    xd_unlisten(&view->set_hints);
    xd_unlisten(&view->content_destroy);
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
    XD_LISTEN(&toplevel->events.request_show_window_menu, view, request_window_menu,
              handle_request_window_menu);
    XD_LISTEN(&toplevel->events.request_fullscreen, view, request_fullscreen,
              handle_request_fullscreen);
    XD_LISTEN(&toplevel->events.set_parent, view, set_parent, handle_set_parent);
    XD_LISTEN(&toplevel->events.set_title, view, set_title, handle_set_title);
    XD_LISTEN(&toplevel->events.set_app_id, view, set_app_id, handle_set_app_id);
}

/* ---- decorations --------------------------------------------------------- */

/* Windows get our themed frame unless the client asks to draw its own
 * (xdg-decoration client-side, or the KDE protocol's client mode, which
 * GTK 3 apps such as Firefox use for title-bar tabs). Drawing ours around
 * a client-decorated window doubles the title bar and puts two sets of
 * buttons and resize edges on one window. Clients that don't negotiate at
 * all get our frame. */
static bool kde_wants_client_side(xd_server *server, struct wlr_surface *surface) {
    if (server->kde_decoration == nullptr)
        return false;
    struct wlr_server_decoration *d;
    wl_list_for_each(d, &server->kde_decoration->decorations, link) {
        if (d->surface == surface)
            return d->mode == WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
    }
    return false;
}

static bool kde_negotiated(xd_server *server, struct wlr_surface *surface) {
    if (server->kde_decoration == nullptr)
        return false;
    struct wlr_server_decoration *d;
    wl_list_for_each(d, &server->kde_decoration->decorations, link) {
        if (d->surface == surface)
            return true;
    }
    return false;
}

void xd_view_update_decorations(xd_view *view) {
    xd_server *server = view->server;
    struct wlr_surface *surface = xd_view_surface(view);
    bool csd;
    if (view->kind == XD_VIEW_X11) {
        /* Motif hints: no title bar or border means the app draws its own. */
        csd = view->xsurface->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    } else if (view->decoration != nullptr) {
        csd = view->decoration->requested_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    } else if (kde_negotiated(server, surface)) {
        csd = kde_wants_client_side(server, surface);
    } else {
        /* No negotiation (GTK 4, libadwaita): a window geometry inset from
         * the surface means the client draws shadows, and so its own
         * frame. */
        struct wlr_box geo = view->toplevel->base->geometry;
        csd = geo.x > 0 || geo.y > 0;
    }
    /* [Windows] AlwaysDrawFrames: our frame on everything. */
    if (server->config.always_frames)
        csd = false;
    if (view->decoration != nullptr && view_initialized(view))
        wlr_xdg_toplevel_decoration_v1_set_mode(
            view->decoration, csd ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                                  : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    if (csd == view->csd)
        return;
    view->csd = csd;
    if (view->mapped) {
        update_content_position(view);
        xd_frame_render(view, true);
    }
}

static void handle_decoration_mode(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_view *view = XD_CONTAINER_OF(listener, xd_view, decoration_mode);
    xd_view_update_decorations(view);
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

/* A KDE decoration object: re-check its window whenever the mode changes. */
typedef struct kde_deco {
    xd_server *server;
    struct wlr_server_decoration *deco;
    struct wl_listener mode;
    struct wl_listener destroy;
} kde_deco;

static void kde_deco_refresh(kde_deco *k) {
    struct wlr_xdg_toplevel *top = wlr_xdg_toplevel_try_from_wlr_surface(k->deco->surface);
    struct wlr_scene_tree *content = top != nullptr ? top->base->data : nullptr;
    xd_view *view = content != nullptr ? xd_view_from_node(&content->node) : nullptr;
    if (view != nullptr)
        xd_view_update_decorations(view);
}

static void handle_kde_mode(struct wl_listener *listener, [[maybe_unused]] void *data) {
    kde_deco_refresh(XD_CONTAINER_OF(listener, kde_deco, mode));
}

static void handle_kde_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    kde_deco *k = XD_CONTAINER_OF(listener, kde_deco, destroy);
    xd_unlisten(&k->mode);
    xd_unlisten(&k->destroy);
    xd_free(k);
}

static void handle_new_kde_decoration(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_kde_decoration);
    kde_deco *k = XD_NEW(kde_deco);
    k->server = server;
    k->deco = data;
    XD_LISTEN(&k->deco->events.mode, k, mode, handle_kde_mode);
    XD_LISTEN(&k->deco->events.destroy, k, destroy, handle_kde_destroy);
    kde_deco_refresh(k);
}

/* ---- popups --------------------------------------------------------------- */

typedef struct xd_popup {
    struct wlr_xdg_popup *popup;
    struct wl_listener commit;
    struct wl_listener destroy;
} xd_popup;

/* Layout position of the surface a popup chain is rooted at: a window's
 * content or a layer surface (the taskbar's Start menu). */
static bool popup_root_origin(struct wlr_scene_node *node, xd_server **server, int *x,
                              int *y) {
    xd_view *view = xd_view_from_node(node);
    if (view != nullptr) {
        *server = view->server;
        return wlr_scene_node_coords(&view->content->node, x, y);
    }
    xd_layer_surface *ls = xd_layer_from_node(node);
    if (ls != nullptr) {
        *server = ls->server;
        return wlr_scene_node_coords(&ls->scene->tree->node, x, y);
    }
    return false;
}

static void handle_popup_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_popup *p = XD_CONTAINER_OF(listener, xd_popup, commit);
    if (!p->popup->base->initial_commit)
        return;

    /* Keep popups (menus, submenus) inside the work area of the output
     * their root surface is on, flipping or sliding as the client allows. */
    struct wlr_scene_tree *tree = p->popup->base->data;
    xd_server *server = nullptr;
    int rx = 0;
    int ry = 0;
    if (tree != nullptr && popup_root_origin(&tree->node, &server, &rx, &ry)) {
        struct wlr_box area = xd_output_work_area_at(server, rx + 1, ry + 1);
        struct wlr_box box = {area.x - rx, area.y - ry, area.width, area.height};
        wlr_xdg_popup_unconstrain_from_box(p->popup, &box);
        wlr_log(WLR_DEBUG, "popup: root (%d,%d) box (%d,%d %dx%d) -> geometry (%d,%d %dx%d)", rx, ry, box.x, box.y, box.width, box.height, p->popup->scheduled.geometry.x, p->popup->scheduled.geometry.y, p->popup->scheduled.geometry.width, p->popup->scheduled.geometry.height);
    }
    wlr_xdg_surface_schedule_configure(p->popup->base);
}

static void handle_popup_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_popup *p = XD_CONTAINER_OF(listener, xd_popup, destroy);
    xd_unlisten(&p->commit);
    xd_unlisten(&p->destroy);
    xd_free(p);
}

void xd_popup_attach(struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent_tree) {
    popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);

    xd_popup *p = XD_NEW(xd_popup);
    p->popup = popup;
    XD_LISTEN(&popup->base->surface->events.commit, p, commit, handle_popup_commit);
    XD_LISTEN(&popup->events.destroy, p, destroy, handle_popup_destroy);
}

static void handle_new_popup([[maybe_unused]] struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *popup = data;
    /* Layer-shell popups (the Start menu) are created without a parent and
     * arrive again through the layer surface's new_popup; see layers.c. */
    if (popup->parent == nullptr)
        return;
    struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (xdg != nullptr && xdg->data != nullptr)
        xd_popup_attach(popup, xdg->data);
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

    /* GTK 3 asks for (or declines) server-side decorations with the KDE
     * protocol. */
    server->kde_decoration = wlr_server_decoration_manager_create(server->display);
    wlr_server_decoration_manager_set_default_mode(server->kde_decoration,
                                                   WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    XD_LISTEN(&server->kde_decoration->events.new_decoration, server, new_kde_decoration,
              handle_new_kde_decoration);
}
