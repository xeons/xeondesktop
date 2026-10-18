/* SPDX-License-Identifier: MIT
 * User services: the systemd user manager (org.freedesktop.systemd1 on the
 * session bus), and the list of services turned off here.
 */
#include <string.h>

#include "startup.h"

#define SYSTEMD "org.freedesktop.systemd1"
#define SYSTEMD_PATH "/org/freedesktop/systemd1"
#define MANAGER SYSTEMD ".Manager"
#define UNIT_IFACE SYSTEMD ".Unit"

static constexpr int TIMEOUT_MS = 15000;

/* The targets a session starts: what's enabled into them starts at log on. */
static const char *const session_targets[] = {
    "default.target.wants",
    "graphical-session.target.wants",
    "xeondesktop-session.target.wants",
};

void xd_user_service_free(xd_user_service *s) {
    if (s == nullptr)
        return;
    g_free(s->unit);
    g_free(s->description);
    g_free(s);
}

bool xd_service_links_start_session(const char *const *links, const char *user_unit_dir,
                                    bool *global) {
    bool starts = false;
    bool mine = false;
    for (size_t i = 0; links != nullptr && links[i] != nullptr; i++) {
        char *dir = g_path_get_dirname(links[i]);
        char *wants = g_path_get_basename(dir);
        char *top = g_path_get_dirname(dir);
        bool session = false;
        for (size_t t = 0; t < G_N_ELEMENTS(session_targets); t++)
            session = session || strcmp(wants, session_targets[t]) == 0;
        if (session) {
            starts = true;
            mine = mine || (user_unit_dir != nullptr && strcmp(top, user_unit_dir) == 0);
        }
        g_free(top);
        g_free(wants);
        g_free(dir);
    }
    if (global != nullptr)
        *global = starts && !mine;
    return starts;
}

static GVariant *call(GDBusConnection *bus, const char *method, GVariant *args, const char *reply,
                      GError **error) {
    return g_dbus_connection_call_sync(bus, SYSTEMD, SYSTEMD_PATH, MANAGER, method, args,
                                       reply != nullptr ? G_VARIANT_TYPE(reply) : nullptr,
                                       G_DBUS_CALL_FLAGS_NONE, TIMEOUT_MS, nullptr, error);
}

/* A unit's string property, or nullptr. g_free. */
static char *unit_string(GDBusConnection *bus, const char *object, const char *prop) {
    GVariant *r = g_dbus_connection_call_sync(
        bus, SYSTEMD, object, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", UNIT_IFACE, prop), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
        TIMEOUT_MS, nullptr, nullptr);
    if (r == nullptr)
        return nullptr;
    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    char *s = g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string(v, nullptr)
                                                             : nullptr;
    g_variant_unref(v);
    g_variant_unref(r);
    return s;
}

static xd_user_service *service(GDBusConnection *bus, const char *unit, bool enabled,
                                bool global) {
    xd_user_service *s = g_new0(xd_user_service, 1);
    s->unit = g_strdup(unit);
    s->enabled = enabled;
    s->global = global;
    /* LoadUnit only reads the unit in: nothing starts. */
    GVariant *r = call(bus, "LoadUnit", g_variant_new("(s)", unit), "(o)", nullptr);
    if (r != nullptr) {
        const char *object = nullptr;
        g_variant_get(r, "(&o)", &object);
        s->description = unit_string(bus, object, "Description");
        char *active = unit_string(bus, object, "ActiveState");
        s->running = active != nullptr && (strcmp(active, "active") == 0 ||
                                           strcmp(active, "reloading") == 0 ||
                                           strcmp(active, "activating") == 0);
        g_free(active);
        g_variant_unref(r);
    }
    if (s->description == nullptr || s->description[0] == '\0') {
        g_free(s->description);
        s->description = g_strdup(unit);
    }
    return s;
}

static int by_description(gconstpointer a, gconstpointer b) {
    const xd_user_service *x = *(const xd_user_service *const *)a;
    const xd_user_service *y = *(const xd_user_service *const *)b;
    int c = g_utf8_collate(x->description, y->description);
    return c != 0 ? c : strcmp(x->unit, y->unit);
}

GPtrArray *xd_user_services_list(GDBusConnection *bus, const char *const *remembered,
                                 GError **error) {
    const char *patterns[] = {"*.service", nullptr};
    GVariant *r = call(bus, "ListUnitFilesByPatterns",
                       g_variant_new("(^as^as)", (const char *const[]){nullptr}, patterns),
                       "(a(ss))", error);
    if (r == nullptr)
        return nullptr;
    char *user_unit_dir = g_build_filename(g_get_user_config_dir(), "systemd", "user", nullptr);
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)xd_user_service_free);
    GVariantIter *it = nullptr;
    g_variant_get(r, "(a(ss))", &it);
    const char *path = nullptr;
    const char *state = nullptr;
    while (g_variant_iter_next(it, "(&s&s)", &path, &state)) {
        char *unit = g_path_get_basename(path);
        /* Templates aren't started by themselves; their instances are. */
        bool tmpl = g_str_has_suffix(unit, "@.service");
        if (!tmpl && strcmp(state, "enabled") == 0) {
            GVariant *l = call(bus, "GetUnitFileLinks", g_variant_new("(sb)", unit, FALSE),
                               "(as)", nullptr);
            if (l != nullptr) {
                const char **links = nullptr;
                g_variant_get(l, "(^a&s)", &links);
                bool global = false;
                if (xd_service_links_start_session(links, user_unit_dir, &global))
                    g_ptr_array_add(list, service(bus, unit, true, global));
                g_free(links);
                g_variant_unref(l);
            }
        } else if (!tmpl && strcmp(state, "disabled") == 0 && remembered != nullptr &&
                   g_strv_contains(remembered, unit)) {
            g_ptr_array_add(list, service(bus, unit, false, false));
        }
        g_free(unit);
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    g_free(user_unit_dir);
    g_ptr_array_sort(list, by_description);
    return list;
}

bool xd_user_service_set_enabled(GDBusConnection *bus, const char *unit, bool enabled,
                                 GError **error) {
    const char *files[] = {unit, nullptr};
    GVariant *r = enabled ? call(bus, "EnableUnitFiles", g_variant_new("(^asbb)", files, FALSE,
                                                                        FALSE),
                                 "(ba(sss))", error)
                          : call(bus, "DisableUnitFiles", g_variant_new("(^asb)", files, FALSE),
                                 "(a(sss))", error);
    if (r == nullptr)
        return false;
    GVariant *changes = g_variant_get_child_value(r, enabled ? 1 : 0);
    size_t n = g_variant_n_children(changes);
    g_variant_unref(changes);
    g_variant_unref(r);
    /* Nothing of yours to unlink: it's enabled where only root can change it. */
    if (!enabled && n == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "%s starts for every user, so only an administrator can turn it off.", unit);
        return false;
    }
    /* As systemctl does after enabling or disabling. */
    GVariant *done = call(bus, "Reload", nullptr, nullptr, error);
    if (done == nullptr)
        return false;
    g_variant_unref(done);
    return true;
}

/* ---- remembered ------------------------------------------------------------------ */

#define REMEMBER_GROUP "Services"
#define REMEMBER_KEY "Disabled"

char **xd_startup_disabled_load(const char *path) {
    GKeyFile *kf = g_key_file_new();
    char **units = nullptr;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr))
        units = g_key_file_get_string_list(kf, REMEMBER_GROUP, REMEMBER_KEY, nullptr, nullptr);
    g_key_file_free(kf);
    return units != nullptr ? units : g_new0(char *, 1);
}

bool xd_startup_disabled_save(const char *path, const char *const *units, GError **error) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    size_t n = 0;
    while (units != nullptr && units[n] != nullptr)
        n++;
    if (n == 0)
        g_key_file_remove_key(kf, REMEMBER_GROUP, REMEMBER_KEY, nullptr);
    else
        g_key_file_set_string_list(kf, REMEMBER_GROUP, REMEMBER_KEY, units, n);
    char *dir = g_path_get_dirname(path);
    bool ok = g_mkdir_with_parents(dir, 0700) == 0;
    if (!ok)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Cannot create %s.", dir);
    else
        ok = g_key_file_save_to_file(kf, path, error);
    g_free(dir);
    g_key_file_free(kf);
    return ok;
}
