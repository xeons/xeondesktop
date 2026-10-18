/* SPDX-License-Identifier: MIT
 * Installed icon and GTK themes, for Display Properties' theme choices,
 * and the places other desktops' settings tools (lxappearance, nwg-look,
 * GNOME Tweaks) keep the chosen themes, so that GTK programs started any
 * way agree with ours.
 */
#ifndef XD_SETTINGS_THEMES_H
#define XD_SETTINGS_THEMES_H

#include <gio/gio.h>

typedef struct xd_installed_theme {
    char *dir;  /* directory name: what [Appearance] stores */
    char *name; /* what the list shows */
} xd_installed_theme;

/* Icon themes' search path, in GTK's order: $XDG_DATA_HOME/icons,
 * ~/.icons, each $XDG_DATA_DIRS/icons. Free with g_strfreev. */
char **xd_icon_theme_dirs(void);
/* Icon themes in `dirs` (the first of each name wins), sorted by name:
 * directories whose index.theme has icon directories other than cursors,
 * and isn't Hidden (but for Adwaita, GTK's own default). Named by their
 * (translated) Name. A GPtrArray of xd_installed_theme. */
GPtrArray *xd_icon_themes_list(const char *const *dirs);

/* GTK themes' search path, in GTK's order: $XDG_DATA_HOME/themes,
 * ~/.themes, each $XDG_DATA_DIRS/themes. Free with g_strfreev. */
char **xd_gtk_theme_dirs(void);
/* GTK3 themes in `dirs` (those with gtk-3.0/gtk.css) and GTK's built-in
 * Adwaita and HighContrast, sorted, named by directory. Our generated
 * theme is left out: it is the "matches the style" choice. */
GPtrArray *xd_gtk_themes_list(const char *const *dirs);

/* Qt's plugin directories: $QT_PLUGIN_PATH's, then Qt 6's and Qt 5's as
 * qtpaths6/qmake6 and qmake-qt5 report them, else where distributions
 * install them. Only those that exist. Free with g_strfreev. */
char **xd_qt_plugin_dirs(void);
/* Qt widget styles for QtStyle, both name and dir being the key: Qt's
 * built-in Windows and Fusion, then the style plugins in `plugin_dirs`'
 * styles/, sorted. A plugin is named by its file (breeze6.so is
 * "Breeze"): Qt matches style keys without regard to case. */
GPtrArray *xd_qt_styles_list(const char *const *plugin_dirs);

/* Sets `key` in a GTK settings.ini's [Settings] (nullptr removes it),
 * keeping the rest of the file. */
bool xd_gtk_ini_set(const char *path, const char *key, const char *value);

/* After the user chose themes in Display Properties: points
 * $XDG_CONFIG_HOME/gtk-3.0 and gtk-4.0/settings.ini and, when its schema
 * is installed, org.gnome.desktop.interface at them. nullptr leaves that
 * one alone. `gtk_theme` is the full name (XD_GTK_THEME_NAME for ours, which
 * GTK4 can't use: its settings.ini loses the key instead). `backend`:
 * GSettings' (nullptr = the default, dconf). */
void xd_gtk_settings_export(const char *config_dir, GSettingsBackend *backend,
                            const char *gtk_theme, const char *icon_theme);

#endif
