/* SPDX-License-Identifier: MIT
 * Windows XP's balloon tips, in the InfoWindow colours:
 *   - "New programs installed": once programs appear that weren't there
 *     before, it points at the Start button, where All Programs now
 *     lights them;
 *   - a notification area icon's: an icon, a bold title and a line of
 *     text pointing at the icon ("Updates are ready for your computer");
 *     a click on it does what the icon would;
 *   - a program's notification (notifyd.c): the same, its text wrapped
 *     over a few lines, its picture at the left and its actions as links
 *     under the text.
 * It goes on a click, when the Start menu opens, or after a while (not
 * while the pointer is over it). A popup of the primary taskbar's layer
 * surface, without a grab.
 */
#include <glib.h>
#include <math.h>
#include <pango/pangocairo.h>
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
/* A notification's: its picture, the balloon's width, the lines of text
 * shown and the space between its action links. */
static constexpr int IMAGE = 48;
static constexpr int IMAGE_GAP = 8;
static constexpr int NOTICE_MIN_W = 220;
static constexpr int NOTICE_MAX_W = 360;
static constexpr int BODY_LINES = 6;
static constexpr int BUTTON_GAP = 14;
enum { BUTTONS_MAX = 8 };
/* How long it stays (XP's balloons stay at least ten seconds). */
static constexpr unsigned SHOW_SECONDS = 15;
/* Once the pointer has left it, at least this long. */
static constexpr unsigned AFTER_HOVER_SECONDS = 3;
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
    /* A notification's (xd_balloon_notify). */
    bool notice;
    cairo_surface_t *image;
    const char *const *buttons;
    size_t button_count;
    unsigned seconds;
    void (*ended)(xd_shell *shell, xd_balloon_end end, size_t button, void *data);
} content;

/* The same, kept (copies of the strings, a reference to the picture). */
typedef struct owned {
    char *item, *icon, *title, *text;
    void (*clicked)(xd_shell *shell, void *data);
    void *data;
    bool notice;
    cairo_surface_t *image;
    char **buttons;
    size_t button_count;
    unsigned seconds;
    void (*ended)(xd_shell *shell, xd_balloon_end end, size_t button, void *data);
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
    bool hovered;
    unsigned timeout; /* GLib source */
    /* A notification's layout (logical, the stem's side excluded). */
    int text_x;       /* where the title and text start: right of the picture */
    int body_h;
    xd_rect button_rects[BUTTONS_MAX];
} xd_balloon;

/* A balloon waiting for a taskbar (at startup). */
static unsigned pending;
static int retries;
static owned waiting;

static void content_clear(owned *tip) {
    g_free(tip->item);
    g_free(tip->icon);
    g_free(tip->title);
    g_free(tip->text);
    if (tip->image != nullptr)
        cairo_surface_destroy(tip->image);
    g_strfreev(tip->buttons);
    *tip = (owned){0};
}

static owned content_copy(const content *tip) {
    owned copy = {
        .item = g_strdup(tip->item),
        .icon = g_strdup(tip->icon),
        .title = g_strdup(tip->title),
        .text = g_strdup(tip->text),
        .clicked = tip->clicked,
        .data = tip->data,
        .notice = tip->notice,
        .image = tip->image != nullptr ? cairo_surface_reference(tip->image) : nullptr,
        .button_count = XD_MIN(tip->button_count, (size_t)BUTTONS_MAX),
        .seconds = tip->seconds,
        .ended = tip->ended,
    };
    copy.buttons = g_new0(char *, copy.button_count + 1);
    for (size_t i = 0; i < copy.button_count; i++)
        copy.buttons[i] = g_strdup(tip->buttons[i]);
    return copy;
}

static content content_view(const owned *tip) {
    return (content){
        .item = tip->item,
        .icon = tip->icon,
        .title = tip->title,
        .text = tip->text,
        .clicked = tip->clicked,
        .data = tip->data,
        .notice = tip->notice,
        .image = tip->image,
        .buttons = (const char *const *)tip->buttons,
        .button_count = tip->button_count,
        .seconds = tip->seconds,
        .ended = tip->ended,
    };
}

/* Tells whoever showed it how it ended, then forgets it. */
static void end_content(xd_shell *shell, owned *tip, xd_balloon_end end, size_t button) {
    owned kept = *tip;
    *tip = (owned){0};
    if (end == XD_BALLOON_CLICKED && kept.clicked != nullptr)
        kept.clicked(shell, kept.data);
    if (kept.ended != nullptr)
        kept.ended(shell, end, button, kept.data);
    content_clear(&kept);
}

static xd_theme theme_at(const xd_shell *shell, double scale) {
    xd_theme theme;
    xd_theme_init(&theme, shell->style, &shell->scheme, scale);
    return theme;
}

/* The stem's upright edge (tip) and its slanted end (base), logical. */
static void stem_x(const xd_balloon *balloon, double *tip, double *base) {
    *tip = balloon->right ? balloon->width - STEM_X : STEM_X;
    *base = balloon->right ? *tip - STEM_W : *tip + STEM_W;
}

/* The outline: a rounded box with the stem on the side facing what it
 * points at (device px). */
static void outline(cairo_t *cairo, const xd_balloon *balloon, double scale) {
    double width = balloon->width * scale, height = balloon->height * scale;
    double stem = STEM_H * scale, radius = RADIUS * scale;
    double top = balloon->down ? stem : 0, bottom = balloon->down ? height : height - stem;
    double tip, base;
    stem_x(balloon, &tip, &base);
    tip *= scale;
    base *= scale;
    double inset = 0.5 * XD_MAX(round(scale), 1);
    top += inset;
    bottom -= inset;
    double left = inset, right = width - inset;
    /* Clockwise from the top left: along the top left to right, the
     * bottom right to left; the stem's two ends in that order. */
    double first = XD_MIN(tip, base), second = XD_MAX(tip, base);
    cairo_new_path(cairo);
    cairo_move_to(cairo, left + radius, top);
    if (balloon->down) {
        cairo_line_to(cairo, first, top);
        cairo_line_to(cairo, tip, inset);
        cairo_line_to(cairo, second, top);
    }
    cairo_arc(cairo, right - radius, top + radius, radius, -XD_PI / 2, 0);
    cairo_arc(cairo, right - radius, bottom - radius, radius, 0, XD_PI / 2);
    if (!balloon->down) {
        cairo_line_to(cairo, second, bottom);
        cairo_line_to(cairo, tip, height - inset);
        cairo_line_to(cairo, first, bottom);
    }
    cairo_arc(cairo, left + radius, bottom - radius, radius, XD_PI / 2, XD_PI);
    cairo_arc(cairo, left + radius, top + radius, radius, XD_PI, 3 * XD_PI / 2);
    cairo_close_path(cairo);
}

/* A notification's wrapped text, `width` wide (device px), at most
 * BODY_LINES lines. g_object_unref. */
static PangoLayout *body_layout(cairo_t *cairo, const char *text, const xd_text_style *style,
                                int width) {
    PangoLayout *layout = xd_text_layout(cairo, text, style);
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_height(layout, -BODY_LINES);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    return layout;
}

/* A notification's picture, fit into a square of `side` at (x, y),
 * device px. */
static void paint_image(cairo_t *cairo, cairo_surface_t *image, double x, double y, double side) {
    int width = cairo_image_surface_get_width(image), height = cairo_image_surface_get_height(image);
    if (width <= 0 || height <= 0)
        return;
    double fit = XD_MIN(side / width, side / height);
    cairo_save(cairo);
    cairo_translate(cairo, x + (side - width * fit) / 2, y + (side - height * fit) / 2);
    cairo_scale(cairo, fit, fit);
    cairo_set_source_surface(cairo, image, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_GOOD);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

/* The close box's centre (logical, surface-local). */
static void close_box(const xd_balloon *balloon, double *cx, double *cy) {
    int top = balloon->down ? STEM_H : 0;
    *cx = balloon->width - PAD + 4 - CLOSE / 2.0;
    *cy = top + PAD + TEXT_H / 2.0;
}

static void render(xd_balloon *balloon) {
    xd_shell *shell = balloon->shell;
    double fw = ceil(balloon->width * balloon->scale), fh = ceil(balloon->height * balloon->scale);
    int dw = (int)fw, dh = (int)fh;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&balloon->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme theme = theme_at(shell, balloon->scale);
    double scale = balloon->scale;
    cairo_t *cairo = cairo_create(buf->surface);
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);

    outline(cairo, balloon, scale);
    xd_set_color(cairo, xd_sys(&theme, XD_COLOR_INFO_WINDOW));
    cairo_fill_preserve(cairo);
    cairo_set_line_width(cairo, XD_MAX(round(scale), 1));
    xd_set_color(cairo, xd_sys(&theme, XD_COLOR_INFO_TEXT));
    cairo_stroke(cairo);

    int top = balloon->down ? STEM_H : 0;
    xd_rgba ink = xd_sys(&theme, XD_COLOR_INFO_TEXT);
    xd_text_style st = xd_text(&theme, XD_FONT_TOOLTIP, ink, XD_ALIGN_START);
    int text_w = balloon->width - 2 * PAD - CLOSE - 4;
    if (balloon->c.notice) {
        /* The picture at the left; the icon and the title in bold, the
         * text wrapped under, the actions as links under that. */
        if (balloon->c.image != nullptr)
            paint_image(cairo, balloon->c.image, xd_px(scale, PAD), xd_px(scale, top + PAD),
                        xd_px(scale, IMAGE));
        int x = balloon->text_x;
        cairo_surface_t *icon =
            balloon->c.icon != nullptr
                ? xd_icons_get(shell->icons, balloon->c.icon, xd_px(scale, ICON))
                : nullptr;
        if (icon != nullptr && balloon->c.image == nullptr) {
            cairo_set_source_surface(cairo, icon, xd_px(scale, x),
                                     xd_px(scale, top + PAD + (TEXT_H - ICON) / 2));
            cairo_paint(cairo);
            x += ICON + 6;
        }
        xd_font_spec bold = *st.font;
        bold.bold = true;
        xd_text_style bst = st;
        bst.font = &bold;
        bst.ellipsize = true;
        int title_right = balloon->width - PAD - CLOSE - 4;
        xd_draw_text(cairo,
                     (xd_rect){xd_px(scale, x), xd_px(scale, top + PAD),
                               xd_px(scale, title_right - x), xd_px(scale, TEXT_H)},
                     balloon->c.title, &bst);
        int body_y = top + PAD + TEXT_H + GAP;
        if (balloon->body_h > 0) {
            PangoLayout *layout =
                body_layout(cairo, balloon->c.text, &st,
                            xd_px(scale, balloon->width - PAD - balloon->text_x));
            xd_set_color(cairo, ink);
            cairo_move_to(cairo, xd_px(scale, balloon->text_x), xd_px(scale, body_y));
            pango_cairo_show_layout(cairo, layout);
            g_object_unref(layout);
        }
        xd_rgba link = xd_sys(&theme, XD_COLOR_HOT_TRACKING);
        xd_text_style lst = xd_text(&theme, XD_FONT_TOOLTIP, link, XD_ALIGN_START);
        for (size_t i = 0; i < balloon->c.button_count; i++) {
            xd_rect rect = balloon->button_rects[i];
            rect.y += top;
            xd_rect device = {xd_px(scale, rect.x), xd_px(scale, rect.y), xd_px(scale, rect.w),
                              xd_px(scale, rect.h)};
            xd_draw_text(cairo, device, balloon->c.buttons[i], &lst);
            int line = XD_MAX(xd_px(scale, 1), 1);
            xd_fill_rect(cairo,
                         (xd_rect){device.x, device.y + device.h - xd_px(scale, 2), device.w, line},
                         link);
        }
    } else if (balloon->c.title != nullptr) {
        /* XP's: the icon, the title in bold beside it, the text under. */
        int x = PAD;
        cairo_surface_t *icon =
            balloon->c.icon != nullptr
                ? xd_icons_get(shell->icons, balloon->c.icon, xd_px(scale, ICON))
                : nullptr;
        if (icon != nullptr) {
            cairo_set_source_surface(cairo, icon, xd_px(scale, PAD),
                                     xd_px(scale, top + PAD + (TEXT_H - ICON) / 2));
            cairo_paint(cairo);
            x += ICON + 6;
        }
        xd_font_spec bold = *st.font;
        bold.bold = true;
        xd_text_style bst = st;
        bst.font = &bold;
        xd_draw_text(cairo,
                     (xd_rect){xd_px(scale, x), xd_px(scale, top + PAD),
                               xd_px(scale, text_w - (x - PAD)), xd_px(scale, TEXT_H)},
                     balloon->c.title, &bst);
        xd_draw_text(cairo,
                     (xd_rect){xd_px(scale, PAD), xd_px(scale, top + PAD + TEXT_H + GAP),
                               xd_px(scale, balloon->width - 2 * PAD), xd_px(scale, TEXT_H)},
                     balloon->c.text, &st);
    } else {
        xd_draw_text(cairo,
                     (xd_rect){xd_px(scale, PAD), xd_px(scale, top + PAD), xd_px(scale, text_w),
                               xd_px(scale, TEXT_H)},
                     balloon->c.text, &st);
    }
    /* The close box's cross, top right. */
    double cx, cy;
    close_box(balloon, &cx, &cy);
    cx *= scale;
    cy *= scale;
    double arm = 3.5 * scale;
    xd_set_color(cairo, ink);
    cairo_set_line_width(cairo, XD_MAX(round(1.5 * scale), 1));
    cairo_move_to(cairo, cx - arm, cy - arm);
    cairo_line_to(cairo, cx + arm, cy + arm);
    cairo_move_to(cairo, cx + arm, cy - arm);
    cairo_line_to(cairo, cx - arm, cy + arm);
    cairo_stroke(cairo);

    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(balloon->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(balloon->surface, 0, 0, dw, dh);
    if (balloon->viewport != nullptr)
        wp_viewport_set_destination(balloon->viewport, balloon->width, balloon->height);
    wl_surface_commit(balloon->surface);
    buf->busy = true;
    balloon->dirty = false;
}

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    xd_balloon *balloon = data;
    xdg_surface_ack_configure(surface, serial);
    balloon->configured = true;
    balloon->dirty = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *popup,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *popup) {
    xd_balloon_hide(((xd_balloon *)data)->shell);
}

static void popup_repositioned([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_popup *popup,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

/* Puts the balloon away (and one waiting for a taskbar), then tells
 * whoever showed them how they ended: they may show another. */
static void finish(xd_shell *shell, xd_balloon_end end, size_t button) {
    owned dropped = {0};
    if (pending != 0) {
        g_source_remove(pending);
        pending = 0;
        dropped = waiting;
        waiting = (owned){0};
    }
    owned shown = {0};
    xd_balloon *balloon = shell->balloon;
    if (balloon != nullptr) {
        shell->balloon = nullptr;
        shell->pointer_balloon = false;
        if (balloon->timeout != 0)
            g_source_remove(balloon->timeout);
        xdg_popup_destroy(balloon->popup);
        xdg_surface_destroy(balloon->xdg_surface);
        if (balloon->viewport != nullptr)
            wp_viewport_destroy(balloon->viewport);
        wl_surface_destroy(balloon->surface);
        xd_shm_ring_finish(&balloon->ring);
        shown = balloon->c;
        xd_free(balloon);
    }
    bool any = balloon != nullptr || dropped.text != nullptr;
    end_content(shell, &shown, end, button);
    end_content(shell, &dropped, XD_BALLOON_HIDDEN, 0);
    if (any)
        xd_notifyd_balloon_gone(shell);
}

static gboolean timed_out(gpointer data) {
    xd_shell *shell = data;
    xd_balloon *balloon = shell->balloon;
    if (balloon == nullptr)
        return G_SOURCE_REMOVE;
    if (balloon->hovered) {
        /* Not while it's being read. */
        balloon->timeout = g_timeout_add_seconds(AFTER_HOVER_SECONDS, timed_out, shell);
        return G_SOURCE_REMOVE;
    }
    balloon->timeout = 0;
    finish(shell, XD_BALLOON_TIMED_OUT, 0);
    return G_SOURCE_REMOVE;
}

void xd_balloon_hide(xd_shell *shell) { finish(shell, XD_BALLOON_HIDDEN, 0); }

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
static bool show(xd_shell *shell, const content *tip) {
    xd_taskbar *bar = nullptr, *candidate;
    xd_rect rect = {0};
    wl_list_for_each(candidate, &shell->taskbars, link) {
        if (!candidate->configured || candidate->layer == nullptr)
            continue;
        if (tip->item != nullptr ? tray_anchor(candidate, tip->item, &rect)
                                 : candidate->start_rect.w > 0) {
            bar = candidate;
            break;
        }
    }
    if (bar == nullptr || shell->wm_base == nullptr)
        return false;
    if (tip->item == nullptr)
        rect = bar->start_rect;
    xd_balloon_hide(shell);
    xd_balloon *bl = XD_NEW(xd_balloon);
    bl->shell = shell;
    bl->c = content_copy(tip);
    bl->scale = bar->scale;
    bl->down = shell->settings.edge != XD_EDGE_BOTTOM;
    if (tip->item != nullptr) {
        /* The notification area is at the right end (or the bottom of a
         * side taskbar, where the balloon goes up). */
        bl->down = shell->settings.edge == XD_EDGE_TOP;
        bl->right = shell->settings.edge != XD_EDGE_LEFT;
    }

    xd_theme t1 = theme_at(shell, 1.0);
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(probe);
    xd_text_style st = xd_text(&t1, XD_FONT_TOOLTIP, xd_rgba_hex(0), XD_ALIGN_START);
    int text_w = xd_text_width(cairo, tip->text, &st);
    if (tip->notice) {
        /* As wide as its title, a line of its text or its links need, in
         * bounds; the text wrapped to that. */
        bl->text_x = PAD + (tip->image != nullptr ? IMAGE + IMAGE_GAP : 0);
        xd_font_spec bold = *st.font;
        bold.bold = true;
        xd_text_style bst = st;
        bst.font = &bold;
        int title_w = (tip->image == nullptr && tip->icon != nullptr ? ICON + 6 : 0) +
                      xd_text_width(cairo, tip->title, &bst) + 4 + CLOSE;
        int buttons_w = 0;
        for (size_t i = 0; i < bl->c.button_count; i++)
            buttons_w += (i > 0 ? BUTTON_GAP : 0) + xd_text_width(cairo, tip->buttons[i], &st);
        int natural = bl->text_x + XD_MAX(XD_MAX(title_w, text_w), buttons_w) + PAD;
        bl->width = XD_CLAMP(natural, NOTICE_MIN_W, NOTICE_MAX_W);
        bl->body_h = 0;
        if (tip->text != nullptr && tip->text[0] != '\0') {
            PangoLayout *layout = body_layout(cairo, tip->text, &st, bl->width - PAD - bl->text_x);
            int layout_w = 0;
            pango_layout_get_pixel_size(layout, &layout_w, &bl->body_h);
            g_object_unref(layout);
        }
        int y = PAD + TEXT_H + GAP + bl->body_h;
        if (bl->c.button_count > 0) {
            y += GAP;
            int x = bl->text_x;
            for (size_t i = 0; i < bl->c.button_count; i++) {
                int width = XD_MIN(xd_text_width(cairo, tip->buttons[i], &st),
                                   XD_MAX(bl->width - PAD - x, 0));
                bl->button_rects[i] = (xd_rect){x, y, width, TEXT_H};
                x += width + BUTTON_GAP;
            }
            y += TEXT_H;
        }
        int picture_bottom = tip->image != nullptr ? PAD + IMAGE : 0;
        bl->height = XD_MAX(y, picture_bottom) + PAD + STEM_H;
    } else if (tip->title != nullptr) {
        xd_font_spec bold = *st.font;
        bold.bold = true;
        xd_text_style bst = st;
        bst.font = &bold;
        int title_w =
            (tip->icon != nullptr ? ICON + 6 : 0) + xd_text_width(cairo, tip->title, &bst);
        bl->width = PAD + XD_MAX(title_w + 4 + CLOSE, text_w) + PAD;
        bl->height = PAD + TEXT_H + GAP + TEXT_H + PAD + STEM_H;
    } else {
        bl->width = PAD + text_w + 4 + CLOSE + PAD;
        bl->height = PAD + TEXT_H + PAD + STEM_H;
    }
    cairo_destroy(cairo);
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
    int tip_x = (int)((rect.x + rect.w / 2.0) / bar->scale);
    int edge_y = (int)((bl->down ? rect.y + rect.h : rect.y) / bar->scale);
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
    unsigned seconds = tip->notice ? tip->seconds : SHOW_SECONDS;
    if (seconds > 0)
        bl->timeout = g_timeout_add_seconds(seconds, timed_out, shell);
    shell->balloon = bl;
    return true;
}

static gboolean retry(gpointer data) {
    xd_shell *shell = data;
    /* Taken out first: show() hides any other balloon, and with it what
     * waits here. */
    owned kept = waiting;
    waiting = (owned){0};
    unsigned self = pending;
    pending = 0;
    content tip = content_view(&kept);
    bool start_open =
        kept.item == nullptr && (shell->start_menu != nullptr || shell->start7 != nullptr);
    if (start_open) {
        end_content(shell, &kept, XD_BALLOON_HIDDEN, 0);
        return G_SOURCE_REMOVE;
    }
    if (show(shell, &tip)) {
        content_clear(&kept);
        return G_SOURCE_REMOVE;
    }
    if (start_open || --retries <= 0) {
        end_content(shell, &kept, XD_BALLOON_HIDDEN, 0);
        xd_notifyd_balloon_gone(shell);
        return G_SOURCE_REMOVE;
    }
    waiting = kept;
    pending = self;
    return G_SOURCE_CONTINUE;
}

static void show_or_wait(xd_shell *shell, const content *tip) {
    if (show(shell, tip))
        return;
    if (pending != 0) {
        g_source_remove(pending);
        pending = 0;
        end_content(shell, &waiting, XD_BALLOON_HIDDEN, 0);
    }
    waiting = content_copy(tip);
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
    content tip = {.text = TEXT};
    show_or_wait(shell, &tip);
}

void xd_balloon_tray(xd_shell *shell, const char *item, const char *icon, const char *title,
                     const char *text, void (*clicked)(xd_shell *shell, void *data),
                     void *data) {
    content tip = {
        .item = item, .icon = icon, .title = title, .text = text, .clicked = clicked, .data = data};
    show_or_wait(shell, &tip);
}

void xd_balloon_notify(xd_shell *shell, const xd_balloon_notice *notice) {
    content tip = {
        .item = notice->item,
        .icon = notice->icon,
        .title = notice->title != nullptr ? notice->title : "",
        .text = notice->text != nullptr ? notice->text : "",
        .notice = true,
        .image = notice->image,
        .buttons = notice->buttons,
        .button_count = notice->button_count,
        .seconds = notice->seconds,
        .ended = notice->ended,
        .data = notice->data,
    };
    show_or_wait(shell, &tip);
}

bool xd_balloon_busy(const xd_shell *shell) { return shell->balloon != nullptr || pending != 0; }

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
        shell->balloon->hovered = true;
    }
}

void xd_balloon_pointer_leave(xd_shell *shell) {
    if (shell->balloon != nullptr)
        shell->balloon->hovered = false;
}

void xd_balloon_button(xd_shell *shell, bool pressed) {
    xd_balloon *balloon = shell->balloon;
    if (pressed || balloon == nullptr)
        return;
    /* The close box only closes it; anywhere else does what the icon
     * would, as in XP. */
    double cx, cy;
    close_box(balloon, &cx, &cy);
    bool on_close =
        fabs(balloon->px - cx) <= CLOSE / 2.0 + 2 && fabs(balloon->py - cy) <= CLOSE / 2.0 + 2;
    if (on_close) {
        finish(shell, XD_BALLOON_CLOSED, 0);
        return;
    }
    int top = balloon->down ? STEM_H : 0;
    for (size_t i = 0; i < balloon->c.button_count; i++) {
        xd_rect rect = balloon->button_rects[i];
        if (balloon->px >= rect.x && balloon->px < rect.x + rect.w &&
            balloon->py >= rect.y + top && balloon->py < rect.y + top + rect.h) {
            finish(shell, XD_BALLOON_ACTION, i);
            return;
        }
    }
    finish(shell, XD_BALLOON_CLICKED, 0);
}

void xd_balloon_render_dirty(xd_shell *shell) {
    xd_balloon *balloon = shell->balloon;
    if (balloon != nullptr && balloon->configured && balloon->dirty)
        render(balloon);
}
