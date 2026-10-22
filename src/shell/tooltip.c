/* SPDX-License-Identifier: MIT
 * Tooltips over the taskbar (task buttons, Quick Launch, the clock):
 * a popup of the taskbar's layer surface, without a grab, in the scheme's
 * InfoWindow colours. Shown after a hover delay; once one is up, moving to
 * the next item shows its tooltip almost at once, as in Windows.
 */
#include <math.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "shell.h"

enum { INITIAL_MS = 500, RESHOW_MS = 60, PAD_X = 4 };

struct xd_tooltip {
    xd_shell *shell;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    char *text;
    int width, height; /* logical */
    double scale;
    bool configured;
    bool dirty;
};

static xd_theme theme_at(const xd_shell *shell, double scale) {
    xd_theme theme;
    xd_theme_init(&theme, shell->style, &shell->scheme, scale);
    return theme;
}

static xd_text_style text_style(const xd_theme *theme) {
    return xd_text(theme, XD_FONT_TOOLTIP, xd_sys(theme, XD_COLOR_INFO_TEXT), XD_ALIGN_START);
}

static void render(xd_tooltip *tip) {
    xd_shell *shell = tip->shell;
    double width = ceil(tip->width * tip->scale), height = ceil(tip->height * tip->scale);
    int dw = (int)width, dh = (int)height;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&tip->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme theme = theme_at(shell, tip->scale);
    cairo_t *cairo = cairo_create(buf->surface);
    xd_theme_draw_tooltip(&theme, cairo, (xd_rect){0, 0, dw, dh}, tip->text);
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(tip->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(tip->surface, 0, 0, dw, dh);
    if (tip->viewport != nullptr)
        wp_viewport_set_destination(tip->viewport, tip->width, tip->height);
    else
        wl_surface_set_buffer_scale(tip->surface, 1);
    wl_surface_commit(tip->surface);
    buf->busy = true;
    tip->dirty = false;
}

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    xd_tooltip *tip = data;
    xdg_surface_ack_configure(surface, serial);
    tip->configured = true;
    tip->dirty = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *popup,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *popup) {
    xd_tooltip_hide(((xd_tooltip *)data)->shell);
}

static void popup_repositioned([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_popup *popup,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

void xd_tooltip_hide(xd_shell *shell) {
    xd_tooltip *tip = shell->tooltip;
    if (tip == nullptr)
        return;
    shell->tooltip = nullptr;
    shell->tooltip_hidden_ms = xd_tooltip_now_ms();
    xdg_popup_destroy(tip->popup);
    xdg_surface_destroy(tip->xdg_surface);
    if (tip->viewport != nullptr)
        wp_viewport_destroy(tip->viewport);
    wl_surface_destroy(tip->surface);
    xd_shm_ring_finish(&tip->ring);
    xd_free(tip->text);
    xd_free(tip);
}

void xd_tooltip_show(xd_shell *shell, struct zwlr_layer_surface_v1 *layer, xd_rect anchor,
                     const char *text, double scale) {
    xd_tooltip_hide(shell);
    if (text == nullptr || text[0] == '\0' || shell->wm_base == nullptr)
        return;
    xd_tooltip *tip = XD_NEW(xd_tooltip);
    tip->shell = shell;
    tip->text = xd_str_dup_cstr(xd_str_from_cstr(text));
    tip->scale = scale;

    /* Size in logical px, measured at scale 1. */
    xd_theme t1 = theme_at(shell, 1.0);
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(probe);
    xd_text_style st = text_style(&t1);
    int text_w = xd_text_width(cairo, text, &st);
    cairo_destroy(cairo);
    cairo_surface_destroy(probe);
    tip->width = text_w + 2 * PAD_X + 2;
    tip->height = xd_metric_px(&t1, XD_METRIC_MENU_HEIGHT) + 1; /* 19px, as XP's */

    tip->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->viewporter != nullptr)
        tip->viewport = wp_viewporter_get_viewport(shell->viewporter, tip->surface);
    else
        tip->scale = 1.0;
    /* Tooltips never take input: clicks fall through to the taskbar. */
    struct wl_region *none = wl_compositor_create_region(shell->compositor);
    wl_surface_set_input_region(tip->surface, none);
    wl_region_destroy(none);
    tip->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, tip->surface);
    xdg_surface_add_listener(tip->xdg_surface, &xdg_surface_listener, tip);

    /* Above the item (the taskbar is at the bottom), left-aligned with the
     * pointer's item; flipped below if there's no room. */
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, tip->width, tip->height);
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, XD_MAX(anchor.w, 1),
                                   XD_MAX(anchor.h, 1));
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP_RIGHT);
    xdg_positioner_set_offset(pos, 0, -2);
    xdg_positioner_set_constraint_adjustment(
        pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    tip->popup = xdg_surface_get_popup(tip->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(layer, tip->popup);
    xdg_popup_add_listener(tip->popup, &popup_listener, tip);
    wl_surface_commit(tip->surface);
    shell->tooltip = tip;
    shell->tooltip_shown_ms = xd_tooltip_now_ms();
}

void xd_tooltip_render_dirty(xd_shell *shell) {
    xd_tooltip *tip = shell->tooltip;
    if (tip != nullptr && tip->configured && tip->dirty)
        render(tip);
}

int64_t xd_tooltip_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void xd_tooltip_arm(xd_shell *shell) {
    if (shell->tooltip_timer_fd < 0)
        return;
    /* Quick when a tooltip was showing a moment ago (moving along the
     * taskbar), else the usual hover delay. */
    bool recent = shell->tooltip != nullptr ||
                  xd_tooltip_now_ms() - shell->tooltip_hidden_ms < INITIAL_MS;
    int ms = recent ? RESHOW_MS : INITIAL_MS;
    struct itimerspec spec = {.it_value = {.tv_sec = ms / 1000,
                                           .tv_nsec = (long)(ms % 1000) * 1000000}};
    timerfd_settime(shell->tooltip_timer_fd, 0, &spec, nullptr);
}

void xd_tooltip_disarm(xd_shell *shell) {
    if (shell->tooltip_timer_fd >= 0) {
        struct itimerspec off = {0};
        timerfd_settime(shell->tooltip_timer_fd, 0, &off, nullptr);
    }
}
