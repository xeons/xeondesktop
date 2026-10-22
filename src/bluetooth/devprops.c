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

static const char *yes_no(bool value) { return value ? "Yes" : "No"; }

static void show_services(devprops *properties) {
    gtk_list_store_clear(properties->services);
    GVariant *uuids = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(properties->dev), "UUIDs");
    if (uuids != nullptr && g_variant_is_of_type(uuids, G_VARIANT_TYPE_STRING_ARRAY)) {
        GVariantIter it;
        const char *uuid;
        g_variant_iter_init(&it, uuids);
        while (g_variant_iter_next(&it, "&s", &uuid)) {
            const char *name = xd_bt_uuid_name(uuid);
            gtk_list_store_insert_with_values(properties->services, nullptr, -1, SVC_NAME,
                                              name != nullptr ? name : "Unknown service", SVC_UUID,
                                              uuid, -1);
        }
    }
    if (uuids != nullptr)
        g_variant_unref(uuids);
}

static void show_state(devprops *properties) {
    xd_bt_type bt_type = bt_device_type(properties->dev);
    gtk_image_set_from_icon_name(GTK_IMAGE(properties->icon), bt_type.icon, GTK_ICON_SIZE_DIALOG);
    gtk_label_set_text(GTK_LABEL(properties->type), bt_type.name);
    char *address = bt_prop_s(properties->dev, "Address");
    gtk_label_set_text(GTK_LABEL(properties->address), address != nullptr ? address : "");
    g_free(address);
    gtk_label_set_text(GTK_LABEL(properties->paired), yes_no(bt_prop_b(properties->dev, "Paired")));
    bool on = bt_prop_b(properties->dev, "Connected");
    gtk_label_set_text(GTK_LABEL(properties->connected), yes_no(on));
    gtk_button_set_label(GTK_BUTTON(properties->connect), on ? "_Disconnect" : "_Connect");
    if (!gtk_widget_get_sensitive(properties->apply)) {
        properties->updating = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->trusted),
                                     bt_prop_b(properties->dev, "Trusted"));
        properties->updating = false;
    }
}

static bool has_changed(GVariant *changed, const char *const *invalidated, const char *name) {
    GVariant *variant = g_variant_lookup_value(changed, name, nullptr);
    if (variant != nullptr) {
        g_variant_unref(variant);
        return true;
    }
    return invalidated != nullptr && g_strv_contains(invalidated, name);
}

/* A connected device reports its signal strength often: the services
 * list is redone only when they change, and Connect waits (greyed out)
 * for the connection's state. */
static void on_dev_changed([[maybe_unused]] GDBusProxy *proxy, GVariant *variant,
                           const char *const *inv, gpointer data) {
    devprops *properties = data;
    show_state(properties);
    if (has_changed(variant, inv, "Connected"))
        gtk_widget_set_sensitive(properties->connect, TRUE);
    if (has_changed(variant, inv, "UUIDs"))
        show_services(properties);
}

/* The device was removed (or BlueZ went away): nothing left to show. */
static void on_removed([[maybe_unused]] GDBusObjectManager *dbus_object_manager,
                       GDBusObject *object, gpointer data) {
    devprops *properties = data;
    if (strcmp(g_dbus_object_get_object_path(object), properties->path) == 0)
        gtk_widget_destroy(properties->window);
}

static void changed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    devprops *properties = data;
    if (!properties->updating)
        gtk_widget_set_sensitive(properties->apply, TRUE);
}

static void connect_done(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    GtkWidget **window = data;
    if (variant != nullptr) {
        g_variant_unref(variant);
    } else {
        if (*window != nullptr) {
            devprops *properties = g_object_get_data(G_OBJECT(*window), "devprops");
            gtk_widget_set_sensitive(properties->connect, TRUE); /* to try again */
            bt_error(GTK_WINDOW(*window), "The device could not be connected or disconnected.",
                     error);
        }
        g_error_free(error);
    }
    if (*window != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(*window), (gpointer *)window);
    g_free(window);
}

static void on_connect([[maybe_unused]] GtkButton *button, gpointer data) {
    devprops *properties = data;
    bool on = bt_prop_b(properties->dev, "Connected");
    gtk_widget_set_sensitive(properties->connect, FALSE); /* until the state changes */
    GtkWidget **window = g_new(GtkWidget *, 1);
    *window = properties->window;
    g_object_add_weak_pointer(G_OBJECT(properties->window), (gpointer *)window);
    g_dbus_connection_call(properties->app->bus, BLUEZ, properties->path, IFACE_DEVICE,
                           on ? "Disconnect" : "Connect", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE,
                           60000, nullptr, connect_done, window);
}

static void apply(devprops *properties) {
    GtkWindow *parent = GTK_WINDOW(properties->window);
    char *alias = bt_prop_s(properties->dev, "Alias");
    const char *name = gtk_entry_get_text(GTK_ENTRY(properties->alias));
    /* An empty name goes back to the device's own. */
    if (g_strcmp0(alias, name) != 0)
        bt_set_prop(properties->app, parent, properties->path, IFACE_DEVICE, "Alias",
                    g_variant_new_string(name));
    g_free(alias);
    bool trusted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->trusted));
    if (trusted != bt_prop_b(properties->dev, "Trusted"))
        bt_set_prop(properties->app, parent, properties->path, IFACE_DEVICE, "Trusted",
                    g_variant_new_boolean(trusted));
    gtk_widget_set_sensitive(properties->apply, FALSE);
}

static void on_response(GtkDialog *dlg, int response, gpointer data) {
    devprops *properties = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply))
        apply(properties);
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    devprops *properties = data;
    g_signal_handlers_disconnect_by_data(properties->dev, properties);
    if (properties->app->objects != nullptr)
        g_signal_handlers_disconnect_by_data(properties->app->objects, properties);
    g_object_unref(properties->dev);
    g_free(properties->path);
    g_free(properties);
}

static GtkWidget *value_label(GtkWidget *grid, int row, const char *title) {
    GtkWidget *title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    GtkWidget *value = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(value), 0);
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(grid), title_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
    return value;
}

static GtkWidget *general_page(devprops *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    properties->icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(properties->icon), 32);
    gtk_box_pack_start(GTK_BOX(top), properties->icon, FALSE, FALSE, 0);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Name:");
    properties->alias = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(properties->alias), 248);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->alias);
    char *alias = bt_prop_s(properties->dev, "Alias");
    gtk_entry_set_text(GTK_ENTRY(properties->alias), alias != nullptr ? alias : "");
    g_free(alias);
    gtk_box_pack_start(GTK_BOX(top), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), properties->alias, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    properties->type = value_label(grid, 0, "Device type:");
    properties->address = value_label(grid, 1, "Unique address:");
    properties->paired = value_label(grid, 2, "Paired:");
    properties->connected = value_label(grid, 3, "Connected:");
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    properties->trusted =
        gtk_check_button_new_with_mnemonic("_Trust this device (it can connect without asking)");
    gtk_box_pack_start(GTK_BOX(page), properties->trusted, FALSE, FALSE, 0);
    properties->connect = gtk_button_new_with_mnemonic("_Connect");
    gtk_widget_set_halign(properties->connect, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), properties->connect, FALSE, FALSE, 0);
    g_signal_connect(properties->alias, "changed", G_CALLBACK(changed), properties);
    g_signal_connect(properties->trusted, "toggled", G_CALLBACK(changed), properties);
    g_signal_connect(properties->connect, "clicked", G_CALLBACK(on_connect), properties);
    return page;
}

static GtkWidget *services_page(devprops *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *intro = gtk_label_new("This Bluetooth device offers the following services:");
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_box_pack_start(GTK_BOX(page), intro, FALSE, FALSE, 0);
    properties->services = gtk_list_store_new(SVC_COUNT, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(properties->services));
    g_object_unref(properties->services);
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
        "Service", gtk_cell_renderer_text_new(), "text", SVC_NAME, nullptr);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
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

void bt_device_properties(bt_app *state, const char *path) {
    GDBusInterface *dev = bt_interface(state, path, IFACE_DEVICE);
    if (dev == nullptr)
        return;
    devprops *properties = g_new0(devprops, 1);
    properties->app = state;
    properties->path = g_strdup(path);
    properties->dev = dev;
    char *name = bt_device_name(dev);
    char *title = g_strdup_printf("%s Properties", name);
    properties->window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties->window), title);
    g_free(title);
    g_free(name);
    g_object_set_data(G_OBJECT(properties->window), "devprops", properties);
    gtk_window_set_icon_name(GTK_WINDOW(properties->window), "bluetooth");
    gtk_window_set_default_size(GTK_WINDOW(properties->window), 400, 380);
    if (state->window != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(properties->window), GTK_WINDOW(state->window));
    gtk_dialog_add_button(GTK_DIALOG(properties->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties->window), "Cancel", GTK_RESPONSE_CANCEL);
    properties->apply =
        gtk_dialog_add_button(GTK_DIALOG(properties->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(properties->apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(properties->window), GTK_RESPONSE_OK);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(properties),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), services_page(properties),
                             gtk_label_new_with_mnemonic("_Services"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties->window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties->window, GTK_NOTEBOOK(tabs));
    show_state(properties);
    show_services(properties);
    g_signal_connect(dev, "g-properties-changed", G_CALLBACK(on_dev_changed), properties);
    g_signal_connect(state->objects, "object-removed", G_CALLBACK(on_removed), properties);
    g_signal_connect(properties->window, "response", G_CALLBACK(on_response), properties);
    g_signal_connect(properties->window, "destroy", G_CALLBACK(on_destroy), properties);
    gtk_widget_show_all(properties->window);
}
