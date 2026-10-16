/* SPDX-License-Identifier: MIT
 * Cascade and tile windows for the taskbar menu, and the window menu's
 * keyboard Move and Size (xd-window-arranger-v1, a private protocol only
 * the shell we started can see).
 */
#include <math.h>
#include <sys/socket.h>

#include "../core/alloc.h"
#include "server.h"
#include "xd-window-arranger-v1-protocol.h"

static constexpr int CASCADE_STEP = 26; /* logical px: about a caption */

/* Windows that take part: mapped, not minimized. */
static bool arrangeable(const xd_view *v) { return v->mapped && !v->minimized; }

static void remember(xd_view *v) {
    v->undo_valid = true;
    v->undo_maximized = v->maximized;
    v->undo_box = v->maximized ? v->restore : xd_view_outer_box(v);
}

static void place(xd_view *v, struct wlr_box box) {
    if (v->maximized)
        xd_view_set_maximized(v, false);
    if (v->dialog) {
        /* Fixed-size windows keep their size, centred in their cell. */
        struct wlr_box cur = xd_view_outer_box(v);
        int x = box.x + (box.width - cur.width) / 2;
        int y = box.y + (box.height - cur.height) / 2;
        xd_view_set_position(v, XD_MAX(x, box.x), XD_MAX(y, box.y));
        return;
    }
    xd_view_set_outer_box(v, box);
}

static void arrange_output(xd_server *server, struct wlr_box area, xd_view **views, size_t n,
                           uint32_t mode) {
    if (n == 0)
        return;
    if (mode == XD_WINDOW_ARRANGER_V1_MODE_CASCADE) {
        /* Back to front, each a caption lower and to the right, sized to
         * leave room for the rest. */
        int span = (int)(n - 1) * CASCADE_STEP;
        int w = XD_MAX(area.width - span, area.width * 2 / 3);
        int h = XD_MAX(area.height - span, area.height * 2 / 3);
        for (size_t i = 0; i < n; i++) {
            xd_view *v = views[n - 1 - i];
            int off = (int)(i * CASCADE_STEP) % XD_MAX(area.height / 2, CASCADE_STEP);
            place(v, (struct wlr_box){area.x + off, area.y + off, w, h});
            wlr_scene_node_raise_to_top(&v->tree->node);
        }
        (void)server;
        return;
    }
    /* Tiling: one row (or column) per window up to three, then a grid. */
    bool horizontal = mode == XD_WINDOW_ARRANGER_V1_MODE_TILE_HORIZONTALLY;
    double root = ceil(sqrt((double)n));
    int lines = n <= 3 ? 1 : (int)root; /* columns, or rows */
    int per = (int)((n + (size_t)lines - 1) / (size_t)lines);
    size_t i = 0;
    for (int l = 0; l < lines && i < n; l++) {
        int count = (int)XD_MIN((size_t)per, n - i);
        for (int k = 0; k < count; k++, i++) {
            struct wlr_box cell;
            if (horizontal) {
                /* Windows stacked one above another (wide bands). */
                int cw = area.width / lines, ch = area.height / count;
                cell = (struct wlr_box){area.x + l * cw, area.y + k * ch, cw, ch};
            } else {
                int cw = area.width / count, ch = area.height / lines;
                cell = (struct wlr_box){area.x + k * cw, area.y + l * ch, cw, ch};
            }
            place(views[i], cell);
        }
    }
}

static void handle_arrange(struct wl_client *client, struct wl_resource *res, uint32_t mode) {
    (void)client;
    xd_server *server = wl_resource_get_user_data(res);
    if (mode > XD_WINDOW_ARRANGER_V1_MODE_TILE_VERTICALLY)
        return;
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        v->undo_valid = false;
    }
    /* Group by output (by window centre), front to back. */
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        size_t cap = (size_t)wl_list_length(&server->views);
        xd_view **views = xd_alloc_array(XD_MAX(cap, 1), sizeof(*views));
        size_t n = 0;
        XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
            if (!arrangeable(v))
                continue;
            struct wlr_box b = xd_view_outer_box(v);
            if (xd_output_at(server, b.x + b.width / 2.0, b.y + b.height / 2.0) != out)
                continue;
            remember(v);
            views[n++] = v;
        }
        arrange_output(server, out->usable, views, n, mode);
        xd_free(views);
    }
}

static void handle_undo(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    xd_server *server = wl_resource_get_user_data(res);
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (!v->undo_valid)
            continue;
        v->undo_valid = false;
        xd_view_set_outer_box(v, v->undo_box);
        if (v->undo_maximized)
            xd_view_set_maximized(v, true);
    }
}

static void handle_begin_move(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    xd_view *v = xd_view_focused(wl_resource_get_user_data(res));
    if (v != nullptr)
        xd_view_begin_keyboard_move(v);
}

static void handle_begin_size(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    xd_view *v = xd_view_focused(wl_resource_get_user_data(res));
    if (v != nullptr)
        xd_view_begin_keyboard_size(v);
}

static void handle_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void resource_destroyed(struct wl_resource *res) {
    xd_server *server = wl_resource_get_user_data(res);
    if (server->arranger_resource == res)
        server->arranger_resource = nullptr;
}

/* The shell's wl_output for `out`, or nullptr. */
static struct wl_resource *shell_output(struct wl_resource *res, xd_output *out) {
    struct wl_client *client = wl_resource_get_client(res);
    struct wl_resource *r;
    wl_resource_for_each(r, &out->wlr_output->resources) {
        if (wl_resource_get_client(r) == client)
            return r;
    }
    return nullptr;
}

void xd_start_menu_toggle(xd_server *server) {
    struct wl_resource *res = server->arranger_resource;
    xd_output *out = xd_output_at(server, server->cursor->x, server->cursor->y);
    if (res == nullptr || out == nullptr)
        return;
    struct wl_resource *output_res = shell_output(res, out);
    if (output_res != nullptr)
        xd_window_arranger_v1_send_start_menu(res, output_res,
                                              wl_display_next_serial(server->display));
}

void xd_window_menu_show(xd_view *view, double lx, double ly) {
    xd_server *server = view->server;
    xd_view_focus(view);
    struct wl_resource *res = server->arranger_resource;
    xd_output *out = xd_output_at(server, lx, ly);
    if (res == nullptr || out == nullptr)
        return;
    /* The shell's wl_output for this output. */
    struct wl_client *client = wl_resource_get_client(res);
    struct wl_resource *output_res = nullptr;
    struct wl_resource *r;
    wl_resource_for_each(r, &out->wlr_output->resources) {
        if (wl_resource_get_client(r) == client) {
            output_res = r;
            break;
        }
    }
    if (output_res == nullptr)
        return;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
    xd_window_arranger_v1_send_window_menu(res, output_res, (int32_t)(lx - box.x),
                                           (int32_t)(ly - box.y),
                                           wl_display_next_serial(server->display));
}

void xd_view_attention(xd_view *view) {
    struct wl_resource *res = view->server->arranger_resource;
    if (res == nullptr || view->ftl == nullptr ||
        wl_resource_get_version(res) < XD_WINDOW_ARRANGER_V1_ATTENTION_SINCE_VERSION)
        return;
    /* The shell's handle for this window. */
    struct wl_client *client = wl_resource_get_client(res);
    struct wl_resource *r;
    wl_resource_for_each(r, &view->ftl->resources) {
        if (wl_resource_get_client(r) == client) {
            xd_window_arranger_v1_send_attention(res, r);
            return;
        }
    }
}

static const struct xd_window_arranger_v1_interface arranger_impl = {
    .arrange = handle_arrange,
    .undo = handle_undo,
    .begin_move = handle_begin_move,
    .begin_size = handle_begin_size,
    .destroy = handle_destroy,
};

static void bind_arranger(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *res =
        wl_resource_create(client, &xd_window_arranger_v1_interface, (int)version, id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &arranger_impl, data, resource_destroyed);
    ((xd_server *)data)->arranger_resource = res;
}

/* Only the shell we started sees the global. */
static bool global_filter(const struct wl_client *client, const struct wl_global *global,
                          void *data) {
    xd_server *server = data;
    if (global != server->arranger)
        return true;
    pid_t pid = 0;
    wl_client_get_credentials((struct wl_client *)(uintptr_t)client, &pid, nullptr, nullptr);
    return server->shell_pid > 0 && pid == server->shell_pid;
}

void xd_arrange_init(xd_server *server) {
    server->arranger = wl_global_create(server->display, &xd_window_arranger_v1_interface, 2,
                                        server, bind_arranger);
    wl_display_set_global_filter(server->display, global_filter, server);
}
