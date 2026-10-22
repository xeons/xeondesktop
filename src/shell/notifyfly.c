/* SPDX-License-Identifier: MIT
 * The notification history: a click on the notifications' icon in the
 * notification area shows the ones kept, newest first, as Windows 10's
 * Action Center lists them, in the style's colours: each with its icon or
 * picture, its program and time, its title and some of its text. Unread
 * ones are marked at the left until it closes.
 *
 * A click on one does what a click on its balloon would (its program
 * told, or opened) and closes the history; its close box (shown while
 * the pointer is over it) puts it away. "Clear all" at the top puts them
 * all away; the band at the bottom turns Do Not Disturb on or off. The
 * wheel scrolls; Esc closes it.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <glib.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "shell.h"

/* Logical px. */
static constexpr int FLY_W = 340;
static constexpr int FLY_H = 420;
static constexpr int HEAD_H = 34;
static constexpr int BAND_H = 36;
static constexpr int LIST_Y = HEAD_H;
static constexpr int LIST_H = FLY_H - HEAD_H - BAND_H;
static constexpr int SIDE = 12;  /* the left and right margins */
static constexpr int ROW_PAD = 8; /* above and below a notification's text */
static constexpr int ICON = 32;
static constexpr int ICON_GAP = 10;
static constexpr int LINE_H = 16;
static constexpr int BODY_LINES = 3;
static constexpr int CLOSE = 16;
static constexpr int UNREAD_W = 3;
static constexpr double WHEEL_PX = 3.0; /* per wheel unit (a notch is 10) */
enum { ROWS_MAX = 64 };

/* What the pointer is over: a row (0 ...), or one of these. */
typedef enum part {
    PART_NONE = -1,
    PART_CLEAR = -2, /* "Clear all" */
    PART_QUIET = -3, /* the band: Do Not Disturb */
} part;

typedef struct row {
    uint32_t id;
    int y, height; /* logical, in the list (before scrolling) */
    int body_h;
} row;

typedef struct flyout {
    xd_shell *shell;
    xd_taskbar *bar; /* has the keyboard while we're open */
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    double scale;
    bool configured, dirty;
    int hot, pressed;
    bool hot_close, pressed_close; /* on the hot row's close box */
    double scroll;                 /* logical px the list is scrolled by */
    row rows[ROWS_MAX];
    size_t row_count;
    int list_h; /* all the rows' height */
} flyout;

static flyout *the_flyout;
static int64_t closed_ms; /* when it last closed: the click that closed it doesn't reopen it */

/* ---- layout ------------------------------------------------------------------------ */

static xd_rect clear_rect(void) { return (xd_rect){FLY_W - SIDE - 70, 0, 70, HEAD_H}; }
static xd_rect band_rect(void) { return (xd_rect){0, FLY_H - BAND_H, FLY_W, BAND_H}; }
static int text_x(void) { return SIDE + ICON + ICON_GAP; }
static int text_w(void) { return FLY_W - text_x() - SIDE - CLOSE; }

static bool inside(xd_rect rect, double x, double y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

static xd_rect device(xd_rect rect, double scale) {
    return (xd_rect){(int)lround(rect.x * scale), (int)lround(rect.y * scale),
                     (int)lround(rect.w * scale), (int)lround(rect.h * scale)};
}

/* A row on the surface (logical), scrolled. */
static xd_rect row_rect(const flyout *fly, const row *item) {
    return (xd_rect){0, LIST_Y + item->y - (int)lround(fly->scroll), FLY_W, item->height};
}

static xd_rect close_rect(const flyout *fly, const row *item) {
    xd_rect rect = row_rect(fly, item);
    return (xd_rect){FLY_W - SIDE - CLOSE, rect.y + ROW_PAD, CLOSE, CLOSE};
}

static void clamp_scroll(flyout *fly) {
    double most = XD_MAX(fly->list_h - LIST_H, 0);
    fly->scroll = XD_CLAMP(fly->scroll, 0.0, most);
}

/* ---- drawing ------------------------------------------------------------------------- */

typedef struct palette {
    xd_rgba body, border, band, band_line, separator;
    xd_rgba text, dim_text, link, hot_row, unread;
} palette;

static palette palette_for(const xd_theme *theme) {
    if (theme->style == XD_STYLE_AERO) {
        return (palette){
            .body = xd_rgba_hex(0xFFFFFF),
            .border = xd_rgba_hex(0x8E8F8F),
            .band = xd_rgba_hex(0xF1F5FB),
            .band_line = xd_rgba_hex(0xCCD9EA),
            .separator = xd_rgba_hex(0xE3E8EE),
            .text = xd_rgba_hex(0x000000),
            .dim_text = xd_rgba_hex(0x6D6D6D),
            .link = xd_rgba_hex(0x0066CC),
            .hot_row = xd_rgba_hex(0xE5F1FB),
            .unread = xd_rgba_hex(0x3399FF),
        };
    }
    xd_rgba window = xd_sys(theme, XD_COLOR_WINDOW);
    xd_rgba hilight = xd_sys(theme, XD_COLOR_HILIGHT);
    return (palette){
        .body = window,
        .border = xd_sys(theme, XD_COLOR_WINDOW_FRAME),
        .band = xd_sys(theme, XD_COLOR_BUTTON_FACE),
        .band_line = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
        .separator = xd_rgba_mix(window, xd_sys(theme, XD_COLOR_BUTTON_SHADOW), 0.4),
        .text = xd_sys(theme, XD_COLOR_WINDOW_TEXT),
        .dim_text = xd_sys(theme, XD_COLOR_GRAY_TEXT),
        .link = xd_sys(theme, XD_COLOR_HOT_TRACKING),
        .hot_row = xd_rgba_mix(window, hilight, 0.15),
        .unread = hilight,
    };
}

/* "3:41 PM" today, the date before. g_free. */
static char *when_text(int64_t time) {
    GDateTime *then = g_date_time_new_from_unix_local(time);
    GDateTime *now = g_date_time_new_now_local();
    if (then == nullptr || now == nullptr) {
        if (then != nullptr)
            g_date_time_unref(then);
        if (now != nullptr)
            g_date_time_unref(now);
        return g_strdup("");
    }
    bool today = g_date_time_get_year(then) == g_date_time_get_year(now) &&
                 g_date_time_get_day_of_year(then) == g_date_time_get_day_of_year(now);
    char *ampm = g_date_time_format(then, "%p");
    bool twelve = ampm != nullptr && ampm[0] != '\0';
    g_free(ampm);
    char *text = g_date_time_format(then, !today ? "%x" : twelve ? "%-l:%M %p" : "%H:%M");
    g_date_time_unref(then);
    g_date_time_unref(now);
    return text != nullptr ? text : g_strdup("");
}

/* A notification's text, wrapped to `width` (device px), at most
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

/* The rows' heights, from their text (logical). */
static void lay_out(flyout *fly, const xd_theme *unscaled) {
    xd_shell *shell = fly->shell;
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(probe);
    xd_text_style st = xd_text(unscaled, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_START);
    fly->row_count = XD_MIN(xd_notifyd_count(shell), (size_t)ROWS_MAX);
    int y = 0;
    for (size_t i = 0; i < fly->row_count; i++) {
        const xd_notice *notice = xd_notifyd_get(shell, i);
        row *item = &fly->rows[i];
        item->id = notice->id;
        item->body_h = 0;
        if (notice->body[0] != '\0') {
            PangoLayout *layout = body_layout(cairo, notice->body, &st, text_w());
            int width = 0;
            pango_layout_get_pixel_size(layout, &width, &item->body_h);
            g_object_unref(layout);
        }
        int text_h = 2 * LINE_H + item->body_h;
        item->y = y;
        item->height = ROW_PAD + XD_MAX(text_h, ICON) + ROW_PAD;
        y += item->height;
    }
    fly->list_h = y;
    clamp_scroll(fly);
    cairo_destroy(cairo);
    cairo_surface_destroy(probe);
}

static void draw_row(flyout *fly, const xd_theme *theme, cairo_t *cairo, const palette *pal,
                     size_t index) {
    xd_shell *shell = fly->shell;
    double scale = fly->scale;
    const row *item = &fly->rows[index];
    const xd_notice *notice = xd_notifyd_get(shell, index);
    if (notice == nullptr)
        return;
    xd_rect rect = row_rect(fly, item);
    bool hot = fly->hot == (int)index;
    if (hot)
        xd_fill_rect(cairo, device(rect, scale), pal->hot_row);
    if (notice->unread)
        xd_fill_rect(cairo, device((xd_rect){0, rect.y, UNREAD_W, rect.h}, scale), pal->unread);
    if (index + 1 < fly->row_count) {
        xd_rect line = device((xd_rect){SIDE, rect.y + rect.h - 1, FLY_W - 2 * SIDE, 1}, scale);
        line.h = XD_MAX(line.h, 1);
        xd_fill_rect(cairo, line, pal->separator);
    }

    /* The picture, or the icon. */
    int icon_y = rect.y + ROW_PAD;
    if (notice->image != nullptr) {
        cairo_surface_t *image = notice->image;
        int width = cairo_image_surface_get_width(image),
            height = cairo_image_surface_get_height(image);
        double side = ICON * scale;
        double fit = XD_MIN(side / XD_MAX(width, 1), side / XD_MAX(height, 1));
        cairo_save(cairo);
        cairo_translate(cairo, SIDE * scale + (side - width * fit) / 2,
                        icon_y * scale + (side - height * fit) / 2);
        cairo_scale(cairo, fit, fit);
        cairo_set_source_surface(cairo, image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_GOOD);
        cairo_paint(cairo);
        cairo_restore(cairo);
    } else {
        const char *names[] = {notice->icon != nullptr ? notice->icon : "dialog-information",
                               "dialog-information"};
        cairo_surface_t *icon = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names),
                                                 xd_px(scale, ICON));
        if (icon != nullptr) {
            cairo_set_source_surface(cairo, icon, xd_px(scale, SIDE), xd_px(scale, icon_y));
            cairo_paint(cairo);
        }
    }

    /* Its program and time; its title; its text. */
    int x = text_x(), width = text_w();
    xd_text_style dim = xd_text(theme, XD_FONT_MENU, pal->dim_text, XD_ALIGN_START);
    dim.ellipsize = true;
    char *when = when_text(notice->time);
    char *source = notice->app_name[0] != '\0' ? g_strdup_printf("%s  ·  %s", notice->app_name, when)
                                               : g_strdup(when);
    xd_draw_text(cairo, device((xd_rect){x, rect.y + ROW_PAD, width, LINE_H}, scale), source, &dim);
    g_free(source);
    g_free(when);
    xd_font_spec bold = theme->scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style title = xd_text(theme, XD_FONT_MENU, pal->text, XD_ALIGN_START);
    title.font = &bold;
    title.ellipsize = true;
    xd_draw_text(cairo, device((xd_rect){x, rect.y + ROW_PAD + LINE_H, width, LINE_H}, scale),
                 notice->summary[0] != '\0' ? notice->summary : notice->app_name, &title);
    if (item->body_h > 0) {
        xd_text_style st = xd_text(theme, XD_FONT_MENU, pal->text, XD_ALIGN_START);
        PangoLayout *layout = body_layout(cairo, notice->body, &st, xd_px(scale, width));
        xd_set_color(cairo, pal->text);
        cairo_move_to(cairo, xd_px(scale, x), xd_px(scale, rect.y + ROW_PAD + 2 * LINE_H));
        pango_cairo_show_layout(cairo, layout);
        g_object_unref(layout);
    }

    /* Its close box, while the pointer's over it. */
    if (hot) {
        xd_rect box = close_rect(fly, item);
        double cx = (box.x + box.w / 2.0) * scale, cy = (box.y + box.h / 2.0) * scale;
        double arm = 3.5 * scale;
        xd_set_color(cairo, fly->hot_close ? pal->text : pal->dim_text);
        cairo_set_line_width(cairo, XD_MAX(round(1.5 * scale), 1));
        cairo_new_path(cairo);
        cairo_move_to(cairo, cx - arm, cy - arm);
        cairo_line_to(cairo, cx + arm, cy + arm);
        cairo_move_to(cairo, cx + arm, cy - arm);
        cairo_line_to(cairo, cx - arm, cy + arm);
        cairo_stroke(cairo);
    }
}

static void draw_link(cairo_t *cairo, const xd_theme *theme, xd_rect rect, const char *text,
                      xd_rgba color, xd_halign align, bool hot) {
    xd_text_style st = xd_text(theme, XD_FONT_MENU, color, align);
    xd_draw_text(cairo, rect, text, &st);
    if (!hot)
        return;
    int width = XD_MIN(xd_text_width(cairo, text, &st), rect.w);
    int x = align == XD_ALIGN_END      ? rect.x + rect.w - width
            : align == XD_ALIGN_CENTER ? rect.x + (rect.w - width) / 2
                                       : rect.x;
    xd_fill_rect(cairo, (xd_rect){x, rect.y + rect.h / 2 + xd_tpx(theme, 6), width, xd_tpx(theme, 1)},
                 color);
}

static void render(flyout *fly) {
    xd_shell *shell = fly->shell;
    double scale = fly->scale;
    double width = ceil(FLY_W * scale), height = ceil(FLY_H * scale);
    int dw = (int)width, dh = (int)height;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&fly->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme theme, unscaled;
    xd_theme_init(&theme, shell->style, &shell->scheme, scale);
    xd_theme_init(&unscaled, shell->style, &shell->scheme, 1.0);
    lay_out(fly, &unscaled);
    palette pal = palette_for(&theme);
    cairo_t *cairo = cairo_create(buf->surface);
    xd_rect all = {0, 0, dw, dh};
    xd_fill_rect(cairo, all, pal.body);
    int px = xd_tpx(&theme, 1);

    /* The list, clipped to its part. */
    xd_rect list = device((xd_rect){0, LIST_Y, FLY_W, LIST_H}, scale);
    cairo_save(cairo);
    cairo_rectangle(cairo, list.x, list.y, list.w, list.h);
    cairo_clip(cairo);
    if (fly->row_count == 0) {
        xd_text_style st = xd_text(&theme, XD_FONT_MENU, pal.dim_text, XD_ALIGN_CENTER);
        xd_draw_text(cairo, device((xd_rect){0, LIST_Y + 24, FLY_W, LINE_H}, scale),
                     "No notifications", &st);
    }
    for (size_t i = 0; i < fly->row_count; i++) {
        xd_rect rect = row_rect(fly, &fly->rows[i]);
        if (rect.y + rect.h > LIST_Y && rect.y < LIST_Y + LIST_H)
            draw_row(fly, &theme, cairo, &pal, i);
    }
    cairo_restore(cairo);

    /* The heading and Clear all, over a line. */
    xd_fill_rect(cairo, device((xd_rect){0, 0, FLY_W, HEAD_H}, scale), pal.body);
    xd_font_spec bold = theme.scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style head = xd_text(&theme, XD_FONT_MENU, pal.text, XD_ALIGN_START);
    head.font = &bold;
    xd_draw_text(cairo, device((xd_rect){SIDE, 0, FLY_W / 2, HEAD_H}, scale), "Notifications",
                 &head);
    if (fly->row_count > 0)
        draw_link(cairo, &theme, device(clear_rect(), scale), "Clear all", pal.link, XD_ALIGN_END,
                  fly->hot == PART_CLEAR);
    xd_fill_rect(cairo, (xd_rect){0, (int)lround(HEAD_H * scale) - px, dw, px}, pal.separator);

    /* The band: Do Not Disturb. */
    xd_rect band = device(band_rect(), scale);
    xd_fill_rect(cairo, band, pal.band);
    xd_fill_rect(cairo, (xd_rect){band.x, band.y, band.w, px}, pal.band_line);
    draw_link(cairo, &theme, band,
              shell->settings.do_not_disturb ? "Turn off Do Not Disturb" : "Turn on Do Not Disturb",
              pal.link, XD_ALIGN_CENTER, fly->hot == PART_QUIET);
    xd_draw_bevel(cairo, all, px, pal.border, pal.border);

    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);
    wl_surface_attach(fly->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(fly->surface, 0, 0, dw, dh);
    if (fly->viewport != nullptr)
        wp_viewport_set_destination(fly->viewport, FLY_W, FLY_H);
    wl_surface_commit(fly->surface);
    buf->busy = true;
    fly->dirty = false;
}

/* ---- the popup ------------------------------------------------------------------- */

static void close_flyout(flyout *fly) {
    if (fly == nullptr)
        return;
    the_flyout = nullptr;
    closed_ms = g_get_monotonic_time() / 1000;
    xdg_popup_destroy(fly->popup);
    xdg_surface_destroy(fly->xdg_surface);
    if (fly->viewport != nullptr)
        wp_viewport_destroy(fly->viewport);
    wl_surface_destroy(fly->surface);
    xd_shm_ring_finish(&fly->ring);
    /* The keyboard back to the windows, unless the taskbar went with its
     * monitor meanwhile. */
    XD_LIST_FOR_EACH(bar, &fly->shell->taskbars, xd_taskbar, link) {
        if (bar == fly->bar && bar->layer != nullptr) {
            zwlr_layer_surface_v1_set_keyboard_interactivity(
                bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
            wl_surface_commit(bar->surface);
            bar->dirty = true;
        }
    }
    xd_shell *shell = fly->shell;
    xd_free(fly);
    /* Seen: none are new any more. */
    xd_notifyd_mark_read(shell);
    xd_taskbars_schedule_tuck(shell);
}

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    flyout *fly = data;
    xdg_surface_ack_configure(surface, serial);
    fly->configured = true;
    fly->dirty = true;
}

static const struct xdg_surface_listener surface_listener = {.configure = surface_configure};

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *popup,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *popup) {
    close_flyout(data); /* a click elsewhere */
}

static void popup_repositioned([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_popup *popup,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

void xd_notify_flyout_toggle(xd_taskbar *bar, xd_rect anchor, uint32_t serial) {
    xd_shell *shell = bar->shell;
    xd_menu_close_all(shell);
    xd_volume_flyout_dismiss(shell);
    xd_clock_flyout_dismiss(shell);
    /* A second click on the icon closes it: the press already did. */
    if (the_flyout != nullptr) {
        close_flyout(the_flyout);
        return;
    }
    if (g_get_monotonic_time() / 1000 - closed_ms < 250)
        return;
    flyout *fly = XD_NEW(flyout);
    fly->shell = shell;
    fly->bar = bar;
    fly->scale = bar->scale;
    fly->hot = fly->pressed = PART_NONE;
    fly->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->viewporter != nullptr)
        fly->viewport = wp_viewporter_get_viewport(shell->viewporter, fly->surface);
    else
        fly->scale = 1.0;
    fly->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, fly->surface);
    xdg_surface_add_listener(fly->xdg_surface, &surface_listener, fly);
    /* Above the icon, its right edge at the icon's; beside a side
     * taskbar. */
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, FLY_W, FLY_H);
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, XD_MAX(anchor.w, 1),
                                   XD_MAX(anchor.h, 1));
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP_LEFT);
    xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                                      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    fly->popup = xdg_surface_get_popup(fly->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(bar->layer, fly->popup);
    xdg_popup_add_listener(fly->popup, &popup_listener, fly);
    /* Esc reaches us only with the keyboard taken before the grab starts
     * (see xd_taskbar_take_keyboard). */
    xd_taskbar_take_keyboard(bar);
    if (shell->seat != nullptr && serial != 0)
        xdg_popup_grab(fly->popup, shell->seat, serial);
    wl_surface_commit(fly->surface);
    the_flyout = fly;
}

bool xd_notify_flyout_owns([[maybe_unused]] xd_shell *shell, struct wl_surface *surface) {
    return the_flyout != nullptr && the_flyout->surface == surface;
}

/* What's at (x, y); *on_close: a row's close box. */
static int part_at(const flyout *fly, double x, double y, bool *on_close) {
    *on_close = false;
    if (inside(band_rect(), x, y))
        return PART_QUIET;
    if (y < LIST_Y)
        return fly->row_count > 0 && inside(clear_rect(), x, y) ? PART_CLEAR : PART_NONE;
    if (y >= LIST_Y + LIST_H)
        return PART_NONE;
    for (size_t i = 0; i < fly->row_count; i++) {
        if (inside(row_rect(fly, &fly->rows[i]), x, y)) {
            xd_rect box = close_rect(fly, &fly->rows[i]);
            *on_close = inside(xd_rect_inset(box, -3, -3), x, y);
            return (int)i;
        }
    }
    return PART_NONE;
}

void xd_notify_flyout_motion([[maybe_unused]] xd_shell *shell, double x, double y) {
    flyout *fly = the_flyout;
    if (fly == nullptr)
        return;
    bool on_close = false;
    int hot = part_at(fly, x, y, &on_close);
    if (hot != fly->hot || on_close != fly->hot_close) {
        fly->hot = hot;
        fly->hot_close = on_close;
        fly->dirty = true;
    }
}

void xd_notify_flyout_leave([[maybe_unused]] xd_shell *shell) {
    flyout *fly = the_flyout;
    if (fly != nullptr && fly->hot != PART_NONE) {
        fly->hot = PART_NONE;
        fly->hot_close = false;
        fly->dirty = true;
    }
}

void xd_notify_flyout_button(xd_shell *shell, uint32_t button, bool pressed, double x, double y) {
    flyout *fly = the_flyout;
    if (fly == nullptr || button != BTN_LEFT)
        return;
    bool on_close = false;
    int at = part_at(fly, x, y, &on_close);
    if (pressed) {
        fly->pressed = at;
        fly->pressed_close = on_close;
        return;
    }
    int target = fly->pressed;
    bool target_close = fly->pressed_close;
    fly->pressed = PART_NONE;
    if (target != at || target_close != on_close)
        return;
    if (target >= 0 && (size_t)target < fly->row_count) {
        uint32_t id = fly->rows[target].id;
        if (on_close) {
            xd_notifyd_dismiss(shell, id);
        } else {
            close_flyout(fly);
            xd_notifyd_activate(shell, id);
        }
    } else if (target == PART_CLEAR) {
        xd_notifyd_dismiss_all(shell);
    } else if (target == PART_QUIET) {
        xd_notifyd_set_do_not_disturb(shell, !shell->settings.do_not_disturb);
    }
}

void xd_notify_flyout_axis([[maybe_unused]] xd_shell *shell, double delta) {
    flyout *fly = the_flyout;
    if (fly == nullptr)
        return;
    double before = fly->scroll;
    fly->scroll += delta * WHEEL_PX;
    clamp_scroll(fly);
    if (fly->scroll != before)
        fly->dirty = true;
}

bool xd_notify_flyout_key([[maybe_unused]] xd_shell *shell, uint32_t sym) {
    flyout *fly = the_flyout;
    if (fly == nullptr)
        return false;
    switch (sym) {
    case XKB_KEY_Escape:
        close_flyout(fly);
        break;
    case XKB_KEY_Up:
    case XKB_KEY_KP_Up:
        xd_notify_flyout_axis(shell, -10);
        break;
    case XKB_KEY_Down:
    case XKB_KEY_KP_Down:
        xd_notify_flyout_axis(shell, 10);
        break;
    default:
        break; /* others are the history's too while it's open */
    }
    return true;
}

void xd_notify_flyout_dismiss([[maybe_unused]] xd_shell *shell) { close_flyout(the_flyout); }

void xd_notify_flyout_changed([[maybe_unused]] xd_shell *shell) {
    if (the_flyout != nullptr)
        the_flyout->dirty = true;
}

void xd_notify_flyout_render_dirty([[maybe_unused]] xd_shell *shell) {
    flyout *fly = the_flyout;
    if (fly != nullptr && fly->configured && fly->dirty)
        render(fly);
}
