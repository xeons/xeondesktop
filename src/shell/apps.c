/* SPDX-License-Identifier: MIT */
#include <gio/gdesktopappinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../files/control.h"
#include "apps.h"
#include "gpu.h"

/* Windows-style Programs folders for the freedesktop main categories,
 * checked in this order (an entry often lists several). */
static const struct {
    const char *category;
    const char *folder;
    const char *icon;
} folders[] = {
    {"Game", "Games", "applications-games"},
    {"Settings", "Settings", "preferences-desktop"},
    {"Development", "Programming", "applications-development"},
    {"Education", "Education", "applications-education"},
    {"Science", "Education", "applications-science"},
    {"Graphics", "Graphics", "applications-graphics"},
    {"AudioVideo", "Multimedia", "applications-multimedia"},
    {"Audio", "Multimedia", "applications-multimedia"},
    {"Video", "Multimedia", "applications-multimedia"},
    {"Network", "Internet", "applications-internet"},
    {"Office", "Office", "applications-office"},
    {"System", "System Tools", "applications-system"},
    {"Utility", "Accessories", "applications-accessories"},
};

/* Does a ';'-separated list contain `item`? */
static bool list_contains(const char *list, const char *item) {
    xd_str rest = xd_str_from_cstr(list);
    xd_str want = xd_str_from_cstr(item);
    while (rest.len > 0) {
        size_t semi = rest.len;
        bool more = xd_str_find_char(rest, ';', &semi);
        if (xd_str_eq(xd_str_trim(xd_str_sub(rest, 0, semi)), want))
            return true;
        rest = more ? xd_str_sub(rest, semi + 1, rest.len) : XD_STR("");
    }
    return false;
}

static const char *arena_copy(xd_arena *arena, const char *text) {
    return text != nullptr ? xd_str_arena_cstr(arena, xd_str_from_cstr(text)) : nullptr;
}

/* Fills `out` from a GDesktopAppInfo, taking a reference. */
static void fill(GDesktopAppInfo *info, xd_arena *arena, xd_app *out) {
    GAppInfo *ai = G_APP_INFO(info);
    const char *folder = "Other";
    const char *folder_icon = "applications-other";
    const char *categories = g_desktop_app_info_get_categories(info);
    if (categories != nullptr) {
        for (size_t i = 0; i < XD_ARRAY_LEN(folders); i++) {
            if (list_contains(categories, folders[i].category)) {
                folder = folders[i].folder;
                folder_icon = folders[i].icon;
                break;
            }
        }
    }
    /* The Icon key as written (a theme name or a path), for our own icon
     * lookup at the sizes the shell draws. */
    char *icon = g_desktop_app_info_get_string(info, "Icon");
    char *action = g_desktop_app_info_get_string(info, "X-XeonDesktop-Action");
    xd_strbuf keywords = {0};
    const char *const *kw = g_desktop_app_info_get_keywords(info);
    for (size_t i = 0; kw != nullptr && kw[i] != nullptr; i++) {
        if (i > 0)
            xd_strbuf_append_char(&keywords, ' ');
        xd_strbuf_append(&keywords, xd_str_from_cstr(kw[i]));
    }
    const char *id = g_app_info_get_id(ai);

    *out = (xd_app){
        .id = arena_copy(arena, id != nullptr ? id : ""),
        .name = arena_copy(arena, g_app_info_get_name(ai)),
        .icon = icon != nullptr && icon[0] != '\0' ? arena_copy(arena, icon) : nullptr,
        .folder = folder,
        .folder_icon = folder_icon,
        .action = action != nullptr && action[0] != '\0' ? arena_copy(arena, action) : nullptr,
        .keywords = keywords.len > 0 ? arena_copy(arena, xd_strbuf_cstr(&keywords)) : nullptr,
        .visible = g_app_info_should_show(ai),
        .info = g_object_ref(info),
    };
    xd_strbuf_free(&keywords);
    g_free(action);
    g_free(icon);
}

static int compare_apps(const void *left, const void *right) {
    const xd_app *left_app = left;
    const xd_app *right_app = right;
    int order = g_utf8_collate(left_app->folder, right_app->folder);
    return order != 0 ? order : g_utf8_collate(left_app->name, right_app->name);
}

void xd_apps_load(xd_app_list *list) {
    *list = (xd_app_list){0};
    xd_arena_init(&list->arena, 64 * 1024);
    /* All installed apps, with user entries overriding system ones, and
     * Hidden/TryExec handled by GIO. */
    GList *all = g_app_info_get_all();
    for (GList *node = all; node != nullptr; node = node->next) {
        if (!G_IS_DESKTOP_APP_INFO(node->data) || !g_app_info_should_show(node->data))
            continue;
        if (list->count == list->cap) {
            list->cap = list->cap == 0 ? 64 : list->cap * 2;
            list->items = xd_realloc_array(list->items, list->cap, sizeof(*list->items));
        }
        fill(G_DESKTOP_APP_INFO(node->data), &list->arena, &list->items[list->count++]);
    }
    g_list_free_full(all, g_object_unref);
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(*list->items), compare_apps);
}

void xd_apps_load_control_panel(xd_app_list *list) {
    *list = (xd_app_list){0};
    xd_arena_init(&list->arena, 4 * 1024);
    GList *applets = xd_control_applets(); /* by name */
    for (GList *node = applets; node != nullptr; node = node->next) {
        if (list->count == list->cap) {
            list->cap = list->cap == 0 ? 16 : list->cap * 2;
            list->items = xd_realloc_array(list->items, list->cap, sizeof(*list->items));
        }
        fill(node->data, &list->arena, &list->items[list->count++]);
    }
    g_list_free_full(applets, g_object_unref);
}

void xd_apps_free(xd_app_list *list) {
    for (size_t i = 0; i < list->count; i++)
        xd_app_release(&list->items[i]);
    xd_free(list->items);
    xd_arena_destroy(&list->arena);
    *list = (xd_app_list){0};
}

bool xd_app_from_file(const char *path, xd_arena *arena, xd_app *out) {
    GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(path);
    if (info == nullptr)
        return false;
    fill(info, arena, out);
    g_object_unref(info);
    return true;
}

void xd_app_release(xd_app *app) {
    if (app->info != nullptr)
        g_object_unref(app->info);
    app->info = nullptr;
}

/* Runs in the child between fork and exec: undo the shell's blocked
 * signals and ignored SIGCHLD, and leave the shell's session so closing
 * the shell doesn't take launched apps with it. Async-signal-safe only. */
static void child_setup([[maybe_unused]] gpointer data) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    setsid();
}

/* Launches `info` detached; `chooser` is the program whose graphics card
 * it starts on: card `gpu`, or with gpu < 0 the program's own (hybrid
 * graphics laptops, gpu.h). */
static bool launch(GDesktopAppInfo *info, GList *uris, GDesktopAppInfo *chooser, int gpu,
                   const char *what) {
    GAppLaunchContext *ctx = g_app_launch_context_new();
    xd_gpu_setup_launch(ctx, chooser, gpu);
    GError *err = nullptr;
    bool ok = g_desktop_app_info_launch_uris_as_manager(info, uris, ctx, G_SPAWN_SEARCH_PATH,
                                                        child_setup, nullptr, nullptr, nullptr,
                                                        &err);
    if (!ok) {
        fprintf(stderr, "xd-shell: cannot launch %s: %s\n", what,
                err != nullptr ? err->message : "unknown error");
        g_clear_error(&err);
    }
    g_object_unref(ctx);
    return ok;
}

bool xd_app_launch(const xd_app *app) { return xd_app_launch_gpu(app, -1); }

bool xd_app_launch_gpu(const xd_app *app, int gpu) {
    if (app->info == nullptr)
        return false;
    return launch(app->info, nullptr, app->info, gpu, app->id);
}

bool xd_app_launch_info(GDesktopAppInfo *info, GList *uris) {
    return launch(info, uris, info, -1, g_app_info_get_name(G_APP_INFO(info)));
}

bool xd_app_launch_action(GDesktopAppInfo *info, const char *action) {
    /* g_desktop_app_info_launch_action can't detach the program: run the
     * action's Exec as a program of its own (GLib still expands it), from
     * the desktop file's "Desktop Action" group. */
    const char *path = g_desktop_app_info_get_filename(info);
    GKeyFile *file = g_key_file_new();
    char *group = g_strconcat("Desktop Action ", action, nullptr);
    char *exec = nullptr;
    if (path != nullptr && g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, nullptr))
        exec = g_key_file_get_string(file, group, G_KEY_FILE_DESKTOP_KEY_EXEC, nullptr);
    bool ok = false;
    if (exec != nullptr) {
        GKeyFile *entry = g_key_file_new();
        g_key_file_set_string(entry, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE,
                              G_KEY_FILE_DESKTOP_TYPE_APPLICATION);
        g_key_file_set_string(entry, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME,
                              g_app_info_get_name(G_APP_INFO(info)));
        g_key_file_set_string(entry, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, exec);
        GDesktopAppInfo *one = g_desktop_app_info_new_from_keyfile(entry);
        if (one != nullptr) {
            /* On the card the program itself starts on. */
            ok = launch(one, nullptr, info, -1, g_app_info_get_name(G_APP_INFO(info)));
            g_object_unref(one);
        }
        g_key_file_free(entry);
    } else {
        fprintf(stderr, "xd-shell: %s has no action %s\n", g_app_info_get_name(G_APP_INFO(info)),
                action);
    }
    g_free(exec);
    g_free(group);
    g_key_file_free(file);
    return ok;
}

