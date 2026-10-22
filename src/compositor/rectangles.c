/* SPDX-License-Identifier: MIT
 * xd-window-rectangles-v1: where the windows are, front to back, for the
 * Snipping Tool's Window Snip and Alt+Print Screen. Any program that may
 * capture the screen may ask (trust.c's privileged globals).
 */
#include <wlr/types/wlr_scene.h>

#include "server.h"
#include "xd-window-rectangles-v1-protocol.h"

static void send_layer(struct wl_resource *resource, struct wlr_scene_tree *layer,
                       const xd_view *focused) {
    struct wlr_scene_node *node;
    /* A tree's children are back to front. */
    wl_list_for_each_reverse(node, &layer->children, link) {
        if (!node->enabled)
            continue;
        xd_view *view = xd_view_from_node(node);
        if (view == nullptr || !view->mapped || view->minimized || &view->tree->node != node)
            continue;
        struct wlr_box box = xd_view_outer_box(view);
        if (box.width <= 0 || box.height <= 0)
            continue;
        uint32_t flags = view == focused ? XD_WINDOW_RECTANGLES_V1_FLAGS_ACTIVATED : 0;
        xd_window_rectangles_v1_send_window(resource, box.x, box.y, box.width, box.height, flags);
    }
}

static void handle_list_destroy([[maybe_unused]] struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xd_window_rectangles_v1_interface list_impl = {
    .destroy = handle_list_destroy,
};

static void handle_get_rectangles(struct wl_client *client, struct wl_resource *manager,
                                  uint32_t id) {
    xd_server *server = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(
        client, &xd_window_rectangles_v1_interface, wl_resource_get_version(manager), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &list_impl, nullptr, nullptr);
    xd_view *focused = xd_view_focused(server);
    /* The trees holding windows, frontmost first (main.c's scene order). */
    send_layer(resource, server->layer_fullscreen, focused);
    send_layer(resource, server->layer_on_top, focused);
    send_layer(resource, server->layer_views, focused);
    xd_window_rectangles_v1_send_done(resource);
}

static void handle_manager_destroy([[maybe_unused]] struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xd_window_rectangles_manager_v1_interface manager_impl = {
    .get_rectangles = handle_get_rectangles,
    .destroy = handle_manager_destroy,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource =
        wl_resource_create(client, &xd_window_rectangles_manager_v1_interface, (int)version, id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

void xd_rectangles_init(xd_server *server) {
    struct wl_global *global = wl_global_create(
        server->display, &xd_window_rectangles_manager_v1_interface, 1, server, bind_manager);
    xd_trust_privileged(server, global);
}
