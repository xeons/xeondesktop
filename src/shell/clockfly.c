/* SPDX-License-Identifier: MIT
 * Windows 7's clock flyout: clicking the unified taskbar's clock shows
 * today's date, a month calendar and an analog clock above it, with
 * "Change date and time settings..." (Date and Time) in a band at the
 * bottom. XP's taskbar keeps XP's double-click, straight to Date and Time.
 *
 * Aero draws it as 7 did (from memory); the other styles as their own
 * month calendar (XP's MonthCal: the month in a caption-coloured bar,
 * today ringed in red, the chosen day in the selection colour).
 *
 * The calendar takes the keyboard: the arrows move the chosen day a day
 * or a week, Page Up and Page Down a month, Home goes back to today and
 * Esc closes it. The month's arrows and the date at the top (today) are
 * clicked too.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <glib.h>

#include "../core/alloc.h"
#include "../theme/draw.h"
#include "calendar.h"
#include "shell.h"

/* Logical px. */
static constexpr int FLY_W = 352;
static constexpr int FLY_H = 262;
static constexpr int DATE_Y = 10;
static constexpr int DATE_H = 18;
static constexpr int CAL_X = 12;
static constexpr int CAL_Y = 38;
static constexpr int CELL_W = 28;
static constexpr int CELL_H = 20;
static constexpr int CAL_W = 7 * CELL_W;
static constexpr int HEAD_H = 24;
static constexpr int WEEKDAYS_H = 20;
static constexpr int CLOCK_R = 62;
static constexpr int BAND_H = 38;

/* What the pointer is over. */
typedef enum part {
    PART_NONE = -1,
    PART_TODAY = -2, /* the date at the top */
    PART_PREVIOUS = -3, /* the month's arrows */
    PART_NEXT = -4,
    PART_SETTINGS = -5, /* "Change date and time settings..." */
    /* 0 .. XD_CALENDAR_CELLS - 1: a day */
} part;

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
    xd_day chosen; /* the day picked, whose month shows */
    guint tick; /* the second hand */
} flyout;

static flyout *the_flyout;
static int64_t closed_ms; /* when it last closed: the click that closed it doesn't reopen it */

/* ---- time ------------------------------------------------------------------------ */

/* Now, or noon of today with XD_CLOCK_FIXED (screenshots and tests). */
static struct tm now_tm(void) {
    time_t now = time(nullptr);
    struct tm tm = {0};
    localtime_r(&now, &tm);
    const char *fixed = getenv("XD_CLOCK_FIXED");
    if (fixed != nullptr && fixed[0] != '\0') {
        tm.tm_hour = 12;
        tm.tm_min = 0;
        tm.tm_sec = 0;
    }
    return tm;
}

static xd_day today(void) {
    struct tm tm = now_tm();
    return (xd_day){tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

/* "9:41:05 PM", or 24-hour where the locale has no AM/PM. */
static void time_text(const struct tm *tm, char *out, size_t size) {
    char ampm[8];
    bool twelve = strftime(ampm, sizeof(ampm), "%p", tm) > 0;
    if (strftime(out, size, twelve ? "%I:%M:%S %p" : "%H:%M:%S", tm) == 0)
        out[0] = '\0';
    if (twelve && out[0] == '0')
        memmove(out, out + 1, strlen(out));
}

static char *format_day(xd_day day, const char *format) {
    GDateTime *date = g_date_time_new_local(day.year, day.month, day.day, 12, 0, 0);
    char *text = date != nullptr ? g_date_time_format(date, format) : nullptr;
    if (date != nullptr)
        g_date_time_unref(date);
    return text != nullptr ? text : g_strdup("");
}

/* ---- layout ------------------------------------------------------------------------ */

static xd_rect cell_rect(int cell) {
    return (xd_rect){CAL_X + (cell % 7) * CELL_W, CAL_Y + HEAD_H + WEEKDAYS_H + (cell / 7) * CELL_H,
                     CELL_W, CELL_H};
}
static xd_rect previous_rect(void) { return (xd_rect){CAL_X, CAL_Y, 24, HEAD_H}; }
static xd_rect next_rect(void) { return (xd_rect){CAL_X + CAL_W - 24, CAL_Y, 24, HEAD_H}; }
static xd_rect date_rect(void) { return (xd_rect){0, DATE_Y, FLY_W, DATE_H}; }
static xd_rect band_rect(void) { return (xd_rect){0, FLY_H - BAND_H, FLY_W, BAND_H}; }
static int clock_cx(void) { return CAL_X + CAL_W + (FLY_W - CAL_X - CAL_W) / 2; }
static int clock_cy(void) { return CAL_Y + 8 + CLOCK_R; }

static bool inside(xd_rect rect, double x, double y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

static xd_rect device(xd_rect rect, double scale) {
    return (xd_rect){(int)lround(rect.x * scale), (int)lround(rect.y * scale),
                     (int)lround(rect.w * scale), (int)lround(rect.h * scale)};
}

/* A link's text, centred in `rect`: its extent there (device px). */
static xd_rect link_extent(cairo_t *cairo, xd_rect rect, const char *text,
                           const xd_text_style *style) {
    int width = XD_MIN(xd_text_width(cairo, text, style), rect.w);
    return (xd_rect){rect.x + (rect.w - width) / 2, rect.y, width, rect.h};
}

/* ---- drawing ------------------------------------------------------------------------- */

typedef struct palette {
    xd_rgba body, border, band, band_line;
    xd_rgba text, dim_text, link, link_hot;
    xd_rgba head, head_text, weekdays;
    xd_rgba chosen, chosen_text, chosen_edge, hot_cell, hot_edge, today_edge;
    xd_rgba face, face_edge, hands, second_hand;
    bool round_today; /* XP's red ring, else 7's square */
} palette;

static palette palette_for(const xd_theme *theme) {
    if (theme->style == XD_STYLE_AERO) {
        return (palette){
            .body = xd_rgba_hex(0xFFFFFF),
            .border = xd_rgba_hex(0x8E8F8F),
            .band = xd_rgba_hex(0xF1F5FB),
            .band_line = xd_rgba_hex(0xCCD9EA),
            .text = xd_rgba_hex(0x000000),
            .dim_text = xd_rgba_hex(0x8D8D8D),
            .link = xd_rgba_hex(0x0066CC),
            .link_hot = xd_rgba_hex(0x3399FF),
            .head = xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0),
            .head_text = xd_rgba_hex(0x1E395B),
            .weekdays = xd_rgba_hex(0x1E395B),
            .chosen = xd_rgba_hex(0xCBE3F9),
            .chosen_text = xd_rgba_hex(0x000000),
            .chosen_edge = xd_rgba_hex(0x7DA2CE),
            .hot_cell = xd_rgba_hex(0xE5F1FB),
            .hot_edge = xd_rgba_hex(0xA5C8EB),
            .today_edge = xd_rgba_hex(0x3A6EA5),
            .face = xd_rgba_hex(0xFBFBFB),
            .face_edge = xd_rgba_hex(0x9AA4B1),
            .hands = xd_rgba_hex(0x1E1E1E),
            .second_hand = xd_rgba_hex(0xC81E1E),
            .round_today = false,
        };
    }
    xd_rgba hilight = xd_sys(theme, XD_COLOR_HILIGHT);
    return (palette){
        .body = xd_sys(theme, XD_COLOR_WINDOW),
        .border = xd_sys(theme, XD_COLOR_WINDOW_FRAME),
        .band = xd_sys(theme, XD_COLOR_BUTTON_FACE),
        .band_line = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
        .text = xd_sys(theme, XD_COLOR_WINDOW_TEXT),
        .dim_text = xd_sys(theme, XD_COLOR_GRAY_TEXT),
        .link = xd_sys(theme, XD_COLOR_HOT_TRACKING),
        .link_hot = xd_sys(theme, XD_COLOR_HOT_TRACKING),
        .head = xd_sys(theme, XD_COLOR_ACTIVE_TITLE),
        .head_text = xd_sys(theme, XD_COLOR_TITLE_TEXT),
        .weekdays = xd_sys(theme, XD_COLOR_ACTIVE_TITLE),
        .chosen = hilight,
        .chosen_text = xd_sys(theme, XD_COLOR_HILIGHT_TEXT),
        .chosen_edge = hilight,
        .hot_cell = xd_rgba_alpha(hilight, 0),
        .hot_edge = xd_rgba_alpha(hilight, 0),
        .today_edge = xd_rgba_hex(0xD00000),
        .face = xd_sys(theme, XD_COLOR_WINDOW),
        .face_edge = xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
        .hands = xd_sys(theme, XD_COLOR_WINDOW_TEXT),
        .second_hand = xd_rgba_hex(0xD00000),
        .round_today = true,
    };
}

static void draw_link(cairo_t *cairo, const xd_theme *theme, xd_rect rect, const char *text,
                      xd_rgba color, bool hot) {
    xd_text_style st = xd_text(theme, XD_FONT_MENU, color, XD_ALIGN_CENTER);
    xd_draw_text(cairo, rect, text, &st);
    if (!hot)
        return;
    xd_rect extent = link_extent(cairo, rect, text, &st);
    int px = xd_tpx(theme, 1);
    xd_fill_rect(cairo, (xd_rect){extent.x, rect.y + rect.h / 2 + xd_tpx(theme, 6), extent.w, px},
                 color);
}

/* The month's arrows: small triangles, pointing back or on. */
static void draw_arrow(cairo_t *cairo, xd_rect rect, int direction, xd_rgba color, double scale) {
    double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h / 2.0;
    double half = 4 * scale;
    cairo_move_to(cairo, cx - direction * half / 2, cy - half);
    cairo_line_to(cairo, cx + direction * half / 2, cy);
    cairo_line_to(cairo, cx - direction * half / 2, cy + half);
    cairo_close_path(cairo);
    xd_set_color(cairo, color);
    cairo_fill(cairo);
}

static void draw_calendar(flyout *fly, const xd_theme *theme, cairo_t *cairo, const palette *pal) {
    double scale = fly->scale;
    xd_rect head = device((xd_rect){CAL_X, CAL_Y, CAL_W, HEAD_H}, scale);
    xd_fill_rect(cairo, head, pal->head);
    char *month = format_day(fly->chosen, "%B %Y");
    xd_font_spec bold = theme->scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style head_style = xd_text(theme, XD_FONT_MENU, pal->head_text, XD_ALIGN_CENTER);
    head_style.font = &bold;
    xd_draw_text(cairo, head, month, &head_style);
    g_free(month);
    for (int k = 0; k < 2; k++) {
        part arrow = k == 0 ? PART_PREVIOUS : PART_NEXT;
        xd_rect rect = device(k == 0 ? previous_rect() : next_rect(), scale);
        if (fly->hot == arrow && theme->style == XD_STYLE_AERO) {
            xd_fill_rect(cairo, xd_rect_inset(rect, xd_tpx(theme, 2), xd_tpx(theme, 3)),
                         pal->hot_cell);
        }
        draw_arrow(cairo, rect, k == 0 ? -1 : 1, pal->head_text, scale);
    }

    /* The weekdays' initials, from the locale's first day, over a line. */
    int first_weekday = xd_calendar_first_weekday();
    xd_text_style day_style = xd_text(theme, XD_FONT_MENU, pal->weekdays, XD_ALIGN_CENTER);
    for (int column = 0; column < 7; column++) {
        /* 2026-02-01 was a Sunday. */
        xd_day sample = {2026, 2, 1 + (first_weekday + column) % 7};
        char *name = format_day(sample, "%a");
        /* Windows 7 shows two letters: "Su", "Mo". */
        char *initials = g_utf8_substring(name, 0, XD_MIN(g_utf8_strlen(name, -1), 2));
        xd_rect rect =
            device((xd_rect){CAL_X + column * CELL_W, CAL_Y + HEAD_H, CELL_W, WEEKDAYS_H}, scale);
        xd_draw_text(cairo, rect, initials, &day_style);
        g_free(initials);
        g_free(name);
    }
    int line_y = (int)lround((CAL_Y + HEAD_H + WEEKDAYS_H - 2) * scale);
    xd_fill_rect(
        cairo,
        (xd_rect){head.x + xd_tpx(theme, 4), line_y, head.w - xd_tpx(theme, 8), xd_tpx(theme, 1)},
        theme->style == XD_STYLE_AERO ? xd_rgba_hex(0xD5DFE5) : pal->dim_text);

    xd_day cells[XD_CALENDAR_CELLS];
    xd_calendar_page(fly->chosen.year, fly->chosen.month, first_weekday, cells);
    xd_day now = today();
    int px = xd_tpx(theme, 1);
    for (int cell = 0; cell < XD_CALENDAR_CELLS; cell++) {
        xd_rect rect = device(cell_rect(cell), scale);
        xd_rect box = xd_rect_inset(rect, px, px);
        bool chosen = xd_calendar_same(cells[cell], fly->chosen);
        bool in_month = cells[cell].month == fly->chosen.month;
        if (chosen) {
            xd_fill_rect(cairo, box, pal->chosen);
            xd_draw_bevel(cairo, box, px, pal->chosen_edge, pal->chosen_edge);
        } else if (fly->hot == cell) {
            xd_fill_rect(cairo, box, pal->hot_cell);
            xd_draw_bevel(cairo, box, px, pal->hot_edge, pal->hot_edge);
        }
        if (xd_calendar_same(cells[cell], now)) {
            if (pal->round_today) {
                cairo_save(cairo);
                cairo_new_path(cairo);
                cairo_translate(cairo, box.x + box.w / 2.0, box.y + box.h / 2.0);
                cairo_scale(cairo, box.w / 2.0 - px, box.h / 2.0 - px / 2.0);
                cairo_arc(cairo, 0, 0, 1, 0, 2 * G_PI);
                cairo_restore(cairo);
                cairo_set_line_width(cairo, px);
                xd_set_color(cairo, pal->today_edge);
                cairo_stroke(cairo);
            } else {
                xd_draw_bevel(cairo, box, px, pal->today_edge, pal->today_edge);
            }
        }
        char number[12];
        snprintf(number, sizeof(number), "%d", cells[cell].day);
        xd_text_style st = xd_text(theme, XD_FONT_MENU,
                                   chosen     ? pal->chosen_text
                                   : in_month ? pal->text
                                              : pal->dim_text,
                                   XD_ALIGN_CENTER);
        xd_draw_text(cairo, rect, number, &st);
    }
}

/* The clock: a round face with the hours marked, and its three hands. */
static void draw_clock(flyout *fly, const xd_theme *theme, cairo_t *cairo, const palette *pal) {
    double scale = fly->scale;
    double cx = clock_cx() * scale, cy = clock_cy() * scale, radius = CLOCK_R * scale;
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, cy, radius, 0, 2 * G_PI);
    cairo_pattern_t *face = cairo_pattern_create_linear(0, cy - radius, 0, cy + radius);
    xd_rgba light = pal->face, shade = xd_rgba_mix(pal->face, pal->face_edge, 0.25);
    cairo_pattern_add_color_stop_rgba(face, 0, light.r, light.g, light.b, light.a);
    cairo_pattern_add_color_stop_rgba(face, 1, shade.r, shade.g, shade.b, shade.a);
    cairo_set_source(cairo, face);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(face);
    cairo_set_line_width(cairo, 2 * scale);
    xd_set_color(cairo, pal->face_edge);
    cairo_stroke(cairo);
    xd_set_color(cairo, pal->hands);
    for (int mark = 0; mark < 60; mark++) {
        bool hour = mark % 5 == 0;
        if (!hour && radius < 40 * scale)
            continue;
        double angle = mark * G_PI / 30;
        double outer = radius - 5 * scale, inner = outer - (hour ? 7 : 2) * scale;
        cairo_set_line_width(cairo, (hour ? 2.0 : 1.0) * scale);
        cairo_move_to(cairo, cx + sin(angle) * inner, cy - cos(angle) * inner);
        cairo_line_to(cairo, cx + sin(angle) * outer, cy - cos(angle) * outer);
        cairo_stroke(cairo);
    }
    struct tm tm = now_tm();
    double seconds = tm.tm_sec, minutes = tm.tm_min + seconds / 60;
    double hours = tm.tm_hour % 12 + minutes / 60;
    const struct {
        double turn, length, width;
        xd_rgba color;
    } hands[] = {
        {hours / 12, 0.50, 3.5, pal->hands},
        {minutes / 60, 0.75, 2.5, pal->hands},
        {seconds / 60, 0.82, 1.0, pal->second_hand},
    };
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    for (size_t i = 0; i < XD_ARRAY_LEN(hands); i++) {
        double angle = hands[i].turn * 2 * G_PI;
        cairo_set_line_width(cairo, hands[i].width * scale);
        cairo_move_to(cairo, cx - sin(angle) * radius * 0.1, cy + cos(angle) * radius * 0.1);
        cairo_line_to(cairo, cx + sin(angle) * radius * hands[i].length,
                      cy - cos(angle) * radius * hands[i].length);
        xd_set_color(cairo, hands[i].color);
        cairo_stroke(cairo);
    }
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, cy, 3 * scale, 0, 2 * G_PI);
    xd_set_color(cairo, pal->second_hand);
    cairo_fill(cairo);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_BUTT);

    char text[32];
    time_text(&tm, text, sizeof(text));
    xd_text_style st = xd_text(theme, XD_FONT_MENU, pal->text, XD_ALIGN_CENTER);
    xd_draw_text(
        cairo,
        device((xd_rect){clock_cx() - CLOCK_R, clock_cy() + CLOCK_R + 6, 2 * CLOCK_R, 18}, scale),
        text, &st);
}

static void render(flyout *fly) {
    xd_shell *shell = fly->shell;
    double scale = fly->scale;
    double width = ceil(FLY_W * scale), height = ceil(FLY_H * scale);
    int dw = (int)width, dh = (int)height;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&fly->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme theme;
    xd_theme_init(&theme, shell->style, &shell->scheme, scale);
    palette pal = palette_for(&theme);
    cairo_t *cairo = cairo_create(buf->surface);
    xd_rect all = {0, 0, dw, dh};
    xd_fill_rect(cairo, all, pal.body);
    xd_rect band = device(band_rect(), scale);
    xd_fill_rect(cairo, band, pal.band);
    int px = xd_tpx(&theme, 1);
    xd_fill_rect(cairo, (xd_rect){band.x, band.y, band.w, px}, pal.band_line);
    xd_draw_bevel(cairo, all, px, pal.border, pal.border);

    char *date = format_day(today(), "%A, %B %-d, %Y");
    draw_link(cairo, &theme, device(date_rect(), scale), date,
              fly->hot == PART_TODAY ? pal.link_hot : pal.link, fly->hot == PART_TODAY);
    g_free(date);
    draw_calendar(fly, &theme, cairo, &pal);
    draw_clock(fly, &theme, cairo, &pal);
    draw_link(cairo, &theme, band, "Change date and time settings...",
              fly->hot == PART_SETTINGS ? pal.link_hot : pal.link, fly->hot == PART_SETTINGS);

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
    if (fly->tick != 0)
        g_source_remove(fly->tick);
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
    xd_taskbars_schedule_tuck(shell);
}

/* The second hand moves on each second, on the second. */
static gboolean tick(gpointer data) {
    flyout *fly = data;
    fly->dirty = true;
    gint64 ms = g_get_real_time() / 1000;
    fly->tick = g_timeout_add((guint)(1000 - ms % 1000), tick, fly);
    return G_SOURCE_REMOVE;
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

void xd_clock_flyout_toggle(xd_taskbar *bar, xd_rect anchor, uint32_t serial) {
    xd_shell *shell = bar->shell;
    xd_menu_close_all(shell);
    xd_volume_flyout_dismiss(shell);
    /* A second click on the clock closes it: the press already did. */
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
    fly->chosen = today();
    fly->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->viewporter != nullptr)
        fly->viewport = wp_viewporter_get_viewport(shell->viewporter, fly->surface);
    else
        fly->scale = 1.0;
    fly->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, fly->surface);
    xdg_surface_add_listener(fly->xdg_surface, &surface_listener, fly);
    /* Above the clock, its right edge at the clock's, as 7's; beside a
     * side taskbar. */
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
    /* The calendar's keys reach us only with the keyboard taken before the
     * grab starts (see xd_taskbar_take_keyboard). */
    xd_taskbar_take_keyboard(bar);
    if (shell->seat != nullptr && serial != 0)
        xdg_popup_grab(fly->popup, shell->seat, serial);
    wl_surface_commit(fly->surface);
    the_flyout = fly;
    tick(fly);
}

bool xd_clock_flyout_owns([[maybe_unused]] xd_shell *shell, struct wl_surface *surface) {
    return the_flyout != nullptr && the_flyout->surface == surface;
}

static int part_at(double x, double y) {
    if (inside(previous_rect(), x, y))
        return PART_PREVIOUS;
    if (inside(next_rect(), x, y))
        return PART_NEXT;
    for (int cell = 0; cell < XD_CALENDAR_CELLS; cell++) {
        if (inside(cell_rect(cell), x, y))
            return cell;
    }
    /* The links: their whole rows, the text centred in them. */
    if (inside(date_rect(), x, y))
        return PART_TODAY;
    if (inside(band_rect(), x, y))
        return PART_SETTINGS;
    return PART_NONE;
}

void xd_clock_flyout_motion([[maybe_unused]] xd_shell *shell, double x, double y) {
    flyout *fly = the_flyout;
    if (fly == nullptr)
        return;
    int hot = part_at(x, y);
    if (hot != fly->hot) {
        fly->hot = hot;
        fly->dirty = true;
    }
}

void xd_clock_flyout_leave([[maybe_unused]] xd_shell *shell) {
    flyout *fly = the_flyout;
    if (fly != nullptr && fly->hot != PART_NONE) {
        fly->hot = PART_NONE;
        fly->dirty = true;
    }
}

static void choose(flyout *fly, xd_day day) {
    fly->chosen = day;
    fly->dirty = true;
}

void xd_clock_flyout_button([[maybe_unused]] xd_shell *shell, uint32_t button, bool pressed,
                            double x, double y) {
    flyout *fly = the_flyout;
    if (fly == nullptr || button != BTN_LEFT)
        return;
    int at = part_at(x, y);
    if (pressed) {
        fly->pressed = at;
        return;
    }
    int target = fly->pressed;
    fly->pressed = PART_NONE;
    if (target != at)
        return;
    if (target >= 0) {
        xd_day cells[XD_CALENDAR_CELLS];
        xd_calendar_page(fly->chosen.year, fly->chosen.month, xd_calendar_first_weekday(), cells);
        choose(fly, cells[target]);
    } else if (target == PART_PREVIOUS || target == PART_NEXT) {
        choose(fly, xd_calendar_add_months(fly->chosen, target == PART_NEXT ? 1 : -1));
    } else if (target == PART_TODAY) {
        choose(fly, today());
    } else if (target == PART_SETTINGS) {
        xd_shell *owner = fly->shell;
        close_flyout(fly);
        xd_shell_launch(owner, xd_shell_own_command(owner, "xd-timedate", nullptr));
    }
}

bool xd_clock_flyout_key([[maybe_unused]] xd_shell *shell, uint32_t sym) {
    flyout *fly = the_flyout;
    if (fly == nullptr)
        return false;
    switch (sym) {
    case XKB_KEY_Left:
    case XKB_KEY_KP_Left:
        choose(fly, xd_calendar_add_days(fly->chosen, -1));
        break;
    case XKB_KEY_Right:
    case XKB_KEY_KP_Right:
        choose(fly, xd_calendar_add_days(fly->chosen, 1));
        break;
    case XKB_KEY_Up:
    case XKB_KEY_KP_Up:
        choose(fly, xd_calendar_add_days(fly->chosen, -7));
        break;
    case XKB_KEY_Down:
    case XKB_KEY_KP_Down:
        choose(fly, xd_calendar_add_days(fly->chosen, 7));
        break;
    case XKB_KEY_Page_Up:
    case XKB_KEY_KP_Page_Up:
        choose(fly, xd_calendar_add_months(fly->chosen, -1));
        break;
    case XKB_KEY_Page_Down:
    case XKB_KEY_KP_Page_Down:
        choose(fly, xd_calendar_add_months(fly->chosen, 1));
        break;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
        choose(fly, today());
        break;
    case XKB_KEY_Escape:
        close_flyout(fly);
        break;
    default:
        break; /* others are the flyout's too while it's open */
    }
    return true;
}

void xd_clock_flyout_dismiss([[maybe_unused]] xd_shell *shell) { close_flyout(the_flyout); }

void xd_clock_flyout_render_dirty([[maybe_unused]] xd_shell *shell) {
    flyout *fly = the_flyout;
    if (fly != nullptr && fly->configured && fly->dirty)
        render(fly);
}
