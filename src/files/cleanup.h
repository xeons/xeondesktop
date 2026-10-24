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
 *
 * Linux's own, which Windows has no counterpart for: graphics drivers'
 * shader caches, developer tools' download caches, and the rest of
 * $XDG_CACHE_HOME (programs' caches); Flatpak runtimes nothing uses
 * (flatpakclean.h) and, with an administrator's password, the package
 * cache, old system logs and crash dumps (syscleanup.h) are categories
 * here too, found and cleaned by those modules.
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
    XD_CLEANUP_SHADERS,    /* Graphics shader caches */
    XD_CLEANUP_DEVELOPER,  /* Developer tool caches */
    XD_CLEANUP_PROGRAMS,   /* Program caches: the rest of $XDG_CACHE_HOME */
    XD_CLEANUP_FLATPAK,    /* Unused Flatpak runtimes (flatpakclean.h) */
    XD_CLEANUP_PACKAGES,   /* system files (syscleanup.h): the package cache */
    XD_CLEANUP_LOGS,       /* system files: old system logs */
    XD_CLEANUP_CRASHES,    /* system files: crash dumps */
    XD_CLEANUP_KIND_COUNT,
} XdCleanupKind;

/* Whether the kind is one of the system files, which only an
 * administrator may clean (syscleanup.h). */
gboolean xd_cleanup_kind_is_system(XdCleanupKind kind);
/* "Temporary Internet Files": the kind's name in the list. */
const char *xd_cleanup_kind_title(XdCleanupKind kind);

/* Where a drive's cleanable files are. Every list is nullptr-terminated
 * and may be empty. */
typedef struct XdCleanupPlaces {
    char **browser_caches;   /* folders whose contents go */
    char **trashes;          /* trash folders (files/, info/ inside) */
    char **temp_dirs;        /* only the user's week-old entries go */
    char *thumbnails;        /* the thumbnail cache, or nullptr */
    char **shader_caches;    /* folders whose contents go */
    char **developer_caches; /* folders whose contents go */
    char *program_caches;    /* $XDG_CACHE_HOME (what the others don't take), or nullptr */
    char *root;              /* the drive's root, and every drive's */
    char **roots;
} XdCleanupPlaces;

/* The places on the drive whose root is `root`, out of every drive's
 * root (`roots`): a place is on the drive with the longest root that
 * contains it. The user's places come from `data_home` (its Trash),
 * `cache_home` (thumbnails, browsers', shader, developer tools' and
 * programs' caches), `home` (~/.nv, ~/.npm, ~/.cargo, ~/.gradle; may be
 * nullptr) and `temp_dirs` (only those that exist); another drive's
 * trash is its .Trash-UID or .Trash/UID. Free with
 * xd_cleanup_places_free. */
XdCleanupPlaces *xd_cleanup_places_new(const char *root, const char *const *roots,
                                       const char *data_home, const char *cache_home,
                                       const char *home, const char *const *temp_dirs,
                                       uid_t uid);
/* Whether `path` is on the places' drive (no other drive inside it holds
 * it): where a Flatpak installation's or the system's files count. */
gboolean xd_cleanup_places_holds(const XdCleanupPlaces *cleanup_places, const char *path);
void xd_cleanup_places_free(XdCleanupPlaces *cleanup_places);

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
    /* What a module that isn't about files keeps (Flatpak's refs). */
    gpointer extra;
    GDestroyNotify extra_free;
} XdCleanupCategory;

/* An empty category of the kind, with its title, description and box,
 * for the arrays xd_cleanup_scan returns (which free it). */
XdCleanupCategory *xd_cleanup_category_new(XdCleanupKind kind);

/* The drive's categories, with what each would delete: the Recycle Bin
 * always; Temporary files and Thumbnails where the drive holds them;
 * Temporary Internet Files only where a browser's cache was found; the
 * shader, developer tools' and programs' caches where there are any.
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
gboolean xd_cleanup_delete(const XdCleanupCategory *cleanup_category, uid_t uid, gint64 now,
                           GCancellable *cancel, GError **err);

/* "0 KB", "1 KB", "12,345 KB": sizes as Disk Cleanup shows them (whole
 * kilobytes, rounded up). Free with g_free. */
char *xd_cleanup_format_kb(guint64 bytes);

G_END_DECLS

#endif
