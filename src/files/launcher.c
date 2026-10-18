/* SPDX-License-Identifier: MIT
 * A launcher's Shortcut tab fields. See launcher.h.
 */
#include "launcher.h"

#include <glib/gstdio.h>
#include <unistd.h>

#define GROUP G_KEY_FILE_DESKTOP_GROUP

static GKeyFile *load(const char *path, GError **err) {
    GKeyFile *kf = g_key_file_new();
    GKeyFileFlags flags = G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS;
    if (!g_key_file_load_from_file(kf, path, flags, err)) {
        g_key_file_unref(kf);
        return nullptr;
    }
    return kf;
}

static gboolean is_link(GKeyFile *kf) {
    char *type = g_key_file_get_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, nullptr);
    gboolean link = g_strcmp0(type, G_KEY_FILE_DESKTOP_TYPE_LINK) == 0;
    g_free(type);
    return link;
}

gboolean xd_launcher_read(const char *path, XdLauncher *out, GError **err) {
    *out = (XdLauncher){0};
    GKeyFile *kf = load(path, err);
    if (kf == nullptr)
        return FALSE;
    char *type = g_key_file_get_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, nullptr);
    gboolean ok = g_strcmp0(type, G_KEY_FILE_DESKTOP_TYPE_APPLICATION) == 0 ||
                  g_strcmp0(type, G_KEY_FILE_DESKTOP_TYPE_LINK) == 0;
    g_free(type);
    if (!ok) {
        g_set_error(err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                    "%s is not a shortcut to a program or a location", path);
        g_key_file_unref(kf);
        return FALSE;
    }
    out->is_link = is_link(kf);
    out->name =
        g_key_file_get_locale_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, nullptr, nullptr);
    const char *target_key =
        out->is_link ? G_KEY_FILE_DESKTOP_KEY_URL : G_KEY_FILE_DESKTOP_KEY_EXEC;
    out->target = g_key_file_get_string(kf, GROUP, target_key, nullptr);
    out->start_in = g_key_file_get_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_PATH, nullptr);
    out->comment =
        g_key_file_get_locale_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_COMMENT, nullptr, nullptr);
    out->icon = g_key_file_get_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, nullptr);
    g_key_file_unref(kf);
    return TRUE;
}

void xd_launcher_clear(XdLauncher *l) {
    g_free(l->name);
    g_free(l->target);
    g_free(l->start_in);
    g_free(l->comment);
    g_free(l->icon);
    *l = (XdLauncher){0};
}

static void set_or_remove(GKeyFile *kf, const char *key, const char *value) {
    if (value != nullptr && value[0] != '\0')
        g_key_file_set_string(kf, GROUP, key, value);
    else
        g_key_file_remove_key(kf, GROUP, key, nullptr);
}

/* The file can be replaced in place (g_file_set_contents writes a new
 * file next to it and renames it over). */
static gboolean writable(const char *path) {
    char *dir = g_path_get_dirname(path);
    gboolean ok = g_access(path, W_OK) == 0 && g_access(dir, W_OK) == 0;
    g_free(dir);
    return ok;
}

char *xd_launcher_write(const char *path, const XdLauncher *l, const char *user_dir,
                        GError **err) {
    GKeyFile *kf = load(path, err);
    if (kf == nullptr)
        return nullptr;
    set_or_remove(kf, is_link(kf) ? G_KEY_FILE_DESKTOP_KEY_URL : G_KEY_FILE_DESKTOP_KEY_EXEC,
                  l->target);
    set_or_remove(kf, G_KEY_FILE_DESKTOP_KEY_PATH, l->start_in);
    set_or_remove(kf, G_KEY_FILE_DESKTOP_KEY_ICON, l->icon);
    /* The Comment shown is the current language's, when it has one:
     * that's the one changed. */
    char *locale =
        g_key_file_get_locale_for_key(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_COMMENT, nullptr);
    if (locale != nullptr) {
        if (l->comment != nullptr && l->comment[0] != '\0') {
            g_key_file_set_locale_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_COMMENT, locale,
                                         l->comment);
        } else {
            char *key = g_strdup_printf("%s[%s]", G_KEY_FILE_DESKTOP_KEY_COMMENT, locale);
            g_key_file_remove_key(kf, GROUP, key, nullptr);
            g_free(key);
        }
    } else {
        set_or_remove(kf, G_KEY_FILE_DESKTOP_KEY_COMMENT, l->comment);
    }
    g_free(locale);

    char *dest = nullptr;
    if (writable(path)) {
        dest = g_strdup(path);
    } else {
        g_mkdir_with_parents(user_dir, 0700);
        char *base = g_path_get_basename(path);
        dest = g_build_filename(user_dir, base, nullptr);
        g_free(base);
    }
    if (!g_key_file_save_to_file(kf, dest, err))
        g_clear_pointer(&dest, g_free);
    g_key_file_unref(kf);
    return dest;
}
