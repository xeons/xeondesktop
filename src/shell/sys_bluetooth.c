/* SPDX-License-Identifier: MIT
 * The Bluetooth icon in the notification area, from BlueZ over D-Bus
 * (its ObjectManager gives us adapters and devices with live properties).
 * Shown when the machine has an adapter, unless Bluetooth Devices'
 * Options turned it off ([Bluetooth] ShowIcon). Its menu turns Bluetooth
 * on or off, connects or disconnects paired devices, and has XP's items
 * for Bluetooth Devices (xd-bluetooth).
 */
#include <gio/gio.h>

#include "shell.h"

#define BLUEZ "org.bluez"
#define IFACE_ADAPTER "org.bluez.Adapter1"
#define IFACE_DEVICE "org.bluez.Device1"

typedef struct bluetooth {
    xd_shell *shell;
    GCancellable *cancel;
    GDBusObjectManager *objects;
    int shown; /* what the icon last showed: -1 none, 0 off, 1 on */
} bluetooth;

typedef struct device_choice {
    bluetooth *bt;
    const char *path;
    bool connected;
} device_choice;

static GVariant *prop(GDBusInterface *i, const char *name) {
    return i != nullptr ? g_dbus_proxy_get_cached_property(G_DBUS_PROXY(i), name) : nullptr;
}

static bool prop_b(GDBusInterface *i, const char *name) {
    GVariant *v = prop(i, name);
    bool b = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
             g_variant_get_boolean(v);
    if (v != nullptr)
        g_variant_unref(v);
    return b;
}

static char *prop_s(GDBusInterface *i, const char *name) {
    GVariant *v = prop(i, name);
    char *s = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                  ? g_variant_dup_string(v, nullptr)
                  : nullptr;
    if (v != nullptr)
        g_variant_unref(v);
    return s;
}

/* The first adapter (most machines have one); caller unrefs. */
static GDBusInterface *adapter(bluetooth *bt) {
    if (bt->objects == nullptr)
        return nullptr;
    GList *objs = g_dbus_object_manager_get_objects(bt->objects);
    GDBusInterface *found = nullptr;
    for (GList *o = objs; o != nullptr && found == nullptr; o = o->next)
        found = g_dbus_object_get_interface(o->data, IFACE_ADAPTER);
    g_list_free_full(objs, g_object_unref);
    return found;
}

static int connected_count(bluetooth *bt) {
    int n = 0;
    GList *objs = g_dbus_object_manager_get_objects(bt->objects);
    for (GList *o = objs; o != nullptr; o = o->next) {
        GDBusInterface *d = g_dbus_object_get_interface(o->data, IFACE_DEVICE);
        if (d != nullptr) {
            n += prop_b(d, "Connected");
            g_object_unref(d);
        }
    }
    g_list_free_full(objs, g_object_unref);
    return n;
}

static const char *icon(void *data) {
    bluetooth *bt = data;
    GDBusInterface *a = adapter(bt);
    if (a == nullptr || !bt->shell->settings.bluetooth_icon)
        return nullptr; /* no adapter, or turned off: no icon */
    bool on = prop_b(a, "Powered");
    g_object_unref(a);
    return on ? "bluetooth-active" : "bluetooth-disabled";
}

static char *tooltip(void *data) {
    bluetooth *bt = data;
    GDBusInterface *a = adapter(bt);
    bool on = a != nullptr && prop_b(a, "Powered");
    if (a != nullptr)
        g_object_unref(a);
    if (!on)
        return g_strdup("Bluetooth is off");
    int n = connected_count(bt);
    return n == 0   ? g_strdup("Bluetooth: no devices connected")
           : n == 1 ? g_strdup("Bluetooth: 1 device connected")
                    : g_strdup_printf("Bluetooth: %d devices connected", n);
}

static void call_done(GObject *src, GAsyncResult *res, [[maybe_unused]] gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r != nullptr) {
        g_variant_unref(r);
    } else {
        fprintf(stderr, "xd-shell: bluetooth: %s\n", error->message);
        g_error_free(error);
    }
}

static GDBusConnection *bus_of(bluetooth *bt) {
    return g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(bt->objects));
}

static void act_power(xd_shell *shell, void *d) {
    (void)shell;
    bluetooth *bt = d;
    GDBusInterface *a = adapter(bt);
    if (a == nullptr)
        return;
    bool on = prop_b(a, "Powered");
    const char *path = g_dbus_proxy_get_object_path(G_DBUS_PROXY(a));
    g_dbus_connection_call(bus_of(bt), BLUEZ, path, "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)", IFACE_ADAPTER, "Powered",
                                         g_variant_new_boolean(!on)),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, -1, bt->cancel, call_done, bt);
    g_object_unref(a);
}

static void act_device(xd_shell *shell, void *d) {
    (void)shell;
    const device_choice *c = d;
    /* Connecting can take a while (and BlueZ may ask the device to pair). */
    g_dbus_connection_call(bus_of(c->bt), BLUEZ, c->path, IFACE_DEVICE,
                           c->connected ? "Disconnect" : "Connect", nullptr, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, 60000, c->bt->cancel, call_done, c->bt);
}

/* XP's items for Bluetooth Devices. */
static const struct {
    const char *label, *command;
} devices_items[] = {
    {"Add a Bluetooth Device", "xd-bluetooth --add"},
    {"Show Bluetooth Devices", "xd-bluetooth"},
    {"Open Bluetooth Settings", "xd-bluetooth --options"},
};

static void act_devices(xd_shell *shell, void *d) {
    xd_shell_launch(shell, devices_items[GPOINTER_TO_SIZE(d)].command);
}

/* XP's Remove Bluetooth Icon: what Options' check box turns back on. */
static void act_remove_icon(xd_shell *shell, [[maybe_unused]] void *d) {
    shell->settings.bluetooth_icon = false;
    if (!xd_settings_save(&shell->settings))
        fprintf(stderr, "xd-shell: cannot save settings\n");
    xd_tray_changed(shell);
}

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *m) {
    bluetooth *bt = data;
    GDBusInterface *a = adapter(bt);
    bool on = a != nullptr && prop_b(a, "Powered");
    if (a != nullptr)
        g_object_unref(a);
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION,
                                   on ? "Turn Bluetooth Off" : "Turn Bluetooth On", nullptr);
    it->action = act_power;
    it->action_data = bt;
    if (on) {
        /* Paired devices: a check mark on connected ones; clicking toggles. */
        bool any = false;
        GList *objs = g_dbus_object_manager_get_objects(bt->objects);
        for (GList *o = objs; o != nullptr; o = o->next) {
            GDBusInterface *dev = g_dbus_object_get_interface(o->data, IFACE_DEVICE);
            if (dev == nullptr)
                continue;
            if (prop_b(dev, "Paired")) {
                if (!any)
                    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
                any = true;
                char *name = prop_s(dev, "Alias");
                char *dev_icon = prop_s(dev, "Icon");
                device_choice *c = XD_ARENA_NEW(&tree->arena, device_choice);
                c->bt = bt;
                c->path = xd_str_arena_cstr(
                    &tree->arena, xd_str_from_cstr(g_dbus_object_get_object_path(o->data)));
                c->connected = prop_b(dev, "Connected");
                it = xd_menu_add(tree, m, XD_MENU_ACTION, name != nullptr ? name : "Device",
                                 dev_icon);
                it->action = act_device;
                it->action_data = c;
                it->checked = c->connected;
                g_free(name);
                g_free(dev_icon);
            }
            g_object_unref(dev);
        }
        g_list_free_full(objs, g_object_unref);
    }
    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
    bool have = xd_shell_have_program("xd-bluetooth");
    for (size_t i = 0; i < XD_ARRAY_LEN(devices_items); i++) {
        it = xd_menu_add(tree, m, XD_MENU_ACTION, devices_items[i].label, nullptr);
        it->action = act_devices;
        it->action_data = GSIZE_TO_POINTER(i);
        it->disabled = !have || (i == 0 && !on); /* adding needs the radio on */
    }
    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
    it = xd_menu_add(tree, m, XD_MENU_ACTION, "Remove Bluetooth Icon", nullptr);
    it->action = act_remove_icon;
}

/* Every property of every device lands here (their signal strength while
 * discovering): redraw only when the adapter came, went or switched. */
static void changed(bluetooth *bt) {
    GDBusInterface *a = adapter(bt);
    int shown = a == nullptr ? -1 : prop_b(a, "Powered");
    if (a != nullptr)
        g_object_unref(a);
    if (shown == bt->shown)
        return;
    bt->shown = shown;
    xd_tray_changed(bt->shell);
}

static void props_changed([[maybe_unused]] GDBusObjectManagerClient *m,
                          [[maybe_unused]] GDBusObjectProxy *o, [[maybe_unused]] GDBusProxy *i,
                          [[maybe_unused]] GVariant *c, [[maybe_unused]] const char *const *inv,
                          gpointer data) {
    changed(data);
}

static void added_removed([[maybe_unused]] GDBusObjectManager *m, [[maybe_unused]] GDBusObject *o,
                          gpointer data) {
    changed(data);
}

static void got_objects([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GDBusObjectManager *m = g_dbus_object_manager_client_new_for_bus_finish(res, nullptr);
    if (m == nullptr)
        return; /* no BlueZ */
    bluetooth *bt = data;
    bt->objects = m;
    g_signal_connect(m, "interface-proxy-properties-changed", G_CALLBACK(props_changed), bt);
    g_signal_connect(m, "object-added", G_CALLBACK(added_removed), bt);
    g_signal_connect(m, "object-removed", G_CALLBACK(added_removed), bt);
    changed(bt);
}

static void destroy(void *data) {
    bluetooth *bt = data;
    g_cancellable_cancel(bt->cancel);
    g_object_unref(bt->cancel);
    if (bt->objects != nullptr) {
        g_signal_handlers_disconnect_by_data(bt->objects, bt);
        g_object_unref(bt->objects);
    }
    g_free(bt);
}

static void draw(void *data, const xd_theme *t, cairo_t *cr, xd_rect r) {
    bluetooth *bt = data;
    GDBusInterface *a = adapter(bt);
    bool on = a != nullptr && prop_b(a, "Powered");
    if (a != nullptr)
        g_object_unref(a);
    xd_theme_draw_tray_icon(t, cr, r, XD_TRAY_ICON_BLUETOOTH, 0, on ? 0 : XD_TRAY_ICON_OFF);
}

static const xd_sysicon_ops ops = {
    .draw = draw,
    .icon = icon,
    .tooltip = tooltip,
    .menu = menu,
    .destroy = destroy,
};

void xd_sys_bluetooth_init(xd_shell *shell) {
    bluetooth *bt = g_new0(bluetooth, 1);
    bt->shell = shell;
    bt->cancel = g_cancellable_new();
    bt->shown = -1;
    xd_tray_add_sysicon(shell, "bluetooth", &ops, bt);
    g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM,
                                             G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, BLUEZ, "/",
                                             nullptr, nullptr, nullptr, bt->cancel, got_objects,
                                             bt);
}
