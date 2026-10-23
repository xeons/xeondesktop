/* SPDX-License-Identifier: MIT
 * Previous Versions' snapshots. See versions.h.
 *
 * Each source gives the snapshot trees that may hold the item, each with
 * the path of the item inside it; what can be read and has the item
 * becomes a version. Paths are compared as they really are (the item's
 * folder with its links resolved), as the snapshots hold them.
 */
#include "versions.h"

#include <gio/gunixmounts.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char SNAPPER_NAME[] = "org.opensuse.Snapper";
static const char SNAPPER_PATH[] = "/org/opensuse/Snapper";
static const char SNAPPER_INTERFACE[] = "org.opensuse.Snapper";
static constexpr int SNAPPER_TIMEOUT_MS = 5000;
/* How long Snapper's configs are believed: they change only when the
 * administrator adds or removes one. */
static constexpr gint64 SNAPPER_CONFIGS_LIFETIME = 60 * G_TIME_SPAN_SECOND;
/* A walk deeper than this is a loop or something odd: left alone. */
static constexpr int RESTORE_MAX_DEPTH = 256;

void xd_version_free(xd_version *version) {
    if (version == nullptr)
        return;
    g_free(version->path);
    g_free(version->location);
    g_free(version);
}

/* ---- paths ------------------------------------------------------------------------ */

/* `path` with its folder's links resolved (the item itself, if a link,
 * stays the link), or nullptr if the folder doesn't exist. */
static char *versions_resolve_path(const char *path) {
    char *absolute = g_canonicalize_filename(path, nullptr);
    if (strcmp(absolute, "/") == 0)
        return absolute;
    char *folder = g_path_get_dirname(absolute);
    char *name = g_path_get_basename(absolute);
    char *real_folder = realpath(folder, nullptr);
    char *resolved = real_folder != nullptr ? g_build_filename(real_folder, name, nullptr) : nullptr;
    free(real_folder);
    g_free(name);
    g_free(folder);
    g_free(absolute);
    return resolved;
}

/* The part of `path` below `root` ("" for the root itself), or nullptr if
 * it isn't under it. Both are absolute and canonical. */
static const char *versions_get_relative(const char *root, const char *path) {
    size_t length = strlen(root);
    while (length > 1 && root[length - 1] == '/')
        length--;
    if (length == 1 && root[0] == '/')
        return path + 1;
    if (strncmp(root, path, length) != 0)
        return nullptr;
    if (path[length] == '\0')
        return path + length;
    if (path[length] != '/')
        return nullptr;
    return path + length + 1;
}

/* ---- the sources ----------------------------------------------------------------- */

typedef enum {
    VERSIONS_SOURCE_SNAPPER,
    VERSIONS_SOURCE_ZFS,       /* `root`: the dataset's mount point; nullptr: the item's mount */
    VERSIONS_SOURCE_TIMESHIFT, /* `root`: the backup holding timeshift/ */
} versions_source_kind;

typedef struct versions_source {
    versions_source_kind kind;
    char *root;
} versions_source;

static void versions_free_source(gpointer data) {
    versions_source *source = data;
    g_free(source->root);
    g_free(source);
}

static void versions_add_source(GPtrArray *sources, versions_source_kind kind, const char *root) {
    versions_source *source = g_new0(versions_source, 1);
    source->kind = kind;
    source->root = root != nullptr ? g_canonicalize_filename(root, nullptr) : nullptr;
    g_ptr_array_add(sources, source);
}

/* Timeshift's backups: each of /run/timeshift/<n>/backup that's there,
 * and / for /timeshift. */
static void versions_add_timeshift_backups(GPtrArray *sources) {
    GDir *run = g_dir_open("/run/timeshift", 0, nullptr);
    if (run != nullptr) {
        const char *name;
        while ((name = g_dir_read_name(run)) != nullptr) {
            char *backup = g_build_filename("/run/timeshift", name, "backup", nullptr);
            if (g_file_test(backup, G_FILE_TEST_IS_DIR))
                versions_add_source(sources, VERSIONS_SOURCE_TIMESHIFT, backup);
            g_free(backup);
        }
        g_dir_close(run);
    }
    versions_add_source(sources, VERSIONS_SOURCE_TIMESHIFT, "/");
}

/* Where to look: XD_SNAPSHOT_ROOTS's list, else everywhere. */
static GPtrArray *versions_get_sources(void) {
    GPtrArray *sources = g_ptr_array_new_with_free_func(versions_free_source);
    const char *roots = g_getenv("XD_SNAPSHOT_ROOTS");
    if (roots == nullptr) {
        versions_add_source(sources, VERSIONS_SOURCE_SNAPPER, nullptr);
        versions_add_source(sources, VERSIONS_SOURCE_ZFS, nullptr);
        versions_add_timeshift_backups(sources);
        return sources;
    }
    char **entries = g_strsplit(roots, G_SEARCHPATH_SEPARATOR_S, -1);
    for (char **entry = entries; *entry != nullptr; entry++) {
        if (strcmp(*entry, "snapper") == 0)
            versions_add_source(sources, VERSIONS_SOURCE_SNAPPER, nullptr);
        else if (g_str_has_prefix(*entry, "zfs=") && (*entry)[4] == '/')
            versions_add_source(sources, VERSIONS_SOURCE_ZFS, *entry + 4);
        else if (g_str_has_prefix(*entry, "timeshift=") && (*entry)[10] == '/')
            versions_add_source(sources, VERSIONS_SOURCE_TIMESHIFT, *entry + 10);
    }
    g_strfreev(entries);
    return sources;
}

/* The mount point of the ZFS dataset holding `path`, or nullptr. */
static char *versions_find_zfs_mount(const versions_source *source, const char *path) {
    if (source->root != nullptr)
        return versions_get_relative(source->root, path) != nullptr ? g_strdup(source->root)
                                                                    : nullptr;
    GUnixMountEntry *mount = g_unix_mount_entry_for(path, nullptr);
    if (mount == nullptr)
        return nullptr;
    char *mount_path = nullptr;
    if (g_strcmp0(g_unix_mount_entry_get_fs_type(mount), "zfs") == 0)
        mount_path = g_strdup(g_unix_mount_entry_get_mount_path(mount));
    g_unix_mount_entry_free(mount);
    return mount_path;
}

/* ---- Snapper's configs ----------------------------------------------------------- */

typedef struct snapper_config {
    char *name;
    char *subvolume;
} snapper_config;

static void snapper_free_config(gpointer data) {
    snapper_config *config = data;
    g_free(config->name);
    g_free(config->subvolume);
    g_free(config);
}

G_LOCK_DEFINE_STATIC(snapper_cache);
static GPtrArray *snapper_cached_configs;
static gint64 snapper_cached_time;

/* Snapper's configs (snapper_config*), as ListConfigs gives them: none if
 * Snapper isn't there. Free with g_ptr_array_unref. */
static GPtrArray *snapper_get_configs(void) {
    G_LOCK(snapper_cache);
    gint64 now = g_get_monotonic_time();
    if (snapper_cached_configs != nullptr &&
        now - snapper_cached_time < SNAPPER_CONFIGS_LIFETIME) {
        GPtrArray *configs = g_ptr_array_ref(snapper_cached_configs);
        G_UNLOCK(snapper_cache);
        return configs;
    }
    G_UNLOCK(snapper_cache);

    GPtrArray *configs = g_ptr_array_new_with_free_func(snapper_free_config);
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (bus != nullptr) {
        GVariant *reply = g_dbus_connection_call_sync(
            bus, SNAPPER_NAME, SNAPPER_PATH, SNAPPER_INTERFACE, "ListConfigs", nullptr,
            G_VARIANT_TYPE("(a(ssa{ss}))"), G_DBUS_CALL_FLAGS_NONE, SNAPPER_TIMEOUT_MS, nullptr,
            nullptr);
        if (reply != nullptr) {
            GVariantIter *iter;
            const char *name, *subvolume;
            g_variant_get(reply, "(a(ssa{ss}))", &iter);
            while (g_variant_iter_next(iter, "(&s&s@a{ss})", &name, &subvolume, nullptr)) {
                if (subvolume[0] != '/')
                    continue;
                snapper_config *config = g_new0(snapper_config, 1);
                config->name = g_strdup(name);
                config->subvolume = g_canonicalize_filename(subvolume, nullptr);
                g_ptr_array_add(configs, config);
            }
            g_variant_iter_free(iter);
            g_variant_unref(reply);
        }
        g_object_unref(bus);
    }

    G_LOCK(snapper_cache);
    g_clear_pointer(&snapper_cached_configs, g_ptr_array_unref);
    snapper_cached_configs = g_ptr_array_ref(configs);
    snapper_cached_time = now;
    G_UNLOCK(snapper_cache);
    return configs;
}

/* The config whose subvolume holds `path` most closely (a config for
 * /home rather than /'s), or nullptr. */
static const snapper_config *snapper_find_config(GPtrArray *configs, const char *path) {
    const snapper_config *best = nullptr;
    for (guint i = 0; i < configs->len; i++) {
        const snapper_config *config = g_ptr_array_index(configs, i);
        if (versions_get_relative(config->subvolume, path) != nullptr &&
            (best == nullptr || strlen(config->subvolume) > strlen(best->subvolume)))
            best = config;
    }
    return best;
}

/* ---- finding the versions --------------------------------------------------------- */

typedef struct versions_search {
    const char *path; /* resolved */
    GPtrArray *found; /* xd_version* */
    gboolean has_snapshots;
} versions_search;

/* The item's copy in the snapshot tree `tree` (what the source's root was
 * when the snapshot was taken), at `relative` below it. */
static void versions_add_candidate(versions_search *search, const char *tree,
                                   const char *relative, char *location, gint64 snapshot_time) {
    if (g_access(tree, R_OK | X_OK) != 0) {
        g_free(location);
        return; /* not ours to read: as if it weren't there */
    }
    search->has_snapshots = TRUE;
    char *copy = g_build_filename(tree, relative, nullptr);
    GStatBuf info;
    if (g_lstat(copy, &info) != 0) {
        g_free(copy);
        g_free(location);
        return;
    }
    xd_version *version = g_new0(xd_version, 1);
    version->path = copy;
    version->location = location;
    version->snapshot_time = snapshot_time;
    version->modified_time = info.st_mtim.tv_sec;
    version->modified_nanoseconds = info.st_mtim.tv_nsec;
    version->size = (guint64)info.st_size;
    version->is_directory = S_ISDIR(info.st_mode);
    g_ptr_array_add(search->found, version);
}

static void snapper_add_versions(versions_search *search) {
    GPtrArray *configs = snapper_get_configs();
    const snapper_config *config = snapper_find_config(configs, search->path);
    GDBusConnection *bus =
        config != nullptr ? g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr) : nullptr;
    GVariant *reply = nullptr;
    if (bus != nullptr)
        reply = g_dbus_connection_call_sync(bus, SNAPPER_NAME, SNAPPER_PATH, SNAPPER_INTERFACE,
                                            "ListSnapshots", g_variant_new("(s)", config->name),
                                            nullptr, G_DBUS_CALL_FLAGS_NONE, SNAPPER_TIMEOUT_MS,
                                            nullptr, nullptr);
    /* (uquxussa{ss}): number, type, pre number, date, uid, description,
     * cleanup, user data. Only the number and the date are needed, so a
     * Snapper that adds fields after them still works. Snapshot 0 is the
     * file system as it is now. */
    if (reply != nullptr && g_variant_is_of_type(reply, G_VARIANT_TYPE("(a*)"))) {
        GVariant *list = g_variant_get_child_value(reply, 0);
        const char *relative = versions_get_relative(config->subvolume, search->path);
        for (gsize i = 0; i < g_variant_n_children(list); i++) {
            GVariant *snapshot = g_variant_get_child_value(list, i);
            if (g_variant_is_container(snapshot) && g_variant_n_children(snapshot) >= 4) {
                GVariant *number = g_variant_get_child_value(snapshot, 0);
                GVariant *date = g_variant_get_child_value(snapshot, 3);
                if (g_variant_is_of_type(number, G_VARIANT_TYPE_UINT32) &&
                    g_variant_is_of_type(date, G_VARIANT_TYPE_INT64) &&
                    g_variant_get_uint32(number) != 0) {
                    guint32 id = g_variant_get_uint32(number);
                    char *id_text = g_strdup_printf("%u", id);
                    char *tree = g_build_filename(config->subvolume, ".snapshots", id_text,
                                                  "snapshot", nullptr);
                    versions_add_candidate(search, tree, relative,
                                           g_strdup_printf("Snapper snapshot %u", id),
                                           g_variant_get_int64(date));
                    g_free(tree);
                    g_free(id_text);
                }
                g_variant_unref(date);
                g_variant_unref(number);
            }
            g_variant_unref(snapshot);
        }
        g_variant_unref(list);
    }
    g_clear_pointer(&reply, g_variant_unref);
    g_clear_object(&bus);
    g_ptr_array_unref(configs);
}

static void zfs_add_versions(versions_search *search, const versions_source *source) {
    char *mount = versions_find_zfs_mount(source, search->path);
    if (mount == nullptr)
        return;
    const char *relative = versions_get_relative(mount, search->path);
    char *snapshots = g_build_filename(mount, ".zfs", "snapshot", nullptr);
    GDir *dir = relative != nullptr ? g_dir_open(snapshots, 0, nullptr) : nullptr;
    if (dir != nullptr) {
        const char *name;
        while ((name = g_dir_read_name(dir)) != nullptr) {
            char *tree = g_build_filename(snapshots, name, nullptr);
            GStatBuf info;
            if (g_stat(tree, &info) == 0)
                versions_add_candidate(search, tree, relative,
                                       g_strdup_printf("ZFS snapshot %s", name), info.st_mtime);
            g_free(tree);
        }
        g_dir_close(dir);
    }
    g_free(snapshots);
    g_free(mount);
}

/* When a Timeshift snapshot was taken, from its name (local time,
 * YYYY-MM-DD_HH-MM-SS), or -1. */
static gint64 timeshift_parse_time(const char *name) {
    if (strlen(name) != 19 || name[10] != '_' || name[13] != '-' || name[16] != '-')
        return -1;
    char iso[20];
    memcpy(iso, name, sizeof iso);
    iso[10] = 'T';
    iso[13] = ':';
    iso[16] = ':';
    GTimeZone *local = g_time_zone_new_local();
    GDateTime *when = g_date_time_new_from_iso8601(iso, local);
    g_time_zone_unref(local);
    if (when == nullptr)
        return -1;
    gint64 seconds = g_date_time_to_unix(when);
    g_date_time_unref(when);
    return seconds;
}

/* Timeshift's snapshots in `backup`: rsync's copy the whole system under
 * localhost/; btrfs's are the @ and @home subvolumes. */
static void timeshift_add_versions(versions_search *search, const versions_source *source) {
    static const char *const modes[] = {"timeshift", "timeshift-btrfs"};
    for (gsize mode = 0; mode < G_N_ELEMENTS(modes); mode++) {
        gboolean is_btrfs = mode == 1;
        char *snapshots = g_build_filename(source->root, modes[mode], "snapshots", nullptr);
        GDir *dir = g_dir_open(snapshots, 0, nullptr);
        const char *name;
        while (dir != nullptr && (name = g_dir_read_name(dir)) != nullptr) {
            gint64 when = timeshift_parse_time(name);
            if (when < 0)
                continue;
            char *snapshot = g_build_filename(snapshots, name, nullptr);
            char *tree = nullptr;
            const char *relative = search->path + 1;
            const char *in_home = versions_get_relative("/home", search->path);
            if (!is_btrfs) {
                tree = g_build_filename(snapshot, "localhost", nullptr);
            } else {
                char *home = g_build_filename(snapshot, "@home", nullptr);
                if (in_home != nullptr && g_file_test(home, G_FILE_TEST_IS_DIR)) {
                    tree = home;
                    home = nullptr;
                    relative = in_home;
                } else {
                    tree = g_build_filename(snapshot, "@", nullptr);
                }
                g_free(home);
            }
            versions_add_candidate(search, tree, relative,
                                   g_strdup_printf("Timeshift snapshot %s", name), when);
            g_free(tree);
            g_free(snapshot);
        }
        if (dir != nullptr)
            g_dir_close(dir);
        g_free(snapshots);
    }
}

static int versions_compare_newest_first(gconstpointer left, gconstpointer right) {
    const xd_version *first = *(xd_version *const *)left;
    const xd_version *second = *(xd_version *const *)right;
    if (first->snapshot_time != second->snapshot_time)
        return first->snapshot_time > second->snapshot_time ? -1 : 1;
    return g_strcmp0(first->path, second->path);
}

static gboolean versions_is_same(const xd_version *version, const xd_version *other) {
    return version->modified_time == other->modified_time &&
           version->modified_nanoseconds == other->modified_nanoseconds &&
           version->size == other->size && version->is_directory == other->is_directory;
}

GPtrArray *xd_versions_list(const char *path, gboolean *has_snapshots) {
    GPtrArray *versions = g_ptr_array_new_with_free_func((GDestroyNotify)xd_version_free);
    if (has_snapshots != nullptr)
        *has_snapshots = FALSE;
    char *resolved = versions_resolve_path(path);
    if (resolved == nullptr)
        return versions;
    versions_search search = {
        .path = resolved,
        .found = g_ptr_array_new_with_free_func((GDestroyNotify)xd_version_free),
    };
    GPtrArray *sources = versions_get_sources();
    for (guint i = 0; i < sources->len; i++) {
        const versions_source *source = g_ptr_array_index(sources, i);
        switch (source->kind) {
        case VERSIONS_SOURCE_SNAPPER:
            snapper_add_versions(&search);
            break;
        case VERSIONS_SOURCE_ZFS:
            zfs_add_versions(&search, source);
            break;
        case VERSIONS_SOURCE_TIMESHIFT:
            timeshift_add_versions(&search, source);
            break;
        }
    }
    g_ptr_array_unref(sources);
    g_ptr_array_sort(search.found, versions_compare_newest_first);

    /* Only where the item changed: a version the same as the newer one
     * before it (or as the item now) says nothing new. */
    GStatBuf now;
    xd_version current = {0};
    gboolean is_present = g_lstat(resolved, &now) == 0;
    if (is_present) {
        current.modified_time = now.st_mtim.tv_sec;
        current.modified_nanoseconds = now.st_mtim.tv_nsec;
        current.size = (guint64)now.st_size;
        current.is_directory = S_ISDIR(now.st_mode);
    }
    const xd_version *newer = is_present ? &current : nullptr;
    for (guint i = 0; i < search.found->len; i++) {
        xd_version *version = g_ptr_array_index(search.found, i);
        gboolean is_repeat = newer != nullptr && versions_is_same(version, newer);
        newer = version;
        if (!is_repeat)
            g_ptr_array_add(versions, g_steal_pointer(&search.found->pdata[i]));
    }
    g_ptr_array_unref(search.found);
    g_free(resolved);
    if (has_snapshots != nullptr)
        *has_snapshots = search.has_snapshots;
    return versions;
}

gboolean xd_versions_can_have_versions(const char *path) {
    char *resolved = versions_resolve_path(path);
    if (resolved == nullptr)
        return FALSE;
    gboolean is_covered = FALSE;
    GPtrArray *sources = versions_get_sources();
    for (guint i = 0; i < sources->len && !is_covered; i++) {
        const versions_source *source = g_ptr_array_index(sources, i);
        if (source->kind == VERSIONS_SOURCE_SNAPPER) {
            GPtrArray *configs = snapper_get_configs();
            is_covered = snapper_find_config(configs, resolved) != nullptr;
            g_ptr_array_unref(configs);
        } else if (source->kind == VERSIONS_SOURCE_ZFS) {
            char *mount = versions_find_zfs_mount(source, resolved);
            is_covered = mount != nullptr;
            g_free(mount);
        } else {
            char *rsync = g_build_filename(source->root, "timeshift", "snapshots", nullptr);
            char *btrfs = g_build_filename(source->root, "timeshift-btrfs", "snapshots", nullptr);
            is_covered = g_file_test(rsync, G_FILE_TEST_IS_DIR) ||
                         g_file_test(btrfs, G_FILE_TEST_IS_DIR);
            g_free(btrfs);
            g_free(rsync);
        }
    }
    g_ptr_array_unref(sources);
    g_free(resolved);
    return is_covered;
}

/* ---- restoring --------------------------------------------------------------------- */

static void restore_keep_error(GError **first, GError *error) {
    if (*first == nullptr)
        *first = error;
    else
        g_error_free(error);
}

static void restore_copy(GFile *source, GFile *target, int depth, GCancellable *cancellable,
                         GError **first_error) {
    GError *error = nullptr;
    GFileInfo *info = g_file_query_info(source, "standard::type,standard::name",
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
    if (info == nullptr) {
        restore_keep_error(first_error, error);
        return;
    }
    GFileType type = g_file_info_get_file_type(info);
    g_object_unref(info);
    GFileType target_type =
        g_file_query_file_type(target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);

    if (type != G_FILE_TYPE_DIRECTORY) {
        if (target_type == G_FILE_TYPE_DIRECTORY) {
            char *name = g_file_get_parse_name(target);
            restore_keep_error(first_error,
                               g_error_new(G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                           "“%s” is a folder now, so it was left as it is.",
                                           name));
            g_free(name);
            return;
        }
        /* A link where the file was goes, rather than what it points to
         * being written. */
        if (target_type == G_FILE_TYPE_SYMBOLIC_LINK && !g_file_delete(target, cancellable, &error)) {
            restore_keep_error(first_error, error);
            return;
        }
        if (!g_file_copy(source, target,
                         G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS |
                             G_FILE_COPY_ALL_METADATA,
                         cancellable, nullptr, nullptr, &error))
            restore_keep_error(first_error, error);
        return;
    }

    if (target_type == G_FILE_TYPE_UNKNOWN) {
        if (!g_file_make_directory(target, cancellable, &error)) {
            restore_keep_error(first_error, error);
            return;
        }
    } else if (target_type != G_FILE_TYPE_DIRECTORY) {
        /* Never into a link: it could lead anywhere. */
        char *name = g_file_get_parse_name(target);
        restore_keep_error(first_error,
                           g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                                       "“%s” isn't a folder now, so it was left as it is.", name));
        g_free(name);
        return;
    }
    if (depth >= RESTORE_MAX_DEPTH)
        return;
    GFileEnumerator *children =
        g_file_enumerate_children(source, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                  cancellable, &error);
    if (children == nullptr) {
        restore_keep_error(first_error, error);
        return;
    }
    GFileInfo *child_info;
    GFile *child;
    while (g_file_enumerator_iterate(children, &child_info, &child, cancellable, &error) &&
           child_info != nullptr) {
        GFile *target_child = g_file_get_child(target, g_file_info_get_name(child_info));
        restore_copy(child, target_child, depth + 1, cancellable, first_error);
        g_object_unref(target_child);
    }
    if (error != nullptr)
        restore_keep_error(first_error, error);
    g_object_unref(children);

    /* The folder's own permissions and dates, as in the snapshot. */
    GFileInfo *attributes = g_file_query_info(source, "unix::mode,time::modified,time::modified-usec",
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable,
                                              nullptr);
    if (attributes != nullptr) {
        g_file_set_attributes_from_info(target, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, nullptr);
        g_object_unref(attributes);
    }
}

gboolean xd_versions_restore(const char *source, const char *target, GCancellable *cancellable,
                             GError **error) {
    GFile *source_file = g_file_new_for_path(source);
    GFile *target_file = g_file_new_for_path(target);
    GError *first_error = nullptr;
    restore_copy(source_file, target_file, 0, cancellable, &first_error);
    g_object_unref(target_file);
    g_object_unref(source_file);
    if (first_error != nullptr) {
        g_propagate_error(error, first_error);
        return FALSE;
    }
    return TRUE;
}
