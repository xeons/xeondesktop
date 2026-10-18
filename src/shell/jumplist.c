/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <string.h>

#include "jumplist.h"

/* Whether a recent-files entry's program is ours. GTK records
 * g_get_prgname(): the executable's name, or for a GApplication its id
 * (the desktop id without ".desktop"). Qt and KDE record the program's
 * name too. The command's first word is compared as well; GTK quotes the
 * whole command ("'name %u'"), so the first word of that is taken again. */
static bool names_program(const char *app, const char *exec, const char *id_stem,
                          const char *exe_base) {
    if ((id_stem != nullptr && g_ascii_strcasecmp(app, id_stem) == 0) ||
        (exe_base != nullptr && g_ascii_strcasecmp(app, exe_base) == 0))
        return true;
    if (exec == nullptr || exe_base == nullptr)
        return false;
    int argc = 0;
    char **argv = nullptr;
    bool same = false;
    if (g_shell_parse_argv(exec, &argc, &argv, nullptr) && argc > 0) {
        char *space = strchr(argv[0], ' ');
        if (space != nullptr)
            *space = '\0';
        char *base = g_path_get_basename(argv[0]);
        same = strcmp(base, exe_base) == 0;
        g_free(base);
    }
    g_strfreev(argv);
    return same;
}

typedef struct used {
    char *uri;
    gint64 when;
} used;

static int newest_first(gconstpointer a, gconstpointer b) {
    const used *x = a, *y = b;
    return (x->when < y->when) - (x->when > y->when);
}

char **xd_jumplist_recent(GBookmarkFile *recent, const char *desktop_id, const char *executable,
                          size_t max) {
    GPtrArray *out = g_ptr_array_new();
    if (recent == nullptr || (desktop_id == nullptr && executable == nullptr) || max == 0) {
        g_ptr_array_add(out, nullptr);
        return (char **)g_ptr_array_free(out, FALSE);
    }
    char *stem = desktop_id == nullptr                         ? nullptr
                 : g_str_has_suffix(desktop_id, ".desktop") ? g_strndup(desktop_id,
                                                                        strlen(desktop_id) - 8)
                                                            : g_strdup(desktop_id);
    char *exe_base = executable != nullptr && executable[0] != '\0'
                         ? g_path_get_basename(executable)
                         : nullptr;
    GArray *found = g_array_new(FALSE, FALSE, sizeof(used));
    gsize n = 0;
    char **uris = g_bookmark_file_get_uris(recent, &n);
    for (gsize i = 0; i < n; i++) {
        gsize napps = 0;
        char **apps = g_bookmark_file_get_applications(recent, uris[i], &napps, nullptr);
        gint64 when = -1;
        for (gsize a = 0; apps != nullptr && a < napps; a++) {
            char *exec = nullptr;
            GDateTime *stamp = nullptr;
            if (!g_bookmark_file_get_application_info(recent, uris[i], apps[a], &exec, nullptr,
                                                      &stamp, nullptr))
                continue;
            if (names_program(apps[a], exec, stem, exe_base))
                when = MAX(when, stamp != nullptr ? g_date_time_to_unix(stamp) : 0);
            g_free(exec);
        }
        g_strfreev(apps);
        if (when < 0)
            continue;
        /* Only local files that are still there, as Windows 7 drops the
         * ones that are gone. */
        char *path = g_filename_from_uri(uris[i], nullptr, nullptr);
        if (path != nullptr && g_file_test(path, G_FILE_TEST_EXISTS))
            g_array_append_val(found, ((used){g_strdup(uris[i]), when}));
        g_free(path);
    }
    g_strfreev(uris);
    g_array_sort(found, newest_first);
    for (guint i = 0; i < found->len; i++) {
        used *u = &g_array_index(found, used, i);
        if (out->len < max)
            g_ptr_array_add(out, u->uri);
        else
            g_free(u->uri);
    }
    g_array_free(found, TRUE);
    g_free(exe_base);
    g_free(stem);
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

bool xd_jumplist_move(void **items, size_t n, size_t from, size_t to) {
    if (from >= n || to >= n || from == to)
        return false;
    void *moving = items[from];
    if (from < to)
        memmove(items + from, items + from + 1, (to - from) * sizeof(*items));
    else
        memmove(items + to + 1, items + to, (from - to) * sizeof(*items));
    items[to] = moving;
    return true;
}

static constexpr char PINNED_KEY[] = "Pinned";

char **xd_jumplist_pinned(GKeyFile *kf, const char *desktop_id) {
    char **list = kf != nullptr && desktop_id != nullptr
                      ? g_key_file_get_string_list(kf, desktop_id, PINNED_KEY, nullptr, nullptr)
                      : nullptr;
    return list != nullptr ? list : g_new0(char *, 1);
}

bool xd_jumplist_set_pinned(GKeyFile *kf, const char *desktop_id, const char *uri, bool pin) {
    char **list = xd_jumplist_pinned(kf, desktop_id);
    GPtrArray *out = g_ptr_array_new();
    bool found = false;
    for (char **u = list; *u != nullptr; u++) {
        if (strcmp(*u, uri) == 0) {
            found = true;
            if (!pin)
                continue;
        }
        g_ptr_array_add(out, *u);
    }
    bool changed = pin != found;
    if (pin && !found)
        g_ptr_array_add(out, (char *)(uintptr_t)uri);
    if (changed && out->len > 0)
        g_key_file_set_string_list(kf, desktop_id, PINNED_KEY, (const char *const *)out->pdata,
                                   out->len);
    else if (changed)
        g_key_file_remove_group(kf, desktop_id, nullptr);
    g_ptr_array_free(out, TRUE);
    g_strfreev(list);
    return changed;
}

void xd_jumplist_drop_pinned(char **uris, char *const *pinned) {
    size_t k = 0;
    for (size_t i = 0; uris[i] != nullptr; i++) {
        bool is_pinned = false;
        for (char *const *p = pinned; *p != nullptr && !is_pinned; p++)
            is_pinned = strcmp(uris[i], *p) == 0;
        if (is_pinned)
            g_free(uris[i]);
        else
            uris[k++] = uris[i];
    }
    for (size_t i = k; uris[i] != nullptr; i++)
        uris[i] = nullptr;
}

size_t xd_jumplist_combine_next(const size_t *windows, const uint64_t *last_used,
                                const bool *combined, size_t n) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        if (combined[i] || windows[i] < 2)
            continue;
        if (best == SIZE_MAX || last_used[i] < last_used[best])
            best = i;
    }
    return best;
}
