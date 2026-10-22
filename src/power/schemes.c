/* SPDX-License-Identifier: MIT */
#include <string.h>

#include <gio/gio.h>

#include "schemes.h"

/* XP's power schemes (plugged in; running on batteries), without the
 * hard disk timers Linux has no counterpart for. */
static const struct {
    const char *label;
    xd_power_timers ac, dc;
} builtins[XD_POWER_SCHEME_COUNT] = {
    [XD_POWER_HOME] = {"Home/Office Desk", {20, 0, 0}, {5, 20, 0}},
    [XD_POWER_PORTABLE] = {"Portable/Laptop", {15, 20, 180}, {5, 5, 120}},
    [XD_POWER_PRESENTATION] = {"Presentation", {0, 0, 0}, {0, 15, 120}},
    [XD_POWER_ALWAYS_ON] = {"Always On", {20, 0, 0}, {15, 0, 0}},
    [XD_POWER_MINIMAL] = {"Minimal Power Management", {15, 0, 0}, {5, 15, 120}},
    [XD_POWER_MAX_BATTERY] = {"Max Battery", {15, 20, 45}, {1, 2, 45}},
};

static const char *const keys[2][3] = {
    {"MonitorOff", "Standby", "Hibernate"},
    {"MonitorOffOnBattery", "StandbyOnBattery", "HibernateOnBattery"},
};

const char *xd_power_builtin_label(xd_power_scheme scheme) {
    return (unsigned)scheme < XD_POWER_SCHEME_COUNT ? builtins[scheme].label : "";
}

static int builtin_index(const char *name) {
    for (int i = 0; i < XD_POWER_SCHEME_COUNT; i++)
        if (strcmp(builtins[i].label, name) == 0)
            return i;
    return -1;
}

bool xd_power_scheme_name_valid(const char *name) {
    if (name == nullptr || name[0] == '\0' || strlen(name) >= 64 ||
        !g_utf8_validate(name, -1, nullptr))
        return false;
    for (const char *cursor = name; *cursor != '\0'; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (!g_unichar_isprint(character) || character == '[' || character == ']')
            return false;
    }
    /* Leading or trailing blanks would be lost in the file. */
    return !g_ascii_isspace(name[0]) && !g_ascii_isspace(name[strlen(name) - 1]);
}

static void def_free(gpointer data) {
    xd_power_scheme_def *scheme = data;
    g_free(scheme->name);
    g_free(scheme);
}

static xd_power_scheme_def *def_new(const char *name, xd_power_timers ac, xd_power_timers dc) {
    xd_power_scheme_def *scheme = g_new0(xd_power_scheme_def, 1);
    scheme->name = g_strdup(name);
    scheme->ac = ac;
    scheme->dc = dc;
    scheme->builtin = builtin_index(name);
    return scheme;
}

/* A timer from the file; out of range or missing keeps `*out`. */
static void read_timer(GKeyFile *kf, const char *group, const char *key, int *out) {
    GError *error = nullptr;
    int minute_count = g_key_file_get_integer(kf, group, key, &error);
    if (error == nullptr && minute_count >= 0 && minute_count <= 1440)
        *out = minute_count;
    g_clear_error(&error);
}

static void read_timers(GKeyFile *kf, const char *group, xd_power_scheme_def *scheme) {
    int *timers[2][3] = {{&scheme->ac.monitor, &scheme->ac.standby, &scheme->ac.hibernate},
                         {&scheme->dc.monitor, &scheme->dc.standby, &scheme->dc.hibernate}};
    for (size_t i = 0; i < 2; i++)
        for (size_t j = 0; j < 3; j++)
            read_timer(kf, group, keys[i][j], timers[i][j]);
}

GPtrArray *xd_power_schemes_load(const char *path) {
    GPtrArray *out = g_ptr_array_new_with_free_func(def_free);
    GKeyFile *kf = g_key_file_new();
    bool have = path != nullptr && g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr);
    for (int i = 0; i < XD_POWER_SCHEME_COUNT; i++) {
        const char *label = builtins[i].label;
        if (have && g_key_file_get_boolean(kf, label, "Deleted", nullptr))
            continue;
        xd_power_scheme_def *scheme = def_new(label, builtins[i].ac, builtins[i].dc);
        if (have && g_key_file_has_group(kf, label))
            read_timers(kf, label, scheme);
        g_ptr_array_add(out, scheme);
    }
    gchar **groups = have ? g_key_file_get_groups(kf, nullptr) : nullptr;
    for (gchar **group_name = groups; group_name != nullptr && *group_name != nullptr;
         group_name++) {
        if (builtin_index(*group_name) >= 0 || !xd_power_scheme_name_valid(*group_name) ||
            g_key_file_get_boolean(kf, *group_name, "Deleted", nullptr))
            continue;
        xd_power_scheme_def *scheme =
            def_new(*group_name, (xd_power_timers){0}, (xd_power_timers){0});
        read_timers(kf, *group_name, scheme);
        g_ptr_array_add(out, scheme);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
    return out;
}

bool xd_power_schemes_save(const char *path, GPtrArray *schemes, GError **error) {
    GKeyFile *kf = g_key_file_new();
    for (int i = 0; i < XD_POWER_SCHEME_COUNT; i++)
        if (xd_power_schemes_find(schemes, builtins[i].label) == nullptr)
            g_key_file_set_boolean(kf, builtins[i].label, "Deleted", TRUE);
    for (guint i = 0; i < schemes->len; i++) {
        const xd_power_scheme_def *scheme = g_ptr_array_index(schemes, i);
        const int timers[2][3] = {{scheme->ac.monitor, scheme->ac.standby, scheme->ac.hibernate},
                                  {scheme->dc.monitor, scheme->dc.standby, scheme->dc.hibernate}};
        for (size_t supply = 0; supply < 2; supply++)
            for (size_t timer = 0; timer < 3; timer++)
                g_key_file_set_integer(kf, scheme->name, keys[supply][timer],
                                       timers[supply][timer]);
    }
    char *dir = g_path_get_dirname(path);
    bool ok = g_mkdir_with_parents(dir, 0755) == 0;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot create %s", dir);
    else
        ok = g_key_file_save_to_file(kf, path, error);
    g_free(dir);
    g_key_file_free(kf);
    return ok;
}

xd_power_scheme_def *xd_power_schemes_find(GPtrArray *schemes, const char *name) {
    for (guint i = 0; name != nullptr && i < schemes->len; i++) {
        xd_power_scheme_def *scheme = g_ptr_array_index(schemes, i);
        if (strcmp(scheme->name, name) == 0)
            return scheme;
    }
    return nullptr;
}

xd_power_scheme_def *xd_power_schemes_put(GPtrArray *schemes, const char *name,
                                          xd_power_timers ac, xd_power_timers dc) {
    xd_power_scheme_def *scheme = xd_power_schemes_find(schemes, name);
    if (scheme == nullptr) {
        scheme = def_new(name, ac, dc);
        g_ptr_array_add(schemes, scheme);
    }
    scheme->ac = ac;
    scheme->dc = dc;
    return scheme;
}
