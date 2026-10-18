/* SPDX-License-Identifier: MIT
 * Disk Cleanup's categories, as XP's (and 7's Thumbnails): what can go
 * from a drive, found and deleted without GTK so it can be tested.
 *
 * Only the user's own files, in places that are clearly theirs and
 * clearly disposable: the Recycle Bin, the thumbnail cache, browsers'
 * caches (Firefox's cache2, Chromium-based browsers' Cache and Code
 * Cache), and in the temporary folders, only what the user owns that
 * hasn't been touched for a week (and nothing holding a socket or FIFO,
 * which a running program may still use). Nothing is followed through
 * symbolic links or into other file systems.
 */
#ifndef XD_FILES_CLEANUP_H
#define XD_FILES_CLEANUP_H

#include <gio/gio.h>
#include <sys/types.h>

G_BEGIN_DECLS

typedef enum XdCleanupKind {
    XD_CLEANUP_INTERNET,   /* Temporary Internet Files */
    XD_CLEANUP_TRASH,      /* Recycle Bin */
    XD_CLEANUP_TEMP,       /* Temporary files */
    XD_CLEANUP_THUMBNAILS, /* Thumbnails */
    XD_CLEANUP_KIND_COUNT,
} XdCleanupKind;

/* Where a drive's cleanable files are. Every list is nullptr-terminated
 * and may be empty. */
typedef struct XdCleanupPlaces {
    char **browser_caches; /* folders whose contents go */
    char **trashes;        /* trash folders (files/, info/ inside) */
    char **temp_dirs;      /* only the user's week-old entries go */
    char *thumbnails;      /* the thumbnail cache, or nullptr */
} XdCleanupPlaces;

/* The places on the drive whose root is `root`, out of every drive's
 * root (`roots`): a place is on the drive with the longest root that
 * contains it. The user's places come from `data_home` (its Trash),
 * `cache_home` (thumbnails, browsers' caches) and `temp_dirs` (only
 * those that exist); another drive's trash is its .Trash-UID or
 * .Trash/UID. Free with xd_cleanup_places_free. */
XdCleanupPlaces *xd_cleanup_places_new(const char *root, const char *const *roots,
                                       const char *data_home, const char *cache_home,
                                       const char *const *temp_dirs, uid_t uid);
void xd_cleanup_places_free(XdCleanupPlaces *p);

typedef struct XdCleanupCategory {
    XdCleanupKind kind;
    const char *title;       /* "Temporary Internet Files" */
    const char *description; /* XP's text below the list */
    gboolean checked;        /* ticked to start with, as XP */
    char *view;              /* View Files' folder (a path or URI), or nullptr */
    GPtrArray *roots;        /* char*: the folders the items are in */
    GPtrArray *items;        /* char*: what goes (each deleted whole) */
    guint64 size;            /* bytes on disk */
    gint64 age;              /* temporary files: untouched this long (µs) */
} XdCleanupCategory;

/* The drive's categories, with what each would delete: the Recycle Bin
 * always; Temporary files and Thumbnails where the drive holds them;
 * Temporary Internet Files only where a browser's cache was found.
 * `now` (µs, g_get_real_time) says what's a week old. `scanning`, if
 * given, is set to each category's kind as it starts. Free with
 * g_ptr_array_unref. */
GPtrArray *xd_cleanup_scan(const XdCleanupPlaces *places, uid_t uid, gint64 now,
                           GCancellable *cancel, gint *scanning);

/* Housekeeping, as GNOME's gsd-housekeeping does it for the Privacy
 * settings (org.gnome.desktop.privacy remove-old-trash-files,
 * remove-old-temp-files, old-files-age): with `trash`, the Recycle Bin's
 * items deleted more than `age` (µs) before `now` by their .trashinfo's
 * DeletionDate (the item and its .trashinfo; an item without a readable
 * date stays); with `temp`, the temporary files untouched for `age`,
 * under Disk Cleanup's rules. The categories, as xd_cleanup_scan's, for
 * xd_cleanup_delete. Free with g_ptr_array_unref. */
GPtrArray *xd_cleanup_scan_old(const XdCleanupPlaces *places, uid_t uid, gint64 now, gint64 age,
                               gboolean trash, gboolean temp, GCancellable *cancel);

/* Deletes a category's items (checked again: still the user's, still in
 * the category's folders, not through links; temporary files still
 * untouched for the category's age at `now`). Returns FALSE and sets
 * `err` to the first failure, after trying everything. */
gboolean xd_cleanup_delete(const XdCleanupCategory *c, uid_t uid, gint64 now,
                           GCancellable *cancel, GError **err);

/* "0 KB", "1 KB", "12,345 KB": sizes as Disk Cleanup shows them (whole
 * kilobytes, rounded up). Free with g_free. */
char *xd_cleanup_format_kb(guint64 bytes);

G_END_DECLS

#endif
