/* SPDX-License-Identifier: MIT
 * xd-netconn: Windows XP's Network Connections, through NetworkManager
 * (libnm). The window shows a connection per network adapter, plus VPN,
 * broadband and spare wired connections, grouped as XP did, as tiles or
 * as a details list, with XP's task pane on the left.
 *
 *   per connection  Enable/Disable (the adapter), Connect/Disconnect,
 *                   Status (status.c), Repair, Rename, Delete,
 *                   Properties and TCP/IP (props.c)
 *   new             the New Connection Wizard (wizard.c); Wi-Fi networks
 *                   are picked in View Available Wireless Networks
 *
 * Every change is a NetworkManager call, so NetworkManager's polkit rules
 * decide what needs a password (xd-polkit asks).
 *
 *   xd-netconn                    the window
 *   xd-netconn --status [IFACE]   a connection's Status (the primary one)
 *   xd-netconn --wireless [IFACE] View Available Wireless Networks
 *   xd-netconn --new              the New Connection Wizard
 */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "app.h"

static constexpr char TITLE[] = "Network Connections";

enum { COL_ICON, COL_NAME, COL_TYPE, COL_STATUS, COL_DEVICE, COL_KEY, COL_SENSITIVE, N_COLS };

static const char css[] =
    ".xd-nc-pane { background-image: linear-gradient(to bottom, #7ba2e7, #6375d6); }\n"
    ".xd-nc-box { background-color: #d6dff7; border-radius: 3px 3px 0 0; }\n"
    ".xd-nc-box-title { color: #215dc6; font-weight: bold; padding: 4px 8px;"
    " background-image: linear-gradient(to right, #ffffff, #c6d3f7);"
    " border-radius: 3px 3px 0 0; }\n"
    "button.xd-nc-link { background: none; border: none; box-shadow: none; padding: 1px 2px; }\n"
    "button.xd-nc-link label { color: #215dc6; }\n"
    "button.xd-nc-link:hover label { color: #428eff; text-decoration: underline; }\n"
    ".xd-nc-details label { color: #000000; }\n"
    ".xd-nc-group { font-weight: bold; color: #0e3c8c; padding: 6px 2px 3px 2px;"
    " background-image: linear-gradient(to right, #3a6ea5, rgba(58, 110, 165, 0));"
    " background-size: 100% 1px; background-position: left bottom;"
    " background-repeat: no-repeat; }\n"
    ".xd-nc-dim { color: #6d6d6d; }\n"
    ".xd-nc-view, .xd-nc-view flowbox { background-color: @theme_base_color; }\n"
    "flowboxchild.xd-nc-tile { padding: 3px; }\n"
    "flowboxchild.xd-nc-tile:selected label { color: @theme_selected_fg_color; }\n";

/* ---- helpers ----------------------------------------------------------------------- */

static void add_class(GtkWidget *widget, const char *class) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), class);
}

static GtkWidget *label(const char *text, const char *class) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (class != nullptr)
        add_class(widget, class);
    return widget;
}

static void clear_container(GtkWidget *container) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *child = kids; child != nullptr; child = child->next)
        gtk_widget_destroy(child->data);
    g_list_free(kids);
}

GtkWindow *nc_parent(nc_app *app) { return GTK_WINDOW(app->window); }

void nc_error(GtkWindow *parent, const char *what, const GError *error) {
    char *text;
    if (error != nullptr) {
        /* Without the D-Bus error name GIO puts in front. */
        GError *stripped = g_error_copy(error);
        g_dbus_error_strip_remote_error(stripped);
        text = g_strdup_printf("%s\n\n%s", what, stripped->message);
        g_error_free(stripped);
    } else {
        text = g_strdup(what);
    }
    xd_message_box(parent, TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

static void maybe_quit(nc_app *app) {
    if (app->window == nullptr && app->dialogs == 0 && app->busy == 0)
        gtk_main_quit();
}

static void on_tracked_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    nc_app *app = data;
    app->dialogs--;
    maybe_quit(app);
}

void nc_hold(nc_app *app) { app->busy++; }

void nc_release(nc_app *app) {
    app->busy--;
    maybe_quit(app);
}

void nc_track(nc_app *app, GtkWidget *dialog) {
    app->dialogs++;
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_tracked_destroy), app);
}

char *nc_own_program(const char *name) {
    /* Our own programs from the build tree when we run from it. */
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = self != nullptr ? g_path_get_dirname(self) : g_strdup(".");
    char *local = g_build_filename(dir, name, nullptr);
    g_free(dir);
    g_free(self);
    if (g_file_test(local, G_FILE_TEST_IS_EXECUTABLE))
        return local;
    g_free(local);
    char *found = g_find_program_in_path(name);
    return found != nullptr ? found : g_strdup(name);
}

bool nc_spawn(GtkWindow *parent, const char *const *argv) {
    char *prog = nc_own_program(argv[0]);
    GStrvBuilder *builder = g_strv_builder_new();
    g_strv_builder_add(builder, prog);
    for (size_t i = 1; argv[i] != nullptr; i++)
        g_strv_builder_add(builder, argv[i]);
    char **args = g_strv_builder_end(builder);
    g_strv_builder_unref(builder);
    GError *error = nullptr;
    bool ok = g_spawn_async(nullptr, args, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                            &error);
    if (!ok) {
        char *what = g_strdup_printf("%s could not be started.", argv[0]);
        nc_error(parent, what, error);
        g_free(what);
        g_error_free(error);
    }
    g_strfreev(args);
    g_free(prog);
    return ok;
}

bool nc_have_editor(void) {
    char *path = g_find_program_in_path("nm-connection-editor");
    g_free(path);
    return path != nullptr;
}

void nc_run_editor(GtkWindow *parent, const char *uuid) {
    const char *with[] = {"nm-connection-editor", "--edit", uuid, nullptr};
    const char *without[] = {"nm-connection-editor", nullptr};
    nc_spawn(parent, uuid != nullptr ? with : without);
}

/* ---- the items ------------------------------------------------------------------- */

static void item_free(gpointer item_data) {
    nc_item *it = item_data;
    g_free(it->key);
    g_free(it->name);
    g_free(it->status);
    g_free(it->device_name);
    g_clear_object(&it->device);
    g_clear_object(&it->conn);
    g_clear_object(&it->active);
    g_free(it);
}

nc_item *nc_find(nc_app *app, const char *key) {
    for (guint i = 0; key != nullptr && i < app->items->len; i++) {
        nc_item *it = g_ptr_array_index(app->items, i);
        if (strcmp(it->key, key) == 0)
            return it;
    }
    return nullptr;
}

/* The connection types of the adapters we show. Software devices
 * (bridges, VLANs, ...) only when they carry a connection of the user's. */
static const struct {
    NMDeviceType type;
    const char *setting;
    bool always;
} device_kinds[] = {
    {NM_DEVICE_TYPE_ETHERNET, NM_SETTING_WIRED_SETTING_NAME, true},
    {NM_DEVICE_TYPE_WIFI, NM_SETTING_WIRELESS_SETTING_NAME, true},
    {NM_DEVICE_TYPE_MODEM, NM_SETTING_GSM_SETTING_NAME, true},
    {NM_DEVICE_TYPE_BT, NM_SETTING_BLUETOOTH_SETTING_NAME, true},
    {NM_DEVICE_TYPE_ADSL, NM_SETTING_ADSL_SETTING_NAME, true},
    {NM_DEVICE_TYPE_INFINIBAND, NM_SETTING_INFINIBAND_SETTING_NAME, true},
    {NM_DEVICE_TYPE_BOND, NM_SETTING_BOND_SETTING_NAME, false},
    {NM_DEVICE_TYPE_BRIDGE, NM_SETTING_BRIDGE_SETTING_NAME, false},
    {NM_DEVICE_TYPE_TEAM, NM_SETTING_TEAM_SETTING_NAME, false},
    {NM_DEVICE_TYPE_VLAN, NM_SETTING_VLAN_SETTING_NAME, false},
    {NM_DEVICE_TYPE_MACVLAN, NM_SETTING_MACVLAN_SETTING_NAME, false},
    {NM_DEVICE_TYPE_WIREGUARD, NM_SETTING_WIREGUARD_SETTING_NAME, false},
};

/* Profiles NetworkManager made up for something it doesn't manage
 * (loopback, docker's bridges): not the user's connections. */
static bool generated(NMRemoteConnection *connection) {
    NMSettingsConnectionFlags flags = nm_remote_connection_get_flags(connection);
    return (flags &
            (NM_SETTINGS_CONNECTION_FLAG_NM_GENERATED | NM_SETTINGS_CONNECTION_FLAG_EXTERNAL)) != 0;
}

static guint64 last_used(NMRemoteConnection *connection) {
    NMSettingConnection *setting = nm_connection_get_setting_connection(NM_CONNECTION(connection));
    return setting != nullptr ? nm_setting_connection_get_timestamp(setting) : 0;
}

/* The connection an adapter stands for: the active one, else the one
 * NetworkManager would pick (the most recently used it can take). */
static NMRemoteConnection *device_connection(NMDevice *dev, NMActiveConnection *active) {
    if (active != nullptr) {
        NMRemoteConnection *connection = nm_active_connection_get_connection(active);
        if (connection != nullptr)
            return connection;
    }
    if (NM_IS_DEVICE_WIFI(dev))
        return nullptr; /* the saved networks are its preferred list */
    const GPtrArray *avail = nm_device_get_available_connections(dev);
    NMRemoteConnection *best = nullptr;
    for (guint i = 0; avail != nullptr && i < avail->len; i++) {
        NMRemoteConnection *connection = g_ptr_array_index(avail, i);
        if (!generated(connection) && (best == nullptr || last_used(connection) > last_used(best)))
            best = connection;
    }
    return best;
}

static const char *kind_setting(NMDevice *dev, bool *always) {
    NMDeviceType device_type = nm_device_get_device_type(dev);
    for (size_t i = 0; i < XD_ARRAY_LEN(device_kinds); i++) {
        if (device_kinds[i].type == device_type) {
            *always = device_kinds[i].always;
            return device_kinds[i].setting;
        }
    }
    return nullptr;
}

static const char *icon_for(xd_nc_group connection_group, bool connected, const nc_item *it) {
    switch (connection_group) {
    case XD_NC_WIRELESS:
        return connected || it->disabled ? "network-wireless" : "network-wireless-disconnected";
    case XD_NC_VPN:
        return "network-vpn";
    case XD_NC_BROADBAND:
        return "network-modem";
    case XD_NC_PAN:
        return "preferences-system-bluetooth";
    case XD_NC_WIZARD:
        return "tools-wizard";
    case XD_NC_LAN:
    case XD_NC_OTHER:
    case XD_NC_GROUP_COUNT:
        break;
    }
    return !connected && it->state.has_device && it->state.carrier_known && !it->state.carrier &&
                   !it->disabled
               ? "network-wired-disconnected"
               : "network-wired";
}

GIcon *nc_icon(const char *name) {
    /* Not every icon theme has them all. */
    static const char *const fallbacks[][3] = {
        {"tools-wizard", "network-workgroup", "network-wired"},
        {"network-modem", "modem", "network-wired"},
        {"preferences-system-bluetooth", "bluetooth", "network-wired"},
        {"network-wired-disconnected", "network-offline", "network-wired"},
        {"network-wireless-disconnected", "network-wireless-offline", "network-wireless"},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(fallbacks); i++) {
        if (strcmp(name, fallbacks[i][0]) == 0)
            return g_themed_icon_new_from_names((char **)(uintptr_t)fallbacks[i], 3);
    }
    return g_themed_icon_new(name);
}

static void finish_item(nc_item *it) {
    it->status = g_strdup(xd_nc_status_text(&it->state));
    it->connected = xd_nc_state_connected(&it->state);
    it->disabled = xd_nc_state_disabled(&it->state);
    /* Wi-Fi says which network, as XP's tooltip did. */
    if (it->connected && it->group == XD_NC_WIRELESS && it->conn != nullptr) {
        g_free(it->status);
        it->status = g_strdup_printf("Connected to %s", nm_connection_get_id(NM_CONNECTION(it->conn)));
    }
    it->icon = icon_for(it->group, it->connected, it);
}

static nc_item *device_item(nc_app *app, NMDevice *dev, const char *setting, const char *desc,
                            unsigned number) {
    nc_item *it = g_new0(nc_item, 1);
    it->key = g_strdup_printf("dev:%s", nm_object_get_path(NM_OBJECT(dev)));
    it->group = xd_nc_group_for_type(setting);
    it->device = g_object_ref(dev);
    NMActiveConnection *active = nm_device_get_active_connection(dev);
    it->active = active != nullptr ? g_object_ref(active) : nullptr;
    NMRemoteConnection *connection = device_connection(dev, active);
    it->conn = connection != nullptr ? g_object_ref(connection) : nullptr;
    if (it->group == XD_NC_WIRELESS)
        it->name = xd_nc_numbered_name("Wireless Network Connection", number);
    else if (it->conn != nullptr)
        it->name = g_strdup(nm_connection_get_id(NM_CONNECTION(it->conn)));
    else
        it->name = xd_nc_numbered_name(it->group == XD_NC_LAN ? "Local Area Connection"
                                                               : nm_device_get_iface(dev),
                                       number);
    it->device_name = g_strdup(desc != nullptr ? desc : nm_device_get_iface(dev));
    NMDeviceState state = nm_device_get_state(dev);
    NMConnectivityState connectivity = nm_device_get_connectivity(dev, AF_INET);
    it->state = (xd_nc_state){
        .has_device = true,
        .device_state = (unsigned)state,
        .managed = nm_device_get_managed(dev),
        .autoconnect = nm_device_get_autoconnect(dev),
        .radio_off = NM_IS_DEVICE_WIFI(dev) && !nm_client_wireless_get_enabled(app->client),
        .connectivity = (unsigned)connectivity,
    };
    if (NM_IS_DEVICE_ETHERNET(dev)) {
        it->state.carrier_known = true;
        it->state.carrier = nm_device_ethernet_get_carrier(NM_DEVICE_ETHERNET(dev));
    }
    finish_item(it);
    return it;
}

static NMActiveConnection *find_active(nc_app *app, NMRemoteConnection *connection) {
    const GPtrArray *all = nm_client_get_active_connections(app->client);
    for (guint i = 0; all != nullptr && i < all->len; i++) {
        NMActiveConnection *active_connection = g_ptr_array_index(all, i);
        if (nm_active_connection_get_connection(active_connection) == connection)
            return active_connection;
    }
    return nullptr;
}

static char *vpn_device_name(NMConnection *connection) {
    NMSettingVpn *vpn = nm_connection_get_setting_vpn(connection);
    const char *service = vpn != nullptr ? nm_setting_vpn_get_service_type(vpn) : nullptr;
    if (service == nullptr)
        return g_strdup(nm_connection_get_interface_name(connection) != nullptr
                            ? nm_connection_get_interface_name(connection)
                            : "");
    /* As XP's "WAN Miniport (PPTP)". */
    const char *dot = strrchr(service, '.');
    char *plugin = g_ascii_strup(dot != nullptr ? dot + 1 : service, -1);
    if (strcmp(plugin, "OPENVPN") == 0) {
        g_free(plugin);
        plugin = g_strdup("OpenVPN");
    }
    char *name = g_strdup_printf("WAN Miniport (%s)", plugin);
    g_free(plugin);
    return name;
}

static nc_item *connection_item(nc_app *app, NMRemoteConnection *connection, xd_nc_group group) {
    NMConnection *nc = NM_CONNECTION(connection);
    nc_item *it = g_new0(nc_item, 1);
    it->key = g_strdup_printf("con:%s", nm_connection_get_uuid(nc));
    it->group = group;
    it->conn = g_object_ref(connection);
    NMActiveConnection *active = find_active(app, connection);
    it->active = active != nullptr ? g_object_ref(active) : nullptr;
    it->name = g_strdup(nm_connection_get_id(nc));
    if (group == XD_NC_VPN || group == XD_NC_BROADBAND)
        it->device_name = vpn_device_name(nc);
    else
        it->device_name = g_strdup(nm_connection_get_interface_name(nc) != nullptr
                                       ? nm_connection_get_interface_name(nc)
                                       : "");
    NMActiveConnectionState state =
        active != nullptr ? nm_active_connection_get_state(active) : NM_ACTIVE_CONNECTION_STATE_UNKNOWN;
    it->state = (xd_nc_state){.active_state = (unsigned)state};
    finish_item(it);
    return it;
}

static int compare_items(gconstpointer left, gconstpointer right) {
    const nc_item *left_item = *(nc_item *const *)left, *right_item = *(nc_item *const *)right;
    if (left_item->group != right_item->group)
        return (int)left_item->group - (int)right_item->group;
    return g_utf8_collate(left_item->name, right_item->name);
}

static void build_items(nc_app *app) {
    g_ptr_array_set_size(app->items, 0);
    if (app->client == nullptr || !nm_client_get_nm_running(app->client))
        return;
    GHashTable *used = g_hash_table_new(g_direct_hash, g_direct_equal);
    const GPtrArray *devs = nm_client_get_devices(app->client);
    /* Names for adapters as the hardware database has them, told apart. */
    GPtrArray *shown = g_ptr_array_new();
    for (guint i = 0; devs != nullptr && i < devs->len; i++) {
        NMDevice *dev = g_ptr_array_index(devs, i);
        bool always = false;
        if (kind_setting(dev, &always) == nullptr)
            continue;
        if (!always) {
            NMActiveConnection *active_connection = nm_device_get_active_connection(dev);
            NMRemoteConnection *connection = device_connection(dev, active_connection);
            if (connection == nullptr || generated(connection))
                continue;
        }
        g_ptr_array_add(shown, dev);
    }
    char **names = shown->len > 0 ? nm_device_disambiguate_names((NMDevice **)shown->pdata,
                                                                 (int)shown->len)
                                  : nullptr;
    unsigned counts[XD_NC_GROUP_COUNT] = {0};
    for (guint i = 0; i < shown->len; i++) {
        NMDevice *dev = g_ptr_array_index(shown, i);
        bool always;
        const char *setting = kind_setting(dev, &always);
        xd_nc_group connection_group = xd_nc_group_for_type(setting);
        nc_item *it = device_item(app, dev, setting, names != nullptr ? names[i] : nullptr,
                                  ++counts[connection_group]);
        if (it->conn != nullptr)
            g_hash_table_add(used, it->conn);
        g_ptr_array_add(app->items, it);
    }
    g_strfreev(names);
    g_ptr_array_free(shown, TRUE);

    /* Connections of their own: VPN, broadband, Bluetooth, and wired
     * profiles besides the ones the adapters stand for. Saved Wi-Fi
     * networks are the wireless adapter's preferred networks instead. */
    const GPtrArray *conns = nm_client_get_connections(app->client);
    for (guint i = 0; conns != nullptr && i < conns->len; i++) {
        NMRemoteConnection *connection = g_ptr_array_index(conns, i);
        NMConnection *nc = NM_CONNECTION(connection);
        NMSettingConnection *connection_setting = nm_connection_get_setting_connection(nc);
        if (g_hash_table_contains(used, connection) || generated(connection) ||
            connection_setting == nullptr ||
            nm_setting_connection_get_controller(connection_setting) != nullptr)
            continue;
        xd_nc_group connection_group = xd_nc_group_for_type(nm_connection_get_connection_type(nc));
        if (connection_group == XD_NC_WIRELESS || connection_group == XD_NC_OTHER)
            continue;
        g_ptr_array_add(app->items, connection_item(app, connection, connection_group));
    }
    g_hash_table_destroy(used);

    nc_item *wizard = g_new0(nc_item, 1);
    wizard->key = g_strdup("wizard");
    wizard->group = XD_NC_WIZARD;
    wizard->name = g_strdup("New Connection Wizard");
    wizard->status = g_strdup("");
    wizard->device_name = g_strdup("");
    wizard->icon = "tools-wizard";
    g_ptr_array_add(app->items, wizard);
    g_ptr_array_sort(app->items, compare_items);
}

/* ---- actions ------------------------------------------------------------------------ */

typedef struct pending {
    nc_app *app;
    char *what; /* the error message's first line */
} pending;

static pending *pending_new(nc_app *app, const char *what) {
    pending *pending_action = g_new0(pending, 1);
    pending_action->app = app;
    pending_action->what = g_strdup(what);
    nc_hold(app);
    return pending_action;
}

static void pending_fail(pending *pending_action, GError *error) {
    if (error != nullptr && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        nc_error(nc_parent(pending_action->app), pending_action->what, error);
    g_clear_error(&error);
    nc_release(pending_action->app);
    g_free(pending_action->what);
    g_free(pending_action);
}

static void activated(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    NMActiveConnection *active_connection =
        nm_client_activate_connection_finish(NM_CLIENT(src), res, &error);
    g_clear_object(&active_connection);
    pending_fail(data, error);
}

static void deactivated(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    nm_client_deactivate_connection_finish(NM_CLIENT(src), res, &error);
    pending_fail(data, error);
}

static void disconnected(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    nm_device_disconnect_finish(NM_DEVICE(src), res, &error);
    pending_fail(data, error);
}

static void property_set(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    nm_client_dbus_set_property_finish(NM_CLIENT(src), res, &error);
    pending_fail(data, error);
}

static void set_property(nc_app *app, const char *path, const char *iface, const char *name,
                         bool value, const char *what) {
    nm_client_dbus_set_property(app->client, path, iface, name, g_variant_new_boolean(value), -1,
                                nullptr, property_set, pending_new(app, what));
}

void nc_enable(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || it->device == nullptr)
        return;
    const char *path = nm_object_get_path(NM_OBJECT(it->device));
    char *what = g_strdup_printf("Could not enable %s.", it->name);
    if (!it->state.managed)
        set_property(app, path, NM_DBUS_INTERFACE_DEVICE, "Managed", true, what);
    if (it->state.radio_off)
        set_property(app, NM_DBUS_PATH, NM_DBUS_INTERFACE, "WirelessEnabled", true, what);
    /* Let NetworkManager connect it by itself again; a Wi-Fi adapter then
     * joins its preferred networks as they come in range. */
    set_property(app, path, NM_DBUS_INTERFACE_DEVICE, "Autoconnect", true, what);
    if (!NM_IS_DEVICE_WIFI(it->device) && it->conn != nullptr)
        nm_client_activate_connection_async(app->client, NM_CONNECTION(it->conn), it->device,
                                            nullptr, nullptr, activated, pending_new(app, what));
    g_free(what);
}

void nc_disable(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || it->device == nullptr)
        return;
    char *what = g_strdup_printf("Could not disable %s.", it->name);
    nm_device_disconnect_async(it->device, nullptr, disconnected, pending_new(app, what));
    g_free(what);
}

void nc_connect(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || it->conn == nullptr)
        return;
    char *what = g_strdup_printf("Could not connect %s.", it->name);
    nm_client_activate_connection_async(app->client, NM_CONNECTION(it->conn), it->device, nullptr,
                                        nullptr, activated, pending_new(app, what));
    g_free(what);
}

void nc_disconnect(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr)
        return;
    if (it->device != nullptr) {
        nc_disable(app, key);
        return;
    }
    if (it->active == nullptr)
        return;
    char *what = g_strdup_printf("Could not disconnect %s.", it->name);
    nm_client_deactivate_connection_async(app->client, it->active, nullptr, deactivated,
                                          pending_new(app, what));
    g_free(what);
}

typedef struct repair {
    nc_app *app;
    char *name;
} repair;

static void repaired(GObject *src, GAsyncResult *res, gpointer data) {
    repair *repair_job = data;
    GError *error = nullptr;
    NMActiveConnection *active_connection =
        nm_client_activate_connection_finish(NM_CLIENT(src), res, &error);
    g_clear_object(&active_connection);
    char *what = g_strdup_printf("The problem could not be repaired because the "
                                 "following action cannot be completed:\n\nRenewing your IP "
                                 "address for %s.",
                                 repair_job->name);
    if (error != nullptr) {
        nc_error(nc_parent(repair_job->app), what, error);
        g_error_free(error);
    } else {
        char *done = g_strdup_printf("The problem has been repaired.\n\n%s is connecting again.",
                                     repair_job->name);
        xd_message_box(nc_parent(repair_job->app), "Repair", XD_MB_INFO, XD_MB_OK, FALSE, done);
        g_free(done);
    }
    g_free(what);
    nc_release(repair_job->app);
    g_free(repair_job->name);
    g_free(repair_job);
}

/* XP renewed the DHCP lease and flushed caches; NetworkManager does
 * that and more when the connection is activated again. */
void nc_repair(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || it->conn == nullptr || !it->connected)
        return;
    repair *repair_job = g_new0(repair, 1);
    repair_job->app = app;
    nc_hold(app);
    repair_job->name = g_strdup(it->name);
    nm_client_activate_connection_async(app->client, NM_CONNECTION(it->conn), it->device, nullptr,
                                        nullptr, repaired, repair_job);
}

static void deleted(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(src), res, &error);
    pending_fail(data, error);
}

static void do_delete(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || it->conn == nullptr || it->device != nullptr)
        return;
    char *question_text =
        g_strdup_printf("Are you sure you want to delete the '%s' connection?", it->name);
    int response = xd_message_box(nc_parent(app), "Confirm Connection Delete", XD_MB_QUESTION,
                                  XD_MB_YES_NO, TRUE, question_text);
    g_free(question_text);
    if (response != GTK_RESPONSE_YES)
        return;
    it = nc_find(app, key); /* the box ran the main loop */
    if (it == nullptr || it->conn == nullptr)
        return;
    char *what = g_strdup_printf("Could not delete %s.", it->name);
    nm_remote_connection_delete_async(it->conn, nullptr, deleted, pending_new(app, what));
    g_free(what);
}

static void committed(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *update_result =
        nm_remote_connection_update2_finish(NM_REMOTE_CONNECTION(src), res, &error);
    if (update_result != nullptr)
        g_variant_unref(update_result);
    pending_fail(data, error);
}

void nc_commit(nc_app *app, NMRemoteConnection *remote, NMConnection *changed, const char *what) {
    /* Without secrets: NetworkManager keeps the ones it has. */
    nm_remote_connection_update2(remote, nm_connection_to_dbus(changed, NM_CONNECTION_SERIALIZE_ALL),
                                 NM_SETTINGS_UPDATE2_FLAG_TO_DISK, nullptr, nullptr, committed,
                                 pending_new(app, what));
}

static void added(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    NMRemoteConnection *connection = nm_client_add_connection_finish(NM_CLIENT(src), res, &error);
    g_clear_object(&connection);
    pending_fail(data, error);
}

void nc_add(nc_app *app, NMConnection *connection, const char *what) {
    nm_client_add_connection_async(app->client, connection, TRUE, nullptr, added,
                                   pending_new(app, what));
}

static bool can_rename(const nc_item *it) {
    return it->conn != nullptr && it->group != XD_NC_WIRELESS && it->group != XD_NC_WIZARD;
}

static void on_rename_changed(GtkEditable *editable, gpointer data) {
    char *text = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(editable))));
    gtk_widget_set_sensitive(data, text[0] != '\0');
    g_free(text);
}

static void do_rename(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || !can_rename(it))
        return;
    GtkWidget *rename_dialog = gtk_dialog_new_with_buttons(
        "Rename", nc_parent(app),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), nullptr, nullptr);
    GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(rename_dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(rename_dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(rename_dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(rename_dialog), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), it->name);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
    g_signal_connect(entry, "changed", G_CALLBACK(on_rename_changed), ok);
    GtkWidget *label_widget = gtk_label_new_with_mnemonic("_Connection name:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(label_widget), entry);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    gtk_box_pack_start(GTK_BOX(box), label_widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(rename_dialog))), box, TRUE,
                       TRUE, 0);
    gtk_widget_show_all(rename_dialog);
    /* Closing the window destroys the box too: keep it until we're done. */
    g_object_ref(rename_dialog);
    char *name = gtk_dialog_run(GTK_DIALOG(rename_dialog)) == GTK_RESPONSE_OK
                     ? g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))))
                     : nullptr;
    gtk_widget_destroy(rename_dialog);
    g_object_unref(rename_dialog);
    it = nc_find(app, key);
    if (it != nullptr && it->conn != nullptr && name != nullptr && name[0] != '\0' &&
        strcmp(name, it->name) != 0) {
        NMConnection *renamed = nm_simple_connection_new_clone(NM_CONNECTION(it->conn));
        g_object_set(nm_connection_get_setting_connection(renamed), NM_SETTING_CONNECTION_ID, name,
                     nullptr);
        char *what = g_strdup_printf("Could not rename %s.", it->name);
        nc_commit(app, it->conn, renamed, what);
        g_free(what);
        g_object_unref(renamed);
    }
    g_free(name);
}

/* Double-click or Enter: what XP did for the kind of connection. */
static void default_action(nc_app *app, nc_item *it) {
    if (it->group == XD_NC_WIZARD)
        nc_wizard_show(app);
    else if (it->disabled)
        nc_enable(app, it->key);
    else if (it->connected)
        nc_status_show(app, it->key);
    else if (it->device == nullptr)
        nc_connect(app, it->key);
    else if (NM_IS_DEVICE_WIFI(it->device))
        nc_wireless_show(app, it->device);
    else
        nc_properties_show(app, it->key);
}

/* ---- menus -------------------------------------------------------------------------- */

typedef void (*key_action)(nc_app *app, const char *key);

/* What a menu item or task does, kept on the widget. */
typedef struct action_ref {
    key_action fn;
    char *key;
} action_ref;

static void action_ref_free(gpointer action_data) {
    action_ref *bound_action = action_data;
    g_free(bound_action->key);
    g_free(bound_action);
}

static void set_action(GtkWidget *widget, key_action fn, const char *key) {
    action_ref *bound_action = g_new0(action_ref, 1);
    bound_action->fn = fn;
    bound_action->key = g_strdup(key);
    g_object_set_data_full(G_OBJECT(widget), "action", bound_action, action_ref_free);
}

static void run_action(GtkWidget *widget, nc_app *app) {
    action_ref *bound_action = g_object_get_data(G_OBJECT(widget), "action");
    if (bound_action != nullptr && bound_action->fn != nullptr)
        bound_action->fn(app, bound_action->key);
}

static void on_menu_action(GtkMenuItem *mi, gpointer data) { run_action(GTK_WIDGET(mi), data); }

static GtkWidget *menu_add(nc_app *app, GtkWidget *menu, const char *text, key_action fn,
                           const char *key, bool sensitive, bool bold) {
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(text);
    if (bold) {
        GtkWidget *label_widget = gtk_bin_get_child(GTK_BIN(mi));
        char *bold_markup = g_markup_printf_escaped("<b>%s</b>", text);
        gtk_label_set_markup_with_mnemonic(GTK_LABEL(label_widget), bold_markup);
        g_free(bold_markup);
    }
    set_action(mi, fn, key);
    g_signal_connect(mi, "activate", G_CALLBACK(on_menu_action), app);
    gtk_widget_set_sensitive(mi, sensitive);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    return mi;
}

static void menu_separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static void act_wizard(nc_app *app, [[maybe_unused]] const char *key) { nc_wizard_show(app); }

static void act_wireless(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it != nullptr && it->device != nullptr)
        nc_wireless_show(app, it->device);
}

static void act_status(nc_app *app, const char *key) { nc_status_show(app, key); }
static void act_properties(nc_app *app, const char *key) { nc_properties_show(app, key); }

/* The selected connection's commands (its context menu and File). */
static void fill_actions(nc_app *app, GtkWidget *menu, nc_item *it) {
    if (it == nullptr)
        return;
    const char *key = it->key;
    if (it->group == XD_NC_WIZARD) {
        menu_add(app, menu, "_Open", act_wizard, key, true, true);
        return;
    }
    bool wifi = it->device != nullptr && NM_IS_DEVICE_WIFI(it->device);
    bool def_status = it->connected && !it->disabled;
    if (it->device != nullptr) {
        if (it->disabled)
            menu_add(app, menu, "En_able", nc_enable, key, true, true);
        else
            menu_add(app, menu, "Disa_ble", nc_disable, key, true, false);
        if (wifi)
            menu_add(app, menu, "View Available _Wireless Networks", act_wireless, key,
                     !it->disabled, !it->connected && !it->disabled);
    } else if (it->connected || it->state.active_state == NM_ACTIVE_CONNECTION_STATE_ACTIVATING) {
        menu_add(app, menu, "Dis_connect", nc_disconnect, key, true, false);
    } else {
        menu_add(app, menu, "C_onnect", nc_connect, key, true, true);
    }
    menu_add(app, menu, "Stat_us", act_status, key, it->connected, def_status);
    menu_add(app, menu, "Re_pair", nc_repair, key, it->connected && it->conn != nullptr, false);
    menu_separator(menu);
    menu_add(app, menu, "_Delete", do_delete, key, it->conn != nullptr && it->device == nullptr,
             false);
    menu_add(app, menu, "Rena_me", do_rename, key, can_rename(it), false);
    menu_add(app, menu, "P_roperties", act_properties, key, true,
             it->device != nullptr && !it->connected && !it->disabled && !wifi);
}

static gboolean destroy_now(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

/* The item's "activate" comes after the menu's "deactivate". */
static void destroy_later(GtkWidget *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_now, menu);
}

static void popup_menu(GtkWidget *menu, GdkEvent *event) {
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_later), nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
}

static void build_file_menu(nc_app *app);
static void rebuild_now(nc_app *app);
static void show_view(nc_app *app, bool details);

static void on_view_toggled(GtkCheckMenuItem *mi, gpointer data) {
    nc_app *app = data;
    if (gtk_check_menu_item_get_active(mi))
        show_view(app, GTK_WIDGET(mi) == app->details_item);
}

static void on_refresh([[maybe_unused]] GtkWidget *widget, gpointer data) { rebuild_now(data); }

static void background_menu(nc_app *app, GdkEvent *event) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *view = gtk_menu_item_new_with_mnemonic("_View");
    GtkWidget *sub = gtk_menu_new();
    GtkWidget *tiles = gtk_radio_menu_item_new_with_mnemonic(nullptr, "_Tiles");
    GtkWidget *details = gtk_radio_menu_item_new_with_mnemonic_from_widget(
        GTK_RADIO_MENU_ITEM(tiles), "_Details");
    bool is_details = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(app->details_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(is_details ? details : tiles), TRUE);
    /* Through the main View menu's items, so the two agree. */
    g_signal_connect_swapped(tiles, "activate", G_CALLBACK(gtk_menu_item_activate),
                             app->tiles_item);
    g_signal_connect_swapped(details, "activate", G_CALLBACK(gtk_menu_item_activate),
                             app->details_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), tiles);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), details);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), view);
    GtkWidget *refresh = gtk_menu_item_new_with_mnemonic("R_efresh");
    g_signal_connect(refresh, "activate", G_CALLBACK(on_refresh), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), refresh);
    menu_separator(menu);
    menu_add(app, menu, "_New Connection...", act_wizard, nullptr,
             app->client != nullptr && nm_client_get_nm_running(app->client), false);
    popup_menu(menu, event);
}

/* ---- selection, the task pane ------------------------------------------------------ */

static void build_pane(nc_app *app);

static void select_key(nc_app *app, const char *key) {
    if (g_strcmp0(app->selected, key) == 0)
        return;
    g_free(app->selected);
    app->selected = g_strdup(key);
    if (app->pane == nullptr)
        return; /* the window is going */
    build_pane(app);
    build_file_menu(app);
}

static GtkWidget *pane_box(const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    add_class(box, "xd-nc-box");
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-nc-box-title"), FALSE, FALSE, 0);
    return box;
}

static void on_task(GtkButton *button, gpointer data) { run_action(GTK_WIDGET(button), data); }

static void pane_task(nc_app *app, GtkWidget *box, const char *icon, const char *text,
                      key_action fn, const char *key) {
    GtkWidget *task_button = gtk_button_new();
    add_class(task_button, "xd-nc-link");
    gtk_button_set_relief(GTK_BUTTON(task_button), GTK_RELIEF_NONE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU), FALSE,
                       FALSE, 0);
    GtkWidget *label_widget = label(text, nullptr);
    gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label_widget), 22);
    gtk_box_pack_start(GTK_BOX(row), label_widget, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(task_button), row);
    gtk_widget_set_halign(task_button, GTK_ALIGN_START);
    gtk_widget_set_margin_start(task_button, 6);
    set_action(task_button, fn, key);
    g_signal_connect(task_button, "clicked", G_CALLBACK(on_task), app);
    gtk_box_pack_start(GTK_BOX(box), task_button, FALSE, FALSE, 0);
}

static void act_control_panel(nc_app *app, [[maybe_unused]] const char *key) {
    const char *argv[] = {"xd-control", nullptr};
    nc_spawn(nc_parent(app), argv);
}

static void details_line(GtkWidget *box, const char *text, bool bold) {
    GtkWidget *label_widget = label("", nullptr);
    char *line_markup = g_markup_printf_escaped(bold ? "<b>%s</b>" : "%s", text);
    gtk_label_set_markup(GTK_LABEL(label_widget), line_markup);
    g_free(line_markup);
    gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label_widget), 24);
    gtk_box_pack_start(GTK_BOX(box), label_widget, FALSE, FALSE, 0);
}

static void build_pane(nc_app *app) {
    clear_container(app->pane);
    nc_item *it = nc_find(app, app->selected);
    bool running = app->client != nullptr && nm_client_get_nm_running(app->client);
    GtkWidget *tasks = pane_box("Network Tasks");
    if (running)
        pane_task(app, tasks, "list-add", "Create a new connection", act_wizard, nullptr);
    if (it != nullptr && it->group != XD_NC_WIZARD) {
        const char *key = it->key;
        bool wifi = it->device != nullptr && NM_IS_DEVICE_WIFI(it->device);
        if (wifi && !it->disabled)
            pane_task(app, tasks, "network-wireless", "View available wireless networks",
                      act_wireless, key);
        if (it->device != nullptr && it->disabled)
            pane_task(app, tasks, "media-playback-start", "Enable this network device", nc_enable,
                      key);
        else if (it->device != nullptr)
            pane_task(app, tasks, "process-stop", "Disable this network device", nc_disable, key);
        else if (it->connected)
            pane_task(app, tasks, "process-stop", "Disconnect this connection", nc_disconnect, key);
        else
            pane_task(app, tasks, "media-playback-start", "Start this connection", nc_connect, key);
        if (it->connected && it->conn != nullptr)
            pane_task(app, tasks, "view-refresh", "Repair this connection", nc_repair, key);
        if (can_rename(it))
            pane_task(app, tasks, "edit-rename", "Rename this connection", do_rename, key);
        if (it->connected)
            pane_task(app, tasks, "dialog-information", "View status of this connection",
                      act_status, key);
        if (it->conn != nullptr && it->device == nullptr)
            pane_task(app, tasks, "edit-delete", "Delete this connection", do_delete, key);
        pane_task(app, tasks, "document-properties", "Change settings of this connection",
                  act_properties, key);
    }
    gtk_box_pack_start(GTK_BOX(app->pane), tasks, FALSE, FALSE, 0);

    GtkWidget *places = pane_box("Other Places");
    pane_task(app, places, "preferences-system", "Control Panel", act_control_panel, nullptr);
    gtk_box_pack_start(GTK_BOX(app->pane), places, FALSE, FALSE, 0);

    GtkWidget *details = pane_box("Details");
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    add_class(inner, "xd-nc-details");
    gtk_container_set_border_width(GTK_CONTAINER(inner), 8);
    if (it == nullptr) {
        details_line(inner, TITLE, true);
        details_line(inner, "System Folder", false);
        if (!running)
            details_line(inner, "NetworkManager is not running.", false);
    } else if (it->group == XD_NC_WIZARD) {
        details_line(inner, it->name, true);
        details_line(inner, "Creates a new connection to the Internet, a network or a VPN.",
                     false);
    } else {
        details_line(inner, it->name, true);
        details_line(inner, xd_nc_group_title(it->group), false);
        details_line(inner, it->status, false);
        if (it->device_name[0] != '\0')
            details_line(inner, it->device_name, false);
        if (it->connected) {
            nc_ipv4_info info;
            nc_ipv4_info_get(it->device, it->active, &info);
            if (info.address[0] != '\0') {
                char *text = g_strdup_printf("IP Address: %s", info.address);
                details_line(inner, text, false);
                g_free(text);
                text = g_strdup_printf("Subnet Mask: %s", info.netmask);
                details_line(inner, text, false);
                g_free(text);
                details_line(inner, info.dhcp ? "Assigned by DHCP" : "Manually Configured",
                             false);
            }
            nc_ipv4_info_clear(&info);
        }
    }
    gtk_box_pack_start(GTK_BOX(details), inner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app->pane), details, FALSE, FALSE, 0);
    gtk_widget_show_all(app->pane);
}

static void build_file_menu(nc_app *app) {
    GtkWidget *menu = app->file_menu;
    clear_container(menu);
    nc_item *it = nc_find(app, app->selected);
    fill_actions(app, menu, it);
    if (it != nullptr)
        menu_separator(menu);
    menu_add(app, menu, "_New Connection...", act_wizard, nullptr,
             app->client != nullptr && nm_client_get_nm_running(app->client), false);
    menu_separator(menu);
    GtkWidget *close = gtk_menu_item_new_with_mnemonic("_Close");
    g_signal_connect_swapped(close, "activate", G_CALLBACK(gtk_widget_destroy), app->window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), close);
    gtk_widget_show_all(menu);
}

/* ---- the tiles -------------------------------------------------------------------- */

static void on_tile_selected(GtkFlowBox *box, gpointer data) {
    nc_app *app = data;
    if (app->selecting)
        return;
    GList *sel = gtk_flow_box_get_selected_children(box);
    if (sel == nullptr) {
        g_list_free(sel);
        return;
    }
    const char *key = g_object_get_data(G_OBJECT(sel->data), "key");
    g_list_free(sel);
    /* One selection across the groups. */
    app->selecting = true;
    GList *boxes = gtk_container_get_children(GTK_CONTAINER(app->groups));
    for (GList *child = boxes; child != nullptr; child = child->next) {
        if (GTK_IS_FLOW_BOX(child->data) && child->data != box)
            gtk_flow_box_unselect_all(child->data);
    }
    g_list_free(boxes);
    app->selecting = false;
    select_key(app, key);
}

static void on_tile_activated([[maybe_unused]] GtkFlowBox *box, GtkFlowBoxChild *child,
                              gpointer data) {
    nc_item *it = nc_find(data, g_object_get_data(G_OBJECT(child), "key"));
    if (it != nullptr)
        default_action(data, it);
}

static gboolean on_tile_button(GtkWidget *widget, GdkEventButton *button_event, gpointer data) {
    nc_app *app = data;
    if (button_event->type != GDK_BUTTON_PRESS || button_event->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkFlowBoxChild *child = gtk_flow_box_get_child_at_pos(
        GTK_FLOW_BOX(widget), (int)button_event->x, (int)button_event->y);
    if (child == nullptr) {
        background_menu(app, (GdkEvent *)button_event);
        return TRUE;
    }
    gtk_flow_box_select_child(GTK_FLOW_BOX(widget), child);
    gtk_widget_grab_focus(GTK_WIDGET(child));
    nc_item *it = nc_find(app, g_object_get_data(G_OBJECT(child), "key"));
    GtkWidget *menu = gtk_menu_new();
    fill_actions(app, menu, it);
    popup_menu(menu, (GdkEvent *)button_event);
    return TRUE;
}

static gboolean on_background_button([[maybe_unused]] GtkWidget *widget,
                                     GdkEventButton *button_event, gpointer data) {
    if (button_event->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (button_event->button == GDK_BUTTON_SECONDARY)
        background_menu(data, (GdkEvent *)button_event);
    return TRUE;
}

static GtkWidget *tile(const nc_item *it) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GIcon *gicon = nc_icon(it->icon);
    GtkWidget *img = gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_DIALOG);
    g_object_unref(gicon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
    /* XP greys a disabled connection's icon. */
    gtk_widget_set_sensitive(img, !it->disabled);
    gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    const char *lines[] = {it->name, it->status, it->device_name};
    for (size_t i = 0; i < XD_ARRAY_LEN(lines); i++) {
        if (lines[i][0] == '\0')
            continue;
        GtkWidget *label_widget = label(lines[i], i > 0 ? "xd-nc-dim" : nullptr);
        gtk_label_set_ellipsize(GTK_LABEL(label_widget), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label_widget), 28);
        gtk_box_pack_start(GTK_BOX(col), label_widget, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
    GtkWidget *child = gtk_flow_box_child_new();
    add_class(child, "xd-nc-tile");
    gtk_widget_set_size_request(child, 250, -1);
    gtk_container_add(GTK_CONTAINER(child), row);
    g_object_set_data_full(G_OBJECT(child), "key", g_strdup(it->key), g_free);
    char *tip = g_strdup_printf("%s\n%s", it->name, it->status);
    gtk_widget_set_tooltip_text(child, it->group == XD_NC_WIZARD ? it->name : tip);
    g_free(tip);
    return child;
}

static void build_tiles(nc_app *app) {
    clear_container(app->groups);
    GtkWidget *flow = nullptr;
    xd_nc_group group = XD_NC_GROUP_COUNT;
    GtkFlowBoxChild *selected = nullptr;
    if (app->items->len == 0) {
        GtkWidget *label_widget =
            label(app->client == nullptr || !nm_client_get_nm_running(app->client)
                      ? "NetworkManager is not running, so there are no network "
                        "connections to show."
                      : "There are no network connections.",
                  "xd-nc-dim");
        gtk_box_pack_start(GTK_BOX(app->groups), label_widget, FALSE, FALSE, 0);
    }
    for (guint i = 0; i < app->items->len; i++) {
        nc_item *it = g_ptr_array_index(app->items, i);
        if (it->group != group || flow == nullptr) {
            group = it->group;
            gtk_box_pack_start(GTK_BOX(app->groups), label(xd_nc_group_title(group), "xd-nc-group"),
                               FALSE, FALSE, 0);
            flow = gtk_flow_box_new();
            gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_SINGLE);
            gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(flow), FALSE);
            gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
            gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 8);
            gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 4);
            gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 4);
            gtk_widget_set_margin_bottom(flow, 6);
            g_signal_connect(flow, "selected-children-changed", G_CALLBACK(on_tile_selected), app);
            g_signal_connect(flow, "child-activated", G_CALLBACK(on_tile_activated), app);
            g_signal_connect(flow, "button-press-event", G_CALLBACK(on_tile_button), app);
            gtk_box_pack_start(GTK_BOX(app->groups), flow, FALSE, FALSE, 0);
        }
        GtkWidget *child = tile(it);
        gtk_container_add(GTK_CONTAINER(flow), child);
        if (g_strcmp0(app->selected, it->key) == 0)
            selected = GTK_FLOW_BOX_CHILD(child);
    }
    gtk_widget_show_all(app->groups);
    if (selected != nullptr) {
        GtkWidget *box = gtk_widget_get_parent(GTK_WIDGET(selected));
        gtk_flow_box_select_child(GTK_FLOW_BOX(box), selected);
    }
}

/* ---- the details list ------------------------------------------------------------ */

static void build_list(nc_app *app) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree));
    gtk_list_store_clear(app->store);
    for (guint i = 0; i < app->items->len; i++) {
        nc_item *it = g_ptr_array_index(app->items, i);
        GtkTreeIter iter;
        GIcon *icon = nc_icon(it->icon);
        gtk_list_store_insert_with_values(
            app->store, &iter, -1, COL_ICON, icon, COL_NAME, it->name, COL_TYPE,
            it->group == XD_NC_WIZARD ? "" : xd_nc_group_title(it->group), COL_STATUS, it->status,
            COL_DEVICE, it->device_name, COL_KEY, it->key, COL_SENSITIVE, !it->disabled, -1);
        g_object_unref(icon);
        if (g_strcmp0(app->selected, it->key) == 0)
            gtk_tree_selection_select_iter(sel, &iter);
    }
}

static char *row_key(nc_app *app, GtkTreePath *path) {
    GtkTreeIter iter;
    char *key = nullptr;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->store), &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter, COL_KEY, &key, -1);
    return key;
}

static void on_row_selected(GtkTreeSelection *sel, gpointer data) {
    nc_app *app = data;
    if (app->selecting)
        return;
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    char *key = nullptr;
    gtk_tree_model_get(model, &iter, COL_KEY, &key, -1);
    select_key(app, key);
    g_free(key);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tv, GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *col, gpointer data) {
    nc_app *app = data;
    char *key = row_key(app, path);
    nc_item *it = nc_find(app, key);
    g_free(key);
    if (it != nullptr)
        default_action(app, it);
}

static gboolean on_list_button(GtkWidget *widget, GdkEventButton *button_event, gpointer data) {
    nc_app *app = data;
    if (button_event->type != GDK_BUTTON_PRESS || button_event->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)button_event->x,
                                       (int)button_event->y, &path, nullptr, nullptr, nullptr)) {
        background_menu(app, (GdkEvent *)button_event);
        return TRUE;
    }
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), path);
    char *key = row_key(app, path);
    gtk_tree_path_free(path);
    GtkWidget *menu = gtk_menu_new();
    fill_actions(app, menu, nc_find(app, key));
    g_free(key);
    popup_menu(menu, (GdkEvent *)button_event);
    return TRUE;
}

static GtkWidget *make_list(nc_app *app) {
    app->store = gtk_list_store_new(N_COLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                    G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    app->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->store));
    g_object_unref(app->store);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "Name");
    gtk_tree_view_column_set_resizable(col, TRUE);
    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    gtk_tree_view_column_set_attributes(col, renderer, "gicon", COL_ICON, "sensitive",
                                        COL_SENSITIVE, nullptr);
    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_set_attributes(col, renderer, "text", COL_NAME, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree), col);
    static const struct {
        const char *title;
        int col;
    } cols[] = {{"Type", COL_TYPE}, {"Status", COL_STATUS}, {"Device Name", COL_DEVICE}};
    for (size_t i = 0; i < XD_ARRAY_LEN(cols); i++) {
        col = gtk_tree_view_column_new_with_attributes(cols[i].title,
                                                       gtk_cell_renderer_text_new(), "text",
                                                       cols[i].col, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(app->tree), col);
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_row_selected), app);
    g_signal_connect(app->tree, "row-activated", G_CALLBACK(on_row_activated), app);
    g_signal_connect(app->tree, "button-press-event", G_CALLBACK(on_list_button), app);
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(sw), app->tree);
    return sw;
}

static void show_view(nc_app *app, bool details) {
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), details ? "details" : "tiles");
}

/* ---- keeping up with NetworkManager ---------------------------------------------- */

static void rebuild_now(nc_app *app) {
    if (app->rebuild_idle != 0) {
        g_source_remove(app->rebuild_idle);
        app->rebuild_idle = 0;
    }
    /* Rename's box and others keep the key, not the item. */
    app->selecting = true;
    build_items(app);
    if (app->selected != nullptr && nc_find(app, app->selected) == nullptr)
        g_clear_pointer(&app->selected, g_free);
    if (app->groups != nullptr) {
        build_tiles(app);
        build_list(app);
        build_pane(app);
        build_file_menu(app);
    }
    app->selecting = false;
}

static gboolean rebuild_idle(gpointer data) {
    nc_app *app = data;
    app->rebuild_idle = 0;
    rebuild_now(app);
    return G_SOURCE_REMOVE;
}

static void changed(nc_app *app) {
    if (app->rebuild_idle == 0)
        app->rebuild_idle = g_timeout_add(100, rebuild_idle, app);
}

static void on_object_changed([[maybe_unused]] GObject *object, [[maybe_unused]] GParamSpec *ps,
                              gpointer data) {
    changed(data);
}

static void on_conn_changed([[maybe_unused]] NMRemoteConnection *connection, gpointer data) {
    changed(data);
}

static void on_client_object([[maybe_unused]] NMClient *client, GObject *object, gpointer data);

static void watch(nc_app *app, GObject *object) {
    g_signal_handlers_disconnect_by_data(object, app);
    g_signal_connect(object, "notify", G_CALLBACK(on_object_changed), app);
    if (NM_IS_REMOTE_CONNECTION(object))
        g_signal_connect(object, "changed", G_CALLBACK(on_conn_changed), app);
}

static void on_client_object([[maybe_unused]] NMClient *client, GObject *object, gpointer data) {
    nc_app *app = data;
    watch(app, object);
    changed(app);
}

static void watch_all(nc_app *app) {
    NMClient *client = app->client;
    g_signal_connect(client, "notify", G_CALLBACK(on_object_changed), app);
    static const char *const added[] = {"device-added",        "device-removed",
                                        "connection-added",    "connection-removed",
                                        "active-connection-added", "active-connection-removed"};
    for (size_t i = 0; i < XD_ARRAY_LEN(added); i++)
        g_signal_connect(client, added[i], G_CALLBACK(on_client_object), app);
    const GPtrArray *lists[] = {nm_client_get_devices(client), nm_client_get_connections(client),
                                nm_client_get_active_connections(client)};
    for (size_t list_index = 0; list_index < XD_ARRAY_LEN(lists); list_index++) {
        for (guint i = 0; lists[list_index] != nullptr && i < lists[list_index]->len; i++)
            watch(app, g_ptr_array_index(lists[list_index], i));
    }
}

/* ---- the window ------------------------------------------------------------------ */

static GtkWidget *menu_bar_menu(GtkWidget *bar, const char *title) {
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(title);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    return menu;
}

static void on_editor(GtkWidget *widget, [[maybe_unused]] gpointer data) {
    nc_run_editor(GTK_WINDOW(gtk_widget_get_toplevel(widget)), nullptr);
}

static void on_about([[maybe_unused]] GtkWidget *widget, gpointer data) {
    nc_app *app = data;
    const char *version = app->client != nullptr ? nm_client_get_version(app->client) : nullptr;
    char *text = g_strdup_printf("Network Connections\nXeonDesktop\n\nNetworkManager %s",
                                 version != nullptr ? version : "is not running");
    xd_message_box(nc_parent(app), "About Network Connections", XD_MB_INFO, XD_MB_OK, FALSE, text);
    g_free(text);
}

static GtkWidget *menu_bar(nc_app *app, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    app->file_menu = menu_bar_menu(bar, "_File");
    gtk_menu_set_accel_group(GTK_MENU(app->file_menu), accel);

    GtkWidget *view = menu_bar_menu(bar, "_View");
    app->tiles_item = gtk_radio_menu_item_new_with_mnemonic(nullptr, "T_iles");
    app->details_item = gtk_radio_menu_item_new_with_mnemonic_from_widget(
        GTK_RADIO_MENU_ITEM(app->tiles_item), "_Details");
    g_signal_connect(app->tiles_item, "toggled", G_CALLBACK(on_view_toggled), app);
    g_signal_connect(app->details_item, "toggled", G_CALLBACK(on_view_toggled), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), app->tiles_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), app->details_item);
    menu_separator(view);
    GtkWidget *refresh = gtk_menu_item_new_with_mnemonic("_Refresh");
    gtk_widget_add_accelerator(refresh, "activate", accel, GDK_KEY_F5, (GdkModifierType)0, GTK_ACCEL_VISIBLE);
    g_signal_connect(refresh, "activate", G_CALLBACK(on_refresh), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), refresh);

    GtkWidget *adv = menu_bar_menu(bar, "Adva_nced");
    GtkWidget *editor = gtk_menu_item_new_with_mnemonic("Advanced _Settings...");
    gtk_widget_set_sensitive(editor, nc_have_editor());
    g_signal_connect(editor, "activate", G_CALLBACK(on_editor), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(adv), editor);

    GtkWidget *help = menu_bar_menu(bar, "_Help");
    GtkWidget *about = gtk_menu_item_new_with_mnemonic("_About Network Connections");
    g_signal_connect(about, "activate", G_CALLBACK(on_about), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(help), about);
    return bar;
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *key_event, gpointer data) {
    nc_app *app = data;
    if (GTK_IS_ENTRY(gtk_window_get_focus(GTK_WINDOW(widget))) || app->selected == nullptr)
        return FALSE;
    /* Explorer's keys for the selected item. Rename and Delete ask first,
     * and the selection may change or go meanwhile: they get a copy. */
    if (key_event->keyval == GDK_KEY_F2 || key_event->keyval == GDK_KEY_Delete) {
        char *key = g_strdup(app->selected);
        if (key_event->keyval == GDK_KEY_F2)
            do_rename(app, key);
        else
            do_delete(app, key);
        g_free(key);
        return TRUE;
    }
    if ((key_event->keyval == GDK_KEY_Return || key_event->keyval == GDK_KEY_KP_Enter) &&
        (key_event->state & GDK_MOD1_MASK)) {
        nc_properties_show(app, app->selected);
        return TRUE;
    }
    return FALSE;
}

/* The window closed: the Properties, Status and wizard dialogs over it go
 * too, and the program ends once NetworkManager has answered what's
 * still pending (maybe_quit), as with a dialog alone. Nothing may reach
 * the window's widgets meanwhile: a change from NetworkManager only
 * rebuilds the items. */
static void on_window_destroy(GtkWidget *widget, gpointer data) {
    nc_app *app = data;
    app->window = nullptr;
    app->stack = app->groups = app->tree = app->pane = nullptr;
    app->file_menu = app->tiles_item = app->details_item = nullptr;
    app->store = nullptr;
    /* Referenced: destroying one may take others with it. */
    GList *all = gtk_window_list_toplevels();
    for (GList *link = all; link != nullptr; link = link->next)
        g_object_ref(link->data);
    for (GList *link = all; link != nullptr; link = link->next) {
        if (gtk_window_get_transient_for(link->data) == GTK_WINDOW(widget))
            gtk_widget_destroy(link->data);
    }
    g_list_free_full(all, g_object_unref);
    maybe_quit(app);
}

static void build_window(nc_app *app) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "preferences-system-network");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 780, 520);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app->window), accel);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(on_key), app);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), outer);
    gtk_box_pack_start(GTK_BOX(outer), menu_bar(app, accel), FALSE, FALSE, 0);
    g_object_unref(accel);
    gtk_box_pack_start(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);
    app->pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(app->pane), 12);
    GtkWidget *pane_bg = gtk_event_box_new();
    add_class(pane_bg, "xd-nc-pane");
    gtk_container_add(GTK_CONTAINER(pane_bg), app->pane);
    GtkWidget *pane_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pane_sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(pane_sw, 210, -1);
    gtk_container_add(GTK_CONTAINER(pane_sw), pane_bg);
    gtk_box_pack_start(GTK_BOX(body), pane_sw, FALSE, FALSE, 0);

    app->stack = gtk_stack_new();
    app->groups = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(app->groups), 10);
    GtkWidget *view_bg = gtk_event_box_new();
    add_class(view_bg, "xd-nc-view");
    g_signal_connect(view_bg, "button-press-event", G_CALLBACK(on_background_button), app);
    gtk_container_add(GTK_CONTAINER(view_bg), app->groups);
    GtkWidget *tiles_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tiles_sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(tiles_sw), view_bg);
    gtk_stack_add_named(GTK_STACK(app->stack), tiles_sw, "tiles");
    gtk_stack_add_named(GTK_STACK(app->stack), make_list(app), "details");
    gtk_box_pack_start(GTK_BOX(body), app->stack, TRUE, TRUE, 0);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);
}

/* The item for IFACE (an interface or connection name), or the primary
 * connection's, or the first connected one. */
static nc_item *find_by_name(nc_app *app, const char *name) {
    for (guint i = 0; i < app->items->len; i++) {
        nc_item *it = g_ptr_array_index(app->items, i);
        if (name == nullptr)
            continue;
        if (strcmp(it->name, name) == 0 ||
            (it->device != nullptr && g_strcmp0(nm_device_get_iface(it->device), name) == 0))
            return it;
    }
    if (name != nullptr)
        return nullptr;
    NMActiveConnection *primary =
        app->client != nullptr ? nm_client_get_primary_connection(app->client) : nullptr;
    for (guint i = 0; primary != nullptr && i < app->items->len; i++) {
        nc_item *it = g_ptr_array_index(app->items, i);
        if (it->active == primary)
            return it;
    }
    for (guint i = 0; i < app->items->len; i++) {
        nc_item *it = g_ptr_array_index(app->items, i);
        if (it->connected)
            return it;
    }
    return nullptr;
}

static NMDevice *first_wifi(nc_app *app, const char *iface) {
    for (guint i = 0; i < app->items->len; i++) {
        nc_item *it = g_ptr_array_index(app->items, i);
        if (it->device != nullptr && NM_IS_DEVICE_WIFI(it->device) &&
            (iface == nullptr || strcmp(nm_device_get_iface(it->device), iface) == 0))
            return it->device;
    }
    return nullptr;
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.NetworkConnections");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static nc_app app;
    app.items = g_ptr_array_new_with_free_func(item_free);
    GError *error = nullptr;
    app.client = nm_client_new(nullptr, &error);
    if (app.client == nullptr) {
        g_printerr("xd-netconn: %s\n", error->message);
        g_error_free(error);
    } else {
        watch_all(&app);
    }
    build_items(&app);

    const char *mode = nullptr, *arg = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--style") == 0 || strcmp(argv[i], "--scheme") == 0) {
            i++; /* the shell passes its overrides on; GTK follows desktop.ini */
        } else if (mode == nullptr && (strcmp(argv[i], "--status") == 0 ||
                                       strcmp(argv[i], "--wireless") == 0 ||
                                       strcmp(argv[i], "--new") == 0)) {
            mode = argv[i];
        } else if (mode != nullptr && arg == nullptr && argv[i][0] != '-' &&
                   strcmp(mode, "--new") != 0) {
            arg = argv[i];
        } else {
            g_printerr("usage: xd-netconn [--status [IFACE] | --wireless [IFACE] | --new]\n");
            return 2;
        }
    }
    if (mode == nullptr) {
        build_window(&app);
        rebuild_now(&app);
        gtk_widget_show_all(app.window);
    } else {
        /* A dialog alone, as the notification area's icon opens it. */
        app.window = nullptr;
        if (strcmp(mode, "--status") == 0) {
            nc_item *it = find_by_name(&app, arg);
            if (it == nullptr) {
                xd_message_box(nullptr, TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                               arg != nullptr ? "There is no network connection by that name."
                                              : "No network connection is connected.");
                return EXIT_FAILURE;
            }
            nc_status_show(&app, it->key);
        } else if (strcmp(mode, "--wireless") == 0) {
            NMDevice *dev = first_wifi(&app, arg);
            if (dev == nullptr) {
                xd_message_box(nullptr, TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                               "This computer has no wireless network adapter.");
                return EXIT_FAILURE;
            }
            nc_wireless_show(&app, dev);
        } else {
            nc_wizard_show(&app);
        }
        if (app.dialogs == 0 && app.busy == 0)
            return EXIT_SUCCESS;
    }
    gtk_main();
    if (app.rebuild_idle != 0)
        g_source_remove(app.rebuild_idle);
    g_ptr_array_free(app.items, TRUE);
    g_free(app.selected);
    if (app.up_since != nullptr)
        g_key_file_free(app.up_since);
    if (app.client != nullptr) {
        g_signal_handlers_disconnect_by_data(app.client, &app);
        g_object_unref(app.client);
    }
    return EXIT_SUCCESS;
}
