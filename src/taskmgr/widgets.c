/* SPDX-License-Identifier: MIT */
#include "taskmgr.h"

GtkWidget *xd_tm_scrolled(GtkWidget *child) {
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

GtkWidget *xd_tm_button_row(GtkWidget **buttons, int n) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (int i = n - 1; i >= 0; i--)
        gtk_box_pack_end(GTK_BOX(row), buttons[i], FALSE, FALSE, 0);
    for (int i = 0; i < n; i++)
        gtk_widget_set_size_request(buttons[i], 84, -1);
    return row;
}

void xd_tm_draw_well(GtkWidget *w, cairo_t *cr, double *x, double *y, double *wd, double *ht) {
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA shadow = {0.5, 0.5, 0.5, 1}, light = {1, 1, 1, 1};
    gtk_style_context_lookup_color(sc, "borders", &shadow);
    cairo_set_line_width(cr, 1);
    gdk_cairo_set_source_rgba(cr, &shadow);
    cairo_move_to(cr, 0.5, height - 1);
    cairo_line_to(cr, 0.5, 0.5);
    cairo_line_to(cr, width - 1, 0.5);
    cairo_stroke(cr);
    gdk_cairo_set_source_rgba(cr, &light);
    cairo_move_to(cr, width - 0.5, 0);
    cairo_line_to(cr, width - 0.5, height - 0.5);
    cairo_line_to(cr, 0, height - 0.5);
    cairo_stroke(cr);
    *x = 1;
    *y = 1;
    *wd = width - 2;
    *ht = height - 2;
}

GtkWidget *xd_tm_group(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_widget_set_margin_start(child, 8);
    gtk_widget_set_margin_end(child, 8);
    gtk_widget_set_margin_top(child, 4);
    gtk_widget_set_margin_bottom(child, 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}
