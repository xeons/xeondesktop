/* SPDX-License-Identifier: MIT
 * Control Panel applets. See control.h.
 */
#include "control.h"

#include <string.h>

const XdControlCategory xd_control_categories[] = {
    {"appearance", "Appearance and Themes", "preferences-desktop-theme", "preferences-desktop"},
    {"network", "Network and Internet Connections",
     "preferences-system-network", "network-workgroup"},
    {"programs", "Add or Remove Programs", "system-software-install", "applications-other"},
    {"sound", "Sounds, Speech, and Audio Devices", "preferences-desktop-sound", "audio-card"},
    {"performance", "Performance and Maintenance",
     "preferences-system-performance", "utilities-system-monitor"},
    {"hardware", "Printers and Other Hardware", "preferences-devices-printer", "printer"},
    {"accounts", "User Accounts", "preferences-system-users", "system-users"},
    {"datetime", "Date, Time, Language, and Regional Options",
     "preferences-desktop-locale", "preferences-system-time"},
    {"accessibility", "Accessibility Options", "preferences-desktop-accessibility",
     "preferences-desktop"},
    {XD_CONTROL_OTHER, "Other Control Panel Options", "preferences-other", "preferences-system"},
};
const size_t xd_control_category_count = G_N_ELEMENTS(xd_control_categories);

const XdControlCategory *xd_control_category(const char *key) {
    for (size_t i = 0; key != nullptr && i < xd_control_category_count; i++)
        if (g_ascii_strcasecmp(xd_control_categories[i].key, key) == 0)
            return &xd_control_categories[i];
    return nullptr;
}

GIcon *xd_control_category_icon(const XdControlCategory *c) {
    GIcon *icon = g_themed_icon_new(c->icon);
    g_themed_icon_append_name(G_THEMED_ICON(icon), c->fallback);
    return icon;
}

gboolean xd_control_is_applet(GDesktopAppInfo *info) {
    const char *categories = g_desktop_app_info_get_categories(info);
    if (categories == nullptr || g_desktop_app_info_get_is_hidden(info) ||
        !g_desktop_app_info_get_show_in(info, nullptr))
        return FALSE;
    char **list = g_strsplit(categories, ";", -1);
    gboolean found = g_strv_contains((const char *const *)list, XD_CONTROL_CATEGORY);
    g_strfreev(list);
    return found;
}

static int by_name(gconstpointer a, gconstpointer b) {
    return g_utf8_collate(g_app_info_get_name(G_APP_INFO(a)), g_app_info_get_name(G_APP_INFO(b)));
}

GList *xd_control_applets(void) {
    /* Every installed entry, NoDisplay ones included; user entries
     * override system ones. */
    GList *all = g_app_info_get_all();
    GList *applets = nullptr;
    for (GList *l = all; l != nullptr; l = l->next)
        if (G_IS_DESKTOP_APP_INFO(l->data) && xd_control_is_applet(l->data))
            applets = g_list_prepend(applets, g_object_ref(l->data));
    g_list_free_full(all, g_object_unref);
    return g_list_sort(applets, by_name);
}

gboolean xd_control_applet_in(GDesktopAppInfo *info, const char *key) {
    char **keys = g_desktop_app_info_get_string_list(info, XD_CONTROL_CATEGORY_KEY, nullptr);
    gboolean known = FALSE;
    gboolean in = FALSE;
    for (char **k = keys; k != nullptr && *k != nullptr; k++) {
        if (xd_control_category(*k) == nullptr)
            continue;
        known = TRUE;
        in = in || g_ascii_strcasecmp(*k, key) == 0;
    }
    g_strfreev(keys);
    return known ? in : g_ascii_strcasecmp(key, XD_CONTROL_OTHER) == 0;
}

/* "xd-desk" for Exec=xd-desk or Exec=/usr/bin/xd-desk --flag. */
static char *program_name(GDesktopAppInfo *info) {
    const char *exe = g_app_info_get_executable(G_APP_INFO(info));
    return exe != nullptr ? g_path_get_basename(exe) : nullptr;
}

GDesktopAppInfo *xd_control_find(const char *name) {
    char *want = g_ascii_strdown(name, -1);
    if (g_str_has_suffix(want, ".cpl"))
        want[strlen(want) - strlen(".cpl")] = '\0';
    GList *applets = xd_control_applets();
    GDesktopAppInfo *found = nullptr;
    for (GList *l = applets; l != nullptr && found == nullptr; l = l->next) {
        GDesktopAppInfo *info = l->data;
        char *prog = program_name(info);
        const char *id = g_app_info_get_id(G_APP_INFO(info));
        char *stem = id != nullptr ? g_strndup(id, strlen(id) - (g_str_has_suffix(id, ".desktop")
                                                                     ? strlen(".desktop")
                                                                     : 0))
                                   : nullptr;
        if ((prog != nullptr && (g_ascii_strcasecmp(prog, want) == 0 ||
                                 (g_str_has_prefix(prog, "xd-") &&
                                  g_ascii_strcasecmp(prog + 3, want) == 0))) ||
            (stem != nullptr && g_ascii_strcasecmp(stem, want) == 0) ||
            g_ascii_strcasecmp(g_app_info_get_name(G_APP_INFO(info)), want) == 0)
            found = g_object_ref(info);
        g_free(stem);
        g_free(prog);
    }
    g_list_free_full(applets, g_object_unref);
    g_free(want);
    return found;
}
