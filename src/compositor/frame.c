/* SPDX-License-Identifier: MIT
 * Server-side window frames, drawn by libxdtheme into cairo image buffers.
 *
 * Frame geometry (insets, hit areas) is defined in layout units using a
 * scale-1 theme. Pixels are rendered at the scale of the output the window
 * is on and shown at layout size via wlr_scene_buffer_set_dest_size, so
 * frames stay sharp on every monitor.
 */
#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>

#include "../core/alloc.h"
#include "server.h"

typedef struct xd_cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
} xd_cairo_buffer;

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    xd_cairo_buffer *buf = XD_CONTAINER_OF(wlr_buffer, xd_cairo_buffer, base);
    cairo_surface_destroy(buf->surface);
    xd_free(buf);
}

static bool cairo_buffer_begin_access(struct wlr_buffer *wlr_buffer, uint32_t flags, void **data,
                                      uint32_t *format, size_t *stride) {
    xd_cairo_buffer *buf = XD_CONTAINER_OF(wlr_buffer, xd_cairo_buffer, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;
    int s = cairo_image_surface_get_stride(buf->surface);
    if (s <= 0)
        return false;
    *data = cairo_image_surface_get_data(buf->surface);
    /* CAIRO_FORMAT_ARGB32 is premultiplied native-endian ARGB, which is
     * DRM_FORMAT_ARGB8888 on little-endian hosts. */
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)s;
    return true;
}

static void cairo_buffer_end_access([[maybe_unused]] struct wlr_buffer *wlr_buffer) {}

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cairo_buffer_destroy,
    .begin_data_ptr_access = cairo_buffer_begin_access,
    .end_data_ptr_access = cairo_buffer_end_access,
};

static xd_cairo_buffer *cairo_buffer_create(int width, int height) {
    XD_ASSERT(width > 0 && height > 0);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    xd_cairo_buffer *buf = XD_NEW(xd_cairo_buffer);
    buf->surface = surface;
    wlr_buffer_init(&buf->base, &cairo_buffer_impl, width, height);
    return buf;
}

double xd_view_scale(const xd_view *view) {
    struct wlr_box box = xd_view_outer_box(view);
    return xd_output_scale_at(view->server, box.x + box.width / 2.0, box.y + box.height / 2.0);
}

static int to_device(int logical, double scale) {
    double v = ceil(logical * scale);
    XD_ASSERT(v >= 0 && v < 32768);
    return (int)v;
}

void xd_frame_render(xd_view *view, bool force) {
    if (!view->mapped)
        return;
    /* The client draws its own decorations. */
    bool none = view->csd || view->fullscreen;
    wlr_scene_node_set_enabled(&view->frame->node, !none);
    if (none)
        return;

    struct wlr_box outer = xd_view_outer_box(view);
    double scale = xd_view_scale(view);
    bool active = xd_view_focused(view->server) == view;

    if (!force && !view->drawn.title_dirty && view->drawn.width == outer.width &&
        view->drawn.height == outer.height && view->drawn.scale == scale &&
        view->drawn.active == active && view->drawn.maximized == view->maximized &&
        view->drawn.dialog == view->dialog &&
        view->drawn.hot == view->hot && view->drawn.pressed == view->pressed)
        return;

    int dw = to_device(outer.width, scale);
    int dh = to_device(outer.height, scale);
    if (dw <= 0 || dh <= 0)
        return;

    xd_cairo_buffer *buf = cairo_buffer_create(dw, dh);
    if (buf == nullptr)
        return;

    xd_theme theme;
    xd_theme_init(&theme, view->server->config.style, &view->server->config.scheme, scale);
    xd_frame f = xd_view_frame(view, active);
    cairo_t *cr = cairo_create(buf->surface);
    xd_theme_draw_frame(&theme, cr, (xd_rect){0, 0, dw, dh}, &f);
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    wlr_scene_buffer_set_buffer(view->frame, &buf->base);
    wlr_scene_buffer_set_dest_size(view->frame, outer.width, outer.height);
    wlr_buffer_drop(&buf->base); /* the scene holds its own lock */

    view->drawn.width = outer.width;
    view->drawn.height = outer.height;
    view->drawn.scale = scale;
    view->drawn.active = active;
    view->drawn.maximized = view->maximized;
    view->drawn.dialog = view->dialog;
    view->drawn.hot = view->hot;
    view->drawn.pressed = view->pressed;
    view->drawn.title_dirty = false;
}
