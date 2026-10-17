/* SPDX-License-Identifier: MIT */
#include <gio/gdesktopappinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "apps.h"

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

static const char *arena_copy(xd_arena *arena, const char *s) {
    return s != nullptr ? xd_str_arena_cstr(arena, xd_str_from_cstr(s)) : nullptr;
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

static int compare_apps(const void *a, const void *b) {
    const xd_app *x = a;
    const xd_app *y = b;
    int c = g_utf8_collate(x->folder, y->folder);
    return c != 0 ? c : g_utf8_collate(x->name, y->name);
}

void xd_apps_load(xd_app_list *list) {
    *list = (xd_app_list){0};
    xd_arena_init(&list->arena, 64 * 1024);
    /* All installed apps, with user entries overriding system ones, and
     * Hidden/TryExec handled by GIO. */
    GList *all = g_app_info_get_all();
    for (GList *l = all; l != nullptr; l = l->next) {
        if (!G_IS_DESKTOP_APP_INFO(l->data) || !g_app_info_should_show(l->data))
            continue;
        if (list->count == list->cap) {
            list->cap = list->cap == 0 ? 64 : list->cap * 2;
            list->items = xd_realloc_array(list->items, list->cap, sizeof(*list->items));
        }
        fill(G_DESKTOP_APP_INFO(l->data), &list->arena, &list->items[list->count++]);
    }
    g_list_free_full(all, g_object_unref);
    if (list->count > 1)
        qsort(list->items, list->count, sizeof(*list->items), compare_apps);
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

bool xd_app_launch(const xd_app *app) {
    if (app->info == nullptr)
        return false;
    GError *err = nullptr;
    bool ok = g_desktop_app_info_launch_uris_as_manager(
        app->info, nullptr, nullptr, G_SPAWN_SEARCH_PATH, child_setup, nullptr, nullptr, nullptr,
        &err);
    if (!ok) {
        fprintf(stderr, "xd-shell: cannot launch %s: %s\n", app->id,
                err != nullptr ? err->message : "unknown error");
        g_clear_error(&err);
    }
    return ok;
}

bool xd_app_launch_info(GDesktopAppInfo *info, GList *uris) {
    GError *err = nullptr;
    bool ok = g_desktop_app_info_launch_uris_as_manager(info, uris, nullptr, G_SPAWN_SEARCH_PATH,
                                                        child_setup, nullptr, nullptr, nullptr,
                                                        &err);
    if (!ok) {
        fprintf(stderr, "xd-shell: cannot launch %s: %s\n",
                g_app_info_get_name(G_APP_INFO(info)), err != nullptr ? err->message : "?");
        g_clear_error(&err);
    }
    return ok;
}

static char *desktop_icon(GDesktopAppInfo *info) {
    char *icon = info != nullptr ? g_desktop_app_info_get_string(info, "Icon") : nullptr;
    if (icon != nullptr && icon[0] == '\0') {
        g_free(icon);
        icon = nullptr;
    }
    return icon;
}

static GDesktopAppInfo *desktop_by_id(const char *stem) {
    char *id = g_strconcat(stem, ".desktop", nullptr);
    GDesktopAppInfo *info = g_desktop_app_info_new(id);
    g_free(id);
    return info;
}

bool xd_app_for_window(const char *app_id, char **icon, char **name) {
    *icon = *name = nullptr;
    if (app_id == nullptr || app_id[0] == '\0')
        return false;
    GDesktopAppInfo *info = desktop_by_id(app_id);
    if (info == nullptr) {
        char *lower = g_ascii_strdown(app_id, -1);
        info = desktop_by_id(lower);
        g_free(lower);
    }
    if (info == nullptr) {
        GList *all = g_app_info_get_all();
        GDesktopAppInfo *by_exec = nullptr;
        for (GList *l = all; l != nullptr && info == nullptr; l = l->next) {
            GDesktopAppInfo *d = G_DESKTOP_APP_INFO(l->data);
            const char *wm_class = g_desktop_app_info_get_startup_wm_class(d);
            const char *id = g_app_info_get_id(G_APP_INFO(d));
            size_t n = strlen(app_id);
            if ((wm_class != nullptr && g_ascii_strcasecmp(wm_class, app_id) == 0) ||
                (id != nullptr && g_ascii_strncasecmp(id, app_id, n) == 0 &&
                 strcmp(id + n, ".desktop") == 0))
                info = g_object_ref(d);
            /* Many X11 programs' WM_CLASS is their executable's name. */
            const char *exe = g_app_info_get_executable(G_APP_INFO(d));
            if (by_exec == nullptr && exe != nullptr) {
                const char *base = strrchr(exe, '/');
                if (g_ascii_strcasecmp(base != nullptr ? base + 1 : exe, app_id) == 0)
                    by_exec = d;
            }
        }
        if (info == nullptr && by_exec != nullptr)
            info = g_object_ref(by_exec);
        g_list_free_full(all, g_object_unref);
    }
    if (info == nullptr) {
        const char *dot = strrchr(app_id, '.');
        if (dot != nullptr && dot[1] != '\0') {
            char *last = g_ascii_strdown(dot + 1, -1);
            info = desktop_by_id(last);
            g_free(last);
        }
    }
    if (info == nullptr)
        return false;
    *icon = desktop_icon(info);
    *name = g_strdup(g_app_info_get_name(G_APP_INFO(info)));
    g_object_unref(info);
    return true;
}
