/* SPDX-License-Identifier: MIT
 * Which application a window belongs to, from its app id: shared by the
 * shell's taskbar and the compositor's title bars.
 */
#include <gio/gdesktopappinfo.h>
#include <string.h>

#include "appid.h"

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
