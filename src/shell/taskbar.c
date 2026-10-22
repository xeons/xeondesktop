/* SPDX-License-Identifier: MIT
 * The taskbar: one layer-shell surface per output, docked to the bottom
 * edge with an exclusive zone so maximized windows stop above it.
 *
 * Drawing happens in device pixels at the output's (fractional) scale; the
 * surface is shown at its logical size through wp_viewport.
 */
#include <glib.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>

#include "../core/alloc.h"
#include "shell.h"

/* Windows caps task buttons at about 160 px and shrinks them as more
 * windows open. */
/* Below the minimum (an icon and a few letters) buttons go onto pages. */
enum { XD_TASK_MAX_W = 160, XD_TASK_MIN_W = 56, XD_TASK_GAP = 3 };
/* Group similar buttons once they'd get narrower than this. */
enum { XD_TASK_GROUP_W = 100 };

static void bar_theme(const xd_taskbar *bar, xd_theme *out) {
    xd_theme_init(out, bar->shell->style, &bar->shell->scheme, bar->scale);
}

static bool large_icons(const xd_shell *shell) {
    return xd_settings_taskbar_large_icons(&shell->settings, shell->style);
}

static int logical_height(const xd_shell *shell) {
    xd_theme theme;
    xd_theme_init(&theme, shell->style, &shell->scheme, 1.0);
    return xd_theme_taskbar_height(&theme, large_icons(shell));
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

static void apply_options(xd_taskbar *bar);

void xd_taskbar_retheme(xd_taskbar *bar) {
    /* The height follows the scheme (font size changes it). */
    if (bar->surface != nullptr)
        apply_options(bar);
    bar->task_count = 0;
    bar->dirty = true;
    if (bar->desktop != nullptr)
        bar->desktop->dirty = true;
}

void xd_taskbar_reserve(xd_taskbar *bar, size_t count) {
    if (count <= bar->task_cap)
        return;
    size_t cap = XD_MAX(count, bar->task_cap * 2);
    bar->task_rects = xd_realloc_array(bar->task_rects, cap, sizeof(*bar->task_rects));
    bar->task_views = xd_realloc_array(bar->task_views, cap, sizeof(*bar->task_views));
    bar->task_counts = xd_realloc_array(bar->task_counts, cap, sizeof(*bar->task_counts));
    bar->entry_views = xd_realloc_array(bar->entry_views, cap, sizeof(*bar->entry_views));
    bar->entry_counts = xd_realloc_array(bar->entry_counts, cap, sizeof(*bar->entry_counts));
    bar->bar_buttons = xd_realloc_array(bar->bar_buttons, cap, sizeof(*bar->bar_buttons));
    bar->task_cap = cap;
}

/* The notification area's chevron: "<<" shows folded icons, ">>" folds
 * them again. A flat toolbar button, as XP's. */
static void draw_chevron(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                         bool expanded, bool side) {
    if (xd_theme_draw_tray_chevron(theme, cairo, rect, state, expanded, side))
        return;
    if (state & (XD_STATE_HOT | XD_STATE_PRESSED))
        xd_theme_draw_toolbar_button(theme, cairo, rect, state);
    xd_rgba color = theme->style == XD_STYLE_CLASSIC ? xd_sys(theme, XD_COLOR_BUTTON_TEXT)
                                                     : xd_rgba_hex(0xFFFFFF);
    cairo_set_source_rgba(cairo, color.r, color.g, color.b, color.a);
    cairo_set_line_width(cairo, xd_tpx(theme, 1) * 1.5);
    double unit = xd_tpx(theme, 1);
    double cx = rect.x + rect.w / 2.0, cy = rect.y + rect.h / 2.0;
    double dir = expanded ? 1 : -1; /* pointing right when it would fold */
    for (int i = 0; i < 2; i++) {
        double x = cx + (i * 4 - 2) * unit;
        cairo_move_to(cairo, x - dir * 2 * unit, cy - 3 * unit);
        cairo_line_to(cairo, x + dir * 1 * unit, cy);
        cairo_line_to(cairo, x - dir * 2 * unit, cy + 3 * unit);
    }
    cairo_stroke(cairo);
}

/* The small down arrow at the right end of a group's button. */
static void draw_group_arrow(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state) {
    int arrow_size = xd_tpx(theme, 3);
    double cx = rect.x + rect.w - xd_tpx(theme, 10);
    double cy = rect.y + rect.h / 2.0 - arrow_size / 2.0;
    if (state & (XD_STATE_CHECKED | XD_STATE_PRESSED) && theme->style == XD_STYLE_CLASSIC) {
        cx += xd_tpx(theme, 1);
        cy += xd_tpx(theme, 1);
    }
    xd_rgba color = theme->style == XD_STYLE_CLASSIC ? xd_sys(theme, XD_COLOR_BUTTON_TEXT)
                                                     : xd_rgba_hex(0xFFFFFF);
    cairo_set_source_rgba(cairo, color.r, color.g, color.b, color.a);
    cairo_move_to(cairo, cx - arrow_size, cy);
    cairo_line_to(cairo, cx + arrow_size + 1, cy);
    cairo_line_to(cairo, cx + 0.5, cy + arrow_size + 1);
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

static uint8_t button_state(const xd_taskbar *bar, int index) {
    uint8_t state = 0;
    if (bar->hot == index)
        state |= XD_STATE_HOT;
    if (bar->pressed == index && bar->hot == index)
        state |= XD_STATE_PRESSED;
    return state;
}

/* ---- layout ---------------------------------------------------------------------- */

static bool side_bar(const xd_shell *shell) {
    return shell->settings.edge == XD_EDGE_LEFT || shell->settings.edge == XD_EDGE_RIGHT;
}

/* Where buttons go: inside the rim (Classic leaves a 2px border and the
 * rim's two lines; the XP styles draw over the whole bar). */
static xd_rect content_box(const xd_theme *theme, xd_edge edge, int dw, int dh) {
    if (theme->style != XD_STYLE_CLASSIC)
        return (xd_rect){0, 0, dw, dh};
    int inset = xd_tpx(theme, 2);
    xd_rect rect = {inset, inset, dw - 2 * inset, dh - 2 * inset};
    switch (edge) {
    case XD_EDGE_BOTTOM:
        rect.y += inset;
        rect.h -= inset;
        break;
    case XD_EDGE_TOP:
        rect.h -= inset;
        break;
    case XD_EDGE_LEFT:
        rect.w -= inset;
        break;
    case XD_EDGE_RIGHT:
        rect.x += inset;
        rect.w -= inset;
        break;
    }
    return rect;
}

/* Text on the taskbar's own face (Quick Launch's Show Text and Show
 * Title), as the other toolbar text. */
static xd_text_style toolbar_text(const xd_theme *theme) {
    xd_rgba ink = theme->style == XD_STYLE_CLASSIC ? xd_sys(theme, XD_COLOR_BUTTON_TEXT)
                                                   : xd_rgba_hex(0xFFFFFF);
    return xd_text(theme, XD_FONT_MENU, ink, XD_ALIGN_START);
}

/* Quick Launch's icons: View > Small Icons (16) or Large Icons (32, or
 * what the row's height leaves room for). */
static int quick_icon_px(const xd_shell *shell, const xd_theme *theme, int button_h) {
    int small = xd_tpx(theme, 16);
    if (!shell->settings.quick_large)
        return small;
    return XD_MAX(small, XD_MIN(xd_tpx(theme, 32), button_h - xd_tpx(theme, 4)));
}

/* A button's width: the icon, and with Show Text its name (XP cuts long
 * names short). */
static int quick_button_w(const xd_shell *shell, const xd_theme *theme, cairo_t *cairo, size_t i,
                          int icon) {
    int width = icon + xd_tpx(theme, 7);
    const char *name = shell->quicklaunch.items[i].name;
    if (!shell->settings.quick_show_text || name == nullptr)
        return width;
    xd_text_style st = toolbar_text(theme);
    return width + XD_MIN(xd_text_width(cairo, name, &st), xd_tpx(theme, 120)) + xd_tpx(theme, 4);
}

static void draw_quick_button(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, size_t i,
                              xd_rect rect, int icon) {
    const xd_quicklaunch *ql = &bar->shell->quicklaunch;
    int hit = XD_HIT_QUICK - (int)i;
    xd_theme_draw_toolbar_button(theme, cairo, rect, button_state(bar, hit));
    bool text = bar->shell->settings.quick_show_text && ql->items[i].name != nullptr;
    int pressed = bar->pressed == hit && bar->hot == hit ? xd_tpx(theme, 1) : 0;
    int ix = text ? rect.x + xd_tpx(theme, 3) : rect.x + (rect.w - icon) / 2;
    const char *names[] = {ql->items[i].icon, "application-x-executable"};
    cairo_surface_t *img = xd_icons_get_any(bar->shell->icons, names, XD_ARRAY_LEN(names), icon);
    if (img != nullptr) {
        cairo_set_source_surface(cairo, img, ix + pressed, rect.y + (rect.h - icon) / 2 + pressed);
        cairo_paint(cairo);
    }
    if (text) {
        xd_text_style st = toolbar_text(theme);
        int tx = ix + icon + xd_tpx(theme, 4);
        xd_draw_text(cairo,
                     (xd_rect){tx + pressed, rect.y + pressed,
                               rect.x + rect.w - tx - xd_tpx(theme, 2), rect.h},
                     ql->items[i].name, &st);
    }
}

/* XP's black (or white) bar where a drop or a dragged button goes. */
static void draw_insert_mark(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, bool side) {
    if (bar->drop_insert < 0 || bar->quick_band.w <= 0)
        return;
    size_t k = (size_t)bar->drop_insert;
    xd_rect ref = k < bar->quick_count     ? bar->quick_rects[k]
                  : bar->quick_count > 0 ? bar->quick_rects[bar->quick_count - 1]
                                         : bar->quick_band;
    int px = xd_tpx(theme, 1);
    xd_rgba ink = theme->style == XD_STYLE_CLASSIC ? xd_sys(theme, XD_COLOR_BUTTON_TEXT)
                                                   : xd_rgba_hex(0xFFFFFF);
    bool after = k >= bar->quick_count && bar->quick_count > 0;
    if (side)
        xd_fill_rect(
            cairo,
            (xd_rect){ref.x + px, after ? ref.y + ref.h - px : ref.y - px, ref.w - 2 * px, 2 * px},
            ink);
    else
        xd_fill_rect(
            cairo,
            (xd_rect){after ? ref.x + ref.w - px : ref.x - px, ref.y + px, 2 * px, ref.h - 2 * px},
            ink);
}

/* Quick Launch from (x, y): a row on top and bottom taskbars (with grips
 * when unlocked), a grid filling the width on side ones. Returns the
 * band's far edge (x for rows, y for side taskbars). */
static int layout_quick_launch(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, int x, int y,
                               int row_h, int width) {
    xd_shell *shell = bar->shell;
    const xd_quicklaunch *ql = &shell->quicklaunch;
    bar->quick_count = 0;
    bar->quick_band = (xd_rect){0};
    bool side = side_bar(shell);
    if (!shell->settings.quick_launch || ql->count == 0)
        return side ? y : x;
    if (ql->count > bar->quick_cap) {
        bar->quick_cap = ql->count;
        bar->quick_rects =
            xd_realloc_array(bar->quick_rects, bar->quick_cap, sizeof(*bar->quick_rects));
    }
    int qh = theme->style == XD_STYLE_CLASSIC ? row_h : row_h - xd_tpx(theme, 6);
    int qy = theme->style == XD_STYLE_CLASSIC ? y : y + xd_tpx(theme, 3);
    int icon = quick_icon_px(shell, theme, qh);
    int qw = icon + xd_tpx(theme, 7);
    bool text = shell->settings.quick_show_text;
    xd_text_style title_st = toolbar_text(theme);
    const char *title = "Quick Launch";
    if (side) {
        /* Show Title: its own line on top. */
        int band_y = qy;
        if (shell->settings.quick_show_title) {
            xd_draw_text(cairo, (xd_rect){x + xd_tpx(theme, 4), qy, width - xd_tpx(theme, 8), qh},
                         title, &title_st);
            qy += qh;
        }
        /* A grid, as many across as fit; with Show Text, one a line. */
        int cols = text ? 1 : XD_MAX(1, width / qw);
        int bw = text ? width : qw;
        for (size_t i = 0; i < ql->count; i++) {
            xd_rect rect = {x + (int)(i % (size_t)cols) * bw, qy + (int)(i / (size_t)cols) * qh, bw,
                            qh};
            bar->quick_rects[bar->quick_count++] = rect;
            draw_quick_button(bar, theme, cairo, i, rect, icon);
        }
        int rows = (int)((ql->count + (size_t)cols - 1) / (size_t)cols);
        bar->quick_band = (xd_rect){x, band_y, width, qy - band_y + rows * qh};
        draw_insert_mark(bar, theme, cairo, true);
        return qy + rows * qh + xd_tpx(theme, 4);
    }
    /* A locked taskbar has no grips, as in XP. */
    bool locked = shell->settings.locked;
    int grip = locked ? xd_tpx(theme, 3) : xd_theme_toolbar_grip_width(theme);
    if (!locked)
        xd_theme_draw_toolbar_grip(theme, cairo, (xd_rect){x, qy, grip, qh});
    int qx = x + grip + xd_tpx(theme, 1);
    /* Show Title: "Quick Launch" before the buttons. */
    if (shell->settings.quick_show_title) {
        int tw = xd_text_width(cairo, title, &title_st);
        xd_draw_text(cairo, (xd_rect){qx + xd_tpx(theme, 2), qy, tw + xd_tpx(theme, 2), qh}, title,
                     &title_st);
        qx += tw + xd_tpx(theme, 8);
    }
    for (size_t i = 0; i < ql->count; i++) {
        int bw = quick_button_w(shell, theme, cairo, i, icon);
        xd_rect rect = {qx, qy, bw, qh};
        bar->quick_rects[bar->quick_count++] = rect;
        draw_quick_button(bar, theme, cairo, i, rect, icon);
        qx += bw;
    }
    if (!locked)
        xd_theme_draw_toolbar_grip(theme, cairo, (xd_rect){qx + xd_tpx(theme, 2), qy, grip, qh});
    bar->quick_band = (xd_rect){x, qy, qx + xd_tpx(theme, 2) + grip - x, qh};
    draw_insert_mark(bar, theme, cairo, false);
    return qx + xd_tpx(theme, 2) + grip;
}

static void draw_tray_icon(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, size_t i,
                           xd_rect slot) {
    xd_shell *shell = bar->shell;
    int icon_px = xd_tpx(theme, 16);
    xd_tray_item *item = xd_tray_get(shell, bar->tray_index[i]);
    xd_rect at = {slot.x + (slot.w - icon_px) / 2, slot.y + (slot.h - icon_px) / 2, icon_px,
                  icon_px};
    if (item == nullptr || xd_tray_item_draw(item, theme, cairo, at))
        return;
    cairo_surface_t *img = xd_tray_item_icon(shell, item, icon_px);
    if (img != nullptr) {
        cairo_set_source_surface(cairo, img, at.x, at.y);
        cairo_paint(cairo);
    }
}

/* The clock's text for a top or bottom taskbar `avail_h` tall (device
 * px): the time, then as XP shows them when the taskbar is tall enough,
 * the day of the week and the date. Its width in *width. */
static void clock_text(const xd_shell *shell, const xd_theme *theme, cairo_t *cairo, int avail_h,
                       char *out, size_t size, int *width) {
    xd_text_style st = xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_END);
    PangoLayout *probe = xd_text_layout(cairo, "Ag", &st);
    int line_h = 0;
    pango_layout_get_pixel_size(probe, nullptr, &line_h);
    g_object_unref(probe);
    int lines = line_h > 0 ? (avail_h - xd_tpx(theme, 8)) / line_h : 1;
    char day[64] = "", date[64] = "";
    GDateTime *now = lines >= 2 ? g_date_time_new_now_local() : nullptr;
    if (now != nullptr) {
        char *weekday = g_date_time_format(now, "%A");
        /* The locale's short date, as Windows' clock shows it. */
        char *short_date = lines >= 3 ? g_date_time_format(now, "%x") : nullptr;
        g_strlcpy(day, weekday != nullptr ? weekday : "", sizeof(day));
        g_strlcpy(date, short_date != nullptr ? short_date : "", sizeof(date));
        g_free(short_date);
        g_free(weekday);
        g_date_time_unref(now);
    }
    snprintf(out, size, "%s%s%s%s%s", shell->clock, day[0] != '\0' ? "\n" : "", day,
             date[0] != '\0' ? "\n" : "", date);
    *width = 0;
    const char *parts[] = {shell->clock, day, date};
    for (size_t i = 0; i < XD_ARRAY_LEN(parts); i++) {
        if (parts[i][0] != '\0')
            *width = XD_MAX(*width, xd_text_width(cairo, parts[i], &st));
    }
}

/* The notification area at the end of the taskbar (the right end of rows,
 * the bottom of a side taskbar): the chevron, icons (in as many rows as
 * the taskbar has, or a grid on a side one) and the clock. */
static void layout_tray(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                        int rows, int row_h) {
    xd_shell *shell = bar->shell;
    bool side = side_bar(shell);
    xd_text_style clock_style = xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_END);
    int margin_l, margin_r;
    xd_theme_tray_margins(theme, &margin_l, &margin_r);
    size_t total = xd_tray_count(shell);
    if (total > bar->tray_cap) {
        bar->tray_cap = total;
        bar->tray_rects = xd_realloc_array(bar->tray_rects, total, sizeof(*bar->tray_rects));
        bar->tray_index = xd_realloc_array(bar->tray_index, total, sizeof(*bar->tray_index));
    }
    /* Hide inactive icons: quiet programs' icons fold behind a chevron
     * until it's clicked, as in XP. */
    size_t icons = 0;
    bool any_inactive = false;
    for (size_t i = 0; i < total; i++) {
        xd_tray_item *item = xd_tray_get(shell, i);
        bool quiet = item != nullptr && xd_tray_item_folded(shell, item);
        any_inactive = any_inactive || quiet;
        if (!quiet || bar->tray_expanded)
            bar->tray_index[icons++] = i;
    }
    if (!any_inactive)
        bar->tray_expanded = false;
    int pitch = xd_tpx(theme, 22);
    int chevron_w = any_inactive ? xd_tpx(theme, 15) : 0;
    bool clock = shell->settings.show_clock;
    char clock_lines[192];
    int clock_w = 0;
    if (clock && !side) {
        clock_text(shell, theme, cairo, rect.h, clock_lines, sizeof(clock_lines), &clock_w);
        clock_w += xd_tpx(theme, 1);
    } else if (clock) {
        clock_w = xd_text_width(cairo, shell->clock, &clock_style) + xd_tpx(theme, 1);
    }
    bar->tray_count = 0;
    bar->tray_chevron = (xd_rect){0};
    if (icons == 0 && chevron_w == 0 && !clock) {
        /* No clock and no icons: no notification area at all. */
        bar->tray_rect = side ? (xd_rect){rect.x, rect.y + rect.h, rect.w, 0}
                              : (xd_rect){rect.x + rect.w, rect.y, 0, rect.h};
        return;
    }
    xd_rect slots_origin;
    int cols;
    if (side) {
        /* Icons in a grid across the width, the clock under them. */
        int inner = rect.w - margin_l - margin_r;
        cols = XD_MAX(1, (inner - chevron_w) / pitch);
        int icon_rows = (int)((icons + (size_t)cols - 1) / (size_t)cols);
        int clock_h = clock ? row_h * 2 / 3 : 0;
        int height = xd_tpx(theme, 4) + icon_rows * pitch + clock_h + xd_tpx(theme, 2);
        bar->tray_rect = (xd_rect){rect.x, rect.y + rect.h - height, rect.w, height};
        xd_theme_draw_tray_on(theme, cairo, bar->tray_rect, "", shell->settings.edge);
        if (clock) {
            xd_text_style st =
                xd_text(theme, XD_FONT_MENU, xd_theme_clock_color(theme), XD_ALIGN_CENTER);
            xd_draw_text(cairo,
                         (xd_rect){rect.x,
                                   bar->tray_rect.y + bar->tray_rect.h - clock_h - xd_tpx(theme, 2),
                                   rect.w, clock_h},
                         shell->clock, &st);
        }
        slots_origin = (xd_rect){rect.x + margin_l + chevron_w, bar->tray_rect.y + xd_tpx(theme, 4),
                                 pitch, pitch};
    } else {
        /* As many icon rows as the taskbar has. */
        cols = (int)XD_MAX((icons + (size_t)rows - 1) / (size_t)rows, 1);
        int icons_w = (icons > 0 ? cols * pitch : 0) + chevron_w;
        int width =
            margin_l + icons_w + (icons_w > 0 && clock ? xd_tpx(theme, 4) : 0) + clock_w + margin_r;
        if (clock && icons == 0)
            width = XD_MAX(xd_tpx(theme, 60), width);
        bar->tray_rect = (xd_rect){rect.x + rect.w - width, rect.y, width, rect.h};
        xd_theme_draw_tray_on(theme, cairo, bar->tray_rect, clock ? clock_lines : "",
                              shell->settings.edge);
        slots_origin = (xd_rect){bar->tray_rect.x + margin_l + chevron_w, rect.y, pitch, row_h};
    }
    if (chevron_w > 0) {
        bar->tray_chevron =
            (xd_rect){bar->tray_rect.x + xd_tpx(theme, 2), slots_origin.y + xd_tpx(theme, 3),
                      chevron_w, (side ? pitch : row_h) - xd_tpx(theme, 6)};
        draw_chevron(theme, cairo, bar->tray_chevron, button_state(bar, XD_HIT_TRAY_CHEVRON),
                     bar->tray_expanded, side);
    }
    for (size_t i = 0; i < icons; i++) {
        int col = (int)(i % (size_t)cols), row = (int)(i / (size_t)cols);
        xd_rect slot = {slots_origin.x + col * pitch, slots_origin.y + row * slots_origin.h, pitch,
                        slots_origin.h};
        bar->tray_rects[bar->tray_count++] = slot;
        draw_tray_icon(bar, theme, cairo, i, slot);
    }
}

/* One entry per window; when crowded, XP's "Group similar taskbar
 * buttons" folds each program's windows into one button where its first
 * window was. */
static size_t task_entries(xd_taskbar *bar, bool group) {
    xd_shell *shell = bar->shell;
    xd_toplevel **entries = bar->entry_views;
    size_t *counts = bar->entry_counts;
    size_t ne = 0;
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        entries[ne] = tl;
        counts[ne++] = 1;
    }
    if (!group)
        return ne;
    size_t out = 0;
    for (size_t i = 0; i < ne; i++) {
        xd_toplevel *tl = entries[i];
        size_t j = 0;
        while (j < out && !(tl->app_id != nullptr && entries[j]->app_id != nullptr &&
                            strcmp(entries[j]->app_id, tl->app_id) == 0))
            j++;
        if (j < out) {
            counts[j]++;
            continue;
        }
        entries[out] = tl;
        counts[out++] = 1;
    }
    return out;
}

/* Tells the compositor where a window's button is (zwlr's
 * set_rectangle), where XP's minimize animation goes. */
void xd_taskbar_tell_button(xd_taskbar *bar, xd_toplevel *tl, xd_rect rect) {
    double scale = bar->scale;
    int x = (int)(rect.x / scale), y = (int)(rect.y / scale), width = (int)(rect.w / scale),
        height = (int)(rect.h / scale);
    if (tl->button_surface == bar->surface && tl->button_x == x && tl->button_y == y &&
        tl->button_w == width && tl->button_h == height)
        return;
    tl->button_surface = bar->surface;
    tl->button_x = x;
    tl->button_y = y;
    tl->button_w = width;
    tl->button_h = height;
    zwlr_foreign_toplevel_handle_v1_set_rectangle(tl->handle, bar->surface, x, y, width, height);
}

static void draw_task(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, size_t entry_index,
                      xd_rect rect) {
    xd_shell *shell = bar->shell;
    xd_toplevel *tl = bar->entry_views[entry_index];
    size_t count = bar->entry_counts[entry_index];
    size_t i = bar->task_count++;
    bar->task_rects[i] = rect;
    bar->task_views[i] = tl;
    bar->task_counts[i] = count;
    /* A group's button stands for each of its windows. */
    if (count > 1 && tl->app_id != nullptr) {
        XD_LIST_FOR_EACH(other, &shell->toplevels, xd_toplevel, link) {
            if (other->app_id != nullptr && strcmp(other->app_id, tl->app_id) == 0)
                xd_taskbar_tell_button(bar, other, rect);
        }
    } else {
        xd_taskbar_tell_button(bar, tl, rect);
    }
    /* Keyed as the unified taskbar's, for its windows' thumbnails: a
     * group by its program, else the window itself. */
    char *key = xd_superbar_window_key(tl);
    bar->bar_buttons[i] = (xd_bar_button){
        .key = g_string_chunk_insert(bar->bar_keys, key),
        .window = count > 1 ? nullptr : tl,
        .windows = count,
    };
    g_free(key);

    uint8_t state = button_state(bar, (int)i) | (side_bar(shell) ? XD_STATE_VERTICAL : 0);
    const char *label = tl->title != nullptr && tl->title[0] != '\0' ? tl->title
                        : tl->app_id != nullptr                      ? tl->app_id
                                                                      : "";
    if (count > 1) {
        /* "3 Firefox": pressed while one of them is active. */
        bool any_active = false, any_flash = false;
        XD_LIST_FOR_EACH(toplevel, &shell->toplevels, xd_toplevel, link) {
            if (toplevel->app_id == nullptr || strcmp(toplevel->app_id, tl->app_id) != 0)
                continue;
            any_active = any_active || (toplevel->activated && !toplevel->minimized);
            any_flash = any_flash || (toplevel->attention && toplevel->flashes % 2 == 0);
        }
        if (any_active)
            state |= XD_STATE_CHECKED;
        else if (any_flash)
            state |= XD_STATE_FLASH;
        label = tl->app_name != nullptr ? tl->app_name : tl->app_id;
    } else if (tl->activated && !tl->minimized) {
        state |= XD_STATE_CHECKED;
    } else if (tl->attention && tl->flashes % 2 == 0) {
        state |= XD_STATE_FLASH;
    }
    /* As the title bar: its application's icon, else one named after the
     * app id (as many programs name theirs), else the generic program;
     * 24px with large icons, as 7's labelled buttons have. */
    const char *names[] = {tl->icon, tl->app_id, "application-x-executable"};
    cairo_surface_t *icon = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names),
                                             xd_tpx(theme, large_icons(shell) ? 24 : 16));
    if (count > 1) {
        xd_theme_draw_task_group(theme, cairo, rect, state, (int)count, label, icon);
        draw_group_arrow(theme, cairo, rect, state);
    } else {
        xd_theme_draw_task_button(theme, cairo, rect, state, label, icon);
    }
}

/* Which page shows the active window, when it changed. */
static void follow_active_page(xd_taskbar *bar, size_t ne, size_t per_page) {
    xd_shell *shell = bar->shell;
    uint64_t active_id = 0;
    size_t active = SIZE_MAX;
    for (size_t i = 0; i < ne; i++) {
        XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
            if (tl->activated && !tl->minimized &&
                (tl == bar->entry_views[i] ||
                 (bar->entry_counts[i] > 1 && tl->app_id != nullptr &&
                  strcmp(tl->app_id, bar->entry_views[i]->app_id) == 0))) {
                active = i;
                active_id = tl->id;
            }
        }
    }
    if (active != SIZE_MAX && active_id != bar->page_active_id)
        bar->page = active / per_page;
    bar->page_active_id = active_id;
    if (bar->page >= bar->pages)
        bar->page = bar->pages - 1;
}

/* XP's little up/down buttons for task button pages: stacked at the end
 * of a row band, side by side at the bottom of a side taskbar's. */
void xd_taskbar_draw_page_arrows(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo) {
    uint8_t up_state = button_state(bar, XD_HIT_PAGE_UP);
    uint8_t down_state = button_state(bar, XD_HIT_PAGE_DOWN);
    if (bar->page == 0)
        up_state = XD_STATE_DISABLED;
    if (bar->page + 1 >= bar->pages)
        down_state = XD_STATE_DISABLED;
    /* The combo box's arrow button, flipped for "up". */
    xd_rect rect = bar->page_up_rect;
    cairo_save(cairo);
    cairo_translate(cairo, 0, 2 * rect.y + rect.h);
    cairo_scale(cairo, 1, -1);
    xd_theme_draw_dropdown_button(theme, cairo, rect, up_state);
    cairo_restore(cairo);
    xd_theme_draw_dropdown_button(theme, cairo, bar->page_down_rect, down_state);
}

/* Task buttons in `band`: rows of buttons on top and bottom taskbars,
 * a column on side ones; grouped, then paged, when they don't fit. */
static void layout_tasks(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, xd_rect band,
                         int rows, int row_h) {
    xd_shell *shell = bar->shell;
    /* The unified taskbar: Windows 7's program buttons instead. */
    bar->superbar = false;
    if (xd_settings_taskbar_unified(&shell->settings, shell->style)) {
        xd_superbar_layout(bar, theme, cairo, band, rows, row_h);
        return;
    }
    size_t count = (size_t)wl_list_length(&shell->toplevels);
    xd_taskbar_reserve(bar, count);
    bar->task_count = 0;
    if (bar->bar_keys == nullptr)
        bar->bar_keys = g_string_chunk_new(256);
    g_string_chunk_clear(bar->bar_keys);
    bar->pages = 1;
    bar->page_up_rect = bar->page_down_rect = (xd_rect){0};
    if (count == 0 || band.w <= 0 || band.h <= 0)
        return;
    bool side = side_bar(shell);
    bool classic = theme->style == XD_STYLE_CLASSIC;
    int gap = xd_tpx(theme, XD_TASK_GAP);
    /* A theme package's buttons fill the row (their images carry the
     * margins); the built-in XP styles' sit 3 px down, 5 px shorter. */
    bool fill = classic || xd_theme_task_buttons_fill_row(theme);
    int th = fill ? row_h : row_h - xd_tpx(theme, 5);
    int toff = fill ? 0 : xd_tpx(theme, 3);
    int arrows_w = xd_theme_dropdown_button_width(theme);

    if (side) {
        /* One column, full width, as many as fit top to bottom. */
        int fits = XD_MAX(1, (band.h + gap) / (th + gap));
        size_t ne = task_entries(bar, false);
        if (ne > (size_t)fits && shell->settings.group)
            ne = task_entries(bar, true);
        size_t per_page = ne;
        if (ne > (size_t)fits) {
            per_page = (size_t)XD_MAX(1, fits - 1); /* the last slot holds the arrows */
            bar->pages = (ne + per_page - 1) / per_page;
            follow_active_page(bar, ne, per_page);
            int ay = band.y + (int)per_page * (th + gap);
            bar->page_up_rect = (xd_rect){band.x, ay, band.w / 2, th};
            bar->page_down_rect = (xd_rect){band.x + band.w / 2, ay, band.w - band.w / 2, th};
            xd_taskbar_draw_page_arrows(bar, theme, cairo);
        } else {
            bar->page = 0;
        }
        for (size_t entry_index = bar->page * per_page;
             entry_index < ne && bar->task_count < per_page; entry_index++)
            draw_task(bar, theme, cairo, entry_index,
                      (xd_rect){band.x, band.y + (int)bar->task_count * (th + gap), band.w, th});
        return;
    }

    /* Rows: full-width buttons fill the first row before the next, as in
     * XP; once all rows are full they shrink to share them. */
    int max_w = xd_tpx(theme, XD_TASK_MAX_W);
    int fit = XD_MAX(1, (band.w + gap) / (max_w + gap));
    size_t ne = task_entries(bar, false);
    int cols = XD_MAX((int)((ne + (size_t)rows - 1) / (size_t)rows), XD_MIN((int)ne, fit));
    int width = XD_MIN(max_w, (band.w - (cols - 1) * gap) / XD_MAX(cols, 1));
    if (shell->settings.group && width < xd_tpx(theme, XD_TASK_GROUP_W)) {
        ne = task_entries(bar, true);
        cols = XD_MAX((int)((ne + (size_t)rows - 1) / (size_t)rows), XD_MIN((int)ne, fit));
        width = XD_MIN(max_w, (band.w - (cols - 1) * gap) / XD_MAX(cols, 1));
    }
    int min_w = xd_tpx(theme, XD_TASK_MIN_W);
    size_t per_page = ne;
    int avail = band.w;
    if (width < min_w) {
        /* Still too many to fit: pages, with up/down arrows at the end. */
        avail = band.w - arrows_w - gap;
        cols = XD_MAX(1, (avail + gap) / (min_w + gap));
        per_page = (size_t)cols * (size_t)rows;
        width = (avail - (cols - 1) * gap) / cols;
        bar->pages = (ne + per_page - 1) / per_page;
        follow_active_page(bar, ne, per_page);
        int ax = band.x + band.w - arrows_w;
        int ah = rows * row_h - (classic ? 0 : 2 * toff);
        int ay = band.y + toff;
        bar->page_up_rect = (xd_rect){ax, ay, arrows_w, ah / 2};
        bar->page_down_rect = (xd_rect){ax, ay + ah / 2, arrows_w, ah - ah / 2};
        xd_taskbar_draw_page_arrows(bar, theme, cairo);
    } else {
        bar->page = 0;
    }
    cairo_save(cairo);
    cairo_rectangle(cairo, band.x, band.y, XD_MAX(avail, 0), band.h);
    cairo_clip(cairo);
    size_t first = bar->page * per_page;
    for (size_t entry_index = first; entry_index < ne && bar->task_count < per_page;
         entry_index++) {
        size_t k = entry_index - first;
        int col = (int)(k % (size_t)cols), row = (int)(k / (size_t)cols);
        draw_task(bar, theme, cairo, entry_index,
                  (xd_rect){band.x + col * (width + gap), band.y + row * row_h + toff, width, th});
    }
    cairo_restore(cairo);
}

/* Lays out the start button, Quick Launch, task buttons and the
 * notification area for the taskbar's edge and size, then draws them. */
/* Aero: Windows 7's Show desktop strip at the far end; hovering it peeks
 * at the desktop. Takes it out of `area`. */
static void layout_peek(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, xd_rect *area,
                        bool side) {
    bar->peek_rect = (xd_rect){0};
    if (theme->style != XD_STYLE_AERO)
        return;
    int pw = xd_tpx(theme, 15);
    xd_rect strip;
    if (side) {
        area->h -= pw;
        strip = (xd_rect){area->x, area->y + area->h, area->w, pw};
    } else {
        area->w -= pw;
        strip = (xd_rect){area->x + area->w, area->y, pw, area->h};
    }
    bar->peek_rect = strip;
    bool hot = bar->hot == XD_HIT_PEEK;
    uint8_t peek = (hot ? XD_STATE_HOT : 0) | (bar->pressed == XD_HIT_PEEK ? XD_STATE_PRESSED : 0);
    if (xd_theme_draw_show_desktop(theme, cairo, strip, peek, side))
        return;
    if (hot || bar->pressed == XD_HIT_PEEK)
        xd_fill_rect(
            cairo, strip,
            xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), bar->pressed == XD_HIT_PEEK ? 0.15 : 0.28));
    /* Its edge: a dark line and a light one. */
    int px = xd_tpx(theme, 1);
    if (side) {
        xd_fill_rect(cairo, (xd_rect){strip.x, strip.y, strip.w, px},
                     xd_rgba_alpha(xd_rgba_hex(0x000000), 0.5));
        xd_fill_rect(cairo, (xd_rect){strip.x, strip.y + px, strip.w, px},
                     xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.3));
    } else {
        xd_fill_rect(cairo, (xd_rect){strip.x, strip.y, px, strip.h},
                     xd_rgba_alpha(xd_rgba_hex(0x000000), 0.5));
        xd_fill_rect(cairo, (xd_rect){strip.x + px, strip.y, px, strip.h},
                     xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.3));
    }
}

static void layout_and_draw(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, int dw,
                            int dh) {
    xd_shell *shell = bar->shell;
    xd_edge edge = shell->settings.edge;
    bool side = side_bar(shell);
    xd_theme_draw_taskbar(theme, cairo, (xd_rect){0, 0, dw, dh}, edge);
    if (!shell->settings.locked)
        xd_theme_draw_taskbar_sizing_bar(theme, cairo, (xd_rect){0, 0, dw, dh}, edge);
    xd_rect rect = content_box(theme, edge, dw, dh);
    layout_peek(bar, theme, cairo, &rect, side);
    bool classic = theme->style == XD_STYLE_CLASSIC;
    /* One row: the XP styles' full 30px, Classic's inside its border. */
    int rows = side ? 1 : XD_MAX(1, shell->settings.rows);
    int row_h =
        side ? xd_theme_taskbar_height(theme, large_icons(shell)) - (classic ? xd_tpx(theme, 6) : 0)
             : rect.h / rows;

    /* Start button: held down while its menu is open. */
    int sw = XD_MIN(xd_theme_start_button_width(theme, cairo), rect.w);
    bar->start_rect = (xd_rect){rect.x, rect.y, sw, row_h};
    uint8_t start_state = button_state(bar, XD_HIT_START);
    if ((shell->start_menu != nullptr || shell->start7 != nullptr) && shell->start_bar == bar &&
        !shell->context_menu)
        start_state |= XD_STATE_PRESSED;
    xd_theme_draw_start_button(theme, cairo, bar->start_rect, start_state);

    int gap = xd_tpx(theme, 2);
    if (side) {
        int y = layout_quick_launch(bar, theme, cairo, rect.x, rect.y + row_h + xd_tpx(theme, 4),
                                    row_h, rect.w);
        layout_tray(bar, theme, cairo, rect, 1, row_h);
        int top = y + xd_tpx(theme, 2);
        /* The language indicator sits above the notification area. */
        xd_rect left = xd_langbar_layout(
            bar, theme, cairo, (xd_rect){rect.x, top, rect.w, bar->tray_rect.y - top}, true, row_h);
        layout_tasks(bar, theme, cairo,
                     (xd_rect){rect.x, top, rect.w, left.y + left.h - xd_tpx(theme, 4) - top}, 1,
                     row_h);
        return;
    }
    int x = layout_quick_launch(bar, theme, cairo, rect.x + sw + gap, rect.y, row_h, 0);
    layout_tray(bar, theme, cairo, rect, rows, row_h);
    int x0 = x + xd_tpx(theme, 4);
    /* The language indicator sits left of the notification area. */
    xd_rect left = xd_langbar_layout(
        bar, theme, cairo, (xd_rect){x0, rect.y, bar->tray_rect.x - x0, rect.h}, false, row_h);
    int x1 = left.x + left.w - xd_tpx(theme, 6);
    layout_tasks(bar, theme, cairo, (xd_rect){x0, rect.y, x1 - x0, rect.h}, rows, row_h);
}

static int hit_test(const xd_taskbar *bar, double lx, double ly);

int xd_taskbar_quick_insert_at(const xd_taskbar *bar, double lx, double ly) {
    int x = (int)(lx * bar->scale), y = (int)(ly * bar->scale);
    xd_rect band = bar->quick_band;
    /* A little slack across the band, so the row's edges count. */
    int slack = (int)(4 * bar->scale);
    if (band.w <= 0 || x < band.x - slack || x >= band.x + band.w + slack ||
        y < band.y - slack || y >= band.y + band.h + slack)
        return -1;
    /* Before the first button the point comes before, in reading order
     * (a row, or a side taskbar's grid). */
    x = XD_CLAMP(x, band.x, band.x + band.w - 1);
    y = XD_CLAMP(y, band.y, band.y + band.h - 1);
    size_t i = 0;
    while (i < bar->quick_count) {
        xd_rect rect = bar->quick_rects[i];
        if (y < rect.y || (y < rect.y + rect.h && x < rect.x + rect.w / 2))
            break;
        i++;
    }
    return (int)i;
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

    xd_theme theme;
    bar_theme(bar, &theme);
    cairo_t *cairo = cairo_create(buf->surface);
    layout_and_draw(bar, &theme, cairo, dw, dh);
    cairo_destroy(cairo);
    /* The layout may have moved buttons under a still pointer: hover the
     * right one (drawn next time round). */
    bool again = false;
    if (shell->pointer_bar == bar && bar->pressed == XD_HIT_NONE) {
        int hot = hit_test(bar, bar->pointer_x, bar->pointer_y);
        again = hot != bar->hot;
        bar->hot = hot;
    }
    cairo_surface_flush(buf->surface);

    wl_surface_attach(bar->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(bar->surface, 0, 0, dw, dh);
    if (bar->viewport != nullptr)
        wp_viewport_set_destination(bar->viewport, bar->width, bar->height);
    wl_surface_commit(bar->surface);
    buf->busy = true;
    bar->dirty = again;
}

/* ---- input ------------------------------------------------------------------ */

static bool rect_contains(xd_rect rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x - rect.x < rect.w && y - rect.y < rect.h;
}

static int floor_int(double value) {
    double floored = floor(value);
    if (!(floored > -65536.0 && floored < 65536.0))
        return -1; /* off any button */
    return (int)floored;
}

/* A button's hit area runs to the top and bottom of the bar, so the
 * pointer flung against the screen edge still hits it (and Start reaches
 * the corner), as in Windows. */
static bool hits_column(xd_rect rect, int x, double ly, int bar_h) {
    return x >= rect.x && x < rect.x + rect.w && ly >= 0 && ly < bar_h;
}

static int hit_test(const xd_taskbar *bar, double lx, double ly) {
    int x = floor_int(lx * bar->scale);
    int y = floor_int(ly * bar->scale);
    int height = bar->height; /* logical */
    const xd_settings *settings = &bar->shell->settings;
    bool side = settings->edge == XD_EDGE_LEFT || settings->edge == XD_EDGE_RIGHT;
    /* Start reaches into the screen's corner (Fitts' law, as Windows'). */
    xd_rect start = bar->start_rect;
    int dw = (int)(bar->width * bar->scale), dh = (int)(bar->height * bar->scale);
    if (side) {
        start = (xd_rect){0, 0, dw, start.y + start.h};
    } else {
        start.w += start.x;
        start.x = 0;
        if (settings->rows <= 1) {
            start.y = 0;
            start.h = dh;
        } else if (settings->edge == XD_EDGE_TOP) {
            start.h += start.y;
            start.y = 0;
        } else {
            start.y = 0;
            start.h = start.h + bar->start_rect.y;
        }
    }
    if (rect_contains(start, x, y))
        return XD_HIT_START;
    if (!xd_rect_empty(bar->peek_rect) && rect_contains(bar->peek_rect, x, y))
        return XD_HIT_PEEK;
    for (size_t i = 0; i < bar->tray_count; i++) {
        if (rect_contains(bar->tray_rects[i], x, y))
            return XD_HIT_TRAY_ICON - (int)i;
    }
    if (rect_contains(bar->lang_rect, x, y))
        return XD_HIT_LANG;
    if (rect_contains(bar->tray_chevron, x, y))
        return XD_HIT_TRAY_CHEVRON;
    if (rect_contains(bar->tray_rect, x, y))
        return XD_HIT_TRAY;
    if (bar->pages > 1) {
        if (rect_contains(bar->page_up_rect, x, y))
            return XD_HIT_PAGE_UP;
        if (rect_contains(bar->page_down_rect, x, y))
            return XD_HIT_PAGE_DOWN;
    }
    for (size_t i = 0; i < bar->quick_count; i++) {
        if (rect_contains(bar->quick_rects[i], x, y))
            return XD_HIT_QUICK - (int)i;
    }
    if (rect_contains(bar->quick_band, x, y))
        return XD_HIT_QUICK_BAND;
    /* One row's task buttons reach the screen edge too. */
    bool column = !side && settings->rows <= 1;
    for (size_t i = 0; i < bar->task_count; i++) {
        if (column ? hits_column(bar->task_rects[i], x, ly, height)
                   : rect_contains(bar->task_rects[i], x, y))
            return (int)i;
    }
    return XD_HIT_NONE;
}

/* ---- moving and sizing (unlocked) ---------------------------------------------------- */

static void set_cursor(xd_shell *shell, uint32_t shape) {
    if (shell->cursor_shape == nullptr || shell->cursor == shape)
        return;
    shell->cursor = shape;
    wp_cursor_shape_device_v1_set_shape(shell->cursor_shape, shell->enter_serial, shape);
}

/* Within a few px of the edge facing the desktop: resizing. */
static bool on_inner_edge(const xd_taskbar *bar, double x, double y) {
    double grip = 4;
    switch (bar->shell->settings.edge) {
    case XD_EDGE_TOP:
        return y >= bar->height - grip;
    case XD_EDGE_LEFT:
        return x >= bar->width - grip;
    case XD_EDGE_RIGHT:
        return x < grip;
    case XD_EDGE_BOTTOM:
        break;
    }
    return y < grip;
}

/* The pointer in the output's coordinates (the taskbar's surface is
 * docked to one of its edges). */
static void output_point(const xd_taskbar *bar, double x, double y, double *ox, double *oy) {
    *ox = x;
    *oy = y;
    if (bar->desktop == nullptr)
        return;
    if (bar->shell->settings.edge == XD_EDGE_BOTTOM)
        *oy += bar->desktop->height - bar->height;
    else if (bar->shell->settings.edge == XD_EDGE_RIGHT)
        *ox += bar->desktop->width - bar->width;
}

/* Applies a new edge or size at once (saved when the button comes up). */
static void apply_layout_change(xd_shell *shell) {
    xd_tooltip_hide(shell);
    xd_taskbars_update(shell);
    xd_taskbars_forget_layout(shell);
}

static void drag_bar(xd_taskbar *bar, double x, double y) {
    xd_shell *shell = bar->shell;
    xd_settings *settings = &shell->settings;
    if (bar->grab == XD_BAR_MOVING) {
        /* XP's rule: the screen edge nearest the pointer. */
        if (bar->desktop == nullptr || bar->desktop->width <= 0 || bar->desktop->height <= 0)
            return;
        double ox, oy;
        output_point(bar, x, y, &ox, &oy);
        double desktop_width = bar->desktop->width, desktop_height = bar->desktop->height;
        double distance[] = {[XD_EDGE_BOTTOM] = (desktop_height - oy) / desktop_height,
                             [XD_EDGE_TOP] = oy / desktop_height,
                             [XD_EDGE_LEFT] = ox / desktop_width,
                             [XD_EDGE_RIGHT] = (desktop_width - ox) / desktop_width};
        xd_edge best = XD_EDGE_BOTTOM;
        for (int edge_index = 1; edge_index < 4; edge_index++) {
            if (distance[edge_index] < distance[best])
                best = (xd_edge)edge_index;
        }
        if (best != settings->edge) {
            settings->edge = best;
            apply_layout_change(shell);
        }
        return;
    }
    /* Sizing: rows for top and bottom taskbars, width for side ones. */
    double depth = settings->edge == XD_EDGE_BOTTOM ? bar->height - y
                   : settings->edge == XD_EDGE_TOP  ? y
                   : settings->edge == XD_EDGE_LEFT ? x
                                                    : bar->width - x;
    if (settings->edge == XD_EDGE_LEFT || settings->edge == XD_EDGE_RIGHT) {
        int max = bar->desktop != nullptr ? XD_MAX(bar->desktop->width / 2, 80) : 400;
        int width = XD_CLAMP((int)lround(depth), 60, max);
        if (width != settings->side_width) {
            settings->side_width = width;
            apply_layout_change(shell);
        }
    } else {
        int row = logical_height(shell);
        int extra = shell->style == XD_STYLE_CLASSIC ? row - 6 : row;
        int max = bar->desktop != nullptr ? XD_MAX(bar->desktop->height / 2 / row, 1) : 4;
        int rows = XD_CLAMP(1 + (int)lround((depth - row) / extra), 1, XD_MIN(max, 8));
        if (rows != settings->rows) {
            settings->rows = rows;
            apply_layout_change(shell);
        }
    }
}

static bool end_bar_grab(xd_taskbar *bar) {
    if (bar->grab == XD_BAR_IDLE)
        return false;
    bar->grab = XD_BAR_IDLE;
    if (!xd_settings_save(&bar->shell->settings))
        fprintf(stderr, "xd-shell: cannot save the taskbar's place\n");
    return true;
}

/* Aero Peek through the compositor (xd-window-arranger-v1 version 6). */
static void set_peek(xd_taskbar *bar, bool on) {
    xd_shell *shell = bar->shell;
    if (bar->peek_timer != 0) {
        g_source_remove(bar->peek_timer);
        bar->peek_timer = 0;
    }
    if (bar->peeking == on)
        return;
    bar->peeking = on;
    if (shell->arranger != nullptr &&
        xd_window_arranger_v1_get_version(shell->arranger) >= XD_WINDOW_ARRANGER_V1_PEEK_SINCE_VERSION)
        xd_window_arranger_v1_peek(shell->arranger, on ? 1 : 0);
}

static gboolean peek_now(gpointer data) {
    xd_taskbar *bar = data;
    bar->peek_timer = 0;
    set_peek(bar, true);
    return G_SOURCE_REMOVE;
}

/* Hovering Show desktop peeks after half a second, as in Windows 7. */
static void track_peek(xd_taskbar *bar, int hot) {
    if (hot == XD_HIT_PEEK) {
        if (!bar->peeking && bar->peek_timer == 0)
            bar->peek_timer = g_timeout_add(500, peek_now, bar);
    } else {
        set_peek(bar, false);
    }
}

void xd_taskbar_pointer_motion(xd_taskbar *bar, double x, double y) {
    if (bar->grab != XD_BAR_IDLE) {
        drag_bar(bar, x, y);
        return;
    }
    bar->pointer_x = x;
    bar->pointer_y = y;
    if (!bar->shell->settings.locked) {
        bool side = bar->shell->settings.edge == XD_EDGE_LEFT ||
                    bar->shell->settings.edge == XD_EDGE_RIGHT;
        set_cursor(bar->shell, on_inner_edge(bar, x, y)
                                   ? (side ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE
                                           : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE)
                                   : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    }
    /* A Quick Launch button pressed and pulled sideways: rearranging. */
    if (xd_hit_is_quick(bar->pressed) && bar->quick_drag < 0 && fabs(x - bar->press_x) > 4)
        bar->quick_drag = XD_HIT_QUICK - bar->pressed;
    if (bar->quick_drag >= 0) {
        int insert = xd_taskbar_quick_insert_at(bar, x, y);
        if (insert != bar->drop_insert) {
            bar->drop_insert = insert;
            bar->dirty = true;
        }
        return;
    }
    /* Aero: a program's button pulled along: rearranging. */
    if (xd_superbar_drag(bar, x, y))
        return;
    int hot = hit_test(bar, x, y);
    track_peek(bar, hot);
    if (bar->shell->settings.previews)
        xd_thumbs_hover(bar, hot); /* Windows 7's thumbnails */
    if (hot != bar->hot) {
        bar->hot = hot;
        bar->dirty = true;
        /* A new item: its tooltip after the hover delay. */
        xd_tooltip_hide(bar->shell);
        bar->tooltip_off = false;
        if (hot != XD_HIT_NONE)
            xd_tooltip_arm(bar->shell);
        else
            xd_tooltip_disarm(bar->shell);
    }
}

/* What the tooltip over `hit` says, or nullptr: a task button's whole
 * title when it doesn't fit, a Quick Launch shortcut's name, the date over
 * the clock, and "Click here to begin" over Start, as in XP. */
static char *tooltip_text(xd_taskbar *bar, int hit) {
    xd_shell *shell = bar->shell;
    if (hit == XD_HIT_START)
        return xd_str_dup_cstr(XD_STR("Click here to begin"));
    if (hit == XD_HIT_PEEK)
        return xd_str_dup_cstr(XD_STR("Show desktop"));
    if (hit == XD_HIT_LANG) {
        const char *name = xd_langbar_tooltip(shell);
        return name != nullptr ? xd_str_dup_cstr(xd_str_from_cstr(name)) : nullptr;
    }
    if (hit == XD_HIT_TRAY) {
        time_t now = time(nullptr);
        struct tm tm;
        char buf[128];
        if (localtime_r(&now, &tm) == nullptr ||
            strftime(buf, sizeof(buf), "%A, %B %d, %Y", &tm) == 0)
            return nullptr;
        return xd_str_dup_cstr(xd_str_from_cstr(buf));
    }
    if (hit <= XD_HIT_TRAY_ICON) {
        size_t slot = (size_t)(XD_HIT_TRAY_ICON - hit);
        xd_tray_item *item =
            slot < bar->tray_count ? xd_tray_get(shell, bar->tray_index[slot]) : nullptr;
        char *tip = item != nullptr ? xd_tray_item_tooltip(item) : nullptr;
        char *copy = tip != nullptr ? xd_str_dup_cstr(xd_str_from_cstr(tip)) : nullptr;
        g_free(tip);
        return copy;
    }
    if (xd_hit_is_quick(hit)) {
        size_t i = (size_t)(XD_HIT_QUICK - hit);
        const xd_quicklaunch *ql = &shell->quicklaunch;
        return i < ql->count && ql->items[i].name != nullptr
                   ? xd_str_dup_cstr(xd_str_from_cstr(ql->items[i].name))
                   : nullptr;
    }
    if (xd_superbar_is_button(bar, hit))
        return xd_superbar_tooltip(bar, hit);
    if (hit >= 0 && (size_t)hit < bar->task_count) {
        const xd_toplevel *tl = bar->task_views[hit];
        if (bar->task_counts[hit] > 1)
            return nullptr; /* a group: its menu lists the titles */
        if (shell->settings.previews && xd_thumbs_cover(shell, 1))
            return nullptr; /* its thumbnail says it */
        const char *title = tl->title != nullptr && tl->title[0] != '\0' ? tl->title : tl->app_id;
        if (title == nullptr)
            return nullptr;
        /* Only when the button cuts it short: its text starts after the
         * icon and ends a few px before the edge. */
        xd_theme theme;
        bar_theme(bar, &theme);
        cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *cairo = cairo_create(probe);
        xd_font_spec bold = theme.scheme.fonts[XD_FONT_MENU];
        bold.bold = true; /* the active button's bold text is the widest */
        xd_text_style st = xd_text(&theme, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_START);
        st.font = &bold;
        int text_w = xd_text_width(cairo, title, &st);
        cairo_destroy(cairo);
        cairo_surface_destroy(probe);
        if (text_w <= bar->task_rects[hit].w - xd_tpx(&theme, 3 + 16 + 4 + 8))
            return nullptr;
        return xd_str_dup_cstr(xd_str_from_cstr(title));
    }
    return nullptr;
}

void xd_taskbar_tooltip_timer(xd_shell *shell) {
    xd_taskbar *bar = shell->pointer_bar;
    if (bar == nullptr || bar->hot == XD_HIT_NONE || bar->tooltip_off ||
        bar->pressed != XD_HIT_NONE || shell->start_menu != nullptr || shell->start7 != nullptr)
        return;
    xd_rect rect = {0};
    if (bar->hot == XD_HIT_START)
        rect = bar->start_rect;
    else if (bar->hot == XD_HIT_TRAY)
        rect = bar->tray_rect;
    else if (bar->hot <= XD_HIT_TRAY_ICON &&
             (size_t)(XD_HIT_TRAY_ICON - bar->hot) < bar->tray_count)
        rect = bar->tray_rects[XD_HIT_TRAY_ICON - bar->hot];
    else if (xd_hit_is_quick(bar->hot) && (size_t)(XD_HIT_QUICK - bar->hot) < bar->quick_count)
        rect = bar->quick_rects[XD_HIT_QUICK - bar->hot];
    else if (bar->hot >= 0 && (size_t)bar->hot < bar->task_count)
        rect = bar->task_rects[bar->hot];
    else
        return;
    char *text = tooltip_text(bar, bar->hot);
    if (text == nullptr)
        return;
    /* Rects are device px; the popup anchor is surface-local logical. At
     * the pointer horizontally, like Windows' tooltips. */
    xd_rect anchor = {(int)bar->pointer_x, (int)(rect.y / bar->scale), 1,
                      (int)(rect.h / bar->scale)};
    xd_tooltip_show(shell, bar->layer, anchor, text, bar->scale);
    xd_free(text);
}

void xd_taskbar_page(xd_taskbar *bar, int delta) {
    if (bar->pages <= 1)
        return;
    if (delta < 0 && bar->page > 0)
        bar->page--;
    else if (delta > 0 && bar->page + 1 < bar->pages)
        bar->page++;
    else
        return;
    bar->task_count = 0; /* indices change; hit-testing waits for the redraw */
    bar->hot = XD_HIT_NONE;
    bar->dirty = true;
}

void xd_taskbar_pointer_leave(xd_taskbar *bar) {
    set_peek(bar, false);
    xd_thumbs_hover(bar, XD_HIT_NONE);
    xd_superbar_drop(bar);
    xd_taskbars_schedule_tuck(bar->shell);
    xd_tooltip_hide(bar->shell);
    xd_tooltip_disarm(bar->shell);
    if (bar->hot != XD_HIT_NONE || bar->pressed != XD_HIT_NONE) {
        bar->hot = XD_HIT_NONE;
        bar->pressed = XD_HIT_NONE;
        bar->dirty = true;
    }
}

void xd_taskbar_toggle_start(xd_taskbar *bar, uint32_t serial) {
    xd_shell *shell = bar->shell;
    if ((shell->start_menu != nullptr && !shell->context_menu) || shell->start7 != nullptr)
        xd_menu_close_all(shell);
    else if (shell->settings.start_style != XD_START_CLASSIC)
        xd_start7_open(bar, serial, shell->settings.start_style == XD_START_XP);
    else {
        xd_start_menu_refresh_documents(shell);
        xd_taskbar_open_start_menu(bar, serial);
    }
}

void xd_taskbar_popup_position(const xd_taskbar *bar, xd_rect anchor,
                               struct xdg_positioner *pos) {
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, XD_MAX(anchor.w, 1),
                                   XD_MAX(anchor.h, 1));
    uint32_t slide = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
    switch (bar->shell->settings.edge) {
    case XD_EDGE_TOP: /* down from the button */
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            pos, slide | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
        return;
    case XD_EDGE_LEFT: /* to its right, top-aligned */
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            pos, slide | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X);
        return;
    case XD_EDGE_RIGHT: /* to its left */
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_LEFT);
        xdg_positioner_set_constraint_adjustment(
            pos, slide | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X);
        return;
    case XD_EDGE_BOTTOM:
        break;
    }
    /* Up from the button, as the Start menu always has. */
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos, slide | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
}

void xd_taskbar_open_start_menu(xd_taskbar *bar, uint32_t serial) {
    xd_shell *shell = bar->shell;
    if (shell->start_model == nullptr || !bar->configured)
        return;
    /* start_rect is in device px; the popup anchor is surface-local
     * logical. */
    xd_rect rect = bar->start_rect;
    xd_rect anchor = {(int)(rect.x / bar->scale), (int)(rect.y / bar->scale),
                      (int)(rect.w / bar->scale), (int)(rect.h / bar->scale)};
    /* Close a right-click menu first: closing gives the keyboard back,
     * which must happen before we take it for the Start menu. */
    xd_menu_close_all(shell);
    xd_taskbar_take_keyboard(bar);
    xd_menu_open_root_bar(shell, shell->start_model, bar, anchor, serial);
    shell->start_bar = bar;
    bar->dirty = true;
}

void xd_taskbar_take_keyboard(xd_taskbar *bar) {
    xd_taskbar_reveal(bar);
    /* Take the keyboard while a menu is open so arrows, Esc and typing
     * reach us. This must be committed before the menu's popup grab
     * starts: during a grab the compositor won't move keyboard focus. */
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    wl_surface_commit(bar->surface);
}

/* Shown icons fold away again a while after the chevron opened them. */
static gboolean fold_tray(gpointer data) {
    xd_taskbar *bar = data;
    bar->tray_fold_timer = 0;
    if (bar->shell->pointer_bar == bar && bar->hot <= XD_HIT_TRAY_ICON) {
        bar->tray_fold_timer = g_timeout_add_seconds(5, fold_tray, bar); /* in use */
        return G_SOURCE_REMOVE;
    }
    bar->tray_expanded = false;
    bar->dirty = true;
    return G_SOURCE_REMOVE;
}

void xd_taskbar_pointer_button(xd_taskbar *bar, uint32_t button, bool pressed,
                               uint32_t serial) {
    /* What's under the pointer now: buttons move without the pointer
     * moving when windows open or close. */
    bar->hot = hit_test(bar, bar->pointer_x, bar->pointer_y);
    /* Clicking puts tooltips away until the pointer moves to another item. */
    xd_tooltip_hide(bar->shell);
    xd_tooltip_disarm(bar->shell);
    bar->tooltip_off = true;
    if (bar->hot <= XD_HIT_TRAY_ICON) {
        /* Notification area icons act on release: left activates, middle
         * is the secondary action, right is the icon's menu. */
        size_t i = (size_t)(XD_HIT_TRAY_ICON - bar->hot);
        xd_tray_item *item =
            i < bar->tray_count ? xd_tray_get(bar->shell, bar->tray_index[i]) : nullptr;
        if (pressed || item == nullptr || i >= bar->tray_count)
            return;
        xd_rect rect = bar->tray_rects[i];
        xd_rect anchor = {(int)(rect.x / bar->scale), (int)(rect.y / bar->scale),
                          (int)(rect.w / bar->scale), (int)(rect.h / bar->scale)};
        uint32_t which = button == BTN_RIGHT    ? XD_TRAY_BUTTON_RIGHT
                         : button == BTN_MIDDLE ? XD_TRAY_BUTTON_MIDDLE
                                                : XD_TRAY_BUTTON_LEFT;
        xd_tray_item_click(bar->shell, item, which, bar, anchor, serial);
        return;
    }
    if (bar->hot == XD_HIT_LANG) {
        /* The language indicator's menus open on release. */
        bar->pressed = pressed ? XD_HIT_LANG : XD_HIT_NONE;
        bar->dirty = true;
        if (!pressed)
            xd_langbar_click(bar, button, serial);
        return;
    }
    if (button == BTN_RIGHT) {
        /* Right-click (opened on release, like Windows): the Start button's
         * menu, a task button's window menu, or the taskbar's menu. */
        if (!pressed)
            xd_taskbar_context_menu(bar, bar->hot, bar->pointer_x, bar->pointer_y, serial);
        return;
    }
    if (button == BTN_MIDDLE && xd_superbar_is_button(bar, bar->hot)) {
        /* Aero: another window of the program, on release. */
        if (!pressed)
            xd_superbar_click(bar, bar->hot, button);
        return;
    }
    if (button != BTN_LEFT)
        return;
    if (!pressed && end_bar_grab(bar))
        return;
    if (pressed && !bar->shell->settings.locked &&
        (on_inner_edge(bar, bar->pointer_x, bar->pointer_y) || bar->hot == XD_HIT_NONE)) {
        /* Unlocked: the inner edge resizes, empty space moves it. */
        bar->grab = on_inner_edge(bar, bar->pointer_x, bar->pointer_y) ? XD_BAR_SIZING
                                                                       : XD_BAR_MOVING;
        return;
    }
    if (pressed) {
        bar->pressed = bar->hot;
        bar->press_x = bar->pointer_x;
        bar->press_y = bar->pointer_y;
        bar->dirty = true;
        /* The Start menu opens on press, like Windows'. */
        if (bar->hot == XD_HIT_START)
            xd_taskbar_toggle_start(bar, serial);
        return;
    }
    int target = bar->pressed;
    bar->pressed = XD_HIT_NONE;
    bar->dirty = true;
    if (xd_superbar_drop(bar))
        return; /* a program's button dragged to its new place */
    if (bar->quick_drag >= 0) {
        /* Dropped along Quick Launch: there it goes (elsewhere: nothing). */
        if (bar->drop_insert >= 0)
            xd_quicklaunch_move(bar->shell, (size_t)bar->quick_drag, (size_t)bar->drop_insert);
        bar->quick_drag = -1;
        bar->drop_insert = -1;
        return;
    }
    if (target != bar->hot)
        return; /* released somewhere else: cancel, like a real button */
    if (target == XD_HIT_PEEK) {
        /* A click on Show desktop: the desktop, for real. */
        set_peek(bar, false);
        xd_show_desktop_toggle(bar->shell);
        return;
    }
    if (target == XD_HIT_TRAY_CHEVRON) {
        bar->tray_expanded = !bar->tray_expanded;
        if (bar->tray_fold_timer != 0)
            g_source_remove(bar->tray_fold_timer);
        bar->tray_fold_timer =
            bar->tray_expanded ? g_timeout_add_seconds(15, fold_tray, bar) : 0;
        return;
    }
    if (target == XD_HIT_TRAY) {
        /* Double-clicking the clock: Date and Time Properties. */
        int64_t now = g_get_monotonic_time() / 1000;
        if (now - bar->clock_click_ms <= bar->shell->settings.mouse.double_click_ms) {
            bar->clock_click_ms = 0;
            xd_shell_launch(bar->shell, xd_shell_own_command(bar->shell, "xd-timedate", nullptr));
        } else {
            bar->clock_click_ms = now;
        }
        return;
    }
    if (target == XD_HIT_PAGE_UP || target == XD_HIT_PAGE_DOWN) {
        xd_taskbar_page(bar, target == XD_HIT_PAGE_UP ? -1 : 1);
        return;
    }
    if (xd_superbar_is_button(bar, target))
        xd_superbar_click(bar, target, button);
    else if (target >= 0 && (size_t)target < bar->task_count && bar->task_counts[target] > 1)
        xd_taskbar_group_menu(bar, target, serial);
    else if (target >= 0 && (size_t)target < bar->task_count)
        xd_toplevel_click(bar->task_views[target]);
    else if (xd_hit_is_quick(target))
        xd_quicklaunch_activate(bar->shell, (size_t)(XD_HIT_QUICK - target));
}

void xd_taskbars_forget(xd_shell *shell, xd_toplevel *toplevel) {
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        for (size_t i = 0; i < bar->task_count; i++) {
            /* Aero's buttons stand for several windows each: any of them. */
            if (bar->task_views[i] == toplevel || bar->superbar) {
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

static void preferred_scale(void *data,
                            [[maybe_unused]] struct wp_fractional_scale_v1 *fractional_scale,
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

/* Layer, exclusive zone and position from [Taskbar]: on top of windows
 * or under them, and with Auto-hide pushed down to a 2px strip with no
 * reserved space. */
/* The taskbar's thickness (logical px): a side taskbar's width, or one
 * row's height plus a button row for each extra row. */
int xd_taskbar_thickness(const xd_shell *shell) {
    const xd_settings *settings = &shell->settings;
    if (settings->edge == XD_EDGE_LEFT || settings->edge == XD_EDGE_RIGHT)
        return settings->side_width;
    int row = logical_height(shell);
    int extra = shell->style == XD_STYLE_CLASSIC ? row - 6 : row;
    return row + (XD_MAX(settings->rows, 1) - 1) * extra;
}

static void apply_options(xd_taskbar *bar) {
    const xd_settings *settings = &bar->shell->settings;
    int height = xd_taskbar_thickness(bar->shell);
    bool side = settings->edge == XD_EDGE_LEFT || settings->edge == XD_EDGE_RIGHT;
    static const uint32_t anchors[] = {
        [XD_EDGE_BOTTOM] = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
        [XD_EDGE_TOP] = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
        [XD_EDGE_LEFT] = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
        [XD_EDGE_RIGHT] = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
    };
    zwlr_layer_surface_v1_set_anchor(bar->layer, anchors[settings->edge]);
    zwlr_layer_surface_v1_set_size(bar->layer, side ? (uint32_t)height : 0,
                                   side ? 0 : (uint32_t)height);
    if (zwlr_layer_surface_v1_get_version(bar->layer) >=
        ZWLR_LAYER_SURFACE_V1_SET_LAYER_SINCE_VERSION)
        zwlr_layer_surface_v1_set_layer(bar->layer, settings->on_top || settings->auto_hide
                                                        ? ZWLR_LAYER_SHELL_V1_LAYER_TOP
                                                        : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer, settings->auto_hide ? 0 : height);
    /* Auto-hide: pushed off its edge but for a 2px strip. */
    int out = settings->auto_hide && bar->tucked ? -(height - 2) : 0;
    zwlr_layer_surface_v1_set_margin(bar->layer, settings->edge == XD_EDGE_TOP ? out : 0,
                                     settings->edge == XD_EDGE_RIGHT ? out : 0,
                                     settings->edge == XD_EDGE_BOTTOM ? out : 0,
                                     settings->edge == XD_EDGE_LEFT ? out : 0);
    wl_surface_commit(bar->surface);
}

static void surface_create(xd_taskbar *bar) {
    xd_shell *shell = bar->shell;
    bar->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        bar->fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(shell->fractional_manager,
                                                                bar->surface);
        wp_fractional_scale_v1_add_listener(bar->fractional, &fractional_listener, bar);
        bar->viewport = wp_viewporter_get_viewport(shell->viewporter, bar->surface);
    }

    bar->layer = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, bar->surface, bar->output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "taskbar");
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        bar->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(bar->layer, &layer_listener, bar);
    bar->tucked = shell->settings.auto_hide;
    apply_options(bar);
}

static void surface_destroy(xd_taskbar *bar) {
    xd_shell *shell = bar->shell;
    if (bar->surface == nullptr)
        return;
    if (shell->start_bar == bar)
        xd_menu_close_all(shell); /* its popups hang off this surface */
    if (shell->pointer_bar == bar)
        shell->pointer_bar = nullptr; /* no leave will come */
    set_peek(bar, false);              /* ...to end a peek either */
    xd_thumbs_bar_gone(shell, bar);
    xd_tooltip_hide(shell);
    if (bar->layer != nullptr)
        zwlr_layer_surface_v1_destroy(bar->layer);
    if (bar->viewport != nullptr)
        wp_viewport_destroy(bar->viewport);
    if (bar->fractional != nullptr)
        wp_fractional_scale_v1_destroy(bar->fractional);
    wl_surface_destroy(bar->surface);
    xd_shm_ring_finish(&bar->ring);
    bar->layer = nullptr;
    bar->viewport = nullptr;
    bar->fractional = nullptr;
    bar->surface = nullptr;
    bar->configured = false;
    bar->task_count = bar->quick_count = 0;
    bar->hot = bar->pressed = XD_HIT_NONE;
}

/* ---- auto-hide ----------------------------------------------------------- */

enum { TUCK_DELAY_MS = 700 };

/* Something keeps the taskbar out: the pointer, or a menu opened from it. */
static bool in_use(const xd_taskbar *bar) {
    return bar->shell->pointer_bar == bar || bar->shell->start_bar == bar;
}

void xd_taskbar_reveal(xd_taskbar *bar) {
    if (!bar->tucked)
        return;
    bar->tucked = false;
    if (bar->surface != nullptr)
        apply_options(bar);
}

void xd_taskbars_schedule_tuck(xd_shell *shell) {
    if (!shell->settings.auto_hide || shell->autohide_timer_fd < 0)
        return;
    struct itimerspec spec = {.it_value = {.tv_nsec = TUCK_DELAY_MS * 1000000L}};
    timerfd_settime(shell->autohide_timer_fd, 0, &spec, nullptr);
}

void xd_taskbars_tuck_timer(xd_shell *shell) {
    if (!shell->settings.auto_hide)
        return;
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        if (bar->surface != nullptr && !bar->tucked && !in_use(bar)) {
            bar->tucked = true;
            xd_tooltip_hide(shell);
            apply_options(bar);
        }
    }
}

/* The first monitor's taskbar always exists; the others only with
 * "Show the taskbar on all displays". */
void xd_taskbars_update(xd_shell *shell) {
    bool first = true;
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        bool want = first || shell->settings.all_displays;
        first = false;
        bar->tucked = shell->settings.auto_hide && !in_use(bar);
        if (want && bar->surface == nullptr)
            surface_create(bar);
        else if (!want)
            surface_destroy(bar);
        else
            apply_options(bar);
    }
}

void xd_taskbar_create(xd_shell *shell, struct wl_output *output, uint32_t global) {
    xd_taskbar *bar = XD_NEW(xd_taskbar);
    bar->shell = shell;
    bar->output = output;
    bar->output_global = global;
    bar->scale = 1.0;
    bar->hot = XD_HIT_NONE;
    bar->pressed = XD_HIT_NONE;
    bar->drop_insert = -1;
    bar->quick_drag = -1;
    wl_list_insert(shell->taskbars.prev, &bar->link);
    bar->desktop = xd_desktop_create(shell, output);
    xd_taskbars_update(shell);
}

void xd_taskbar_destroy(xd_taskbar *bar) {
    xd_shell *shell = bar->shell;
    xd_shellbus_taskbar_gone(shell, bar);
    surface_destroy(bar);
    if (bar->peek_timer != 0)
        g_source_remove(bar->peek_timer);
    wl_list_remove(&bar->link);
    xd_desktop_destroy(bar->desktop);
    if (bar->output != nullptr)
        wl_output_release(bar->output);
    xd_free(bar->task_rects);
    xd_free(bar->task_views);
    xd_free(bar->task_counts);
    xd_free(bar->tray_rects);
    xd_free(bar->tray_index);
    if (bar->tray_fold_timer != 0)
        g_source_remove(bar->tray_fold_timer);
    xd_free(bar->entry_views);
    xd_free(bar->entry_counts);
    xd_free(bar->bar_buttons);
    if (bar->bar_keys != nullptr)
        g_string_chunk_free(bar->bar_keys);
    g_free(bar->drag_key);
    xd_free(bar->quick_rects);
    xd_free(bar);
    /* If it was the first monitor's, the next one takes over. */
    if (shell->running)
        xd_taskbars_update(shell);
}
