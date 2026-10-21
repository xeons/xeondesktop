/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "launcher.h"

#define LAUNCHER_IFACE "com.canonical.Unity.LauncherEntry"
#define URI_PREFIX "application://"

/* ---- the properties --------------------------------------------------------------- */

char *xd_launcher_desktop_id(const char *app_uri) {
    if (app_uri == nullptr || !g_str_has_prefix(app_uri, URI_PREFIX))
        return nullptr;
    const char *name = app_uri + strlen(URI_PREFIX);
    if (name[0] == '\0' || strchr(name, '/') != nullptr)
        return nullptr;
    return g_str_has_suffix(name, ".desktop") ? g_strdup(name)
                                              : g_strconcat(name, ".desktop", nullptr);
}

/* Unity says x; senders written by hand send whatever integer they had. */
static bool integer_of(GVariant *v, int64_t *out) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
        *out = g_variant_get_int64(v);
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
        *out = g_variant_get_int32(v);
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
        *out = g_variant_get_uint32(v);
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64))
        *out = (int64_t)MIN(g_variant_get_uint64(v), (guint64)INT64_MAX);
    else
        return false;
    return true;
}

static void set_bool(bool *field, GVariant *v) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
        *field = g_variant_get_boolean(v);
}

bool xd_launcher_apply(xd_launcher_state *st, GVariant *props) {
    if (props == nullptr || !g_variant_is_of_type(props, G_VARIANT_TYPE_VARDICT))
        return false;
    xd_launcher_state was = *st;
    GVariantIter iter;
    const char *key = nullptr;
    GVariant *v = nullptr;
    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &v)) {
        if (strcmp(key, "progress") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) {
            double p = g_variant_get_double(v);
            if (isfinite(p))
                st->progress = CLAMP(p, 0.0, 1.0);
        } else if (strcmp(key, "progress-visible") == 0) {
            set_bool(&st->progress_visible, v);
        } else if (strcmp(key, "count") == 0) {
            int64_t n = 0;
            if (integer_of(v, &n))
                st->count = n;
        } else if (strcmp(key, "count-visible") == 0) {
            set_bool(&st->count_visible, v);
        } else if (strcmp(key, "urgent") == 0) {
            set_bool(&st->urgent, v);
        }
        g_variant_unref(v);
    }
    return memcmp(&was, st, sizeof(was)) != 0;
}

bool xd_launcher_key_is(const char *id, const char *key) {
    if (id == nullptr || key == nullptr)
        return false;
    if (strcmp(id, key) == 0)
        return true;
    /* A window whose app id has no desktop file found for it: its app id
     * is the desktop file's name, as Wayland asks of programs. */
    if (!g_str_has_prefix(key, "app:") || !g_str_has_suffix(id, ".desktop"))
        return false;
    size_t n = strlen(id) - strlen(".desktop");
    return strncmp(key + 4, id, n) == 0 && key[4 + n] == '\0';
}

void xd_launcher_count_text(int64_t count, char *buf, size_t size) {
    if (count > 99)
        snprintf(buf, size, "99+");
    else
        snprintf(buf, size, "%" PRId64, count);
}

/* ---- the bus ----------------------------------------------------------------------- */

typedef struct entry {
    char *id;     /* desktop file id */
    char *sender; /* its unique bus name */
    xd_launcher_state st;
    bool seen; /* it has had a window */
} entry;

typedef struct sender_watch {
    xd_launcher *l;
    char *name;
} sender_watch;

struct xd_launcher {
    xd_launcher_changed_fn changed;
    void *data;
    GCancellable *cancel;
    GDBusConnection *bus;
    guint subscription;
    GPtrArray *entries;  /* entry * (owned) */
    GHashTable *senders; /* unique name -> watcher id */
};

static void entry_free(gpointer p) {
    entry *e = p;
    g_free(e->id);
    g_free(e->sender);
    g_free(e);
}

static void unwatch(gpointer id) { g_bus_unwatch_name(GPOINTER_TO_UINT(id)); }

static void watch_free(gpointer p) {
    sender_watch *w = p;
    g_free(w->name);
    g_free(w);
}

/* Drops entry i, telling the caller its state went. */
static void forget(xd_launcher *l, guint i) {
    entry *e = g_ptr_array_steal_index(l->entries, i);
    xd_launcher_state none = {0};
    if (l->changed != nullptr)
        l->changed(l->data, e->id, &e->st, &none);
    entry_free(e);
}

static void name_vanished([[maybe_unused]] GDBusConnection *bus, const char *name, gpointer data) {
    xd_launcher *l = ((sender_watch *)data)->l;
    for (guint i = l->entries->len; i-- > 0;) {
        if (strcmp(((entry *)g_ptr_array_index(l->entries, i))->sender, name) == 0)
            forget(l, i);
    }
    /* Frees `data`: the watch goes. */
    g_hash_table_remove(l->senders, name);
}

static void watch_sender(xd_launcher *l, const char *name) {
    if (g_hash_table_contains(l->senders, name))
        return;
    sender_watch *w = g_new0(sender_watch, 1);
    w->l = l;
    w->name = g_strdup(name);
    guint id = g_bus_watch_name_on_connection(l->bus, name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                              nullptr, name_vanished, w, watch_free);
    g_hash_table_insert(l->senders, g_strdup(name), GUINT_TO_POINTER(id));
}

static entry *find(const xd_launcher *l, const char *id, guint *at) {
    for (guint i = 0; i < l->entries->len; i++) {
        entry *e = g_ptr_array_index(l->entries, i);
        if (strcmp(e->id, id) == 0) {
            if (at != nullptr)
                *at = i;
            return e;
        }
    }
    return nullptr;
}

static void update([[maybe_unused]] GDBusConnection *bus, const char *sender,
                   [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                   [[maybe_unused]] const char *signal, GVariant *params, gpointer data) {
    xd_launcher *l = data;
    if (sender == nullptr || !g_variant_is_of_type(params, G_VARIANT_TYPE("(sa{sv})")))
        return;
    const char *uri = nullptr;
    GVariant *props = nullptr;
    g_variant_get(params, "(&s@a{sv})", &uri, &props);
    char *id = xd_launcher_desktop_id(uri);
    if (id != nullptr) {
        entry *e = find(l, id, nullptr);
        if (e == nullptr) {
            e = g_new0(entry, 1);
            e->id = g_strdup(id);
            g_ptr_array_add(l->entries, e);
        }
        /* The program's latest sender speaks for it (one that restarted). */
        if (e->sender == nullptr || strcmp(e->sender, sender) != 0) {
            g_free(e->sender);
            e->sender = g_strdup(sender);
            watch_sender(l, sender);
        }
        xd_launcher_state was = e->st;
        if (xd_launcher_apply(&e->st, props) && l->changed != nullptr)
            l->changed(l->data, id, &was, &e->st);
        g_free(id);
    }
    g_variant_unref(props);
}

static void bus_ready([[maybe_unused]] GObject *source, GAsyncResult *res, gpointer data) {
    GDBusConnection *bus = g_bus_get_finish(res, nullptr);
    if (bus == nullptr)
        return; /* cancelled (`data` is gone), or no session bus */
    xd_launcher *l = data;
    l->bus = bus;
    l->subscription = g_dbus_connection_signal_subscribe(
        bus, nullptr, LAUNCHER_IFACE, "Update", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, update,
        l, nullptr);
}

xd_launcher *xd_launcher_new(xd_launcher_changed_fn changed, void *data) {
    xd_launcher *l = g_new0(xd_launcher, 1);
    l->changed = changed;
    l->data = data;
    l->cancel = g_cancellable_new();
    l->entries = g_ptr_array_new_with_free_func(entry_free);
    l->senders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, unwatch);
    g_bus_get(G_BUS_TYPE_SESSION, l->cancel, bus_ready, l);
    return l;
}

void xd_launcher_free(xd_launcher *l) {
    if (l == nullptr)
        return;
    g_cancellable_cancel(l->cancel);
    g_object_unref(l->cancel);
    g_hash_table_destroy(l->senders);
    g_ptr_array_free(l->entries, TRUE);
    if (l->bus != nullptr) {
        g_dbus_connection_signal_unsubscribe(l->bus, l->subscription);
        g_object_unref(l->bus);
    }
    g_free(l);
}

const xd_launcher_state *xd_launcher_lookup(const xd_launcher *l, const char *key) {
    if (l == nullptr || key == nullptr)
        return nullptr;
    for (guint i = 0; i < l->entries->len; i++) {
        const entry *e = g_ptr_array_index(l->entries, i);
        if (xd_launcher_key_is(e->id, key))
            return &e->st;
    }
    return nullptr;
}

void xd_launcher_sweep(xd_launcher *l, GHashTable *open) {
    if (l == nullptr)
        return;
    for (guint i = l->entries->len; i-- > 0;) {
        entry *e = g_ptr_array_index(l->entries, i);
        bool has_window = false;
        GHashTableIter it;
        gpointer key = nullptr;
        g_hash_table_iter_init(&it, open);
        while (!has_window && g_hash_table_iter_next(&it, &key, nullptr))
            has_window = xd_launcher_key_is(e->id, key);
        if (has_window)
            e->seen = true;
        else if (e->seen)
            forget(l, i);
    }
}
