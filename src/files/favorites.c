/* SPDX-License-Identifier: MIT
 * Favorites: GTK's bookmarks file. See favorites.h.
 */
#include "favorites.h"

#include <glib/gstdio.h>
#include <string.h>

char *xd_favorites_path(void) {
    return g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", nullptr);
}

static void favorite_free(gpointer p) {
    XdFavorite *f = p;
    g_free(f->uri);
    g_free(f->label);
    g_free(f);
}

/* The file's lines (without their ends); none when it's missing. */
static GPtrArray *read_lines(const char *path) {
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    char *text = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path, &text, &len, nullptr))
        return lines;
    char **split = g_strsplit(text, "\n", -1);
    guint n = g_strv_length(split);
    /* The last line's end leaves an empty piece after it. */
    if (n > 0 && split[n - 1][0] == '\0')
        n--;
    for (guint i = 0; i < n; i++)
        g_ptr_array_add(lines, g_strdup(split[i]));
    g_strfreev(split);
    g_free(text);
    return lines;
}

static gboolean write_lines(const char *path, GPtrArray *lines, GError **err) {
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GString *out = g_string_new(nullptr);
    for (guint i = 0; i < lines->len; i++) {
        g_string_append(out, g_ptr_array_index(lines, i));
        g_string_append_c(out, '\n');
    }
    gboolean ok = g_file_set_contents(path, out->str, (gssize)out->len, err);
    g_string_free(out, TRUE);
    return ok;
}

/* A bookmark's line: its URI (and label); FALSE for any other line. */
static gboolean parse_line(const char *line, char **uri, char **label) {
    if (line[0] == '\0' || line[0] == '#')
        return FALSE;
    const char *space = strchr(line, ' ');
    char *u = space != nullptr ? g_strndup(line, (gsize)(space - line)) : g_strdup(line);
    g_strchomp(u); /* a "\r" from a file edited elsewhere */
    if (u[0] == '\0' || g_uri_peek_scheme(u) == nullptr) {
        g_free(u);
        return FALSE;
    }
    if (uri != nullptr)
        *uri = u;
    else
        g_free(u);
    if (label != nullptr) {
        char *l = space != nullptr ? g_strchomp(g_strdup(space + 1)) : nullptr;
        if (l != nullptr && l[0] == '\0')
            g_clear_pointer(&l, g_free);
        *label = l;
    }
    return TRUE;
}

/* The index of `uri`'s line, or -1. */
static int find_line(GPtrArray *lines, const char *uri) {
    for (guint i = 0; i < lines->len; i++) {
        char *u = nullptr;
        gboolean match = parse_line(g_ptr_array_index(lines, i), &u, nullptr) &&
                         strcmp(u, uri) == 0;
        g_free(u);
        if (match)
            return (int)i;
    }
    return -1;
}

/* "URI label", with the label on one line. */
static char *make_line(const char *uri, const char *label) {
    if (label == nullptr || label[0] == '\0')
        return g_strdup(uri);
    char *flat = g_strdelimit(g_strdup(label), "\r\n", ' ');
    char *line = g_strconcat(uri, " ", flat, nullptr);
    g_free(flat);
    return line;
}

GPtrArray *xd_favorites_load(const char *path) {
    GPtrArray *out = g_ptr_array_new_with_free_func(favorite_free);
    GPtrArray *lines = read_lines(path);
    for (guint i = 0; i < lines->len; i++) {
        XdFavorite f = {0};
        if (parse_line(g_ptr_array_index(lines, i), &f.uri, &f.label))
            g_ptr_array_add(out, g_memdup2(&f, sizeof(f)));
    }
    g_ptr_array_unref(lines);
    return out;
}

char *xd_favorite_name(const XdFavorite *f) {
    if (f->label != nullptr)
        return g_strdup(f->label);
    GFile *file = g_file_new_for_uri(f->uri);
    char *name = nullptr;
    char *path = g_file_get_path(file);
    if (path != nullptr) {
        name = g_filename_display_basename(path);
        g_free(path);
    } else {
        /* sftp://host/dir: the folder's name; sftp://host/: the host. */
        char *base = g_file_get_basename(file);
        if (base != nullptr && strcmp(base, "/") != 0) {
            name = g_uri_unescape_string(base, nullptr);
        } else {
            GUri *u = g_uri_parse(f->uri, G_URI_FLAGS_NONE, nullptr);
            if (u != nullptr && g_uri_get_host(u) != nullptr)
                name = g_strdup(g_uri_get_host(u));
            g_clear_pointer(&u, g_uri_unref);
        }
        g_free(base);
    }
    g_object_unref(file);
    return name != nullptr ? name : g_strdup(f->uri);
}

gboolean xd_favorites_add(const char *path, const char *uri, const char *label, GError **err) {
    GPtrArray *lines = read_lines(path);
    int at = find_line(lines, uri);
    char *line = make_line(uri, label);
    if (at >= 0) {
        g_free(g_ptr_array_index(lines, (guint)at));
        g_ptr_array_index(lines, (guint)at) = line;
    } else {
        g_ptr_array_add(lines, line);
    }
    gboolean ok = write_lines(path, lines, err);
    g_ptr_array_unref(lines);
    return ok;
}

gboolean xd_favorites_rename(const char *path, const char *uri, const char *label, GError **err) {
    GPtrArray *lines = read_lines(path);
    int at = find_line(lines, uri);
    gboolean ok = TRUE;
    if (at >= 0) {
        g_free(g_ptr_array_index(lines, (guint)at));
        g_ptr_array_index(lines, (guint)at) = make_line(uri, label);
        ok = write_lines(path, lines, err);
    }
    g_ptr_array_unref(lines);
    return ok;
}

gboolean xd_favorites_remove(const char *path, const char *uri, GError **err) {
    GPtrArray *lines = read_lines(path);
    int at = find_line(lines, uri);
    gboolean ok = TRUE;
    if (at >= 0) {
        g_ptr_array_remove_index(lines, (guint)at);
        ok = write_lines(path, lines, err);
    }
    g_ptr_array_unref(lines);
    return ok;
}

typedef struct ranked {
    char *line;
    guint rank;
} ranked;

static int compare_ranked(gconstpointer a, gconstpointer b) {
    guint ra = ((const ranked *)a)->rank, rb = ((const ranked *)b)->rank;
    return (ra > rb) - (ra < rb);
}

gboolean xd_favorites_reorder(const char *path, const char *const *uris, GError **err) {
    GPtrArray *lines = read_lines(path);
    guint named = uris != nullptr ? g_strv_length((char **)(uintptr_t)uris) : 0;
    /* The bookmarks' slots, and their lines ranked: named ones by their
     * place in `uris`, the rest after them in their own order. */
    GArray *slots = g_array_new(FALSE, FALSE, sizeof(guint));
    GArray *marks = g_array_new(FALSE, FALSE, sizeof(ranked));
    for (guint i = 0; i < lines->len; i++) {
        char *u = nullptr;
        if (!parse_line(g_ptr_array_index(lines, i), &u, nullptr))
            continue;
        guint rank = named + i;
        for (guint k = 0; k < named; k++)
            if (strcmp(uris[k], u) == 0) {
                rank = k;
                break;
            }
        g_free(u);
        g_array_append_val(slots, i);
        ranked r = {g_ptr_array_index(lines, i), rank};
        g_array_append_val(marks, r);
    }
    g_array_sort(marks, compare_ranked);
    for (guint k = 0; k < slots->len; k++)
        g_ptr_array_index(lines, g_array_index(slots, guint, k)) =
            g_array_index(marks, ranked, k).line;
    gboolean ok = write_lines(path, lines, err);
    g_array_unref(slots);
    g_array_unref(marks);
    g_ptr_array_unref(lines);
    return ok;
}
