/* SPDX-License-Identifier: MIT
 * My Computer and the Recycle Bin. See places.h.
 */
#include "places.h"

#include "../gtkapp/gtkapp.h"
#include "fileops.h"

#define TRASHINFO_GROUP "Trash Info"

/* ---- locations ------------------------------------------------------------------ */

static gboolean has_uri(GFile *f, const char *uri) {
    if (f == nullptr)
        return FALSE;
    char *u = g_file_get_uri(f);
    gboolean same = g_str_equal(u, uri);
    g_free(u);
    return same;
}

XdPlace xd_place_of(GFile *location) {
    if (has_uri(location, XD_COMPUTER_URI))
        return XD_PLACE_COMPUTER;
    if (has_uri(location, XD_TRASH_URI))
        return XD_PLACE_TRASH;
    return XD_PLACE_FOLDER;
}

GFile *xd_place_computer(void) { return g_file_new_for_uri(XD_COMPUTER_URI); }
GFile *xd_place_trash(void) { return g_file_new_for_uri(XD_TRASH_URI); }

const char *xd_place_title(GFile *location) {
    switch (xd_place_of(location)) {
    case XD_PLACE_COMPUTER:
        return "My Computer";
    case XD_PLACE_TRASH:
        return "Recycle Bin";
    case XD_PLACE_FOLDER:
        break;
    }
    return nullptr;
}

const char *xd_place_icon(GFile *location) {
    switch (xd_place_of(location)) {
    case XD_PLACE_COMPUTER:
        return "computer";
    case XD_PLACE_TRASH:
        return xd_trash_is_empty() ? "user-trash" : "user-trash-full";
    case XD_PLACE_FOLDER:
        break;
    }
    return "folder";
}

GFile *xd_place_parse(const char *text) {
    char *t = g_strstrip(g_strdup(text));
    GFile *f = nullptr;
    if (g_ascii_strcasecmp(t, "My Computer") == 0 || g_ascii_strcasecmp(t, "computer:") == 0 ||
        g_ascii_strcasecmp(t, XD_COMPUTER_URI) == 0)
        f = xd_place_computer();
    else if (g_ascii_strcasecmp(t, "Recycle Bin") == 0 || g_ascii_strcasecmp(t, "trash:") == 0 ||
             g_ascii_strcasecmp(t, XD_TRASH_URI) == 0)
        f = xd_place_trash();
    g_free(t);
    return f;
}

gboolean xd_place_is_drive_root(GFile *dir) {
    if (dir == nullptr || xd_place_of(dir) != XD_PLACE_FOLDER)
        return FALSE;
    if (!g_file_has_parent(dir, nullptr))
        return TRUE;
    GMount *mount = g_file_find_enclosing_mount(dir, nullptr, nullptr);
    if (mount == nullptr)
        return FALSE;
    GFile *root = g_mount_get_root(mount);
    gboolean is_root = g_file_equal(root, dir);
    g_object_unref(root);
    g_object_unref(mount);
    return is_root;
}

/* ---- My Computer ---------------------------------------------------------------- */

GVolumeMonitor *xd_computer_monitor(void) {
    static GVolumeMonitor *monitor;
    if (monitor == nullptr)
        monitor = g_volume_monitor_get(); /* kept for the process */
    return monitor;
}

char *xd_format_drive_size(guint64 bytes) {
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)bytes;
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < G_N_ELEMENTS(units)) {
        v /= 1024.0;
        u++;
    }
    /* Three significant digits, as Windows: 9.81 GB, 98.1 GB, 981 GB. */
    if (u == 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " bytes", bytes);
    return g_strdup_printf(v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

static void drive_free(gpointer p) {
    XdDrive *d = p;
    g_clear_object(&d->root);
    g_clear_object(&d->icon);
    g_free(d->name);
    g_free(d->kind);
    g_free(d);
}

static XdDrive *drive_new(GFile *root, const char *name, const char *kind, GIcon *icon) {
    XdDrive *d = g_new0(XdDrive, 1);
    d->root = g_object_ref(root);
    d->name = g_strdup(name);
    d->kind = g_strdup(kind);
    d->icon = icon != nullptr ? g_object_ref(icon) : nullptr;
    /* Sizes only for local disks: a network share can take long to ask. */
    if (g_file_is_native(root)) {
        GFileInfo *fs = g_file_query_filesystem_info(
            root, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE, nullptr,
            nullptr);
        if (fs != nullptr) {
            d->total = g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
            d->free = g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
            g_object_unref(fs);
        }
    }
    return d;
}

static gboolean icon_has(GIcon *icon, const char *word) {
    if (icon == nullptr || !G_IS_THEMED_ICON(icon))
        return FALSE;
    const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    for (; names != nullptr && *names != nullptr; names++)
        if (strstr(*names, word) != nullptr)
            return TRUE;
    return FALSE;
}

static const char *mount_kind(GMount *mount, GFile *root, GIcon *icon) {
    if (!g_file_is_native(root))
        return "Network Drive";
    if (icon_has(icon, "optical"))
        return "CD Drive";
    GDrive *drive = g_mount_get_drive(mount);
    gboolean removable = drive != nullptr && g_drive_is_removable(drive);
    g_clear_object(&drive);
    return removable || g_mount_can_eject(mount) ? "Removable Disk" : "Local Disk";
}

GPtrArray *xd_computer_drives(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(drive_free);
    GFile *slash = g_file_new_for_path("/");
    GIcon *disk = g_themed_icon_new("drive-harddisk");
    g_ptr_array_add(out, drive_new(slash, "File System", "Local Disk", disk));
    g_object_unref(disk);

    /* XP lists the user's documents beside the drives. */
    const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    GFile *docs_dir = g_file_new_for_path(docs != nullptr ? docs : g_get_home_dir());
    const char *real = g_get_real_name();
    char *docs_name = real != nullptr && *real != '\0' && strcmp(real, "Unknown") != 0
                          ? g_strdup_printf("%s's Documents", real)
                          : g_strdup("My Documents");
    GIcon *docs_icon = g_themed_icon_new("folder-documents");
    XdDrive *d = drive_new(docs_dir, docs_name, "File Folder", docs_icon);
    d->total = d->free = 0; /* a folder, not a drive */
    g_ptr_array_add(out, d);
    g_object_unref(docs_icon);
    g_free(docs_name);
    g_object_unref(docs_dir);

    GList *mounts = g_volume_monitor_get_mounts(xd_computer_monitor());
    for (GList *l = mounts; l != nullptr; l = l->next) {
        GMount *m = l->data;
        if (g_mount_is_shadowed(m))
            continue;
        GFile *root = g_mount_get_root(m);
        if (!g_file_equal(root, slash)) {
            GIcon *icon = g_mount_get_icon(m);
            char *name = g_mount_get_name(m);
            g_ptr_array_add(out, drive_new(root, name, mount_kind(m, root, icon), icon));
            g_free(name);
            g_clear_object(&icon);
        }
        g_object_unref(root);
    }
    g_list_free_full(mounts, g_object_unref);
    g_object_unref(slash);
    return out;
}

/* ---- Recycle Bin ------------------------------------------------------------------- */

static GFile *trash_dir(const char *sub) {
    char *path = g_build_filename(g_get_user_data_dir(), "Trash", sub, nullptr);
    GFile *f = g_file_new_for_path(path);
    g_free(path);
    return f;
}

GFile *xd_trash_files_dir(void) {
    GFile *f = trash_dir("files");
    g_file_make_directory_with_parents(f, nullptr, nullptr);
    return f;
}

static GFile *info_file(GFile *trashed) {
    char *base = g_file_get_basename(trashed);
    char *name = g_strconcat(base, ".trashinfo", nullptr);
    GFile *dir = trash_dir("info");
    GFile *f = g_file_get_child(dir, name);
    g_object_unref(dir);
    g_free(name);
    g_free(base);
    return f;
}

gboolean xd_trash_item_info(GFile *trashed, char **original_path, GDateTime **deleted) {
    *original_path = nullptr;
    if (deleted != nullptr)
        *deleted = nullptr;
    GFile *info = info_file(trashed);
    char *path = g_file_get_path(info);
    g_object_unref(info);
    GKeyFile *kf = g_key_file_new();
    gboolean ok = path != nullptr && g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr);
    g_free(path);
    if (ok) {
        /* Path is URI-escaped; relative paths are relative to the trash's
         * top directory (the home trash's is /). */
        char *escaped = g_key_file_get_string(kf, TRASHINFO_GROUP, "Path", nullptr);
        char *orig = escaped != nullptr ? g_uri_unescape_string(escaped, nullptr) : nullptr;
        g_free(escaped);
        if (orig != nullptr && orig[0] != '/') {
            char *abs = g_build_filename("/", orig, nullptr);
            g_free(orig);
            orig = abs;
        }
        *original_path = orig;
        char *date = g_key_file_get_string(kf, TRASHINFO_GROUP, "DeletionDate", nullptr);
        if (date != nullptr && deleted != nullptr) {
            /* Local time, without a zone: "2026-10-16T19:58:00". */
            GTimeZone *tz = g_time_zone_new_local();
            *deleted = g_date_time_new_from_iso8601(date, tz);
            g_time_zone_unref(tz);
        }
        g_free(date);
        ok = orig != nullptr;
    }
    g_key_file_unref(kf);
    return ok;
}

gboolean xd_trash_is_empty(void) {
    GFile *dir = trash_dir("files");
    GFileEnumerator *en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr,
                                                    nullptr);
    g_object_unref(dir);
    if (en == nullptr)
        return TRUE;
    GFileInfo *first = g_file_enumerator_next_file(en, nullptr, nullptr);
    gboolean empty = first == nullptr;
    g_clear_object(&first);
    g_object_unref(en);
    return empty;
}

void xd_trash_restore(GtkWindow *parent, GList *files) {
    for (GList *l = files; l != nullptr; l = l->next) {
        GFile *trashed = l->data;
        char *orig = nullptr;
        char *name = g_file_get_basename(trashed);
        char *problem = nullptr;
        if (!xd_trash_item_info(trashed, &orig, nullptr)) {
            problem = g_strdup_printf("Cannot restore %s: where it came from isn't known.", name);
        } else {
            GFile *target = g_file_new_for_path(orig);
            GFile *parent_dir = g_file_get_parent(target);
            if (parent_dir != nullptr)
                g_file_make_directory_with_parents(parent_dir, nullptr, nullptr);
            g_clear_object(&parent_dir);
            GError *err = nullptr;
            char *shown = g_filename_display_name(orig);
            if (g_file_query_exists(target, nullptr)) {
                problem = g_strdup_printf("Cannot restore %s: a file or folder with that name "
                                          "already exists.",
                                          shown);
            } else if (g_file_move(trashed, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr,
                                   nullptr, nullptr, &err)) {
                GFile *info = info_file(trashed);
                g_file_delete(info, nullptr, nullptr);
                g_object_unref(info);
            } else {
                problem = g_strdup_printf("Cannot restore %s: %s", shown, err->message);
                g_error_free(err);
            }
            g_free(shown);
            g_object_unref(target);
        }
        if (problem != nullptr) {
            xd_message_box(parent, "Error Restoring File or Folder", XD_MB_ERROR, XD_MB_OK, FALSE,
                           problem);
            g_free(problem);
        }
        g_free(orig);
        g_free(name);
    }
}

void xd_trash_tidy(void) {
    GFile *info_dir = trash_dir("info");
    GFile *files_dir = trash_dir("files");
    GFileEnumerator *en = g_file_enumerate_children(info_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr,
                                                    nullptr);
    GFileInfo *fi;
    while (en != nullptr && (fi = g_file_enumerator_next_file(en, nullptr, nullptr)) != nullptr) {
        const char *name = g_file_info_get_name(fi);
        if (g_str_has_suffix(name, ".trashinfo")) {
            char *item = g_strndup(name, strlen(name) - strlen(".trashinfo"));
            GFile *f = g_file_get_child(files_dir, item);
            if (!g_file_query_exists(f, nullptr)) {
                GFile *stale = g_file_get_child(info_dir, name);
                g_file_delete(stale, nullptr, nullptr);
                g_object_unref(stale);
            }
            g_object_unref(f);
            g_free(item);
        }
        g_object_unref(fi);
    }
    g_clear_object(&en);
    g_object_unref(info_dir);
    g_object_unref(files_dir);
}

typedef struct empty_request {
    void (*done)(GList *created, gpointer user);
    gpointer user;
} empty_request;

static void emptied(GList *created, gpointer data) {
    empty_request *r = data;
    xd_trash_tidy();
    if (r->done != nullptr)
        r->done(created, r->user);
    g_free(r);
}

void xd_trash_empty(GtkWindow *parent, void (*done)(GList *created, gpointer user),
                    gpointer user) {
    GFile *dir = xd_trash_files_dir();
    GList *files = nullptr;
    GFileEnumerator *en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr,
                                                    nullptr);
    GFileInfo *fi;
    while (en != nullptr && (fi = g_file_enumerator_next_file(en, nullptr, nullptr)) != nullptr) {
        files = g_list_prepend(files, g_file_get_child(dir, g_file_info_get_name(fi)));
        g_object_unref(fi);
    }
    g_clear_object(&en);
    g_object_unref(dir);
    if (files == nullptr) {
        if (done != nullptr)
            done(nullptr, user);
        return;
    }
    empty_request *r = g_new0(empty_request, 1);
    r->done = done;
    r->user = user;
    /* Asks first; nothing happens (and `emptied` isn't called) on No. */
    if (!xd_fileop_start_checked(parent, XD_FILEOP_DELETE, files, nullptr, emptied, r)) {
        if (done != nullptr)
            done(nullptr, user);
        g_free(r);
    }
    g_list_free_full(files, g_object_unref);
}

/* ---- renaming ------------------------------------------------------------------------- */

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
