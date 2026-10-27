/* SPDX-License-Identifier: MIT
 * The virtual desktops' pager: a small button per desktop, numbered,
 * left of the notification area (and the language indicator), the one
 * showing pushed in, as XP PowerToys' Virtual Desktop Manager put its
 * four buttons in the taskbar. Windows had no virtual desktops of its
 * own; this shows only when Desktop Effects' General Options make more
 * than one. A click switches to that desktop through the compositor
 * (xd-window-arranger-v1's switch_desktop), which tells back which one
 * shows.
 */
#include <linux/input-event-codes.h>

#include "shell.h"

/* At most this many buttons: a bigger grid is still reached by its keys. */
enum { MAX_BUTTONS = 16 };

static uint32_t pager_count_buttons(const xd_shell *shell) {
    if (shell->arranger == nullptr ||
        xd_window_arranger_v1_get_version(shell->arranger) <
            XD_WINDOW_ARRANGER_V1_SWITCH_DESKTOP_SINCE_VERSION)
        return 0;
    uint32_t count = shell->desktop_columns * shell->desktop_rows;
    return count > 1 ? XD_MIN(count, (uint32_t)MAX_BUTTONS) : 0;
}

/* Button `index`'s place within the pager (device px). */
static xd_rect pager_get_button(const xd_taskbar *bar, uint32_t count, uint32_t index,
                                bool side) {
    xd_rect rect = bar->pager_rect;
    if (side) {
        int height = rect.h / (int)count;
        return (xd_rect){rect.x, rect.y + height * (int)index, rect.w, height};
    }
    int width = rect.w / (int)count;
    return (xd_rect){rect.x + width * (int)index, rect.y, width, rect.h};
}

static bool pager_is_side(const xd_taskbar *bar) {
    return bar->pager_rect.h > bar->pager_rect.w;
}

/* Which button (device px), or -1. */
static int pager_find_button(const xd_taskbar *bar, double x, double y) {
    uint32_t count = pager_count_buttons(bar->shell);
    for (uint32_t i = 0; i < count; i++) {
        xd_rect rect = pager_get_button(bar, count, i, pager_is_side(bar));
        if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h)
            return (int)i;
    }
    return -1;
}

xd_rect xd_pager_layout(xd_taskbar *bar, const xd_theme *theme, cairo_t *cairo, xd_rect room,
                        bool side, int row_h) {
    xd_shell *shell = bar->shell;
    bar->pager_rect = (xd_rect){0};
    uint32_t count = pager_count_buttons(shell);
    if (count == 0)
        return room;
    int button = XD_MIN(row_h, xd_tpx(theme, 22));
    int gap = xd_tpx(theme, 2);
    if (side) {
        int height = XD_MIN(button * (int)count, room.h);
        bar->pager_rect = (xd_rect){room.x, room.y + room.h - height, room.w, height};
        room.h -= height + gap;
    } else {
        int width = XD_MIN(button * (int)count, room.w);
        bar->pager_rect =
            (xd_rect){room.x + room.w - width, room.y + (XD_MIN(row_h, room.h) - button) / 2,
                      width, XD_MIN(button, room.h)};
        room.w -= width + gap;
    }
    if (bar->pager_rect.w <= 0 || bar->pager_rect.h <= 0) {
        bar->pager_rect = (xd_rect){0};
        return room;
    }
    int hot = bar->hot == XD_HIT_PAGER ? pager_find_button(bar, bar->pointer_x * bar->scale,
                                                            bar->pointer_y * bar->scale)
                                       : -1;
    xd_text_style text = xd_text(
        theme, XD_FONT_MENU,
        xd_rgba_hex(theme->style == XD_STYLE_CLASSIC ? 0x000000 : 0xFFFFFF), XD_ALIGN_CENTER);
    for (uint32_t i = 0; i < count; i++) {
        xd_rect rect = pager_get_button(bar, count, i, side);
        uint8_t state = XD_STATE_NORMAL;
        if (i == shell->desktop_current)
            state |= XD_STATE_PRESSED;
        else if ((int)i == hot)
            state |= XD_STATE_HOT;
        xd_theme_draw_toolbar_button(theme, cairo, rect, state);
        char number[16];
        snprintf(number, sizeof(number), "%u", i + 1);
        xd_draw_text(cairo, rect, number, &text);
    }
    return room;
}

void xd_pager_click(xd_taskbar *bar, uint32_t button, double x, double y) {
    xd_shell *shell = bar->shell;
    if (button != BTN_LEFT || pager_count_buttons(shell) == 0)
        return;
    int index = pager_find_button(bar, x, y);
    if (index >= 0 && (uint32_t)index != shell->desktop_current)
        xd_window_arranger_v1_switch_desktop(shell->arranger, (uint32_t)index);
}

char *xd_pager_get_tooltip(xd_taskbar *bar, double x, double y) {
    int index = pager_find_button(bar, x, y);
    if (index < 0)
        return nullptr;
    char text[32];
    snprintf(text, sizeof(text), "Workspace %d", index + 1);
    return xd_str_dup_cstr(xd_str_from_cstr(text));
}
