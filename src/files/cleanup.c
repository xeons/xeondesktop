/* SPDX-License-Identifier: MIT
 * Disk Cleanup's categories. See cleanup.h.
 *
 * Everything is walked with openat() and friends from the category's
 * folder down, never following a symbolic link and never leaving the
 * file system the walk started on, so a link or a mount inside a
 * temporary folder can't lead the deletion anywhere else.
 */
#include "cleanup.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* XP's "Temporary files": what hasn't been modified in over a week. */
static constexpr gint64 TEMP_AGE = 7 * G_TIME_SPAN_DAY;
/* Deeper than any real cache: a loop or something odd, left alone. */
static constexpr int MAX_DEPTH = 64;

/* Browsers' caches under $XDG_CACHE_HOME: <browser>/<profile>/<cache>.
 * Only these are known to hold nothing but cached web content. */
static const struct {
    const char *browser, *cache;
} browser_caches[] = {
    {"mozilla/firefox", "cache2"},
    {"chromium", "Cache"},
    {"chromium", "Code Cache"},
    {"google-chrome", "Cache"},
    {"google-chrome", "Code Cache"},
    {"BraveSoftware/Brave-Browser", "Cache"},
    {"BraveSoftware/Brave-Browser", "Code Cache"},
    {"microsoft-edge", "Cache"},
    {"microsoft-edge", "Code Cache"},
    {"vivaldi", "Cache"},
    {"vivaldi", "Code Cache"},
};

/* ---- places ------------------------------------------------------------------------ */

/* Whether `path` is `root` or inside it. */
static gboolean path_in(const char *path, const char *root) {
    size_t count = strlen(root);
    if (count > 1 && root[count - 1] == '/')
        count--;
    if (strcmp(root, "/") == 0)
        return path[0] == '/';
    return strncmp(path, root, count) == 0 && (path[count] == '\0' || path[count] == '/');
}

/* Whether `path` is on the drive `root`: no other drive's root inside
 * `root` holds it. */
static gboolean on_drive(const char *path, const char *root, const char *const *roots) {
    if (path == nullptr || !path_in(path, root))
        return FALSE;
    for (const char *const *other_root = roots; other_root != nullptr && *other_root != nullptr;
         other_root++)
        if (strlen(*other_root) > strlen(root) && path_in(*other_root, root) &&
            path_in(path, *other_root))
            return FALSE;
    return TRUE;
}

/* A real folder (not a link to one), owned by `uid` if uid isn't -1. */
static gboolean own_dir(const char *path, uid_t uid) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           (uid == (uid_t)-1 || st.st_uid == uid);
}

static gint by_path(gconstpointer left, gconstpointer right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static char **strv_from(GPtrArray *paths) {
    g_ptr_array_add(paths, nullptr);
    return (char **)g_ptr_array_free(paths, FALSE);
}

XdCleanupPlaces *xd_cleanup_places_new(const char *root, const char *const *roots,
                                       const char *data_home, const char *cache_home,
                                       const char *const *temp_dirs, uid_t uid) {
    XdCleanupPlaces *cleanup_places = g_new0(XdCleanupPlaces, 1);

    GPtrArray *trashes = g_ptr_array_new();
    char *home_trash = data_home != nullptr ? g_build_filename(data_home, "Trash", nullptr)
                                            : nullptr;
    if (home_trash != nullptr && on_drive(home_trash, root, roots) && own_dir(home_trash, uid))
        g_ptr_array_add(trashes, home_trash);
    else
        g_free(home_trash);
    /* Another drive's: where GIO sends that drive's files. */
    char *own = g_strdup_printf(".Trash-%u", (unsigned)uid);
    char *mine = g_strdup_printf("%u", (unsigned)uid);
    char *others[] = {g_build_filename(root, own, nullptr),
                      g_build_filename(root, ".Trash", mine, nullptr)};
    for (size_t i = 0; i < G_N_ELEMENTS(others); i++) {
        if (own_dir(others[i], uid) && on_drive(others[i], root, roots) &&
            !g_ptr_array_find_with_equal_func(trashes, others[i], g_str_equal, nullptr))
            g_ptr_array_add(trashes, others[i]);
        else
            g_free(others[i]);
    }
    g_free(own);
    g_free(mine);
    cleanup_places->trashes = strv_from(trashes);

    GPtrArray *caches = g_ptr_array_new();
    if (cache_home != nullptr && on_drive(cache_home, root, roots)) {
        char *thumbs = g_build_filename(cache_home, "thumbnails", nullptr);
        if (own_dir(thumbs, uid))
            cleanup_places->thumbnails = thumbs;
        else
            g_free(thumbs);
        for (size_t i = 0; i < G_N_ELEMENTS(browser_caches); i++) {
            char *dir = g_build_filename(cache_home, browser_caches[i].browser, nullptr);
            GDir *profiles = own_dir(dir, uid) ? g_dir_open(dir, 0, nullptr) : nullptr;
            const char *name;
            while (profiles != nullptr && (name = g_dir_read_name(profiles)) != nullptr) {
                char *cache = g_build_filename(dir, name, browser_caches[i].cache, nullptr);
                char *profile = g_build_filename(dir, name, nullptr);
                if (own_dir(profile, uid) && own_dir(cache, uid))
                    g_ptr_array_add(caches, cache);
                else
                    g_free(cache);
                g_free(profile);
            }
            if (profiles != nullptr)
                g_dir_close(profiles);
            g_free(dir);
        }
    }
    g_ptr_array_sort(caches, by_path);
    cleanup_places->browser_caches = strv_from(caches);

    GPtrArray *temps = g_ptr_array_new();
    for (const char *const *temp_dir = temp_dirs; temp_dir != nullptr && *temp_dir != nullptr;
         temp_dir++)
        if (own_dir(*temp_dir, (uid_t)-1) && on_drive(*temp_dir, root, roots))
            g_ptr_array_add(temps, g_strdup(*temp_dir));
    cleanup_places->temp_dirs = strv_from(temps);
    return cleanup_places;
}

void xd_cleanup_places_free(XdCleanupPlaces *cleanup_places) {
    if (cleanup_places == nullptr)
        return;
    g_strfreev(cleanup_places->browser_caches);
    g_strfreev(cleanup_places->trashes);
    g_strfreev(cleanup_places->temp_dirs);
    g_free(cleanup_places->thumbnails);
    g_free(cleanup_places);
}

/* ---- walking ----------------------------------------------------------------------- */

typedef struct walk {
    dev_t dev;
    uid_t uid;
    /* Temporary files: everything must be the user's, untouched since
     * `cutoff` (seconds), and no socket, FIFO or device. */
    gboolean strict;
    gint64 cutoff;
    GCancellable *cancel;
    guint64 size;
    gboolean ok;
} walk;

static gint64 newest_time(const struct stat *st) {
    gint64 newest = st->st_mtim.tv_sec;
    newest = MAX(newest, (gint64)st->st_atim.tv_sec);
    return MAX(newest, (gint64)st->st_ctim.tv_sec);
}

static void walk_at(int at, const char *name, walk *walker, int depth) {
    struct stat st;
    if (!walker->ok || g_cancellable_is_cancelled(walker->cancel) ||
        fstatat(at, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        walker->ok = walker->ok && !g_cancellable_is_cancelled(walker->cancel);
        return;
    }
    if (st.st_dev != walker->dev) {
        /* A mount inside: never ours to clean. */
        walker->ok = FALSE;
        return;
    }
    if (walker->strict &&
        (st.st_uid != walker->uid || S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode) ||
         S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || newest_time(&st) >= walker->cutoff)) {
        walker->ok = FALSE;
        return;
    }
    walker->size += (guint64)st.st_blocks * 512;
    if (!S_ISDIR(st.st_mode))
        return;
    if (depth >= MAX_DEPTH) {
        walker->ok = FALSE;
        return;
    }
    int fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dir_stream = fd >= 0 ? fdopendir(fd) : nullptr;
    if (dir_stream == nullptr) {
        if (fd >= 0)
            close(fd);
        /* Unreadable: what's inside can't be judged, or deleted. */
        walker->ok = walker->ok && !walker->strict;
        return;
    }
    struct dirent *dir_entry;
    while (walker->ok && (dir_entry = readdir(dir_stream)) != nullptr)
        if (strcmp(dir_entry->d_name, ".") != 0 && strcmp(dir_entry->d_name, "..") != 0)
            walk_at(dirfd(dir_stream), dir_entry->d_name, walker, depth + 1);
    closedir(dir_stream);
}

/* The items in `dir` (each deleted whole), with their size. */
static void scan_dir(XdCleanupCategory *cleanup_category, const char *dir, uid_t uid,
                     gboolean strict, gint64 now, GCancellable *cancel) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat top;
    if (fd < 0 || fstat(fd, &top) != 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    g_ptr_array_add(cleanup_category->roots, g_strdup(dir));
    DIR *dir_stream = fdopendir(fd);
    if (dir_stream == nullptr) {
        close(fd);
        return;
    }
    struct dirent *dir_entry;
    while (!g_cancellable_is_cancelled(cancel) && (dir_entry = readdir(dir_stream)) != nullptr) {
        if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0)
            continue;
        struct stat st;
        if (fstatat(dirfd(dir_stream), dir_entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            st.st_uid != uid)
            continue;
        walk walker = {.dev = top.st_dev,
                       .uid = uid,
                       .strict = strict,
                       .cutoff = (now - cleanup_category->age) / G_USEC_PER_SEC,
                       .cancel = cancel,
                       .ok = TRUE};
        walk_at(dirfd(dir_stream), dir_entry->d_name, &walker, 0);
        if (!walker.ok)
            continue;
        g_ptr_array_add(cleanup_category->items, g_build_filename(dir, dir_entry->d_name, nullptr));
        cleanup_category->size += walker.size;
    }
    closedir(dir_stream);
}

/* ---- categories ------------------------------------------------------------------- */

static XdCleanupCategory *category_new(XdCleanupKind kind) {
    static const struct {
        const char *title, *description;
        gboolean checked;
    } info[XD_CLEANUP_KIND_COUNT] = {
        [XD_CLEANUP_INTERNET] = {"Temporary Internet Files",
                                 "The Temporary Internet Files folder contains Web pages stored "
                                 "on your hard disk for quick viewing. Your personalized "
                                 "settings for Web pages will be left intact.",
                                 TRUE},
        [XD_CLEANUP_TRASH] = {"Recycle Bin",
                              "The Recycle Bin contains files you have deleted from your "
                              "computer. These files are not permanently removed until you "
                              "empty the Recycle Bin.",
                              FALSE},
        [XD_CLEANUP_TEMP] = {"Temporary files",
                             "Programs sometimes store temporary information in a TEMP folder. "
                             "Before a program closes, it usually deletes this information.\n\n"
                             "You can safely delete temporary files that have not been "
                             "modified in over a week.",
                             FALSE},
        [XD_CLEANUP_THUMBNAILS] = {"Thumbnails",
                                   "XeonDesktop keeps a copy of all of your picture, video, and "
                                   "document thumbnails so they can be displayed quickly when "
                                   "you open a folder. If you delete these thumbnails, they "
                                   "will be automatically recreated as needed.",
                                   TRUE},
    };
    XdCleanupCategory *cleanup_category = g_new0(XdCleanupCategory, 1);
    cleanup_category->kind = kind;
    cleanup_category->title = info[kind].title;
    cleanup_category->description = info[kind].description;
    cleanup_category->checked = info[kind].checked;
    cleanup_category->age = TEMP_AGE;
    cleanup_category->roots = g_ptr_array_new_with_free_func(g_free);
    cleanup_category->items = g_ptr_array_new_with_free_func(g_free);
    return cleanup_category;
}

static void category_free(gpointer user_data) {
    XdCleanupCategory *cleanup_category = user_data;
    g_free(cleanup_category->view);
    g_ptr_array_unref(cleanup_category->roots);
    g_ptr_array_unref(cleanup_category->items);
    g_free(cleanup_category);
}

static void set_scanning(gint *scanning, XdCleanupKind kind) {
    if (scanning != nullptr)
        g_atomic_int_set(scanning, (gint)kind);
}

GPtrArray *xd_cleanup_scan(const XdCleanupPlaces *places, uid_t uid, gint64 now,
                           GCancellable *cancel, gint *scanning) {
    GPtrArray *out = g_ptr_array_new_with_free_func(category_free);
    if (places->browser_caches[0] != nullptr) {
        set_scanning(scanning, XD_CLEANUP_INTERNET);
        XdCleanupCategory *cleanup_category = category_new(XD_CLEANUP_INTERNET);
        for (char **dir = places->browser_caches; *dir != nullptr; dir++)
            scan_dir(cleanup_category, *dir, uid, FALSE, now, cancel);
        cleanup_category->view = g_path_get_dirname(places->browser_caches[0]);
        g_ptr_array_add(out, cleanup_category);
    }

    set_scanning(scanning, XD_CLEANUP_TRASH);
    XdCleanupCategory *trash = category_new(XD_CLEANUP_TRASH);
    for (char **trash_dir = places->trashes; *trash_dir != nullptr; trash_dir++) {
        for (const char *const *sub = (const char *const[]){"files", "info", nullptr};
             *sub != nullptr; sub++) {
            char *dir = g_build_filename(*trash_dir, *sub, nullptr);
            if (own_dir(dir, uid))
                scan_dir(trash, dir, uid, FALSE, now, cancel);
            g_free(dir);
        }
    }
    trash->view = g_strdup("trash:///");
    g_ptr_array_add(out, trash);

    if (places->temp_dirs[0] != nullptr) {
        set_scanning(scanning, XD_CLEANUP_TEMP);
        XdCleanupCategory *cleanup_category = category_new(XD_CLEANUP_TEMP);
        for (char **dir = places->temp_dirs; *dir != nullptr; dir++)
            scan_dir(cleanup_category, *dir, uid, TRUE, now, cancel);
        cleanup_category->view = g_strdup(places->temp_dirs[0]);
        g_ptr_array_add(out, cleanup_category);
    }

    if (places->thumbnails != nullptr) {
        set_scanning(scanning, XD_CLEANUP_THUMBNAILS);
        XdCleanupCategory *cleanup_category = category_new(XD_CLEANUP_THUMBNAILS);
        /* The sizes' folders stay (normal, large, fail/...); what's in
         * them goes. */
        GDir *thumbnail_dir = g_dir_open(places->thumbnails, 0, nullptr);
        const char *name;
        while (thumbnail_dir != nullptr && (name = g_dir_read_name(thumbnail_dir)) != nullptr) {
            char *sub = g_build_filename(places->thumbnails, name, nullptr);
            if (own_dir(sub, uid))
                scan_dir(cleanup_category, sub, uid, FALSE, now, cancel);
            g_free(sub);
        }
        if (thumbnail_dir != nullptr)
            g_dir_close(thumbnail_dir);
        cleanup_category->view = g_strdup(places->thumbnails);
        g_ptr_array_add(out, cleanup_category);
    }
    return out;
}

/* ---- housekeeping ------------------------------------------------------------------ */

/* When a trashed item was deleted (µs), from its .trashinfo (`info`, a
 * name in the folder `at`); -1 if that can't be told. The date is local
 * time without a zone, as the trash specification has it. */
static gint64 deletion_date(int at, const char *info, uid_t uid) {
    struct stat st;
    if (fstatat(at, info, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != uid || st.st_size > 64 * 1024)
        return -1;
    int fd = openat(at, info, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    char *buf = g_malloc((gsize)st.st_size + 1);
    ssize_t bytes_read = read(fd, buf, (size_t)st.st_size);
    close(fd);
    GKeyFile *kf = g_key_file_new();
    char *date = bytes_read > 0 && g_key_file_load_from_data(kf, buf, (gsize)bytes_read,
                                                             G_KEY_FILE_NONE, nullptr)
                     ? g_key_file_get_string(kf, "Trash Info", "DeletionDate", nullptr)
                     : nullptr;
    g_key_file_unref(kf);
    g_free(buf);
    GTimeZone *tz = g_time_zone_new_local();
    GDateTime *dt = date != nullptr ? g_date_time_new_from_iso8601(date, tz) : nullptr;
    g_time_zone_unref(tz);
    g_free(date);
    gint64 when = dt != nullptr ? g_date_time_to_unix_usec(dt) : -1;
    if (dt != nullptr)
        g_date_time_unref(dt);
    return when;
}

/* A trash's items deleted before `cutoff` (µs): each item in files/,
 * then its .trashinfo (alone if the item is gone already). An item
 * that couldn't be judged (another owner, a mount inside) stays with
 * its .trashinfo. */
static void scan_old_trash(XdCleanupCategory *cleanup_category, const char *trash, uid_t uid,
                           gint64 cutoff, GCancellable *cancel) {
    char *files = g_build_filename(trash, "files", nullptr);
    char *info = g_build_filename(trash, "info", nullptr);
    int ifd = own_dir(info, uid) && own_dir(files, uid)
                  ? open(info, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                  : -1;
    int ffd = ifd >= 0 ? open(files, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    struct stat top;
    DIR *dir_stream = ffd >= 0 && fstat(ffd, &top) == 0 ? fdopendir(ifd) : nullptr;
    if (dir_stream == nullptr) {
        if (ifd >= 0)
            close(ifd);
        if (ffd >= 0)
            close(ffd);
        g_free(files);
        g_free(info);
        return;
    }
    g_ptr_array_add(cleanup_category->roots, g_strdup(files));
    g_ptr_array_add(cleanup_category->roots, g_strdup(info));
    const size_t suffix = strlen(".trashinfo");
    struct dirent *dir_entry;
    while (!g_cancellable_is_cancelled(cancel) && (dir_entry = readdir(dir_stream)) != nullptr) {
        if (!g_str_has_suffix(dir_entry->d_name, ".trashinfo") ||
            strlen(dir_entry->d_name) == suffix)
            continue;
        gint64 when = deletion_date(dirfd(dir_stream), dir_entry->d_name, uid);
        if (when < 0 || when >= cutoff)
            continue;
        char *name = g_strndup(dir_entry->d_name, strlen(dir_entry->d_name) - suffix);
        struct stat st;
        if (fstatat(ffd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            walk walker = {.dev = top.st_dev, .uid = uid, .cancel = cancel, .ok = st.st_uid == uid};
            walk_at(ffd, name, &walker, 0);
            if (!walker.ok) {
                g_free(name);
                continue;
            }
            g_ptr_array_add(cleanup_category->items, g_build_filename(files, name, nullptr));
            cleanup_category->size += walker.size;
        }
        g_ptr_array_add(cleanup_category->items,
                        g_build_filename(info, dir_entry->d_name, nullptr));
        g_free(name);
    }
    closedir(dir_stream);
    close(ffd);
    g_free(files);
    g_free(info);
}

GPtrArray *xd_cleanup_scan_old(const XdCleanupPlaces *places, uid_t uid, gint64 now, gint64 age,
                               gboolean trash, gboolean temp, GCancellable *cancel) {
    GPtrArray *out = g_ptr_array_new_with_free_func(category_free);
    if (trash) {
        XdCleanupCategory *cleanup_category = category_new(XD_CLEANUP_TRASH);
        for (char **trash_dir = places->trashes; *trash_dir != nullptr; trash_dir++)
            scan_old_trash(cleanup_category, *trash_dir, uid, now - age, cancel);
        cleanup_category->view = g_strdup("trash:///");
        g_ptr_array_add(out, cleanup_category);
    }
    if (temp && places->temp_dirs[0] != nullptr) {
        XdCleanupCategory *cleanup_category = category_new(XD_CLEANUP_TEMP);
        cleanup_category->age = age;
        for (char **dir = places->temp_dirs; *dir != nullptr; dir++)
            scan_dir(cleanup_category, *dir, uid, TRUE, now, cancel);
        cleanup_category->view = g_strdup(places->temp_dirs[0]);
        g_ptr_array_add(out, cleanup_category);
    }
    return out;
}

/* ---- deleting ---------------------------------------------------------------------- */

static int remove_at(int at, const char *name, dev_t dev, uid_t uid, GCancellable *cancel,
                     int depth) {
    if (g_cancellable_is_cancelled(cancel))
        return ECANCELED;
    struct stat st;
    if (fstatat(at, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : errno;
    if (st.st_dev != dev)
        return EXDEV;
    if (!S_ISDIR(st.st_mode))
        return unlinkat(at, name, 0) == 0 || errno == ENOENT ? 0 : errno;
    if (depth >= MAX_DEPTH)
        return ELOOP;
    int fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == EACCES && st.st_uid == uid &&
        fchmodat(at, name, (st.st_mode & 07777) | S_IRWXU, 0) == 0)
        fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno;
    /* A read-only folder of the user's own (a copied CD, a Go module
     * cache) can still be emptied. */
    if (st.st_uid == uid && (st.st_mode & S_IRWXU) != S_IRWXU)
        (void)fchmod(fd, (st.st_mode & 07777) | S_IRWXU);
    DIR *dir_stream = fdopendir(fd);
    if (dir_stream == nullptr) {
        int error_number = errno;
        close(fd);
        return error_number;
    }
    int first = 0;
    struct dirent *dir_entry;
    while ((dir_entry = readdir(dir_stream)) != nullptr) {
        if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0)
            continue;
        int status = remove_at(dirfd(dir_stream), dir_entry->d_name, dev, uid, cancel, depth + 1);
        if (first == 0)
            first = status;
        if (status == ECANCELED)
            break;
    }
    closedir(dir_stream);
    if (first != 0)
        return first;
    return unlinkat(at, name, AT_REMOVEDIR) == 0 || errno == ENOENT ? 0 : errno;
}

gboolean xd_cleanup_delete(const XdCleanupCategory *cleanup_category, uid_t uid, gint64 now,
                           GCancellable *cancel, GError **err) {
    gboolean ok = TRUE;
    for (guint i = 0; i < cleanup_category->items->len && !g_cancellable_is_cancelled(cancel);
         i++) {
        const char *item = g_ptr_array_index(cleanup_category->items, i);
        char *dir = g_path_get_dirname(item);
        char *base = g_path_get_basename(item);
        int status = 0;
        if (!g_ptr_array_find_with_equal_func(cleanup_category->roots, dir, g_str_equal, nullptr) ||
            strcmp(base, "..") == 0 || strcmp(base, ".") == 0 || strchr(base, '/') != nullptr) {
            status = EPERM; /* not one of the category's: never */
        } else {
            int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            struct stat top, st;
            if (fd < 0 || fstat(fd, &top) != 0) {
                status = errno;
            } else if (fstatat(fd, base, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                status = errno == ENOENT ? 0 : errno; /* gone already */
            } else if (st.st_uid != uid) {
                status = EPERM;
            } else {
                if (cleanup_category->kind == XD_CLEANUP_TEMP) {
                    /* Touched since the scan: in use after all. */
                    walk walker = {.dev = top.st_dev,
                                   .uid = uid,
                                   .strict = TRUE,
                                   .cutoff = (now - cleanup_category->age) / G_USEC_PER_SEC,
                                   .cancel = cancel,
                                   .ok = TRUE};
                    walk_at(fd, base, &walker, 0);
                    if (!walker.ok) {
                        close(fd);
                        g_free(dir);
                        g_free(base);
                        continue;
                    }
                }
                status = remove_at(fd, base, top.st_dev, uid, cancel, 0);
            }
            if (fd >= 0)
                close(fd);
        }
        if (status != 0 && status != ECANCELED && ok) {
            ok = FALSE;
            GIOErrorEnum code = g_io_error_from_errno(status);
            g_set_error(err, G_IO_ERROR, (gint)code, "Cannot delete %s: %s", item,
                        g_strerror(status));
        }
        g_free(dir);
        g_free(base);
    }
    return ok;
}

char *xd_cleanup_format_kb(guint64 bytes) {
    guint64 kb = bytes / 1024 + (bytes % 1024 != 0);
    char digits[32];
    g_snprintf(digits, sizeof digits, "%" G_GUINT64_FORMAT, kb);
    GString *string = g_string_new(nullptr);
    size_t count = strlen(digits);
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && (count - i) % 3 == 0)
            g_string_append_c(string, ',');
        g_string_append_c(string, digits[i]);
    }
    g_string_append(string, " KB");
    return g_string_free(string, FALSE);
}
