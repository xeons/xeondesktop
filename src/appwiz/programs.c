/* SPDX-License-Identifier: MIT */
#include <packagekit-glib2/packagekit.h>

#include "../core/common.h"
#include "programs.h"

/* The Start menu counts every launch; a handful is "rarely", a couple of
 * weeks of daily use "frequently". */
static constexpr int64_t OCCASIONALLY = 3;
static constexpr int64_t FREQUENTLY = 10;
/* xd-shell caps its counts at a million. */
static constexpr int64_t MAX_LAUNCHES = 1000000;

xd_appwiz_usage xd_appwiz_usage_for(int64_t launches, bool have_counts) {
    if (!have_counts)
        return XD_USAGE_UNKNOWN;
    if (launches >= FREQUENTLY)
        return XD_USAGE_FREQUENTLY;
    return launches >= OCCASIONALLY ? XD_USAGE_OCCASIONALLY : XD_USAGE_RARELY;
}

const char *xd_appwiz_usage_name(xd_appwiz_usage usage) {
    switch (usage) {
    case XD_USAGE_RARELY:
        return "rarely";
    case XD_USAGE_OCCASIONALLY:
        return "occasionally";
    case XD_USAGE_FREQUENTLY:
        return "frequently";
    case XD_USAGE_UNKNOWN:
        break;
    }
    return nullptr;
}

GHashTable *xd_appwiz_parse_launch_counts(char *const *lines, size_t count) {
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (size_t i = 0; i < count; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab == nullptr || tab[1] == '\0')
            continue;
        char *num = g_strndup(lines[i], (gsize)(tab - lines[i]));
        gint64 n = 0;
        bool ok = g_ascii_string_to_signed(num, 10, 0, MAX_LAUNCHES, &n, nullptr);
        g_free(num);
        /* The file is sorted by count, so a repeated id keeps the first. */
        if (ok && !g_hash_table_contains(t, tab + 1))
            g_hash_table_insert(t, g_strdup(tab + 1), GINT_TO_POINTER((int)n));
    }
    return t;
}

int64_t xd_appwiz_launches(GHashTable *counts, const char *desktop_id) {
    gpointer n = counts != nullptr ? g_hash_table_lookup(counts, desktop_id) : nullptr;
    return GPOINTER_TO_INT(n);
}

GHashTable *xd_appwiz_parse_launch_times(char *const *lines, size_t count) {
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (size_t i = 0; i < count; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab == nullptr || tab[1] == '\0')
            continue;
        char *num = g_strndup(lines[i], (gsize)(tab - lines[i]));
        gint64 when = 0;
        bool ok = g_ascii_string_to_signed(num, 10, 1, G_MAXINT64, &when, nullptr);
        g_free(num);
        /* Newest first: a repeated id keeps the first. */
        if (ok && !g_hash_table_contains(t, tab + 1))
            g_hash_table_insert(t, g_strdup(tab + 1), g_memdup2(&when, sizeof(when)));
    }
    return t;
}

int64_t xd_appwiz_launch_time(GHashTable *times, const char *desktop_id) {
    const gint64 *when = times != nullptr ? g_hash_table_lookup(times, desktop_id) : nullptr;
    return when != nullptr ? *when : 0;
}

/* The program a recent-files entry names: GTK records g_get_prgname(),
 * which is the executable's name, or the application id for GApplications
 * (the desktop id without ".desktop"). The command's first word is
 * compared too; GTK quotes the whole command ("'name %u'"), so the first
 * word of that is taken again. */
static bool names_program(const char *app, const char *exec, const char *id_stem,
                          const char *exe_base) {
    if (g_strcmp0(app, id_stem) == 0 || (exe_base != nullptr && g_strcmp0(app, exe_base) == 0))
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

int64_t xd_appwiz_last_used(GBookmarkFile *recent, const char *desktop_id,
                            const char *executable) {
    if (recent == nullptr || desktop_id == nullptr)
        return 0;
    char *stem = g_str_has_suffix(desktop_id, ".desktop")
                     ? g_strndup(desktop_id, strlen(desktop_id) - strlen(".desktop"))
                     : g_strdup(desktop_id);
    char *exe_base = executable != nullptr && executable[0] != '\0'
                         ? g_path_get_basename(executable)
                         : nullptr;
    int64_t last = 0;
    gsize n = 0;
    char **uris = g_bookmark_file_get_uris(recent, &n);
    for (gsize i = 0; i < n; i++) {
        gsize napps = 0;
        char **apps = g_bookmark_file_get_applications(recent, uris[i], &napps, nullptr);
        for (gsize a = 0; apps != nullptr && a < napps; a++) {
            char *exec = nullptr;
            GDateTime *stamp = nullptr;
            if (!g_bookmark_file_get_application_info(recent, uris[i], apps[a], &exec, nullptr,
                                                      &stamp, nullptr))
                continue;
            if (stamp != nullptr && names_program(apps[a], exec, stem, exe_base))
                last = XD_MAX(last, (int64_t)g_date_time_to_unix(stamp));
            g_free(exec);
        }
        g_strfreev(apps);
    }
    g_strfreev(uris);
    g_free(exe_base);
    g_free(stem);
    return last;
}

char *xd_appwiz_format_size(guint64 bytes) {
    static constexpr double MB = 1024.0 * 1024.0;
    static constexpr double GB = MB * 1024.0;
    double b = (double)bytes;
    if (b >= GB)
        return g_strdup_printf("%.2fGB", b / GB);
    return g_strdup_printf("%.2fMB", XD_MAX(b / MB, 0.01));
}

char *xd_appwiz_format_date(int64_t when) {
    if (when <= 0)
        return g_strdup("");
    GDateTime *t = g_date_time_new_from_unix_local(when);
    if (t == nullptr)
        return g_strdup("");
    char *s = g_date_time_format(t, "%x");
    g_date_time_unref(t);
    return s != nullptr ? s : g_strdup("");
}

static int by_name(const xd_appwiz_key *a, const xd_appwiz_key *b) {
    char *x = g_utf8_casefold(a->name != nullptr ? a->name : "", -1);
    char *y = g_utf8_casefold(b->name != nullptr ? b->name : "", -1);
    int r = g_utf8_collate(x, y);
    g_free(x);
    g_free(y);
    return r;
}

/* Larger first; 0 (unknown) last. */
static int larger_first(int64_t a, int64_t b) {
    if (a == b)
        return 0;
    return a > b ? -1 : 1;
}

int xd_appwiz_compare(const xd_appwiz_key *a, const xd_appwiz_key *b, xd_appwiz_sort sort) {
    int r = 0;
    switch (sort) {
    case XD_SORT_NAME:
        break;
    case XD_SORT_SIZE:
        r = a->size == b->size ? 0 : (a->size > b->size ? -1 : 1);
        break;
    case XD_SORT_FREQUENCY:
        r = larger_first(a->launches, b->launches);
        break;
    case XD_SORT_DATE:
        r = larger_first(a->last_used, b->last_used);
        break;
    case XD_SORT_SOURCE:
        /* Together by source, A-Z; not known last. */
        if ((a->source == nullptr) != (b->source == nullptr))
            r = a->source == nullptr ? 1 : -1;
        else if (a->source != nullptr)
            r = g_utf8_collate(a->source, b->source);
        break;
    }
    return r != 0 ? r : by_name(a, b);
}

const char *xd_appwiz_package_manager(const char *backend) {
    static const char *const names[][2] = {
        {"alpm", "pacman"},   {"aptcc", "APT"},       {"apt", "APT"},
        {"dnf", "DNF"},       {"dnf5", "DNF"},        {"zypp", "zypper"},
        {"apk", "apk"},       {"portage", "Portage"}, {"eopkg", "eopkg"},
        {"freebsd", "pkg"},   {"nix", "Nix"},         {"slack", "slackpkg"},
        {"poldek", "poldek"}, {"yum", "YUM"},
    };
    if (backend == nullptr || backend[0] == '\0')
        return "a package manager";
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        if (strcmp(backend, names[i][0]) == 0)
            return names[i][1];
    }
    return backend;
}

char *xd_appwiz_package_source(const char *backend, const char *package_id) {
    const char *manager = xd_appwiz_package_manager(backend);
    char **parts = package_id != nullptr ? pk_package_id_split(package_id) : nullptr;
    const char *data = parts != nullptr ? parts[PK_PACKAGE_ID_DATA] : "";
    /* "installed:extra" or "extra": the repository; "installed": none
     * known; "local" or "installed:local": from a package file. */
    const char *repo = g_str_has_prefix(data, "installed:") ? data + strlen("installed:")
                       : strcmp(data, "installed") == 0  ? ""
                                                         : data;
    char *s = strcmp(repo, "local") == 0 ? g_strdup_printf("%s (local package)", manager)
              : repo[0] != '\0'          ? g_strdup_printf("%s (%s)", manager, repo)
                                         : g_strdup(manager);
    g_strfreev(parts);
    return s;
}

char *xd_appwiz_flatpak_source(const char *remote, const char *installation) {
    const char *where = installation == nullptr || strcmp(installation, "default") == 0
                            ? "system"
                            : installation;
    return remote != nullptr && remote[0] != '\0'
               ? g_strdup_printf("Flatpak (%s, %s)", remote, where)
               : g_strdup_printf("Flatpak (%s)", where);
}

const char *xd_appwiz_other_source(const char *executable) {
    char *lower = executable != nullptr ? g_ascii_strdown(executable, -1) : nullptr;
    bool appimage = lower != nullptr && g_str_has_suffix(lower, ".appimage");
    g_free(lower);
    return appimage ? "AppImage" : "Not from a package";
}

bool xd_appwiz_split_package_id(const char *package_id, char **name, char **version,
                                char **repo) {
    char **parts = package_id != nullptr ? pk_package_id_split(package_id) : nullptr;
    if (parts == nullptr)
        return false;
    const char *data = parts[PK_PACKAGE_ID_DATA];
    const char *colon = strchr(data, ':');
    const char *source = data;
    if (g_str_has_prefix(data, "installed"))
        source = colon != nullptr ? colon + 1 : "";
    else if (strcmp(data, "local") == 0)
        source = "";
    if (name != nullptr)
        *name = g_strdup(parts[PK_PACKAGE_ID_NAME]);
    if (version != nullptr)
        *version = g_strdup(parts[PK_PACKAGE_ID_VERSION]);
    if (repo != nullptr)
        *repo = g_strdup(source);
    g_strfreev(parts);
    return true;
}

bool xd_appwiz_package_installed(const char *package_id) {
    char **parts = package_id != nullptr ? pk_package_id_split(package_id) : nullptr;
    if (parts == nullptr)
        return false;
    const char *data = parts[PK_PACKAGE_ID_DATA];
    bool yes = strcmp(data, "installed") == 0 || g_str_has_prefix(data, "installed:");
    g_strfreev(parts);
    return yes;
}
