/* SPDX-License-Identifier: MIT
 * Cascade and tile windows for the taskbar menu, the window menu's
 * keyboard Move and Size, the language indicator's keyboard layout, and
 * Aero Peek and Windows 7's thumbnails (xd-window-arranger-v1, a private
 * protocol only the shell we started can see).
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>

#include "../core/alloc.h"
#include "server.h"
#include "xd-window-arranger-v1-protocol.h"

static constexpr int CASCADE_STEP = 26; /* logical px: about a caption */

/* One client's binding: the shell's, or Task Manager's with the windows
 * it marked for arrange_marked. */
typedef struct arranger_client {
    xd_server *server;
    xd_view **marked;
    size_t marked_n, marked_cap;
} arranger_client;

static xd_server *server_of(struct wl_resource *res) {
    return ((arranger_client *)wl_resource_get_user_data(res))->server;
}

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

static bool of_app(const xd_view *v, const char *app_id) {
    const char *id = xd_view_app_id(v);
    return app_id == nullptr || (id != nullptr && strcmp(id, app_id) == 0);
}

/* Which windows take part: all, one program's, or Task Manager's marks. */
typedef struct selection {
    const char *app_id;
    const arranger_client *marks; /* nullptr: not by marks */
} selection;

static bool selected(const xd_view *v, const selection *sel) {
    if (sel->marks != nullptr) {
        for (size_t i = 0; i < sel->marks->marked_n; i++)
            if (sel->marks->marked[i] == v)
                return true;
        return false;
    }
    return of_app(v, sel->app_id);
}

/* Every window, or only the selection's. */
static void arrange_selection(xd_server *server, uint32_t mode, const selection *sel) {
    if (mode > XD_WINDOW_ARRANGER_V1_MODE_TILE_VERTICALLY)
        return;
    bool chosen = sel->app_id != nullptr || sel->marks != nullptr;
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        v->undo_valid = false;
        if (chosen && v->mapped && v->minimized && selected(v, sel))
            xd_view_restore(v);
    }
    /* Group by output (by window centre), front to back. */
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        size_t cap = (size_t)wl_list_length(&server->views);
        xd_view **views = xd_alloc_array(XD_MAX(cap, 1), sizeof(*views));
        size_t n = 0;
        XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
            if (!arrangeable(v) || !selected(v, sel))
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

static void arrange(xd_server *server, uint32_t mode, const char *app_id) {
    selection sel = {.app_id = app_id};
    arrange_selection(server, mode, &sel);
}

static void handle_arrange(struct wl_client *client, struct wl_resource *res, uint32_t mode) {
    (void)client;
    arrange(server_of(res), mode, nullptr);
}

static void handle_arrange_app(struct wl_client *client, struct wl_resource *res, uint32_t mode,
                               const char *app_id) {
    (void)client;
    arrange(server_of(res), mode, app_id);
}

/* The window behind one of this client's foreign-toplevel handles. */
static xd_view *view_for_handle(xd_server *server, struct wl_resource *handle) {
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (v->ftl == nullptr)
            continue;
        struct wl_resource *r;
        wl_resource_for_each(r, &v->ftl->resources) {
            if (r == handle)
                return v;
        }
    }
    return nullptr;
}

static void handle_mark(struct wl_client *client, struct wl_resource *res,
                        struct wl_resource *toplevel) {
    (void)client;
    arranger_client *ac = wl_resource_get_user_data(res);
    xd_view *v = view_for_handle(ac->server, toplevel);
    if (v == nullptr)
        return;
    if (ac->marked_n == ac->marked_cap) {
        ac->marked_cap = ac->marked_cap == 0 ? 8 : ac->marked_cap * 2;
        ac->marked = xd_realloc_array(ac->marked, ac->marked_cap, sizeof(*ac->marked));
    }
    ac->marked[ac->marked_n++] = v;
}

static void handle_set_always_on_top(struct wl_client *client, struct wl_resource *res,
                                     struct wl_resource *surface, uint32_t on_top) {
    xd_server *server = server_of(res);
    struct wlr_surface *s = wlr_surface_from_resource(surface);
    /* Only the client's own windows. */
    if (wl_resource_get_client(surface) != client)
        return;
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (xd_view_surface(v) == s) {
            xd_view_set_on_top(v, on_top != 0);
            return;
        }
    }
}

static void handle_peek(struct wl_client *client, struct wl_resource *res, uint32_t on) {
    (void)client;
    xd_view_peek(server_of(res), on != 0);
}

static void handle_set_keyboard_layout(struct wl_client *client, struct wl_resource *res,
                                       uint32_t index) {
    (void)client;
    xd_input_set_keyboard_layout(server_of(res), index);
}

/* Windows 7's taskbar thumbnails: which ext-foreign-toplevel-list handle
 * is this window's, so the shell can capture it. */
static void handle_identify(struct wl_client *client, struct wl_resource *res,
                            struct wl_resource *toplevel) {
    (void)client;
    xd_view *v = view_for_handle(server_of(res), toplevel);
    if (v != nullptr && v->ext_toplevel != nullptr && v->ext_toplevel->identifier != nullptr)
        xd_window_arranger_v1_send_identifier(res, toplevel, v->ext_toplevel->identifier);
}

static void handle_peek_window(struct wl_client *client, struct wl_resource *res,
                               struct wl_resource *toplevel) {
    (void)client;
    xd_server *server = server_of(res);
    xd_view_peek_window(server, toplevel != nullptr ? view_for_handle(server, toplevel) : nullptr);
}

static void handle_arrange_marked(struct wl_client *client, struct wl_resource *res,
                                  uint32_t mode) {
    (void)client;
    arranger_client *ac = wl_resource_get_user_data(res);
    selection sel = {.marks = ac};
    if (ac->marked_n > 0)
        arrange_selection(ac->server, mode, &sel);
    ac->marked_n = 0;
}

/* A window going away leaves every client's marks. */
void xd_arrange_forget_view(xd_server *server, xd_view *view) {
    for (size_t c = 0; c < server->arranger_clients_n; c++) {
        arranger_client *ac = server->arranger_clients[c];
        for (size_t i = 0; i < ac->marked_n;) {
            if (ac->marked[i] == view)
                ac->marked[i] = ac->marked[--ac->marked_n];
            else
                i++;
        }
    }
}

static void handle_undo(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    xd_server *server = server_of(res);
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
    xd_view *v = xd_view_focused(server_of(res));
    if (v != nullptr)
        xd_view_begin_keyboard_move(v);
}

static void handle_begin_size(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    xd_view *v = xd_view_focused(server_of(res));
    if (v != nullptr)
        xd_view_begin_keyboard_size(v);
}

static void handle_destroy(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void resource_destroyed(struct wl_resource *res) {
    arranger_client *ac = wl_resource_get_user_data(res);
    xd_server *server = ac->server;
    if (server->arranger_resource == res) {
        server->arranger_resource = nullptr;
        xd_view_peek(server, false); /* a shell gone mid-peek leaves no glass behind */
    }
    for (size_t c = 0; c < server->arranger_clients_n; c++) {
        if (server->arranger_clients[c] == ac) {
            server->arranger_clients[c] = server->arranger_clients[--server->arranger_clients_n];
            break;
        }
    }
    xd_free(ac->marked);
    xd_free(ac);
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

void xd_window_menu_show(xd_view *view, double lx, double ly, bool keyboard) {
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
    if (keyboard &&
        wl_resource_get_version(res) >= XD_WINDOW_ARRANGER_V1_WINDOW_MENU_KEYBOARD_SINCE_VERSION)
        xd_window_arranger_v1_send_window_menu_keyboard(res);
    xd_window_arranger_v1_send_window_menu(res, output_res, (int32_t)(lx - box.x),
                                           (int32_t)(ly - box.y),
                                           wl_display_next_serial(server->display));
}

void xd_arrange_keyboard_layout(xd_server *server) {
    struct wl_resource *res = server->arranger_resource;
    if (res != nullptr &&
        wl_resource_get_version(res) >= XD_WINDOW_ARRANGER_V1_KEYBOARD_LAYOUT_SINCE_VERSION)
        xd_window_arranger_v1_send_keyboard_layout(res, server->kb_group);
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

/* xd_power_event is the protocol's enum. */
static_assert((int)XD_POWER_EVENT_LID_CLOSED ==
                  (int)XD_WINDOW_ARRANGER_V1_POWER_BUTTON_LID_CLOSED &&
              (int)XD_POWER_EVENT_LID_OPENED ==
                  (int)XD_WINDOW_ARRANGER_V1_POWER_BUTTON_LID_OPENED &&
              (int)XD_POWER_EVENT_POWER == (int)XD_WINDOW_ARRANGER_V1_POWER_BUTTON_POWER &&
              (int)XD_POWER_EVENT_SLEEP == (int)XD_WINDOW_ARRANGER_V1_POWER_BUTTON_SLEEP);

bool xd_arrange_power_button(xd_server *server, xd_power_event event) {
    struct wl_resource *res = server->arranger_resource;
    if (res == nullptr ||
        wl_resource_get_version(res) < XD_WINDOW_ARRANGER_V1_POWER_BUTTON_SINCE_VERSION)
        return false;
    xd_window_arranger_v1_send_power_button(res, event);
    return true;
}

/* Testing only (a headless compositor has no lid): the global is offered
 * to every client then, and the request ignored otherwise. */
static void handle_test_lid(struct wl_client *client, struct wl_resource *res, uint32_t closed) {
    (void)client;
    xd_server *server = server_of(res);
    if (server->config.virtual_input)
        xd_input_lid(server, closed != 0);
}

static const struct xd_window_arranger_v1_interface arranger_impl = {
    .arrange = handle_arrange,
    .arrange_app = handle_arrange_app,
    .undo = handle_undo,
    .begin_move = handle_begin_move,
    .begin_size = handle_begin_size,
    .destroy = handle_destroy,
    .mark = handle_mark,
    .arrange_marked = handle_arrange_marked,
    .set_always_on_top = handle_set_always_on_top,
    .peek = handle_peek,
    .set_keyboard_layout = handle_set_keyboard_layout,
    .identify = handle_identify,
    .peek_window = handle_peek_window,
    .test_lid = handle_test_lid,
};

static void bind_arranger(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *res =
        wl_resource_create(client, &xd_window_arranger_v1_interface, (int)version, id);
    if (res == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    xd_server *server = data;
    arranger_client *ac = XD_NEW(arranger_client);
    ac->server = server;
    if (server->arranger_clients_n == server->arranger_clients_cap) {
        server->arranger_clients_cap =
            server->arranger_clients_cap == 0 ? 4 : server->arranger_clients_cap * 2;
        server->arranger_clients = xd_realloc_array(
            server->arranger_clients, server->arranger_clients_cap, sizeof(*server->arranger_clients));
    }
    server->arranger_clients[server->arranger_clients_n++] = ac;
    wl_resource_set_implementation(res, &arranger_impl, ac, resource_destroyed);
    /* Events (Start menu, window menu, attention) go to the shell. */
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, nullptr, nullptr);
    if (server->shell_pid > 0 && pid == server->shell_pid) {
        server->arranger_resource = res;
        xd_arrange_keyboard_layout(server); /* the language indicator's start */
    }
}

/* Whether `pid` runs our Task Manager: the xd-taskmgr installed beside
 * this compositor. */
static bool is_task_manager(pid_t pid) {
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
    char *exe = realpath(link, nullptr);
    char *self = realpath("/proc/self/exe", nullptr);
    bool ok = false;
    if (exe != nullptr && self != nullptr) {
        char *slash = strrchr(self, '/');
        if (slash != nullptr) {
            slash[1] = '\0';
            size_t n = strlen(self);
            ok = strncmp(exe, self, n) == 0 && strcmp(exe + n, "xd-taskmgr") == 0;
        }
    }
    free(exe);
    free(self);
    return ok;
}

/* Only the shell we started and our Task Manager see the global; window
 * capture (capture.c), only the shell. */
static bool global_filter(const struct wl_client *client, const struct wl_global *global,
                          void *data) {
    xd_server *server = data;
    /* The input method's (ime.c). */
    if (xd_ime_global_private(server, global))
        return xd_ime_client_allowed(server, client);
    bool capture = xd_capture_global_private(server, global);
    if (global != server->arranger && !capture)
        return true;
    /* UI tests' xd-inject toggles the lid through it (test_lid). */
    if (global == server->arranger && server->config.virtual_input)
        return true;
    pid_t pid = 0;
    wl_client_get_credentials((struct wl_client *)(uintptr_t)client, &pid, nullptr, nullptr);
    if (server->shell_pid > 0 && pid == server->shell_pid)
        return true;
    return !capture && pid > 0 && is_task_manager(pid);
}

void xd_arrange_init(xd_server *server) {
    server->arranger = wl_global_create(server->display, &xd_window_arranger_v1_interface, 10,
                                        server, bind_arranger);
    wl_display_set_global_filter(server->display, global_filter, server);
}
