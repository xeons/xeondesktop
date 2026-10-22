/* SPDX-License-Identifier: MIT
 * xd-bluetooth: Windows XP SP2's Bluetooth Devices, over BlueZ's D-Bus
 * API (org.bluez; its ObjectManager gives adapters and devices with live
 * properties).
 *
 *   Devices   paired devices; Add (the wizard), Remove, Properties
 *   Options   discovery, new pairings, the notification area icon, the
 *             computer's Bluetooth name
 *   Hardware  the Bluetooth radios
 *
 * While it runs it's BlueZ's pairing agent, asking for passkeys and
 * confirmations (agent.c). `--add` opens just the wizard (the tray's Add a
 * Bluetooth Device), `--options` the Options tab.
 */
#include <libudev.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "bt.h"

/* ---- BlueZ helpers ----------------------------------------------------------- */

static GVariant *prop(GDBusInterface *dbus_interface, const char *name) {
    return dbus_interface != nullptr
               ? g_dbus_proxy_get_cached_property(G_DBUS_PROXY(dbus_interface), name)
               : nullptr;
}

bool bt_prop_b(GDBusInterface *dbus_interface, const char *name) {
    GVariant *variant = prop(dbus_interface, name);
    bool value = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BOOLEAN) &&
                 g_variant_get_boolean(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

char *bt_prop_s(GDBusInterface *dbus_interface, const char *name) {
    GVariant *variant = prop(dbus_interface, name);
    char *text = variant != nullptr && (g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING) ||
                                        g_variant_is_of_type(variant, G_VARIANT_TYPE_OBJECT_PATH))
                     ? g_variant_dup_string(variant, nullptr)
                     : nullptr;
    if (variant != nullptr)
        g_variant_unref(variant);
    return text;
}

/* Unsigned integers of any width (Class is u, Appearance and
 * Manufacturer q, Version y). */
uint32_t bt_prop_u(GDBusInterface *dbus_interface, const char *name) {
    GVariant *variant = prop(dbus_interface, name);
    uint32_t number = 0;
    if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT32))
        number = g_variant_get_uint32(variant);
    else if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT16))
        number = g_variant_get_uint16(variant);
    else if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BYTE))
        number = g_variant_get_byte(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return number;
}

GDBusInterface *bt_adapter(bt_app *state) {
    if (state->objects == nullptr)
        return nullptr;
    GList *objs = g_dbus_object_manager_get_objects(state->objects);
    GDBusInterface *found = nullptr;
    for (GList *list = objs; list != nullptr && found == nullptr; list = list->next)
        found = g_dbus_object_get_interface(list->data, IFACE_ADAPTER);
    g_list_free_full(objs, g_object_unref);
    return found;
}

GDBusInterface *bt_interface(bt_app *state, const char *path, const char *iface) {
    return state->objects != nullptr && path != nullptr
               ? g_dbus_object_manager_get_interface(state->objects, path, iface)
               : nullptr;
}

char *bt_device_name(GDBusInterface *dev) {
    char *name = bt_prop_s(dev, "Alias");
    if (name == nullptr || name[0] == '\0') {
        g_free(name);
        name = bt_prop_s(dev, "Address");
    }
    return name != nullptr ? name : g_strdup("Bluetooth device");
}

xd_bt_type bt_device_type(GDBusInterface *dev) {
    xd_bt_type bt_type = xd_bt_class_type(bt_prop_u(dev, "Class"));
    if (bt_type.name == nullptr)
        bt_type = xd_bt_appearance_type((uint16_t)bt_prop_u(dev, "Appearance"));
    if (bt_type.name == nullptr) {
        char *icon = bt_prop_s(dev, "Icon");
        bt_type = xd_bt_icon_type(icon);
        g_free(icon);
    }
    return bt_type;
}

void bt_error(GtkWindow *parent, const char *what, const GError *error) {
    /* BlueZ's own message, without GDBus' "GDBus.Error:org.bluez..." */
    GError *stripped = g_error_copy(error);
    g_dbus_error_strip_remote_error(stripped);
    char *text = g_strdup_printf("%s\n\n%s", what, stripped->message);
    xd_message_box(parent, BT_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
    g_error_free(stripped);
}

typedef struct set_call {
    GtkWindow *parent;
    char *name;
} set_call;

static void set_done(GObject *src, GAsyncResult *res, gpointer data) {
    set_call *call = data;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant != nullptr) {
        g_variant_unref(variant);
    } else {
        char *what = g_strdup_printf("The setting \"%s\" could not be changed.", call->name);
        bt_error(call->parent, what, error);
        g_free(what);
        g_error_free(error);
    }
    if (call->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(call->parent), (gpointer *)&call->parent);
    g_free(call->name);
    g_free(call);
}

void bt_set_prop(bt_app *state, GtkWindow *parent, const char *path, const char *iface,
                 const char *name, GVariant *value) {
    set_call *call = g_new0(set_call, 1);
    call->parent = parent;
    call->name = g_strdup(name);
    if (parent != nullptr)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&call->parent);
    g_dbus_connection_call(state->bus, BLUEZ, path, "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)", iface, name, value), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, set_done, call);
}

/* ---- Devices --------------------------------------------------------------------- */

enum { DEV_ICON, DEV_TEXT, DEV_PATH, DEV_SORT, DEV_COUNT };

static char *selected_path(GtkWidget *view) {
    GtkTreeModel *model;
    GtkTreeIter it;
    char *path = nullptr;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model,
                                        &it))
        gtk_tree_model_get(model, &it, DEV_PATH, &path, -1);
    return path;
}

static void select_path(GtkWidget *view, GtkListStore *store, const char *path) {
    GtkTreeIter it;
    GtkTreeModel *model = GTK_TREE_MODEL(store);
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok && path != nullptr;
         ok = gtk_tree_model_iter_next(model, &it)) {
        char *row_path = nullptr;
        gtk_tree_model_get(model, &it, DEV_PATH, &row_path, -1);
        bool match = g_strcmp0(row_path, path) == 0;
        g_free(row_path);
        if (match) {
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &it);
            return;
        }
    }
}

/* The second line under a device's name, as XP's list had it. */
static const char *device_state(GDBusInterface *dev) {
    if (bt_prop_b(dev, "Connected"))
        return "Connected";
    if (bt_prop_b(dev, "Blocked"))
        return "Blocked";
    return bt_prop_b(dev, "LegacyPairing") ? "Passkey enabled" : "Paired";
}

static void fill_devices(bt_app *state) {
    char *keep = selected_path(state->device_view);
    gtk_list_store_clear(state->devices);
    GList *objs =
        state->objects != nullptr ? g_dbus_object_manager_get_objects(state->objects) : nullptr;
    for (GList *list = objs; list != nullptr; list = list->next) {
        GDBusInterface *dev = g_dbus_object_get_interface(list->data, IFACE_DEVICE);
        if (dev == nullptr)
            continue;
        if (bt_prop_b(dev, "Paired")) {
            char *name = bt_device_name(dev);
            xd_bt_type bt_type = bt_device_type(dev);
            char *text = g_markup_printf_escaped("%s\n<small>%s, %s</small>", name, bt_type.name,
                                                 device_state(dev));
            char *sort = g_utf8_collate_key(name, -1);
            gtk_list_store_insert_with_values(
                state->devices, nullptr, -1, DEV_ICON, bt_type.icon, DEV_TEXT, text, DEV_PATH,
                g_dbus_object_get_object_path(list->data), DEV_SORT, sort, -1);
            g_free(sort);
            g_free(text);
            g_free(name);
        }
        g_object_unref(dev);
    }
    g_list_free_full(objs, g_object_unref);
    select_path(state->device_view, state->devices, keep);
    g_free(keep);
}

static void device_buttons(bt_app *state) {
    char *path = selected_path(state->device_view);
    gtk_widget_set_sensitive(state->remove, path != nullptr);
    gtk_widget_set_sensitive(state->properties, path != nullptr);
    g_free(path);
    GDBusInterface *ad = bt_adapter(state);
    gtk_widget_set_sensitive(state->add, ad != nullptr && bt_prop_b(ad, "Powered"));
    if (ad != nullptr)
        g_object_unref(ad);
}

static void on_device_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    device_buttons(data);
}

static void on_add([[maybe_unused]] GtkButton *button, gpointer data) { bt_wizard_open(data); }

static void on_properties([[maybe_unused]] GtkButton *button, gpointer data) {
    bt_app *state = data;
    char *path = selected_path(state->device_view);
    if (path != nullptr)
        bt_device_properties(state, path);
    g_free(path);
}

static void on_device_activated([[maybe_unused]] GtkTreeView *tree_view,
                                [[maybe_unused]] GtkTreePath *path,
                                [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    on_properties(nullptr, data);
}

static void remove_done(GObject *src, GAsyncResult *res, gpointer data) {
    bt_app *state = data;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant != nullptr) {
        g_variant_unref(variant);
    } else {
        bt_error(GTK_WINDOW(state->window), "The device could not be removed.", error);
        g_error_free(error);
    }
}

static void on_remove([[maybe_unused]] GtkButton *button, gpointer data) {
    bt_app *state = data;
    char *path = selected_path(state->device_view);
    GDBusInterface *dev = bt_interface(state, path, IFACE_DEVICE);
    char *adapter = bt_prop_s(dev, "Adapter");
    if (dev == nullptr || adapter == nullptr) {
        g_free(path);
        g_free(adapter);
        if (dev != nullptr)
            g_object_unref(dev);
        return;
    }
    char *name = bt_device_name(dev);
    char *text = g_strdup_printf(
        "Are you sure you want to remove %s?\n\nAfter you remove it, the device can no longer "
        "connect to this computer until you add it again.",
        name);
    if (xd_message_box(GTK_WINDOW(state->window), "Remove Bluetooth Device", XD_MB_QUESTION,
                       XD_MB_YES_NO, TRUE, text) == GTK_RESPONSE_YES)
        g_dbus_connection_call(state->bus, BLUEZ, adapter, IFACE_ADAPTER, "RemoveDevice",
                               g_variant_new("(o)", path), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                               nullptr, remove_done, state);
    g_free(text);
    g_free(name);
    g_free(adapter);
    g_free(path);
    g_object_unref(dev);
}

static GtkWidget *devices_page(bt_app *state) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    state->devices =
        gtk_list_store_new(DEV_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(state->devices), DEV_SORT,
                                         GTK_SORT_ASCENDING);
    state->device_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(state->devices));
    g_object_unref(state->devices);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(state->device_view), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "stock-size", GTK_ICON_SIZE_DND, "xpad", 6, "ypad", 4, nullptr);
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", DEV_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "markup", DEV_TEXT);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state->device_view), col);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 220);
    gtk_container_add(GTK_CONTAINER(scroll), state->device_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->add = gtk_button_new_with_mnemonic("A_dd...");
    state->remove = gtk_button_new_with_mnemonic("_Remove");
    state->properties = gtk_button_new_with_mnemonic("Propert_ies");
    gtk_box_pack_start(GTK_BOX(row), state->add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), state->remove, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), state->properties, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    g_signal_connect(state->add, "clicked", G_CALLBACK(on_add), state);
    g_signal_connect(state->remove, "clicked", G_CALLBACK(on_remove), state);
    g_signal_connect(state->properties, "clicked", G_CALLBACK(on_properties), state);
    g_signal_connect(state->device_view, "row-activated", G_CALLBACK(on_device_activated), state);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->device_view)), "changed",
                     G_CALLBACK(on_device_selected), state);
    return page;
}

/* ---- Options ----------------------------------------------------------------------- */

static void changed(bt_app *state) {
    if (!state->updating)
        gtk_widget_set_sensitive(state->apply, TRUE);
}

static void on_option([[maybe_unused]] GtkWidget *widget, gpointer data) { changed(data); }

/* Shows the adapter's settings, unless they're being edited. */
static void show_options(bt_app *state) {
    if (gtk_widget_get_sensitive(state->apply))
        return;
    GDBusInterface *ad = bt_adapter(state);
    state->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->discoverable),
                                 bt_prop_b(ad, "Discoverable"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->pairable), bt_prop_b(ad, "Pairable"));
    char *alias = bt_prop_s(ad, "Alias");
    gtk_entry_set_text(GTK_ENTRY(state->alias), alias != nullptr ? alias : "");
    g_free(alias);
    state->updating = false;
    bool on = ad != nullptr && bt_prop_b(ad, "Powered");
    gtk_widget_set_sensitive(state->discoverable, on);
    gtk_widget_set_sensitive(state->pairable, on);
    gtk_widget_set_sensitive(state->alias, ad != nullptr);
    gtk_label_set_text(GTK_LABEL(state->options_note),
                       ad == nullptr ? "No Bluetooth radio was found."
                       : !on         ? "Bluetooth is turned off. Turn it on from its icon in the "
                                       "notification area."
                                     : "");
    if (ad != nullptr)
        g_object_unref(ad);
}

static GtkWidget *note(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 56);
    return label;
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *frame_widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame_widget), child);
    return frame_widget;
}

static GtkWidget *options_page(bt_app *state) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *disc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    state->discoverable = gtk_check_button_new_with_mnemonic(
        "_Turn discovery on\n(Allow Bluetooth devices to find this computer)");
    gtk_box_pack_start(GTK_BOX(disc), state->discoverable, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(disc),
                       note("To protect your privacy, select this check box only when you "
                            "want a Bluetooth device to find this computer. Discovery turns "
                            "itself off again after a while."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Discovery", disc), FALSE, FALSE, 0);

    GtkWidget *conn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    state->pairable =
        gtk_check_button_new_with_mnemonic("Allow Bluetooth devices to _connect to this computer");
    gtk_widget_set_tooltip_text(state->pairable, "Lets new devices pair. Devices already added can "
                                                 "connect either way.");
    gtk_box_pack_start(GTK_BOX(conn), state->pairable, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Connections", conn), FALSE, FALSE, 0);

    state->show_icon =
        gtk_check_button_new_with_mnemonic("_Show the Bluetooth icon in the notification area");
    xd_settings settings;
    xd_settings_load(&settings);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->show_icon), settings.bluetooth_icon);
    gtk_box_pack_start(GTK_BOX(page), state->show_icon, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("Computer _name:");
    state->alias = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(state->alias), 248); /* the HCI name's bytes */
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), state->alias);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), state->alias, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), note("Bluetooth devices see this computer by this name."),
                       FALSE, FALSE, 0);

    state->options_note = note("");
    gtk_box_pack_end(GTK_BOX(page), state->options_note, FALSE, FALSE, 0);

    show_options(state);
    g_signal_connect(state->discoverable, "toggled", G_CALLBACK(on_option), state);
    g_signal_connect(state->pairable, "toggled", G_CALLBACK(on_option), state);
    g_signal_connect(state->show_icon, "toggled", G_CALLBACK(on_option), state);
    g_signal_connect(state->alias, "changed", G_CALLBACK(on_option), state);
    return page;
}

/* ---- Hardware ----------------------------------------------------------------------- */

enum { RADIO_NAME, RADIO_TYPE, RADIO_PATH, RADIO_COUNT };

/* A value from the hardware database (hwdb) for a modalias. */
static char *hwdb_value(struct udev *udev, const char *modalias, const char *key) {
    struct udev_hwdb *hwdb = udev != nullptr ? udev_hwdb_new(udev) : nullptr;
    char *value = nullptr;
    struct udev_list_entry *list_entry;
    if (hwdb != nullptr && modalias != nullptr) {
        udev_list_entry_foreach(list_entry,
                                udev_hwdb_get_properties_list_entry(hwdb, modalias, 0)) {
            if (strcmp(udev_list_entry_get_name(list_entry), key) == 0) {
                value = g_strdup(udev_list_entry_get_value(list_entry));
                break;
            }
        }
    }
    if (hwdb != nullptr)
        udev_hwdb_unref(hwdb);
    return value;
}

/* The radio's chip: the USB, PCI, SDIO or serial device under hciN. */
static struct udev_device *radio_parent(struct udev_device *hci) {
    for (struct udev_device *device = udev_device_get_parent(hci); device != nullptr;
         device = udev_device_get_parent(device)) {
        const char *sub = udev_device_get_subsystem(device);
        if (sub != nullptr && (strcmp(sub, "usb") == 0 || strcmp(sub, "pci") == 0 ||
                               strcmp(sub, "sdio") == 0 || strcmp(sub, "serial") == 0 ||
                               strcmp(sub, "serial-base") == 0 || strcmp(sub, "tty") == 0))
            return device;
    }
    return nullptr;
}

static const char *bus_location(const char *subsystem) {
    if (subsystem == nullptr)
        return "Unknown";
    if (strcmp(subsystem, "usb") == 0)
        return "on USB";
    if (strcmp(subsystem, "pci") == 0)
        return "on PCI bus";
    if (strcmp(subsystem, "sdio") == 0)
        return "on SDIO";
    return "on a serial port (UART)";
}

/* What's known of a radio's chip; free with radio_hw_free. */
typedef struct radio_hw {
    char *maker, *model; /* nullptr when unknown */
    const char *location;
} radio_hw;

static radio_hw radio_info(GDBusInterface *ad, const char *path) {
    radio_hw hw = {.location = "Unknown"};
    struct udev *udev = udev_new();
    /* hciN, from /org/bluez/hciN. */
    const char *hci = strrchr(path, '/') != nullptr ? strrchr(path, '/') + 1 : path;
    struct udev_device *dev =
        udev != nullptr ? udev_device_new_from_subsystem_sysname(udev, "bluetooth", hci) : nullptr;
    struct udev_device *chip = dev != nullptr ? radio_parent(dev) : nullptr;
    if (chip != nullptr) {
        const char *vendor = udev_device_get_property_value(chip, "ID_VENDOR_FROM_DATABASE");
        const char *model = udev_device_get_property_value(chip, "ID_MODEL_FROM_DATABASE");
        hw.maker = vendor != nullptr ? g_strdup(vendor) : nullptr;
        hw.model = model != nullptr ? g_strdup(model) : nullptr;
        hw.location = bus_location(udev_device_get_subsystem(chip));
    } else if (dev != nullptr) {
        hw.location = "Virtual device";
    }
    /* Else the chip's maker from its Bluetooth company identifier. */
    GVariant *company = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(ad), "Manufacturer");
    if (hw.maker == nullptr && company != nullptr) {
        char *key = g_strdup_printf("bluetooth:v%04X", bt_prop_u(ad, "Manufacturer"));
        hw.maker = hwdb_value(udev, key, "ID_VENDOR_FROM_DATABASE");
        g_free(key);
    }
    if (company != nullptr)
        g_variant_unref(company);
    /* Else what BlueZ reports as its Device ID. */
    char *modalias = bt_prop_s(ad, "Modalias");
    if (hw.maker == nullptr)
        hw.maker = hwdb_value(udev, modalias, "ID_VENDOR_FROM_DATABASE");
    if (hw.model == nullptr)
        hw.model = hwdb_value(udev, modalias, "ID_MODEL_FROM_DATABASE");
    g_free(modalias);
    if (dev != nullptr)
        udev_device_unref(dev);
    if (udev != nullptr)
        udev_unref(udev);
    return hw;
}

static void radio_hw_free(radio_hw *hw) {
    g_free(hw->maker);
    g_free(hw->model);
}

static void show_radio(bt_app *state, const char *path) {
    GDBusInterface *ad = bt_interface(state, path, IFACE_ADAPTER);
    if (ad == nullptr) {
        GtkWidget *labels[] = {state->maker,   state->model,    state->address,
                               state->version, state->location, state->status};
        for (size_t i = 0; i < XD_ARRAY_LEN(labels); i++)
            gtk_label_set_text(GTK_LABEL(labels[i]), "");
        return;
    }
    radio_hw hw = radio_info(ad, path);
    gtk_label_set_text(GTK_LABEL(state->maker), hw.maker != nullptr ? hw.maker : "(Unknown)");
    gtk_label_set_text(GTK_LABEL(state->model), hw.model != nullptr ? hw.model : "(Unknown)");
    char *address = bt_prop_s(ad, "Address");
    gtk_label_set_text(GTK_LABEL(state->address), address != nullptr ? address : "");
    GVariant *ver = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(ad), "Version");
    const char *version =
        ver != nullptr ? xd_bt_version_name((int)bt_prop_u(ad, "Version")) : nullptr;
    gtk_label_set_text(GTK_LABEL(state->version), version != nullptr ? version : "(Unknown)");
    if (ver != nullptr)
        g_variant_unref(ver);
    gtk_label_set_text(GTK_LABEL(state->location), hw.location);
    gtk_label_set_text(GTK_LABEL(state->status), bt_prop_b(ad, "Powered")
                                                     ? "This device is working properly."
                                                     : "This device is turned off.");
    g_free(address);
    radio_hw_free(&hw);
    g_object_unref(ad);
}

static void on_radio_selected(GtkTreeSelection *sel, gpointer data) {
    bt_app *state = data;
    GtkTreeModel *model;
    GtkTreeIter it;
    char *path = nullptr;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, RADIO_PATH, &path, -1);
    show_radio(state, path);
    g_free(path);
}

static void fill_radios(bt_app *state) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(state->radio_view));
    GtkTreeModel *model;
    GtkTreeIter it;
    char *keep = nullptr;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, RADIO_PATH, &keep, -1);
    gtk_list_store_clear(state->radios);
    GList *objs =
        state->objects != nullptr ? g_dbus_object_manager_get_objects(state->objects) : nullptr;
    for (GList *list = objs; list != nullptr; list = list->next) {
        GDBusInterface *ad = g_dbus_object_get_interface(list->data, IFACE_ADAPTER);
        if (ad == nullptr)
            continue;
        const char *path = g_dbus_object_get_object_path(list->data);
        /* The chip's name, as Windows has its driver's. */
        radio_hw hw = radio_info(ad, path);
        char *name = hw.model != nullptr ? g_strdup(hw.model)
                                         : g_strdup_printf("Bluetooth Radio (%s)",
                                                           strrchr(path, '/') + 1);
        radio_hw_free(&hw);
        gtk_list_store_insert_with_values(state->radios, &it, -1, RADIO_NAME, name, RADIO_TYPE,
                                          "Bluetooth Radios", RADIO_PATH, path, -1);
        if (keep == nullptr || strcmp(keep, path) == 0) {
            gtk_tree_selection_select_iter(sel, &it);
            g_free(keep);
            keep = g_strdup(path);
        }
        g_free(name);
        g_object_unref(ad);
    }
    g_list_free_full(objs, g_object_unref);
    g_free(keep);
    on_radio_selected(sel, state);
}

static GtkWidget *value_label(GtkWidget *grid, int row, const char *title) {
    GtkWidget *title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    GtkWidget *value = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(value), 0);
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(grid), title_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
    return value;
}

static GtkWidget *hardware_page(bt_app *state) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *title = gtk_label_new_with_mnemonic("_Devices:");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);
    state->radios = gtk_list_store_new(RADIO_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    state->radio_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(state->radios));
    g_object_unref(state->radios);
    gtk_label_set_mnemonic_widget(GTK_LABEL(title), state->radio_view);
    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(column, "Name");
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "icon-name", "bluetooth", nullptr);
    gtk_tree_view_column_pack_start(column, icon, FALSE);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(column, renderer, TRUE);
    gtk_tree_view_column_add_attribute(column, renderer, "text", RADIO_NAME);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state->radio_view), column);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(state->radio_view),
        gtk_tree_view_column_new_with_attributes("Type", gtk_cell_renderer_text_new(), "text",
                                                 RADIO_TYPE, nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 100);
    gtk_container_add(GTK_CONTAINER(scroll), state->radio_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    state->maker = value_label(grid, 0, "Manufacturer:");
    state->model = value_label(grid, 1, "Model:");
    state->address = value_label(grid, 2, "Address:");
    state->version = value_label(grid, 3, "Bluetooth version:");
    state->location = value_label(grid, 4, "Location:");
    state->status = value_label(grid, 5, "Device status:");
    gtk_box_pack_start(GTK_BOX(page), frame("Device Properties", grid), FALSE, FALSE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->radio_view)), "changed",
                     G_CALLBACK(on_radio_selected), state);
    return page;
}

/* ---- live updates ----------------------------------------------------------------- */

static void refresh(bt_app *state) {
    if (state->window == nullptr)
        return; /* closed while the wizard stays */
    fill_devices(state);
    device_buttons(state);
    show_options(state);
    fill_radios(state);
}

/* Devices change often while searching (signal strength): only what
 * changed is redone. */
static void props_changed([[maybe_unused]] GDBusObjectManagerClient *dbus_object_manager_client,
                          [[maybe_unused]] GDBusObjectProxy *object, GDBusProxy *proxy,
                          GVariant *changed, const char *const *inv, gpointer data) {
    bt_app *state = data;
    if (state->window == nullptr)
        return;
    const char *iface = g_dbus_proxy_get_interface_name(proxy);
    if (strcmp(iface, IFACE_DEVICE) == 0) {
        /* The list has paired devices only, and doesn't show their signal
         * strength (RSSI) or advertising data, which change all the time. */
        GVariant *paired = g_variant_lookup_value(changed, "Paired", nullptr);
        bool listed = paired != nullptr || bt_prop_b(G_DBUS_INTERFACE(proxy), "Paired");
        if (paired != nullptr)
            g_variant_unref(paired);
        static const char *const shown[] = {"Paired",     "Alias", "Name",      "Address",
                                            "Class",      "Icon",  "Connected", "Blocked",
                                            "Appearance", "LegacyPairing"};
        bool relevant = inv != nullptr && inv[0] != nullptr;
        for (size_t name_index = 0; name_index < XD_ARRAY_LEN(shown) && !relevant; name_index++) {
            GVariant *value = g_variant_lookup_value(changed, shown[name_index], nullptr);
            relevant = value != nullptr;
            if (value != nullptr)
                g_variant_unref(value);
        }
        if (!listed || !relevant)
            return;
        fill_devices(state);
        device_buttons(state);
    } else if (strcmp(iface, IFACE_ADAPTER) == 0) {
        device_buttons(state);
        show_options(state);
        on_radio_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->radio_view)), state);
    }
}

static void added_removed([[maybe_unused]] GDBusObjectManager *dbus_object_manager,
                          GDBusObject *object, gpointer data) {
    bt_app *state = data;
    /* A device (BlueZ's path for one is its adapter's plus "/dev_..."), as
     * they come and go while searching: the radios (read from udev and the
     * hardware database) stay as they are. */
    if (state->window != nullptr &&
        strstr(g_dbus_object_get_object_path(object), "/dev_") != nullptr) {
        fill_devices(state);
        device_buttons(state);
        return;
    }
    refresh(state);
}

static void owner_changed([[maybe_unused]] GObject *object, [[maybe_unused]] GParamSpec *ps,
                          gpointer data) {
    refresh(data);
}

/* ---- applying -------------------------------------------------------------------- */

static void apply(bt_app *state) {
    GDBusInterface *ad = bt_adapter(state);
    GtkWindow *parent = GTK_WINDOW(state->window);
    if (ad != nullptr) {
        const char *path = g_dbus_proxy_get_object_path(G_DBUS_PROXY(ad));
        bool disc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->discoverable));
        if (disc != bt_prop_b(ad, "Discoverable"))
            bt_set_prop(state, parent, path, IFACE_ADAPTER, "Discoverable",
                        g_variant_new_boolean(disc));
        bool pair = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->pairable));
        if (pair != bt_prop_b(ad, "Pairable"))
            bt_set_prop(state, parent, path, IFACE_ADAPTER, "Pairable",
                        g_variant_new_boolean(pair));
        char *alias = bt_prop_s(ad, "Alias");
        const char *name = gtk_entry_get_text(GTK_ENTRY(state->alias));
        if (name[0] != '\0' && g_strcmp0(alias, name) != 0)
            bt_set_prop(state, parent, path, IFACE_ADAPTER, "Alias", g_variant_new_string(name));
        g_free(alias);
        g_object_unref(ad);
    }
    /* Re-read: another applet may have saved since we opened. */
    xd_settings settings;
    xd_settings_load(&settings);
    bool icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->show_icon));
    if (settings.bluetooth_icon != icon) {
        settings.bluetooth_icon = icon;
        if (!xd_settings_save(&settings))
            xd_message_box(parent, BT_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "The settings could not be saved.");
    }
    gtk_widget_set_sensitive(state->apply, FALSE);
}

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    bt_app *state = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(state);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(state->apply))
        apply(state);
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    bt_app *state = data;
    state->window = nullptr;
    if (state->wizard == nullptr)
        gtk_main_quit();
}

static void build_window(bt_app *state) {
    state->window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(state->window), BT_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(state->window), "bluetooth");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 420, -1);
    gtk_dialog_add_button(GTK_DIALOG(state->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(state->window), "Cancel", GTK_RESPONSE_CANCEL);
    state->apply = gtk_dialog_add_button(GTK_DIALOG(state->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(state->apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(state->window), GTK_RESPONSE_OK);

    state->tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(state->tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(state->tabs), devices_page(state),
                             gtk_label_new_with_mnemonic("De_vices"));
    gtk_notebook_append_page(GTK_NOTEBOOK(state->tabs), options_page(state),
                             gtk_label_new_with_mnemonic("_Options"));
    gtk_notebook_append_page(GTK_NOTEBOOK(state->tabs), hardware_page(state),
                             gtk_label_new_with_mnemonic("_Hardware"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(state->window))), state->tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(state->window, GTK_NOTEBOOK(state->tabs));
    g_signal_connect(state->window, "response", G_CALLBACK(on_response), state);
    g_signal_connect(state->window, "destroy", G_CALLBACK(on_destroy), state);
}

/* Killed: still say goodbye to BlueZ (stop discovery, unregister). */
static gboolean on_signal([[maybe_unused]] gpointer data) {
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Bluetooth");
    gboolean add = FALSE, options = FALSE;
    const GOptionEntry entries[] = {
        {"add", 0, 0, G_OPTION_ARG_NONE, &add, "Open the Add Bluetooth Device Wizard", nullptr},
        {"options", 0, 0, G_OPTION_ARG_NONE, &options, "Open the Options tab", nullptr},
        G_OPTION_ENTRY_NULL,
    };
    GError *error = nullptr;
    if (!gtk_init_with_args(&argc, &argv, nullptr, entries, nullptr, &error)) {
        g_printerr("xd-bluetooth: %s\n", error != nullptr ? error->message : "cannot start");
        return EXIT_FAILURE;
    }
    xd_gtk_app_follow_theme();
    static bt_app state;
    state.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (state.bus != nullptr) {
        /* Don't start bluetoothd just to look. */
        state.objects = g_dbus_object_manager_client_new_sync(
            state.bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, BLUEZ, "/", nullptr,
            nullptr, nullptr, nullptr, &error);
    }
    if (state.objects == nullptr) {
        g_printerr("xd-bluetooth: %s\n", error != nullptr ? error->message : "no BlueZ");
        g_clear_error(&error);
    }

    build_window(&state);
    refresh(&state);
    if (state.objects != nullptr) {
        g_signal_connect(state.objects, "interface-proxy-properties-changed",
                         G_CALLBACK(props_changed), &state);
        g_signal_connect(state.objects, "object-added", G_CALLBACK(added_removed), &state);
        g_signal_connect(state.objects, "object-removed", G_CALLBACK(added_removed), &state);
        g_signal_connect(state.objects, "notify::name-owner", G_CALLBACK(owner_changed), &state);
        bt_agent_start(&state);
    }
    if (add) {
        state.quit_with_wizard = true;
        bt_wizard_open(&state);
    } else {
        gtk_widget_show_all(state.window);
        /* Once shown: a notebook only turns to visible pages. */
        if (options)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(state.tabs), 1);
    }
    g_unix_signal_add(SIGTERM, on_signal, nullptr);
    g_unix_signal_add(SIGINT, on_signal, nullptr);
    g_unix_signal_add(SIGHUP, on_signal, nullptr);
    gtk_main();
    if (state.wizard != nullptr)
        gtk_widget_destroy(GTK_WIDGET(bt_wizard_window(&state)));
    if (state.window != nullptr) {
        g_signal_handlers_disconnect_by_data(state.window, &state);
        gtk_widget_destroy(state.window);
    }
    if (state.objects != nullptr) {
        bt_agent_stop(&state);
        g_signal_handlers_disconnect_by_data(state.objects, &state);
        g_object_unref(state.objects);
    }
    if (state.bus != nullptr)
        g_object_unref(state.bus);
    return EXIT_SUCCESS;
}
