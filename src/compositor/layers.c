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

static void handle_map(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_layer_surface *ls = XD_CONTAINER_OF(listener, xd_layer_surface, map);
    xd_output_arrange(ls->output);
}

static void handle_unmap(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_layer_surface *ls = XD_CONTAINER_OF(listener, xd_layer_surface, unmap);
    /* Gives its exclusive zone back to the windows. */
    xd_output_arrange(ls->output);
}

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
    if (l->initial_commit || l->current.committed != 0)
        xd_output_arrange(ls->output);
}

static void handle_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_layer_surface *ls = XD_CONTAINER_OF(listener, xd_layer_surface, destroy);
    xd_unlisten(&ls->map);
    xd_unlisten(&ls->unmap);
    xd_unlisten(&ls->commit);
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
    out->usable = usable;
    if (changed)
        xd_view_refit_maximized(server, out);
}

void xd_layers_init(xd_server *server) {
    server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
    XD_LISTEN(&server->layer_shell->events.new_surface, server, new_layer_surface,
              handle_new_layer_surface);
}
