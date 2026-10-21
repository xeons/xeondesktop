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

static GDBusProxy *proxy_for(network *n, const char *path, const char *iface) {
    if (n->objects == nullptr || path == nullptr || strcmp(path, "/") == 0)
        return nullptr;
    GDBusInterface *i = g_dbus_object_manager_get_interface(n->objects, path, iface);
    return i != nullptr ? G_DBUS_PROXY(i) : nullptr;
}

static GVariant *prop(GDBusProxy *p, const char *name) {
    return p != nullptr ? g_dbus_proxy_get_cached_property(p, name) : nullptr;
}

static unsigned prop_u(GDBusProxy *p, const char *name) {
    GVariant *v = prop(p, name);
    unsigned u = 0;
    if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
        u = g_variant_get_uint32(v);
    else if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE))
        u = g_variant_get_byte(v);
    if (v != nullptr)
        g_variant_unref(v);
    return u;
}

static bool prop_b(GDBusProxy *p, const char *name) {
    GVariant *v = prop(p, name);
    bool b = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
             g_variant_get_boolean(v);
    if (v != nullptr)
        g_variant_unref(v);
    return b;
}

static char *prop_s(GDBusProxy *p, const char *name) {
    GVariant *v = prop(p, name);
    char *s = nullptr;
    if (v != nullptr && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
                         g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
        s = g_variant_dup_string(v, nullptr);
    if (v != nullptr)
        g_variant_unref(v);
    return s;
}

/* An SSID (bytes) as text; nullptr for hidden networks. */
static char *ap_ssid(GDBusProxy *ap) {
    GVariant *v = prop(ap, "Ssid");
    char *s = nullptr;
    if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING)) {
        gsize len = 0;
        const char *bytes = g_variant_get_fixed_array(v, &len, 1);
        if (len > 0)
            s = g_utf8_make_valid(bytes, (gssize)len);
    }
    if (v != nullptr)
        g_variant_unref(v);
    return s;
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
static GDBusProxy *primary(network *n, char **type, GDBusProxy **ap) {
    *type = nullptr;
    *ap = nullptr;
    char *path = prop_s(n->nm, "PrimaryConnection");
    GDBusProxy *active = proxy_for(n, path, IFACE_ACTIVE);
    g_free(path);
    if (active == nullptr) {
        /* None yet (still connecting): the default active connection, or
         * the first one. */
        GVariant *all = prop(n->nm, "ActiveConnections");
        if (all != nullptr && g_variant_is_of_type(all, G_VARIANT_TYPE("ao"))) {
            GVariantIter iter;
            const char *p;
            g_variant_iter_init(&iter, all);
            while (g_variant_iter_next(&iter, "&o", &p)) {
                GDBusProxy *c = proxy_for(n, p, IFACE_ACTIVE);
                if (c == nullptr)
                    continue;
                if (active == nullptr || prop_b(c, "Default")) {
                    if (active != nullptr)
                        g_object_unref(active);
                    active = c;
                } else {
                    g_object_unref(c);
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
    *ap = proxy_for(n, specific, IFACE_AP);
    g_free(specific);
    return active;
}

static void update(network *n) {
    network was = *n;
    char *owner = n->objects != nullptr ? g_dbus_object_manager_client_get_name_owner(
                                              G_DBUS_OBJECT_MANAGER_CLIENT(n->objects))
                                        : nullptr;
    n->running = owner != nullptr;
    g_free(owner);
    unsigned state = prop_u(n->nm, "State");
    char *type = nullptr;
    GDBusProxy *ap = nullptr;
    GDBusProxy *active = primary(n, &type, &ap);
    bool wifi = type != nullptr && strstr(type, "wireless") != nullptr;
    const char *icon;
    n->wifi = wifi;
    n->connected = state >= NM_CONNECTED_GLOBAL - 20 && active != nullptr;
    n->strength = wifi ? prop_u(ap, "Strength") : 0;
    if (state >= NM_CONNECTED_GLOBAL - 20 && active != nullptr)
        icon = wifi ? signal_icon(prop_u(ap, "Strength")) : "network-wired";
    else if (state >= NM_CONNECTING && state < NM_CONNECTED_GLOBAL - 20)
        icon = wifi ? "network-wireless-acquiring" : "network-wired-acquiring";
    else
        icon = "network-offline";
    g_strlcpy(n->icon, icon, sizeof(n->icon));
    g_free(type);
    if (ap != nullptr)
        g_object_unref(ap);
    if (active != nullptr)
        g_object_unref(active);
    /* Any property of any of NetworkManager's objects lands here (access
     * points' signals on every scan): redraw only when the icon changed. */
    if (n->running != was.running || n->nm != was.nm || n->wifi != was.wifi ||
        n->connected != was.connected || (n->strength + 19) / 20 != (was.strength + 19) / 20 ||
        strcmp(n->icon, was.icon) != 0)
        xd_tray_changed(n->shell);
}

/* ---- the icon ------------------------------------------------------------------ */

static const char *icon(void *data) {
    network *n = data;
    return n->running && n->nm != nullptr && n->icon[0] != '\0' ? n->icon : nullptr;
}

static char *tooltip(void *data) {
    network *n = data;
    unsigned state = prop_u(n->nm, "State");
    char *type = nullptr;
    GDBusProxy *ap = nullptr;
    GDBusProxy *active = primary(n, &type, &ap);
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
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r != nullptr) {
        g_variant_unref(r);
    } else {
        fprintf(stderr, "xd-shell: network: %s\n", error->message);
        g_error_free(error);
    }
}

static void nm_call(network *n, const char *path, const char *iface, const char *method,
                    GVariant *args) {
    GDBusConnection *bus = g_dbus_proxy_get_connection(n->nm);
    g_dbus_connection_call(bus, NM, path, iface, method, args, nullptr, G_DBUS_CALL_FLAGS_NONE,
                           -1, n->cancel, call_done, n);
}

/* {"802-11-wireless-security": {...}} for a key, as NetworkManager wants
 * it for AddAndActivateConnection (it fills in the SSID from the AP). */
static GVariant *security_settings(const ap_choice *c, const char *key) {
    GVariantBuilder conn, sec;
    g_variant_builder_init(&conn, G_VARIANT_TYPE("a{sa{sv}}"));
    if (key != nullptr) {
        g_variant_builder_init(&sec, G_VARIANT_TYPE("a{sv}"));
        bool sae_only = (c->rsn & KEY_MGMT_SAE) && !(c->rsn & KEY_MGMT_PSK);
        if (c->wpa == 0 && c->rsn == 0) {
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

static void add_and_activate(const ap_choice *c, const char *key) {
    nm_call(c->net, NM_PATH, NM, "AddAndActivateConnection",
            g_variant_new("(@a{sa{sv}}oo)", security_settings(c, key), c->device_path,
                          c->ap_path));
}

typedef struct key_prompt {
    ap_choice choice; /* copied: the menu's arena is gone by then */
    char *ssid, *ap_path, *device_path;
} key_prompt;

static key_prompt *copy_choice(const ap_choice *c) {
    key_prompt *k = g_new0(key_prompt, 1);
    k->choice = *c;
    k->choice.ssid = k->ssid = g_strdup(c->ssid);
    k->choice.ap_path = k->ap_path = g_strdup(c->ap_path);
    k->choice.device_path = k->device_path = g_strdup(c->device_path);
    return k;
}

static void key_prompt_free(key_prompt *k) {
    g_free(k->ssid);
    g_free(k->ap_path);
    g_free(k->device_path);
    g_free(k);
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
    key_reader *r = data;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    /* No key is longer than 64 characters (WPA's in hex): anything much
     * longer isn't one, and isn't kept. */
    bool too_long = n > 0 && r->out->len + (size_t)n > KEY_MAX;
    if (n > 0 && !too_long) {
        g_string_append_len(r->out, buf, n);
        explicit_bzero(buf, sizeof(buf));
        return G_SOURCE_CONTINUE;
    }
    explicit_bzero(buf, sizeof(buf));
    if (n < 0 && errno == EINTR)
        return G_SOURCE_CONTINUE;
    g_strchomp(r->out->str);
    if (!too_long && r->out->str[0] != '\0')
        add_and_activate(&r->k->choice, r->out->str);
    explicit_bzero(r->out->str, r->out->allocated_len);
    g_string_free(r->out, TRUE);
    close(r->fd);
    key_prompt_free(r->k);
    g_free(r);
    return G_SOURCE_REMOVE;
}

/* Asks for the network key with xd-wifi-key, which prints it. Takes `k`. */
static void ask_key(key_prompt *k) {
    char *prog = xd_shell_own_program("xd-wifi-key");
    char *argv[] = {prog, k->ssid, nullptr};
    int out = -1;
    GError *error = nullptr;
    if (!g_spawn_async_with_pipes(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                                  nullptr, nullptr, &out, nullptr, &error)) {
        fprintf(stderr, "xd-shell: network: %s\n", error->message);
        g_error_free(error);
        g_free(prog);
        key_prompt_free(k);
        return;
    }
    g_free(prog);
    key_reader *r = g_new0(key_reader, 1);
    r->k = k;
    r->out = g_string_new(nullptr);
    r->fd = out;
    g_unix_fd_add(out, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_key, r);
}

typedef struct find_saved {
    key_prompt *k; /* the choice, copied */
    GVariant *paths;
    gsize next;
} find_saved;

static void check_next_saved(find_saved *f);

static void got_settings(GObject *src, GAsyncResult *res, gpointer data) {
    find_saved *f = data;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    bool match = false;
    if (r != nullptr) {
        GVariant *settings = g_variant_get_child_value(r, 0);
        GVariant *wifi = g_variant_lookup_value(settings, "802-11-wireless", nullptr);
        GVariant *ssid = wifi != nullptr ? g_variant_lookup_value(wifi, "ssid", nullptr) : nullptr;
        if (ssid != nullptr && g_variant_is_of_type(ssid, G_VARIANT_TYPE_BYTESTRING)) {
            gsize len = 0;
            const char *bytes = g_variant_get_fixed_array(ssid, &len, 1);
            match = len == strlen(f->k->ssid) && memcmp(bytes, f->k->ssid, len) == 0;
        }
        if (ssid != nullptr)
            g_variant_unref(ssid);
        if (wifi != nullptr)
            g_variant_unref(wifi);
        g_variant_unref(settings);
        g_variant_unref(r);
    }
    if (match) {
        /* A saved connection: NetworkManager has (or asks for) its key. */
        const char *path = nullptr;
        g_variant_get_child(f->paths, f->next - 1, "&o", &path);
        nm_call(f->k->choice.net, NM_PATH, NM, "ActivateConnection",
                g_variant_new("(ooo)", path, f->k->device_path, f->k->ap_path));
        g_variant_unref(f->paths);
        key_prompt_free(f->k);
        g_free(f);
        return;
    }
    check_next_saved(f);
}

static void check_next_saved(find_saved *f) {
    network *n = f->k->choice.net;
    if (f->next >= g_variant_n_children(f->paths)) {
        /* Not saved: open networks connect, secured ones need the key;
         * 802.1X (enterprise) networks need the full editor. */
        const ap_choice *c = &f->k->choice;
        g_variant_unref(f->paths);
        if ((c->wpa | c->rsn) & KEY_MGMT_8021X) {
            xd_shell_launch(n->shell, "nm-connection-editor");
            key_prompt_free(f->k);
        } else if ((c->flags & AP_PRIVACY) || c->wpa != 0 || c->rsn != 0) {
            ask_key(f->k);
        } else {
            add_and_activate(c, nullptr);
            key_prompt_free(f->k);
        }
        g_free(f);
        return;
    }
    const char *path = nullptr;
    g_variant_get_child(f->paths, f->next++, "&o", &path);
    g_dbus_connection_call(g_dbus_proxy_get_connection(n->nm), NM, path,
                           "org.freedesktop.NetworkManager.Settings.Connection", "GetSettings",
                           nullptr, G_VARIANT_TYPE("(a{sa{sv}})"), G_DBUS_CALL_FLAGS_NONE, -1,
                           n->cancel, got_settings, f);
}

static void got_connections(GObject *src, GAsyncResult *res, gpointer data) {
    key_prompt *k = data;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    find_saved *f = g_new0(find_saved, 1);
    f->k = k;
    f->paths = r != nullptr
                   ? g_variant_get_child_value(r, 0)
                   : g_variant_ref_sink(g_variant_new_array(G_VARIANT_TYPE_OBJECT_PATH, nullptr, 0));
    if (r != nullptr)
        g_variant_unref(r);
    check_next_saved(f);
}

static void act_connect(xd_shell *shell, void *d) {
    (void)shell;
    const ap_choice *c = d;
    key_prompt *k = copy_choice(c);
    g_dbus_connection_call(g_dbus_proxy_get_connection(c->net->nm), NM, NM_SETTINGS_PATH,
                           "org.freedesktop.NetworkManager.Settings", "ListConnections",
                           nullptr, G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE, -1,
                           c->net->cancel, got_connections, k);
}

static void act_disconnect(xd_shell *shell, void *d) {
    (void)shell;
    network *n = d;
    char *type = nullptr;
    GDBusProxy *ap = nullptr;
    GDBusProxy *active = primary(n, &type, &ap);
    if (active != nullptr)
        nm_call(n, NM_PATH, NM, "DeactivateConnection",
                g_variant_new("(o)", g_dbus_proxy_get_object_path(active)));
    g_free(type);
    if (ap != nullptr)
        g_object_unref(ap);
    if (active != nullptr)
        g_object_unref(active);
}

static void act_wifi_toggle(xd_shell *shell, void *d) {
    (void)shell;
    network *n = d;
    bool on = prop_b(n->nm, "WirelessEnabled");
    nm_call(n, NM_PATH, "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", NM, "WirelessEnabled", g_variant_new_boolean(!on)));
}

static void act_status(xd_shell *shell, [[maybe_unused]] void *d) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-netconn", "--status"));
}

static void act_connections(xd_shell *shell, [[maybe_unused]] void *d) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-netconn", nullptr));
}

static bool have_wifi(network *n) {
    bool any = false;
    GList *objs = n->objects != nullptr ? g_dbus_object_manager_get_objects(n->objects) : nullptr;
    for (GList *o = objs; o != nullptr && !any; o = o->next) {
        GDBusProxy *wifi = proxy_for(n, g_dbus_object_get_object_path(o->data), IFACE_WIRELESS);
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
    network *n = data;
    if (n->nm == nullptr)
        return;
    if (n->connected)
        act_status(n->shell, n);
    else if (have_wifi(n))
        xd_shell_launch(n->shell, xd_shell_own_command(n->shell, "xd-netconn", "--wireless"));
    else
        act_connections(n->shell, n);
}

/* ---- the menu ------------------------------------------------------------------- */

typedef struct listed {
    char *ssid;
    char *ap_path, *device_path;
    unsigned strength, flags, wpa, rsn;
    bool current;
} listed;

static int by_strength(gconstpointer a, gconstpointer b) {
    const listed *x = *(listed *const *)a, *y = *(listed *const *)b;
    return (int)y->strength - (int)x->strength;
}

static void listed_free(gpointer p) {
    listed *l = p;
    g_free(l->ssid);
    g_free(l->ap_path);
    g_free(l->device_path);
    g_free(l);
}

/* The networks in range, strongest first, one entry per SSID. */
static GPtrArray *networks_in_range(network *n, bool rescan, const char *current_ap) {
    GPtrArray *out = g_ptr_array_new_with_free_func(listed_free);
    GList *objs = g_dbus_object_manager_get_objects(n->objects);
    for (GList *o = objs; o != nullptr; o = o->next) {
        const char *dev_path = g_dbus_object_get_object_path(o->data);
        GDBusProxy *wifi = proxy_for(n, dev_path, IFACE_WIRELESS);
        if (wifi == nullptr)
            continue;
        if (rescan)
            nm_call(n, dev_path, IFACE_WIRELESS, "RequestScan",
                    g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"),
                                                                    nullptr, 0)));
        char *active_ap = prop_s(wifi, "ActiveAccessPoint");
        GVariant *aps = prop(wifi, "AccessPoints");
        GVariantIter iter;
        const char *ap_path;
        if (aps != nullptr && g_variant_is_of_type(aps, G_VARIANT_TYPE("ao"))) {
            g_variant_iter_init(&iter, aps);
            while (g_variant_iter_next(&iter, "&o", &ap_path)) {
                GDBusProxy *ap = proxy_for(n, ap_path, IFACE_AP);
                char *ssid = ap_ssid(ap);
                if (ssid != nullptr) {
                    listed *l = g_new0(listed, 1);
                    l->ssid = ssid;
                    l->ap_path = g_strdup(ap_path);
                    l->device_path = g_strdup(dev_path);
                    l->strength = prop_u(ap, "Strength");
                    l->flags = prop_u(ap, "Flags");
                    l->wpa = prop_u(ap, "WpaFlags");
                    l->rsn = prop_u(ap, "RsnFlags");
                    l->current = (active_ap != nullptr && strcmp(active_ap, ap_path) == 0) ||
                                 (current_ap != nullptr && strcmp(current_ap, ap_path) == 0);
                    g_ptr_array_add(out, l);
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
        listed *a = g_ptr_array_index(out, i);
        for (guint j = i + 1; j < out->len;) {
            listed *b = g_ptr_array_index(out, j);
            if (strcmp(a->ssid, b->ssid) == 0) {
                a->current = a->current || b->current;
                g_ptr_array_remove_index(out, j);
            } else {
                j++;
            }
        }
    }
    return out;
}

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *m) {
    network *n = data;
    char *type = nullptr;
    GDBusProxy *ap = nullptr;
    GDBusProxy *active = primary(n, &type, &ap);
    GPtrArray *nets =
        networks_in_range(n, true, ap != nullptr ? g_dbus_proxy_get_object_path(ap) : nullptr);
    for (guint i = 0; i < nets->len && i < 15; i++) {
        listed *l = g_ptr_array_index(nets, i);
        ap_choice *c = xd_arena_alloc(&tree->arena, sizeof(*c), alignof(ap_choice));
        *c = (ap_choice){
            .net = n,
            .ssid = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(l->ssid)),
            .ap_path = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(l->ap_path)),
            .device_path = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(l->device_path)),
            .wpa = l->wpa,
            .rsn = l->rsn,
            .flags = l->flags,
        };
        xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, l->ssid, signal_icon(l->strength));
        it->action = act_connect;
        it->action_data = c;
        it->bold = l->current;
        bool secured = (l->flags & AP_PRIVACY) || l->wpa != 0 || l->rsn != 0;
        char accel[32];
        g_snprintf(accel, sizeof(accel), "%s%u%%", secured ? "Secured  " : "", l->strength);
        it->accel = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(accel));
    }
    bool any_wifi = nets->len > 0;
    g_ptr_array_free(nets, TRUE);
    if (any_wifi)
        xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, "Disconnect", nullptr);
    it->action = act_disconnect;
    it->action_data = n;
    it->disabled = active == nullptr;
    g_free(type);
    if (ap != nullptr)
        g_object_unref(ap);
    if (active != nullptr)
        g_object_unref(active);
    if (prop_b(n->nm, "WirelessHardwareEnabled")) {
        it = xd_menu_add(tree, m, XD_MENU_ACTION, "Wireless Networking", nullptr);
        it->action = act_wifi_toggle;
        it->action_data = n;
        it->checked = prop_b(n->nm, "WirelessEnabled");
    }
    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
    it = xd_menu_add(tree, m, XD_MENU_ACTION, "Status", nullptr);
    it->action = act_status;
    it->disabled = !n->connected;
    it = xd_menu_add(tree, m, XD_MENU_ACTION, "Open Network Connections", nullptr);
    it->action = act_connections;
}

/* ---- setup ------------------------------------------------------------------------ */

/* One handler per signal: their argument lists differ. */
static void manager_props_changed([[maybe_unused]] GDBusProxy *p, [[maybe_unused]] GVariant *c,
                                  [[maybe_unused]] const char *const *inv, gpointer data) {
    update(data);
}

static void object_props_changed([[maybe_unused]] GDBusObjectManagerClient *m,
                                 [[maybe_unused]] GDBusObjectProxy *o,
                                 [[maybe_unused]] GDBusProxy *i, [[maybe_unused]] GVariant *c,
                                 [[maybe_unused]] const char *const *inv, gpointer data) {
    update(data);
}

static void object_added_removed([[maybe_unused]] GDBusObjectManager *m,
                                 [[maybe_unused]] GDBusObject *o, gpointer data) {
    update(data);
}

static void owner_changed([[maybe_unused]] GObject *o, [[maybe_unused]] GParamSpec *ps,
                          gpointer data) {
    update(data);
}

static void got_manager([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GDBusProxy *p = g_dbus_proxy_new_for_bus_finish(res, nullptr);
    if (p == nullptr)
        return;
    network *n = data;
    n->nm = p;
    g_signal_connect(p, "g-properties-changed", G_CALLBACK(manager_props_changed), n);
    update(n);
}

static void got_objects([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GDBusObjectManager *m = g_dbus_object_manager_client_new_for_bus_finish(res, nullptr);
    if (m == nullptr)
        return; /* no NetworkManager: no network icon */
    network *n = data;
    n->objects = m;
    g_signal_connect(m, "interface-proxy-properties-changed", G_CALLBACK(object_props_changed), n);
    g_signal_connect(m, "object-added", G_CALLBACK(object_added_removed), n);
    g_signal_connect(m, "object-removed", G_CALLBACK(object_added_removed), n);
    /* NetworkManager starting or stopping. */
    g_signal_connect(m, "notify::name-owner", G_CALLBACK(owner_changed), n);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, NM, NM_PATH, NM,
                             n->cancel, got_manager, n);
}

static void destroy(void *data) {
    network *n = data;
    g_cancellable_cancel(n->cancel);
    g_object_unref(n->cancel);
    if (n->nm != nullptr) {
        g_signal_handlers_disconnect_by_data(n->nm, n);
        g_object_unref(n->nm);
    }
    if (n->objects != nullptr) {
        g_signal_handlers_disconnect_by_data(n->objects, n);
        g_object_unref(n->objects);
    }
    g_free(n);
}

static void draw(void *data, const xd_theme *t, cairo_t *cr, xd_rect r) {
    network *n = data;
    if (n->wifi)
        xd_theme_draw_tray_icon(t, cr, r, XD_TRAY_ICON_WIRELESS, (int)(n->strength + 19) / 20,
                                n->connected ? 0 : XD_TRAY_ICON_OFF);
    else
        xd_theme_draw_tray_icon(t, cr, r, XD_TRAY_ICON_WIRED, 0,
                                n->connected ? 0 : XD_TRAY_ICON_OFF);
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
    network *n = g_new0(network, 1);
    n->shell = shell;
    n->cancel = g_cancellable_new();
    xd_tray_add_sysicon(shell, "network", &ops, n);
    g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM,
                                             G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, NM,
                                             "/org/freedesktop", nullptr, nullptr, nullptr,
                                             n->cancel, got_objects, n);
}
