/* SPDX-License-Identifier: MIT
 * Disk Cleanup's system files. See syscleanup.h.
 */
#include "syscleanup.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Old system logs: the journal's archived files untouched this long, as
 * journalctl --vacuum-time=2weeks removes them. */
static constexpr gint64 LOG_AGE = 14 * G_TIME_SPAN_DAY;

typedef enum package_manager : uint8_t {
    PACKAGES_NONE,
    PACKAGES_PACMAN,
    PACKAGES_APT,
    PACKAGES_DNF,
} package_manager;

/* ---- tools and paths ---------------------------------------------------------- */

/* The tool's full path, or nullptr if it isn't there. */
static char *find_tool(const xd_sysclean_env *env, const char *name) {
    static const char *const places[] = {"/usr/bin", "/usr/sbin", "/bin", "/sbin"};
    if (env->tools != nullptr) {
        char *path = g_build_filename(env->tools, name, nullptr);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
            return path;
        g_free(path);
        return nullptr;
    }
    for (size_t i = 0; i < G_N_ELEMENTS(places); i++) {
        char *path = g_build_filename(places[i], name, nullptr);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
            return path;
        g_free(path);
    }
    return nullptr;
}

static package_manager find_package_manager(const xd_sysclean_env *env) {
    static const struct {
        const char *tool;
        package_manager manager;
    } tools[] = {
        {"pacman", PACKAGES_PACMAN},
        {"apt-get", PACKAGES_APT},
        {"dnf", PACKAGES_DNF},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(tools); i++) {
        char *path = find_tool(env, tools[i].tool);
        g_free(path);
        if (path != nullptr)
            return tools[i].manager;
    }
    return PACKAGES_NONE;
}

/* A system path under the environment's root. */
static char *system_path(const xd_sysclean_env *env, const char *path) {
    return g_strconcat(env->root != nullptr ? env->root : "", path, nullptr);
}

/* Runs a tool (argv[0] a name looked up by find_tool), in the C locale;
 * its output in *out if wanted. FALSE with `err` set if it couldn't run
 * or failed. */
static bool run_tool(const xd_sysclean_env *env, const char *const *argv, char **out,
                     GError **err) {
    char *path = find_tool(env, argv[0]);
    if (path == nullptr) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s is not installed", argv[0]);
        return false;
    }
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, path);
    for (const char *const *arg = argv + 1; *arg != nullptr; arg++)
        g_ptr_array_add(args, (gpointer)(uintptr_t)*arg);
    g_ptr_array_add(args, nullptr);
    char **envp = g_environ_setenv(g_get_environ(), "LC_ALL", "C", TRUE);
    char *errors = nullptr;
    int status = 0;
    bool ok = g_spawn_sync(nullptr, (char **)args->pdata, envp, G_SPAWN_DEFAULT, nullptr, nullptr,
                           out, &errors, &status, err) &&
              g_spawn_check_wait_status(status, err);
    if (!ok && err != nullptr && *err != nullptr && errors != nullptr && errors[0] != '\0')
        g_prefix_error(err, "%s: %s", argv[0], g_strstrip(errors));
    g_free(errors);
    g_strfreev(envp);
    g_ptr_array_free(args, TRUE);
    g_free(path);
    return ok;
}

/* ---- sizes ---------------------------------------------------------------------- */

typedef bool (*file_filter)(const char *name, const struct stat *st, gpointer data);

/* The bytes on disk of the regular files in `dir` (not through links)
 * that `filter` takes. */
static guint64 files_size(const char *dir, file_filter filter, gpointer data,
                          GCancellable *cancel) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dir_stream = fd >= 0 ? fdopendir(fd) : nullptr;
    if (dir_stream == nullptr) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    guint64 size = 0;
    struct dirent *dir_entry;
    while (!g_cancellable_is_cancelled(cancel) && (dir_entry = readdir(dir_stream)) != nullptr) {
        struct stat st;
        if (fstatat(dirfd(dir_stream), dir_entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(st.st_mode) && filter(dir_entry->d_name, &st, data))
            size += (guint64)st.st_blocks * 512;
    }
    closedir(dir_stream);
    return size;
}

/* The folders in `dir` (not through links), full paths. */
static GPtrArray *subdirs(const char *dir) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GDir *handle = g_dir_open(dir, 0, nullptr);
    const char *name;
    while (handle != nullptr && (name = g_dir_read_name(handle)) != nullptr) {
        char *path = g_build_filename(dir, name, nullptr);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
            g_ptr_array_add(out, path);
        else
            g_free(path);
    }
    if (handle != nullptr)
        g_dir_close(handle);
    return out;
}

static bool any_file([[maybe_unused]] const char *name, [[maybe_unused]] const struct stat *st,
                     [[maybe_unused]] gpointer data) {
    return true;
}

static bool deb_file(const char *name, [[maybe_unused]] const struct stat *st,
                     [[maybe_unused]] gpointer data) {
    return g_str_has_suffix(name, ".deb");
}

static bool rpm_file(const char *name, [[maybe_unused]] const struct stat *st,
                     [[maybe_unused]] gpointer data) {
    return g_str_has_suffix(name, ".rpm");
}

/* pacman: a cached package (or its signature) that isn't the installed
 * version, which pacman -Sc removes. `data` is the set of "name-version"
 * installed; a file is name-version-arch.pkg.tar.*[.sig]. */
static bool pacman_unneeded(const char *name, [[maybe_unused]] const struct stat *st,
                            gpointer data) {
    const char *pkg = strstr(name, ".pkg.tar");
    if (pkg == nullptr)
        return false;
    char *stem = g_strndup(name, (gsize)(pkg - name));
    char *arch = strrchr(stem, '-');
    bool unneeded = arch != nullptr;
    if (unneeded) {
        *arch = '\0';
        unneeded = !g_hash_table_contains(data, stem);
    }
    g_free(stem);
    return unneeded;
}

/* The installed packages as "name-version" (pacman -Q's lines). */
static GHashTable *pacman_installed(const xd_sysclean_env *env) {
    char *out = nullptr;
    if (!run_tool(env, (const char *const[]){"pacman", "-Q", nullptr}, &out, nullptr)) {
        g_free(out);
        return nullptr;
    }
    GHashTable *installed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    char **lines = g_strsplit(out, "\n", -1);
    for (char **line = lines; *line != nullptr; line++) {
        char **fields = g_strsplit(g_strstrip(*line), " ", 2);
        if (g_strv_length(fields) == 2)
            g_hash_table_add(installed, g_strdup_printf("%s-%s", fields[0], fields[1]));
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(out);
    return installed;
}

/* pacman's cache folders (pacman-conf CacheDir), under the root. */
static GPtrArray *pacman_cache_dirs(const xd_sysclean_env *env) {
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    char *out = nullptr;
    if (run_tool(env, (const char *const[]){"pacman-conf", "CacheDir", nullptr}, &out, nullptr)) {
        char **lines = g_strsplit(out, "\n", -1);
        for (char **line = lines; *line != nullptr; line++)
            if (g_strstrip(*line)[0] == '/')
                g_ptr_array_add(dirs, system_path(env, *line));
        g_strfreev(lines);
    }
    g_free(out);
    if (dirs->len == 0)
        g_ptr_array_add(dirs, system_path(env, "/var/cache/pacman/pkg"));
    return dirs;
}

static guint64 packages_size(const xd_sysclean_env *env, package_manager manager, char **view,
                             GCancellable *cancel) {
    guint64 size = 0;
    switch (manager) {
    case PACKAGES_PACMAN: {
        GHashTable *installed = pacman_installed(env);
        if (installed == nullptr)
            return 0;
        GPtrArray *dirs = pacman_cache_dirs(env);
        for (guint i = 0; i < dirs->len; i++)
            size += files_size(g_ptr_array_index(dirs, i), pacman_unneeded, installed, cancel);
        *view = g_strdup(g_ptr_array_index(dirs, 0));
        g_ptr_array_unref(dirs);
        g_hash_table_unref(installed);
        break;
    }
    case PACKAGES_APT: {
        char *archives = system_path(env, "/var/cache/apt/archives");
        char *partial = g_build_filename(archives, "partial", nullptr);
        size = files_size(archives, deb_file, nullptr, cancel) +
               files_size(partial, any_file, nullptr, cancel);
        *view = archives;
        g_free(partial);
        break;
    }
    case PACKAGES_DNF: {
        /* dnf's repositories each keep their packages; dnf5's are in
         * libdnf5's cache. */
        static const char *const caches[] = {"/var/cache/dnf", "/var/cache/libdnf5"};
        for (size_t i = 0; i < G_N_ELEMENTS(caches); i++) {
            char *cache = system_path(env, caches[i]);
            GPtrArray *repos = subdirs(cache);
            for (guint j = 0; j < repos->len; j++) {
                char *packages = g_build_filename(g_ptr_array_index(repos, j), "packages", nullptr);
                size += files_size(packages, rpm_file, nullptr, cancel);
                g_free(packages);
            }
            g_ptr_array_unref(repos);
            if (*view == nullptr && g_file_test(cache, G_FILE_TEST_IS_DIR))
                *view = cache;
            else
                g_free(cache);
        }
        break;
    }
    case PACKAGES_NONE:
        break;
    }
    return size;
}

/* An archived journal file (system@...journal, user-1000@...journal~)
 * untouched since `data` (seconds). */
static bool old_journal(const char *name, const struct stat *st, gpointer data) {
    return strchr(name, '@') != nullptr &&
           (g_str_has_suffix(name, ".journal") || g_str_has_suffix(name, ".journal~")) &&
           (gint64)st->st_mtim.tv_sec < *(const gint64 *)data;
}

static guint64 logs_size(const xd_sysclean_env *env, char **view, GCancellable *cancel) {
    char *journal = system_path(env, "/var/log/journal");
    gint64 cutoff = (env->now - LOG_AGE) / G_USEC_PER_SEC;
    guint64 size = 0;
    GPtrArray *machines = subdirs(journal);
    for (guint i = 0; i < machines->len; i++)
        size += files_size(g_ptr_array_index(machines, i), old_journal, &cutoff, cancel);
    g_ptr_array_unref(machines);
    *view = journal;
    return size;
}

static bool core_dump(const char *name, [[maybe_unused]] const struct stat *st,
                      [[maybe_unused]] gpointer data) {
    return g_str_has_prefix(name, "core.");
}

static guint64 crashes_size(const xd_sysclean_env *env, char **view, GCancellable *cancel) {
    char *dir = system_path(env, "/var/lib/systemd/coredump");
    guint64 size = files_size(dir, core_dump, nullptr, cancel);
    *view = dir;
    return size;
}

XdCleanupCategory *xd_sysclean_scan(XdCleanupKind kind, const xd_sysclean_env *env,
                                    GCancellable *cancel) {
    char *view = nullptr;
    guint64 size = 0;
    if (kind == XD_CLEANUP_PACKAGES)
        size = packages_size(env, find_package_manager(env), &view, cancel);
    else if (kind == XD_CLEANUP_LOGS)
        size = logs_size(env, &view, cancel);
    else if (kind == XD_CLEANUP_CRASHES)
        size = crashes_size(env, &view, cancel);
    if (size == 0) {
        g_free(view);
        return nullptr;
    }
    XdCleanupCategory *cleanup_category = xd_cleanup_category_new(kind);
    cleanup_category->size = size;
    cleanup_category->view = view;
    return cleanup_category;
}

/* ---- cleaning --------------------------------------------------------------------- */

/* Deletes the crash dumps: regular files named core.* in the folder,
 * never through a link. */
static bool delete_crashes(const xd_sysclean_env *env, GError **err) {
    char *dir = system_path(env, "/var/lib/systemd/coredump");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dir_stream = fd >= 0 ? fdopendir(fd) : nullptr;
    if (dir_stream == nullptr) {
        int error_number = errno;
        if (fd >= 0)
            close(fd);
        bool none = error_number == ENOENT;
        GIOErrorEnum code = g_io_error_from_errno(error_number);
        if (!none)
            g_set_error(err, G_IO_ERROR, (gint)code, "Cannot open %s: %s", dir,
                        g_strerror(error_number));
        g_free(dir);
        return none;
    }
    bool ok = true;
    struct dirent *dir_entry;
    while ((dir_entry = readdir(dir_stream)) != nullptr) {
        struct stat st;
        if (!g_str_has_prefix(dir_entry->d_name, "core.") ||
            fstatat(dirfd(dir_stream), dir_entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(st.st_mode))
            continue;
        if (unlinkat(dirfd(dir_stream), dir_entry->d_name, 0) != 0 && errno != ENOENT && ok) {
            int error_number = errno;
            GIOErrorEnum code = g_io_error_from_errno(error_number);
            ok = false;
            g_set_error(err, G_IO_ERROR, (gint)code, "Cannot delete %s/%s: %s", dir,
                        dir_entry->d_name, g_strerror(error_number));
        }
    }
    closedir(dir_stream);
    g_free(dir);
    return ok;
}

bool xd_sysclean_clean(XdCleanupKind kind, const xd_sysclean_env *env, GError **err) {
    if (kind == XD_CLEANUP_LOGS)
        return run_tool(env, (const char *const[]){"journalctl", "--vacuum-time=2weeks", nullptr},
                        nullptr, err);
    if (kind == XD_CLEANUP_CRASHES)
        return delete_crashes(env, err);
    if (kind != XD_CLEANUP_PACKAGES) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not a kind of system file");
        return false;
    }
    switch (find_package_manager(env)) {
    case PACKAGES_PACMAN:
        /* Everything but the installed versions, and unused repositories'
         * databases (both of -Sc's questions answered yes). */
        return run_tool(env, (const char *const[]){"pacman", "-Sc", "--noconfirm", nullptr},
                        nullptr, err);
    case PACKAGES_APT:
        return run_tool(env, (const char *const[]){"apt-get", "clean", nullptr}, nullptr, err);
    case PACKAGES_DNF:
        return run_tool(env, (const char *const[]){"dnf", "clean", "packages", nullptr}, nullptr,
                        err);
    case PACKAGES_NONE:
        break;
    }
    g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "No package manager was found");
    return false;
}

static const struct {
    XdCleanupKind kind;
    const char *name;
} kind_names[] = {
    {XD_CLEANUP_PACKAGES, "packages"},
    {XD_CLEANUP_LOGS, "logs"},
    {XD_CLEANUP_CRASHES, "crashes"},
};

const char *xd_sysclean_name(XdCleanupKind kind) {
    for (size_t i = 0; i < G_N_ELEMENTS(kind_names); i++)
        if (kind_names[i].kind == kind)
            return kind_names[i].name;
    return nullptr;
}

bool xd_sysclean_kind_from_name(const char *name, XdCleanupKind *out) {
    for (size_t i = 0; i < G_N_ELEMENTS(kind_names); i++) {
        if (strcmp(kind_names[i].name, name) == 0) {
            *out = kind_names[i].kind;
            return true;
        }
    }
    return false;
}
