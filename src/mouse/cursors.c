/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "cursors.h"

char **xd_cursor_theme_dirs(void) {
    GPtrArray *dirs = g_ptr_array_new();
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "icons", nullptr));
    g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".icons", nullptr));
    for (const char *const *d = g_get_system_data_dirs(); *d != nullptr; d++)
        g_ptr_array_add(dirs, g_build_filename(*d, "icons", nullptr));
    g_ptr_array_add(dirs, g_strdup("/usr/share/pixmaps"));
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

static void theme_free(gpointer p) {
    xd_cursor_theme *t = p;
    g_free(t->dir);
    g_free(t->name);
    g_free(t);
}

static int by_name(gconstpointer a, gconstpointer b) {
    const xd_cursor_theme *x = *(xd_cursor_theme *const *)a;
    const xd_cursor_theme *y = *(xd_cursor_theme *const *)b;
    return g_utf8_collate(x->name, y->name);
}

/* index.theme's Name, if it has one. */
static char *theme_name(const char *path) {
    char *index = g_build_filename(path, "index.theme", nullptr);
    GKeyFile *kf = g_key_file_new();
    char *name = nullptr;
    if (g_key_file_load_from_file(kf, index, G_KEY_FILE_NONE, nullptr))
        name = g_key_file_get_locale_string(kf, "Icon Theme", "Name", nullptr, nullptr);
    g_key_file_free(kf);
    g_free(index);
    if (name != nullptr && name[0] == '\0')
        g_clear_pointer(&name, g_free);
    return name;
}

GPtrArray *xd_cursor_themes_list(const char *const *dirs) {
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
            char *cursors = g_build_filename(path, "cursors", nullptr);
            if (g_file_test(cursors, G_FILE_TEST_IS_DIR)) {
                g_hash_table_add(seen, g_strdup(entry));
                xd_cursor_theme *t = g_new0(xd_cursor_theme, 1);
                t->dir = g_strdup(entry);
                t->name = theme_name(path);
                if (t->name == nullptr)
                    t->name = g_strdup(entry);
                g_ptr_array_add(list, t);
            }
            g_free(cursors);
            g_free(path);
        }
        g_dir_close(d);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_sort(list, by_name);
    return list;
}
