/* SPDX-License-Identifier: MIT
 * The notification area's own icons, drawn in the spirit of XP's (no
 * Microsoft artwork): the speaker, the two monitors of a network
 * connection, Wi-Fi signal bars, the power meter's battery and the
 * Bluetooth badge. Drawn as vectors on a 16x16 grid, so they're sharp at
 * any scale.
 */
#include <math.h>

#include "theme.h"

static void rgb(cairo_t *cr, uint32_t hex) {
    cairo_set_source_rgb(cr, ((hex >> 16) & 0xFF) / 255.0, ((hex >> 8) & 0xFF) / 255.0,
                         (hex & 0xFF) / 255.0);
}

static void vgrad(cairo_t *cr, double y0, double y1, uint32_t top, uint32_t bottom) {
    cairo_pattern_t *p = cairo_pattern_create_linear(0, y0, 0, y1);
    cairo_pattern_add_color_stop_rgb(p, 0, ((top >> 16) & 0xFF) / 255.0,
                                     ((top >> 8) & 0xFF) / 255.0, (top & 0xFF) / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, ((bottom >> 16) & 0xFF) / 255.0,
                                     ((bottom >> 8) & 0xFF) / 255.0, (bottom & 0xFF) / 255.0);
    cairo_set_source(cr, p);
    cairo_pattern_destroy(p);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -XD_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, XD_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, XD_PI / 2, XD_PI);
    cairo_arc(cr, x + r, y + r, r, XD_PI, 3 * XD_PI / 2);
    cairo_close_path(cr);
}

/* A red "no" badge (circle with a slash) in the bottom-right corner. */
static void badge_off(cairo_t *cr) {
    cairo_arc(cr, 11.5, 11.5, 3.6, 0, 2 * XD_PI);
    rgb(cr, 0xFFFFFF);
    cairo_fill_preserve(cr);
    rgb(cr, 0xD42020);
    cairo_set_line_width(cr, 1.4);
    cairo_stroke(cr);
    cairo_move_to(cr, 9.4, 13.6);
    cairo_line_to(cr, 13.6, 9.4);
    cairo_stroke(cr);
}

/* A red X with a white outline, for "disconnected". */
static void badge_x(cairo_t *cr) {
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int pass = 0; pass < 2; pass++) {
        cairo_set_line_width(cr, pass == 0 ? 3.4 : 1.8);
        rgb(cr, pass == 0 ? 0xFFFFFF : 0xD42020);
        cairo_move_to(cr, 9.5, 9.5);
        cairo_line_to(cr, 14.5, 14.5);
        cairo_move_to(cr, 14.5, 9.5);
        cairo_line_to(cr, 9.5, 14.5);
        cairo_stroke(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static void speaker(cairo_t *cr, int level, bool muted) {
    /* Body and cone, light grey with a dark outline. */
    cairo_move_to(cr, 1.5, 5.5);
    cairo_line_to(cr, 4.5, 5.5);
    cairo_line_to(cr, 8.5, 2.0);
    cairo_line_to(cr, 8.5, 14.0);
    cairo_line_to(cr, 4.5, 10.5);
    cairo_line_to(cr, 1.5, 10.5);
    cairo_close_path(cr);
    vgrad(cr, 2, 14, 0xF4F4F0, 0xA8A8A0);
    cairo_fill_preserve(cr);
    rgb(cr, 0x3C3C3C);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_rectangle(cr, 1.5, 5.5, 3, 5);
    rgb(cr, 0x5A5A5A);
    cairo_fill(cr);
    if (muted) {
        badge_off(cr);
        return;
    }
    /* Sound waves: one to three by loudness, dark enough for Luna's blue. */
    rgb(cr, 0x303030);
    cairo_set_line_width(cr, 1.2);
    for (int i = 0; i < level && i < 3; i++) {
        double r = 2.6 + 2.2 * i;
        cairo_arc(cr, 8.5, 8.0, r, -XD_PI / 3.2, XD_PI / 3.2);
        cairo_stroke(cr);
    }
}

static void monitor(cairo_t *cr, double x, double y, bool lit) {
    rounded(cr, x, y, 9, 7, 0.8);
    vgrad(cr, y, y + 7, 0xD8D8D0, 0x989890);
    cairo_fill_preserve(cr);
    rgb(cr, 0x404040);
    cairo_set_line_width(cr, 0.9);
    cairo_stroke(cr);
    cairo_rectangle(cr, x + 1.4, y + 1.4, 6.2, 4.2);
    if (lit)
        vgrad(cr, y + 1.4, y + 5.6, 0x7CC0FF, 0x1F5FC8);
    else
        rgb(cr, 0x505860);
    cairo_fill(cr);
    cairo_rectangle(cr, x + 3.5, y + 7, 2, 1.5);
    rgb(cr, 0x606060);
    cairo_fill(cr);
}

static void wired(cairo_t *cr, bool connected) {
    monitor(cr, 0.5, 0.5, connected);
    monitor(cr, 6.0, 6.0, connected);
    if (!connected)
        badge_x(cr);
}

static void wireless(cairo_t *cr, int level, bool connected) {
    /* Five rising bars, green up to the signal level. */
    for (int i = 0; i < 5; i++) {
        double h = 3.0 + 2.6 * i;
        double x = 1.0 + 3.0 * i;
        cairo_rectangle(cr, x, 15.0 - h, 2.2, h);
        if (connected && i < level)
            vgrad(cr, 15.0 - h, 15.0, 0x8CE06A, 0x2E8B12);
        else
            rgb(cr, 0xB8B8B0);
        cairo_fill_preserve(cr);
        rgb(cr, 0x2C3A2C);
        cairo_set_line_width(cr, 0.7);
        cairo_stroke(cr);
    }
    if (!connected)
        badge_x(cr);
}

static void battery(cairo_t *cr, int level, bool charging) {
    /* Upright battery: tip, body, and the charge from the bottom. */
    cairo_rectangle(cr, 6.0, 1.0, 4.0, 1.6);
    rgb(cr, 0x505050);
    cairo_fill(cr);
    rounded(cr, 3.5, 2.5, 9.0, 13.0, 1.0);
    vgrad(cr, 2.5, 15.5, 0xF8F8F4, 0xC8C8C0);
    cairo_fill_preserve(cr);
    rgb(cr, 0x383838);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    double fill = 11.0 * XD_CLAMP(level, 0, 100) / 100.0;
    cairo_rectangle(cr, 5.0, 14.0 - fill, 6.0, fill);
    uint32_t top = level > 20 ? 0x8CE06A : level > 10 ? 0xFFE060 : 0xFF7060;
    uint32_t bottom = level > 20 ? 0x2E8B12 : level > 10 ? 0xC09000 : 0xC01818;
    vgrad(cr, 14.0 - fill, 14.0, top, bottom);
    cairo_fill(cr);
    if (charging) {
        /* A lightning bolt over it. */
        cairo_move_to(cr, 9.6, 3.6);
        cairo_line_to(cr, 5.4, 9.4);
        cairo_line_to(cr, 8.0, 9.4);
        cairo_line_to(cr, 6.4, 14.4);
        cairo_line_to(cr, 10.8, 8.0);
        cairo_line_to(cr, 8.2, 8.0);
        cairo_close_path(cr);
        rgb(cr, 0xFFD820);
        cairo_fill_preserve(cr);
        rgb(cr, 0x705000);
        cairo_set_line_width(cr, 0.7);
        cairo_stroke(cr);
    }
}

static void bluetooth(cairo_t *cr, bool on) {
    rounded(cr, 2.5, 0.5, 11.0, 15.0, 5.0);
    if (on)
        vgrad(cr, 0.5, 15.5, 0x5A9CFF, 0x0A3EA8);
    else
        vgrad(cr, 0.5, 15.5, 0xC8C8C8, 0x7A7A7A);
    cairo_fill_preserve(cr);
    rgb(cr, on ? 0x062A70 : 0x505050);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);
    /* The rune. */
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, 5.2, 5.0);
    cairo_line_to(cr, 10.6, 10.4);
    cairo_line_to(cr, 8.0, 13.0);
    cairo_line_to(cr, 8.0, 3.0);
    cairo_line_to(cr, 10.6, 5.6);
    cairo_line_to(cr, 5.2, 11.0);
    rgb(cr, 0xFFFFFF);
    cairo_set_line_width(cr, 1.3);
    cairo_stroke(cr);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
}

void xd_theme_draw_tray_icon(const xd_theme *t, cairo_t *cr, xd_rect r, xd_tray_icon icon,
                             int level, uint8_t flags) {
    (void)t;
    cairo_save(cr);
    cairo_translate(cr, r.x, r.y);
    cairo_scale(cr, r.w / 16.0, r.h / 16.0);
    bool off = flags & XD_TRAY_ICON_OFF;
    switch (icon) {
    case XD_TRAY_ICON_VOLUME:
        speaker(cr, level, off);
        break;
    case XD_TRAY_ICON_WIRED:
        wired(cr, !off);
        break;
    case XD_TRAY_ICON_WIRELESS:
        wireless(cr, level, !off);
        break;
    case XD_TRAY_ICON_BATTERY:
        battery(cr, level, flags & XD_TRAY_ICON_CHARGING);
        break;
    case XD_TRAY_ICON_BLUETOOTH:
        bluetooth(cr, !off);
        break;
    }
    cairo_restore(cr);
}
