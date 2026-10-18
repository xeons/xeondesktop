/* SPDX-License-Identifier: MIT
 * The portal permission store. See permissions.h.
 *
 * Tables and values are xdg-desktop-portal's own (location.c,
 * notification.c, device.c, background.c, screenshot.c, wallpaper.c);
 * GNOME Settings shows the same ones per program.
 */
#include "permissions.h"

#include <stdlib.h>
#include <string.h>

#define STORE "org.freedesktop.impl.portal.PermissionStore"
#define STORE_PATH "/org/freedesktop/impl/portal/PermissionStore"

/* A D-Bus call to the store: quick, it's a local database. */
static constexpr int CALL_TIMEOUT_MS = 5000;

const XdPermKind xd_perm_kinds[] = {
    {"location", "location", "Location", "EXACT", "NONE"},
    {"devices", "camera", "Camera", "yes", "no"},
    {"devices", "microphone", "Microphone", "yes", "no"},
    {"devices", "speakers", "Speakers", "yes", "no"},
    {"notifications", "notification", "Notifications", "yes", "no"},
    {"background", "background", "Run in the background", "yes", "no"},
    {"screenshot", "screenshot", "Screenshots", "yes", "no"},
    {"wallpaper", "wallpaper", "Change the desktop background", "yes", "no"},
};
const size_t xd_perm_kind_count = G_N_ELEMENTS(xd_perm_kinds);

gboolean xd_perm_allowed(const XdPermKind *kind, const char *const *values) {
    /* "ask": the portal asks each time (background), so not allowed. */
    return values != nullptr && values[0] != nullptr && strcmp(values[0], kind->deny) != 0 &&
           strcmp(values[0], "ask") != 0;
}

char **xd_perm_values_set(const XdPermKind *kind, const char *const *values, gboolean allow) {
    GPtrArray *out = g_ptr_array_new();
    g_ptr_array_add(out, g_strdup(allow ? kind->allow : kind->deny));
    for (size_t i = 1; values != nullptr && values[0] != nullptr && values[i] != nullptr; i++)
        g_ptr_array_add(out, g_strdup(values[i]));
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

gint64 xd_perm_last_used(const XdPermEntry *e) {
    if (strcmp(e->kind->table, "location") != 0 || e->values == nullptr ||
        e->values[0] == nullptr || e->values[1] == nullptr)
        return -1;
    char *end = nullptr;
    gint64 t = g_ascii_strtoll(e->values[1], &end, 10);
    return end != e->values[1] && *end == '\0' && t > 0 ? t : -1;
}

static void entry_free(gpointer p) {
    XdPermEntry *e = p;
    g_free(e->app);
    g_strfreev(e->values);
    g_free(e);
}

static gint by_app(gconstpointer a, gconstpointer b) {
    const XdPermEntry *x = *(XdPermEntry *const *)a, *y = *(XdPermEntry *const *)b;
    int c = strcmp(x->app, y->app);
    return c != 0 ? c : (x->kind > y->kind) - (x->kind < y->kind);
}

/* The store says a table or id it doesn't have yet is "not found". */
static gboolean not_found(const GError *error) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return TRUE;
    char *remote = g_dbus_error_get_remote_error(error);
    gboolean nf = remote != nullptr && g_str_has_suffix(remote, ".NotFound");
    g_free(remote);
    return nf;
}

GPtrArray *xd_perm_list(GDBusConnection *bus, GCancellable *cancel, GError **error) {
    GPtrArray *out = g_ptr_array_new_with_free_func(entry_free);
    for (size_t k = 0; k < xd_perm_kind_count; k++) {
        const XdPermKind *kind = &xd_perm_kinds[k];
        GError *e = nullptr;
        GVariant *r = g_dbus_connection_call_sync(
            bus, STORE, STORE_PATH, STORE, "Lookup", g_variant_new("(ss)", kind->table, kind->id),
            G_VARIANT_TYPE("(a{sas}v)"), G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, cancel, &e);
        if (r == nullptr) {
            if (not_found(e)) {
                g_error_free(e);
                continue;
            }
            g_propagate_error(error, e);
            g_ptr_array_unref(out);
            return nullptr;
        }
        GVariant *perms = g_variant_get_child_value(r, 0);
        GVariantIter it;
        g_variant_iter_init(&it, perms);
        const char *app;
        GVariant *values;
        while (g_variant_iter_next(&it, "{&s@as}", &app, &values)) {
            XdPermEntry *entry = g_new0(XdPermEntry, 1);
            entry->kind = kind;
            entry->app = g_strdup(app);
            entry->values = g_variant_dup_strv(values, nullptr);
            g_ptr_array_add(out, entry);
            g_variant_unref(values);
        }
        g_variant_unref(perms);
        g_variant_unref(r);
    }
    g_ptr_array_sort(out, by_app);
    return out;
}

gboolean xd_perm_set(GDBusConnection *bus, const XdPermEntry *e, gboolean allow,
                     GCancellable *cancel, GError **error) {
    char **values = xd_perm_values_set(e->kind, (const char *const *)e->values, allow);
    GVariant *r = g_dbus_connection_call_sync(
        bus, STORE, STORE_PATH, STORE, "SetPermission",
        g_variant_new("(sbss^as)", e->kind->table, TRUE, e->kind->id, e->app, values), nullptr,
        G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, cancel, error);
    g_strfreev(values);
    if (r == nullptr)
        return FALSE;
    g_variant_unref(r);
    return TRUE;
}

/* Without DeletePermission (stores before version 2): the id's whole
 * table entry again, less this program. */
static gboolean remove_by_set(GDBusConnection *bus, const XdPermEntry *e, GCancellable *cancel,
                              GError **error) {
    GVariant *r = g_dbus_connection_call_sync(
        bus, STORE, STORE_PATH, STORE, "Lookup", g_variant_new("(ss)", e->kind->table, e->kind->id),
        G_VARIANT_TYPE("(a{sas}v)"), G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, cancel, error);
    if (r == nullptr)
        return FALSE;
    GVariant *perms = g_variant_get_child_value(r, 0);
    GVariant *data = g_variant_get_child_value(r, 1);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sas}"));
    GVariantIter it;
    g_variant_iter_init(&it, perms);
    const char *app;
    GVariant *values;
    while (g_variant_iter_next(&it, "{&s@as}", &app, &values)) {
        if (strcmp(app, e->app) != 0)
            g_variant_builder_add(&b, "{s@as}", app, values);
        g_variant_unref(values);
    }
    GVariant *s = g_dbus_connection_call_sync(
        bus, STORE, STORE_PATH, STORE, "Set",
        g_variant_new("(sbsa{sas}@v)", e->kind->table, TRUE, e->kind->id, &b, data), nullptr,
        G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, cancel, error);
    g_variant_unref(data);
    g_variant_unref(perms);
    g_variant_unref(r);
    if (s == nullptr)
        return FALSE;
    g_variant_unref(s);
    return TRUE;
}

gboolean xd_perm_remove(GDBusConnection *bus, const XdPermEntry *e, GCancellable *cancel,
                        GError **error) {
    GError *err = nullptr;
    GVariant *r = g_dbus_connection_call_sync(
        bus, STORE, STORE_PATH, STORE, "DeletePermission",
        g_variant_new("(sss)", e->kind->table, e->kind->id, e->app), nullptr,
        G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, cancel, &err);
    if (r != nullptr) {
        g_variant_unref(r);
        return TRUE;
    }
    if (!g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
        g_propagate_error(error, err);
        return FALSE;
    }
    g_error_free(err);
    return remove_by_set(bus, e, cancel, error);
}
