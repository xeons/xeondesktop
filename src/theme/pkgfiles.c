/* SPDX-License-Identifier: MIT
 * A theme package's files as files: what Display Properties' Themes tab
 * unpacks (the wallpaper, sounds and cursors a theme brings) and the
 * package its Save As... writes.
 */
#include <errno.h>
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

static void errno_error(GError **error, const char *path) {
    int e = errno;
    GFileError code = g_file_error_from_errno(e);
    g_set_error(error, G_FILE_ERROR, (gint)code, "%s: %s", path, g_strerror(e));
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
    for (char **p = parts; *p != nullptr && ok; p++)
        ok = **p != '\0' && strcmp(*p, ".") != 0 && strcmp(*p, "..") != 0;
    g_strfreev(parts);
    return ok;
}

static bool walk_folder(const char *root, const char *rel, want_fn want, const void *wu,
                        member_fn fn, void *user, gsize *total, GError **error) {
    char *dir = rel[0] != '\0' ? g_build_filename(root, rel, nullptr) : g_strdup(root);
    GDir *d = g_dir_open(dir, 0, error);
    g_free(dir);
    if (d == nullptr)
        return false;
    bool ok = true;
    const char *e;
    while (ok && (e = g_dir_read_name(d)) != nullptr) {
        char *name = rel[0] != '\0' ? g_strconcat(rel, "/", e, nullptr) : g_strdup(e);
        char *full = g_build_filename(root, name, nullptr);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            ok = walk_folder(root, name, want, wu, fn, user, total, error);
        } else if (g_file_test(full, G_FILE_TEST_IS_REGULAR) && want(name, wu)) {
            char *data = nullptr;
            gsize len = 0;
            ok = g_file_get_contents(full, &data, &len, error);
            if (ok && (len > MEMBER_MAX || *total + len > TOTAL_MAX)) {
                g_free(data);
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: too large", full);
                ok = false;
            } else if (ok) {
                *total += len;
                GBytes *b = g_bytes_new_take(data, len);
                ok = fn(name, b, user);
                g_bytes_unref(b);
            }
        }
        g_free(full);
        g_free(name);
    }
    g_dir_close(d);
    return ok;
}

/* Each file of the package at `path` that `want` accepts, to `fn`. */
static bool for_members(const char *path, want_fn want, const void *wu, member_fn fn, void *user,
                        GError **error) {
    gsize total = 0;
    if (g_file_test(path, G_FILE_TEST_IS_DIR))
        return walk_folder(path, "", want, wu, fn, user, &total, error);
#if XD_HAVE_LIBARCHIVE
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    bool ok = archive_read_open_filename(a, path, 64 * 1024) == ARCHIVE_OK;
    bool failed = !ok;
    struct archive_entry *e;
    while (ok && archive_read_next_header(a, &e) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(e);
        la_int64_t size = archive_entry_size(e);
        if (archive_entry_filetype(e) != AE_IFREG || !member_ok(name) || !want(name, wu) ||
            size < 0 || size > MEMBER_MAX || total + (gsize)size > TOTAL_MAX) {
            archive_read_data_skip(a);
            continue;
        }
        char *buf = g_malloc((gsize)size + 1);
        if (archive_read_data(a, buf, (size_t)size) != size) {
            g_free(buf);
            failed = true;
            break;
        }
        total += (gsize)size;
        GBytes *b = g_bytes_new_take(buf, (gsize)size);
        ok = fn(name, b, user);
        g_bytes_unref(b);
    }
    if (failed)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s", path,
                    archive_error_string(a) != nullptr ? archive_error_string(a)
                                                        : "not a theme package");
    archive_read_free(a);
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
    size_t n = strlen(prefix);
    return member_ok(name) &&
           (strcmp(name, prefix) == 0 || (strncmp(name, prefix, n) == 0 && name[n] == '/'));
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
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        char *full = g_build_filename(path, member, nullptr);
        char *data = nullptr;
        gsize len = 0;
        if (g_file_get_contents(full, &data, &len, error))
            out = g_bytes_new_take(data, len);
        g_free(full);
        return out;
    }
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
    extract_ctx *c = user;
    /* A file by its name; a folder's files by their paths below it. */
    size_t n = strlen(c->member);
    const char *rel = name[n] == '/' ? name + n + 1 : strrchr(name, '/');
    if (rel == nullptr)
        rel = name;
    else if (rel[0] == '/')
        rel++;
    char *full = g_build_filename(c->dest, rel, nullptr);
    char *dir = g_path_get_dirname(full);
    gsize len = 0;
    const char *bytes = g_bytes_get_data(data, &len);
    bool ok = g_mkdir_with_parents(dir, 0755) == 0;
    if (!ok)
        errno_error(c->error, dir);
    ok = ok && g_file_set_contents(full, bytes != nullptr ? bytes : "", (gssize)len, c->error);
    c->written += ok;
    g_free(dir);
    g_free(full);
    return ok;
}

bool xd_pkg_extract(const char *path, const char *member, const char *dest, GError **error) {
    if (!member_ok(member)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: bad path %s", path, member);
        return false;
    }
    extract_ctx c = {.member = member, .dest = dest, .error = error};
    if (!for_members(path, want_below, member, write_one, &c, error))
        return false;
    if (c.written == 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "%s: no %s", path, member);
        return false;
    }
    return true;
}

/* theme.xml first, as xd-theme-import writes them: a reader finds it at
 * once. */
static int theme_xml_first(gconstpointer a, gconstpointer b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    bool tx = strcmp(x, "theme.xml") == 0, ty = strcmp(y, "theme.xml") == 0;
    return tx != ty ? (tx ? -1 : 1) : strcmp(x, y);
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
    guint n = 0;
    gpointer *names = g_hash_table_get_keys_as_array(files, &n);
    qsort(names, n, sizeof(*names), theme_xml_first);
    struct archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    bool ok = archive_write_open_filename(a, part) == ARCHIVE_OK;
    for (guint i = 0; ok && i < n; i++) {
        GBytes *b = g_hash_table_lookup(files, names[i]);
        gsize len = 0;
        const void *data = g_bytes_get_data(b, &len);
        struct archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, names[i]);
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_size(e, (la_int64_t)len);
        ok = member_ok(names[i]) && archive_write_header(a, e) == ARCHIVE_OK &&
             (len == 0 || archive_write_data(a, data, len) == (la_ssize_t)len);
        archive_entry_free(e);
    }
    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    if (!ok)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s", dest,
                    archive_error_string(a) != nullptr ? archive_error_string(a)
                                                        : "cannot be written");
    archive_write_free(a);
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
