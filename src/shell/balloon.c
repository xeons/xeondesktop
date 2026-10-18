/* SPDX-License-Identifier: MIT
 * Windows XP's "New programs installed" balloon: once programs appear
 * that weren't there before, a balloon tip in the InfoWindow colours
 * points at the Start button, where All Programs now lights them. It goes
 * on a click, when the Start menu opens, or after a while. A popup of the
 * primary taskbar's layer surface, without a grab.
 */
#include <glib.h>
#include <math.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "shell.h"

/* Logical px, after XP's balloon tooltips: rounded corners, a stem down
 * to the Start button from near the balloon's left end, a close box. */
static constexpr int PAD = 10;
static constexpr int RADIUS = 7;
static constexpr int STEM_H = 16;
static constexpr int STEM_X = 16; /* the stem's tip, from the balloon's left */
static constexpr int STEM_W = 16;
static constexpr int CLOSE = 14;
static constexpr int TEXT_H = 16;
/* How long it stays (XP's balloons stay at least ten seconds). */
static constexpr unsigned SHOW_SECONDS = 15;
/* Waiting at startup for a taskbar to show it on. */
static constexpr unsigned RETRY_SECONDS = 2;
static constexpr int RETRIES = 15;

static const char TEXT[] = "New programs installed";

typedef struct xd_balloon {
    xd_shell *shell;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    int width, height; /* logical, stem included */
    bool down;         /* below the Start button (a taskbar at the top or a side) */
    double scale;
    bool configured, dirty;
    unsigned timeout; /* GLib source */
} xd_balloon;

/* A balloon waiting for a taskbar (at startup). */
static unsigned pending;
static int retries;

static xd_theme theme_at(const xd_shell *shell, double scale) {
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, scale);
    return t;
}

/* The outline: a rounded box with the stem on the side facing the
 * button, its upright edge at the tip (device px). */
static void outline(cairo_t *cr, const xd_balloon *b, double s) {
    double w = b->width * s, h = b->height * s;
    double stem = STEM_H * s, r = RADIUS * s;
    double top = b->down ? stem : 0, bottom = b->down ? h : h - stem;
    double tip = STEM_X * s, base = (STEM_X + STEM_W) * s;
    double inset = 0.5 * XD_MAX(round(s), 1);
    top += inset;
    bottom -= inset;
    double left = inset, right = w - inset;
    cairo_new_path(cr);
    cairo_move_to(cr, left + r, top);
    if (b->down) {
        cairo_line_to(cr, tip, top);
        cairo_line_to(cr, tip, inset);
        cairo_line_to(cr, base, top);
    }
    cairo_arc(cr, right - r, top + r, r, -XD_PI / 2, 0);
    cairo_arc(cr, right - r, bottom - r, r, 0, XD_PI / 2);
    if (!b->down) {
        cairo_line_to(cr, base, bottom);
        cairo_line_to(cr, tip, h - inset);
        cairo_line_to(cr, tip, bottom);
    }
    cairo_arc(cr, left + r, bottom - r, r, XD_PI / 2, XD_PI);
    cairo_arc(cr, left + r, top + r, r, XD_PI, 3 * XD_PI / 2);
    cairo_close_path(cr);
}

static void render(xd_balloon *b) {
    xd_shell *shell = b->shell;
    double fw = ceil(b->width * b->scale), fh = ceil(b->height * b->scale);
    int dw = (int)fw, dh = (int)fh;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&b->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme t = theme_at(shell, b->scale);
    double s = b->scale;
    cairo_t *cr = cairo_create(buf->surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    outline(cr, b, s);
    xd_set_color(cr, xd_sys(&t, XD_COLOR_INFO_WINDOW));
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, XD_MAX(round(s), 1));
    xd_set_color(cr, xd_sys(&t, XD_COLOR_INFO_TEXT));
    cairo_stroke(cr);

    int top = b->down ? STEM_H : 0;
    xd_rgba ink = xd_sys(&t, XD_COLOR_INFO_TEXT);
    xd_text_style st = xd_text(&t, XD_FONT_TOOLTIP, ink, XD_ALIGN_START);
    xd_rect text = {xd_px(s, PAD), xd_px(s, top + PAD), xd_px(s, b->width - 2 * PAD - CLOSE - 4),
                    xd_px(s, TEXT_H)};
    xd_draw_text(cr, text, TEXT, &st);
    /* The close box's cross, top right. */
    double cx = (b->width - PAD + 4 - CLOSE / 2.0) * s, cy = (top + PAD + TEXT_H / 2.0) * s;
    double arm = 3.5 * s;
    xd_set_color(cr, ink);
    cairo_set_line_width(cr, XD_MAX(round(1.5 * s), 1));
    cairo_move_to(cr, cx - arm, cy - arm);
    cairo_line_to(cr, cx + arm, cy + arm);
    cairo_move_to(cr, cx + arm, cy - arm);
    cairo_line_to(cr, cx - arm, cy + arm);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(b->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(b->surface, 0, 0, dw, dh);
    if (b->viewport != nullptr)
        wp_viewport_set_destination(b->viewport, b->width, b->height);
    wl_surface_commit(b->surface);
    buf->busy = true;
    b->dirty = false;
}

static void xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    xd_balloon *b = data;
    xdg_surface_ack_configure(s, serial);
    b->configured = true;
    b->dirty = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *p,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *p) {
    xd_balloon_hide(((xd_balloon *)data)->shell);
}

static void popup_repositioned([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_popup *p,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static gboolean timed_out(gpointer data) {
    xd_shell *shell = data;
    if (shell->balloon != nullptr)
        shell->balloon->timeout = 0;
    xd_balloon_hide(shell);
    return G_SOURCE_REMOVE;
}

void xd_balloon_hide(xd_shell *shell) {
    if (pending != 0) {
        g_source_remove(pending);
        pending = 0;
    }
    xd_balloon *b = shell->balloon;
    if (b == nullptr)
        return;
    shell->balloon = nullptr;
    shell->pointer_balloon = false;
    if (b->timeout != 0)
        g_source_remove(b->timeout);
    xdg_popup_destroy(b->popup);
    xdg_surface_destroy(b->xdg_surface);
    if (b->viewport != nullptr)
        wp_viewport_destroy(b->viewport);
    wl_surface_destroy(b->surface);
    xd_shm_ring_finish(&b->ring);
    xd_free(b);
}

/* Shows it over the primary taskbar's Start button; false if there's no
 * taskbar to show it on yet. */
static bool show(xd_shell *shell) {
    xd_taskbar *bar = nullptr, *b;
    wl_list_for_each(b, &shell->taskbars, link) {
        if (b->configured && b->layer != nullptr && b->start_rect.w > 0) {
            bar = b;
            break;
        }
    }
    if (bar == nullptr || shell->wm_base == nullptr)
        return false;
    xd_balloon_hide(shell);
    xd_balloon *bl = XD_NEW(xd_balloon);
    bl->shell = shell;
    bl->scale = bar->scale;
    bl->down = shell->settings.edge != XD_EDGE_BOTTOM;

    xd_theme t1 = theme_at(shell, 1.0);
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(probe);
    xd_text_style st = xd_text(&t1, XD_FONT_TOOLTIP, xd_rgba_hex(0), XD_ALIGN_START);
    int text_w = xd_text_width(cr, TEXT, &st);
    cairo_destroy(cr);
    cairo_surface_destroy(probe);
    bl->width = PAD + text_w + 4 + CLOSE + PAD;
    bl->height = PAD + TEXT_H + PAD + STEM_H;

    bl->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->viewporter != nullptr)
        bl->viewport = wp_viewporter_get_viewport(shell->viewporter, bl->surface);
    else
        bl->scale = 1.0;
    bl->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, bl->surface);
    xdg_surface_add_listener(bl->xdg_surface, &xdg_surface_listener, bl);

    /* The stem's tip on the middle of the button's edge facing the
     * screen (logical, taskbar-local). */
    xd_rect r = bar->start_rect;
    int tip_x = (int)((r.x + r.w / 2.0) / bar->scale);
    int edge_y = (int)((bl->down ? r.y + r.h : r.y) / bar->scale);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, bl->width, bl->height);
    xdg_positioner_set_anchor_rect(pos, tip_x, edge_y, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, bl->down ? XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT
                                             : XDG_POSITIONER_GRAVITY_TOP_RIGHT);
    xdg_positioner_set_offset(pos, -STEM_X, 0);
    xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);
    bl->popup = xdg_surface_get_popup(bl->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(bar->layer, bl->popup);
    xdg_popup_add_listener(bl->popup, &popup_listener, bl);
    wl_surface_commit(bl->surface);
    bl->timeout = g_timeout_add_seconds(SHOW_SECONDS, timed_out, shell);
    shell->balloon = bl;
    return true;
}

static gboolean retry(gpointer data) {
    xd_shell *shell = data;
    if (shell->start_menu != nullptr || shell->start7 != nullptr || show(shell) ||
        --retries <= 0) {
        pending = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void xd_balloon_new_programs(xd_shell *shell) {
    /* XP's own: with its Start menu, while it lights new programs. */
    if (shell->settings.start_style != XD_START_XP || !shell->settings.start_highlight_new)
        return;
    /* Not over an open Start menu, which shows them already. */
    if (shell->start_menu != nullptr || shell->start7 != nullptr)
        return;
    if (show(shell) || pending != 0)
        return;
    retries = RETRIES;
    pending = g_timeout_add_seconds(RETRY_SECONDS, retry, shell);
}

bool xd_balloon_owns(const xd_shell *shell, struct wl_surface *surface) {
    return shell->balloon != nullptr && surface != nullptr && shell->balloon->surface == surface;
}

void xd_balloon_button(xd_shell *shell, bool pressed) {
    /* Anywhere on it, as on the close box: it's gone. */
    if (!pressed)
        xd_balloon_hide(shell);
}

void xd_balloon_render_dirty(xd_shell *shell) {
    xd_balloon *b = shell->balloon;
    if (b != nullptr && b->configured && b->dirty)
        render(b);
}
