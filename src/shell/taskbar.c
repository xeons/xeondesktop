/* SPDX-License-Identifier: MIT
 * The taskbar: one layer-shell surface per output, docked to the bottom
 * edge with an exclusive zone so maximized windows stop above it.
 *
 * Drawing happens in device pixels at the output's (fractional) scale; the
 * surface is shown at its logical size through wp_viewport.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>

#include "../core/alloc.h"
#include "shell.h"

/* Windows caps task buttons at about 160 px and shrinks them as more
 * windows open. */
enum { XD_TASK_MAX_W = 160, XD_TASK_MIN_W = 24, XD_TASK_GAP = 3 };

static void bar_theme(const xd_taskbar *bar, xd_theme *out) {
    xd_theme_init(out, bar->shell->style, &bar->shell->scheme, bar->scale);
}

static int logical_height(const xd_shell *shell) {
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, 1.0);
    return xd_theme_taskbar_height(&t);
}

void xd_taskbars_mark_dirty(xd_shell *shell) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        bar->dirty = true;
    }
}

void xd_taskbars_render_dirty(xd_shell *shell) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        if (bar->desktop != nullptr && bar->desktop->dirty)
            xd_desktop_render(bar->desktop);
        if (bar->dirty)
            xd_taskbar_render(bar);
    }
}

void xd_taskbars_forget_layout(xd_shell *shell) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        bar->quick_count = 0;
        bar->task_count = 0;
        bar->hot = XD_HIT_NONE;
        bar->pressed = XD_HIT_NONE;
        bar->dirty = true;
    }
}

void xd_taskbar_retheme(xd_taskbar *bar) {
    /* The height follows the scheme (font size changes it). */
    int h = logical_height(bar->shell);
    zwlr_layer_surface_v1_set_size(bar->layer, 0, (uint32_t)h);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer, h);
    bar->task_count = 0;
    bar->dirty = true;
    if (bar->desktop != nullptr)
        bar->desktop->dirty = true;
}

static void ensure_task_capacity(xd_taskbar *bar, size_t n) {
    if (n <= bar->task_cap)
        return;
    size_t cap = XD_MAX(n, bar->task_cap * 2);
    bar->task_rects = xd_realloc_array(bar->task_rects, cap, sizeof(*bar->task_rects));
    bar->task_views = xd_realloc_array(bar->task_views, cap, sizeof(*bar->task_views));
    bar->task_cap = cap;
}

static uint8_t button_state(const xd_taskbar *bar, int index) {
    uint8_t state = 0;
    if (bar->hot == index)
        state |= XD_STATE_HOT;
    if (bar->pressed == index && bar->hot == index)
        state |= XD_STATE_PRESSED;
    return state;
}

/* Lays out the start button, task buttons and tray for the current size,
 * then draws them. Mirrors xd-themeview's taskbar so previews match. */
static void layout_and_draw(xd_taskbar *bar, const xd_theme *t, cairo_t *cr, int dw, int dh) {
    xd_shell *shell = bar->shell;
    xd_rect full = {0, 0, dw, dh};
    xd_theme_draw_taskbar(t, cr, full);

    bool classic = t->style == XD_STYLE_CLASSIC;
    int pad = classic ? xd_tpx(t, 2) : 0;
    int inset_top = classic ? xd_tpx(t, 4) : 0;
    int button_h = classic ? dh - xd_tpx(t, 6) : dh;

    /* Start button: held down while its menu is open. */
    int sw = xd_theme_start_button_width(t, cr);
    bar->start_rect = (xd_rect){pad, inset_top, sw, button_h};
    uint8_t start_state = button_state(bar, XD_HIT_START);
    if ((shell->start_menu != nullptr || shell->start7 != nullptr) && shell->start_bar == bar)
        start_state |= XD_STATE_PRESSED;
    xd_theme_draw_start_button(t, cr, bar->start_rect, start_state);
    int after_start = bar->start_rect.x + bar->start_rect.w + xd_tpx(t, 2);

    /* Quick Launch: grip, small flat icon buttons, grip. */
    const xd_quicklaunch *ql = &shell->quicklaunch;
    bar->quick_count = 0;
    if (shell->settings.quick_launch && ql->count > 0) {
        if (ql->count > bar->quick_cap) {
            bar->quick_cap = ql->count;
            bar->quick_rects = xd_realloc_array(bar->quick_rects, bar->quick_cap,
                                                sizeof(*bar->quick_rects));
        }
        int grip = xd_theme_toolbar_grip_width(t);
        int qy = classic ? inset_top : xd_tpx(t, 3);
        int qh = classic ? button_h : dh - xd_tpx(t, 6);
        xd_theme_draw_toolbar_grip(t, cr, (xd_rect){after_start, qy, grip, qh});
        int qx = after_start + grip + xd_tpx(t, 1);
        int qw = xd_tpx(t, 23);
        int icon = xd_tpx(t, 16);
        for (size_t i = 0; i < ql->count; i++) {
            xd_rect b = {qx, qy, qw, qh};
            bar->quick_rects[i] = b;
            bar->quick_count++;
            xd_theme_draw_toolbar_button(t, cr, b, button_state(bar, XD_HIT_QUICK - (int)i));
            const char *names[] = {ql->items[i].icon, "application-x-executable"};
            cairo_surface_t *img = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names), icon);
            if (img != nullptr) {
                int pressed = (bar->pressed == XD_HIT_QUICK - (int)i &&
                               bar->hot == XD_HIT_QUICK - (int)i)
                                  ? xd_tpx(t, 1)
                                  : 0;
                cairo_set_source_surface(cr, img, b.x + (b.w - icon) / 2 + pressed,
                                         b.y + (b.h - icon) / 2 + pressed);
                cairo_paint(cr);
            }
            qx += qw;
        }
        xd_theme_draw_toolbar_grip(t, cr, (xd_rect){qx + xd_tpx(t, 2), qy, grip, qh});
        after_start = qx + xd_tpx(t, 2) + grip;
    }

    /* Notification area, sized to the clock like Windows does. */
    xd_text_style clock_style = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_END);
    int tray_w = XD_MAX(xd_tpx(t, 60), xd_text_width(cr, shell->clock, &clock_style) +
                                           xd_tpx(t, 22));
    bar->tray_rect = (xd_rect){dw - tray_w - pad, inset_top, tray_w, button_h};
    xd_theme_draw_tray(t, cr, bar->tray_rect, shell->clock);

    /* Task buttons fill the space between, shrinking as windows open. */
    size_t n = (size_t)wl_list_length(&shell->toplevels);
    ensure_task_capacity(bar, n);
    bar->task_count = 0;
    if (n == 0)
        return;

    int gap = xd_tpx(t, XD_TASK_GAP);
    int x0 = after_start + xd_tpx(t, 4);
    int x1 = bar->tray_rect.x - xd_tpx(t, 6);
    int avail = x1 - x0;
    int ni = (int)XD_MIN(n, (size_t)INT32_MAX / 2);
    int w = XD_MIN(xd_tpx(t, XD_TASK_MAX_W), (avail - (ni - 1) * gap) / ni);
    if (w < xd_tpx(t, XD_TASK_MIN_W))
        w = xd_tpx(t, XD_TASK_MIN_W); /* overflow is clipped until rows/scrolling exist */
    int ty = classic ? inset_top : xd_tpx(t, 3);
    int th = classic ? button_h : dh - xd_tpx(t, 5);

    cairo_save(cr);
    cairo_rectangle(cr, x0, 0, XD_MAX(avail, 0), dh);
    cairo_clip(cr);
    int x = x0;
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        size_t i = bar->task_count;
        if (i >= n)
            break;
        xd_rect r = {x, ty, w, th};
        bar->task_rects[i] = r;
        bar->task_views[i] = tl;
        bar->task_count++;

        uint8_t state = button_state(bar, (int)i);
        if (tl->activated && !tl->minimized)
            state |= XD_STATE_CHECKED;
        const char *label = tl->title != nullptr && tl->title[0] != '\0' ? tl->title
                            : tl->app_id != nullptr                     ? tl->app_id
                                                                         : "";
        xd_theme_draw_task_button(t, cr, r, state, label);
        x += w + gap;
    }
    cairo_restore(cr);
}

void xd_taskbar_render(xd_taskbar *bar) {
    xd_shell *shell = bar->shell;
    if (!bar->configured || bar->width <= 0 || bar->height <= 0)
        return;

    double v_w = ceil(bar->width * bar->scale);
    double v_h = ceil(bar->height * bar->scale);
    XD_ASSERT(v_w > 0 && v_w < 65536 && v_h > 0 && v_h < 65536);
    int dw = (int)v_w;
    int dh = (int)v_h;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&bar->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return; /* both buffers busy; redrawn after a release */

    xd_theme t;
    bar_theme(bar, &t);
    cairo_t *cr = cairo_create(buf->surface);
    layout_and_draw(bar, &t, cr, dw, dh);
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    wl_surface_attach(bar->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(bar->surface, 0, 0, dw, dh);
    if (bar->viewport != nullptr)
        wp_viewport_set_destination(bar->viewport, bar->width, bar->height);
    wl_surface_commit(bar->surface);
    buf->busy = true;
    bar->dirty = false;
}

/* ---- input ------------------------------------------------------------------ */

static bool rect_contains(xd_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h;
}

static int floor_int(double v) {
    double f = floor(v);
    if (!(f > -65536.0 && f < 65536.0))
        return -1; /* off any button */
    return (int)f;
}

static int hit_test(const xd_taskbar *bar, double lx, double ly) {
    int x = floor_int(lx * bar->scale);
    int y = floor_int(ly * bar->scale);
    if (rect_contains(bar->start_rect, x, y))
        return XD_HIT_START;
    if (rect_contains(bar->tray_rect, x, y))
        return XD_HIT_TRAY;
    for (size_t i = 0; i < bar->quick_count; i++) {
        if (rect_contains(bar->quick_rects[i], x, y))
            return XD_HIT_QUICK - (int)i;
    }
    for (size_t i = 0; i < bar->task_count; i++) {
        if (rect_contains(bar->task_rects[i], x, y))
            return (int)i;
    }
    return XD_HIT_NONE;
}

void xd_taskbar_pointer_motion(xd_taskbar *bar, double x, double y) {
    int hot = hit_test(bar, x, y);
    if (hot != bar->hot) {
        bar->hot = hot;
        bar->dirty = true;
    }
}

void xd_taskbar_pointer_leave(xd_taskbar *bar) {
    if (bar->hot != XD_HIT_NONE || bar->pressed != XD_HIT_NONE) {
        bar->hot = XD_HIT_NONE;
        bar->pressed = XD_HIT_NONE;
        bar->dirty = true;
    }
}

void xd_taskbar_open_start_menu(xd_taskbar *bar, uint32_t serial) {
    xd_shell *shell = bar->shell;
    if (shell->start_model == nullptr || !bar->configured)
        return;
    /* start_rect is in device px; the popup anchor is surface-local
     * logical. */
    xd_rect r = bar->start_rect;
    xd_rect anchor = {(int)(r.x / bar->scale), (int)(r.y / bar->scale),
                      (int)(r.w / bar->scale), (int)(r.h / bar->scale)};
    xd_taskbar_take_keyboard(bar);
    xd_menu_open_root(shell, shell->start_model, bar->layer, anchor, serial);
    shell->start_bar = bar;
    bar->dirty = true;
}

void xd_taskbar_take_keyboard(xd_taskbar *bar) {
    /* Take the keyboard while a menu is open so arrows, Esc and typing
     * reach us. This must be committed before the menu's popup grab
     * starts: during a grab the compositor won't move keyboard focus. */
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    wl_surface_commit(bar->surface);
}

void xd_taskbar_pointer_button(xd_taskbar *bar, uint32_t button, bool pressed,
                               uint32_t serial) {
    if (button != BTN_LEFT)
        return; /* right-click window menu comes with libxdui */
    if (pressed) {
        bar->pressed = bar->hot;
        bar->dirty = true;
        /* The Start menu opens on press, like Windows'. */
        if (bar->hot == XD_HIT_START) {
            if (bar->shell->start_menu != nullptr || bar->shell->start7 != nullptr)
                xd_menu_close_all(bar->shell);
            else if (bar->shell->settings.start_style != XD_START_CLASSIC)
                xd_start7_open(bar, serial, bar->shell->settings.start_style == XD_START_XP);
            else
                xd_taskbar_open_start_menu(bar, serial);
        }
        return;
    }
    int target = bar->pressed;
    bar->pressed = XD_HIT_NONE;
    bar->dirty = true;
    if (target != bar->hot)
        return; /* released somewhere else: cancel, like a real button */
    if (target >= 0 && (size_t)target < bar->task_count)
        xd_toplevel_click(bar->task_views[target]);
    else if (target <= XD_HIT_QUICK)
        xd_quicklaunch_activate(bar->shell, (size_t)(XD_HIT_QUICK - target));
}

void xd_taskbars_forget(xd_shell *shell, xd_toplevel *toplevel) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        for (size_t i = 0; i < bar->task_count; i++) {
            if (bar->task_views[i] == toplevel) {
                /* Indices shift; drop layout until the next render. */
                bar->task_count = 0;
                bar->quick_count = 0;
                bar->hot = XD_HIT_NONE;
                bar->pressed = XD_HIT_NONE;
                bar->dirty = true;
                break;
            }
        }
    }
}

/* ---- surface setup ------------------------------------------------------- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                            uint32_t width, uint32_t height) {
    xd_taskbar *bar = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (width > 0 && width < 65536)
        bar->width = (int)width;
    if (height > 0 && height < 65536)
        bar->height = (int)height;
    bar->configured = true;
    bar->dirty = true;
}

static void layer_closed(void *data, [[maybe_unused]] struct zwlr_layer_surface_v1 *layer) {
    xd_taskbar_destroy(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void preferred_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *f,
                            uint32_t scale_120) {
    xd_taskbar *bar = data;
    double scale = scale_120 / 120.0;
    if (scale < 0.5 || scale > 8.0 || scale == bar->scale)
        return;
    bar->scale = scale;
    bar->dirty = true;
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = preferred_scale,
};

void xd_taskbar_create(xd_shell *shell, struct wl_output *output, uint32_t global) {
    xd_taskbar *bar = XD_NEW(xd_taskbar);
    bar->shell = shell;
    bar->output = output;
    bar->output_global = global;
    bar->scale = 1.0;
    bar->hot = XD_HIT_NONE;
    bar->pressed = XD_HIT_NONE;
    wl_list_insert(shell->taskbars.prev, &bar->link);
    bar->desktop = xd_desktop_create(shell, output);

    bar->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        bar->fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(shell->fractional_manager,
                                                                bar->surface);
        wp_fractional_scale_v1_add_listener(bar->fractional, &fractional_listener, bar);
        bar->viewport = wp_viewporter_get_viewport(shell->viewporter, bar->surface);
    }

    bar->layer = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, bar->surface, output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "taskbar");
    int h = logical_height(shell);
    zwlr_layer_surface_v1_set_anchor(bar->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(bar->layer, 0, (uint32_t)h);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer, h);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(bar->layer, &layer_listener, bar);
    wl_surface_commit(bar->surface);
}

void xd_taskbar_destroy(xd_taskbar *bar) {
    if (bar->shell->start_bar == bar)
        xd_menu_close_all(bar->shell); /* its popups hang off this surface */
    if (bar->shell->pointer_bar == bar)
        bar->shell->pointer_bar = nullptr;
    wl_list_remove(&bar->link);
    if (bar->layer != nullptr)
        zwlr_layer_surface_v1_destroy(bar->layer);
    if (bar->viewport != nullptr)
        wp_viewport_destroy(bar->viewport);
    if (bar->fractional != nullptr)
        wp_fractional_scale_v1_destroy(bar->fractional);
    wl_surface_destroy(bar->surface);
    xd_shm_ring_finish(&bar->ring);
    xd_desktop_destroy(bar->desktop);
    if (bar->output != nullptr)
        wl_output_release(bar->output);
    xd_free(bar->task_rects);
    xd_free(bar->task_views);
    xd_free(bar->quick_rects);
    xd_free(bar);
}
