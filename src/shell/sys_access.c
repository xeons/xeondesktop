/* SPDX-License-Identifier: MIT
 * XP's on-screen status of the accessibility features: an icon in the
 * notification area while StickyKeys, FilterKeys or MouseKeys is on and
 * its "Show ... status on screen" is checked ([Accessibility]
 * *Indicator). Clicking one opens Accessibility Options at its tab.
 *
 * XP's StickyKeys icon also showed which modifiers were latched, and
 * MouseKeys' which button it clicks with; those live in the compositor,
 * which doesn't tell the shell, so these icons only say the feature is on.
 */
#include <glib.h>
#include <math.h>

#include "shell.h"

typedef enum which : uint8_t { STICKY, FILTER, MOUSE } which;

typedef struct indicator {
    xd_shell *shell;
    which what;
} indicator;

static bool shown(const indicator *in) {
    const xd_access_settings *access_settings = &in->shell->settings.access;
    switch (in->what) {
    case STICKY:
        return access_settings->sticky_keys && access_settings->sticky_indicator;
    case FILTER:
        return access_settings->filter_keys && access_settings->filter_indicator;
    case MOUSE:
        return access_settings->mouse_keys && access_settings->mouse_keys_indicator;
    }
    return false;
}

static const char *icon(void *data) {
    indicator *in = data;
    /* Drawn by draw(); the name only says it's shown. */
    return shown(in) ? "preferences-desktop-accessibility" : nullptr;
}

static const char *const names[] = {"StickyKeys", "FilterKeys", "MouseKeys"};

static char *tooltip(void *data) {
    indicator *in = data;
    return g_strdup(names[in->what]);
}

/* Accessibility Options at the feature's tab (Keyboard or Mouse). */
static void open_options(xd_shell *shell, void *data) {
    indicator *in = data;
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-access",
                                                in->what == MOUSE ? "--page 3" : "--page 0"));
}

static void activate(void *data, [[maybe_unused]] xd_taskbar *bar, [[maybe_unused]] xd_rect anchor,
                     [[maybe_unused]] uint32_t serial) {
    indicator *in = data;
    open_options(in->shell, in);
}

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *menu_model) {
    xd_menu_item *it =
        xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Accessibility Options", nullptr);
    it->action = open_options;
    it->action_data = data;
    it->bold = true;
}

/* A rounded box, filled white and outlined black (the icons are drawn
 * in black and white, as XP's were). */
static void key_cap(cairo_t *cairo, double x, double y, double width, double height, double line) {
    double radius = height / 5;
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, x + width - radius, y + radius, radius, -M_PI / 2, 0);
    cairo_arc(cairo, x + width - radius, y + height - radius, radius, 0, M_PI / 2);
    cairo_arc(cairo, x + radius, y + height - radius, radius, M_PI / 2, M_PI);
    cairo_arc(cairo, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(cairo);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_set_line_width(cairo, line);
    cairo_stroke(cairo);
}

static void draw(void *data, [[maybe_unused]] const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    indicator *in = data;
    double unit = rect.w / 16.0; /* drawn on a 16-unit grid */
    double line = fmax(1.0, unit);
    cairo_save(cairo);
    cairo_translate(cairo, rect.x, rect.y);
    switch (in->what) {
    case STICKY:
        /* Shift over Ctrl and Alt. */
        key_cap(cairo, 1.5 * unit, 1.5 * unit, 13 * unit, 6 * unit, line);
        key_cap(cairo, 1.5 * unit, 8.5 * unit, 6 * unit, 6 * unit, line);
        key_cap(cairo, 8.5 * unit, 8.5 * unit, 6 * unit, 6 * unit, line);
        break;
    case FILTER:
        /* A stopwatch. */
        cairo_set_line_width(cairo, line);
        cairo_arc(cairo, 8 * unit, 9.5 * unit, 5.5 * unit, 0, 2 * M_PI);
        cairo_set_source_rgb(cairo, 1, 1, 1);
        cairo_fill_preserve(cairo);
        cairo_set_source_rgb(cairo, 0, 0, 0);
        cairo_stroke(cairo);
        cairo_rectangle(cairo, 6.5 * unit, 1 * unit, 3 * unit, 2 * unit);
        cairo_fill(cairo);
        cairo_move_to(cairo, 8 * unit, 9.5 * unit);
        cairo_line_to(cairo, 8 * unit, 5.5 * unit);
        cairo_move_to(cairo, 8 * unit, 9.5 * unit);
        cairo_line_to(cairo, 10.5 * unit, 11 * unit);
        cairo_stroke(cairo);
        break;
    case MOUSE:
        /* A mouse: its body, and the line between its buttons. */
        key_cap(cairo, 3.5 * unit, 1.5 * unit, 9 * unit, 13 * unit, line);
        cairo_move_to(cairo, 3.5 * unit, 6.5 * unit);
        cairo_line_to(cairo, 12.5 * unit, 6.5 * unit);
        cairo_move_to(cairo, 8 * unit, 1.5 * unit);
        cairo_line_to(cairo, 8 * unit, 6.5 * unit);
        cairo_stroke(cairo);
        break;
    }
    cairo_restore(cairo);
}

static void destroy(void *data) { g_free(data); }

static const xd_sysicon_ops ops = {
    .icon = icon,
    .tooltip = tooltip,
    .menu = menu,
    .activate = activate,
    .draw = draw,
    .destroy = destroy,
};

void xd_sys_access_init(xd_shell *shell) {
    static const char *const ids[] = {"stickykeys", "filterkeys", "mousekeys"};
    for (size_t i = 0; i < XD_ARRAY_LEN(ids); i++) {
        indicator *in = g_new0(indicator, 1);
        *in = (indicator){shell, (which)i};
        xd_tray_add_sysicon(shell, ids[i], &ops, in);
    }
}
