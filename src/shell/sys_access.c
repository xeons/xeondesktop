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
    const xd_access_settings *a = &in->shell->settings.access;
    switch (in->what) {
    case STICKY:
        return a->sticky_keys && a->sticky_indicator;
    case FILTER:
        return a->filter_keys && a->filter_indicator;
    case MOUSE:
        return a->mouse_keys && a->mouse_keys_indicator;
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

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *m) {
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, "Accessibility Options", nullptr);
    it->action = open_options;
    it->action_data = data;
    it->bold = true;
}

/* A rounded box, filled white and outlined black (the icons are drawn
 * in black and white, as XP's were). */
static void key_cap(cairo_t *cr, double x, double y, double w, double h, double line) {
    double r = h / 5;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, line);
    cairo_stroke(cr);
}

static void draw(void *data, [[maybe_unused]] const xd_theme *t, cairo_t *cr, xd_rect r) {
    indicator *in = data;
    double u = r.w / 16.0; /* drawn on a 16-unit grid */
    double line = fmax(1.0, u);
    cairo_save(cr);
    cairo_translate(cr, r.x, r.y);
    switch (in->what) {
    case STICKY:
        /* Shift over Ctrl and Alt. */
        key_cap(cr, 1.5 * u, 1.5 * u, 13 * u, 6 * u, line);
        key_cap(cr, 1.5 * u, 8.5 * u, 6 * u, 6 * u, line);
        key_cap(cr, 8.5 * u, 8.5 * u, 6 * u, 6 * u, line);
        break;
    case FILTER:
        /* A stopwatch. */
        cairo_set_line_width(cr, line);
        cairo_arc(cr, 8 * u, 9.5 * u, 5.5 * u, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_stroke(cr);
        cairo_rectangle(cr, 6.5 * u, 1 * u, 3 * u, 2 * u);
        cairo_fill(cr);
        cairo_move_to(cr, 8 * u, 9.5 * u);
        cairo_line_to(cr, 8 * u, 5.5 * u);
        cairo_move_to(cr, 8 * u, 9.5 * u);
        cairo_line_to(cr, 10.5 * u, 11 * u);
        cairo_stroke(cr);
        break;
    case MOUSE:
        /* A mouse: its body, and the line between its buttons. */
        key_cap(cr, 3.5 * u, 1.5 * u, 9 * u, 13 * u, line);
        cairo_move_to(cr, 3.5 * u, 6.5 * u);
        cairo_line_to(cr, 12.5 * u, 6.5 * u);
        cairo_move_to(cr, 8 * u, 1.5 * u);
        cairo_line_to(cr, 8 * u, 6.5 * u);
        cairo_stroke(cr);
        break;
    }
    cairo_restore(cr);
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
