/* SPDX-License-Identifier: MIT
 * Startup programs: XDG autostart entries (the Desktop Application
 * Autostart Specification), read and written through GKeyFile and
 * GDesktopAppInfo.
 */
#include <gio/gdesktopappinfo.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#include "startup.h"

#define GROUP G_KEY_FILE_DESKTOP_GROUP
#define KEY_HIDDEN G_KEY_FILE_DESKTOP_KEY_HIDDEN
/* GNOME's own switch, which gnome-session and GNOME Tweaks write. */
#define KEY_GNOME_ENABLED "X-GNOME-Autostart-enabled"
#define KEY_GNOME_PHASE "X-GNOME-Autostart-Phase"
#define KEY_SYSTEMD_SKIP "X-systemd-skip"

void xd_autostart_free(xd_autostart *item) {
    if (item == nullptr)
        return;
    g_free(item->file);
    g_free(item->name);
    g_free(item->command);
    g_free(item->icon);
    g_free(item->path);
    g_free(item->location);
    g_free(item);
}

static GKeyFile *load(const char *path, GError **error) {
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                   error)) {
        g_key_file_free(kf);
        return nullptr;
    }
    return kf;
}

static bool key_bool(GKeyFile *kf, const char *key, bool fallback) {
    GError *error = nullptr;
    bool value = g_key_file_get_boolean(kf, GROUP, key, &error);
    if (error != nullptr) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

/* Starts at log on, by the file that counts: not Hidden, and not turned
 * off GNOME's way. */
static bool starts(GKeyFile *kf) {
    return !key_bool(kf, KEY_HIDDEN, false) && key_bool(kf, KEY_GNOME_ENABLED, true);
}

/* The first of `dirs` with `file` in it, or nullptr. g_free. */
static char *find_in(const char *const *dirs, const char *file) {
    for (size_t i = 0; dirs != nullptr && dirs[i] != nullptr; i++) {
        char *path = g_build_filename(dirs[i], file, nullptr);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
            return path;
        g_free(path);
    }
    return nullptr;
}

static void add_names(const char *dir, GPtrArray *names, GHashTable *seen) {
    GDir *directory = dir != nullptr ? g_dir_open(dir, 0, nullptr) : nullptr;
    if (directory == nullptr)
        return;
    const char *name;
    while ((name = g_dir_read_name(directory)) != nullptr) {
        if (!g_str_has_suffix(name, ".desktop") || g_hash_table_contains(seen, name))
            continue;
        char *copy = g_strdup(name);
        g_hash_table_add(seen, copy);
        g_ptr_array_add(names, copy);
    }
    g_dir_close(directory);
}

static int by_name(gconstpointer left, gconstpointer right) {
    const xd_autostart *left_item = *(const xd_autostart *const *)left;
    const xd_autostart *right_item = *(const xd_autostart *const *)right;
    int order = g_utf8_collate(left_item->name, right_item->name);
    return order != 0 ? order : strcmp(left_item->file, right_item->file);
}

/* One entry, or nullptr when it doesn't apply here. */
static xd_autostart *entry(const char *file, const char *user_path, const char *system_path,
                           const char *desktop) {
    const char *winner = user_path != nullptr ? user_path : system_path;
    GKeyFile *kf = load(winner, nullptr);
    if (kf == nullptr)
        return nullptr;
    bool enabled = starts(kf);
    g_key_file_free(kf);
    /* Your copy that only hides a system entry shows that entry. */
    const char *shown = !enabled && system_path != nullptr ? system_path : winner;
    GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(shown);
    if (info == nullptr) /* not an application, or TryExec isn't there */
        return nullptr;
    xd_autostart *item = nullptr;
    if (g_desktop_app_info_get_show_in(info, desktop) &&
        !g_desktop_app_info_get_boolean(info, KEY_SYSTEMD_SKIP) &&
        !g_desktop_app_info_has_key(info, KEY_GNOME_PHASE)) {
        item = g_new0(xd_autostart, 1);
        GAppInfo *ai = G_APP_INFO(info);
        const char *command = g_app_info_get_commandline(ai);
        char *icon = g_desktop_app_info_get_string(info, G_KEY_FILE_DESKTOP_KEY_ICON);
        item->file = g_strdup(file);
        item->name = g_strdup(g_app_info_get_name(ai));
        item->command = g_strdup(command != nullptr ? command : "");
        item->icon = icon != nullptr && icon[0] != '\0' ? icon : nullptr;
        if (item->icon == nullptr)
            g_free(icon);
        item->path = g_strdup(shown);
        item->location = g_path_get_dirname(shown);
        item->user_only = system_path == nullptr;
        item->enabled = enabled;
    }
    g_object_unref(info);
    return item;
}

GPtrArray *xd_autostart_list(const char *user_dir, const char *const *system_dirs,
                             const char *desktop) {
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)xd_autostart_free);
    GPtrArray *names = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    add_names(user_dir, names, seen);
    for (size_t i = 0; system_dirs != nullptr && system_dirs[i] != nullptr; i++)
        add_names(system_dirs[i], names, seen);
    const char *user_dirs[] = {user_dir, nullptr};
    for (guint i = 0; i < names->len; i++) {
        const char *file = names->pdata[i];
        char *user_path = find_in(user_dirs, file);
        char *system_path = find_in(system_dirs, file);
        xd_autostart *item = entry(file, user_path, system_path, desktop);
        if (item != nullptr)
            g_ptr_array_add(list, item);
        g_free(user_path);
        g_free(system_path);
    }
    g_ptr_array_unref(names);
    g_hash_table_destroy(seen);
    g_ptr_array_sort(list, by_name);
    return list;
}

/* errno, as GLib's own file functions report it. Returns false. */
static bool errno_error(GError **error, const char *path) {
    int error_number = errno;
    GFileError code = g_file_error_from_errno(error_number);
    g_set_error(error, G_FILE_ERROR, (int)code, "%s: %s", path, g_strerror(error_number));
    return false;
}

static bool save(GKeyFile *kf, const char *dir, const char *path, GError **error) {
    if (g_mkdir_with_parents(dir, 0700) != 0)
        return errno_error(error, dir);
    return g_key_file_save_to_file(kf, path, error);
}

/* GKeyFile's text of a file, for "differs in nothing else". */
static char *text_of(GKeyFile *kf) { return g_key_file_to_data(kf, nullptr, nullptr); }

static bool unlink_file(const char *path, GError **error) {
    return g_unlink(path) == 0 || errno_error(error, path);
}

/* `kf` (yours, or a copy of the system's `sys`) turned on or off and
 * saved as `user_path`, or removed when it would only repeat `sys`. */
static bool write_enabled(GKeyFile *kf, GKeyFile *sys, const char *user_dir,
                          const char *user_path, bool have_user, bool enabled, GError **error) {
    if (!enabled) {
        g_key_file_set_boolean(kf, GROUP, KEY_HIDDEN, TRUE);
        return save(kf, user_dir, user_path, error);
    }
    g_key_file_remove_key(kf, GROUP, KEY_HIDDEN, nullptr);
    /* GNOME's switch: on where the system's entry has it off, else gone. */
    if (sys != nullptr && !key_bool(sys, KEY_GNOME_ENABLED, true))
        g_key_file_set_boolean(kf, GROUP, KEY_GNOME_ENABLED, TRUE);
    else
        g_key_file_remove_key(kf, GROUP, KEY_GNOME_ENABLED, nullptr);
    if (sys != nullptr) {
        char *mine = text_of(kf);
        char *theirs = text_of(sys);
        bool same = strcmp(mine, theirs) == 0;
        g_free(mine);
        g_free(theirs);
        /* Yours only hid it: removed, so the system's entry counts again. */
        if (same)
            return !have_user || unlink_file(user_path, error);
    }
    return save(kf, user_dir, user_path, error);
}

bool xd_autostart_set_enabled(const xd_autostart *item, const char *user_dir, bool enabled,
                              GError **error) {
    char *user_path = g_build_filename(user_dir, item->file, nullptr);
    bool have_user = g_file_test(user_path, G_FILE_TEST_IS_REGULAR);
    /* A system entry: its path is the system's (the list shows that one). */
    const char *system_path = item->user_only ? nullptr : item->path;
    if (system_path != nullptr && strcmp(system_path, user_path) == 0)
        system_path = nullptr;
    /* Yours, or a copy of the system's to make yours. */
    const char *from = have_user ? user_path : system_path;
    bool ok = false;
    GKeyFile *sys = system_path != nullptr ? load(system_path, error) : nullptr;
    if (from == nullptr) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "%s: %s", user_path,
                    g_strerror(ENOENT));
    } else if (system_path == nullptr || sys != nullptr) {
        GKeyFile *kf = load(from, error);
        if (kf != nullptr) {
            ok = write_enabled(kf, sys, user_dir, user_path, have_user, enabled, error);
            g_key_file_free(kf);
        }
    }
    if (sys != nullptr)
        g_key_file_free(sys);
    g_free(user_path);
    return ok;
}

/* A desktop file name from a program's: letters, digits, '.', '-', '_'. */
static char *file_name_for(const char *base) {
    GString *file_name = g_string_new(nullptr);
    for (const char *position = base; *position != '\0'; position++)
        g_string_append_c(file_name, g_ascii_isalnum(*position) || *position == '.' ||
                                             *position == '-' || *position == '_'
                                         ? *position
                                         : '-');
    if (file_name->len == 0)
        g_string_append(file_name, "program");
    return g_string_free(file_name, FALSE);
}

char *xd_autostart_add_app(const char *user_dir, GDesktopAppInfo *app, GError **error) {
    const char *source = g_desktop_app_info_get_filename(app);
    const char *id = g_app_info_get_id(G_APP_INFO(app));
    if (source == nullptr) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "The program has no desktop entry to start.");
        return nullptr;
    }
    GKeyFile *kf = load(source, error);
    if (kf == nullptr)
        return nullptr;
    char *base = id != nullptr ? g_strdup(id) : g_path_get_basename(source);
    char *file = file_name_for(base);
    g_free(base);
    g_key_file_remove_key(kf, GROUP, KEY_HIDDEN, nullptr);
    g_key_file_remove_key(kf, GROUP, KEY_GNOME_ENABLED, nullptr);
    char *path = g_build_filename(user_dir, file, nullptr);
    bool ok = save(kf, user_dir, path, error);
    g_free(path);
    g_key_file_free(kf);
    if (!ok)
        g_clear_pointer(&file, g_free);
    return file;
}

/* Inside an Exec value's double quotes, these take a backslash. */
static bool exec_escaped(char character) {
    return character == '"' || character == '`' || character == '$' || character == '\\';
}

char *xd_exec_quote(const char *const *argv) {
    /* The spec's reserved characters: an argument with any is quoted. */
    static const char reserved[] = " \t\n\"'\\><~|&;$*?#()`";
    GString *quoted = g_string_new(nullptr);
    for (size_t i = 0; argv != nullptr && argv[i] != nullptr; i++) {
        const char *argument = argv[i];
        if (i > 0)
            g_string_append_c(quoted, ' ');
        bool quote = argument[0] == '\0' || strpbrk(argument, reserved) != nullptr;
        if (quote)
            g_string_append_c(quoted, '"');
        for (const char *position = argument; *position != '\0'; position++) {
            if (*position == '%')
                g_string_append_c(quoted, '%'); /* a literal percent sign */
            else if (quote && exec_escaped(*position))
                g_string_append_c(quoted, '\\');
            g_string_append_c(quoted, *position);
        }
        if (quote)
            g_string_append_c(quoted, '"');
    }
    return g_string_free(quoted, FALSE);
}

char *xd_autostart_add_command(const char *user_dir, const char *command, GError **error) {
    char **argv = nullptr;
    if (command == nullptr || !g_shell_parse_argv(command, nullptr, &argv, nullptr)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The command isn't valid. Check the quotation marks in it.");
        return nullptr;
    }
    char *base = g_path_get_basename(argv[0]);
    char *stem = file_name_for(base);
    /* foo.desktop, else foo-2.desktop, ...: never over another entry. */
    char *file = nullptr;
    for (int count = 1; file == nullptr; count++) {
        char *candidate = count == 1 ? g_strconcat(stem, ".desktop", nullptr)
                                     : g_strdup_printf("%s-%d.desktop", stem, count);
        char *path = g_build_filename(user_dir, candidate, nullptr);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            g_free(candidate);
        else
            file = candidate;
        g_free(path);
    }
    GKeyFile *kf = g_key_file_new();
    char *exec = xd_exec_quote((const char *const *)argv);
    g_key_file_set_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE,
                          G_KEY_FILE_DESKTOP_TYPE_APPLICATION);
    g_key_file_set_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, base);
    g_key_file_set_string(kf, GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, exec);
    char *path = g_build_filename(user_dir, file, nullptr);
    if (!save(kf, user_dir, path, error))
        g_clear_pointer(&file, g_free);
    g_free(path);
    g_free(exec);
    g_key_file_free(kf);
    g_free(stem);
    g_free(base);
    g_strfreev(argv);
    return file;
}

bool xd_autostart_remove(const xd_autostart *item, GError **error) {
    if (!item->user_only) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "%s is installed for every user; you can only turn it off.", item->name);
        return false;
    }
    return unlink_file(item->path, error);
}
