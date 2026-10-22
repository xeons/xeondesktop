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

/* The program's stem (desktop id without ".desktop") and executable's
 * name, as names_program compares them. g_free both. */
static void program_names(const char *desktop_id, const char *executable, char **stem,
                          char **exe_base) {
    *stem = desktop_id == nullptr                         ? nullptr
            : g_str_has_suffix(desktop_id, ".desktop") ? g_strndup(desktop_id,
                                                                   strlen(desktop_id) - 8)
                                                       : g_strdup(desktop_id);
    *exe_base = executable != nullptr && executable[0] != '\0' ? g_path_get_basename(executable)
                                                                : nullptr;
}

typedef struct used {
    char *uri;
    gint64 when;
} used;

static int newest_first(gconstpointer left, gconstpointer right) {
    const used *left_use = left, *right_use = right;
    return (left_use->when < right_use->when) - (left_use->when > right_use->when);
}

char **xd_jumplist_recent(GBookmarkFile *recent, const char *desktop_id, const char *executable,
                          size_t max) {
    GPtrArray *out = g_ptr_array_new();
    if (recent == nullptr || (desktop_id == nullptr && executable == nullptr) || max == 0) {
        g_ptr_array_add(out, nullptr);
        return (char **)g_ptr_array_free(out, FALSE);
    }
    char *stem = nullptr, *exe_base = nullptr;
    program_names(desktop_id, executable, &stem, &exe_base);
    GArray *found = g_array_new(FALSE, FALSE, sizeof(used));
    gsize count = 0;
    char **uris = g_bookmark_file_get_uris(recent, &count);
    for (gsize i = 0; i < count; i++) {
        gsize napps = 0;
        char **apps = g_bookmark_file_get_applications(recent, uris[i], &napps, nullptr);
        gint64 when = -1;
        for (gsize app_index = 0; apps != nullptr && app_index < napps; app_index++) {
            char *exec = nullptr;
            GDateTime *stamp = nullptr;
            if (!g_bookmark_file_get_application_info(recent, uris[i], apps[app_index], &exec,
                                                      nullptr, &stamp, nullptr))
                continue;
            if (names_program(apps[app_index], exec, stem, exe_base))
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
        used *recent_use = &g_array_index(found, used, i);
        if (out->len < max)
            g_ptr_array_add(out, recent_use->uri);
        else
            g_free(recent_use->uri);
    }
    g_array_free(found, TRUE);
    g_free(exe_base);
    g_free(stem);
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

bool xd_jumplist_forget(GBookmarkFile *recent, const char *uri, const char *desktop_id,
                        const char *executable) {
    if (recent == nullptr || uri == nullptr || (desktop_id == nullptr && executable == nullptr))
        return false;
    char *stem = nullptr, *exe_base = nullptr;
    program_names(desktop_id, executable, &stem, &exe_base);
    gsize napps = 0;
    char **apps = g_bookmark_file_get_applications(recent, uri, &napps, nullptr);
    bool changed = false;
    for (gsize app_index = 0; apps != nullptr && app_index < napps; app_index++) {
        char *exec = nullptr;
        if (!g_bookmark_file_get_application_info(recent, uri, apps[app_index], &exec, nullptr,
                                                  nullptr, nullptr))
            continue;
        if (names_program(apps[app_index], exec, stem, exe_base) &&
            g_bookmark_file_remove_application(recent, uri, apps[app_index], nullptr))
            changed = true;
        g_free(exec);
    }
    g_strfreev(apps);
    g_free(exe_base);
    g_free(stem);
    return changed;
}

bool xd_jumplist_move(void **items, size_t count, size_t from, size_t to) {
    if (from >= count || to >= count || from == to)
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
    for (char **pinned_uri = list; *pinned_uri != nullptr; pinned_uri++) {
        if (strcmp(*pinned_uri, uri) == 0) {
            found = true;
            if (!pin)
                continue;
        }
        g_ptr_array_add(out, *pinned_uri);
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

bool xd_jumplist_move_pinned(GKeyFile *kf, const char *desktop_id, const char *uri,
                             const char *onto) {
    char **list = xd_jumplist_pinned(kf, desktop_id);
    size_t count = g_strv_length(list), from = SIZE_MAX, to = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i], uri) == 0)
            from = i;
        if (strcmp(list[i], onto) == 0)
            to = i;
    }
    bool moved = xd_jumplist_move((void **)list, count, from, to);
    if (moved)
        g_key_file_set_string_list(kf, desktop_id, PINNED_KEY, (const char *const *)list, count);
    g_strfreev(list);
    return moved;
}

void xd_jumplist_drop_pinned(char **uris, char *const *pinned) {
    size_t k = 0;
    for (size_t i = 0; uris[i] != nullptr; i++) {
        bool is_pinned = false;
        for (char *const *pinned_uri = pinned; *pinned_uri != nullptr && !is_pinned; pinned_uri++)
            is_pinned = strcmp(uris[i], *pinned_uri) == 0;
        if (is_pinned)
            g_free(uris[i]);
        else
            uris[k++] = uris[i];
    }
    for (size_t i = k; uris[i] != nullptr; i++)
        uris[i] = nullptr;
}

size_t xd_jumplist_combine_next(const size_t *windows, const uint64_t *last_used,
                                const bool *combined, size_t count) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (combined[i] || windows[i] < 2)
            continue;
        if (best == SIZE_MAX || last_used[i] < last_used[best])
            best = i;
    }
    return best;
}

static constexpr char MPRIS_PREFIX[] = "org.mpris.MediaPlayer2.";

bool xd_jumplist_player_of(const char *desktop_entry, const char *bus_name,
                           const char *desktop_id, const char *app_id) {
    char *name = nullptr;
    if (desktop_entry != nullptr && desktop_entry[0] != '\0') {
        name = g_strdup(desktop_entry);
    } else if (bus_name != nullptr && g_str_has_prefix(bus_name, MPRIS_PREFIX)) {
        /* Without DesktopEntry, the name it took: org.mpris.MediaPlayer2.vlc,
         * or .vlc.instance1234 for another of its processes. */
        name = g_strdup(bus_name + strlen(MPRIS_PREFIX));
        char *instance = strstr(name, ".instance");
        if (instance != nullptr)
            *instance = '\0';
    }
    if (name == nullptr || name[0] == '\0') {
        g_free(name);
        return false;
    }
    bool same = app_id != nullptr && strcmp(name, app_id) == 0;
    if (!same && desktop_id != nullptr) {
        /* The id's own name: a launcher kept by its path counts by its file's. */
        char *base = g_path_get_basename(desktop_id);
        size_t len = strlen(name);
        same = strncmp(base, name, len) == 0 && strcmp(base + len, ".desktop") == 0;
        g_free(base);
    }
    g_free(name);
    return same;
}
