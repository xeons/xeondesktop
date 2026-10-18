/* SPDX-License-Identifier: MIT
 * The desktop background: one layer-shell surface per output, below
 * everything, filled with the scheme's Background colour and the
 * wallpaper, and on the first monitor the desktop icons (deskicons.c).
 * With --taskbar-only it's never drawn, so never shown: the compositor
 * still tells it the output's size, which menus and the taskbar go by.
 */
#include <math.h>

#include "../core/alloc.h"
#include "../ui/image.h"
#include "shell.h"

static void desktop_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                              uint32_t width, uint32_t height) {
    xd_desktop *d = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (width > 0 && width < 65536)
        d->width = (int)width;
    if (height > 0 && height < 65536)
        d->height = (int)height;
    d->configured = true;
    d->dirty = true;
}

static void desktop_closed(void *data, [[maybe_unused]] struct zwlr_layer_surface_v1 *layer) {
    xd_desktop *d = data;
    d->closed = true; /* destroyed with its taskbar */
}

static const struct zwlr_layer_surface_v1_listener desktop_layer_listener = {
    .configure = desktop_configure,
    .closed = desktop_closed,
};

static void desktop_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *f,
                          uint32_t scale_120) {
    xd_desktop *d = data;
    double s = scale_120 / 120.0;
    if (s >= 0.5 && s <= 8.0 && s != d->scale) {
        d->scale = s;
        d->dirty = true;
    }
}

static const struct wp_fractional_scale_v1_listener desktop_fractional_listener = {
    .preferred_scale = desktop_scale,
};

xd_desktop *xd_desktop_create(xd_shell *shell, struct wl_output *output) {
    xd_desktop *d = XD_NEW(xd_desktop);
    d->shell = shell;
    d->scale = 1.0;
    d->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        d->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            shell->fractional_manager, d->surface);
        wp_fractional_scale_v1_add_listener(d->fractional, &desktop_fractional_listener, d);
        d->viewport = wp_viewporter_get_viewport(shell->viewporter, d->surface);
    }
    d->layer = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, d->surface, output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "desktop");
    zwlr_layer_surface_v1_set_anchor(d->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    /* -1: cover the whole output, under the taskbar too. */
    zwlr_layer_surface_v1_set_exclusive_zone(d->layer, -1);
    /* A click gives it the keyboard (the icons' keys), as on Windows. */
    if (!shell->taskbar_only)
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            d->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    zwlr_layer_surface_v1_add_listener(d->layer, &desktop_layer_listener, d);
    wl_surface_commit(d->surface);
    return d;
}

xd_desktop *xd_desktop_for_surface(xd_shell *shell, struct wl_surface *surface) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        if (bar->desktop != nullptr && bar->desktop->surface == surface)
            return bar->desktop;
    }
    return nullptr;
}

void xd_desktop_destroy(xd_desktop *d) {
    if (d == nullptr)
        return;
    zwlr_layer_surface_v1_destroy(d->layer);
    if (d->viewport != nullptr)
        wp_viewport_destroy(d->viewport);
    if (d->fractional != nullptr)
        wp_fractional_scale_v1_destroy(d->fractional);
    wl_surface_destroy(d->surface);
    xd_shm_ring_finish(&d->ring);
    if (d->background != nullptr)
        cairo_surface_destroy(d->background);
    xd_free(d);
}

static void paint_background_now(xd_desktop *d, cairo_t *cr, int dw, int dh) {
    xd_shell *shell = d->shell;
    xd_rect all = {0, 0, dw, dh};
    xd_fill_rect(cr, all, xd_rgba_from_rgb(shell->scheme.colors[XD_COLOR_BACKGROUND]));
    xd_image_paint_wallpaper(cr, shell->wallpaper, all, shell->settings.wallpaper_mode,
                             d->scale);
}

/* The desktop with the icons redraws on every motion of a drag over it:
 * it keeps its background (the scaled wallpaper is the slow part). The
 * others draw theirs only when something changed it. */
static void paint_background(xd_desktop *d, cairo_t *cr, int dw, int dh) {
    xd_shell *shell = d->shell;
    xd_taskbar *first = wl_list_empty(&shell->taskbars)
                            ? nullptr
                            : XD_CONTAINER_OF(shell->taskbars.next, xd_taskbar, link);
    if (first == nullptr || first->desktop != d) {
        if (d->background != nullptr)
            cairo_surface_destroy(d->background);
        d->background = nullptr;
        paint_background_now(d, cr, dw, dh);
        return;
    }
    cairo_surface_t *bg = d->background;
    if (bg == nullptr || cairo_image_surface_get_width(bg) != dw ||
        cairo_image_surface_get_height(bg) != dh || d->background_scale != d->scale ||
        d->background_gen != shell->background_gen) {
        if (bg != nullptr)
            cairo_surface_destroy(bg);
        bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dw, dh);
        cairo_t *bcr = cairo_create(bg);
        paint_background_now(d, bcr, dw, dh);
        cairo_destroy(bcr);
        d->background = bg;
        d->background_scale = d->scale;
        d->background_gen = shell->background_gen;
    }
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, bg, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

void xd_desktop_render(xd_desktop *d) {
    xd_shell *shell = d->shell;
    if (shell->taskbar_only) {
        d->dirty = false;
        return;
    }
    if (!d->configured || d->closed || d->width <= 0 || d->height <= 0)
        return;
    double dw_f = ceil(d->width * d->scale);
    double dh_f = ceil(d->height * d->scale);
    XD_ASSERT(dw_f > 0 && dw_f < 65536 && dh_f > 0 && dh_f < 65536);
    int dw = (int)dw_f;
    int dh = (int)dh_f;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&d->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;

    cairo_t *cr = cairo_create(buf->surface);
    paint_background(d, cr, dw, dh);
    xd_deskicons_draw(shell, d, cr);
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    wl_surface_attach(d->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(d->surface, 0, 0, dw, dh);
    if (d->viewport != nullptr)
        wp_viewport_set_destination(d->viewport, d->width, d->height);
    wl_surface_commit(d->surface);
    buf->busy = true;
    d->dirty = false;
}
