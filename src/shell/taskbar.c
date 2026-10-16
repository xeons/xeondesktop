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

static void ensure_task_capacity(xd_taskbar *bar, size_t n) {
    if (n <= bar->task_cap)
        return;
    size_t cap = XD_MAX(n, bar->task_cap * 2);
    bar->task_rects = xd_realloc_array(bar->task_rects, cap, sizeof(*bar->task_rects));
    bar->task_views = xd_realloc_array(bar->task_views, cap, sizeof(*bar->task_views));
    bar->task_counts = xd_realloc_array(bar->task_counts, cap, sizeof(*bar->task_counts));
    bar->entry_views = xd_realloc_array(bar->entry_views, cap, sizeof(*bar->entry_views));
    bar->entry_counts = xd_realloc_array(bar->entry_counts, cap, sizeof(*bar->entry_counts));
    bar->task_cap = cap;
}

/* The small down arrow at the right end of a group's button. */
static void draw_group_arrow(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    int a = xd_tpx(t, 3);
    double cx = r.x + r.w - xd_tpx(t, 10);
    double cy = r.y + r.h / 2.0 - a / 2.0;
    if (state & (XD_STATE_CHECKED | XD_STATE_PRESSED) && t->style == XD_STYLE_CLASSIC) {
        cx += xd_tpx(t, 1);
        cy += xd_tpx(t, 1);
    }
    xd_rgba c = t->style == XD_STYLE_CLASSIC ? xd_sys(t, XD_COLOR_BUTTON_TEXT)
                                             : xd_rgba_hex(0xFFFFFF);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_move_to(cr, cx - a, cy);
    cairo_line_to(cr, cx + a + 1, cy);
    cairo_line_to(cr, cx + 0.5, cy + a + 1);
    cairo_close_path(cr);
    cairo_fill(cr);
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
    if ((shell->start_menu != nullptr || shell->start7 != nullptr) && shell->start_bar == bar &&
        !shell->context_menu)
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
        /* A locked taskbar has no grips, as in XP. */
        bool locked = shell->settings.locked;
        int grip = locked ? xd_tpx(t, 3) : xd_theme_toolbar_grip_width(t);
        int qy = classic ? inset_top : xd_tpx(t, 3);
        int qh = classic ? button_h : dh - xd_tpx(t, 6);
        if (!locked)
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
        if (!locked)
            xd_theme_draw_toolbar_grip(t, cr, (xd_rect){qx + xd_tpx(t, 2), qy, grip, qh});
        after_start = qx + xd_tpx(t, 2) + grip;
    }

    /* Notification area, sized to the clock like Windows does. */
    xd_text_style clock_style = xd_text(t, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_END);
    int margin_l, margin_r;
    xd_theme_tray_margins(t, &margin_l, &margin_r);
    if (shell->settings.show_clock) {
        int tray_w = XD_MAX(xd_tpx(t, 60), margin_l +
                                               xd_text_width(cr, shell->clock, &clock_style) +
                                               xd_tpx(t, 1) + margin_r);
        bar->tray_rect = (xd_rect){dw - tray_w - pad, inset_top, tray_w, button_h};
        xd_theme_draw_tray(t, cr, bar->tray_rect, shell->clock);
    } else {
        /* No clock and (as yet) no icons: no notification area at all. */
        bar->tray_rect = (xd_rect){dw - pad, inset_top, 0, button_h};
    }

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
    int min_w = xd_tpx(t, XD_TASK_MIN_W);
    int ty = classic ? inset_top : xd_tpx(t, 3);
    int th = classic ? button_h : dh - xd_tpx(t, 5);

    /* One entry per window; when crowded, XP's "Group similar taskbar
     * buttons" folds each program's windows into one button where its
     * first window was. */
    xd_toplevel **entries = bar->entry_views;
    size_t *counts = bar->entry_counts;
    size_t ne = 0;
    XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
        entries[ne] = tl;
        counts[ne++] = 1;
    }
    if (shell->settings.group && (avail - ((int)ne - 1) * gap) / (int)ne <
                                     xd_tpx(t, XD_TASK_GROUP_W)) {
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
        ne = out;
    }
    int nei = (int)XD_MIN(ne, (size_t)INT32_MAX / 2);
    int w = XD_MIN(xd_tpx(t, XD_TASK_MAX_W), (avail - (nei - 1) * gap) / nei);

    /* Still too many to fit: pages of buttons with up/down arrows at the
     * end, like XP's one-row taskbar. The active window's page is shown
     * when it changes. */
    size_t per_page = ne;
    bar->pages = 1;
    bar->page_up_rect = bar->page_down_rect = (xd_rect){0};
    if (w < min_w) {
        int arrows_w = xd_theme_dropdown_button_width(t);
        int room = avail - arrows_w - gap;
        per_page = (size_t)XD_MAX(1, (room + gap) / (min_w + gap));
        int pi = (int)per_page;
        w = (room - (pi - 1) * gap) / pi;
        bar->pages = (ne + per_page - 1) / per_page;
        uint64_t active_id = 0;
        size_t active = SIZE_MAX;
        for (size_t i = 0; i < ne; i++) {
            XD_LIST_FOR_EACH(tl, &shell->toplevels, xd_toplevel, link) {
                if (tl->activated && !tl->minimized &&
                    (tl == entries[i] ||
                     (counts[i] > 1 && tl->app_id != nullptr &&
                      strcmp(tl->app_id, entries[i]->app_id) == 0))) {
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
        int ax = x1 - arrows_w;
        int half = th / 2;
        bar->page_up_rect = (xd_rect){ax, ty, arrows_w, half};
        bar->page_down_rect = (xd_rect){ax, ty + half, arrows_w, th - half};
        uint8_t up_state = button_state(bar, XD_HIT_PAGE_UP);
        uint8_t down_state = button_state(bar, XD_HIT_PAGE_DOWN);
        if (bar->page == 0)
            up_state = XD_STATE_DISABLED;
        if (bar->page + 1 >= bar->pages)
            down_state = XD_STATE_DISABLED;
        /* The combo box's arrow button, flipped for "up". */
        xd_rect u = bar->page_up_rect;
        cairo_save(cr);
        cairo_translate(cr, 0, 2 * u.y + u.h);
        cairo_scale(cr, 1, -1);
        xd_theme_draw_dropdown_button(t, cr, u, up_state);
        cairo_restore(cr);
        xd_theme_draw_dropdown_button(t, cr, bar->page_down_rect, down_state);
        avail = room;
    } else {
        bar->page = 0;
    }

    cairo_save(cr);
    cairo_rectangle(cr, x0, 0, XD_MAX(avail, 0), dh);
    cairo_clip(cr);
    int x = x0;
    for (size_t e = bar->page * per_page; e < ne && bar->task_count < per_page; e++) {
        xd_toplevel *tl = entries[e];
        size_t i = bar->task_count++;
        xd_rect r = {x, ty, w, th};
        bar->task_rects[i] = r;
        bar->task_views[i] = tl;
        bar->task_counts[i] = counts[e];

        uint8_t state = button_state(bar, (int)i);
        char *group_label = nullptr;
        const char *label = tl->title != nullptr && tl->title[0] != '\0' ? tl->title
                            : tl->app_id != nullptr                      ? tl->app_id
                                                                          : "";
        if (counts[e] > 1) {
            /* "3 Firefox": pressed while one of them is active. */
            bool any_active = false, any_flash = false;
            XD_LIST_FOR_EACH(m, &shell->toplevels, xd_toplevel, link) {
                if (m->app_id == nullptr || strcmp(m->app_id, tl->app_id) != 0)
                    continue;
                any_active = any_active || (m->activated && !m->minimized);
                any_flash = any_flash || (m->attention && m->flashes % 2 == 0);
            }
            if (any_active)
                state |= XD_STATE_CHECKED;
            else if (any_flash)
                state |= XD_STATE_FLASH;
            xd_strbuf sb = {0};
            xd_strbuf_appendf(&sb, "%zu %s", counts[e],
                              tl->app_name != nullptr ? tl->app_name : tl->app_id);
            group_label = xd_str_dup_cstr(xd_strbuf_view(&sb));
            xd_strbuf_free(&sb);
            label = group_label;
        } else if (tl->activated && !tl->minimized) {
            state |= XD_STATE_CHECKED;
        } else if (tl->attention && tl->flashes % 2 == 0) {
            state |= XD_STATE_FLASH;
        }
        cairo_surface_t *icon =
            tl->icon != nullptr ? xd_icons_get(shell->icons, tl->icon, xd_tpx(t, 16)) : nullptr;
        xd_theme_draw_task_button(t, cr, r, state, label, icon);
        if (counts[e] > 1)
            draw_group_arrow(t, cr, r, state);
        xd_free(group_label);
        x += w + gap;
    }
    cairo_restore(cr);
}

static int hit_test(const xd_taskbar *bar, double lx, double ly);

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

static bool rect_contains(xd_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h;
}

static int floor_int(double v) {
    double f = floor(v);
    if (!(f > -65536.0 && f < 65536.0))
        return -1; /* off any button */
    return (int)f;
}

/* A button's hit area runs to the top and bottom of the bar, so the
 * pointer flung against the screen edge still hits it (and Start reaches
 * the corner), as in Windows. */
static bool hits_column(xd_rect r, int x, double ly, int bar_h) {
    return x >= r.x && x < r.x + r.w && ly >= 0 && ly < bar_h;
}

static int hit_test(const xd_taskbar *bar, double lx, double ly) {
    int x = floor_int(lx * bar->scale);
    int y = floor_int(ly * bar->scale);
    int h = bar->height; /* logical */
    xd_rect start = bar->start_rect;
    start.w += start.x; /* out to the left edge */
    start.x = 0;
    if (hits_column(start, x, ly, h))
        return XD_HIT_START;
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
    for (size_t i = 0; i < bar->task_count; i++) {
        if (hits_column(bar->task_rects[i], x, ly, h))
            return (int)i;
    }
    return XD_HIT_NONE;
}

void xd_taskbar_pointer_motion(xd_taskbar *bar, double x, double y) {
    bar->pointer_x = x;
    bar->pointer_y = y;
    int hot = hit_test(bar, x, y);
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
    if (hit == XD_HIT_TRAY) {
        time_t now = time(nullptr);
        struct tm tm;
        char buf[128];
        if (localtime_r(&now, &tm) == nullptr ||
            strftime(buf, sizeof(buf), "%A, %B %d, %Y", &tm) == 0)
            return nullptr;
        return xd_str_dup_cstr(xd_str_from_cstr(buf));
    }
    if (hit <= XD_HIT_QUICK) {
        size_t i = (size_t)(XD_HIT_QUICK - hit);
        const xd_quicklaunch *ql = &shell->quicklaunch;
        return i < ql->count && ql->items[i].name != nullptr
                   ? xd_str_dup_cstr(xd_str_from_cstr(ql->items[i].name))
                   : nullptr;
    }
    if (hit >= 0 && (size_t)hit < bar->task_count) {
        const xd_toplevel *tl = bar->task_views[hit];
        if (bar->task_counts[hit] > 1)
            return nullptr; /* a group: its menu lists the titles */
        const char *title = tl->title != nullptr && tl->title[0] != '\0' ? tl->title : tl->app_id;
        if (title == nullptr)
            return nullptr;
        /* Only when the button cuts it short: its text starts after the
         * icon and ends a few px before the edge. */
        xd_theme t;
        bar_theme(bar, &t);
        cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *cr = cairo_create(probe);
        xd_font_spec bold = t.scheme.fonts[XD_FONT_MENU];
        bold.bold = true; /* the active button's bold text is the widest */
        xd_text_style st = xd_text(&t, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_START);
        st.font = &bold;
        int text_w = xd_text_width(cr, title, &st);
        cairo_destroy(cr);
        cairo_surface_destroy(probe);
        if (text_w <= bar->task_rects[hit].w - xd_tpx(&t, 3 + 16 + 4 + 8))
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
    xd_rect r = {0};
    if (bar->hot == XD_HIT_START)
        r = bar->start_rect;
    else if (bar->hot == XD_HIT_TRAY)
        r = bar->tray_rect;
    else if (bar->hot <= XD_HIT_QUICK && (size_t)(XD_HIT_QUICK - bar->hot) < bar->quick_count)
        r = bar->quick_rects[XD_HIT_QUICK - bar->hot];
    else if (bar->hot >= 0 && (size_t)bar->hot < bar->task_count)
        r = bar->task_rects[bar->hot];
    else
        return;
    char *text = tooltip_text(bar, bar->hot);
    if (text == nullptr)
        return;
    /* Rects are device px; the popup anchor is surface-local logical. At
     * the pointer horizontally, like Windows' tooltips. */
    xd_rect anchor = {(int)bar->pointer_x, (int)(r.y / bar->scale), 1,
                      (int)(r.h / bar->scale)};
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
    else
        xd_taskbar_open_start_menu(bar, serial);
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
    /* Close a right-click menu first: closing gives the keyboard back,
     * which must happen before we take it for the Start menu. */
    xd_menu_close_all(shell);
    xd_taskbar_take_keyboard(bar);
    xd_menu_open_root(shell, shell->start_model, bar->layer, anchor, serial);
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

void xd_taskbar_pointer_button(xd_taskbar *bar, uint32_t button, bool pressed,
                               uint32_t serial) {
    /* What's under the pointer now: buttons move without the pointer
     * moving when windows open or close. */
    bar->hot = hit_test(bar, bar->pointer_x, bar->pointer_y);
    /* Clicking puts tooltips away until the pointer moves to another item. */
    xd_tooltip_hide(bar->shell);
    xd_tooltip_disarm(bar->shell);
    bar->tooltip_off = true;
    if (button == BTN_RIGHT) {
        /* Right-click (opened on release, like Windows): the Start button's
         * menu, a task button's window menu, or the taskbar's menu. */
        if (!pressed)
            xd_taskbar_context_menu(bar, bar->hot, bar->pointer_x, bar->pointer_y, serial);
        return;
    }
    if (button != BTN_LEFT)
        return;
    if (pressed) {
        bar->pressed = bar->hot;
        bar->dirty = true;
        /* The Start menu opens on press, like Windows'. */
        if (bar->hot == XD_HIT_START)
            xd_taskbar_toggle_start(bar, serial);
        return;
    }
    int target = bar->pressed;
    bar->pressed = XD_HIT_NONE;
    bar->dirty = true;
    if (target != bar->hot)
        return; /* released somewhere else: cancel, like a real button */
    if (target == XD_HIT_PAGE_UP || target == XD_HIT_PAGE_DOWN) {
        xd_taskbar_page(bar, target == XD_HIT_PAGE_UP ? -1 : 1);
        return;
    }
    if (target >= 0 && (size_t)target < bar->task_count && bar->task_counts[target] > 1)
        xd_taskbar_group_menu(bar, target, serial);
    else if (target >= 0 && (size_t)target < bar->task_count)
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

/* Layer, exclusive zone and position from [Taskbar]: on top of windows
 * or under them, and with Auto-hide pushed down to a 2px strip with no
 * reserved space. */
static void apply_options(xd_taskbar *bar) {
    const xd_settings *s = &bar->shell->settings;
    int h = logical_height(bar->shell);
    zwlr_layer_surface_v1_set_size(bar->layer, 0, (uint32_t)h);
    if (zwlr_layer_surface_v1_get_version(bar->layer) >=
        ZWLR_LAYER_SURFACE_V1_SET_LAYER_SINCE_VERSION)
        zwlr_layer_surface_v1_set_layer(bar->layer, s->on_top || s->auto_hide
                                                        ? ZWLR_LAYER_SHELL_V1_LAYER_TOP
                                                        : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer, s->auto_hide ? 0 : h);
    bool tucked = s->auto_hide && bar->tucked;
    zwlr_layer_surface_v1_set_margin(bar->layer, 0, 0, tucked ? -(h - 2) : 0, 0);
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
    int h = logical_height(shell);
    zwlr_layer_surface_v1_set_anchor(bar->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(bar->layer, 0, (uint32_t)h);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer, h);
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
        shell->pointer_bar = nullptr;
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
    wl_list_insert(shell->taskbars.prev, &bar->link);
    bar->desktop = xd_desktop_create(shell, output);
    xd_taskbars_update(shell);
}

void xd_taskbar_destroy(xd_taskbar *bar) {
    xd_shell *shell = bar->shell;
    surface_destroy(bar);
    wl_list_remove(&bar->link);
    xd_desktop_destroy(bar->desktop);
    if (bar->output != nullptr)
        wl_output_release(bar->output);
    xd_free(bar->task_rects);
    xd_free(bar->task_views);
    xd_free(bar->task_counts);
    xd_free(bar->entry_views);
    xd_free(bar->entry_counts);
    xd_free(bar->quick_rects);
    xd_free(bar);
    /* If it was the first monitor's, the next one takes over. */
    if (shell->running)
        xd_taskbars_update(shell);
}
