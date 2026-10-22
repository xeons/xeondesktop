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

#include <string.h>

#include "../core/alloc.h"
#include "appicons.h"
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
    int row_stride = cairo_image_surface_get_stride(buf->surface);
    if (row_stride <= 0)
        return false;
    *data = cairo_image_surface_get_data(buf->surface);
    /* CAIRO_FORMAT_ARGB32 is premultiplied native-endian ARGB, which is
     * DRM_FORMAT_ARGB8888 on little-endian hosts. */
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)row_stride;
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

struct wlr_buffer *xd_cairo_buffer_new(int width, int height, cairo_surface_t **surface) {
    xd_cairo_buffer *buf = cairo_buffer_create(width, height);
    if (buf == nullptr)
        return nullptr;
    *surface = buf->surface;
    return &buf->base;
}

double xd_view_scale(const xd_view *view) {
    struct wlr_box box = xd_view_outer_box(view);
    return xd_output_scale_at(view->server, box.x + box.width / 2.0, box.y + box.height / 2.0);
}

static int to_device(int logical, double scale) {
    double scaled = ceil(logical * scale);
    XD_ASSERT(scaled >= 0 && scaled < 32768);
    return (int)scaled;
}

#ifdef XD_SCENEFX
/* Aero's glass: SceneFX blurs what's behind the frame wherever the frame
 * isn't transparent, and a soft shadow lies under the window. */
static constexpr int SHADOW_SIGMA = 10;
static constexpr int SHADOW_DROP = 3;

static void update_glass(xd_view *view, struct wlr_box outer, bool shown) {
    bool aero = shown && view->server->config.style == XD_STYLE_AERO;
    if (!aero) {
        if (view->blur != nullptr)
            wlr_scene_node_set_enabled(&view->blur->node, false);
        if (view->shadow != nullptr)
            wlr_scene_node_set_enabled(&view->shadow->node, false);
        return;
    }
    if (view->shadow == nullptr) {
        static const float color[4] = {0, 0, 0, 0.45f};
        view->shadow = wlr_scene_shadow_create(view->tree, 1, 1, 6, (float)SHADOW_SIGMA, color);
    }
    if (view->blur == nullptr) {
        view->blur = wlr_scene_blur_create(view->tree, 1, 1);
        wlr_scene_blur_set_transparency_mask_source(view->blur, view->frame);
    }
    int radius = view->maximized ? 0 : 6;
    wlr_scene_blur_set_size(view->blur, outer.width, outer.height);
    wlr_scene_blur_set_corner_radius(view->blur, radius);
    wlr_scene_node_set_position(&view->blur->node, 0, 0);
    /* With transparency off (Window Color and Appearance) the glass is
     * opaque: nothing behind it to blur. */
    wlr_scene_node_set_enabled(&view->blur->node, !view->server->config.scheme.glass_opaque);
    /* No shadow when maximized, as in Windows. */
    int sigma = SHADOW_SIGMA;
    wlr_scene_shadow_set_size(view->shadow, outer.width + 2 * sigma, outer.height + 2 * sigma);
    wlr_scene_shadow_set_corner_radius(view->shadow, radius + sigma);
    wlr_scene_node_set_position(&view->shadow->node, -sigma, -sigma + SHADOW_DROP);
    wlr_scene_node_set_enabled(&view->shadow->node, !view->maximized);
    /* Shadow, then blur, then the frame and the window. */
    wlr_scene_node_lower_to_bottom(&view->blur->node);
    wlr_scene_node_lower_to_bottom(&view->shadow->node);
}
#endif

void xd_frame_render(xd_view *view, bool force) {
    if (!view->mapped)
        return;
    /* The client draws its own decorations. */
    bool none = !xd_view_framed(view);
    wlr_scene_node_set_enabled(&view->frame->node, !none);
#ifdef XD_SCENEFX
    update_glass(view, xd_view_outer_box(view), !none);
#endif
    if (none)
        return;

    struct wlr_box outer = xd_view_outer_box(view);
    double scale = xd_view_scale(view);
    bool active = xd_view_focused(view->server) == view && !view->sentry_dim;
    /* nullptr until it has loaded: then xd_frame_icon_ready redraws. */
    cairo_surface_t *icon =
        xd_appicons_get(view->server->appicons, xd_view_app_id(view), xd_px(scale, 16));

    if (!force && !view->drawn.title_dirty && view->drawn.icon == icon &&
        view->drawn.width == outer.width &&
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
    xd_frame frame = xd_view_frame(view, active);
    frame.icon = icon;
    cairo_t *cairo = cairo_create(buf->surface);
    xd_theme_draw_frame(&theme, cairo, (xd_rect){0, 0, dw, dh}, &frame);
    cairo_destroy(cairo);
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
    view->drawn.icon = icon;
}

void xd_frame_icon_ready(void *data, const char *app_id) {
    xd_server *server = data;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        const char *id = xd_view_app_id(view);
        if (strcmp(id != nullptr ? id : "", app_id) == 0)
            xd_frame_render(view, false);
    }
    /* Alt+Tab's window shows the icons too. */
    if (server->switcher != nullptr)
        xd_switcher_show(server);
}
