/* SPDX-License-Identifier: MIT
 * Input method frameworks: see inputmethod.h.
 */
#include "inputmethod.h"

#include <stdlib.h>
#include <string.h>

#include "settings.h"

typedef struct framework_info {
    const char *name;    /* InputMethod's value */
    const char *display; /* what a list shows */
    const char *program; /* on PATH when installed */
    const char *const *argv;
    const char *config_tool;
    const char *env[XD_IM_ENV_VARS];
    const char *emoji;
} framework_info;

static const char *const fcitx5_argv[] = {"fcitx5", nullptr};
static const char *const ibus_argv[] = {"ibus", "start", "--type", "wayland", nullptr};

static const framework_info frameworks[XD_IM_COUNT] = {
    [XD_IM_NONE] = {.name = "", .display = "(None)", .env = {"", "", ""}},
    [XD_IM_FCITX5] =
        {
            .name = "fcitx5",
            .display = "Fcitx 5",
            .program = "fcitx5",
            .argv = fcitx5_argv,
            .config_tool = "fcitx5-configtool",
            .env = {"@im=fcitx", "fcitx", "wayland;fcitx"},
            .emoji = "In a text box, press Ctrl+Alt+Shift+U to look up emoji and other "
                     "characters by name (Fcitx 5's Unicode input).",
        },
    [XD_IM_IBUS] =
        {
            .name = "ibus",
            .display = "IBus",
            .program = "ibus-daemon",
            .argv = ibus_argv,
            .config_tool = "ibus-setup",
            .env = {"@im=ibus", "ibus", "wayland;ibus"},
            .emoji = "In a text box, press Windows+Period to choose an emoji, or "
                     "Ctrl+Shift+U to type a character by its code (IBus).",
        },
};

const char *const xd_im_env_vars[XD_IM_ENV_VARS] = {"XMODIFIERS", "QT_IM_MODULE",
                                                    "QT_IM_MODULES"};

xd_im_framework xd_im_from_name(const char *name) {
    for (int i = XD_IM_NONE + 1; name != nullptr && i < XD_IM_COUNT; i++) {
        if (strcmp(name, frameworks[i].name) == 0)
            return (xd_im_framework)i;
    }
    return XD_IM_NONE;
}

static const framework_info *info(xd_im_framework fw) {
    return &frameworks[fw < XD_IM_COUNT ? fw : XD_IM_NONE];
}

const char *xd_im_name(xd_im_framework fw) { return info(fw)->name; }

const char *xd_im_display_name(xd_im_framework fw) { return info(fw)->display; }

bool xd_im_installed(xd_im_framework fw) {
    const char *program = info(fw)->program;
    if (program == nullptr)
        return true;
    char *path = g_find_program_in_path(program);
    bool found = path != nullptr;
    g_free(path);
    return found;
}

const char *const *xd_im_argv(xd_im_framework fw) { return info(fw)->argv; }

const char *xd_im_config_tool(xd_im_framework fw) { return info(fw)->config_tool; }

const char *xd_im_emoji_hint(xd_im_framework fw) { return info(fw)->emoji; }

bool xd_im_program_is_framework(const char *basename) {
    /* Fcitx 5 is one program; IBus's Wayland input method is ibus-wayland
     * (in libexec), started by ibus-daemon. */
    static const char *const programs[] = {"fcitx5", "ibus-wayland", "ibus-daemon"};
    for (size_t i = 0; basename != nullptr && i < XD_ARRAY_LEN(programs); i++) {
        if (strcmp(basename, programs[i]) == 0)
            return true;
    }
    return false;
}

const char *xd_im_env_value(xd_im_framework fw, size_t i) {
    return i < XD_IM_ENV_VARS ? info(fw)->env[i] : "";
}

bool xd_im_env_apply(xd_im_framework fw) {
    bool changed = false;
    for (size_t i = 0; i < XD_IM_ENV_VARS; i++) {
        const char *var = xd_im_env_vars[i];
        const char *value = xd_im_env_value(fw, i);
        const char *old = getenv(var);
        if (old != nullptr && !xd_qt_env_ours(var))
            continue; /* the user's own */
        if (old == nullptr && value[0] == '\0')
            continue; /* never set, and nothing to set */
        changed = changed || old == nullptr || strcmp(old, value) != 0;
        char *mark = g_strconcat("XEONDESKTOP_", var, nullptr);
        setenv(var, value, true);
        setenv(mark, value, true);
        g_free(mark);
    }
    return changed;
}

/* Whether the autostart entry starts a framework: its Exec's program. */
static bool starts_framework(GKeyFile *kf) {
    char *exec = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                       G_KEY_FILE_DESKTOP_KEY_EXEC, nullptr);
    char **argv = nullptr;
    bool yes = false;
    if (exec != nullptr && g_shell_parse_argv(exec, nullptr, &argv, nullptr)) {
        char *base = g_path_get_basename(argv[0]);
        /* "ibus start" and "ibus-daemon" start IBus. */
        yes = xd_im_program_is_framework(base) || strcmp(base, "ibus") == 0;
        g_free(base);
    }
    g_strfreev(argv);
    g_free(exec);
    return yes;
}

static constexpr char MARK_KEY[] = "X-XeonDesktop-InputMethod";

/* The user's copy of entry `name` may be (over)written: there is none,
 * or it is ours. */
static bool ours_or_absent(const char *path) {
    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return true;
    GKeyFile *kf = g_key_file_new();
    bool ours = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr) &&
                g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP, MARK_KEY, nullptr);
    g_key_file_free(kf);
    return ours;
}

static bool override_entry(const char *system_path, const char *user_path, GError **error) {
    GKeyFile *kf = g_key_file_new();
    bool ok = true;
    if (g_key_file_load_from_file(kf, system_path,
                                  G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                  nullptr) &&
        starts_framework(kf)) {
        gsize n = 0;
        char **not_in = g_key_file_get_string_list(kf, G_KEY_FILE_DESKTOP_GROUP,
                                                   G_KEY_FILE_DESKTOP_KEY_NOT_SHOW_IN, &n,
                                                   nullptr);
        bool listed = false;
        for (gsize i = 0; i < n; i++)
            listed = listed || strcmp(not_in[i], "XeonDesktop") == 0;
        if (!listed) {
            char **more = g_new0(char *, n + 2);
            for (gsize i = 0; i < n; i++)
                more[i] = not_in[i];
            more[n] = (char *)(uintptr_t) "XeonDesktop";
            g_key_file_set_string_list(kf, G_KEY_FILE_DESKTOP_GROUP,
                                       G_KEY_FILE_DESKTOP_KEY_NOT_SHOW_IN,
                                       (const char *const *)more, n + 1);
            g_free(more);
        }
        g_strfreev(not_in);
        g_key_file_set_boolean(kf, G_KEY_FILE_DESKTOP_GROUP, MARK_KEY, true);
        ok = g_key_file_save_to_file(kf, user_path, error);
    }
    g_key_file_free(kf);
    return ok;
}

bool xd_im_autostart_override(const char *const *system_dirs, const char *user_dir,
                              GError **error) {
    bool ok = true;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (size_t d = 0; system_dirs != nullptr && system_dirs[d] != nullptr && ok; d++) {
        GDir *dir = g_dir_open(system_dirs[d], 0, nullptr);
        if (dir == nullptr)
            continue;
        const char *name;
        while ((name = g_dir_read_name(dir)) != nullptr && ok) {
            /* The first directory with an entry wins, as in the
             * autostart specification. */
            if (!g_str_has_suffix(name, ".desktop") || g_hash_table_contains(seen, name))
                continue;
            g_hash_table_add(seen, g_strdup(name));
            char *user_path = g_build_filename(user_dir, name, nullptr);
            if (ours_or_absent(user_path)) {
                char *system_path = g_build_filename(system_dirs[d], name, nullptr);
                if (g_mkdir_with_parents(user_dir, 0700) != 0) {
                    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "cannot create %s",
                                user_dir);
                    ok = false;
                } else {
                    ok = override_entry(system_path, user_path, error);
                }
                g_free(system_path);
            }
            g_free(user_path);
        }
        g_dir_close(dir);
    }
    g_hash_table_unref(seen);
    return ok;
}

bool xd_im_autostart_override_session(GError **error) {
    const char *const *config_dirs = g_get_system_config_dirs();
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    for (size_t i = 0; config_dirs[i] != nullptr; i++)
        g_ptr_array_add(dirs, g_build_filename(config_dirs[i], "autostart", nullptr));
    g_ptr_array_add(dirs, nullptr);
    char *user = g_build_filename(g_get_user_config_dir(), "autostart", nullptr);
    bool ok = xd_im_autostart_override((const char *const *)dirs->pdata, user, error);
    g_free(user);
    g_ptr_array_unref(dirs);
    return ok;
}
