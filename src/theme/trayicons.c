/* SPDX-License-Identifier: MIT
 * The notification area's own icons, drawn in the spirit of XP's (no
 * Microsoft artwork): the speaker, the two monitors of a network
 * connection, Wi-Fi signal bars, the power meter's battery and the
 * Bluetooth badge. Drawn as vectors on a 16x16 grid, so they're sharp at
 * any scale.
 */
#include <math.h>

#include "theme.h"

static void rgb(cairo_t *cairo, uint32_t hex) {
    cairo_set_source_rgb(cairo, ((hex >> 16) & 0xFF) / 255.0, ((hex >> 8) & 0xFF) / 255.0,
                         (hex & 0xFF) / 255.0);
}

static void vgrad(cairo_t *cairo, double y0, double y1, uint32_t top, uint32_t bottom) {
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, y0, 0, y1);
    cairo_pattern_add_color_stop_rgb(pattern, 0, ((top >> 16) & 0xFF) / 255.0,
                                     ((top >> 8) & 0xFF) / 255.0, (top & 0xFF) / 255.0);
    cairo_pattern_add_color_stop_rgb(pattern, 1, ((bottom >> 16) & 0xFF) / 255.0,
                                     ((bottom >> 8) & 0xFF) / 255.0, (bottom & 0xFF) / 255.0);
    cairo_set_source(cairo, pattern);
    cairo_pattern_destroy(pattern);
}

static void rounded(cairo_t *cairo, double x, double y, double width, double height,
                    double radius) {
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, x + width - radius, y + radius, radius, -XD_PI / 2, 0);
    cairo_arc(cairo, x + width - radius, y + height - radius, radius, 0, XD_PI / 2);
    cairo_arc(cairo, x + radius, y + height - radius, radius, XD_PI / 2, XD_PI);
    cairo_arc(cairo, x + radius, y + radius, radius, XD_PI, 3 * XD_PI / 2);
    cairo_close_path(cairo);
}

/* A red "no" badge (circle with a slash) in the bottom-right corner. */
static void badge_off(cairo_t *cairo) {
    cairo_arc(cairo, 11.5, 11.5, 3.6, 0, 2 * XD_PI);
    rgb(cairo, 0xFFFFFF);
    cairo_fill_preserve(cairo);
    rgb(cairo, 0xD42020);
    cairo_set_line_width(cairo, 1.4);
    cairo_stroke(cairo);
    cairo_move_to(cairo, 9.4, 13.6);
    cairo_line_to(cairo, 13.6, 9.4);
    cairo_stroke(cairo);
}

/* A red X with a white outline, for "disconnected". */
static void badge_x(cairo_t *cairo) {
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    for (int pass = 0; pass < 2; pass++) {
        cairo_set_line_width(cairo, pass == 0 ? 3.4 : 1.8);
        rgb(cairo, pass == 0 ? 0xFFFFFF : 0xD42020);
        cairo_move_to(cairo, 9.5, 9.5);
        cairo_line_to(cairo, 14.5, 14.5);
        cairo_move_to(cairo, 14.5, 9.5);
        cairo_line_to(cairo, 9.5, 14.5);
        cairo_stroke(cairo);
    }
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_BUTT);
}

static void speaker(cairo_t *cairo, int level, bool muted) {
    /* Body and cone, light grey with a dark outline. */
    cairo_move_to(cairo, 1.5, 5.5);
    cairo_line_to(cairo, 4.5, 5.5);
    cairo_line_to(cairo, 8.5, 2.0);
    cairo_line_to(cairo, 8.5, 14.0);
    cairo_line_to(cairo, 4.5, 10.5);
    cairo_line_to(cairo, 1.5, 10.5);
    cairo_close_path(cairo);
    vgrad(cairo, 2, 14, 0xF4F4F0, 0xA8A8A0);
    cairo_fill_preserve(cairo);
    rgb(cairo, 0x3C3C3C);
    cairo_set_line_width(cairo, 1.0);
    cairo_stroke(cairo);
    cairo_rectangle(cairo, 1.5, 5.5, 3, 5);
    rgb(cairo, 0x5A5A5A);
    cairo_fill(cairo);
    if (muted) {
        badge_off(cairo);
        return;
    }
    /* Sound waves: one to three by loudness, dark enough for Luna's blue. */
    rgb(cairo, 0x303030);
    cairo_set_line_width(cairo, 1.2);
    for (int i = 0; i < level && i < 3; i++) {
        double radius = 2.6 + 2.2 * i;
        cairo_arc(cairo, 8.5, 8.0, radius, -XD_PI / 3.2, XD_PI / 3.2);
        cairo_stroke(cairo);
    }
}

static void monitor(cairo_t *cairo, double x, double y, bool lit) {
    rounded(cairo, x, y, 9, 7, 0.8);
    vgrad(cairo, y, y + 7, 0xD8D8D0, 0x989890);
    cairo_fill_preserve(cairo);
    rgb(cairo, 0x404040);
    cairo_set_line_width(cairo, 0.9);
    cairo_stroke(cairo);
    cairo_rectangle(cairo, x + 1.4, y + 1.4, 6.2, 4.2);
    if (lit)
        vgrad(cairo, y + 1.4, y + 5.6, 0x7CC0FF, 0x1F5FC8);
    else
        rgb(cairo, 0x505860);
    cairo_fill(cairo);
    cairo_rectangle(cairo, x + 3.5, y + 7, 2, 1.5);
    rgb(cairo, 0x606060);
    cairo_fill(cairo);
}

static void wired(cairo_t *cairo, bool connected) {
    monitor(cairo, 0.5, 0.5, connected);
    monitor(cairo, 6.0, 6.0, connected);
    if (!connected)
        badge_x(cairo);
}

static void wireless(cairo_t *cairo, int level, bool connected) {
    /* Five rising bars, green up to the signal level. */
    for (int i = 0; i < 5; i++) {
        double height = 3.0 + 2.6 * i;
        double x = 1.0 + 3.0 * i;
        cairo_rectangle(cairo, x, 15.0 - height, 2.2, height);
        if (connected && i < level)
            vgrad(cairo, 15.0 - height, 15.0, 0x8CE06A, 0x2E8B12);
        else
            rgb(cairo, 0xB8B8B0);
        cairo_fill_preserve(cairo);
        rgb(cairo, 0x2C3A2C);
        cairo_set_line_width(cairo, 0.7);
        cairo_stroke(cairo);
    }
    if (!connected)
        badge_x(cairo);
}

static void battery(cairo_t *cairo, int level, bool charging) {
    /* Upright battery: tip, body, and the charge from the bottom. */
    cairo_rectangle(cairo, 6.0, 1.0, 4.0, 1.6);
    rgb(cairo, 0x505050);
    cairo_fill(cairo);
    rounded(cairo, 3.5, 2.5, 9.0, 13.0, 1.0);
    vgrad(cairo, 2.5, 15.5, 0xF8F8F4, 0xC8C8C0);
    cairo_fill_preserve(cairo);
    rgb(cairo, 0x383838);
    cairo_set_line_width(cairo, 1.0);
    cairo_stroke(cairo);
    double fill = 11.0 * XD_CLAMP(level, 0, 100) / 100.0;
    cairo_rectangle(cairo, 5.0, 14.0 - fill, 6.0, fill);
    uint32_t top = level > 20 ? 0x8CE06A : level > 10 ? 0xFFE060 : 0xFF7060;
    uint32_t bottom = level > 20 ? 0x2E8B12 : level > 10 ? 0xC09000 : 0xC01818;
    vgrad(cairo, 14.0 - fill, 14.0, top, bottom);
    cairo_fill(cairo);
    if (charging) {
        /* A lightning bolt over it. */
        cairo_move_to(cairo, 9.6, 3.6);
        cairo_line_to(cairo, 5.4, 9.4);
        cairo_line_to(cairo, 8.0, 9.4);
        cairo_line_to(cairo, 6.4, 14.4);
        cairo_line_to(cairo, 10.8, 8.0);
        cairo_line_to(cairo, 8.2, 8.0);
        cairo_close_path(cairo);
        rgb(cairo, 0xFFD820);
        cairo_fill_preserve(cairo);
        rgb(cairo, 0x705000);
        cairo_set_line_width(cairo, 0.7);
        cairo_stroke(cairo);
    }
}

static void bluetooth(cairo_t *cairo, bool on) {
    rounded(cairo, 2.5, 0.5, 11.0, 15.0, 5.0);
    if (on)
        vgrad(cairo, 0.5, 15.5, 0x5A9CFF, 0x0A3EA8);
    else
        vgrad(cairo, 0.5, 15.5, 0xC8C8C8, 0x7A7A7A);
    cairo_fill_preserve(cairo);
    rgb(cairo, on ? 0x062A70 : 0x505050);
    cairo_set_line_width(cairo, 0.8);
    cairo_stroke(cairo);
    /* The rune. */
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cairo, 5.2, 5.0);
    cairo_line_to(cairo, 10.6, 10.4);
    cairo_line_to(cairo, 8.0, 13.0);
    cairo_line_to(cairo, 8.0, 3.0);
    cairo_line_to(cairo, 10.6, 5.6);
    cairo_line_to(cairo, 5.2, 11.0);
    rgb(cairo, 0xFFFFFF);
    cairo_set_line_width(cairo, 1.3);
    cairo_stroke(cairo);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_MITER);
}

void xd_theme_draw_tray_icon(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_tray_icon icon,
                             int level, uint8_t flags) {
    (void)theme;
    cairo_save(cairo);
    cairo_translate(cairo, rect.x, rect.y);
    cairo_scale(cairo, rect.w / 16.0, rect.h / 16.0);
    bool off = flags & XD_TRAY_ICON_OFF;
    switch (icon) {
    case XD_TRAY_ICON_VOLUME:
        speaker(cairo, level, off);
        break;
    case XD_TRAY_ICON_WIRED:
        wired(cairo, !off);
        break;
    case XD_TRAY_ICON_WIRELESS:
        wireless(cairo, level, !off);
        break;
    case XD_TRAY_ICON_BATTERY:
        battery(cairo, level, flags & XD_TRAY_ICON_CHARGING);
        break;
    case XD_TRAY_ICON_BLUETOOTH:
        bluetooth(cairo, !off);
        break;
    }
    cairo_restore(cairo);
}
