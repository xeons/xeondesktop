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
    xd_desktop *desktop = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (width > 0 && width < 65536)
        desktop->width = (int)width;
    if (height > 0 && height < 65536)
        desktop->height = (int)height;
    desktop->configured = true;
    desktop->dirty = true;
}

static void desktop_closed(void *data, [[maybe_unused]] struct zwlr_layer_surface_v1 *layer) {
    xd_desktop *desktop = data;
    desktop->closed = true; /* destroyed with its taskbar */
}

static const struct zwlr_layer_surface_v1_listener desktop_layer_listener = {
    .configure = desktop_configure,
    .closed = desktop_closed,
};

static void desktop_scale(void *data,
                          [[maybe_unused]] struct wp_fractional_scale_v1 *fractional_scale,
                          uint32_t scale_120) {
    xd_desktop *desktop = data;
    double scale = scale_120 / 120.0;
    if (scale >= 0.5 && scale <= 8.0 && scale != desktop->scale) {
        desktop->scale = scale;
        desktop->dirty = true;
    }
}

static const struct wp_fractional_scale_v1_listener desktop_fractional_listener = {
    .preferred_scale = desktop_scale,
};

xd_desktop *xd_desktop_create(xd_shell *shell, struct wl_output *output) {
    xd_desktop *desktop = XD_NEW(xd_desktop);
    desktop->shell = shell;
    desktop->scale = 1.0;
    desktop->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        desktop->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            shell->fractional_manager, desktop->surface);
        wp_fractional_scale_v1_add_listener(desktop->fractional, &desktop_fractional_listener,
                                            desktop);
        desktop->viewport = wp_viewporter_get_viewport(shell->viewporter, desktop->surface);
    }
    desktop->layer =
        zwlr_layer_shell_v1_get_layer_surface(shell->layer_shell, desktop->surface, output,
                                              ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "desktop");
    zwlr_layer_surface_v1_set_anchor(
        desktop->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    /* -1: cover the whole output, under the taskbar too. */
    zwlr_layer_surface_v1_set_exclusive_zone(desktop->layer, -1);
    /* A click gives it the keyboard (the icons' keys), as on Windows. */
    if (!shell->taskbar_only)
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            desktop->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    zwlr_layer_surface_v1_add_listener(desktop->layer, &desktop_layer_listener, desktop);
    wl_surface_commit(desktop->surface);
    return desktop;
}

xd_desktop *xd_desktop_for_surface(xd_shell *shell, struct wl_surface *surface) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        if (bar->desktop != nullptr && bar->desktop->surface == surface)
            return bar->desktop;
    }
    return nullptr;
}

void xd_desktop_destroy(xd_desktop *desktop) {
    if (desktop == nullptr)
        return;
    zwlr_layer_surface_v1_destroy(desktop->layer);
    if (desktop->viewport != nullptr)
        wp_viewport_destroy(desktop->viewport);
    if (desktop->fractional != nullptr)
        wp_fractional_scale_v1_destroy(desktop->fractional);
    wl_surface_destroy(desktop->surface);
    xd_shm_ring_finish(&desktop->ring);
    if (desktop->background != nullptr)
        cairo_surface_destroy(desktop->background);
    xd_free(desktop);
}

static void paint_background_now(xd_desktop *desktop, cairo_t *cairo, int dw, int dh) {
    xd_shell *shell = desktop->shell;
    xd_rect all = {0, 0, dw, dh};
    xd_fill_rect(cairo, all, xd_rgba_from_rgb(shell->scheme.colors[XD_COLOR_BACKGROUND]));
    xd_image_paint_wallpaper(cairo, shell->wallpaper, all, shell->settings.wallpaper_mode,
                             desktop->scale);
}

/* The desktop with the icons redraws on every motion of a drag over it:
 * it keeps its background (the scaled wallpaper is the slow part). The
 * others draw theirs only when something changed it. */
static void paint_background(xd_desktop *desktop, cairo_t *cairo, int dw, int dh) {
    xd_shell *shell = desktop->shell;
    xd_taskbar *first = wl_list_empty(&shell->taskbars)
                            ? nullptr
                            : XD_CONTAINER_OF(shell->taskbars.next, xd_taskbar, link);
    if (first == nullptr || first->desktop != desktop) {
        if (desktop->background != nullptr)
            cairo_surface_destroy(desktop->background);
        desktop->background = nullptr;
        paint_background_now(desktop, cairo, dw, dh);
        return;
    }
    cairo_surface_t *bg = desktop->background;
    if (bg == nullptr || cairo_image_surface_get_width(bg) != dw ||
        cairo_image_surface_get_height(bg) != dh || desktop->background_scale != desktop->scale ||
        desktop->background_gen != shell->background_gen) {
        if (bg != nullptr)
            cairo_surface_destroy(bg);
        bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dw, dh);
        cairo_t *bcr = cairo_create(bg);
        paint_background_now(desktop, bcr, dw, dh);
        cairo_destroy(bcr);
        desktop->background = bg;
        desktop->background_scale = desktop->scale;
        desktop->background_gen = shell->background_gen;
    }
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cairo, bg, 0, 0);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

void xd_desktop_render(xd_desktop *desktop) {
    xd_shell *shell = desktop->shell;
    if (shell->taskbar_only) {
        desktop->dirty = false;
        return;
    }
    if (!desktop->configured || desktop->closed || desktop->width <= 0 || desktop->height <= 0)
        return;
    double dw_f = ceil(desktop->width * desktop->scale);
    double dh_f = ceil(desktop->height * desktop->scale);
    XD_ASSERT(dw_f > 0 && dw_f < 65536 && dh_f > 0 && dh_f < 65536);
    int dw = (int)dw_f;
    int dh = (int)dh_f;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&desktop->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;

    cairo_t *cairo = cairo_create(buf->surface);
    paint_background(desktop, cairo, dw, dh);
    xd_deskicons_draw(shell, desktop, cairo);
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);

    wl_surface_attach(desktop->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(desktop->surface, 0, 0, dw, dh);
    if (desktop->viewport != nullptr)
        wp_viewport_set_destination(desktop->viewport, desktop->width, desktop->height);
    wl_surface_commit(desktop->surface);
    buf->busy = true;
    desktop->dirty = false;
}
