/* SPDX-License-Identifier: MIT
 * wlr-layer-shell: the taskbar, desktop and other shell surfaces that are
 * anchored to an output instead of being managed as windows.
 */
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

static struct wlr_scene_tree *tree_for_layer(xd_server *server,
                                             enum zwlr_layer_shell_v1_layer layer) {
    XD_ASSERT((unsigned)layer < XD_ARRAY_LEN(server->layers));
    return server->layers[layer];
}

xd_layer_surface *xd_layer_from_node(struct wlr_scene_node *node) {
    for (; node != nullptr; node = node->parent != nullptr ? &node->parent->node : nullptr) {
        xd_node_tag *tag = node->data;
        if (tag != nullptr && tag->kind == XD_NODE_LAYER)
            return XD_CONTAINER_OF(tag, xd_layer_surface, tag);
    }
    return nullptr;
}

void xd_keyboard_refocus(xd_server *server) {
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    struct wlr_surface *target = nullptr;
    if (server->locked) {
        /* Only the screen locker. */
        target = server->lock_focus;
    } else if (server->layer_focus != nullptr) {
        target = server->layer_focus->layer_surface->surface;
    } else if (server->desktop_focus != nullptr) {
        target = server->desktop_focus->layer_surface->surface;
    } else {
        xd_view *view = xd_view_focused(server);
        if (view != nullptr)
            target = xd_view_surface(view);
    }
    if (target == nullptr) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        return;
    }
    if (kb != nullptr)
        wlr_seat_keyboard_notify_enter(server->seat, target, kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
    else
        wlr_seat_keyboard_notify_enter(server->seat, target, nullptr, 0, nullptr);
}

void xd_layer_click_focus(xd_layer_surface *ls) {
    xd_server *server = ls->server;
    struct wlr_layer_surface_v1 *l = ls->layer_surface;
    if (l->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND ||
        server->desktop_focus == ls)
        return;
    xd_view_deactivate_front(server);
    server->desktop_focus = ls;
    xd_keyboard_refocus(server);
}

/* Exclusive keyboard interactivity takes focus while mapped. */
static void update_keyboard_focus(xd_layer_surface *ls) {
    xd_server *server = ls->server;
    struct wlr_layer_surface_v1 *l = ls->layer_surface;
    bool wants = l->surface->mapped &&
                 l->current.keyboard_interactive ==
                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
                 ls->layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    if (wants && server->layer_focus != ls) {
        server->layer_focus = ls;
        xd_keyboard_refocus(server);
    } else if (!wants && server->layer_focus == ls) {
        server->layer_focus = nullptr;
        xd_keyboard_refocus(server);
    }
}

static void handle_map(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_layer_surface *ls = XD_CONTAINER_OF(listener, xd_layer_surface, map);
    xd_output_arrange(ls->output);
    update_keyboard_focus(ls);
}

static void handle_unmap(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_layer_surface *ls = XD_CONTAINER_OF(listener, xd_layer_surface, unmap);
    if (ls->server->desktop_focus == ls) {
        ls->server->desktop_focus = nullptr;
        xd_view_focus_next(ls->server);
    }
    /* Gives its exclusive zone back to the windows. */
    xd_output_arrange(ls->output);
    update_keyboard_focus(ls);
}

#ifdef XD_SCENEFX
static void first_buffer(struct wlr_scene_buffer *buffer, [[maybe_unused]] int sx,
                         [[maybe_unused]] int sy, void *data) {
    struct wlr_scene_buffer **out = data;
    if (*out == nullptr)
        *out = buffer;
}

/* Aero: the shell's taskbar is glass too; what's behind it is blurred
 * where it draws. */
static void update_glass(xd_layer_surface *ls) {
    struct wlr_layer_surface_v1 *l = ls->layer_surface;
    bool want = ls->server->config.style == XD_STYLE_AERO && l->namespace != nullptr &&
                strcmp(l->namespace, "taskbar") == 0 && l->surface->mapped;
    if (!want) {
        if (ls->blur != nullptr)
            wlr_scene_node_set_enabled(&ls->blur->node, false);
        return;
    }
    if (ls->blur == nullptr) {
        struct wlr_scene_buffer *surface = nullptr;
        wlr_scene_node_for_each_buffer(&ls->scene->tree->node, first_buffer, &surface);
        if (surface == nullptr)
            return;
        ls->blur = wlr_scene_blur_create(ls->scene->tree, 1, 1);
        wlr_scene_blur_set_transparency_mask_source(ls->blur, surface);
        wlr_scene_node_lower_to_bottom(&ls->blur->node);
    }
    wlr_scene_blur_set_size(ls->blur, (int)l->current.actual_width,
                            (int)l->current.actual_height);
    wlr_scene_node_set_enabled(&ls->blur->node, true);
}
#endif

static void handle_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_layer_surface *ls = XD_CONTAINER_OF(listener, xd_layer_surface, commit);
    struct wlr_layer_surface_v1 *l = ls->layer_surface;
    if (!l->initialized)
        return;

    if (l->current.layer != ls->layer) {
        ls->layer = l->current.layer;
        wlr_scene_node_reparent(&ls->scene->tree->node, tree_for_layer(ls->server, ls->layer));
    }
    /* The initial commit needs a configure; later ones only if the client
     * changed its size, anchors, margins or exclusive zone. */
    if (l->initial_commit || l->current.committed != 0) {
        xd_output_arrange(ls->output);
        update_keyboard_focus(ls);
    }
#ifdef XD_SCENEFX
    update_glass(ls);
#endif
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
    xd_layer_surface *ls = XD_CONTAINER_OF(listener, xd_layer_surface, new_popup);
    /* The shell's popups (the Start menus) may be glass. */
    xd_popup_attach(data, ls->scene->tree, ls->server);
}

static void handle_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_layer_surface *ls = XD_CONTAINER_OF(listener, xd_layer_surface, destroy);
    if (ls->server->layer_focus == ls) {
        ls->server->layer_focus = nullptr;
        xd_keyboard_refocus(ls->server);
    }
    if (ls->server->desktop_focus == ls) {
        ls->server->desktop_focus = nullptr;
        xd_view_focus_next(ls->server);
    }
    xd_unlisten(&ls->map);
    xd_unlisten(&ls->unmap);
    xd_unlisten(&ls->commit);
    xd_unlisten(&ls->new_popup);
    xd_unlisten(&ls->destroy);
    wl_list_remove(&ls->link);
    xd_output *out = ls->output;
    xd_free(ls);
    /* The scene node goes away with the layer surface. */
    xd_output_arrange(out);
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_layer_surface);
    struct wlr_layer_surface_v1 *l = data;

    xd_output *out = l->output != nullptr
                         ? xd_output_from_wlr(server, l->output)
                         : xd_output_at(server, server->cursor->x, server->cursor->y);
    if (out == nullptr) {
        wlr_log(WLR_INFO, "no output for layer surface '%s'", l->namespace);
        wlr_layer_surface_v1_destroy(l);
        return;
    }
    l->output = out->wlr_output;

    xd_layer_surface *ls = XD_NEW(xd_layer_surface);
    ls->tag.kind = XD_NODE_LAYER;
    ls->server = server;
    ls->output = out;
    ls->layer_surface = l;
    ls->layer = l->pending.layer;
    ls->scene = wlr_scene_layer_surface_v1_create(tree_for_layer(server, ls->layer), l);
    ls->scene->tree->node.data = &ls->tag;
    /* Popups (the Start menu's submenus, later) attach to this tree. */
    l->data = ls->scene->tree;
    wl_list_insert(out->layer_surfaces.prev, &ls->link);

    XD_LISTEN(&l->surface->events.map, ls, map, handle_map);
    XD_LISTEN(&l->surface->events.unmap, ls, unmap, handle_unmap);
    XD_LISTEN(&l->surface->events.commit, ls, commit, handle_commit);
    XD_LISTEN(&l->events.new_popup, ls, new_popup, handle_new_popup);
    XD_LISTEN(&l->events.destroy, ls, destroy, handle_destroy);
}

void xd_output_arrange(xd_output *out) {
    xd_server *server = out->server;
    struct wlr_box full;
    wlr_output_layout_get_box(server->output_layout, out->wlr_output, &full);
    if (wlr_box_empty(&full))
        return;

    /* Surfaces with an exclusive zone go first so the rest are placed in
     * what remains, as layer-shell specifies. Overlay is arranged first so
     * it wins ties, then top, bottom, background. */
    struct wlr_box usable = full;
    for (int pass = 0; pass < 2; pass++) {
        for (int layer = 3; layer >= 0; layer--) {
            XD_LIST_FOR_EACH(ls, &out->layer_surfaces, xd_layer_surface, link) {
                struct wlr_layer_surface_v1 *l = ls->layer_surface;
                /* Unmapped surfaces don't reserve space, except on their
                 * initial commit, which needs a configure to map at all. */
                if (!l->initialized || (!l->surface->mapped && !l->initial_commit) ||
                    (int)ls->layer != layer)
                    continue;
                bool exclusive = l->current.exclusive_zone > 0;
                if (exclusive != (pass == 0))
                    continue;
                wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
            }
        }
    }

    bool changed = !wlr_box_equal(&usable, &out->usable);
    wlr_log(WLR_DEBUG, "arrange %s: full (%d,%d %dx%d) usable (%d,%d %dx%d)",
            out->wlr_output->name, full.x, full.y, full.width, full.height, usable.x, usable.y,
            usable.width, usable.height);
    out->usable = usable;
    if (changed)
        xd_view_refit_maximized(server, out);
}

void xd_layers_init(xd_server *server) {
    server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
    XD_LISTEN(&server->layer_shell->events.new_surface, server, new_layer_surface,
              handle_new_layer_surface);
}
