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
static bool integer_of(GVariant *variant, int64_t *out) {
    if (g_variant_is_of_type(variant, G_VARIANT_TYPE_INT64))
        *out = g_variant_get_int64(variant);
    else if (g_variant_is_of_type(variant, G_VARIANT_TYPE_INT32))
        *out = g_variant_get_int32(variant);
    else if (g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT32))
        *out = g_variant_get_uint32(variant);
    else if (g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT64))
        *out = (int64_t)MIN(g_variant_get_uint64(variant), (guint64)INT64_MAX);
    else
        return false;
    return true;
}

static void set_bool(bool *field, GVariant *variant) {
    if (g_variant_is_of_type(variant, G_VARIANT_TYPE_BOOLEAN))
        *field = g_variant_get_boolean(variant);
}

bool xd_launcher_apply(xd_launcher_state *st, GVariant *props) {
    if (props == nullptr || !g_variant_is_of_type(props, G_VARIANT_TYPE_VARDICT))
        return false;
    xd_launcher_state was = *st;
    GVariantIter iter;
    const char *key = nullptr;
    GVariant *variant = nullptr;
    g_variant_iter_init(&iter, props);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &variant)) {
        if (strcmp(key, "progress") == 0 && g_variant_is_of_type(variant, G_VARIANT_TYPE_DOUBLE)) {
            double progress = g_variant_get_double(variant);
            if (isfinite(progress))
                st->progress = CLAMP(progress, 0.0, 1.0);
        } else if (strcmp(key, "progress-visible") == 0) {
            set_bool(&st->progress_visible, variant);
        } else if (strcmp(key, "count") == 0) {
            int64_t count = 0;
            if (integer_of(variant, &count))
                st->count = count;
        } else if (strcmp(key, "count-visible") == 0) {
            set_bool(&st->count_visible, variant);
        } else if (strcmp(key, "urgent") == 0) {
            set_bool(&st->urgent, variant);
        }
        g_variant_unref(variant);
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
    size_t count = strlen(id) - strlen(".desktop");
    return strncmp(key + 4, id, count) == 0 && key[4 + count] == '\0';
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

static void entry_free(gpointer user_data) {
    entry *launcher_entry = user_data;
    g_free(launcher_entry->id);
    g_free(launcher_entry->sender);
    g_free(launcher_entry);
}

static void unwatch(gpointer id) { g_bus_unwatch_name(GPOINTER_TO_UINT(id)); }

static void watch_free(gpointer user_data) {
    sender_watch *name_watch = user_data;
    g_free(name_watch->name);
    g_free(name_watch);
}

/* Drops entry i, telling the caller its state went. */
static void forget(xd_launcher *launcher, guint i) {
    entry *launcher_entry = g_ptr_array_steal_index(launcher->entries, i);
    xd_launcher_state none = {0};
    if (launcher->changed != nullptr)
        launcher->changed(launcher->data, launcher_entry->id, &launcher_entry->st, &none);
    entry_free(launcher_entry);
}

static void name_vanished([[maybe_unused]] GDBusConnection *bus, const char *name, gpointer data) {
    xd_launcher *launcher = ((sender_watch *)data)->l;
    for (guint i = launcher->entries->len; i-- > 0;) {
        if (strcmp(((entry *)g_ptr_array_index(launcher->entries, i))->sender, name) == 0)
            forget(launcher, i);
    }
    /* Frees `data`: the watch goes. */
    g_hash_table_remove(launcher->senders, name);
}

static void watch_sender(xd_launcher *launcher, const char *name) {
    if (g_hash_table_contains(launcher->senders, name))
        return;
    sender_watch *name_watch = g_new0(sender_watch, 1);
    name_watch->l = launcher;
    name_watch->name = g_strdup(name);
    guint id = g_bus_watch_name_on_connection(launcher->bus, name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                              nullptr, name_vanished, name_watch, watch_free);
    g_hash_table_insert(launcher->senders, g_strdup(name), GUINT_TO_POINTER(id));
}

static entry *find(const xd_launcher *launcher, const char *id, guint *at) {
    for (guint i = 0; i < launcher->entries->len; i++) {
        entry *launcher_entry = g_ptr_array_index(launcher->entries, i);
        if (strcmp(launcher_entry->id, id) == 0) {
            if (at != nullptr)
                *at = i;
            return launcher_entry;
        }
    }
    return nullptr;
}

static void update([[maybe_unused]] GDBusConnection *bus, const char *sender,
                   [[maybe_unused]] const char *path, [[maybe_unused]] const char *iface,
                   [[maybe_unused]] const char *signal, GVariant *params, gpointer data) {
    xd_launcher *launcher = data;
    if (sender == nullptr || !g_variant_is_of_type(params, G_VARIANT_TYPE("(sa{sv})")))
        return;
    const char *uri = nullptr;
    GVariant *props = nullptr;
    g_variant_get(params, "(&s@a{sv})", &uri, &props);
    char *id = xd_launcher_desktop_id(uri);
    if (id != nullptr) {
        entry *launcher_entry = find(launcher, id, nullptr);
        if (launcher_entry == nullptr) {
            launcher_entry = g_new0(entry, 1);
            launcher_entry->id = g_strdup(id);
            g_ptr_array_add(launcher->entries, launcher_entry);
        }
        /* The program's latest sender speaks for it (one that restarted). */
        if (launcher_entry->sender == nullptr || strcmp(launcher_entry->sender, sender) != 0) {
            g_free(launcher_entry->sender);
            launcher_entry->sender = g_strdup(sender);
            watch_sender(launcher, sender);
        }
        xd_launcher_state was = launcher_entry->st;
        if (xd_launcher_apply(&launcher_entry->st, props) && launcher->changed != nullptr)
            launcher->changed(launcher->data, id, &was, &launcher_entry->st);
        g_free(id);
    }
    g_variant_unref(props);
}

static void bus_ready([[maybe_unused]] GObject *source, GAsyncResult *res, gpointer data) {
    GDBusConnection *bus = g_bus_get_finish(res, nullptr);
    if (bus == nullptr)
        return; /* cancelled (`data` is gone), or no session bus */
    xd_launcher *launcher = data;
    launcher->bus = bus;
    launcher->subscription =
        g_dbus_connection_signal_subscribe(bus, nullptr, LAUNCHER_IFACE, "Update", nullptr, nullptr,
                                           G_DBUS_SIGNAL_FLAGS_NONE, update, launcher, nullptr);
}

xd_launcher *xd_launcher_new(xd_launcher_changed_fn changed, void *data) {
    xd_launcher *launcher = g_new0(xd_launcher, 1);
    launcher->changed = changed;
    launcher->data = data;
    launcher->cancel = g_cancellable_new();
    launcher->entries = g_ptr_array_new_with_free_func(entry_free);
    launcher->senders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, unwatch);
    g_bus_get(G_BUS_TYPE_SESSION, launcher->cancel, bus_ready, launcher);
    return launcher;
}

void xd_launcher_free(xd_launcher *launcher) {
    if (launcher == nullptr)
        return;
    g_cancellable_cancel(launcher->cancel);
    g_object_unref(launcher->cancel);
    g_hash_table_destroy(launcher->senders);
    g_ptr_array_free(launcher->entries, TRUE);
    if (launcher->bus != nullptr) {
        g_dbus_connection_signal_unsubscribe(launcher->bus, launcher->subscription);
        g_object_unref(launcher->bus);
    }
    g_free(launcher);
}

const xd_launcher_state *xd_launcher_lookup(const xd_launcher *launcher, const char *key) {
    if (launcher == nullptr || key == nullptr)
        return nullptr;
    for (guint i = 0; i < launcher->entries->len; i++) {
        const entry *launcher_entry = g_ptr_array_index(launcher->entries, i);
        if (xd_launcher_key_is(launcher_entry->id, key))
            return &launcher_entry->st;
    }
    return nullptr;
}

void xd_launcher_sweep(xd_launcher *launcher, GHashTable *open) {
    if (launcher == nullptr)
        return;
    for (guint i = launcher->entries->len; i-- > 0;) {
        entry *launcher_entry = g_ptr_array_index(launcher->entries, i);
        bool has_window = false;
        GHashTableIter it;
        gpointer key = nullptr;
        g_hash_table_iter_init(&it, open);
        while (!has_window && g_hash_table_iter_next(&it, &key, nullptr))
            has_window = xd_launcher_key_is(launcher_entry->id, key);
        if (has_window)
            launcher_entry->seen = true;
        else if (launcher_entry->seen)
            forget(launcher, i);
    }
}
