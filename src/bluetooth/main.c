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

static GVariant *prop(GDBusInterface *i, const char *name) {
    return i != nullptr ? g_dbus_proxy_get_cached_property(G_DBUS_PROXY(i), name) : nullptr;
}

bool bt_prop_b(GDBusInterface *i, const char *name) {
    GVariant *v = prop(i, name);
    bool b =
        v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
    if (v != nullptr)
        g_variant_unref(v);
    return b;
}

char *bt_prop_s(GDBusInterface *i, const char *name) {
    GVariant *v = prop(i, name);
    char *s = v != nullptr && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
                               g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
                  ? g_variant_dup_string(v, nullptr)
                  : nullptr;
    if (v != nullptr)
        g_variant_unref(v);
    return s;
}

/* Unsigned integers of any width (Class is u, Appearance and
 * Manufacturer q, Version y). */
uint32_t bt_prop_u(GDBusInterface *i, const char *name) {
    GVariant *v = prop(i, name);
    uint32_t n = 0;
    if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
        n = g_variant_get_uint32(v);
    else if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT16))
        n = g_variant_get_uint16(v);
    else if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE))
        n = g_variant_get_byte(v);
    if (v != nullptr)
        g_variant_unref(v);
    return n;
}

GDBusInterface *bt_adapter(bt_app *a) {
    if (a->objects == nullptr)
        return nullptr;
    GList *objs = g_dbus_object_manager_get_objects(a->objects);
    GDBusInterface *found = nullptr;
    for (GList *o = objs; o != nullptr && found == nullptr; o = o->next)
        found = g_dbus_object_get_interface(o->data, IFACE_ADAPTER);
    g_list_free_full(objs, g_object_unref);
    return found;
}

GDBusInterface *bt_interface(bt_app *a, const char *path, const char *iface) {
    return a->objects != nullptr && path != nullptr
               ? g_dbus_object_manager_get_interface(a->objects, path, iface)
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
    xd_bt_type t = xd_bt_class_type(bt_prop_u(dev, "Class"));
    if (t.name == nullptr)
        t = xd_bt_appearance_type((uint16_t)bt_prop_u(dev, "Appearance"));
    if (t.name == nullptr) {
        char *icon = bt_prop_s(dev, "Icon");
        t = xd_bt_icon_type(icon);
        g_free(icon);
    }
    return t;
}

void bt_error(GtkWindow *parent, const char *what, const GError *error) {
    /* BlueZ's own message, without GDBus' "GDBus.Error:org.bluez..." */
    GError *e = g_error_copy(error);
    g_dbus_error_strip_remote_error(e);
    char *text = g_strdup_printf("%s\n\n%s", what, e->message);
    xd_message_box(parent, BT_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
    g_error_free(e);
}

typedef struct set_call {
    GtkWindow *parent;
    char *name;
} set_call;

static void set_done(GObject *src, GAsyncResult *res, gpointer data) {
    set_call *c = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r != nullptr) {
        g_variant_unref(r);
    } else {
        char *what = g_strdup_printf("The setting \"%s\" could not be changed.", c->name);
        bt_error(c->parent, what, error);
        g_free(what);
        g_error_free(error);
    }
    if (c->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(c->parent), (gpointer *)&c->parent);
    g_free(c->name);
    g_free(c);
}

void bt_set_prop(bt_app *a, GtkWindow *parent, const char *path, const char *iface,
                 const char *name, GVariant *value) {
    set_call *c = g_new0(set_call, 1);
    c->parent = parent;
    c->name = g_strdup(name);
    if (parent != nullptr)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&c->parent);
    g_dbus_connection_call(a->bus, BLUEZ, path, "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)", iface, name, value), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, set_done, c);
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
        char *p = nullptr;
        gtk_tree_model_get(model, &it, DEV_PATH, &p, -1);
        bool match = g_strcmp0(p, path) == 0;
        g_free(p);
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

static void fill_devices(bt_app *a) {
    char *keep = selected_path(a->device_view);
    gtk_list_store_clear(a->devices);
    GList *objs = a->objects != nullptr ? g_dbus_object_manager_get_objects(a->objects) : nullptr;
    for (GList *o = objs; o != nullptr; o = o->next) {
        GDBusInterface *dev = g_dbus_object_get_interface(o->data, IFACE_DEVICE);
        if (dev == nullptr)
            continue;
        if (bt_prop_b(dev, "Paired")) {
            char *name = bt_device_name(dev);
            xd_bt_type t = bt_device_type(dev);
            char *text = g_markup_printf_escaped("%s\n<small>%s, %s</small>", name, t.name,
                                                 device_state(dev));
            char *sort = g_utf8_collate_key(name, -1);
            gtk_list_store_insert_with_values(
                a->devices, nullptr, -1, DEV_ICON, t.icon, DEV_TEXT, text, DEV_PATH,
                g_dbus_object_get_object_path(o->data), DEV_SORT, sort, -1);
            g_free(sort);
            g_free(text);
            g_free(name);
        }
        g_object_unref(dev);
    }
    g_list_free_full(objs, g_object_unref);
    select_path(a->device_view, a->devices, keep);
    g_free(keep);
}

static void device_buttons(bt_app *a) {
    char *path = selected_path(a->device_view);
    gtk_widget_set_sensitive(a->remove, path != nullptr);
    gtk_widget_set_sensitive(a->properties, path != nullptr);
    g_free(path);
    GDBusInterface *ad = bt_adapter(a);
    gtk_widget_set_sensitive(a->add, ad != nullptr && bt_prop_b(ad, "Powered"));
    if (ad != nullptr)
        g_object_unref(ad);
}

static void on_device_selected([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    device_buttons(data);
}

static void on_add([[maybe_unused]] GtkButton *b, gpointer data) { bt_wizard_open(data); }

static void on_properties([[maybe_unused]] GtkButton *b, gpointer data) {
    bt_app *a = data;
    char *path = selected_path(a->device_view);
    if (path != nullptr)
        bt_device_properties(a, path);
    g_free(path);
}

static void on_device_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                                [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    on_properties(nullptr, data);
}

static void remove_done(GObject *src, GAsyncResult *res, gpointer data) {
    bt_app *a = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r != nullptr) {
        g_variant_unref(r);
    } else {
        bt_error(GTK_WINDOW(a->window), "The device could not be removed.", error);
        g_error_free(error);
    }
}

static void on_remove([[maybe_unused]] GtkButton *b, gpointer data) {
    bt_app *a = data;
    char *path = selected_path(a->device_view);
    GDBusInterface *dev = bt_interface(a, path, IFACE_DEVICE);
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
    if (xd_message_box(GTK_WINDOW(a->window), "Remove Bluetooth Device", XD_MB_QUESTION,
                       XD_MB_YES_NO, TRUE, text) == GTK_RESPONSE_YES)
        g_dbus_connection_call(a->bus, BLUEZ, adapter, IFACE_ADAPTER, "RemoveDevice",
                               g_variant_new("(o)", path), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                               nullptr, remove_done, a);
    g_free(text);
    g_free(name);
    g_free(adapter);
    g_free(path);
    g_object_unref(dev);
}

static GtkWidget *devices_page(bt_app *a) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    a->devices =
        gtk_list_store_new(DEV_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(a->devices), DEV_SORT,
                                         GTK_SORT_ASCENDING);
    a->device_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->devices));
    g_object_unref(a->devices);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a->device_view), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "stock-size", GTK_ICON_SIZE_DND, "xpad", 6, "ypad", 4, nullptr);
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", DEV_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "markup", DEV_TEXT);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->device_view), col);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 220);
    gtk_container_add(GTK_CONTAINER(scroll), a->device_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    a->add = gtk_button_new_with_mnemonic("A_dd...");
    a->remove = gtk_button_new_with_mnemonic("_Remove");
    a->properties = gtk_button_new_with_mnemonic("Propert_ies");
    gtk_box_pack_start(GTK_BOX(row), a->add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), a->remove, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), a->properties, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    g_signal_connect(a->add, "clicked", G_CALLBACK(on_add), a);
    g_signal_connect(a->remove, "clicked", G_CALLBACK(on_remove), a);
    g_signal_connect(a->properties, "clicked", G_CALLBACK(on_properties), a);
    g_signal_connect(a->device_view, "row-activated", G_CALLBACK(on_device_activated), a);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->device_view)), "changed",
                     G_CALLBACK(on_device_selected), a);
    return page;
}

/* ---- Options ----------------------------------------------------------------------- */

static void changed(bt_app *a) {
    if (!a->updating)
        gtk_widget_set_sensitive(a->apply, TRUE);
}

static void on_option([[maybe_unused]] GtkWidget *w, gpointer data) { changed(data); }

/* Shows the adapter's settings, unless they're being edited. */
static void show_options(bt_app *a) {
    if (gtk_widget_get_sensitive(a->apply))
        return;
    GDBusInterface *ad = bt_adapter(a);
    a->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->discoverable), bt_prop_b(ad, "Discoverable"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->pairable), bt_prop_b(ad, "Pairable"));
    char *alias = bt_prop_s(ad, "Alias");
    gtk_entry_set_text(GTK_ENTRY(a->alias), alias != nullptr ? alias : "");
    g_free(alias);
    a->updating = false;
    bool on = ad != nullptr && bt_prop_b(ad, "Powered");
    gtk_widget_set_sensitive(a->discoverable, on);
    gtk_widget_set_sensitive(a->pairable, on);
    gtk_widget_set_sensitive(a->alias, ad != nullptr);
    gtk_label_set_text(GTK_LABEL(a->options_note),
                       ad == nullptr ? "No Bluetooth radio was found."
                       : !on         ? "Bluetooth is turned off. Turn it on from its icon in the "
                                       "notification area."
                                     : "");
    if (ad != nullptr)
        g_object_unref(ad);
}

static GtkWidget *note(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *options_page(bt_app *a) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *disc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    a->discoverable = gtk_check_button_new_with_mnemonic(
        "_Turn discovery on\n(Allow Bluetooth devices to find this computer)");
    gtk_box_pack_start(GTK_BOX(disc), a->discoverable, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(disc),
                       note("To protect your privacy, select this check box only when you "
                            "want a Bluetooth device to find this computer. Discovery turns "
                            "itself off again after a while."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Discovery", disc), FALSE, FALSE, 0);

    GtkWidget *conn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    a->pairable =
        gtk_check_button_new_with_mnemonic("Allow Bluetooth devices to _connect to this computer");
    gtk_widget_set_tooltip_text(a->pairable, "Lets new devices pair. Devices already added can "
                                             "connect either way.");
    gtk_box_pack_start(GTK_BOX(conn), a->pairable, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Connections", conn), FALSE, FALSE, 0);

    a->show_icon =
        gtk_check_button_new_with_mnemonic("_Show the Bluetooth icon in the notification area");
    xd_settings s;
    xd_settings_load(&s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->show_icon), s.bluetooth_icon);
    gtk_box_pack_start(GTK_BOX(page), a->show_icon, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("Computer _name:");
    a->alias = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(a->alias), 248); /* the HCI name's bytes */
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), a->alias);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), a->alias, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), note("Bluetooth devices see this computer by this name."),
                       FALSE, FALSE, 0);

    a->options_note = note("");
    gtk_box_pack_end(GTK_BOX(page), a->options_note, FALSE, FALSE, 0);

    show_options(a);
    g_signal_connect(a->discoverable, "toggled", G_CALLBACK(on_option), a);
    g_signal_connect(a->pairable, "toggled", G_CALLBACK(on_option), a);
    g_signal_connect(a->show_icon, "toggled", G_CALLBACK(on_option), a);
    g_signal_connect(a->alias, "changed", G_CALLBACK(on_option), a);
    return page;
}

/* ---- Hardware ----------------------------------------------------------------------- */

enum { RADIO_NAME, RADIO_TYPE, RADIO_PATH, RADIO_COUNT };

/* A value from the hardware database (hwdb) for a modalias. */
static char *hwdb_value(struct udev *udev, const char *modalias, const char *key) {
    struct udev_hwdb *hwdb = udev != nullptr ? udev_hwdb_new(udev) : nullptr;
    char *value = nullptr;
    struct udev_list_entry *e;
    if (hwdb != nullptr && modalias != nullptr) {
        udev_list_entry_foreach(e, udev_hwdb_get_properties_list_entry(hwdb, modalias, 0)) {
            if (strcmp(udev_list_entry_get_name(e), key) == 0) {
                value = g_strdup(udev_list_entry_get_value(e));
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
    for (struct udev_device *d = udev_device_get_parent(hci); d != nullptr;
         d = udev_device_get_parent(d)) {
        const char *sub = udev_device_get_subsystem(d);
        if (sub != nullptr && (strcmp(sub, "usb") == 0 || strcmp(sub, "pci") == 0 ||
                               strcmp(sub, "sdio") == 0 || strcmp(sub, "serial") == 0 ||
                               strcmp(sub, "serial-base") == 0 || strcmp(sub, "tty") == 0))
            return d;
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
        const char *v = udev_device_get_property_value(chip, "ID_VENDOR_FROM_DATABASE");
        const char *m = udev_device_get_property_value(chip, "ID_MODEL_FROM_DATABASE");
        hw.maker = v != nullptr ? g_strdup(v) : nullptr;
        hw.model = m != nullptr ? g_strdup(m) : nullptr;
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

static void show_radio(bt_app *a, const char *path) {
    GDBusInterface *ad = bt_interface(a, path, IFACE_ADAPTER);
    if (ad == nullptr) {
        GtkWidget *labels[] = {a->maker, a->model, a->address, a->version, a->location, a->status};
        for (size_t i = 0; i < XD_ARRAY_LEN(labels); i++)
            gtk_label_set_text(GTK_LABEL(labels[i]), "");
        return;
    }
    radio_hw hw = radio_info(ad, path);
    gtk_label_set_text(GTK_LABEL(a->maker), hw.maker != nullptr ? hw.maker : "(Unknown)");
    gtk_label_set_text(GTK_LABEL(a->model), hw.model != nullptr ? hw.model : "(Unknown)");
    char *address = bt_prop_s(ad, "Address");
    gtk_label_set_text(GTK_LABEL(a->address), address != nullptr ? address : "");
    GVariant *ver = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(ad), "Version");
    const char *version =
        ver != nullptr ? xd_bt_version_name((int)bt_prop_u(ad, "Version")) : nullptr;
    gtk_label_set_text(GTK_LABEL(a->version), version != nullptr ? version : "(Unknown)");
    if (ver != nullptr)
        g_variant_unref(ver);
    gtk_label_set_text(GTK_LABEL(a->location), hw.location);
    gtk_label_set_text(GTK_LABEL(a->status), bt_prop_b(ad, "Powered")
                                                 ? "This device is working properly."
                                                 : "This device is turned off.");
    g_free(address);
    radio_hw_free(&hw);
    g_object_unref(ad);
}

static void on_radio_selected(GtkTreeSelection *sel, gpointer data) {
    bt_app *a = data;
    GtkTreeModel *model;
    GtkTreeIter it;
    char *path = nullptr;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, RADIO_PATH, &path, -1);
    show_radio(a, path);
    g_free(path);
}

static void fill_radios(bt_app *a) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->radio_view));
    GtkTreeModel *model;
    GtkTreeIter it;
    char *keep = nullptr;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, RADIO_PATH, &keep, -1);
    gtk_list_store_clear(a->radios);
    GList *objs = a->objects != nullptr ? g_dbus_object_manager_get_objects(a->objects) : nullptr;
    for (GList *o = objs; o != nullptr; o = o->next) {
        GDBusInterface *ad = g_dbus_object_get_interface(o->data, IFACE_ADAPTER);
        if (ad == nullptr)
            continue;
        const char *path = g_dbus_object_get_object_path(o->data);
        /* The chip's name, as Windows has its driver's. */
        radio_hw hw = radio_info(ad, path);
        char *name = hw.model != nullptr ? g_strdup(hw.model)
                                         : g_strdup_printf("Bluetooth Radio (%s)",
                                                           strrchr(path, '/') + 1);
        radio_hw_free(&hw);
        gtk_list_store_insert_with_values(a->radios, &it, -1, RADIO_NAME, name, RADIO_TYPE,
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
    on_radio_selected(sel, a);
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

static GtkWidget *hardware_page(bt_app *a) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *title = gtk_label_new_with_mnemonic("_Devices:");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);
    a->radios = gtk_list_store_new(RADIO_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    a->radio_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->radios));
    g_object_unref(a->radios);
    gtk_label_set_mnemonic_widget(GTK_LABEL(title), a->radio_view);
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Name");
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "icon-name", "bluetooth", nullptr);
    gtk_tree_view_column_pack_start(c, icon, FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(c, r, TRUE);
    gtk_tree_view_column_add_attribute(c, r, "text", RADIO_NAME);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->radio_view), c);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(a->radio_view),
        gtk_tree_view_column_new_with_attributes("Type", gtk_cell_renderer_text_new(), "text",
                                                 RADIO_TYPE, nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 100);
    gtk_container_add(GTK_CONTAINER(scroll), a->radio_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    a->maker = value_label(grid, 0, "Manufacturer:");
    a->model = value_label(grid, 1, "Model:");
    a->address = value_label(grid, 2, "Address:");
    a->version = value_label(grid, 3, "Bluetooth version:");
    a->location = value_label(grid, 4, "Location:");
    a->status = value_label(grid, 5, "Device status:");
    gtk_box_pack_start(GTK_BOX(page), frame("Device Properties", grid), FALSE, FALSE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->radio_view)), "changed",
                     G_CALLBACK(on_radio_selected), a);
    return page;
}

/* ---- live updates ----------------------------------------------------------------- */

static void refresh(bt_app *a) {
    if (a->window == nullptr)
        return; /* closed while the wizard stays */
    fill_devices(a);
    device_buttons(a);
    show_options(a);
    fill_radios(a);
}

/* Devices change often while searching (signal strength): only what
 * changed is redone. */
static void props_changed([[maybe_unused]] GDBusObjectManagerClient *m,
                          [[maybe_unused]] GDBusObjectProxy *o, GDBusProxy *i, GVariant *c,
                          [[maybe_unused]] const char *const *inv, gpointer data) {
    bt_app *a = data;
    if (a->window == nullptr)
        return;
    const char *iface = g_dbus_proxy_get_interface_name(i);
    if (strcmp(iface, IFACE_DEVICE) == 0) {
        /* The list has paired devices only. */
        GVariant *paired = g_variant_lookup_value(c, "Paired", nullptr);
        bool listed = paired != nullptr || bt_prop_b(G_DBUS_INTERFACE(i), "Paired");
        if (paired != nullptr)
            g_variant_unref(paired);
        if (!listed)
            return;
        fill_devices(a);
        device_buttons(a);
    } else if (strcmp(iface, IFACE_ADAPTER) == 0) {
        device_buttons(a);
        show_options(a);
        on_radio_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->radio_view)), a);
    }
}

static void added_removed([[maybe_unused]] GDBusObjectManager *m, [[maybe_unused]] GDBusObject *o,
                          gpointer data) {
    refresh(data);
}

static void owner_changed([[maybe_unused]] GObject *m, [[maybe_unused]] GParamSpec *ps,
                          gpointer data) {
    refresh(data);
}

/* ---- applying -------------------------------------------------------------------- */

static void apply(bt_app *a) {
    GDBusInterface *ad = bt_adapter(a);
    GtkWindow *w = GTK_WINDOW(a->window);
    if (ad != nullptr) {
        const char *path = g_dbus_proxy_get_object_path(G_DBUS_PROXY(ad));
        bool disc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->discoverable));
        if (disc != bt_prop_b(ad, "Discoverable"))
            bt_set_prop(a, w, path, IFACE_ADAPTER, "Discoverable", g_variant_new_boolean(disc));
        bool pair = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->pairable));
        if (pair != bt_prop_b(ad, "Pairable"))
            bt_set_prop(a, w, path, IFACE_ADAPTER, "Pairable", g_variant_new_boolean(pair));
        char *alias = bt_prop_s(ad, "Alias");
        const char *name = gtk_entry_get_text(GTK_ENTRY(a->alias));
        if (name[0] != '\0' && g_strcmp0(alias, name) != 0)
            bt_set_prop(a, w, path, IFACE_ADAPTER, "Alias", g_variant_new_string(name));
        g_free(alias);
        g_object_unref(ad);
    }
    /* Re-read: another applet may have saved since we opened. */
    xd_settings s;
    xd_settings_load(&s);
    bool icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->show_icon));
    if (s.bluetooth_icon != icon) {
        s.bluetooth_icon = icon;
        if (!xd_settings_save(&s))
            xd_message_box(w, BT_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "The settings could not be saved.");
    }
    gtk_widget_set_sensitive(a->apply, FALSE);
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    bt_app *a = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(a);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(a->apply))
        apply(a);
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    bt_app *a = data;
    a->window = nullptr;
    if (a->wizard == nullptr)
        gtk_main_quit();
}

static void build_window(bt_app *a) {
    a->window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(a->window), BT_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(a->window), "bluetooth");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 420, -1);
    gtk_dialog_add_button(GTK_DIALOG(a->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(a->window), "Cancel", GTK_RESPONSE_CANCEL);
    a->apply = gtk_dialog_add_button(GTK_DIALOG(a->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(a->apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(a->window), GTK_RESPONSE_OK);

    a->tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(a->tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->tabs), devices_page(a),
                             gtk_label_new_with_mnemonic("De_vices"));
    gtk_notebook_append_page(GTK_NOTEBOOK(a->tabs), options_page(a),
                             gtk_label_new_with_mnemonic("_Options"));
    gtk_notebook_append_page(GTK_NOTEBOOK(a->tabs), hardware_page(a),
                             gtk_label_new_with_mnemonic("_Hardware"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(a->window))), a->tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(a->window, GTK_NOTEBOOK(a->tabs));
    g_signal_connect(a->window, "response", G_CALLBACK(on_response), a);
    g_signal_connect(a->window, "destroy", G_CALLBACK(on_destroy), a);
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
    static bt_app a;
    a.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (a.bus != nullptr) {
        /* Don't start bluetoothd just to look. */
        a.objects = g_dbus_object_manager_client_new_sync(
            a.bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, BLUEZ, "/", nullptr,
            nullptr, nullptr, nullptr, &error);
    }
    if (a.objects == nullptr) {
        g_printerr("xd-bluetooth: %s\n", error != nullptr ? error->message : "no BlueZ");
        g_clear_error(&error);
    }

    build_window(&a);
    refresh(&a);
    if (a.objects != nullptr) {
        g_signal_connect(a.objects, "interface-proxy-properties-changed", G_CALLBACK(props_changed),
                         &a);
        g_signal_connect(a.objects, "object-added", G_CALLBACK(added_removed), &a);
        g_signal_connect(a.objects, "object-removed", G_CALLBACK(added_removed), &a);
        g_signal_connect(a.objects, "notify::name-owner", G_CALLBACK(owner_changed), &a);
        bt_agent_start(&a);
    }
    if (add) {
        a.quit_with_wizard = true;
        bt_wizard_open(&a);
    } else {
        gtk_widget_show_all(a.window);
        /* Once shown: a notebook only turns to visible pages. */
        if (options)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(a.tabs), 1);
    }
    g_unix_signal_add(SIGTERM, on_signal, nullptr);
    g_unix_signal_add(SIGINT, on_signal, nullptr);
    g_unix_signal_add(SIGHUP, on_signal, nullptr);
    gtk_main();
    if (a.wizard != nullptr)
        gtk_widget_destroy(GTK_WIDGET(bt_wizard_window(&a)));
    if (a.window != nullptr) {
        g_signal_handlers_disconnect_by_data(a.window, &a);
        gtk_widget_destroy(a.window);
    }
    if (a.objects != nullptr) {
        bt_agent_stop(&a);
        g_signal_handlers_disconnect_by_data(a.objects, &a);
        g_object_unref(a.objects);
    }
    if (a.bus != nullptr)
        g_object_unref(a.bus);
    return EXIT_SUCCESS;
}
