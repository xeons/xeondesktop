/* SPDX-License-Identifier: MIT */
#include <libudev.h>
#include <string.h>

#include "inputdevs.h"

enum { COL_NAME, COL_TYPE, COL_MAKER, COL_LOCATION, COL_COUNT };

typedef struct devices_page {
    GtkWidget *maker, *location;
} devices_page;

static bool prop_set(struct udev_device *d, const char *key) {
    return g_strcmp0(udev_device_get_property_value(d, key), "1") == 0;
}

static const char *pointer_type(struct udev_device *d) {
    if (prop_set(d, "ID_INPUT_TOUCHPAD"))
        return "Touchpad";
    if (prop_set(d, "ID_INPUT_POINTINGSTICK"))
        return "Pointing stick";
    if (prop_set(d, "ID_INPUT_MOUSE"))
        return "Mouse";
    return nullptr;
}

static char *maker(struct udev_device *d) {
    static const char *const keys[] = {"ID_VENDOR_FROM_DATABASE", "ID_VENDOR_ENC", "ID_VENDOR"};
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        const char *v = udev_device_get_property_value(d, keys[i]);
        if (v != nullptr && v[0] != '\0') {
            /* ID_VENDOR_ENC escapes spaces as \x20. */
            char *s = g_strcompress(v);
            g_strdelimit(s, "_", ' ');
            return s;
        }
    }
    return g_strdup("(Standard devices)");
}

static char *location(struct udev_device *d) {
    const char *bus = udev_device_get_property_value(d, "ID_BUS");
    const char *path = udev_device_get_property_value(d, "ID_PATH");
    const char *devpath = udev_device_get_devpath(d);
    if (g_strcmp0(bus, "usb") == 0)
        return g_strdup("on USB");
    if (g_strcmp0(bus, "bluetooth") == 0)
        return g_strdup("Bluetooth");
    if (g_strcmp0(bus, "i2c") == 0)
        return g_strdup("on I2C");
    if (g_strcmp0(bus, "pci") == 0)
        return g_strdup("on PCI bus");
    if (path != nullptr && strstr(path, "i8042") != nullptr)
        return g_strdup("PS/2 port");
    if (devpath != nullptr && g_str_has_prefix(devpath, "/devices/virtual/"))
        return g_strdup("Virtual device");
    return g_strdup(bus != nullptr ? bus : "Unknown");
}

static void fill(GtkListStore *store, xd_input_kind kind) {
    struct udev *udev = udev_new();
    if (udev == nullptr)
        return;
    struct udev_enumerate *e = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(e, "input");
    udev_enumerate_add_match_sysname(e, "input*"); /* the devices, not their nodes */
    udev_enumerate_scan_devices(e);
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
        struct udev_device *d =
            udev_device_new_from_syspath(udev, udev_list_entry_get_name(entry));
        if (d == nullptr)
            continue;
        const char *type = kind == XD_INPUT_POINTERS       ? pointer_type(d)
                           : prop_set(d, "ID_INPUT_KEYBOARD") ? "Keyboard"
                                                              : nullptr;
        const char *name = udev_device_get_sysattr_value(d, "name");
        if (type != nullptr && name != nullptr) {
            char *m = maker(d), *loc = location(d);
            gtk_list_store_insert_with_values(store, nullptr, -1, COL_NAME, name, COL_TYPE, type,
                                              COL_MAKER, m, COL_LOCATION, loc, -1);
            g_free(m);
            g_free(loc);
        }
        udev_device_unref(d);
    }
    udev_enumerate_unref(e);
    udev_unref(udev);
}

static void on_select(GtkTreeSelection *sel, gpointer data) {
    devices_page *p = data;
    GtkTreeModel *model;
    GtkTreeIter it;
    char *m = nullptr, *loc = nullptr;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, COL_MAKER, &m, COL_LOCATION, &loc, -1);
    gtk_label_set_text(GTK_LABEL(p->maker), m != nullptr ? m : "");
    gtk_label_set_text(GTK_LABEL(p->location), loc != nullptr ? loc : "");
    g_free(m);
    g_free(loc);
}

static GtkWidget *value_label(GtkWidget *grid, int row, const char *title) {
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    GtkWidget *v = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), t, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    return v;
}

GtkWidget *xd_input_devices_page(xd_input_kind kind) {
    devices_page *p = g_new0(devices_page, 1);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    g_object_set_data_full(G_OBJECT(page), "xd-devices", p, g_free);

    GtkWidget *title = gtk_label_new_with_mnemonic("_Devices:");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);

    GtkListStore *store = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING);
    fill(store, kind);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_label_set_mnemonic_widget(GTK_LABEL(title), view);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes("Name", r, "text", COL_NAME,
                                                                    nullptr);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
                                gtk_tree_view_column_new_with_attributes(
                                    "Type", gtk_cell_renderer_text_new(), "text", COL_TYPE,
                                    nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 120);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *frame = gtk_frame_new("Device Properties");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    p->maker = value_label(grid, 0, "Manufacturer:");
    p->location = value_label(grid, 1, "Location:");
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 0);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_select), p);
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first))
        gtk_tree_selection_select_iter(sel, &first);
    return page;
}
