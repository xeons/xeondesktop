/* SPDX-License-Identifier: MIT */
#include "themepkg.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char **xd_theme_package_dirs(void) {
    GPtrArray *dirs = g_ptr_array_new();
    g_ptr_array_add(dirs, xd_theme_package_user_dir());
    for (const char *const *d = g_get_system_data_dirs(); *d != nullptr; d++)
        g_ptr_array_add(dirs, g_build_filename(*d, "xeondesktop", "themes", nullptr));
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

char *xd_theme_package_user_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "xeondesktop", "themes", nullptr);
}

/* A name ThemePackage may hold: no path, nothing hidden (as the settings
 * parser takes it). */
static bool plain_name(const char *name) {
    return name != nullptr && name[0] != '\0' && name[0] != '.' && strchr(name, '/') == nullptr;
}

static bool is_folder_package(const char *path) {
    char *xml = g_build_filename(path, "theme.xml", nullptr);
    bool ok = g_file_test(xml, G_FILE_TEST_IS_REGULAR);
    g_free(xml);
    return ok;
}

char *xd_theme_package_find(const char *const *dirs, const char *name) {
    if (!plain_name(name))
        return nullptr;
    for (; dirs != nullptr && *dirs != nullptr; dirs++) {
        char *folder = g_build_filename(*dirs, name, nullptr);
        if (is_folder_package(folder))
            return folder;
        g_free(folder);
        char *file = g_strconcat(name, ".xdtheme", nullptr);
        char *zip = g_build_filename(*dirs, file, nullptr);
        g_free(file);
        if (g_file_test(zip, G_FILE_TEST_IS_REGULAR))
            return zip;
        g_free(zip);
    }
    return nullptr;
}

static void package_free(gpointer p) {
    xd_installed_package *pkg = p;
    g_free(pkg->name);
    g_free(pkg->path);
    xd_pkg_info_clear(&pkg->info);
    g_free(pkg);
}

static const char *display_name(const xd_installed_package *p) {
    return p->info.name[0] != '\0' ? p->info.name : p->name;
}

static int by_display_name(gconstpointer a, gconstpointer b) {
    const xd_installed_package *x = *(xd_installed_package *const *)a;
    const xd_installed_package *y = *(xd_installed_package *const *)b;
    int c = g_utf8_collate(display_name(x), display_name(y));
    return c != 0 ? c : strcmp(x->name, y->name);
}

/* The package name a directory entry stands for, or nullptr. */
static char *entry_name(const char *dir, const char *entry) {
    if (entry[0] == '.')
        return nullptr;
    if (g_str_has_suffix(entry, ".xdtheme")) {
        char *name = g_strndup(entry, strlen(entry) - strlen(".xdtheme"));
        if (name[0] != '\0')
            return name;
        g_free(name);
        return nullptr;
    }
    char *path = g_build_filename(dir, entry, nullptr);
    bool folder = is_folder_package(path);
    g_free(path);
    return folder ? g_strdup(entry) : nullptr;
}

GPtrArray *xd_theme_packages_list(const char *const *dirs) {
    GPtrArray *list = g_ptr_array_new_with_free_func(package_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (const char *const *d = dirs; d != nullptr && *d != nullptr; d++) {
        GDir *dir = g_dir_open(*d, 0, nullptr);
        if (dir == nullptr)
            continue;
        const char *entry;
        while ((entry = g_dir_read_name(dir)) != nullptr) {
            char *name = entry_name(*d, entry);
            if (name == nullptr || g_hash_table_contains(seen, name)) {
                g_free(name);
                continue;
            }
            g_hash_table_add(seen, name);
            /* Found as the desktop would load it: an earlier directory's,
             * a folder before a zip. */
            char *path = xd_theme_package_find(dirs, name);
            xd_installed_package *p = g_new0(xd_installed_package, 1);
            GError *error = nullptr;
            if (path == nullptr || !xd_pkg_read_info(path, &p->info, &error)) {
                if (error != nullptr) {
                    g_message("theme package %s: %s", name, error->message);
                    g_error_free(error);
                }
                g_free(path);
                g_free(p);
                continue;
            }
            p->name = g_strdup(name);
            p->path = path;
            g_ptr_array_add(list, p);
        }
        g_dir_close(dir);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_sort(list, by_display_name);
    return list;
}

char *xd_theme_names_path(void) {
    xd_strbuf dir = {0};
    char *path = xd_data_dir(&dir)
                     ? g_build_filename(xd_strbuf_cstr(&dir), "theme", "uxtheme.ini", nullptr)
                     : g_strdup("/usr/share/xeondesktop/theme/uxtheme.ini");
    xd_strbuf_free(&dir);
    return path;
}

xd_image_filter xd_settings_image_filter(const xd_settings *s, const xd_pkg *pkg) {
    if (s->image_scaling != XD_FILTER_COUNT)
        return s->image_scaling;
    xd_image_filter f;
    if (pkg != nullptr && xd_pkg_filter(pkg, &f))
        return f;
    return XD_FILTER_SMOOTH;
}

/* ---- the active package ------------------------------------------------------- */

static struct {
    xd_pkg *pkg;
    char *path;    /* as chosen: nullptr for none */
    char *variant; /* as asked for */
    gint64 stamp;  /* when it was written, to notice a re-import */
} active;

/* The latest change to a package: the zip, or a folder's theme.xml and
 * the folder itself (files added or replaced). Styles edited in place
 * aren't noticed: touch theme.xml. */
static gint64 package_stamp(const char *path) {
    GStatBuf st;
    gint64 stamp = 0;
    if (g_stat(path, &st) == 0)
        stamp = (gint64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
    char *xml = g_build_filename(path, "theme.xml", nullptr);
    if (g_stat(xml, &st) == 0)
        stamp = MAX(stamp, (gint64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec);
    g_free(xml);
    return stamp;
}

xd_pkg *xd_settings_use_package(const xd_settings *s) {
    const char *env = getenv("XD_THEME_PACKAGE");
    bool from_env = env != nullptr && env[0] != '\0';
    char *path = nullptr;
    const char *variant = s->theme_variant;
    if (from_env) {
        path = g_strdup(env);
        const char *v = getenv("XD_THEME_VARIANT");
        if (v != nullptr)
            variant = v;
    } else if (s->theme_package[0] != '\0' && !s->access.high_contrast) {
        char **dirs = xd_theme_package_dirs();
        path = xd_theme_package_find((const char *const *)dirs, s->theme_package);
        g_strfreev(dirs);
        if (path == nullptr)
            fprintf(stderr, "theme package %s is not installed\n", s->theme_package);
    }
    gint64 stamp = path != nullptr ? package_stamp(path) : 0;
    xd_pkg *old = nullptr;
    bool same = g_strcmp0(path, active.path) == 0 && g_strcmp0(variant, active.variant) == 0 &&
                stamp == active.stamp;
    if (!same) {
        xd_pkg *pkg = nullptr;
        if (path != nullptr) {
            char *names = xd_theme_names_path();
            GError *error = nullptr;
            pkg = xd_pkg_load(path, variant[0] != '\0' ? variant : nullptr, names, &error);
            g_free(names);
            if (pkg == nullptr) {
                fprintf(stderr, "theme package: %s\n", error->message);
                g_error_free(error);
            }
        }
        old = active.pkg;
        active.pkg = pkg;
        g_free(active.path);
        g_free(active.variant);
        active.path = path;
        active.variant = g_strdup(variant);
        active.stamp = stamp;
        path = nullptr;
    }
    g_free(path);
    xd_image_filter filter = xd_settings_image_filter(s, active.pkg);
    const char *fname = getenv("XD_IMAGE_FILTER");
    xd_image_filter f;
    if (from_env && fname != nullptr && xd_image_filter_from_name(fname, &f))
        filter = f;
    xd_theme_use_package(active.pkg, filter, s->integer_scaling);
    return old;
}

const xd_pkg *xd_settings_active_package(void) { return active.pkg; }
