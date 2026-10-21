/* SPDX-License-Identifier: MIT
 * Extracting archives. See extract.h.
 */
#include "extract.h"

#include <string.h>
#include <sys/stat.h>

#if XD_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

/* What libarchive reads (its formats and filters), as shared-mime-info
 * names them. */
static const char *const archive_types[] = {
    "application/zip",
    "application/x-7z-compressed",
    "application/vnd.rar",
    "application/x-tar",
    "application/x-compressed-tar",
    "application/x-bzip-compressed-tar",
    "application/x-bzip2-compressed-tar",
    "application/x-bzip3-compressed-tar",
    "application/x-xz-compressed-tar",
    "application/x-zstd-compressed-tar",
    "application/x-lzma-compressed-tar",
    "application/x-lzip-compressed-tar",
    "application/x-lz4-compressed-tar",
    "application/x-tarz",
    "application/x-cpio",
    "application/x-archive",
    "application/x-iso9660-image",
    "application/x-cd-image",
    "application/x-xar",
    "application/x-lha",
    "application/vnd.ms-cab-compressed",
    "application/vnd.debian.binary-package",
    "application/x-rpm",
    /* Compressed single files. */
    "application/gzip",
    "application/x-bzip",
    "application/x-bzip2",
    "application/x-bzip3",
    "application/x-xz",
    "application/zstd",
    "application/x-lzma",
    "application/x-lzip",
    "application/x-lz4",
    "application/x-compress",
};

/* Longest first, so ".tar.gz" goes before ".gz". */
static const char *const archive_extensions[] = {
    ".tar.bz2", ".tar.bz3", ".tar.zst", ".tar.lzma", ".tar.lz4", ".tar.gz", ".tar.xz",
    ".tar.lz",  ".tar.Z",   ".tbz2",    ".tzst",     ".tar",     ".tgz",    ".tbz",
    ".txz",     ".tlz",     ".taz",     ".zip",      ".7z",      ".rar",    ".iso",
    ".cpio",    ".cab",     ".xar",     ".lha",      ".lzh",     ".deb",    ".rpm",
    ".gz",      ".bz2",     ".bz3",     ".xz",       ".zst",     ".lzma",   ".lz4",
    ".lz",      ".ar",      ".Z",
};

gboolean xd_extract_available(void) {
    return XD_HAVE_LIBARCHIVE;
}

gboolean xd_is_archive_type(const char *content_type) {
    if (content_type == nullptr)
        return FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(archive_types); i++)
        if (g_content_type_equals(content_type, archive_types[i]))
            return TRUE;
    return FALSE;
}

gboolean xd_is_archive(GFile *file) {
    if (!xd_extract_available() || !g_file_is_native(file))
        return FALSE;
    GFileInfo *info = g_file_query_info(file,
                                        G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE ","
                                        G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    if (info == nullptr)
        return FALSE;
    gboolean yes =
        g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR &&
        xd_is_archive_type(g_file_info_get_attribute_string(
            info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE));
    g_object_unref(info);
    return yes;
}

char *xd_archive_base_name(const char *file_name) {
    size_t len = strlen(file_name);
    for (size_t i = 0; i < G_N_ELEMENTS(archive_extensions); i++) {
        const char *ext = archive_extensions[i];
        size_t n = strlen(ext);
        if (len <= n)
            continue;
        /* Case matters only for compress's ".Z", as it does to gzip. */
        const char *tail = file_name + len - n;
        if (strchr(ext, 'Z') != nullptr ? strcmp(tail, ext) == 0
                                        : g_ascii_strcasecmp(tail, ext) == 0)
            return g_strndup(file_name, len - n);
    }
    const char *dot = strrchr(file_name, '.');
    if (dot != nullptr && dot != file_name && dot[1] != '\0')
        return g_strndup(file_name, (size_t)(dot - file_name));
    return g_strconcat(file_name, " (extracted)", nullptr);
}

char *xd_archive_safe_path(const char *path) {
    if (path == nullptr)
        return nullptr;
    char **parts = g_strsplit(path, "/", -1);
    GString *out = g_string_new(nullptr);
    bool ok = true;
    for (char **p = parts; ok && *p != nullptr; p++) {
        if (**p == '\0' || strcmp(*p, ".") == 0)
            continue;
        if (strcmp(*p, "..") == 0) {
            ok = false;
            break;
        }
        if (out->len > 0)
            g_string_append_c(out, '/');
        g_string_append(out, *p);
    }
    g_strfreev(parts);
    if (!ok || out->len == 0) {
        g_string_free(out, TRUE);
        return nullptr;
    }
    return g_string_free(out, FALSE);
}

#if XD_HAVE_LIBARCHIVE

static gboolean archive_failed(struct archive *a, GError **error) {
    const char *msg = archive_error_string(a);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        msg != nullptr ? msg : "The archive could not be read.");
    return FALSE;
}

/* The entry's path under `into`, or nullptr to leave it out. */
static char *place(const char *into, const char *path) {
    char *safe = xd_archive_safe_path(path);
    if (safe == nullptr)
        return nullptr;
    char *full = g_build_filename(into, safe, nullptr);
    g_free(safe);
    return full;
}

static gboolean copy_data(struct archive *in, struct archive *out, GCancellable *cancel,
                          XdExtractProgress progress, gpointer data, GError **error) {
    for (;;) {
        if (g_cancellable_set_error_if_cancelled(cancel, error))
            return FALSE;
        const void *buf;
        size_t size;
        la_int64_t offset;
        int r = archive_read_data_block(in, &buf, &size, &offset);
        if (r == ARCHIVE_EOF)
            return TRUE;
        if (r < ARCHIVE_WARN)
            return archive_failed(in, error);
        if (archive_write_data_block(out, buf, size, offset) < ARCHIVE_WARN)
            return archive_failed(out, error);
        if (progress != nullptr)
            progress((guint64)MAX(archive_filter_bytes(in, -1), 0), data);
    }
}

gboolean xd_extract_archive(const char *archive, const char *into, const char *raw_name,
                            GCancellable *cancel, XdExtractProgress progress, gpointer data,
                            GError **error) {
    struct archive *in = archive_read_new();
    archive_read_support_filter_all(in);
    archive_read_support_format_all(in);
    /* A compressed file that's no archive: its one entry is the data. */
    archive_read_support_format_raw(in);
    struct archive *out = archive_write_disk_new();
    /* No owners (they're ours), and nothing written through a link the
     * archive itself made. Paths are checked here (place()), since the
     * ones handed over are absolute. */
    archive_write_disk_set_options(out, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                            ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                            ARCHIVE_EXTRACT_SECURE_NODOTDOT);
    gboolean ok = archive_read_open_filename(in, archive, 64 * 1024) == ARCHIVE_OK;
    if (!ok)
        archive_failed(in, error);
    while (ok) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) {
            ok = FALSE;
            break;
        }
        struct archive_entry *entry;
        int r = archive_read_next_header(in, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_WARN) {
            ok = archive_failed(in, error);
            break;
        }
        bool raw = archive_format(in) == ARCHIVE_FORMAT_RAW;
        if (raw) {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
        }
        mode_t type = archive_entry_filetype(entry);
        char *path = place(into, raw ? raw_name : archive_entry_pathname(entry));
        const char *link = archive_entry_hardlink(entry);
        char *link_path = link != nullptr ? place(into, link) : nullptr;
        /* A hard link (no type of its own) only to something already
         * extracted here. */
        bool keep = path != nullptr &&
                    (link != nullptr ? link_path != nullptr
                                     : type == AE_IFREG || type == AE_IFDIR || type == AE_IFLNK);
        if (!keep) {
            g_free(path);
            g_free(link_path);
            archive_read_data_skip(in);
            continue;
        }
        archive_entry_copy_pathname(entry, path);
        if (link_path != nullptr)
            archive_entry_copy_hardlink(entry, link_path);
        archive_entry_set_perm(entry, archive_entry_perm(entry) & 01777);
        g_free(path);
        g_free(link_path);
        if (archive_write_header(out, entry) < ARCHIVE_WARN) {
            ok = archive_failed(out, error);
            break;
        }
        if (type == AE_IFREG || link != nullptr)
            ok = copy_data(in, out, cancel, progress, data, error);
        if (ok && archive_write_finish_entry(out) < ARCHIVE_WARN)
            ok = archive_failed(out, error);
    }
    /* Folders' times and permissions, set last. */
    if (archive_write_close(out) != ARCHIVE_OK && ok)
        ok = archive_failed(out, error);
    archive_write_free(out);
    archive_read_close(in);
    archive_read_free(in);
    return ok;
}

#else

gboolean xd_extract_archive([[maybe_unused]] const char *archive,
                            [[maybe_unused]] const char *into,
                            [[maybe_unused]] const char *raw_name,
                            [[maybe_unused]] GCancellable *cancel,
                            [[maybe_unused]] XdExtractProgress progress,
                            [[maybe_unused]] gpointer data, GError **error) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "xd-files was built without libarchive.");
    return FALSE;
}

#endif
