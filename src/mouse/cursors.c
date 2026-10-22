/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "cursors.h"

char **xd_cursor_theme_dirs(void) {
    GPtrArray *dirs = g_ptr_array_new();
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "icons", nullptr));
    g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".icons", nullptr));
    for (const char *const *dir = g_get_system_data_dirs(); *dir != nullptr; dir++)
        g_ptr_array_add(dirs, g_build_filename(*dir, "icons", nullptr));
    g_ptr_array_add(dirs, g_strdup("/usr/share/pixmaps"));
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

static void theme_free(gpointer user_data) {
    xd_cursor_theme *cursor_theme = user_data;
    g_free(cursor_theme->dir);
    g_free(cursor_theme->name);
    g_free(cursor_theme);
}

static int by_name(gconstpointer left, gconstpointer right) {
    const xd_cursor_theme *left_theme = *(xd_cursor_theme *const *)left;
    const xd_cursor_theme *right_theme = *(xd_cursor_theme *const *)right;
    return g_utf8_collate(left_theme->name, right_theme->name);
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
        GDir *directory = g_dir_open(*dirs, 0, nullptr);
        if (directory == nullptr)
            continue;
        const char *entry;
        while ((entry = g_dir_read_name(directory)) != nullptr) {
            /* "default" only points at another theme (Inherits=). */
            if (entry[0] == '.' || strcmp(entry, "default") == 0 ||
                g_hash_table_contains(seen, entry))
                continue;
            char *path = g_build_filename(*dirs, entry, nullptr);
            char *cursors = g_build_filename(path, "cursors", nullptr);
            if (g_file_test(cursors, G_FILE_TEST_IS_DIR)) {
                g_hash_table_add(seen, g_strdup(entry));
                xd_cursor_theme *cursor_theme = g_new0(xd_cursor_theme, 1);
                cursor_theme->dir = g_strdup(entry);
                cursor_theme->name = theme_name(path);
                if (cursor_theme->name == nullptr)
                    cursor_theme->name = g_strdup(entry);
                g_ptr_array_add(list, cursor_theme);
            }
            g_free(cursors);
            g_free(path);
        }
        g_dir_close(directory);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_sort(list, by_name);
    return list;
}
