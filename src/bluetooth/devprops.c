/* SPDX-License-Identifier: MIT
 * A paired device's Properties: General (its name, kind, address and
 * state, whether it's trusted; Connect/Disconnect) and Services (what its
 * UUIDs stand for).
 */
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "bt.h"

enum { SVC_NAME, SVC_UUID, SVC_COUNT };

typedef struct devprops {
    bt_app *app;
    char *path;
    GDBusInterface *dev;
    GtkWidget *window, *apply, *icon, *alias, *type, *address, *paired, *connected, *trusted,
        *connect;
    GtkListStore *services;
    bool updating;
} devprops;

static const char *yes_no(bool b) { return b ? "Yes" : "No"; }

static void show_state(devprops *d) {
    xd_bt_type t = bt_device_type(d->dev);
    gtk_image_set_from_icon_name(GTK_IMAGE(d->icon), t.icon, GTK_ICON_SIZE_DIALOG);
    gtk_label_set_text(GTK_LABEL(d->type), t.name);
    char *address = bt_prop_s(d->dev, "Address");
    gtk_label_set_text(GTK_LABEL(d->address), address != nullptr ? address : "");
    g_free(address);
    gtk_label_set_text(GTK_LABEL(d->paired), yes_no(bt_prop_b(d->dev, "Paired")));
    bool on = bt_prop_b(d->dev, "Connected");
    gtk_label_set_text(GTK_LABEL(d->connected), yes_no(on));
    gtk_button_set_label(GTK_BUTTON(d->connect), on ? "_Disconnect" : "_Connect");
    gtk_widget_set_sensitive(d->connect, TRUE);
    if (!gtk_widget_get_sensitive(d->apply)) {
        d->updating = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->trusted), bt_prop_b(d->dev, "Trusted"));
        d->updating = false;
    }

    gtk_list_store_clear(d->services);
    GVariant *uuids = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(d->dev), "UUIDs");
    if (uuids != nullptr && g_variant_is_of_type(uuids, G_VARIANT_TYPE_STRING_ARRAY)) {
        GVariantIter it;
        const char *uuid;
        g_variant_iter_init(&it, uuids);
        while (g_variant_iter_next(&it, "&s", &uuid)) {
            const char *name = xd_bt_uuid_name(uuid);
            gtk_list_store_insert_with_values(d->services, nullptr, -1, SVC_NAME,
                                              name != nullptr ? name : "Unknown service", SVC_UUID,
                                              uuid, -1);
        }
    }
    if (uuids != nullptr)
        g_variant_unref(uuids);
}

static void on_dev_changed([[maybe_unused]] GDBusProxy *p, [[maybe_unused]] GVariant *c,
                           [[maybe_unused]] const char *const *inv, gpointer data) {
    show_state(data);
}

/* The device was removed (or BlueZ went away): nothing left to show. */
static void on_removed([[maybe_unused]] GDBusObjectManager *m, GDBusObject *o, gpointer data) {
    devprops *d = data;
    if (strcmp(g_dbus_object_get_object_path(o), d->path) == 0)
        gtk_widget_destroy(d->window);
}

static void changed([[maybe_unused]] GtkWidget *w, gpointer data) {
    devprops *d = data;
    if (!d->updating)
        gtk_widget_set_sensitive(d->apply, TRUE);
}

static void connect_done(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    GtkWidget **window = data;
    if (r != nullptr) {
        g_variant_unref(r);
    } else {
        if (*window != nullptr) {
            devprops *d = g_object_get_data(G_OBJECT(*window), "devprops");
            gtk_widget_set_sensitive(d->connect, TRUE); /* to try again */
            bt_error(GTK_WINDOW(*window), "The device could not be connected or disconnected.",
                     error);
        }
        g_error_free(error);
    }
    if (*window != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(*window), (gpointer *)window);
    g_free(window);
}

static void on_connect([[maybe_unused]] GtkButton *b, gpointer data) {
    devprops *d = data;
    bool on = bt_prop_b(d->dev, "Connected");
    gtk_widget_set_sensitive(d->connect, FALSE); /* until the state changes */
    GtkWidget **window = g_new(GtkWidget *, 1);
    *window = d->window;
    g_object_add_weak_pointer(G_OBJECT(d->window), (gpointer *)window);
    g_dbus_connection_call(d->app->bus, BLUEZ, d->path, IFACE_DEVICE, on ? "Disconnect" : "Connect",
                           nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 60000, nullptr, connect_done,
                           window);
}

static void apply(devprops *d) {
    GtkWindow *w = GTK_WINDOW(d->window);
    char *alias = bt_prop_s(d->dev, "Alias");
    const char *name = gtk_entry_get_text(GTK_ENTRY(d->alias));
    /* An empty name goes back to the device's own. */
    if (g_strcmp0(alias, name) != 0)
        bt_set_prop(d->app, w, d->path, IFACE_DEVICE, "Alias", g_variant_new_string(name));
    g_free(alias);
    bool trusted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->trusted));
    if (trusted != bt_prop_b(d->dev, "Trusted"))
        bt_set_prop(d->app, w, d->path, IFACE_DEVICE, "Trusted", g_variant_new_boolean(trusted));
    gtk_widget_set_sensitive(d->apply, FALSE);
}

static void on_response(GtkDialog *dlg, int response, gpointer data) {
    devprops *d = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(d);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(d->apply))
        apply(d);
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    devprops *d = data;
    g_signal_handlers_disconnect_by_data(d->dev, d);
    if (d->app->objects != nullptr)
        g_signal_handlers_disconnect_by_data(d->app->objects, d);
    g_object_unref(d->dev);
    g_free(d->path);
    g_free(d);
}

static GtkWidget *value_label(GtkWidget *grid, int row, const char *title) {
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    GtkWidget *v = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), t, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    return v;
}

static GtkWidget *general_page(devprops *d) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    d->icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(d->icon), 32);
    gtk_box_pack_start(GTK_BOX(top), d->icon, FALSE, FALSE, 0);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Name:");
    d->alias = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(d->alias), 248);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), d->alias);
    char *alias = bt_prop_s(d->dev, "Alias");
    gtk_entry_set_text(GTK_ENTRY(d->alias), alias != nullptr ? alias : "");
    g_free(alias);
    gtk_box_pack_start(GTK_BOX(top), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), d->alias, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    d->type = value_label(grid, 0, "Device type:");
    d->address = value_label(grid, 1, "Unique address:");
    d->paired = value_label(grid, 2, "Paired:");
    d->connected = value_label(grid, 3, "Connected:");
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    d->trusted =
        gtk_check_button_new_with_mnemonic("_Trust this device (it can connect without asking)");
    gtk_box_pack_start(GTK_BOX(page), d->trusted, FALSE, FALSE, 0);
    d->connect = gtk_button_new_with_mnemonic("_Connect");
    gtk_widget_set_halign(d->connect, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), d->connect, FALSE, FALSE, 0);
    g_signal_connect(d->alias, "changed", G_CALLBACK(changed), d);
    g_signal_connect(d->trusted, "toggled", G_CALLBACK(changed), d);
    g_signal_connect(d->connect, "clicked", G_CALLBACK(on_connect), d);
    return page;
}

static GtkWidget *services_page(devprops *d) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *intro = gtk_label_new("This Bluetooth device offers the following services:");
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_box_pack_start(GTK_BOX(page), intro, FALSE, FALSE, 0);
    d->services = gtk_list_store_new(SVC_COUNT, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->services));
    g_object_unref(d->services);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
        "Service", gtk_cell_renderer_text_new(), "text", SVC_NAME, nullptr);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes(
                                 "UUID", gtk_cell_renderer_text_new(), "text", SVC_UUID, nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    return page;
}

void bt_device_properties(bt_app *a, const char *path) {
    GDBusInterface *dev = bt_interface(a, path, IFACE_DEVICE);
    if (dev == nullptr)
        return;
    devprops *d = g_new0(devprops, 1);
    d->app = a;
    d->path = g_strdup(path);
    d->dev = dev;
    char *name = bt_device_name(dev);
    char *title = g_strdup_printf("%s Properties", name);
    d->window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d->window), title);
    g_free(title);
    g_free(name);
    g_object_set_data(G_OBJECT(d->window), "devprops", d);
    gtk_window_set_icon_name(GTK_WINDOW(d->window), "bluetooth");
    gtk_window_set_default_size(GTK_WINDOW(d->window), 400, 380);
    if (a->window != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(d->window), GTK_WINDOW(a->window));
    gtk_dialog_add_button(GTK_DIALOG(d->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d->window), "Cancel", GTK_RESPONSE_CANCEL);
    d->apply = gtk_dialog_add_button(GTK_DIALOG(d->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(d->apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(d->window), GTK_RESPONSE_OK);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(d),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), services_page(d),
                             gtk_label_new_with_mnemonic("_Services"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d->window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(d->window, GTK_NOTEBOOK(tabs));
    show_state(d);
    g_signal_connect(dev, "g-properties-changed", G_CALLBACK(on_dev_changed), d);
    g_signal_connect(a->objects, "object-removed", G_CALLBACK(on_removed), d);
    g_signal_connect(d->window, "response", G_CALLBACK(on_response), d);
    g_signal_connect(d->window, "destroy", G_CALLBACK(on_destroy), d);
    gtk_widget_show_all(d->window);
}
