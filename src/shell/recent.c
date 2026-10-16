/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../core/alloc.h"
#include "recent.h"

typedef struct entry {
    char *path;
    gint64 when; /* unix seconds of last modification of the bookmark */
} entry;

static int newest_first(const void *a, const void *b) {
    const entry *x = a;
    const entry *y = b;
    return (x->when < y->when) - (x->when > y->when);
}

void xd_recent_load(char ***paths, size_t *count, size_t max) {
    *paths = xd_alloc_array(XD_MAX(max, 1), sizeof(char *));
    *count = 0;

    char *file = g_build_filename(g_get_user_data_dir(), "recently-used.xbel", nullptr);
    GBookmarkFile *bf = g_bookmark_file_new();
    GError *err = nullptr;
    if (!g_bookmark_file_load_from_file(bf, file, &err)) {
        g_clear_error(&err);
        g_bookmark_file_free(bf);
        g_free(file);
        return;
    }

    gsize n = 0;
    gchar **uris = g_bookmark_file_get_uris(bf, &n);
    entry *list = xd_alloc_array(XD_MAX(n, 1), sizeof(*list));
    size_t m = 0;
    for (gsize i = 0; i < n; i++) {
        /* Only local files; g_filename_from_uri rejects everything else. */
        char *path = g_filename_from_uri(uris[i], nullptr, nullptr);
        struct stat st;
        if (path == nullptr || stat(path, &st) != 0) {
            g_free(path);
            continue;
        }
        GDateTime *dt = g_bookmark_file_get_modified_date_time(bf, uris[i], nullptr);
        list[m].path = xd_str_dup_cstr(xd_str_from_cstr(path));
        list[m].when = dt != nullptr ? g_date_time_to_unix(dt) : 0;
        m++;
        g_free(path);
    }
    if (m > 1)
        qsort(list, m, sizeof(*list), newest_first);
    for (size_t i = 0; i < m; i++) {
        if (*count < max)
            (*paths)[(*count)++] = list[i].path;
        else
            xd_free(list[i].path);
    }
    xd_free(list);
    g_strfreev(uris);
    g_bookmark_file_free(bf);
    g_free(file);
}
