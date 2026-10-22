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

GtkWidget *xd_tm_button_row(GtkWidget **buttons, int count) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (int i = count - 1; i >= 0; i--)
        gtk_box_pack_end(GTK_BOX(row), buttons[i], FALSE, FALSE, 0);
    for (int i = 0; i < count; i++)
        gtk_widget_set_size_request(buttons[i], 84, -1);
    return row;
}

void xd_tm_draw_well(GtkWidget *widget, cairo_t *cairo, double *inner_x, double *inner_y,
                     double *wd, double *ht) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    GtkStyleContext *sc = gtk_widget_get_style_context(widget);
    GdkRGBA shadow = {0.5, 0.5, 0.5, 1}, light = {1, 1, 1, 1};
    gtk_style_context_lookup_color(sc, "borders", &shadow);
    cairo_set_line_width(cairo, 1);
    gdk_cairo_set_source_rgba(cairo, &shadow);
    cairo_move_to(cairo, 0.5, height - 1);
    cairo_line_to(cairo, 0.5, 0.5);
    cairo_line_to(cairo, width - 1, 0.5);
    cairo_stroke(cairo);
    gdk_cairo_set_source_rgba(cairo, &light);
    cairo_move_to(cairo, width - 0.5, 0);
    cairo_line_to(cairo, width - 0.5, height - 0.5);
    cairo_line_to(cairo, 0, height - 0.5);
    cairo_stroke(cairo);
    *inner_x = 1;
    *inner_y = 1;
    *wd = width - 2;
    *ht = height - 2;
}

GtkWidget *xd_tm_group(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    gtk_widget_set_margin_start(child, 8);
    gtk_widget_set_margin_end(child, 8);
    gtk_widget_set_margin_top(child, 4);
    gtk_widget_set_margin_bottom(child, 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}
