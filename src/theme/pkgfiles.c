/* SPDX-License-Identifier: MIT
 * A theme package's files as files: what Display Properties' Themes tab
 * unpacks (the wallpaper, sounds and cursors a theme brings) and the
 * package its Save As... writes.
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#if XD_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include "pkg.h"

/* As the loader's caps (pkg.c): a theme's files are small. */
enum { MEMBER_MAX = 64 << 20, TOTAL_MAX = 256 << 20 };

GBytes *xd_pkg_read_folder_file(const char *root, const char *member, gsize max, GError **error) {
    int at = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (at < 0) {
        GFileError code = g_file_error_from_errno(errno);
        g_set_error(error, G_FILE_ERROR, (gint)code, "%s: %s", root,
                    g_strerror(errno));
        return nullptr;
    }
    char **parts = g_strsplit(member, "/", -1);
    GBytes *out = nullptr;
    int err = 0;
    for (size_t i = 0; parts[i] != nullptr && err == 0; i++) {
        bool last = parts[i + 1] == nullptr;
        int next = openat(at, parts[i],
                          last ? O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC
                               : O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            err = errno;
            break;
        }
        close(at);
        at = next;
        if (!last)
            continue;
        struct stat st;
        if (fstat(at, &st) != 0) {
            err = errno;
        } else if (!S_ISREG(st.st_mode)) {
            err = EINVAL;
        } else if ((guint64)st.st_size > max) {
            err = EFBIG;
        } else {
            gsize size = (gsize)st.st_size, got = 0;
            char *data = g_malloc(size + 1);
            while (got < size) {
                ssize_t count = read(at, data + got, size - got);
                if (count <= 0)
                    break;
                got += (gsize)count;
            }
            out = g_bytes_new_take(data, got);
        }
    }
    close(at);
    g_strfreev(parts);
    if (out == nullptr) {
        GFileError code = g_file_error_from_errno(err != 0 ? err : ENOENT);
        g_set_error(error, G_FILE_ERROR, (gint)code, "%s/%s: %s", root, member,
                    g_strerror(err != 0 ? err : ENOENT));
    }
    return out;
}

static void errno_error(GError **error, const char *path) {
    int saved_errno = errno;
    GFileError code = g_file_error_from_errno(saved_errno);
    g_set_error(error, G_FILE_ERROR, (gint)code, "%s: %s", path, g_strerror(saved_errno));
}

/* Called for each file whose path `want` accepts; false stops. */
typedef bool (*member_fn)(const char *name, GBytes *data, void *user);
typedef bool (*want_fn)(const char *name, const void *user);

/* A member path as the loader takes them: relative, no "..", no empty
 * parts. */
static bool member_ok(const char *name) {
    if (name == nullptr || name[0] == '\0' || name[0] == '/')
        return false;
    char **parts = g_strsplit(name, "/", -1);
    bool ok = true;
    for (char **part = parts; *part != nullptr && ok; part++)
        ok = **part != '\0' && strcmp(*part, ".") != 0 && strcmp(*part, "..") != 0;
    g_strfreev(parts);
    return ok;
}

static bool walk_folder(const char *root, const char *rel, want_fn want, const void *wu,
                        member_fn fn, void *user, gsize *total, GError **error) {
    char *dir = rel[0] != '\0' ? g_build_filename(root, rel, nullptr) : g_strdup(root);
    GDir *folder_dir = g_dir_open(dir, 0, error);
    g_free(dir);
    if (folder_dir == nullptr)
        return false;
    bool ok = true;
    const char *entry_name;
    while (ok && (entry_name = g_dir_read_name(folder_dir)) != nullptr) {
        char *name =
            rel[0] != '\0' ? g_strconcat(rel, "/", entry_name, nullptr) : g_strdup(entry_name);
        char *full = g_build_filename(root, name, nullptr);
        /* Links are skipped, as a zip's are: one could lead out of the
         * package. */
        struct stat st;
        bool known = g_lstat(full, &st) == 0;
        if (known && S_ISDIR(st.st_mode)) {
            ok = walk_folder(root, name, want, wu, fn, user, total, error);
        } else if (known && S_ISREG(st.st_mode) && want(name, wu)) {
            GBytes *bytes = xd_pkg_read_folder_file(root, name, MEMBER_MAX, error);
            ok = bytes != nullptr;
            if (ok && *total + g_bytes_get_size(bytes) > TOTAL_MAX) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: too large", full);
                ok = false;
            } else if (ok) {
                *total += g_bytes_get_size(bytes);
                ok = fn(name, bytes, user);
            }
            if (bytes != nullptr)
                g_bytes_unref(bytes);
        }
        g_free(full);
        g_free(name);
    }
    g_dir_close(folder_dir);
    return ok;
}

/* Each file of the package at `path` that `want` accepts, to `fn`. */
static bool for_members(const char *path, want_fn want, const void *wu, member_fn fn, void *user,
                        GError **error) {
    gsize total = 0;
    if (g_file_test(path, G_FILE_TEST_IS_DIR))
        return walk_folder(path, "", want, wu, fn, user, &total, error);
#if XD_HAVE_LIBARCHIVE
    struct archive *reader = archive_read_new();
    archive_read_support_format_zip(reader);
    bool ok = archive_read_open_filename(reader, path, 64 * 1024) == ARCHIVE_OK;
    bool failed = !ok;
    struct archive_entry *archive_member;
    while (ok && archive_read_next_header(reader, &archive_member) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(archive_member);
        la_int64_t size = archive_entry_size(archive_member);
        if (archive_entry_filetype(archive_member) != AE_IFREG || !member_ok(name) ||
            !want(name, wu) || size < 0 || size > MEMBER_MAX || total + (gsize)size > TOTAL_MAX) {
            archive_read_data_skip(reader);
            continue;
        }
        char *buf = g_malloc((gsize)size + 1);
        if (archive_read_data(reader, buf, (size_t)size) != size) {
            g_free(buf);
            failed = true;
            break;
        }
        total += (gsize)size;
        GBytes *bytes = g_bytes_new_take(buf, (gsize)size);
        ok = fn(name, bytes, user);
        g_bytes_unref(bytes);
    }
    if (failed)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s", path,
                    archive_error_string(reader) != nullptr ? archive_error_string(reader)
                                                            : "not a theme package");
    archive_read_free(reader);
    return ok && !failed;
#else
    (void)want;
    (void)wu;
    (void)fn;
    (void)user;
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSYS,
                "%s: theme packages must be folders (built without libarchive)", path);
    return false;
#endif
}

static bool want_all(const char *name, [[maybe_unused]] const void *user) {
    return member_ok(name);
}

static bool want_one(const char *name, const void *user) { return strcmp(name, user) == 0; }

/* `name` is the folder `user` or below it. */
static bool want_below(const char *name, const void *user) {
    const char *prefix = user;
    size_t prefix_len = strlen(prefix);
    return member_ok(name) && (strcmp(name, prefix) == 0 ||
                               (strncmp(name, prefix, prefix_len) == 0 && name[prefix_len] == '/'));
}

static bool keep_one(const char *name, GBytes *data, void *user) {
    (void)name;
    *(GBytes **)user = g_bytes_ref(data);
    return false; /* found: done */
}

GBytes *xd_pkg_read_member(const char *path, const char *member, GError **error) {
    GBytes *out = nullptr;
    GError *err = nullptr;
    if (!member_ok(member)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: bad path %s", path, member);
        return nullptr;
    }
    if (g_file_test(path, G_FILE_TEST_IS_DIR))
        return xd_pkg_read_folder_file(path, member, MEMBER_MAX, error);
    (void)for_members(path, want_one, member, keep_one, &out, &err);
    if (out == nullptr) {
        if (err != nullptr)
            g_propagate_error(error, err);
        else
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "%s: no %s", path, member);
    } else {
        g_clear_error(&err);
    }
    return out;
}

static bool keep_all(const char *name, GBytes *data, void *user) {
    g_hash_table_insert(user, g_strdup(name), g_bytes_ref(data));
    return true;
}

GHashTable *xd_pkg_read_members(const char *path, GError **error) {
    GHashTable *files =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);
    if (!for_members(path, want_all, nullptr, keep_all, files, error)) {
        g_hash_table_unref(files);
        return nullptr;
    }
    return files;
}

typedef struct extract_ctx {
    const char *member;
    const char *dest;
    size_t written;
    GError **error;
} extract_ctx;

static bool write_one(const char *name, GBytes *data, void *user) {
    extract_ctx *extract = user;
    /* A file by its name; a folder's files by their paths below it. */
    size_t member_len = strlen(extract->member);
    const char *rel = name[member_len] == '/' ? name + member_len + 1 : strrchr(name, '/');
    if (rel == nullptr)
        rel = name;
    else if (rel[0] == '/')
        rel++;
    char *full = g_build_filename(extract->dest, rel, nullptr);
    char *dir = g_path_get_dirname(full);
    gsize len = 0;
    const char *bytes = g_bytes_get_data(data, &len);
    bool ok = g_mkdir_with_parents(dir, 0755) == 0;
    if (!ok)
        errno_error(extract->error, dir);
    ok =
        ok && g_file_set_contents(full, bytes != nullptr ? bytes : "", (gssize)len, extract->error);
    extract->written += ok;
    g_free(dir);
    g_free(full);
    return ok;
}

bool xd_pkg_extract(const char *path, const char *member, const char *dest, GError **error) {
    if (!member_ok(member)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: bad path %s", path, member);
        return false;
    }
    extract_ctx extract = {.member = member, .dest = dest, .error = error};
    if (!for_members(path, want_below, member, write_one, &extract, error))
        return false;
    if (extract.written == 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "%s: no %s", path, member);
        return false;
    }
    return true;
}

/* theme.xml first, as xd-theme-import writes them: a reader finds it at
 * once. */
static int theme_xml_first(gconstpointer left, gconstpointer right) {
    const char *left_name = *(const char *const *)left, *right_name = *(const char *const *)right;
    bool tx = strcmp(left_name, "theme.xml") == 0, ty = strcmp(right_name, "theme.xml") == 0;
    return tx != ty ? (tx ? -1 : 1) : strcmp(left_name, right_name);
}

bool xd_pkg_write_zip(const char *dest, GHashTable *files, GError **error) {
#if XD_HAVE_LIBARCHIVE
    char *dir = g_path_get_dirname(dest);
    char *base = g_path_get_basename(dest);
    char *part_name = g_strconcat(".", base, ".part", nullptr);
    char *part = g_build_filename(dir, part_name, nullptr);
    g_free(part_name);
    g_free(base);
    g_free(dir);
    guint count = 0;
    gpointer *names = g_hash_table_get_keys_as_array(files, &count);
    qsort(names, count, sizeof(*names), theme_xml_first);
    struct archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    bool ok = archive_write_open_filename(writer, part) == ARCHIVE_OK;
    for (guint i = 0; ok && i < count; i++) {
        GBytes *bytes = g_hash_table_lookup(files, names[i]);
        gsize len = 0;
        const void *data = g_bytes_get_data(bytes, &len);
        struct archive_entry *archive_member = archive_entry_new();
        archive_entry_set_pathname(archive_member, names[i]);
        archive_entry_set_filetype(archive_member, AE_IFREG);
        archive_entry_set_perm(archive_member, 0644);
        archive_entry_set_size(archive_member, (la_int64_t)len);
        ok = member_ok(names[i]) && archive_write_header(writer, archive_member) == ARCHIVE_OK &&
             (len == 0 || archive_write_data(writer, data, len) == (la_ssize_t)len);
        archive_entry_free(archive_member);
    }
    ok = archive_write_close(writer) == ARCHIVE_OK && ok;
    if (!ok)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s", dest,
                    archive_error_string(writer) != nullptr ? archive_error_string(writer)
                                                            : "cannot be written");
    archive_write_free(writer);
    g_free(names);
    if (ok && g_rename(part, dest) != 0) {
        errno_error(error, dest);
        ok = false;
    }
    if (!ok)
        (void)g_remove(part);
    g_free(part);
    return ok;
#else
    (void)files;
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSYS,
                "%s: built without libarchive, which writes .xdtheme files", dest);
    return false;
#endif
}
