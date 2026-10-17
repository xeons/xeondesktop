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
    for (const char *p = name; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (!g_unichar_isprint(c) || c == '[' || c == ']')
            return false;
    }
    /* Leading or trailing blanks would be lost in the file. */
    return !g_ascii_isspace(name[0]) && !g_ascii_isspace(name[strlen(name) - 1]);
}

static void def_free(gpointer data) {
    xd_power_scheme_def *d = data;
    g_free(d->name);
    g_free(d);
}

static xd_power_scheme_def *def_new(const char *name, xd_power_timers ac, xd_power_timers dc) {
    xd_power_scheme_def *d = g_new0(xd_power_scheme_def, 1);
    d->name = g_strdup(name);
    d->ac = ac;
    d->dc = dc;
    d->builtin = builtin_index(name);
    return d;
}

/* A timer from the file; out of range or missing keeps `*out`. */
static void read_timer(GKeyFile *kf, const char *group, const char *key, int *out) {
    GError *e = nullptr;
    int v = g_key_file_get_integer(kf, group, key, &e);
    if (e == nullptr && v >= 0 && v <= 1440)
        *out = v;
    g_clear_error(&e);
}

static void read_timers(GKeyFile *kf, const char *group, xd_power_scheme_def *d) {
    int *t[2][3] = {{&d->ac.monitor, &d->ac.standby, &d->ac.hibernate},
                    {&d->dc.monitor, &d->dc.standby, &d->dc.hibernate}};
    for (size_t i = 0; i < 2; i++)
        for (size_t j = 0; j < 3; j++)
            read_timer(kf, group, keys[i][j], t[i][j]);
}

GPtrArray *xd_power_schemes_load(const char *path) {
    GPtrArray *out = g_ptr_array_new_with_free_func(def_free);
    GKeyFile *kf = g_key_file_new();
    bool have = path != nullptr && g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr);
    for (int i = 0; i < XD_POWER_SCHEME_COUNT; i++) {
        const char *label = builtins[i].label;
        if (have && g_key_file_get_boolean(kf, label, "Deleted", nullptr))
            continue;
        xd_power_scheme_def *d = def_new(label, builtins[i].ac, builtins[i].dc);
        if (have && g_key_file_has_group(kf, label))
            read_timers(kf, label, d);
        g_ptr_array_add(out, d);
    }
    gchar **groups = have ? g_key_file_get_groups(kf, nullptr) : nullptr;
    for (gchar **g = groups; g != nullptr && *g != nullptr; g++) {
        if (builtin_index(*g) >= 0 || !xd_power_scheme_name_valid(*g) ||
            g_key_file_get_boolean(kf, *g, "Deleted", nullptr))
            continue;
        xd_power_scheme_def *d = def_new(*g, (xd_power_timers){0}, (xd_power_timers){0});
        read_timers(kf, *g, d);
        g_ptr_array_add(out, d);
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
        const xd_power_scheme_def *d = g_ptr_array_index(schemes, i);
        const int t[2][3] = {{d->ac.monitor, d->ac.standby, d->ac.hibernate},
                             {d->dc.monitor, d->dc.standby, d->dc.hibernate}};
        for (size_t a = 0; a < 2; a++)
            for (size_t b = 0; b < 3; b++)
                g_key_file_set_integer(kf, d->name, keys[a][b], t[a][b]);
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
        xd_power_scheme_def *d = g_ptr_array_index(schemes, i);
        if (strcmp(d->name, name) == 0)
            return d;
    }
    return nullptr;
}

xd_power_scheme_def *xd_power_schemes_put(GPtrArray *schemes, const char *name,
                                          xd_power_timers ac, xd_power_timers dc) {
    xd_power_scheme_def *d = xd_power_schemes_find(schemes, name);
    if (d == nullptr) {
        d = def_new(name, ac, dc);
        g_ptr_array_add(schemes, d);
    }
    d->ac = ac;
    d->dc = dc;
    return d;
}
