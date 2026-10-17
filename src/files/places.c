/* SPDX-License-Identifier: MIT
 * My Computer and the Recycle Bin. See places.h.
 */
#include "places.h"

#include <unistd.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/soundplay.h"
#include "../settings/state.h"
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
    if (location != nullptr && g_file_has_uri_scheme(location, "control"))
        return XD_PLACE_CONTROL;
    return XD_PLACE_FOLDER;
}

GFile *xd_place_computer(void) { return g_file_new_for_uri(XD_COMPUTER_URI); }
GFile *xd_place_trash(void) { return g_file_new_for_uri(XD_TRASH_URI); }
GFile *xd_place_control(void) { return g_file_new_for_uri(XD_CONTROL_URI); }

const char *xd_place_title(GFile *location) {
    switch (xd_place_of(location)) {
    case XD_PLACE_COMPUTER:
        return "My Computer";
    case XD_PLACE_TRASH:
        return "Recycle Bin";
    case XD_PLACE_CONTROL: {
        const XdControlCategory *c = xd_control_page(location);
        return c != nullptr ? c->title : "Control Panel";
    }
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
    case XD_PLACE_CONTROL: {
        const XdControlCategory *c = xd_control_page(location);
        return c != nullptr ? c->icon : "preferences-system";
    }
    case XD_PLACE_FOLDER:
        break;
    }
    return "folder";
}

static GFile *control_page_file(const XdControlCategory *c) {
    char *uri = g_strconcat(XD_CONTROL_URI, c->key, nullptr);
    GFile *f = g_file_new_for_uri(uri);
    g_free(uri);
    return f;
}

GFile *xd_place_parse(const char *text) {
    char *t = g_strstrip(g_strdup(text));
    GFile *f = nullptr;
    if (g_ascii_strcasecmp(t, "My Computer") == 0 || g_ascii_strcasecmp(t, "computer:") == 0 ||
        g_ascii_strcasecmp(t, XD_COMPUTER_URI) == 0) {
        f = xd_place_computer();
    } else if (g_ascii_strcasecmp(t, "Recycle Bin") == 0 || g_ascii_strcasecmp(t, "trash:") == 0 ||
               g_ascii_strcasecmp(t, XD_TRASH_URI) == 0) {
        f = xd_place_trash();
    } else if (g_ascii_strcasecmp(t, "Control Panel") == 0 ||
               g_ascii_strcasecmp(t, "control:") == 0 ||
               g_ascii_strcasecmp(t, XD_CONTROL_URI) == 0) {
        f = xd_place_control();
    } else {
        /* A category's page, by its address or its name. */
        const char *key = g_ascii_strncasecmp(t, XD_CONTROL_URI, strlen(XD_CONTROL_URI)) == 0
                              ? t + strlen(XD_CONTROL_URI)
                              : nullptr;
        for (size_t i = 0; f == nullptr && i < xd_control_category_count; i++) {
            const XdControlCategory *c = &xd_control_categories[i];
            if (key != nullptr ? g_ascii_strcasecmp(key, c->key) == 0
                               : g_ascii_strcasecmp(t, c->title) == 0)
                f = control_page_file(c);
        }
    }
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

/* An unmounted volume's place in My Computer: computer:///volume/ and
 * what identifies it (its UUID, else its device, else its name). */
#define VOLUME_URI "computer:///volume/"

static char *volume_uri(GVolume *vol) {
    char *id = g_volume_get_uuid(vol);
    if (id == nullptr)
        id = g_volume_get_identifier(vol, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
    if (id == nullptr)
        id = g_volume_get_name(vol);
    char *escaped = g_uri_escape_string(id != nullptr ? id : "", nullptr, FALSE);
    char *uri = g_strconcat(VOLUME_URI, escaped, nullptr);
    g_free(escaped);
    g_free(id);
    return uri;
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

    /* Volumes not mounted yet: shown as drives, mounted when opened. */
    GList *volumes = g_volume_monitor_get_volumes(xd_computer_monitor());
    for (GList *l = volumes; l != nullptr; l = l->next) {
        GVolume *vol = l->data;
        GMount *m = g_volume_get_mount(vol);
        if (m != nullptr) {
            g_object_unref(m);
            continue;
        }
        if (!g_volume_can_mount(vol))
            continue;
        char *uri = volume_uri(vol);
        GFile *item = g_file_new_for_uri(uri);
        GIcon *icon = g_volume_get_icon(vol);
        char *name = g_volume_get_name(vol);
        GDrive *drive = g_volume_get_drive(vol);
        gboolean removable = drive != nullptr && g_drive_is_removable(drive);
        g_clear_object(&drive);
        const char *kind = icon_has(icon, "optical") ? "CD Drive"
                           : removable              ? "Removable Disk"
                                                    : "Local Disk";
        g_ptr_array_add(out, drive_new(item, name, kind, icon));
        g_free(name);
        g_clear_object(&icon);
        g_object_unref(item);
        g_free(uri);
    }
    g_list_free_full(volumes, g_object_unref);
    return out;
}

GVolume *xd_computer_volume(GFile *item) {
    char *want = g_file_get_uri(item);
    GVolume *found = nullptr;
    if (g_str_has_prefix(want, VOLUME_URI)) {
        GList *volumes = g_volume_monitor_get_volumes(xd_computer_monitor());
        for (GList *l = volumes; l != nullptr && found == nullptr; l = l->next) {
            char *uri = volume_uri(l->data);
            if (strcmp(uri, want) == 0)
                found = g_object_ref(l->data);
            g_free(uri);
        }
        g_list_free_full(volumes, g_object_unref);
    }
    g_free(want);
    return found;
}

GMount *xd_computer_mount(GFile *root) {
    GMount *found = nullptr;
    GList *mounts = g_volume_monitor_get_mounts(xd_computer_monitor());
    for (GList *l = mounts; l != nullptr && found == nullptr; l = l->next) {
        GFile *r = g_mount_get_root(l->data);
        if (g_file_equal(r, root))
            found = g_object_ref(l->data);
        g_object_unref(r);
    }
    g_list_free_full(mounts, g_object_unref);
    return found;
}

gboolean xd_computer_can_eject(GFile *item) {
    GVolume *vol = xd_computer_volume(item);
    if (vol != nullptr) {
        gboolean can = g_volume_can_eject(vol);
        g_object_unref(vol);
        return can;
    }
    GMount *m = xd_computer_mount(item);
    gboolean can = m != nullptr && (g_mount_can_eject(m) || g_mount_can_unmount(m));
    g_clear_object(&m);
    return can;
}

/* ---- Control Panel ---------------------------------------------------------------- */

#define CONTROL_STATE "control-panel" /* "classic" or "category" */

gboolean xd_control_classic_view(void) {
    char **lines = nullptr;
    size_t n = 0;
    xd_state_read_lines(CONTROL_STATE, &lines, &n, 1);
    gboolean classic = n > 0 && strcmp(lines[0], "classic") == 0;
    xd_free_lines(lines, n);
    return classic;
}

void xd_control_set_classic_view(gboolean classic) {
    char *line = g_strdup(classic ? "classic" : "category");
    if (!xd_state_write_lines(CONTROL_STATE, &line, 1))
        g_printerr("xd-files: cannot save the Control Panel's view\n");
    g_free(line);
}

const XdControlCategory *xd_control_page(GFile *location) {
    if (location == nullptr || !g_file_has_uri_scheme(location, "control"))
        return nullptr;
    char *uri = g_file_get_uri(location);
    char *key = g_str_has_prefix(uri, XD_CONTROL_URI) ? uri + strlen(XD_CONTROL_URI) : uri;
    size_t len = strlen(key);
    if (len > 0 && key[len - 1] == '/')
        key[len - 1] = '\0';
    const XdControlCategory *c = *key != '\0' ? xd_control_category(key) : nullptr;
    g_free(uri);
    return c;
}

static void control_item_free(gpointer p) {
    XdControlItem *it = p;
    g_clear_object(&it->file);
    g_clear_object(&it->icon);
    g_free(it->name);
    g_free(it->comment);
    g_free(it);
}

static XdControlItem *applet_item(GDesktopAppInfo *info) {
    const char *path = g_desktop_app_info_get_filename(info);
    if (path == nullptr)
        return nullptr;
    XdControlItem *it = g_new0(XdControlItem, 1);
    it->file = g_file_new_for_path(path);
    it->name = g_strdup(g_app_info_get_name(G_APP_INFO(info)));
    const char *comment = g_app_info_get_description(G_APP_INFO(info));
    it->comment = g_strdup(comment != nullptr ? comment : "");
    GIcon *icon = g_app_info_get_icon(G_APP_INFO(info));
    it->icon = icon != nullptr ? g_object_ref(icon) : g_themed_icon_new("preferences-system");
    return it;
}

GPtrArray *xd_control_items(GFile *location) {
    GPtrArray *out = g_ptr_array_new_with_free_func(control_item_free);
    const XdControlCategory *page = xd_control_page(location);
    GList *applets = xd_control_applets();
    if (page == nullptr && !xd_control_classic_view()) {
        /* Category View: "Pick a category", each with its applets' names
         * (Tiles shows them). "Other" only when something is in it. */
        for (size_t i = 0; i < xd_control_category_count; i++) {
            const XdControlCategory *c = &xd_control_categories[i];
            GString *names = g_string_new(nullptr);
            for (GList *l = applets; l != nullptr; l = l->next) {
                if (!xd_control_applet_in(l->data, c->key))
                    continue;
                if (names->len > 0)
                    g_string_append(names, ", ");
                g_string_append(names, g_app_info_get_name(G_APP_INFO(l->data)));
            }
            if (names->len == 0 && strcmp(c->key, XD_CONTROL_OTHER) == 0) {
                g_string_free(names, TRUE);
                continue;
            }
            XdControlItem *it = g_new0(XdControlItem, 1);
            it->file = control_page_file(c);
            it->name = g_strdup(c->title);
            it->comment = g_string_free(names, FALSE);
            it->icon = xd_control_category_icon(c);
            it->category = TRUE;
            g_ptr_array_add(out, it);
        }
    } else {
        for (GList *l = applets; l != nullptr; l = l->next) {
            if (page != nullptr && !xd_control_applet_in(l->data, page->key))
                continue;
            XdControlItem *it = applet_item(l->data);
            if (it != nullptr)
                g_ptr_array_add(out, it);
        }
    }
    g_list_free_full(applets, g_object_unref);
    return out;
}

GDesktopAppInfo *xd_control_item_applet(GFile *item) {
    char *path = item != nullptr ? g_file_get_path(item) : nullptr;
    GDesktopAppInfo *info = path != nullptr && g_str_has_suffix(path, ".desktop")
                                ? g_desktop_app_info_new_from_filename(path)
                                : nullptr;
    g_free(path);
    if (info != nullptr && !xd_control_is_applet(info))
        g_clear_object(&info);
    return info;
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

GPtrArray *xd_trash_dirs(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(out, xd_trash_files_dir());
    /* Other drives keep theirs at the top: .Trash-UID, or .Trash/UID. */
    char uid[16];
    g_snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
    char *own = g_strconcat(".Trash-", uid, nullptr);
    GList *mounts = g_volume_monitor_get_mounts(xd_computer_monitor());
    for (GList *l = mounts; l != nullptr; l = l->next) {
        GFile *top = g_mount_get_root(l->data);
        char *top_path = g_file_get_path(top);
        if (top_path != nullptr && strcmp(top_path, "/") != 0) {
            char *a = g_build_filename(top_path, own, "files", nullptr);
            char *b = g_build_filename(top_path, ".Trash", uid, "files", nullptr);
            const char *found = g_file_test(a, G_FILE_TEST_IS_DIR)   ? a
                                : g_file_test(b, G_FILE_TEST_IS_DIR) ? b
                                                                     : nullptr;
            if (found != nullptr)
                g_ptr_array_add(out, g_file_new_for_path(found));
            g_free(a);
            g_free(b);
        }
        g_free(top_path);
        g_object_unref(top);
    }
    g_list_free_full(mounts, g_object_unref);
    g_free(own);
    return out;
}

/* A trashed item's .trashinfo, beside its trash's files folder. */
static GFile *info_file(GFile *trashed) {
    char *base = g_file_get_basename(trashed);
    char *name = g_strconcat(base, ".trashinfo", nullptr);
    GFile *files = g_file_get_parent(trashed);
    GFile *trash = files != nullptr ? g_file_get_parent(files) : nullptr;
    GFile *dir = trash != nullptr ? g_file_get_child(trash, "info") : trash_dir("info");
    GFile *f = g_file_get_child(dir, name);
    g_object_unref(dir);
    g_clear_object(&trash);
    g_clear_object(&files);
    g_free(name);
    g_free(base);
    return f;
}

/* Where a trash's relative paths start: a drive's top (above .Trash-UID
 * or .Trash/UID); the home trash's paths are absolute. */
static char *trash_top(GFile *trashed) {
    GFile *files = g_file_get_parent(trashed);
    GFile *trash = files != nullptr ? g_file_get_parent(files) : nullptr;
    GFile *up = trash != nullptr ? g_file_get_parent(trash) : nullptr;
    char *name = trash != nullptr ? g_file_get_basename(trash) : nullptr;
    char *up_name = up != nullptr ? g_file_get_basename(up) : nullptr;
    char *top = nullptr;
    if (name != nullptr && g_str_has_prefix(name, ".Trash-")) {
        top = g_file_get_path(up);
    } else if (up_name != nullptr && strcmp(up_name, ".Trash") == 0) {
        GFile *upup = g_file_get_parent(up);
        top = upup != nullptr ? g_file_get_path(upup) : nullptr;
        g_clear_object(&upup);
    }
    g_free(name);
    g_free(up_name);
    g_clear_object(&up);
    g_clear_object(&trash);
    g_clear_object(&files);
    return top != nullptr ? top : g_strdup("/");
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
            char *top = trash_top(trashed);
            char *abs = g_build_filename(top, orig, nullptr);
            g_free(top);
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

static gboolean dir_is_empty(GFile *dir) {
    GFileEnumerator *en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr,
                                                    nullptr);
    if (en == nullptr)
        return TRUE;
    GFileInfo *first = g_file_enumerator_next_file(en, nullptr, nullptr);
    gboolean empty = first == nullptr;
    g_clear_object(&first);
    g_object_unref(en);
    return empty;
}

gboolean xd_trash_is_empty(void) {
    GPtrArray *dirs = xd_trash_dirs();
    gboolean empty = TRUE;
    for (guint i = 0; i < dirs->len && empty; i++)
        empty = dir_is_empty(g_ptr_array_index(dirs, i));
    g_ptr_array_unref(dirs);
    return empty;
}

/* Restoring over an existing file: XP's Confirm File Replace. Returns
 * whether to replace; Yes to All answers the rest, Cancel stops. */
typedef enum { ASK_EACH, ASK_YES_ALL, ASK_CANCELLED } replace_mode;

static gboolean confirm_replace(GtkWindow *parent, const char *shown, replace_mode *mode,
                                gboolean more) {
    if (*mode == ASK_YES_ALL)
        return TRUE;
    char *text = g_strdup_printf("This folder already contains a file named '%s'.\n\n"
                                 "Would you like to replace the existing file with this one?",
                                 shown);
    static const char *const labels[] = {"_Yes", "Yes to _All", "_No", "Cancel"};
    int n = more ? 4 : 3;
    int pick = xd_message_box_choose(parent, "Confirm File Replace", XD_MB_QUESTION, text,
                                     more ? labels : (const char *const[]){"_Yes", "_No", "Cancel"},
                                     n, 0, n - 1);
    g_free(text);
    if (!more)
        pick = pick == 0 ? 0 : pick == 1 ? 2 : 3;
    if (pick == 1)
        *mode = ASK_YES_ALL;
    else if (pick == 3 || pick < 0)
        *mode = ASK_CANCELLED;
    return pick == 0 || pick == 1;
}

void xd_trash_restore(GtkWindow *parent, GList *files) {
    replace_mode mode = ASK_EACH;
    for (GList *l = files; l != nullptr && mode != ASK_CANCELLED; l = l->next) {
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
            GFileCopyFlags flags = G_FILE_COPY_NOFOLLOW_SYMLINKS;
            gboolean go = TRUE;
            if (g_file_query_exists(target, nullptr)) {
                /* A file may replace a file; folders aren't merged. */
                gboolean dirs = g_file_query_file_type(target, G_FILE_QUERY_INFO_NONE, nullptr) ==
                                    G_FILE_TYPE_DIRECTORY ||
                                g_file_query_file_type(trashed, G_FILE_QUERY_INFO_NONE,
                                                       nullptr) == G_FILE_TYPE_DIRECTORY;
                if (dirs) {
                    problem = g_strdup_printf("Cannot restore %s: a folder with that name "
                                              "already exists.",
                                              shown);
                    go = FALSE;
                } else {
                    char *base = g_file_get_basename(target);
                    char *base_shown = g_filename_display_name(base);
                    go = confirm_replace(parent, base_shown, &mode, l->next != nullptr);
                    g_free(base_shown);
                    g_free(base);
                    flags |= G_FILE_COPY_OVERWRITE;
                }
            }
            /* Not when answered No or Cancel, or a folder is in the way. */
            if (go && problem == nullptr) {
                if (g_file_move(trashed, target, flags, nullptr, nullptr, nullptr, &err)) {
                    GFile *info = info_file(trashed);
                    g_file_delete(info, nullptr, nullptr);
                    g_object_unref(info);
                } else {
                    problem = g_strdup_printf("Cannot restore %s: %s", shown, err->message);
                    g_error_free(err);
                }
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

static void tidy_one(GFile *files_dir) {
    GFile *trash = g_file_get_parent(files_dir);
    GFile *info_dir = g_file_get_child(trash, "info");
    g_object_unref(trash);
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
}

void xd_trash_tidy(void) {
    GPtrArray *dirs = xd_trash_dirs();
    for (guint i = 0; i < dirs->len; i++)
        tidy_one(g_ptr_array_index(dirs, i));
    g_ptr_array_unref(dirs);
}

typedef struct empty_request {
    void (*done)(GList *created, gpointer user);
    gpointer user;
} empty_request;

static void emptied(GList *created, gpointer data) {
    empty_request *r = data;
    xd_trash_tidy();
    xd_sound_play_event(XD_SOUND_EMPTY_RECYCLE_BIN);
    if (r->done != nullptr)
        r->done(created, r->user);
    g_free(r);
}

void xd_trash_empty(GtkWindow *parent, void (*done)(GList *created, gpointer user),
                    gpointer user) {
    GPtrArray *dirs = xd_trash_dirs();
    GList *files = nullptr;
    for (guint i = 0; i < dirs->len; i++) {
        GFile *dir = g_ptr_array_index(dirs, i);
        GFileEnumerator *en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        nullptr, nullptr);
        GFileInfo *fi;
        while (en != nullptr &&
               (fi = g_file_enumerator_next_file(en, nullptr, nullptr)) != nullptr) {
            files = g_list_prepend(files, g_file_get_child(dir, g_file_info_get_name(fi)));
            g_object_unref(fi);
        }
        g_clear_object(&en);
    }
    g_ptr_array_unref(dirs);
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


GList *xd_trash_find(GList *originals) {
    guint n = g_list_length(originals);
    GHashTable *wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    guint k = 0;
    for (GList *l = originals; l != nullptr; l = l->next, k++) {
        char *path = g_file_get_path(l->data);
        if (path != nullptr)
            g_hash_table_replace(wanted, path, GUINT_TO_POINTER(k + 1));
    }
    GFile **found = g_new0(GFile *, n + 1);
    gint64 *when = g_new0(gint64, n + 1);
    GPtrArray *dirs = xd_trash_dirs();
    for (guint i = 0; i < dirs->len; i++) {
        GFile *dir = g_ptr_array_index(dirs, i);
        GFileEnumerator *en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        nullptr, nullptr);
        GFileInfo *fi;
        while (en != nullptr &&
               (fi = g_file_enumerator_next_file(en, nullptr, nullptr)) != nullptr) {
            GFile *child = g_file_get_child(dir, g_file_info_get_name(fi));
            g_object_unref(fi);
            char *orig = nullptr;
            GDateTime *deleted = nullptr;
            if (xd_trash_item_info(child, &orig, &deleted)) {
                guint idx = GPOINTER_TO_UINT(g_hash_table_lookup(wanted, orig));
                gint64 t = deleted != nullptr ? g_date_time_to_unix(deleted) : 0;
                /* The same name deleted twice: the later one. */
                if (idx > 0 && (found[idx - 1] == nullptr || t >= when[idx - 1])) {
                    g_set_object(&found[idx - 1], child);
                    when[idx - 1] = t;
                }
            }
            g_free(orig);
            if (deleted != nullptr)
                g_date_time_unref(deleted);
            g_object_unref(child);
        }
        g_clear_object(&en);
    }
    g_ptr_array_unref(dirs);
    GList *out = nullptr;
    for (guint i = n; i > 0; i--)
        if (found[i - 1] != nullptr)
            out = g_list_prepend(out, found[i - 1]);
    g_free(found);
    g_free(when);
    g_hash_table_unref(wanted);
    return out;
}
