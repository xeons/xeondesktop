/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>

#include "jumplist.h"
#include "mpris.h"

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"

typedef struct player {
    xd_mpris *mp;
    char *name;
    GCancellable *cancel; /* its proxies on the way; cancelled as it goes */
    GDBusProxy *root;     /* DesktopEntry */
    GDBusProxy *player;   /* PlaybackStatus, CanGoNext... */
} player;

struct xd_mpris {
    void (*changed)(void *data);
    void *data;
    GCancellable *cancel;
    GDBusConnection *bus;
    guint watch;
    GPtrArray *players; /* player * (owned) */
};

static void player_free(gpointer p) {
    player *pl = p;
    g_cancellable_cancel(pl->cancel);
    g_object_unref(pl->cancel);
    if (pl->root != nullptr) {
        g_signal_handlers_disconnect_by_data(pl->root, pl);
        g_object_unref(pl->root);
    }
    if (pl->player != nullptr) {
        g_signal_handlers_disconnect_by_data(pl->player, pl);
        g_object_unref(pl->player);
    }
    g_free(pl->name);
    g_free(pl);
}

static void tell(xd_mpris *mp) {
    if (mp->changed != nullptr)
        mp->changed(mp->data);
}

static void props_changed([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *changed,
                          [[maybe_unused]] GStrv invalidated, gpointer data) {
    tell(((player *)data)->mp);
}

static void proxy_ready([[maybe_unused]] GObject *source, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GDBusProxy *proxy = g_dbus_proxy_new_finish(res, &error);
    if (proxy == nullptr) {
        /* Cancelled: `data` is gone with its player. */
        g_error_free(error);
        return;
    }
    player *pl = data;
    if (strcmp(g_dbus_proxy_get_interface_name(proxy), MPRIS_ROOT) == 0)
        pl->root = proxy;
    else
        pl->player = proxy;
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(props_changed), pl);
    tell(pl->mp);
}

static player *find(xd_mpris *mp, const char *name, guint *at) {
    for (guint i = 0; i < mp->players->len; i++) {
        player *pl = g_ptr_array_index(mp->players, i);
        if (strcmp(pl->name, name) == 0) {
            if (at != nullptr)
                *at = i;
            return pl;
        }
    }
    return nullptr;
}

static void add_player(xd_mpris *mp, const char *name) {
    if (!g_str_has_prefix(name, MPRIS_PREFIX) || find(mp, name, nullptr) != nullptr)
        return;
    player *pl = g_new0(player, 1);
    pl->mp = mp;
    pl->name = g_strdup(name);
    pl->cancel = g_cancellable_new();
    g_ptr_array_add(mp->players, pl);
    const char *ifaces[] = {MPRIS_ROOT, MPRIS_PLAYER};
    for (size_t i = 0; i < G_N_ELEMENTS(ifaces); i++)
        g_dbus_proxy_new(mp->bus, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, nullptr, name, MPRIS_PATH,
                         ifaces[i], pl->cancel, proxy_ready, pl);
}

static void name_owner_changed([[maybe_unused]] GDBusConnection *bus,
                               [[maybe_unused]] const char *sender,
                               [[maybe_unused]] const char *path,
                               [[maybe_unused]] const char *iface,
                               [[maybe_unused]] const char *signal, GVariant *params,
                               gpointer data) {
    xd_mpris *mp = data;
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
    guint at = 0;
    if (find(mp, name, &at) != nullptr && new_owner[0] == '\0') {
        g_ptr_array_remove_index(mp->players, at);
        tell(mp);
    } else if (new_owner[0] != '\0') {
        add_player(mp, name);
    }
}

static void names_listed(GObject *source, GAsyncResult *res, gpointer data) {
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, nullptr);
    if (ret == nullptr)
        return; /* cancelled (`data` is gone), or no bus */
    xd_mpris *mp = data;
    GVariantIter *iter = nullptr;
    const char *name = nullptr;
    g_variant_get(ret, "(as)", &iter);
    while (g_variant_iter_next(iter, "&s", &name))
        add_player(mp, name);
    g_variant_iter_free(iter);
    g_variant_unref(ret);
}

static void bus_ready([[maybe_unused]] GObject *source, GAsyncResult *res, gpointer data) {
    GDBusConnection *bus = g_bus_get_finish(res, nullptr);
    if (bus == nullptr)
        return; /* cancelled, or no session bus: no buttons */
    xd_mpris *mp = data;
    mp->bus = bus;
    mp->watch = g_dbus_connection_signal_subscribe(
        bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", "org.mpris.MediaPlayer2",
        G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE, name_owner_changed, mp, nullptr);
    g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "ListNames", nullptr, G_VARIANT_TYPE("(as)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, mp->cancel, names_listed, mp);
}

xd_mpris *xd_mpris_new(void (*changed)(void *data), void *data) {
    xd_mpris *mp = g_new0(xd_mpris, 1);
    mp->changed = changed;
    mp->data = data;
    mp->cancel = g_cancellable_new();
    mp->players = g_ptr_array_new_with_free_func(player_free);
    g_bus_get(G_BUS_TYPE_SESSION, mp->cancel, bus_ready, mp);
    return mp;
}

void xd_mpris_free(xd_mpris *mp) {
    if (mp == nullptr)
        return;
    g_cancellable_cancel(mp->cancel);
    g_object_unref(mp->cancel);
    g_ptr_array_free(mp->players, TRUE);
    if (mp->bus != nullptr) {
        g_dbus_connection_signal_unsubscribe(mp->bus, mp->watch);
        g_object_unref(mp->bus);
    }
    g_free(mp);
}

static char *string_prop(GDBusProxy *proxy, const char *prop) {
    GVariant *v = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, prop) : nullptr;
    char *s = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                  ? g_variant_dup_string(v, nullptr)
                  : nullptr;
    if (v != nullptr)
        g_variant_unref(v);
    return s;
}

static bool bool_prop(GDBusProxy *proxy, const char *prop) {
    GVariant *v = g_dbus_proxy_get_cached_property(proxy, prop);
    bool b = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
             g_variant_get_boolean(v);
    if (v != nullptr)
        g_variant_unref(v);
    return b;
}

const char *xd_mpris_player_of(xd_mpris *mp, const char *desktop_id, const char *app_id) {
    if (mp == nullptr)
        return nullptr;
    const char *found = nullptr;
    for (guint i = 0; i < mp->players->len; i++) {
        player *pl = g_ptr_array_index(mp->players, i);
        /* Not until both proxies are in, and the player is still there
         * (a proxy follows its name's owner). */
        char *owner = pl->player != nullptr ? g_dbus_proxy_get_name_owner(pl->player) : nullptr;
        if (pl->root == nullptr || owner == nullptr) {
            g_free(owner);
            continue;
        }
        g_free(owner);
        char *entry = string_prop(pl->root, "DesktopEntry");
        bool mine = xd_jumplist_player_of(entry, pl->name, desktop_id, app_id);
        g_free(entry);
        if (mine && found != nullptr)
            return nullptr; /* two of them */
        if (mine)
            found = pl->name;
    }
    return found;
}

bool xd_mpris_state_of(xd_mpris *mp, const char *name, xd_mpris_state *out) {
    player *pl = mp != nullptr && name != nullptr ? find(mp, name, nullptr) : nullptr;
    if (pl == nullptr || pl->player == nullptr)
        return false;
    char *status = string_prop(pl->player, "PlaybackStatus");
    bool can_control = bool_prop(pl->player, "CanControl");
    *out = (xd_mpris_state){
        .can_previous = can_control && bool_prop(pl->player, "CanGoPrevious"),
        .can_next = can_control && bool_prop(pl->player, "CanGoNext"),
        .can_play_pause = can_control && (bool_prop(pl->player, "CanPlay") ||
                                          bool_prop(pl->player, "CanPause")),
        .playing = status != nullptr && strcmp(status, "Playing") == 0,
    };
    g_free(status);
    return true;
}

void xd_mpris_send(xd_mpris *mp, const char *name, const char *method) {
    player *pl = mp != nullptr && name != nullptr ? find(mp, name, nullptr) : nullptr;
    if (pl != nullptr && pl->player != nullptr)
        g_dbus_proxy_call(pl->player, method, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
                          nullptr, nullptr, nullptr);
}
