/* SPDX-License-Identifier: MIT
 * Alt+Tab's window: while Alt is held after Tab, the windows' icons with
 * the selection framed and its title, centred on the monitor under the
 * pointer, drawn by the theme (xd_theme_draw_switcher). input.c steps the
 * selection; this only shows it.
 */
#include <math.h>
#include <wlr/util/log.h>

#include "appicons.h"
#include "server.h"

/* As many as five rows of Windows' seven: more windows than that are
 * still reached by Tab, without an icon. */
static constexpr size_t MAX_ITEMS = 35;

void xd_switcher_show(xd_server *server) {
    xd_view *sel = server->cycle_view;
    if (sel == nullptr) {
        xd_switcher_hide(server);
        return;
    }
    xd_view *views[MAX_ITEMS];
    size_t count = 0, selected = 0;
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (!v->mapped || count == MAX_ITEMS)
            continue;
        if (v == sel)
            selected = count;
        views[count++] = v;
    }
    if (count == 0) {
        xd_switcher_hide(server);
        return;
    }
    double cx = server->cursor->x, cy = server->cursor->y;
    double scale = xd_output_scale_at(server, cx, cy);
    struct wlr_box area = xd_output_work_area_at(server, cx, cy);
    xd_theme theme;
    xd_theme_init(&theme, server->config.style, &server->config.scheme, scale);
    int dw, dh;
    xd_theme_switcher_size(&theme, count, &dw, &dh);
    cairo_surface_t *surface = nullptr;
    struct wlr_buffer *buf = xd_cairo_buffer_new(dw, dh, &surface);
    if (buf == nullptr)
        return;
    /* nullptr while an icon loads: the generic one, until the next step. */
    cairo_surface_t *icons[MAX_ITEMS];
    for (size_t i = 0; i < count; i++)
        icons[i] = xd_appicons_get(server->appicons, xd_view_app_id(views[i]), xd_px(scale, 32));
    const char *title = xd_view_title(sel);
    cairo_t *cr = cairo_create(surface);
    xd_theme_draw_switcher(&theme, cr, (xd_rect){0, 0, dw, dh}, icons, count, selected, title);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    if (server->switcher == nullptr)
        server->switcher = wlr_scene_buffer_create(server->layer_drag, nullptr);
    double lw = ceil(dw / scale), lh = ceil(dh / scale);
    int w = (int)lw, h = (int)lh;
    wlr_scene_buffer_set_buffer(server->switcher, buf);
    wlr_scene_buffer_set_dest_size(server->switcher, w, h);
    wlr_buffer_drop(buf); /* the scene holds its own lock */
    wlr_scene_node_set_position(&server->switcher->node, area.x + (area.width - w) / 2,
                                area.y + (area.height - h) / 2);
    wlr_scene_node_raise_to_top(&server->switcher->node);
    wlr_log(WLR_DEBUG, "Alt+Tab: %zu windows, \"%s\" selected", count,
            title != nullptr ? title : "");
}

void xd_switcher_hide(xd_server *server) {
    if (server->switcher == nullptr)
        return;
    wlr_scene_node_destroy(&server->switcher->node);
    server->switcher = nullptr;
}
