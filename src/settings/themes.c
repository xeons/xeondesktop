/* SPDX-License-Identifier: MIT */
#include <glib/gstdio.h>
#include <string.h>

#include "gtktheme.h"
#include "themes.h"

static char **search_path(const char *sub, const char *home_dir) {
    GPtrArray *dirs = g_ptr_array_new();
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), sub, nullptr));
    g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), home_dir, nullptr));
    for (const char *const *d = g_get_system_data_dirs(); *d != nullptr; d++)
        g_ptr_array_add(dirs, g_build_filename(*d, sub, nullptr));
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

char **xd_icon_theme_dirs(void) { return search_path("icons", ".icons"); }

char **xd_gtk_theme_dirs(void) { return search_path("themes", ".themes"); }

static void theme_free(gpointer p) {
    xd_installed_theme *t = p;
    g_free(t->dir);
    g_free(t->name);
    g_free(t);
}

static int by_name(gconstpointer a, gconstpointer b) {
    const xd_installed_theme *x = *(xd_installed_theme *const *)a;
    const xd_installed_theme *y = *(xd_installed_theme *const *)b;
    return g_utf8_collate(x->name, y->name);
}

static void add(GPtrArray *list, GHashTable *seen, const char *dir, char *name) {
    g_hash_table_add(seen, g_strdup(dir));
    xd_installed_theme *t = g_new0(xd_installed_theme, 1);
    t->dir = g_strdup(dir);
    t->name = name != nullptr ? name : g_strdup(dir);
    g_ptr_array_add(list, t);
}

/* An icon theme's Name, or nullptr if `path` isn't one a user would pick:
 * no index.theme, Hidden, or only cursors (cursor themes have an
 * index.theme too, often without any Directories). */
static char *icon_theme_name(const char *path, const char *dir) {
    char *index = g_build_filename(path, "index.theme", nullptr);
    GKeyFile *kf = g_key_file_new();
    char *name = nullptr;
    if (g_key_file_load_from_file(kf, index, G_KEY_FILE_NONE, nullptr)) {
        bool icons = false;
        static const char *const keys[] = {"Directories", "ScaledDirectories"};
        for (size_t k = 0; k < G_N_ELEMENTS(keys) && !icons; k++) {
            char **subdirs = g_key_file_get_string_list(kf, "Icon Theme", keys[k], nullptr,
                                                        nullptr);
            for (char **s = subdirs; s != nullptr && *s != nullptr && !icons; s++)
                icons = (*s)[0] != '\0' && strcmp(*s, "cursors") != 0;
            g_strfreev(subdirs);
        }
        /* Adwaita hides itself from pickers, but it's GTK's own default. */
        bool hidden = g_key_file_get_boolean(kf, "Icon Theme", "Hidden", nullptr) &&
                      strcmp(dir, "Adwaita") != 0;
        if (icons && !hidden) {
            name = g_key_file_get_locale_string(kf, "Icon Theme", "Name", nullptr, nullptr);
            if (name == nullptr || name[0] == '\0') {
                g_free(name);
                name = g_strdup(dir);
            }
        }
    }
    g_key_file_free(kf);
    g_free(index);
    return name;
}

GPtrArray *xd_icon_themes_list(const char *const *dirs) {
    GPtrArray *list = g_ptr_array_new_with_free_func(theme_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (; dirs != nullptr && *dirs != nullptr; dirs++) {
        GDir *d = g_dir_open(*dirs, 0, nullptr);
        if (d == nullptr)
            continue;
        const char *entry;
        while ((entry = g_dir_read_name(d)) != nullptr) {
            /* "default" only points at another theme (Inherits=). */
            if (entry[0] == '.' || strcmp(entry, "default") == 0 ||
                g_hash_table_contains(seen, entry))
                continue;
            char *path = g_build_filename(*dirs, entry, nullptr);
            char *name = icon_theme_name(path, entry);
            if (name != nullptr)
                add(list, seen, entry, name);
            g_free(path);
        }
        g_dir_close(d);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_sort(list, by_name);
    return list;
}

GPtrArray *xd_gtk_themes_list(const char *const *dirs) {
    GPtrArray *list = g_ptr_array_new_with_free_func(theme_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    /* Compiled into GTK3 itself; a theme directory of the same name
     * would only be found after these. */
    add(list, seen, "Adwaita", nullptr);
    add(list, seen, "HighContrast", nullptr);
    g_hash_table_add(seen, g_strdup(XD_GTK_THEME_NAME));
    g_hash_table_add(seen, g_strdup(XD_GTK_THEME_ALT_NAME));
    for (; dirs != nullptr && *dirs != nullptr; dirs++) {
        GDir *d = g_dir_open(*dirs, 0, nullptr);
        if (d == nullptr)
            continue;
        const char *entry;
        while ((entry = g_dir_read_name(d)) != nullptr) {
            if (entry[0] == '.' || g_hash_table_contains(seen, entry))
                continue;
            char *css = g_build_filename(*dirs, entry, "gtk-3.0", "gtk.css", nullptr);
            if (g_file_test(css, G_FILE_TEST_IS_REGULAR))
                add(list, seen, entry, nullptr);
            g_free(css);
        }
        g_dir_close(d);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_sort(list, by_name);
    return list;
}

/* What `argv` prints on its first line, if it runs. */
static char *query(const char *const argv[static 4]) {
    char *prog = g_find_program_in_path(argv[0]);
    if (prog == nullptr)
        return nullptr;
    /* g_spawn_sync wants them writable. */
    char *args[4] = {prog};
    for (size_t i = 1; i < G_N_ELEMENTS(args) - 1 && argv[i] != nullptr; i++)
        args[i] = g_strdup(argv[i]);
    char *out = nullptr;
    int status = 0;
    if (!g_spawn_sync(nullptr, args, nullptr, G_SPAWN_STDERR_TO_DEV_NULL, nullptr, nullptr,
                      &out, nullptr, &status, nullptr) ||
        !g_spawn_check_wait_status(status, nullptr)) {
        g_clear_pointer(&out, g_free);
    }
    for (size_t i = 0; args[i] != nullptr; i++)
        g_free(args[i]);
    if (out != nullptr)
        out[strcspn(out, "\n")] = '\0';
    return out;
}

static void add_dir(GPtrArray *dirs, char *dir) {
    bool dup = dir == nullptr || dir[0] != '/' || !g_file_test(dir, G_FILE_TEST_IS_DIR);
    for (guint i = 0; !dup && i < dirs->len; i++)
        dup = strcmp(g_ptr_array_index(dirs, i), dir) == 0;
    if (dup)
        g_free(dir);
    else
        g_ptr_array_add(dirs, dir);
}

char **xd_qt_plugin_dirs(void) {
    GPtrArray *dirs = g_ptr_array_new();
    const char *env = g_getenv("QT_PLUGIN_PATH");
    if (env != nullptr) {
        char **parts = g_strsplit(env, ":", -1);
        for (char **p = parts; *p != nullptr; p++)
            add_dir(dirs, g_strdup(*p));
        g_strfreev(parts);
    }
    /* Qt 6 then Qt 5, each from the first tool that answers. */
    static const char *const qt6[][4] = {
        {"qtpaths6", "--query", "QT_INSTALL_PLUGINS", nullptr},
        {"qmake6", "-query", "QT_INSTALL_PLUGINS", nullptr},
    };
    static const char *const qt5[][4] = {
        {"qmake-qt5", "-query", "QT_INSTALL_PLUGINS", nullptr},
        {"qtpaths-qt5", "--plugin-dir", nullptr, nullptr},
    };
    char *dir = nullptr;
    for (size_t i = 0; i < G_N_ELEMENTS(qt6) && dir == nullptr; i++)
        dir = query(qt6[i]);
    add_dir(dirs, dir);
    dir = nullptr;
    for (size_t i = 0; i < G_N_ELEMENTS(qt5) && dir == nullptr; i++)
        dir = query(qt5[i]);
    add_dir(dirs, dir);
    /* Arch, Debian/Ubuntu (x86-64), Fedora. */
    static const char *const known[] = {
        "/usr/lib/qt6/plugins",        "/usr/lib/x86_64-linux-gnu/qt6/plugins",
        "/usr/lib64/qt6/plugins",      "/usr/lib/qt/plugins",
        "/usr/lib/x86_64-linux-gnu/qt5/plugins", "/usr/lib64/qt5/plugins",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(known); i++)
        add_dir(dirs, g_strdup(known[i]));
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

/* A style plugin's key from its file name: no "lib" or ".so", no Qt
 * version suffix (breeze6, oxygen6), the first letter capitalized as most
 * keys are. nullptr for what isn't a plugin. */
static char *qt_style_key(const char *file) {
    if (!g_str_has_suffix(file, ".so"))
        return nullptr;
    const char *start = g_str_has_prefix(file, "lib") ? file + 3 : file;
    size_t len = strlen(start) - 3;
    if (len >= 2 && (start[len - 1] == '5' || start[len - 1] == '6') &&
        g_ascii_isalpha(start[len - 2]))
        len--;
    if (len == 0)
        return nullptr;
    char *key = g_strndup(start, len);
    key[0] = g_ascii_toupper(key[0]);
    return key;
}

static int by_key(gconstpointer a, gconstpointer b) {
    const xd_installed_theme *x = *(xd_installed_theme *const *)a;
    const xd_installed_theme *y = *(xd_installed_theme *const *)b;
    return g_ascii_strcasecmp(x->dir, y->dir);
}

GPtrArray *xd_qt_styles_list(const char *const *plugin_dirs) {
    GPtrArray *list = g_ptr_array_new_with_free_func(theme_free);
    /* Keys are compared without case, as Qt does. */
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    static const char *const builtin[] = {"Windows", "Fusion"};
    for (size_t i = 0; i < G_N_ELEMENTS(builtin); i++) {
        g_hash_table_add(seen, g_ascii_strdown(builtin[i], -1));
        add(list, seen, builtin[i], nullptr);
    }
    guint first = list->len;
    for (; plugin_dirs != nullptr && *plugin_dirs != nullptr; plugin_dirs++) {
        char *styles = g_build_filename(*plugin_dirs, "styles", nullptr);
        GDir *d = g_dir_open(styles, 0, nullptr);
        g_free(styles);
        if (d == nullptr)
            continue;
        const char *entry;
        while ((entry = g_dir_read_name(d)) != nullptr) {
            char *key = qt_style_key(entry);
            if (key == nullptr)
                continue;
            char *lower = g_ascii_strdown(key, -1);
            if (!g_hash_table_contains(seen, lower)) {
                g_hash_table_add(seen, g_strdup(lower));
                add(list, seen, key, nullptr);
            }
            g_free(lower);
            g_free(key);
        }
        g_dir_close(d);
    }
    g_hash_table_destroy(seen);
    /* The built-in ones stay first. */
    if (list->len > first) {
        xd_installed_theme **items = (xd_installed_theme **)list->pdata + first;
        qsort(items, list->len - first, sizeof(*items), by_key);
    }
    return list;
}

bool xd_gtk_ini_set(const char *path, const char *key, const char *value) {
    GKeyFile *kf = g_key_file_new();
    GError *err = nullptr;
    bool ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, &err);
    if (!ok && g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        ok = true; /* a new file */
        if (value == nullptr) {
            g_clear_error(&err);
            g_key_file_free(kf);
            return true; /* nothing to remove */
        }
    }
    g_clear_error(&err);
    /* A file GKeyFile can't read isn't overwritten: it's the user's. */
    if (ok) {
        if (value != nullptr)
            g_key_file_set_string(kf, "Settings", key, value);
        else
            g_key_file_remove_key(kf, "Settings", key, nullptr);
        char *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
        ok = g_key_file_save_to_file(kf, path, &err);
        if (!ok)
            g_printerr("xeondesktop: cannot write %s: %s\n", path, err->message);
        g_clear_error(&err);
    }
    g_key_file_free(kf);
    return ok;
}

/* org.gnome.desktop.interface's `key`, where it exists: GTK3 and GTK4 on
 * Wayland read these over their settings.ini files, and so do portals and
 * the programs they serve. */
static void gsettings_set(GSettingsBackend *backend, const char *key, const char *value) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src != nullptr ? g_settings_schema_source_lookup(
                                                   src, "org.gnome.desktop.interface", TRUE)
                                             : nullptr;
    if (schema == nullptr)
        return;
    if (g_settings_schema_has_key(schema, key)) {
        GSettings *s = g_settings_new_full(schema, backend, nullptr);
        g_settings_set_string(s, key, value);
        g_object_unref(s);
        g_settings_sync();
    }
    g_settings_schema_unref(schema);
}

void xd_gtk_settings_export(const char *config_dir, GSettingsBackend *backend,
                            const char *gtk_theme, const char *icon_theme) {
    char *gtk3 = g_build_filename(config_dir, "gtk-3.0", "settings.ini", nullptr);
    char *gtk4 = g_build_filename(config_dir, "gtk-4.0", "settings.ini", nullptr);
    if (gtk_theme != nullptr) {
        xd_gtk_ini_set(gtk3, "gtk-theme-name", gtk_theme);
        /* Ours is GTK3 CSS only: GTK4 keeps its own default. */
        bool ours = strcmp(gtk_theme, XD_GTK_THEME_NAME) == 0;
        xd_gtk_ini_set(gtk4, "gtk-theme-name", ours ? nullptr : gtk_theme);
        gsettings_set(backend, "gtk-theme", gtk_theme);
    }
    if (icon_theme != nullptr) {
        xd_gtk_ini_set(gtk3, "gtk-icon-theme-name", icon_theme);
        xd_gtk_ini_set(gtk4, "gtk-icon-theme-name", icon_theme);
        gsettings_set(backend, "icon-theme", icon_theme);
    }
    g_free(gtk4);
    g_free(gtk3);
}
