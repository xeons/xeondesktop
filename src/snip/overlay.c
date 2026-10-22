/* SPDX-License-Identifier: MIT
 * Choosing what to snip, as Windows 7 does: the screen is frozen and
 * washed white; a free-form snip is drawn around in the ink colour, a
 * rectangular one dragged out (it shows clear inside), and a window snip
 * picks the window under the pointer (or the monitor, on the desktop).
 * Esc cancels. A full-screen snip takes the whole desktop at once.
 *
 * One layer-shell panel covers each monitor, above everything. A drag
 * may cross monitors: the panel it started on keeps the pointer (the
 * implicit grab) and says where it went in its own coordinates.
 */
#include <gtk-layer-shell.h>
#include <string.h>

#include "snip.h"

/* How white the frozen screen is washed, and the selection ink's width
 * while choosing (logical px). */
static constexpr double WASH_ALPHA = 0.45;
static constexpr double INK_WIDTH = 2.0;

typedef struct overlay overlay;

typedef struct overlay_panel {
    overlay *owner;
    GtkWidget *window;
    GdkRectangle geometry; /* its monitor, in the desktop */
} overlay_panel;

struct overlay {
    snip_app *app;
    xd_snip_screen *screen;
    xd_snip_mode mode;
    GPtrArray *panels; /* overlay_panel */
    bool dragging;
    xd_snip_point start, current;
    GArray *path; /* xd_snip_point: the free-form line */
    bool has_target; /* window snip: what a click would take */
    xd_snip_box target;
};

static xd_snip_box selection_box(const overlay *over) {
    if (over->mode == XD_SNIP_FREE_FORM)
        return xd_snip_box_around((const xd_snip_point *)(void *)over->path->data, over->path->len);
    if (over->mode == XD_SNIP_WINDOW)
        return over->target;
    return xd_snip_box_from_corners(over->start, over->current);
}

static void redraw(overlay *over) {
    for (guint i = 0; i < over->panels->len; i++) {
        overlay_panel *panel = g_ptr_array_index(over->panels, i);
        gtk_widget_queue_draw(panel->window);
    }
}

static gboolean draw_panel(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    overlay_panel *panel = data;
    overlay *over = panel->owner;
    const xd_snip_screen *screen = over->screen;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    /* Desktop coordinates from here on. */
    cairo_translate(cairo, -panel->geometry.x, -panel->geometry.y);

    cairo_save(cairo);
    cairo_translate(cairo, screen->origin_x, screen->origin_y);
    cairo_scale(cairo, 1 / screen->scale, 1 / screen->scale);
    cairo_set_source_surface(cairo, screen->image, 0, 0);
    cairo_paint(cairo);
    cairo_restore(cairo);

    /* A rectangle being dragged, or the window a click would take, shows
     * clear through the wash. */
    bool clear = (over->mode == XD_SNIP_RECTANGLE && over->dragging) ||
                 (over->mode == XD_SNIP_WINDOW && over->has_target);
    xd_snip_box box = selection_box(over);
    if (over->app->options.show_overlay) {
        cairo_rectangle(cairo, panel->geometry.x, panel->geometry.y, width, height);
        if (clear) {
            cairo_rectangle(cairo, box.x, box.y, box.width, box.height);
            cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_EVEN_ODD);
        }
        cairo_set_source_rgba(cairo, 1, 1, 1, WASH_ALPHA);
        cairo_fill(cairo);
    }

    const double *ink = over->app->options.ink;
    cairo_set_source_rgb(cairo, ink[0], ink[1], ink[2]);
    cairo_set_line_width(cairo, INK_WIDTH);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    if (over->mode == XD_SNIP_FREE_FORM && over->path->len > 0) {
        const xd_snip_point *points = (const xd_snip_point *)(void *)over->path->data;
        for (guint i = 0; i < over->path->len; i++)
            cairo_line_to(cairo, points[i].x, points[i].y);
        cairo_stroke(cairo);
    } else if (clear) {
        double half = INK_WIDTH / 2;
        cairo_rectangle(cairo, box.x + half, box.y + half, box.width - INK_WIDTH,
                        box.height - INK_WIDTH);
        cairo_stroke(cairo);
    }
    return TRUE;
}

static void close_overlay(overlay *over, cairo_surface_t *snip) {
    double scale = over->screen->scale;
    for (guint i = 0; i < over->panels->len; i++) {
        overlay_panel *panel = g_ptr_array_index(over->panels, i);
        gtk_widget_destroy(panel->window);
    }
    g_ptr_array_unref(over->panels);
    g_array_unref(over->path);
    xd_snip_screen_free(over->screen);
    snip_app *app = over->app;
    g_free(over);
    snip_app_snip_taken(app, snip, scale);
}

/* The monitor at a point of the desktop, as a box. */
static bool monitor_box_at(const overlay *over, double x, double y, xd_snip_box *box) {
    for (guint i = 0; i < over->panels->len; i++) {
        const overlay_panel *panel = g_ptr_array_index(over->panels, i);
        const GdkRectangle *geometry = &panel->geometry;
        if (x >= geometry->x && x < geometry->x + geometry->width && y >= geometry->y &&
            y < geometry->y + geometry->height) {
            *box = (xd_snip_box){geometry->x, geometry->y, geometry->width, geometry->height};
            return true;
        }
    }
    return false;
}

static void point_window_at(overlay *over, xd_snip_point point) {
    const xd_snip_screen *screen = over->screen;
    int index = xd_snip_window_at(screen->windows, screen->window_count, point.x, point.y);
    xd_snip_box target;
    bool has_target;
    if (index >= 0) {
        const xd_snip_window *window = &screen->windows[index];
        target = (xd_snip_box){window->x, window->y, window->width, window->height};
        has_target = true;
    } else {
        has_target = monitor_box_at(over, point.x, point.y, &target);
    }
    if (has_target == over->has_target &&
        (!has_target || memcmp(&target, &over->target, sizeof(target)) == 0))
        return;
    over->has_target = has_target;
    over->target = target;
    redraw(over);
}

static xd_snip_point desktop_point(const overlay_panel *panel, double x, double y) {
    return (xd_snip_point){panel->geometry.x + x, panel->geometry.y + y};
}

static gboolean on_press([[maybe_unused]] GtkWidget *widget, GdkEventButton *event, gpointer data) {
    overlay_panel *panel = data;
    overlay *over = panel->owner;
    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY)
        return TRUE;
    xd_snip_point point = desktop_point(panel, event->x, event->y);
    over->dragging = true;
    over->start = point;
    over->current = point;
    g_array_set_size(over->path, 0);
    g_array_append_val(over->path, point);
    if (over->mode == XD_SNIP_WINDOW)
        point_window_at(over, point);
    redraw(over);
    return TRUE;
}

static gboolean on_motion([[maybe_unused]] GtkWidget *widget, GdkEventMotion *event, gpointer data) {
    overlay_panel *panel = data;
    overlay *over = panel->owner;
    xd_snip_point point = desktop_point(panel, event->x, event->y);
    if (over->mode == XD_SNIP_WINDOW) {
        point_window_at(over, point);
        return TRUE;
    }
    if (!over->dragging)
        return TRUE;
    over->current = point;
    if (over->mode == XD_SNIP_FREE_FORM)
        g_array_append_val(over->path, point);
    redraw(over);
    return TRUE;
}

static gboolean on_release([[maybe_unused]] GtkWidget *widget, GdkEventButton *event, gpointer data) {
    overlay_panel *panel = data;
    overlay *over = panel->owner;
    if (event->button != GDK_BUTTON_PRIMARY || !over->dragging)
        return TRUE;
    over->dragging = false;
    xd_snip_point point = desktop_point(panel, event->x, event->y);
    over->current = point;
    if (over->mode == XD_SNIP_WINDOW)
        point_window_at(over, point);
    xd_snip_box box = selection_box(over);
    bool taken = over->mode == XD_SNIP_WINDOW ? over->has_target : xd_snip_box_is_selection(box);
    if (!taken) {
        /* A click alone: keep waiting for a real selection. */
        g_array_set_size(over->path, 0);
        redraw(over);
        return TRUE;
    }
    const xd_snip_options *options = &over->app->options;
    bool free_form = over->mode == XD_SNIP_FREE_FORM;
    cairo_surface_t *snip = xd_snip_screen_cut(
        over->screen, box, free_form ? (const xd_snip_point *)(void *)over->path->data : nullptr,
        free_form ? over->path->len : 0, options->show_selection_ink ? options->ink : nullptr);
    close_overlay(over, snip);
    return TRUE;
}

static gboolean on_key([[maybe_unused]] GtkWidget *widget, GdkEventKey *event, gpointer data) {
    overlay_panel *panel = data;
    if (event->keyval == GDK_KEY_Escape) {
        close_overlay(panel->owner, nullptr);
        return TRUE;
    }
    return FALSE;
}

static void set_cursor(GtkWidget *widget, gpointer data) {
    overlay_panel *panel = data;
    GdkWindow *window = gtk_widget_get_window(widget);
    const char *name = panel->owner->mode == XD_SNIP_WINDOW ? "default" : "crosshair";
    GdkCursor *cursor = gdk_cursor_new_from_name(gtk_widget_get_display(widget), name);
    gdk_window_set_cursor(window, cursor);
    if (cursor != nullptr)
        g_object_unref(cursor);
}

static void add_panel(overlay *over, GdkMonitor *monitor) {
    overlay_panel *panel = g_new0(overlay_panel, 1);
    panel->owner = over;
    gdk_monitor_get_geometry(monitor, &panel->geometry);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    panel->window = window;
    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_namespace(GTK_WINDOW(window), "xd-snip");
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_monitor(GTK_WINDOW(window), monitor);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_widget_set_app_paintable(window, TRUE);
    gtk_widget_add_events(window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                      GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(window, "draw", G_CALLBACK(draw_panel), panel);
    g_signal_connect(window, "button-press-event", G_CALLBACK(on_press), panel);
    g_signal_connect(window, "motion-notify-event", G_CALLBACK(on_motion), panel);
    g_signal_connect(window, "button-release-event", G_CALLBACK(on_release), panel);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key), panel);
    g_signal_connect(window, "realize", G_CALLBACK(set_cursor), panel);
    g_ptr_array_add(over->panels, panel);
    gtk_widget_show(window);
}

void snip_overlay_start(snip_app *app, xd_snip_screen *screen, xd_snip_mode mode) {
    if (mode == XD_SNIP_FULL_SCREEN) {
        int width = cairo_image_surface_get_width(screen->image);
        int height = cairo_image_surface_get_height(screen->image);
        xd_snip_box all = {screen->origin_x, screen->origin_y, width / screen->scale,
                           height / screen->scale};
        cairo_surface_t *snip = xd_snip_screen_cut(screen, all, nullptr, 0, nullptr);
        double scale = screen->scale;
        xd_snip_screen_free(screen);
        snip_app_snip_taken(app, snip, scale);
        return;
    }
    overlay *over = g_new0(overlay, 1);
    over->app = app;
    over->screen = screen;
    over->mode = mode;
    over->panels = g_ptr_array_new_with_free_func(g_free);
    over->path = g_array_new(FALSE, FALSE, sizeof(xd_snip_point));
    GdkDisplay *display = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(display); i++)
        add_panel(over, gdk_display_get_monitor(display, i));
}
