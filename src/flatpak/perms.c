/* SPDX-License-Identifier: MIT
 * Flatpak permissions: merging the layers and editing the user's override
 * (perms.h). GKeyFile reads and writes the files, as flatpak itself does.
 */
#include "perms.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#define CONTEXT "Context"
#define ENVIRONMENT "Environment"

struct xd_fp_perms {
    GHashTable *state[XD_FP_LIST_COUNT]; /* name -> xd_fp_state */
    GHashTable *conds[XD_FP_LIST_COUNT]; /* name -> GPtrArray of conditions */
    GHashTable *fs;                      /* canonical filesystem -> mode + 1 */
    GHashTable *persistent;              /* a set */
    GHashTable *env;                     /* name -> value, nullptr when unset */
    GHashTable *bus[XD_FP_BUS_COUNT];    /* name -> policy */
};

static const char *const list_keys[XD_FP_LIST_COUNT] = {"shared", "sockets", "devices",
                                                        "features"};

/* flatpak-metadata(5) and flatpak-override(1) of Flatpak 1.18. */
static const char *const shared_names[] = {"network", "ipc", nullptr};
static const char *const socket_names[] = {
    "x11",       "wayland", "fallback-x11", "pulseaudio", "session-bus", "system-bus",
    "ssh-auth",  "pcsc",    "cups",         "gpg-agent",  "inherit-wayland-socket", nullptr};
static const char *const device_names[] = {"dri", "input", "usb", "kvm", "shm", "all", nullptr};
static const char *const feature_names[] = {"devel",  "multiarch",       "bluetooth",
                                            "canbus", "per-app-dev-shm", nullptr};

const char *xd_fp_list_key(xd_fp_list list) { return list_keys[list]; }

const char *xd_fp_bus_group(xd_fp_bus bus) {
    return bus == XD_FP_SESSION_BUS ? "Session Bus Policy" : "System Bus Policy";
}

const char *const *xd_fp_list_names(xd_fp_list list) {
    static const char *const *const names[XD_FP_LIST_COUNT] = {shared_names, socket_names,
                                                               device_names, feature_names};
    return names[list];
}

xd_fp_perms *xd_fp_perms_new(void) {
    xd_fp_perms *permissions = g_new0(xd_fp_perms, 1);
    for (int i = 0; i < XD_FP_LIST_COUNT; i++) {
        permissions->state[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
        permissions->conds[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                      (GDestroyNotify)g_ptr_array_unref);
    }
    permissions->fs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    permissions->persistent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    permissions->env = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (int i = 0; i < XD_FP_BUS_COUNT; i++)
        permissions->bus[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return permissions;
}

void xd_fp_perms_free(xd_fp_perms *permissions) {
    if (permissions == nullptr)
        return;
    for (int i = 0; i < XD_FP_LIST_COUNT; i++) {
        g_hash_table_unref(permissions->state[i]);
        g_hash_table_unref(permissions->conds[i]);
    }
    g_hash_table_unref(permissions->fs);
    g_hash_table_unref(permissions->persistent);
    g_hash_table_unref(permissions->env);
    for (int i = 0; i < XD_FP_BUS_COUNT; i++)
        g_hash_table_unref(permissions->bus[i]);
    g_free(permissions);
}

static GPtrArray *strv_array(void) { return g_ptr_array_new_with_free_func(g_free); }

static GPtrArray *array_copy(const GPtrArray *original) {
    GPtrArray *copy = strv_array();
    for (guint i = 0; i < original->len; i++)
        g_ptr_array_add(copy, g_strdup(g_ptr_array_index(original, i)));
    return copy;
}

xd_fp_perms *xd_fp_perms_copy(const xd_fp_perms *original) {
    xd_fp_perms *copy = xd_fp_perms_new();
    GHashTableIter it;
    gpointer key, value;
    for (int i = 0; i < XD_FP_LIST_COUNT; i++) {
        g_hash_table_iter_init(&it, original->state[i]);
        while (g_hash_table_iter_next(&it, &key, &value))
            g_hash_table_insert(copy->state[i], g_strdup(key), value);
        g_hash_table_iter_init(&it, original->conds[i]);
        while (g_hash_table_iter_next(&it, &key, &value))
            g_hash_table_insert(copy->conds[i], g_strdup(key), array_copy(value));
    }
    g_hash_table_iter_init(&it, original->fs);
    while (g_hash_table_iter_next(&it, &key, &value))
        g_hash_table_insert(copy->fs, g_strdup(key), value);
    g_hash_table_iter_init(&it, original->persistent);
    while (g_hash_table_iter_next(&it, &key, nullptr))
        g_hash_table_add(copy->persistent, g_strdup(key));
    g_hash_table_iter_init(&it, original->env);
    while (g_hash_table_iter_next(&it, &key, &value))
        g_hash_table_insert(copy->env, g_strdup(key), g_strdup(value));
    for (int i = 0; i < XD_FP_BUS_COUNT; i++) {
        g_hash_table_iter_init(&it, original->bus[i]);
        while (g_hash_table_iter_next(&it, &key, &value))
            g_hash_table_insert(copy->bus[i], g_strdup(key), g_strdup(value));
    }
    return copy;
}

/* "if:name:condition": the name (g_free it) and where the condition starts. */
static char *conditional_name(const char *entry, const char **condition) {
    if (!g_str_has_prefix(entry, "if:"))
        return nullptr;
    const char *name = entry + 3;
    const char *colon = strchr(name, ':');
    if (colon == nullptr || colon == name || colon[1] == '\0')
        return nullptr;
    *condition = colon + 1;
    return g_strndup(name, (gsize)(colon - name));
}

static void merge_list(xd_fp_perms *permissions, xd_fp_list list, GKeyFile *layer) {
    char **entries = g_key_file_get_string_list(layer, CONTEXT, list_keys[list], nullptr, nullptr);
    if (entries == nullptr)
        return;
    /* This layer's say on each name it mentions, then over the lower ones'. */
    GHashTable *plain = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GHashTable *conds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              (GDestroyNotify)g_ptr_array_unref);
    for (char **element = entries; *element != nullptr; element++) {
        const char *condition;
        char *name = conditional_name(*element, &condition);
        if (name != nullptr) {
            GPtrArray *array = g_hash_table_lookup(conds, name);
            if (array == nullptr) {
                array = strv_array();
                g_hash_table_insert(conds, g_strdup(name), array);
            }
            g_ptr_array_add(array, g_strdup(condition));
            g_free(name);
        } else if (**element == '!' && (*element)[1] != '\0') {
            g_hash_table_insert(plain, g_strdup(*element + 1), GINT_TO_POINTER(XD_FP_OFF));
        } else if (**element != '\0' && **element != '!') {
            g_hash_table_insert(plain, g_strdup(*element), GINT_TO_POINTER(XD_FP_ON));
        }
    }
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, plain);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        if (g_hash_table_contains(conds, key))
            continue; /* the plain "x" beside conditions is for older Flatpaks */
        g_hash_table_insert(permissions->state[list], g_strdup(key), value);
        g_hash_table_remove(permissions->conds[list], key);
    }
    g_hash_table_iter_init(&it, conds);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        g_hash_table_insert(permissions->state[list], g_strdup(key),
                            GINT_TO_POINTER(XD_FP_CONDITIONAL));
        g_hash_table_insert(permissions->conds[list], g_strdup(key), g_ptr_array_ref(value));
    }
    g_hash_table_unref(plain);
    g_hash_table_unref(conds);
    g_strfreev(entries);
}

static bool is_reset(const char *entry) { return g_str_has_suffix(entry, ":reset"); }

static void merge_filesystems(xd_fp_perms *permissions, GKeyFile *layer) {
    char **entries = g_key_file_get_string_list(layer, CONTEXT, "filesystems", nullptr, nullptr);
    if (entries == nullptr)
        return;
    /* "!host:reset": nothing from the layers below counts. */
    for (char **element = entries; *element != nullptr; element++) {
        if (strcmp(*element, "!host:reset") == 0)
            g_hash_table_remove_all(permissions->fs);
    }
    for (char **element = entries; *element != nullptr; element++) {
        if (is_reset(*element))
            continue;
        xd_fp_fs_mode mode;
        char *fs = xd_fp_filesystem_parse(*element, &mode);
        if (fs != nullptr)
            g_hash_table_insert(permissions->fs, fs, GINT_TO_POINTER(mode + 1));
    }
    g_strfreev(entries);
}

void xd_fp_perms_merge(xd_fp_perms *permissions, GKeyFile *layer) {
    for (int i = 0; i < XD_FP_LIST_COUNT; i++)
        merge_list(permissions, (xd_fp_list)i, layer);
    merge_filesystems(permissions, layer);
    char **persistent = g_key_file_get_string_list(layer, CONTEXT, "persistent", nullptr, nullptr);
    for (char **persistent_path = persistent;
         persistent_path != nullptr && *persistent_path != nullptr; persistent_path++) {
        if (**persistent_path != '\0')
            g_hash_table_add(permissions->persistent, g_strdup(*persistent_path));
    }
    g_strfreev(persistent);
    char **vars = g_key_file_get_keys(layer, ENVIRONMENT, nullptr, nullptr);
    for (char **variable = vars; variable != nullptr && *variable != nullptr; variable++) {
        char *value = g_key_file_get_string(layer, ENVIRONMENT, *variable, nullptr);
        if (value != nullptr)
            g_hash_table_insert(permissions->env, g_strdup(*variable), value);
    }
    g_strfreev(vars);
    /* After [Environment]: flatpak writes "VAR=" there too for an unset one. */
    char **unset = g_key_file_get_string_list(layer, CONTEXT, "unset-environment", nullptr,
                                              nullptr);
    for (char **variable = unset; variable != nullptr && *variable != nullptr; variable++) {
        if (**variable != '\0')
            g_hash_table_insert(permissions->env, g_strdup(*variable), nullptr);
    }
    g_strfreev(unset);
    for (int bus = 0; bus < XD_FP_BUS_COUNT; bus++) {
        const char *group = xd_fp_bus_group((xd_fp_bus)bus);
        char **names = g_key_file_get_keys(layer, group, nullptr, nullptr);
        for (char **name = names; name != nullptr && *name != nullptr; name++) {
            char *policy = g_key_file_get_string(layer, group, *name, nullptr);
            if (policy != nullptr)
                g_hash_table_insert(permissions->bus[bus], g_strdup(*name), policy);
        }
        g_strfreev(names);
    }
}

xd_fp_state xd_fp_perms_state(const xd_fp_perms *permissions, xd_fp_list list, const char *name) {
    return (xd_fp_state)GPOINTER_TO_INT(g_hash_table_lookup(permissions->state[list], name));
}

const GPtrArray *xd_fp_perms_conditions(const xd_fp_perms *permissions, xd_fp_list list,
                                        const char *name) {
    return g_hash_table_lookup(permissions->conds[list], name);
}

static int compare_strings(gconstpointer left, gconstpointer right) {
    return g_strcmp0(*(const char *const *)left, *(const char *const *)right);
}

/* The table's keys, sorted, borrowed. */
static GPtrArray *sorted_keys(GHashTable *table) {
    GPtrArray *array = g_ptr_array_new();
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key, nullptr))
        g_ptr_array_add(array, key);
    g_ptr_array_sort(array, compare_strings);
    return array;
}

GPtrArray *xd_fp_perms_other_names(const xd_fp_perms *permissions, xd_fp_list list) {
    GPtrArray *all = sorted_keys(permissions->state[list]);
    GPtrArray *other = g_ptr_array_new();
    for (guint i = 0; i < all->len; i++) {
        if (!g_strv_contains(xd_fp_list_names(list), g_ptr_array_index(all, i)))
            g_ptr_array_add(other, g_ptr_array_index(all, i));
    }
    g_ptr_array_unref(all);
    return other;
}

GPtrArray *xd_fp_perms_filesystems(const xd_fp_perms *permissions) {
    return sorted_keys(permissions->fs);
}

xd_fp_fs_mode xd_fp_perms_filesystem(const xd_fp_perms *permissions, const char *fs, bool *found) {
    char *canon = xd_fp_filesystem_parse(fs, &(xd_fp_fs_mode){0});
    int stored =
        canon != nullptr ? GPOINTER_TO_INT(g_hash_table_lookup(permissions->fs, canon)) : 0;
    g_free(canon);
    *found = stored != 0;
    return stored != 0 ? (xd_fp_fs_mode)(stored - 1) : XD_FP_FS_NONE;
}

GPtrArray *xd_fp_perms_persistent(const xd_fp_perms *permissions) {
    return sorted_keys(permissions->persistent);
}

GPtrArray *xd_fp_perms_env_names(const xd_fp_perms *permissions) {
    return sorted_keys(permissions->env);
}

const char *xd_fp_perms_env(const xd_fp_perms *permissions, const char *name, bool *found) {
    gpointer value = nullptr;
    *found = g_hash_table_lookup_extended(permissions->env, name, nullptr, &value);
    return value;
}

GPtrArray *xd_fp_perms_bus_names(const xd_fp_perms *permissions, xd_fp_bus bus) {
    return sorted_keys(permissions->bus[bus]);
}

const char *xd_fp_perms_bus(const xd_fp_perms *permissions, xd_fp_bus bus, const char *name) {
    return g_hash_table_lookup(permissions->bus[bus], name);
}

/* ---- names --------------------------------------------------------------------- */

/* Doubled slashes, "/./" and trailing slashes go. */
static char *tidy_path(const char *path) {
    GString *string = g_string_new(nullptr);
    char **parts = g_strsplit(path, "/", -1);
    for (char **part = parts; *part != nullptr; part++) {
        if (part == parts) {
            g_string_append(string, *part);
            continue;
        }
        if (**part == '\0' || strcmp(*part, ".") == 0)
            continue;
        g_string_append_c(string, '/');
        g_string_append(string, *part);
    }
    g_strfreev(parts);
    if (string->len == 0 && path[0] == '/')
        g_string_append_c(string, '/');
    return g_string_free(string, FALSE);
}

char *xd_fp_filesystem_parse(const char *entry, xd_fp_fs_mode *mode) {
    *mode = XD_FP_FS_RW;
    bool negated = entry[0] == '!';
    if (negated)
        entry++;
    char *fs = g_strdup(entry);
    static const struct {
        const char *suffix;
        xd_fp_fs_mode mode;
    } suffixes[] = {{":ro", XD_FP_FS_RO}, {":rw", XD_FP_FS_RW}, {":create", XD_FP_FS_CREATE}};
    for (size_t i = 0; i < G_N_ELEMENTS(suffixes); i++) {
        if (g_str_has_suffix(fs, suffixes[i].suffix)) {
            fs[strlen(fs) - strlen(suffixes[i].suffix)] = '\0';
            *mode = suffixes[i].mode;
            break;
        }
    }
    if (negated)
        *mode = XD_FP_FS_NONE;
    char *tidy = tidy_path(fs);
    g_free(fs);
    if (*tidy == '\0') {
        g_free(tidy);
        return nullptr;
    }
    char *canon;
    if (strcmp(tidy, "~") == 0)
        canon = g_strdup("home");
    else if (g_str_has_prefix(tidy, "home/"))
        canon = g_strconcat("~/", tidy + 5, nullptr);
    else
        canon = g_strdup(tidy);
    g_free(tidy);
    return canon;
}

bool xd_fp_filesystem_valid(const char *fs) {
    if (fs == nullptr || *fs == '\0' || strchr(fs, '\n') != nullptr)
        return false;
    if (fs[0] == '/' || g_str_has_prefix(fs, "~/") || strcmp(fs, "~") == 0)
        return true;
    /* Keywords that stand alone only. */
    static const char *const alone[] = {"host", "host-os", "host-etc", "host-root", nullptr};
    if (g_strv_contains(alone, fs))
        return true;
    /* Keywords that may have a /subpath; xdg-run must. */
    static const char *const with_path[] = {
        "home",        "xdg-desktop", "xdg-documents", "xdg-download", "xdg-music",
        "xdg-pictures", "xdg-public-share", "xdg-videos", "xdg-templates", "xdg-cache",
        "xdg-config",  "xdg-data",    nullptr};
    const char *slash = strchr(fs, '/');
    char *keyword = slash != nullptr ? g_strndup(fs, (gsize)(slash - fs)) : g_strdup(fs);
    bool ok = g_strv_contains(with_path, keyword) ||
              (strcmp(keyword, "xdg-run") == 0 && slash != nullptr && slash[1] != '\0');
    g_free(keyword);
    return ok;
}

bool xd_fp_bus_name_valid(const char *name) {
    if (name == nullptr || *name == '\0')
        return false;
    char *base = g_str_has_suffix(name, ".*") ? g_strndup(name, strlen(name) - 2)
                                               : g_strdup(name);
    bool ok = g_dbus_is_name(base) && !g_dbus_is_unique_name(base);
    g_free(base);
    return ok;
}

bool xd_fp_env_name_valid(const char *name) {
    if (name == nullptr || !(g_ascii_isalpha(*name) || *name == '_'))
        return false;
    for (const char *cursor = name; *cursor != '\0'; cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '_')
            return false;
    }
    return true;
}

/* ---- the override file --------------------------------------------------------- */

char *xd_fp_override_path(const char *dir, const char *app_id) {
    return g_build_filename(dir, "overrides", app_id, nullptr);
}

GKeyFile *xd_fp_override_load(const char *path, GError **error) {
    GKeyFile *kf = g_key_file_new();
    GError *load_error = nullptr;
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, &load_error)) {
        if (g_error_matches(load_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free(load_error);
            return kf;
        }
        g_propagate_error(error, load_error);
        g_key_file_free(kf);
        return nullptr;
    }
    return kf;
}

/* Drops the group when nothing is left in it. */
static void tidy_group(GKeyFile *kf, const char *group) {
    gsize key_count = 0;
    char **keys = g_key_file_get_keys(kf, group, &key_count, nullptr);
    g_strfreev(keys);
    if (keys != nullptr && key_count == 0)
        g_key_file_remove_group(kf, group, nullptr);
}

static void errno_error(GError **error, const char *path, int err) {
    GFileError code = g_file_error_from_errno(err);
    g_set_error(error, G_FILE_ERROR, (gint)code, "%s: %s", path, g_strerror(err));
}

bool xd_fp_override_save(GKeyFile *override, const char *path, GError **error) {
    gsize group_count = 0;
    char **groups = g_key_file_get_groups(override, &group_count);
    for (gsize i = 0; i < group_count; i++)
        tidy_group(override, groups[i]);
    g_strfreev(groups);
    groups = g_key_file_get_groups(override, &group_count);
    g_strfreev(groups);
    if (group_count == 0) {
        if (g_unlink(path) != 0 && errno != ENOENT) {
            errno_error(error, path, errno);
            return false;
        }
        return true;
    }
    char *dir = g_path_get_dirname(path);
    int made = g_mkdir_with_parents(dir, 0755);
    int err = errno;
    g_free(dir);
    if (made != 0) {
        errno_error(error, path, err);
        return false;
    }
    return g_key_file_save_to_file(override, path, error);
}

/* A [Context] list as an array (empty when missing). */
static GPtrArray *context_list(GKeyFile *kf, const char *key) {
    GPtrArray *array = strv_array();
    char **entries = g_key_file_get_string_list(kf, CONTEXT, key, nullptr, nullptr);
    for (char **element = entries; element != nullptr && *element != nullptr; element++)
        g_ptr_array_add(array, g_strdup(*element));
    g_strfreev(entries);
    return array;
}

/* Writes it back, or drops the key when it's empty; frees `array`. */
static void set_context_list(GKeyFile *kf, const char *key, GPtrArray *array) {
    if (array->len == 0) {
        g_key_file_remove_key(kf, CONTEXT, key, nullptr);
        tidy_group(kf, CONTEXT);
    } else {
        g_key_file_set_string_list(kf, CONTEXT, key, (const char *const *)array->pdata, array->len);
    }
    g_ptr_array_unref(array);
}

void xd_fp_override_set_state(GKeyFile *override, xd_fp_list list, const char *name,
                              xd_fp_state state) {
    GPtrArray *array = context_list(override, list_keys[list]);
    char *negated = g_strconcat("!", name, nullptr);
    char *conditional = g_strconcat("if:", name, ":", nullptr);
    for (guint i = array->len; i-- > 0;) {
        const char *element = g_ptr_array_index(array, i);
        if (strcmp(element, name) == 0 || strcmp(element, negated) == 0 ||
            g_str_has_prefix(element, conditional))
            g_ptr_array_remove_index(array, i);
    }
    if (state == XD_FP_ON)
        g_ptr_array_add(array, g_strdup(name));
    else if (state == XD_FP_OFF)
        g_ptr_array_add(array, g_strdup(negated));
    g_free(negated);
    g_free(conditional);
    set_context_list(override, list_keys[list], array);
}

void xd_fp_override_set_filesystem(GKeyFile *override, const char *fs, xd_fp_fs_mode mode,
                                   bool remove) {
    xd_fp_fs_mode ignored;
    char *canon = xd_fp_filesystem_parse(fs, &ignored);
    if (canon == nullptr)
        return;
    GPtrArray *array = context_list(override, "filesystems");
    for (guint i = array->len; i-- > 0;) {
        const char *element = g_ptr_array_index(array, i);
        if (is_reset(element))
            continue;
        char *other = xd_fp_filesystem_parse(element, &ignored);
        if (g_strcmp0(other, canon) == 0)
            g_ptr_array_remove_index(array, i);
        g_free(other);
    }
    if (!remove) {
        g_ptr_array_add(array, mode == XD_FP_FS_NONE     ? g_strconcat("!", canon, nullptr)
                               : mode == XD_FP_FS_RO     ? g_strconcat(canon, ":ro", nullptr)
                               : mode == XD_FP_FS_CREATE ? g_strconcat(canon, ":create", nullptr)
                                                         : g_strdup(canon));
    }
    g_free(canon);
    set_context_list(override, "filesystems", array);
}

void xd_fp_override_set_persistent(GKeyFile *override, const char *path, bool present) {
    GPtrArray *array = context_list(override, "persistent");
    for (guint i = array->len; i-- > 0;) {
        if (strcmp(g_ptr_array_index(array, i), path) == 0)
            g_ptr_array_remove_index(array, i);
    }
    if (present)
        g_ptr_array_add(array, g_strdup(path));
    set_context_list(override, "persistent", array);
}

void xd_fp_override_set_env(GKeyFile *override, const char *name, const char *value,
                            bool remove) {
    GPtrArray *array = context_list(override, "unset-environment");
    for (guint i = array->len; i-- > 0;) {
        if (strcmp(g_ptr_array_index(array, i), name) == 0)
            g_ptr_array_remove_index(array, i);
    }
    g_key_file_remove_key(override, ENVIRONMENT, name, nullptr);
    if (!remove && value == nullptr) {
        g_ptr_array_add(array, g_strdup(name));
        g_key_file_set_string(override, ENVIRONMENT, name, "");
    } else if (!remove) {
        g_key_file_set_string(override, ENVIRONMENT, name, value);
    }
    set_context_list(override, "unset-environment", array);
    tidy_group(override, ENVIRONMENT);
}

void xd_fp_override_set_bus(GKeyFile *override, xd_fp_bus bus, const char *name,
                            const char *policy) {
    const char *group = xd_fp_bus_group(bus);
    if (policy != nullptr) {
        g_key_file_set_string(override, group, name, policy);
    } else {
        g_key_file_remove_key(override, group, name, nullptr);
        tidy_group(override, group);
    }
}
