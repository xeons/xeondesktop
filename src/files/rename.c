/* SPDX-License-Identifier: MIT
 * Renaming as Windows renames: a shortcut (launcher) keeps its file and
 * gets a new Name. GIO only, for xd-files and the shell's desktop icons.
 */
#include "rename.h"

#define DESKTOP_GROUP "Desktop Entry"

static GKeyFile *load_launcher(GFile *file, char **path_out) {
    char *path = g_file_get_path(file);
    if (path == nullptr || !g_str_has_suffix(path, ".desktop")) {
        g_free(path);
        return nullptr;
    }
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path,
                                   G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                   nullptr) ||
        !g_key_file_has_group(kf, DESKTOP_GROUP)) {
        g_key_file_unref(kf);
        g_free(path);
        return nullptr;
    }
    *path_out = path;
    return kf;
}

char *xd_rename_text(GFile *file) {
    char *path = nullptr;
    GKeyFile *kf = load_launcher(file, &path);
    char *name = kf != nullptr ? g_key_file_get_locale_string(kf, DESKTOP_GROUP, "Name", nullptr,
                                                              nullptr)
                               : nullptr;
    if (kf != nullptr)
        g_key_file_unref(kf);
    g_free(path);
    if (name != nullptr)
        return name;
    char *base = g_file_get_basename(file);
    char *shown = g_filename_display_name(base);
    g_free(base);
    return shown;
}

GFile *xd_rename(GFile *file, const char *text, GError **err) {
    char *path = nullptr;
    GKeyFile *kf = load_launcher(file, &path);
    if (kf == nullptr)
        return g_file_set_display_name(file, text, nullptr, err);
    /* The new Name replaces every translation of the old one. */
    gsize n = 0;
    char **keys = g_key_file_get_keys(kf, DESKTOP_GROUP, &n, nullptr);
    for (gsize i = 0; keys != nullptr && i < n; i++)
        if (g_str_has_prefix(keys[i], "Name["))
            g_key_file_remove_key(kf, DESKTOP_GROUP, keys[i], nullptr);
    g_strfreev(keys);
    g_key_file_set_string(kf, DESKTOP_GROUP, "Name", text);
    gboolean ok = g_key_file_save_to_file(kf, path, err);
    g_key_file_unref(kf);
    g_free(path);
    return ok ? g_object_ref(file) : nullptr;
}
