/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "../settings/gtktheme.h"
#include "gtkapp.h"

void xd_gtk_app_preinit(void) {
    /* GTK_THEME pins the theme for the life of the process; our own apps
     * set it through GtkSettings instead so they can reload it. */
    g_unsetenv("GTK_THEME");
}

static void reload_theme(void) {
    GtkSettings *s = gtk_settings_get_default();
    if (s == nullptr)
        return;
    /* GTK re-reads a theme's CSS when the theme name changes. */
    g_object_set(s, "gtk-theme-name", "Adwaita", nullptr);
    g_object_set(s, "gtk-theme-name", XD_GTK_THEME_NAME, nullptr);
}

static void on_css_changed([[maybe_unused]] GFileMonitor *m, [[maybe_unused]] GFile *f,
                           [[maybe_unused]] GFile *other, GFileMonitorEvent ev,
                           [[maybe_unused]] gpointer data) {
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_CREATED ||
        ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN)
        reload_theme();
}

static gboolean theme_installed(const char *name) {
    GtkIconTheme *t = gtk_icon_theme_get_default();
    gchar **paths = nullptr;
    gint n = 0;
    gtk_icon_theme_get_search_path(t, &paths, &n);
    gboolean found = FALSE;
    for (gint i = 0; i < n && !found; i++) {
        char *index = g_build_filename(paths[i], name, "index.theme", nullptr);
        found = g_file_test(index, G_FILE_TEST_IS_REGULAR);
        g_free(index);
    }
    g_strfreev(paths);
    return found;
}

void xd_gtk_app_follow_theme(void) {
    GtkSettings *s = gtk_settings_get_default();
    if (s == nullptr)
        return;
    xd_strbuf path = {0};
    xd_gtk_theme_css_path(&path);
    if (g_file_test(xd_strbuf_cstr(&path), G_FILE_TEST_IS_REGULAR))
        g_object_set(s, "gtk-theme-name", XD_GTK_THEME_NAME, nullptr);

    /* Same icon theme as the shell: XD_ICON_THEME, else breeze, else
     * GTK's default. */
    const char *icons = getenv("XD_ICON_THEME");
    if (icons == nullptr || !theme_installed(icons))
        icons = theme_installed("breeze") ? "breeze" : nullptr;
    if (icons != nullptr)
        g_object_set(s, "gtk-icon-theme-name", icons, nullptr);

    /* Watch the directory: the theme is replaced by rename. The monitor
     * lives as long as the process. */
    GFile *css = g_file_new_for_path(xd_strbuf_cstr(&path));
    GFileMonitor *mon = g_file_monitor_file(css, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (mon != nullptr)
        g_signal_connect(mon, "changed", G_CALLBACK(on_css_changed), nullptr);
    g_object_unref(css);
    xd_strbuf_free(&path);
}
