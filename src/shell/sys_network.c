/* SPDX-License-Identifier: MIT
 * The network icon in the notification area, from NetworkManager over
 * D-Bus (its ObjectManager keeps every device, access point and active
 * connection with their properties up to date for us).
 *
 * The icon shows a wired or wireless connection (with its signal), one
 * being made, or none. Its menu lists the Wi-Fi networks in range:
 * picking one activates its saved connection, or adds one, asking for the
 * key (xd-wifi-key) if the network is secured; then Disconnect, Wi-Fi on
 * or off, the connection's Status and Network Connections (xd-netconn).
 * A double click opens the Status, as in XP.
 */
#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_SETTINGS_PATH "/org/freedesktop/NetworkManager/Settings"
#define IFACE_WIRELESS "org.freedesktop.NetworkManager.Device.Wireless"
#define IFACE_AP "org.freedesktop.NetworkManager.AccessPoint"
#define IFACE_ACTIVE "org.freedesktop.NetworkManager.Connection.Active"

/* NMState, NMDeviceType, AP security flags. */
enum { NM_ASLEEP = 10, NM_DISCONNECTED = 20, NM_CONNECTING = 40, NM_CONNECTED_GLOBAL = 70 };
enum { DEVICE_WIFI = 2 };
enum { AP_PRIVACY = 0x1, KEY_MGMT_PSK = 0x100, KEY_MGMT_8021X = 0x200, KEY_MGMT_SAE = 0x400 };

typedef struct network {
    xd_shell *shell;
    GCancellable *cancel;
    GDBusObjectManager *objects;
    GDBusProxy *nm; /* the manager object itself */
    char icon[64];
    /* For drawing: the kind of connection, its signal and whether up. */
    bool wifi, connected;
    bool running; /* NetworkManager is on the bus */
    unsigned strength;
} network;

/* An access point picked in the menu (lives in the menu tree's arena). */
typedef struct ap_choice {
    network *net;
    const char *ssid;
    const char *ap_path;
    const char *device_path;
    unsigned wpa, rsn, flags;
} ap_choice;

/* ---- reading properties ----------------------------------------------------- */

static GDBusProxy *proxy_for(network *network_icon, const char *path, const char *iface) {
    if (network_icon->objects == nullptr || path == nullptr || strcmp(path, "/") == 0)
        return nullptr;
    GDBusInterface *dbus_interface =
        g_dbus_object_manager_get_interface(network_icon->objects, path, iface);
    return dbus_interface != nullptr ? G_DBUS_PROXY(dbus_interface) : nullptr;
}

static GVariant *prop(GDBusProxy *proxy, const char *name) {
    return proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
}

static unsigned prop_u(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name);
    unsigned value = 0;
    if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT32))
        value = g_variant_get_uint32(variant);
    else if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BYTE))
        value = g_variant_get_byte(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

static bool prop_b(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name);
    bool value = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BOOLEAN) &&
                 g_variant_get_boolean(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

static char *prop_s(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name);
    char *value = nullptr;
    if (variant != nullptr && (g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING) ||
                               g_variant_is_of_type(variant, G_VARIANT_TYPE_OBJECT_PATH)))
        value = g_variant_dup_string(variant, nullptr);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

/* An SSID (bytes) as text; nullptr for hidden networks. */
static char *ap_ssid(GDBusProxy *ap) {
    GVariant *variant = prop(ap, "Ssid");
    char *ssid = nullptr;
    if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BYTESTRING)) {
        gsize len = 0;
        const char *bytes = g_variant_get_fixed_array(variant, &len, 1);
        if (len > 0)
            ssid = g_utf8_make_valid(bytes, (gssize)len);
    }
    if (variant != nullptr)
        g_variant_unref(variant);
    return ssid;
}

static const char *signal_icon(unsigned strength) {
    return strength >= 80   ? "network-wireless-signal-excellent"
           : strength >= 55 ? "network-wireless-signal-good"
           : strength >= 30 ? "network-wireless-signal-ok"
           : strength >= 5  ? "network-wireless-signal-weak"
                            : "network-wireless-signal-none";
}

/* The primary connection: its active-connection proxy, type and, for
 * Wi-Fi, the access point. Caller unrefs. */
static GDBusProxy *primary(network *network_icon, char **type, GDBusProxy **ap) {
    *type = nullptr;
    *ap = nullptr;
    char *path = prop_s(network_icon->nm, "PrimaryConnection");
    GDBusProxy *active = proxy_for(network_icon, path, IFACE_ACTIVE);
    g_free(path);
    if (active == nullptr) {
        /* None yet (still connecting): the default active connection, or
         * the first one. */
        GVariant *all = prop(network_icon->nm, "ActiveConnections");
        if (all != nullptr && g_variant_is_of_type(all, G_VARIANT_TYPE("ao"))) {
            GVariantIter iter;
            const char *active_path;
            g_variant_iter_init(&iter, all);
            while (g_variant_iter_next(&iter, "&o", &active_path)) {
                GDBusProxy *proxy = proxy_for(network_icon, active_path, IFACE_ACTIVE);
                if (proxy == nullptr)
                    continue;
                if (active == nullptr || prop_b(proxy, "Default")) {
                    if (active != nullptr)
                        g_object_unref(active);
                    active = proxy;
                } else {
                    g_object_unref(proxy);
                }
            }
        }
        if (all != nullptr)
            g_variant_unref(all);
    }
    if (active == nullptr)
        return nullptr;
    *type = prop_s(active, "Type");
    char *specific = prop_s(active, "SpecificObject");
    *ap = proxy_for(network_icon, specific, IFACE_AP);
    g_free(specific);
    return active;
}

static void update(network *network_icon) {
    network was = *network_icon;
    char *owner = network_icon->objects != nullptr
                      ? g_dbus_object_manager_client_get_name_owner(
                            G_DBUS_OBJECT_MANAGER_CLIENT(network_icon->objects))
                      : nullptr;
    network_icon->running = owner != nullptr;
    g_free(owner);
    unsigned state = prop_u(network_icon->nm, "State");
    char *type = nullptr;
    GDBusProxy *ap = nullptr;
    GDBusProxy *active = primary(network_icon, &type, &ap);
    bool wifi = type != nullptr && strstr(type, "wireless") != nullptr;
    const char *icon;
    network_icon->wifi = wifi;
    network_icon->connected = state >= NM_CONNECTED_GLOBAL - 20 && active != nullptr;
    network_icon->strength = wifi ? prop_u(ap, "Strength") : 0;
    if (state >= NM_CONNECTED_GLOBAL - 20 && active != nullptr)
        icon = wifi ? signal_icon(prop_u(ap, "Strength")) : "network-wired";
    else if (state >= NM_CONNECTING && state < NM_CONNECTED_GLOBAL - 20)
        icon = wifi ? "network-wireless-acquiring" : "network-wired-acquiring";
    else
        icon = "network-offline";
    g_strlcpy(network_icon->icon, icon, sizeof(network_icon->icon));
    g_free(type);
    if (ap != nullptr)
        g_object_unref(ap);
    if (active != nullptr)
        g_object_unref(active);
    /* Any property of any of NetworkManager's objects lands here (access
     * points' signals on every scan): redraw only when the icon changed. */
    if (network_icon->running != was.running || network_icon->nm != was.nm ||
        network_icon->wifi != was.wifi || network_icon->connected != was.connected ||
        (network_icon->strength + 19) / 20 != (was.strength + 19) / 20 ||
        strcmp(network_icon->icon, was.icon) != 0)
        xd_tray_changed(network_icon->shell);
}

/* ---- the icon ------------------------------------------------------------------ */

static const char *icon(void *data) {
    network *network_icon = data;
    return network_icon->running && network_icon->nm != nullptr && network_icon->icon[0] != '\0'
               ? network_icon->icon
               : nullptr;
}

static char *tooltip(void *data) {
    network *network_icon = data;
    unsigned state = prop_u(network_icon->nm, "State");
    char *type = nullptr;
    GDBusProxy *ap = nullptr;
    GDBusProxy *active = primary(network_icon, &type, &ap);
    char *text;
    if (active != nullptr && state >= NM_CONNECTING) {
        char *id = prop_s(active, "Id");
        bool wifi = type != nullptr && strstr(type, "wireless") != nullptr;
        const char *what = state < NM_CONNECTED_GLOBAL - 20 ? "connecting"
                           : state < NM_CONNECTED_GLOBAL    ? "limited connectivity"
                                                            : "connected";
        if (wifi)
            text = g_strdup_printf("Wireless Network Connection (%s): %u%%, %s",
                                   id != nullptr ? id : "?", prop_u(ap, "Strength"), what);
        else
            text = g_strdup_printf("Local Area Connection (%s): %s", id != nullptr ? id : "?",
                                   what);
        g_free(id);
    } else {
        text = g_strdup(state == NM_ASLEEP ? "Networking is disabled" : "Not connected");
    }
    g_free(type);
    if (ap != nullptr)
        g_object_unref(ap);
    if (active != nullptr)
        g_object_unref(active);
    return text;
}

/* ---- actions -------------------------------------------------------------------- */

static void call_done(GObject *src, GAsyncResult *res, [[maybe_unused]] gpointer data) {
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant != nullptr) {
        g_variant_unref(variant);
    } else {
        fprintf(stderr, "xd-shell: network: %s\n", error->message);
        g_error_free(error);
    }
}

static void nm_call(network *network_icon, const char *path, const char *iface, const char *method,
                    GVariant *args) {
    GDBusConnection *bus = g_dbus_proxy_get_connection(network_icon->nm);
    g_dbus_connection_call(bus, NM, path, iface, method, args, nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                           network_icon->cancel, call_done, network_icon);
}

/* {"802-11-wireless-security": {...}} for a key, as NetworkManager wants
 * it for AddAndActivateConnection (it fills in the SSID from the AP). */
static GVariant *security_settings(const ap_choice *picked_network, const char *key) {
    GVariantBuilder conn, sec;
    g_variant_builder_init(&conn, G_VARIANT_TYPE("a{sa{sv}}"));
    if (key != nullptr) {
        g_variant_builder_init(&sec, G_VARIANT_TYPE("a{sv}"));
        bool sae_only =
            (picked_network->rsn & KEY_MGMT_SAE) && !(picked_network->rsn & KEY_MGMT_PSK);
        if (picked_network->wpa == 0 && picked_network->rsn == 0) {
            /* WEP */
            g_variant_builder_add(&sec, "{sv}", "key-mgmt", g_variant_new_string("none"));
            g_variant_builder_add(&sec, "{sv}", "wep-key0", g_variant_new_string(key));
            g_variant_builder_add(&sec, "{sv}", "wep-key-type", g_variant_new_uint32(2));
        } else {
            g_variant_builder_add(&sec, "{sv}", "key-mgmt",
                                  g_variant_new_string(sae_only ? "sae" : "wpa-psk"));
            g_variant_builder_add(&sec, "{sv}", "psk", g_variant_new_string(key));
        }
        g_variant_builder_add(&conn, "{s@a{sv}}", "802-11-wireless-security",
                              g_variant_builder_end(&sec));
    }
    return g_variant_builder_end(&conn);
}

static void add_and_activate(const ap_choice *picked_network, const char *key) {
    nm_call(picked_network->net, NM_PATH, NM, "AddAndActivateConnection",
            g_variant_new("(@a{sa{sv}}oo)", security_settings(picked_network, key),
                          picked_network->device_path, picked_network->ap_path));
}

typedef struct key_prompt {
    ap_choice choice; /* copied: the menu's arena is gone by then */
    char *ssid, *ap_path, *device_path;
} key_prompt;

static key_prompt *copy_choice(const ap_choice *picked_network) {
    key_prompt *key_request = g_new0(key_prompt, 1);
    key_request->choice = *picked_network;
    key_request->choice.ssid = key_request->ssid = g_strdup(picked_network->ssid);
    key_request->choice.ap_path = key_request->ap_path = g_strdup(picked_network->ap_path);
    key_request->choice.device_path = key_request->device_path =
        g_strdup(picked_network->device_path);
    return key_request;
}

static void key_prompt_free(key_prompt *key_request) {
    g_free(key_request->ssid);
    g_free(key_request->ap_path);
    g_free(key_request->device_path);
    g_free(key_request);
}

/* The helper's output, read until it closes stdout (the shell ignores
 * SIGCHLD, so its exit status isn't ours to wait for: no output means
 * Cancel). */
enum { KEY_MAX = 256 };

typedef struct key_reader {
    key_prompt *k;
    GString *out;
    int fd;
} key_reader;

static gboolean read_key(int fd, [[maybe_unused]] GIOCondition cond, gpointer data) {
    key_reader *reader = data;
    char buf[256];
    ssize_t bytes_read = read(fd, buf, sizeof(buf));
    /* No key is longer than 64 characters (WPA's in hex): anything much
     * longer isn't one, and isn't kept. */
    bool too_long = bytes_read > 0 && reader->out->len + (size_t)bytes_read > KEY_MAX;
    if (bytes_read > 0 && !too_long) {
        g_string_append_len(reader->out, buf, bytes_read);
        explicit_bzero(buf, sizeof(buf));
        return G_SOURCE_CONTINUE;
    }
    explicit_bzero(buf, sizeof(buf));
    if (bytes_read < 0 && errno == EINTR)
        return G_SOURCE_CONTINUE;
    g_strchomp(reader->out->str);
    if (!too_long && reader->out->str[0] != '\0')
        add_and_activate(&reader->k->choice, reader->out->str);
    explicit_bzero(reader->out->str, reader->out->allocated_len);
    g_string_free(reader->out, TRUE);
    close(reader->fd);
    key_prompt_free(reader->k);
    g_free(reader);
    return G_SOURCE_REMOVE;
}

/* Asks for the network key with xd-wifi-key, which prints it. Takes `key_request`. */
static void ask_key(key_prompt *key_request) {
    char *prog = xd_shell_own_program("xd-wifi-key");
    char *argv[] = {prog, key_request->ssid, nullptr};
    int out = -1;
    GError *error = nullptr;
    if (!g_spawn_async_with_pipes(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                                  nullptr, nullptr, &out, nullptr, &error)) {
        fprintf(stderr, "xd-shell: network: %s\n", error->message);
        g_error_free(error);
        g_free(prog);
        key_prompt_free(key_request);
        return;
    }
    g_free(prog);
    key_reader *reader = g_new0(key_reader, 1);
    reader->k = key_request;
    reader->out = g_string_new(nullptr);
    reader->fd = out;
    g_unix_fd_add(out, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_key, reader);
}

typedef struct find_saved {
    key_prompt *k; /* the choice, copied */
    GVariant *paths;
    gsize next;
} find_saved;

static void check_next_saved(find_saved *saved_search);

static void got_settings(GObject *src, GAsyncResult *res, gpointer data) {
    find_saved *saved_search = data;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    bool match = false;
    if (variant != nullptr) {
        GVariant *settings = g_variant_get_child_value(variant, 0);
        GVariant *wifi = g_variant_lookup_value(settings, "802-11-wireless", nullptr);
        GVariant *ssid = wifi != nullptr ? g_variant_lookup_value(wifi, "ssid", nullptr) : nullptr;
        if (ssid != nullptr && g_variant_is_of_type(ssid, G_VARIANT_TYPE_BYTESTRING)) {
            gsize len = 0;
            const char *bytes = g_variant_get_fixed_array(ssid, &len, 1);
            match = len == strlen(saved_search->k->ssid) &&
                    memcmp(bytes, saved_search->k->ssid, len) == 0;
        }
        if (ssid != nullptr)
            g_variant_unref(ssid);
        if (wifi != nullptr)
            g_variant_unref(wifi);
        g_variant_unref(settings);
        g_variant_unref(variant);
    }
    if (match) {
        /* A saved connection: NetworkManager has (or asks for) its key. */
        const char *path = nullptr;
        g_variant_get_child(saved_search->paths, saved_search->next - 1, "&o", &path);
        nm_call(
            saved_search->k->choice.net, NM_PATH, NM, "ActivateConnection",
            g_variant_new("(ooo)", path, saved_search->k->device_path, saved_search->k->ap_path));
        g_variant_unref(saved_search->paths);
        key_prompt_free(saved_search->k);
        g_free(saved_search);
        return;
    }
    check_next_saved(saved_search);
}

static void check_next_saved(find_saved *saved_search) {
    network *network_icon = saved_search->k->choice.net;
    if (saved_search->next >= g_variant_n_children(saved_search->paths)) {
        /* Not saved: open networks connect, secured ones need the key;
         * 802.1X (enterprise) networks need the full editor. */
        const ap_choice *picked_network = &saved_search->k->choice;
        g_variant_unref(saved_search->paths);
        if ((picked_network->wpa | picked_network->rsn) & KEY_MGMT_8021X) {
            xd_shell_launch(network_icon->shell, "nm-connection-editor");
            key_prompt_free(saved_search->k);
        } else if ((picked_network->flags & AP_PRIVACY) || picked_network->wpa != 0 ||
                   picked_network->rsn != 0) {
            ask_key(saved_search->k);
        } else {
            add_and_activate(picked_network, nullptr);
            key_prompt_free(saved_search->k);
        }
        g_free(saved_search);
        return;
    }
    const char *path = nullptr;
    g_variant_get_child(saved_search->paths, saved_search->next++, "&o", &path);
    g_dbus_connection_call(g_dbus_proxy_get_connection(network_icon->nm), NM, path,
                           "org.freedesktop.NetworkManager.Settings.Connection", "GetSettings",
                           nullptr, G_VARIANT_TYPE("(a{sa{sv}})"), G_DBUS_CALL_FLAGS_NONE, -1,
                           network_icon->cancel, got_settings, saved_search);
}

static void got_connections(GObject *src, GAsyncResult *res, gpointer data) {
    key_prompt *key_request = data;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    find_saved *saved_search = g_new0(find_saved, 1);
    saved_search->k = key_request;
    saved_search->paths =
        variant != nullptr
            ? g_variant_get_child_value(variant, 0)
            : g_variant_ref_sink(g_variant_new_array(G_VARIANT_TYPE_OBJECT_PATH, nullptr, 0));
    if (variant != nullptr)
        g_variant_unref(variant);
    check_next_saved(saved_search);
}

static void act_connect(xd_shell *shell, void *data) {
    (void)shell;
    const ap_choice *picked_network = data;
    key_prompt *key_request = copy_choice(picked_network);
    g_dbus_connection_call(g_dbus_proxy_get_connection(picked_network->net->nm), NM,
                           NM_SETTINGS_PATH, "org.freedesktop.NetworkManager.Settings",
                           "ListConnections", nullptr, G_VARIANT_TYPE("(ao)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, picked_network->net->cancel, got_connections,
                           key_request);
}

static void act_disconnect(xd_shell *shell, void *data) {
    (void)shell;
    network *network_icon = data;
    char *type = nullptr;
    GDBusProxy *ap = nullptr;
    GDBusProxy *active = primary(network_icon, &type, &ap);
    if (active != nullptr)
        nm_call(network_icon, NM_PATH, NM, "DeactivateConnection",
                g_variant_new("(o)", g_dbus_proxy_get_object_path(active)));
    g_free(type);
    if (ap != nullptr)
        g_object_unref(ap);
    if (active != nullptr)
        g_object_unref(active);
}

static void act_wifi_toggle(xd_shell *shell, void *data) {
    (void)shell;
    network *network_icon = data;
    bool on = prop_b(network_icon->nm, "WirelessEnabled");
    nm_call(network_icon, NM_PATH, "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", NM, "WirelessEnabled", g_variant_new_boolean(!on)));
}

static void act_status(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-netconn", "--status"));
}

static void act_connections(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-netconn", nullptr));
}

static bool have_wifi(network *network_icon) {
    bool any = false;
    GList *objs = network_icon->objects != nullptr
                      ? g_dbus_object_manager_get_objects(network_icon->objects)
                      : nullptr;
    for (GList *list = objs; list != nullptr && !any; list = list->next) {
        GDBusProxy *wifi =
            proxy_for(network_icon, g_dbus_object_get_object_path(list->data), IFACE_WIRELESS);
        any = wifi != nullptr;
        if (wifi != nullptr)
            g_object_unref(wifi);
    }
    g_list_free_full(objs, g_object_unref);
    return any;
}

/* A double click: XP's "Local Area Connection Status" (or Wireless
 * Network Connection's). Not connected, there's no Status to show: the
 * networks in range, or Network Connections. */
static void open_status(void *data) {
    network *network_icon = data;
    if (network_icon->nm == nullptr)
        return;
    if (network_icon->connected)
        act_status(network_icon->shell, network_icon);
    else if (have_wifi(network_icon))
        xd_shell_launch(network_icon->shell,
                        xd_shell_own_command(network_icon->shell, "xd-netconn", "--wireless"));
    else
        act_connections(network_icon->shell, network_icon);
}

/* ---- the menu ------------------------------------------------------------------- */

typedef struct listed {
    char *ssid;
    char *ap_path, *device_path;
    unsigned strength, flags, wpa, rsn;
    bool current;
} listed;

static int by_strength(gconstpointer left, gconstpointer right) {
    const listed *left_network = *(listed *const *)left, *right_network = *(listed *const *)right;
    return (int)right_network->strength - (int)left_network->strength;
}

static void listed_free(gpointer user_data) {
    listed *listed_network = user_data;
    g_free(listed_network->ssid);
    g_free(listed_network->ap_path);
    g_free(listed_network->device_path);
    g_free(listed_network);
}

/* The networks in range, strongest first, one entry per SSID. */
static GPtrArray *networks_in_range(network *network_icon, bool rescan, const char *current_ap) {
    GPtrArray *out = g_ptr_array_new_with_free_func(listed_free);
    GList *objs = g_dbus_object_manager_get_objects(network_icon->objects);
    for (GList *list = objs; list != nullptr; list = list->next) {
        const char *dev_path = g_dbus_object_get_object_path(list->data);
        GDBusProxy *wifi = proxy_for(network_icon, dev_path, IFACE_WIRELESS);
        if (wifi == nullptr)
            continue;
        if (rescan)
            nm_call(
                network_icon, dev_path, IFACE_WIRELESS, "RequestScan",
                g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)));
        char *active_ap = prop_s(wifi, "ActiveAccessPoint");
        GVariant *aps = prop(wifi, "AccessPoints");
        GVariantIter iter;
        const char *ap_path;
        if (aps != nullptr && g_variant_is_of_type(aps, G_VARIANT_TYPE("ao"))) {
            g_variant_iter_init(&iter, aps);
            while (g_variant_iter_next(&iter, "&o", &ap_path)) {
                GDBusProxy *ap = proxy_for(network_icon, ap_path, IFACE_AP);
                char *ssid = ap_ssid(ap);
                if (ssid != nullptr) {
                    listed *listed_network = g_new0(listed, 1);
                    listed_network->ssid = ssid;
                    listed_network->ap_path = g_strdup(ap_path);
                    listed_network->device_path = g_strdup(dev_path);
                    listed_network->strength = prop_u(ap, "Strength");
                    listed_network->flags = prop_u(ap, "Flags");
                    listed_network->wpa = prop_u(ap, "WpaFlags");
                    listed_network->rsn = prop_u(ap, "RsnFlags");
                    listed_network->current =
                        (active_ap != nullptr && strcmp(active_ap, ap_path) == 0) ||
                        (current_ap != nullptr && strcmp(current_ap, ap_path) == 0);
                    g_ptr_array_add(out, listed_network);
                }
                if (ap != nullptr)
                    g_object_unref(ap);
            }
        }
        if (aps != nullptr)
            g_variant_unref(aps);
        g_free(active_ap);
        g_object_unref(wifi);
    }
    g_list_free_full(objs, g_object_unref);
    g_ptr_array_sort(out, by_strength);
    /* One per SSID: the strongest (or the one we're on). */
    for (guint i = 0; i < out->len; i++) {
        listed *kept = g_ptr_array_index(out, i);
        for (guint j = i + 1; j < out->len;) {
            listed *duplicate = g_ptr_array_index(out, j);
            if (strcmp(kept->ssid, duplicate->ssid) == 0) {
                kept->current = kept->current || duplicate->current;
                g_ptr_array_remove_index(out, j);
            } else {
                j++;
            }
        }
    }
    return out;
}

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *menu_model) {
    network *network_icon = data;
    char *type = nullptr;
    GDBusProxy *ap = nullptr;
    GDBusProxy *active = primary(network_icon, &type, &ap);
    GPtrArray *nets = networks_in_range(network_icon, true,
                                        ap != nullptr ? g_dbus_proxy_get_object_path(ap) : nullptr);
    for (guint i = 0; i < nets->len && i < 15; i++) {
        listed *listed_network = g_ptr_array_index(nets, i);
        ap_choice *picked_network =
            xd_arena_alloc(&tree->arena, sizeof(*picked_network), alignof(ap_choice));
        *picked_network = (ap_choice){
            .net = network_icon,
            .ssid = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(listed_network->ssid)),
            .ap_path = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(listed_network->ap_path)),
            .device_path =
                xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(listed_network->device_path)),
            .wpa = listed_network->wpa,
            .rsn = listed_network->rsn,
            .flags = listed_network->flags,
        };
        xd_menu_item *it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, listed_network->ssid,
                                       signal_icon(listed_network->strength));
        it->action = act_connect;
        it->action_data = picked_network;
        it->bold = listed_network->current;
        bool secured = (listed_network->flags & AP_PRIVACY) || listed_network->wpa != 0 ||
                       listed_network->rsn != 0;
        char accel[32];
        g_snprintf(accel, sizeof(accel), "%s%u%%", secured ? "Secured  " : "",
                   listed_network->strength);
        it->accel = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(accel));
    }
    bool any_wifi = nets->len > 0;
    g_ptr_array_free(nets, TRUE);
    if (any_wifi)
        xd_menu_add(tree, menu_model, XD_MENU_SEPARATOR, nullptr, nullptr);
    xd_menu_item *it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Disconnect", nullptr);
    it->action = act_disconnect;
    it->action_data = network_icon;
    it->disabled = active == nullptr;
    g_free(type);
    if (ap != nullptr)
        g_object_unref(ap);
    if (active != nullptr)
        g_object_unref(active);
    if (prop_b(network_icon->nm, "WirelessHardwareEnabled")) {
        it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Wireless Networking", nullptr);
        it->action = act_wifi_toggle;
        it->action_data = network_icon;
        it->checked = prop_b(network_icon->nm, "WirelessEnabled");
    }
    xd_menu_add(tree, menu_model, XD_MENU_SEPARATOR, nullptr, nullptr);
    it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Status", nullptr);
    it->action = act_status;
    it->disabled = !network_icon->connected;
    it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Open Network Connections", nullptr);
    it->action = act_connections;
}

/* ---- setup ------------------------------------------------------------------------ */

/* One handler per signal: their argument lists differ. */
static void manager_props_changed([[maybe_unused]] GDBusProxy *proxy,
                                  [[maybe_unused]] GVariant *variant,
                                  [[maybe_unused]] const char *const *inv, gpointer data) {
    update(data);
}

static void object_props_changed([[maybe_unused]] GDBusObjectManagerClient *object_manager,
                                 [[maybe_unused]] GDBusObjectProxy *dbus_object_proxy,
                                 [[maybe_unused]] GDBusProxy *proxy,
                                 [[maybe_unused]] GVariant *variant,
                                 [[maybe_unused]] const char *const *inv, gpointer data) {
    update(data);
}

static void object_added_removed([[maybe_unused]] GDBusObjectManager *dbus_object_manager,
                                 [[maybe_unused]] GDBusObject *dbus_object, gpointer data) {
    update(data);
}

static void owner_changed([[maybe_unused]] GObject *object, [[maybe_unused]] GParamSpec *ps,
                          gpointer data) {
    update(data);
}

static void got_manager([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, nullptr);
    if (proxy == nullptr)
        return;
    network *network_icon = data;
    network_icon->nm = proxy;
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(manager_props_changed),
                     network_icon);
    update(network_icon);
}

static void got_objects([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GDBusObjectManager *dbus_object_manager =
        g_dbus_object_manager_client_new_for_bus_finish(res, nullptr);
    if (dbus_object_manager == nullptr)
        return; /* no NetworkManager: no network icon */
    network *network_icon = data;
    network_icon->objects = dbus_object_manager;
    g_signal_connect(dbus_object_manager, "interface-proxy-properties-changed",
                     G_CALLBACK(object_props_changed), network_icon);
    g_signal_connect(dbus_object_manager, "object-added", G_CALLBACK(object_added_removed),
                     network_icon);
    g_signal_connect(dbus_object_manager, "object-removed", G_CALLBACK(object_added_removed),
                     network_icon);
    /* NetworkManager starting or stopping. */
    g_signal_connect(dbus_object_manager, "notify::name-owner", G_CALLBACK(owner_changed),
                     network_icon);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, NM, NM_PATH, NM,
                             network_icon->cancel, got_manager, network_icon);
}

static void destroy(void *data) {
    network *network_icon = data;
    g_cancellable_cancel(network_icon->cancel);
    g_object_unref(network_icon->cancel);
    if (network_icon->nm != nullptr) {
        g_signal_handlers_disconnect_by_data(network_icon->nm, network_icon);
        g_object_unref(network_icon->nm);
    }
    if (network_icon->objects != nullptr) {
        g_signal_handlers_disconnect_by_data(network_icon->objects, network_icon);
        g_object_unref(network_icon->objects);
    }
    g_free(network_icon);
}

static void draw(void *data, const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    network *network_icon = data;
    if (network_icon->wifi)
        xd_theme_draw_tray_icon(theme, cairo, rect, XD_TRAY_ICON_WIRELESS,
                                (int)(network_icon->strength + 19) / 20,
                                network_icon->connected ? 0 : XD_TRAY_ICON_OFF);
    else
        xd_theme_draw_tray_icon(theme, cairo, rect, XD_TRAY_ICON_WIRED, 0,
                                network_icon->connected ? 0 : XD_TRAY_ICON_OFF);
}

static const xd_sysicon_ops ops = {
    .draw = draw,
    .icon = icon,
    .tooltip = tooltip,
    .menu = menu,
    .open = open_status,
    .destroy = destroy,
};

void xd_sys_network_init(xd_shell *shell) {
    network *network_icon = g_new0(network, 1);
    network_icon->shell = shell;
    network_icon->cancel = g_cancellable_new();
    xd_tray_add_sysicon(shell, "network", &ops, network_icon);
    g_dbus_object_manager_client_new_for_bus(
        G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, NM, "/org/freedesktop", nullptr,
        nullptr, nullptr, network_icon->cancel, got_objects, network_icon);
}
