/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>

#include "thumbcache.h"

/* Failure markers are per program and version: a newer version (or another
 * thumbnailer) may well succeed where this one failed. */
#define FAIL_DIR "xd-thumbnailer-" XD_VERSION

static const struct {
    const char *name;
    int px;
} sizes[XD_THUMB_SIZES] = {
    [XD_THUMB_NORMAL] = {"normal", 128},
    [XD_THUMB_LARGE] = {"large", 256},
    [XD_THUMB_XLARGE] = {"x-large", 512},
    [XD_THUMB_XXLARGE] = {"xx-large", 1024},
};

int xd_thumb_size_px(XdThumbSize size) { return sizes[size].px; }

const char *xd_thumb_size_name(XdThumbSize size) { return sizes[size].name; }

gboolean xd_thumb_size_parse(const char *name, XdThumbSize *size) {
    for (int s = 0; s < XD_THUMB_SIZES; s++) {
        if (g_strcmp0(name, sizes[s].name) == 0) {
            *size = (XdThumbSize)s;
            return TRUE;
        }
    }
    return FALSE;
}

XdThumbSize xd_thumb_size_for(int px) {
    for (int s = 0; s < XD_THUMB_SIZES; s++)
        if (px <= sizes[s].px)
            return (XdThumbSize)s;
    return XD_THUMB_XXLARGE;
}

char *xd_thumb_cache_dir(void) { return g_build_filename(g_get_user_cache_dir(), "thumbnails", nullptr); }

/* The spec's file name: the MD5 of the full URI, in lowercase hex. */
static char *hashed_name(const char *uri) {
    char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    char *name = g_strconcat(md5, ".png", nullptr);
    g_free(md5);
    return name;
}

char *xd_thumb_path(const char *root, const char *uri, XdThumbSize size) {
    char *name = hashed_name(uri);
    char *path = g_build_filename(root, sizes[size].name, name, nullptr);
    g_free(name);
    return path;
}

char *xd_thumb_fail_path(const char *root, const char *uri) {
    char *name = hashed_name(uri);
    char *path = g_build_filename(root, "fail", FAIL_DIR, name, nullptr);
    g_free(name);
    return path;
}

gboolean xd_thumb_in_cache(const char *root, const char *path) {
    char *r = g_canonicalize_filename(root, nullptr);
    char *p = g_canonicalize_filename(path, nullptr);
    size_t n = strlen(r);
    gboolean inside = strncmp(p, r, n) == 0 && (p[n] == '/' || p[n] == '\0');
    g_free(r);
    g_free(p);
    return inside;
}

gboolean xd_thumb_save(GdkPixbuf *pb, const char *dest, const char *uri, gint64 mtime,
                       gint64 size, const char *mime, GError **err) {
    char *dir = g_path_get_dirname(dest);
    int made = g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    if (made != 0) {
        int e = errno;
        GFileError code = g_file_error_from_errno(e);
        g_set_error(err, G_FILE_ERROR, (int)code, "%s", g_strerror(e));
        return FALSE;
    }
    GStrvBuilder *kb = g_strv_builder_new();
    GStrvBuilder *vb = g_strv_builder_new();
    g_strv_builder_add(kb, "tEXt::Thumb::URI");
    g_strv_builder_add(vb, uri);
    g_strv_builder_add(kb, "tEXt::Thumb::MTime");
    g_strv_builder_take(vb, g_strdup_printf("%" G_GINT64_FORMAT, mtime));
    if (size >= 0) {
        g_strv_builder_add(kb, "tEXt::Thumb::Size");
        g_strv_builder_take(vb, g_strdup_printf("%" G_GINT64_FORMAT, size));
    }
    if (mime != nullptr) {
        g_strv_builder_add(kb, "tEXt::Thumb::Mimetype");
        g_strv_builder_add(vb, mime);
    }
    g_strv_builder_add(kb, "tEXt::Software");
    g_strv_builder_add(vb, "xd-thumbnailer");
    GStrv keys = g_strv_builder_unref_to_strv(kb);
    GStrv values = g_strv_builder_unref_to_strv(vb);
    char *buf = nullptr;
    gsize len = 0;
    gboolean ok = gdk_pixbuf_save_to_bufferv(pb, &buf, &len, "png", keys, values, err);
    /* Written beside it and renamed over it: another program never reads
     * a half-written thumbnail. */
    if (ok)
        ok = g_file_set_contents_full(dest, buf, (gssize)len, G_FILE_SET_CONTENTS_CONSISTENT,
                                      0600, err);
    g_free(buf);
    g_strfreev(keys);
    g_strfreev(values);
    return ok;
}

gboolean xd_thumb_is_current(const char *png, const char *uri, gint64 mtime) {
    if (!g_file_test(png, G_FILE_TEST_IS_REGULAR))
        return FALSE;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(png, nullptr);
    if (pb == nullptr)
        return FALSE;
    const char *u = gdk_pixbuf_get_option(pb, "tEXt::Thumb::URI");
    const char *m = gdk_pixbuf_get_option(pb, "tEXt::Thumb::MTime");
    gboolean current = FALSE;
    if (u != nullptr && m != nullptr && strcmp(u, uri) == 0) {
        gint64 t = 0;
        current = g_ascii_string_to_signed(m, 10, G_MININT64, G_MAXINT64, &t, nullptr) &&
                  t == mtime;
    }
    g_object_unref(pb);
    return current;
}

gboolean xd_thumb_mark_failed(const char *root, const char *uri, gint64 mtime, GError **err) {
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
    gdk_pixbuf_fill(pb, 0);
    char *path = xd_thumb_fail_path(root, uri);
    gboolean ok = xd_thumb_save(pb, path, uri, mtime, -1, nullptr, err);
    g_free(path);
    g_object_unref(pb);
    return ok;
}

gboolean xd_thumb_has_failed(const char *root, const char *uri, gint64 mtime) {
    char *path = xd_thumb_fail_path(root, uri);
    gboolean failed = xd_thumb_is_current(path, uri, mtime);
    g_free(path);
    return failed;
}

/* ---- what can be thumbnailed ---------------------------------------------------- */

/* MIME type -> Exec, as the thumbnailer files list them. */
static GHashTable *thumbnailers;
/* MIME type asked about -> Exec (or "": none), aliases and subtypes
 * resolved. */
static GHashTable *resolved;
/* MIME types gdk-pixbuf reads -> TRUE, and others asked about -> FALSE. */
static GHashTable *pixbuf_types;

static int compare_names(gconstpointer a, gconstpointer b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void read_thumbnailers(const char *data_dir) {
    char *dir_path = g_build_filename(data_dir, "thumbnailers", nullptr);
    GDir *dir = g_dir_open(dir_path, 0, nullptr);
    if (dir == nullptr) {
        g_free(dir_path);
        return;
    }
    /* In name order, so which one wins doesn't depend on the file system. */
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while ((name = g_dir_read_name(dir)) != nullptr)
        if (g_str_has_suffix(name, ".thumbnailer"))
            g_ptr_array_add(names, g_strdup(name));
    g_dir_close(dir);
    g_ptr_array_sort(names, compare_names);
    for (guint i = 0; i < names->len; i++) {
        char *path = g_build_filename(dir_path, g_ptr_array_index(names, i), nullptr);
        GKeyFile *kf = g_key_file_new();
        static const char group[] = "Thumbnailer Entry";
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr)) {
            char *try_exec = g_key_file_get_string(kf, group, "TryExec", nullptr);
            char *exec = g_key_file_get_string(kf, group, "Exec", nullptr);
            char *found = try_exec != nullptr ? g_find_program_in_path(try_exec) : nullptr;
            char **types = g_key_file_get_string_list(kf, group, "MimeType", nullptr, nullptr);
            if (exec != nullptr && (try_exec == nullptr || found != nullptr) && types != nullptr)
                for (char **t = types; *t != nullptr; t++)
                    if (**t != '\0' && !g_hash_table_contains(thumbnailers, *t))
                        g_hash_table_insert(thumbnailers, g_strdup(*t), g_strdup(exec));
            g_strfreev(types);
            g_free(found);
            g_free(exec);
            g_free(try_exec);
        }
        g_key_file_unref(kf);
        g_free(path);
    }
    g_ptr_array_unref(names);
    g_free(dir_path);
}

static void load_thumbnailers(void) {
    if (thumbnailers != nullptr)
        return;
    thumbnailers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    resolved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    read_thumbnailers(g_get_user_data_dir());
    for (const char *const *d = g_get_system_data_dirs(); *d != nullptr; d++)
        read_thumbnailers(*d);
}

const char *xd_thumbnailer_exec(const char *mime) {
    if (mime == nullptr)
        return nullptr;
    load_thumbnailers();
    const char *exec = g_hash_table_lookup(resolved, mime);
    if (exec == nullptr) {
        exec = g_hash_table_lookup(thumbnailers, mime);
        /* An alias or a subtype of a type a thumbnailer lists. */
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, thumbnailers);
        while (exec == nullptr && g_hash_table_iter_next(&iter, &key, &value))
            if (g_content_type_is_a(mime, key))
                exec = value;
        g_hash_table_insert(resolved, g_strdup(mime), g_strdup(exec != nullptr ? exec : ""));
        exec = g_hash_table_lookup(resolved, mime);
    }
    return *exec != '\0' ? exec : nullptr;
}

gboolean xd_thumb_pixbuf_reads(const char *mime) {
    if (mime == nullptr)
        return FALSE;
    if (pixbuf_types == nullptr) {
        pixbuf_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
        GSList *formats = gdk_pixbuf_get_formats();
        for (GSList *f = formats; f != nullptr; f = f->next) {
            if (gdk_pixbuf_format_is_disabled(f->data))
                continue;
            char **types = gdk_pixbuf_format_get_mime_types(f->data);
            for (char **t = types; t != nullptr && *t != nullptr; t++)
                g_hash_table_insert(pixbuf_types, g_strdup(*t), GINT_TO_POINTER(TRUE));
            g_strfreev(types);
        }
        g_slist_free(formats);
    }
    gpointer known;
    if (g_hash_table_lookup_extended(pixbuf_types, mime, nullptr, &known))
        return GPOINTER_TO_INT(known);
    gboolean reads = FALSE;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, pixbuf_types);
    while (!reads && g_hash_table_iter_next(&iter, &key, &value))
        reads = GPOINTER_TO_INT(value) && g_content_type_is_a(mime, key);
    g_hash_table_insert(pixbuf_types, g_strdup(mime), GINT_TO_POINTER(reads));
    return reads;
}

gboolean xd_thumb_supported(const char *mime) {
    return xd_thumb_pixbuf_reads(mime) || xd_thumbnailer_exec(mime) != nullptr;
}
