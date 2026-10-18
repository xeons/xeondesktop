/* SPDX-License-Identifier: MIT
 * Windows XP's balloon tips, in the InfoWindow colours:
 *   - "New programs installed": once programs appear that weren't there
 *     before, it points at the Start button, where All Programs now
 *     lights them;
 *   - a notification area icon's: an icon, a bold title and a line of
 *     text pointing at the icon ("Updates are ready for your computer");
 *     a click on it does what the icon would.
 * It goes on a click, when the Start menu opens, or after a while. A popup
 * of the primary taskbar's layer surface, without a grab.
 */
#include <glib.h>
#include <math.h>
#include <string.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "shell.h"

/* Logical px, after XP's balloon tooltips: rounded corners, a stem to
 * what it points at from near one end, a close box. */
static constexpr int PAD = 10;
static constexpr int RADIUS = 7;
static constexpr int STEM_H = 16;
static constexpr int STEM_X = 16; /* the stem's tip, from the balloon's end */
static constexpr int STEM_W = 16;
static constexpr int CLOSE = 14;
static constexpr int TEXT_H = 16;
static constexpr int ICON = 16;  /* a titled balloon's icon */
static constexpr int GAP = 4;    /* between the title and the text */
/* How long it stays (XP's balloons stay at least ten seconds). */
static constexpr unsigned SHOW_SECONDS = 15;
/* Waiting at startup for a taskbar to show it on. */
static constexpr unsigned RETRY_SECONDS = 2;
static constexpr int RETRIES = 15;

static const char TEXT[] = "New programs installed";

/* What a balloon says and where it points. */
typedef struct content {
    const char *item; /* the notification area icon's id; nullptr: the Start button */
    const char *icon, *title; /* nullptr for the Start button's */
    const char *text;
    void (*clicked)(xd_shell *shell, void *data);
    void *data;
} content;

/* The same, kept (copies of the strings). */
typedef struct owned {
    char *item, *icon, *title, *text;
    void (*clicked)(xd_shell *shell, void *data);
    void *data;
} owned;

typedef struct xd_balloon {
    xd_shell *shell;
    owned c;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    int width, height; /* logical, stem included */
    bool down;         /* below what it points at (a taskbar at the top or a side) */
    bool right;        /* the stem at the right end (a notification area icon) */
    double scale;
    bool configured, dirty;
    double px, py;    /* the pointer, surface-local */
    unsigned timeout; /* GLib source */
} xd_balloon;

/* A balloon waiting for a taskbar (at startup). */
static unsigned pending;
static int retries;
static owned waiting;

static void content_clear(owned *c) {
    g_free(c->item);
    g_free(c->icon);
    g_free(c->title);
    g_free(c->text);
    *c = (owned){0};
}

static owned content_copy(const content *c) {
    return (owned){g_strdup(c->item), g_strdup(c->icon), g_strdup(c->title), g_strdup(c->text),
                   c->clicked, c->data};
}

static content content_view(const owned *c) {
    return (content){c->item, c->icon, c->title, c->text, c->clicked, c->data};
}

static xd_theme theme_at(const xd_shell *shell, double scale) {
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, scale);
    return t;
}

/* The stem's upright edge (tip) and its slanted end (base), logical. */
static void stem_x(const xd_balloon *b, double *tip, double *base) {
    *tip = b->right ? b->width - STEM_X : STEM_X;
    *base = b->right ? *tip - STEM_W : *tip + STEM_W;
}

/* The outline: a rounded box with the stem on the side facing what it
 * points at (device px). */
static void outline(cairo_t *cr, const xd_balloon *b, double s) {
    double w = b->width * s, h = b->height * s;
    double stem = STEM_H * s, r = RADIUS * s;
    double top = b->down ? stem : 0, bottom = b->down ? h : h - stem;
    double tip, base;
    stem_x(b, &tip, &base);
    tip *= s;
    base *= s;
    double inset = 0.5 * XD_MAX(round(s), 1);
    top += inset;
    bottom -= inset;
    double left = inset, right = w - inset;
    /* Clockwise from the top left: along the top left to right, the
     * bottom right to left; the stem's two ends in that order. */
    double first = XD_MIN(tip, base), second = XD_MAX(tip, base);
    cairo_new_path(cr);
    cairo_move_to(cr, left + r, top);
    if (b->down) {
        cairo_line_to(cr, first, top);
        cairo_line_to(cr, tip, inset);
        cairo_line_to(cr, second, top);
    }
    cairo_arc(cr, right - r, top + r, r, -XD_PI / 2, 0);
    cairo_arc(cr, right - r, bottom - r, r, 0, XD_PI / 2);
    if (!b->down) {
        cairo_line_to(cr, second, bottom);
        cairo_line_to(cr, tip, h - inset);
        cairo_line_to(cr, first, bottom);
    }
    cairo_arc(cr, left + r, bottom - r, r, XD_PI / 2, XD_PI);
    cairo_arc(cr, left + r, top + r, r, XD_PI, 3 * XD_PI / 2);
    cairo_close_path(cr);
}

/* The close box's centre (logical, surface-local). */
static void close_box(const xd_balloon *b, double *cx, double *cy) {
    int top = b->down ? STEM_H : 0;
    *cx = b->width - PAD + 4 - CLOSE / 2.0;
    *cy = top + PAD + TEXT_H / 2.0;
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
    int text_w = b->width - 2 * PAD - CLOSE - 4;
    if (b->c.title != nullptr) {
        /* XP's: the icon, the title in bold beside it, the text under. */
        int x = PAD;
        cairo_surface_t *icon = b->c.icon != nullptr
                                    ? xd_icons_get(shell->icons, b->c.icon, xd_px(s, ICON))
                                    : nullptr;
        if (icon != nullptr) {
            cairo_set_source_surface(cr, icon, xd_px(s, PAD),
                                     xd_px(s, top + PAD + (TEXT_H - ICON) / 2));
            cairo_paint(cr);
            x += ICON + 6;
        }
        xd_font_spec bold = *st.font;
        bold.bold = true;
        xd_text_style bst = st;
        bst.font = &bold;
        xd_draw_text(cr,
                     (xd_rect){xd_px(s, x), xd_px(s, top + PAD), xd_px(s, text_w - (x - PAD)),
                               xd_px(s, TEXT_H)},
                     b->c.title, &bst);
        xd_draw_text(cr,
                     (xd_rect){xd_px(s, PAD), xd_px(s, top + PAD + TEXT_H + GAP),
                               xd_px(s, b->width - 2 * PAD), xd_px(s, TEXT_H)},
                     b->c.text, &st);
    } else {
        xd_draw_text(cr,
                     (xd_rect){xd_px(s, PAD), xd_px(s, top + PAD), xd_px(s, text_w),
                               xd_px(s, TEXT_H)},
                     b->c.text, &st);
    }
    /* The close box's cross, top right. */
    double cx, cy;
    close_box(b, &cx, &cy);
    cx *= s;
    cy *= s;
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
        content_clear(&waiting);
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
    content_clear(&b->c);
    xd_free(b);
}

/* Where a notification area icon is on `bar` (device px), or the
 * chevron it's folded behind; false if the bar has no notification area. */
static bool tray_anchor(xd_taskbar *bar, const char *item, xd_rect *out) {
    for (size_t i = 0; i < bar->tray_count; i++) {
        xd_tray_item *it = xd_tray_get(bar->shell, bar->tray_index[i]);
        const char *id = it != nullptr ? xd_tray_item_id(it) : nullptr;
        if (id != nullptr && strcmp(id, item) == 0) {
            *out = bar->tray_rects[i];
            return true;
        }
    }
    if (bar->tray_chevron.w > 0) {
        *out = bar->tray_chevron;
        return true;
    }
    *out = bar->tray_rect;
    return bar->tray_rect.w > 0;
}

/* Shows it on the primary taskbar; false if there's no taskbar to show
 * it on yet. */
static bool show(xd_shell *shell, const content *c) {
    xd_taskbar *bar = nullptr, *b;
    xd_rect r = {0};
    wl_list_for_each(b, &shell->taskbars, link) {
        if (!b->configured || b->layer == nullptr)
            continue;
        if (c->item != nullptr ? tray_anchor(b, c->item, &r) : b->start_rect.w > 0) {
            bar = b;
            break;
        }
    }
    if (bar == nullptr || shell->wm_base == nullptr)
        return false;
    if (c->item == nullptr)
        r = bar->start_rect;
    xd_balloon_hide(shell);
    xd_balloon *bl = XD_NEW(xd_balloon);
    bl->shell = shell;
    bl->c = content_copy(c);
    bl->scale = bar->scale;
    bl->down = shell->settings.edge != XD_EDGE_BOTTOM;
    if (c->item != nullptr) {
        /* The notification area is at the right end (or the bottom of a
         * side taskbar, where the balloon goes up). */
        bl->down = shell->settings.edge == XD_EDGE_TOP;
        bl->right = shell->settings.edge != XD_EDGE_LEFT;
    }

    xd_theme t1 = theme_at(shell, 1.0);
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(probe);
    xd_text_style st = xd_text(&t1, XD_FONT_TOOLTIP, xd_rgba_hex(0), XD_ALIGN_START);
    int text_w = xd_text_width(cr, c->text, &st);
    if (c->title != nullptr) {
        xd_font_spec bold = *st.font;
        bold.bold = true;
        xd_text_style bst = st;
        bst.font = &bold;
        int title_w = (c->icon != nullptr ? ICON + 6 : 0) + xd_text_width(cr, c->title, &bst);
        bl->width = PAD + XD_MAX(title_w + 4 + CLOSE, text_w) + PAD;
        bl->height = PAD + TEXT_H + GAP + TEXT_H + PAD + STEM_H;
    } else {
        bl->width = PAD + text_w + 4 + CLOSE + PAD;
        bl->height = PAD + TEXT_H + PAD + STEM_H;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(probe);

    bl->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->viewporter != nullptr)
        bl->viewport = wp_viewporter_get_viewport(shell->viewporter, bl->surface);
    else
        bl->scale = 1.0;
    bl->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, bl->surface);
    xdg_surface_add_listener(bl->xdg_surface, &xdg_surface_listener, bl);

    /* The stem's tip on the middle of the edge facing the screen
     * (logical, taskbar-local). */
    int tip_x = (int)((r.x + r.w / 2.0) / bar->scale);
    int edge_y = (int)((bl->down ? r.y + r.h : r.y) / bar->scale);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, bl->width, bl->height);
    xdg_positioner_set_anchor_rect(pos, tip_x, edge_y, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    if (bl->right) {
        xdg_positioner_set_gravity(pos, bl->down ? XDG_POSITIONER_GRAVITY_BOTTOM_LEFT
                                                 : XDG_POSITIONER_GRAVITY_TOP_LEFT);
        xdg_positioner_set_offset(pos, STEM_X, 0);
    } else {
        xdg_positioner_set_gravity(pos, bl->down ? XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT
                                                 : XDG_POSITIONER_GRAVITY_TOP_RIGHT);
        xdg_positioner_set_offset(pos, -STEM_X, 0);
    }
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
    /* Taken out first: show() hides any other balloon, and with it what
     * waits here. */
    owned c = waiting;
    waiting = (owned){0};
    unsigned self = pending;
    pending = 0;
    content v = content_view(&c);
    bool start_open = c.item == nullptr && (shell->start_menu != nullptr ||
                                            shell->start7 != nullptr);
    if (start_open || show(shell, &v) || --retries <= 0) {
        content_clear(&c);
        return G_SOURCE_REMOVE;
    }
    waiting = c;
    pending = self;
    return G_SOURCE_CONTINUE;
}

static void show_or_wait(xd_shell *shell, const content *c) {
    if (show(shell, c))
        return;
    if (pending != 0) {
        g_source_remove(pending);
        content_clear(&waiting);
    }
    waiting = content_copy(c);
    retries = RETRIES;
    pending = g_timeout_add_seconds(RETRY_SECONDS, retry, shell);
}

void xd_balloon_new_programs(xd_shell *shell) {
    /* XP's own: with its Start menu, while it lights new programs. */
    if (shell->settings.start_style != XD_START_XP || !shell->settings.start_highlight_new)
        return;
    /* Not over an open Start menu, which shows them already. */
    if (shell->start_menu != nullptr || shell->start7 != nullptr)
        return;
    if (pending != 0 && waiting.item == nullptr)
        return;
    content c = {.text = TEXT};
    show_or_wait(shell, &c);
}

void xd_balloon_tray(xd_shell *shell, const char *item, const char *icon, const char *title,
                     const char *text, void (*clicked)(xd_shell *shell, void *data),
                     void *data) {
    content c = {item, icon, title, text, clicked, data};
    show_or_wait(shell, &c);
}

void xd_balloon_hide_tray(xd_shell *shell, const char *item) {
    bool mine = (shell->balloon != nullptr && g_strcmp0(shell->balloon->c.item, item) == 0) ||
                (pending != 0 && g_strcmp0(waiting.item, item) == 0);
    if (mine)
        xd_balloon_hide(shell);
}

bool xd_balloon_owns(const xd_shell *shell, struct wl_surface *surface) {
    return shell->balloon != nullptr && surface != nullptr && shell->balloon->surface == surface;
}

void xd_balloon_pointer_motion(xd_shell *shell, double x, double y) {
    if (shell->balloon != nullptr) {
        shell->balloon->px = x;
        shell->balloon->py = y;
    }
}

void xd_balloon_button(xd_shell *shell, bool pressed) {
    xd_balloon *b = shell->balloon;
    if (pressed || b == nullptr)
        return;
    /* The close box only closes it; anywhere else does what the icon
     * would, as in XP. */
    double cx, cy;
    close_box(b, &cx, &cy);
    bool on_close = fabs(b->px - cx) <= CLOSE / 2.0 + 2 && fabs(b->py - cy) <= CLOSE / 2.0 + 2;
    void (*clicked)(xd_shell *, void *) = on_close ? nullptr : b->c.clicked;
    void *data = b->c.data;
    xd_balloon_hide(shell);
    if (clicked != nullptr)
        clicked(shell, data);
}

void xd_balloon_render_dirty(xd_shell *shell) {
    xd_balloon *b = shell->balloon;
    if (b != nullptr && b->configured && b->dirty)
        render(b);
}
