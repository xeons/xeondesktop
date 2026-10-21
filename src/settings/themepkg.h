/* SPDX-License-Identifier: MIT
 * Installed theme packages (docs/THEMES.md) and the one the settings
 * choose: where they live, what Display Properties lists, and loading the
 * chosen one for the theme engine as the settings change.
 */
#ifndef XD_SETTINGS_THEMEPKG_H
#define XD_SETTINGS_THEMEPKG_H

#include "../theme/pkg.h"
#include "settings.h"

/* Where packages are looked for, first wins: $XDG_DATA_HOME, then each
 * $XDG_DATA_DIRS, each's xeondesktop/themes. Free with g_strfreev. */
char **xd_theme_package_dirs(void);

/* $XDG_DATA_HOME/xeondesktop/themes: where Display Properties installs
 * the packages it imports. Free with g_free. */
char *xd_theme_package_user_dir(void);

/* The package called `name` (ThemePackage) in `dirs`: the folder
 * DIR/NAME holding a theme.xml, else DIR/NAME.xdtheme, in the first
 * directory that has either (a folder wins over a zip of the same name,
 * for authoring). nullptr if there's none or `name` isn't a plain name.
 * Free with g_free. */
char *xd_theme_package_find(const char *const *dirs, const char *name);

typedef struct xd_installed_package {
    char *name;    /* what ThemePackage stores */
    char *path;    /* the folder or the .xdtheme */
    xd_pkg_info info; /* from its theme.xml: display name, base, variants */
} xd_installed_package;

/* The packages in `dirs`, as xd_theme_package_find would pick them (one
 * per name), sorted by display name: theme.xml's name, else the package's.
 * Reads only theme.xml; one that can't be read isn't listed. A GPtrArray
 * of xd_installed_package. */
GPtrArray *xd_theme_packages_list(const char *const *dirs);

/* uxtheme's names (data/theme/uxtheme.ini, from xd_data_dir). Free with
 * g_free. */
char *xd_theme_names_path(void);

/* ImageScaling as the theme engine takes it: the user's filter, or for
 * Automatic the package's own (`pkg` may be nullptr), else Smooth. */
xd_image_filter xd_settings_image_filter(const xd_settings *s, const xd_pkg *pkg);

/* Makes the package `s` chooses the active one (xd_theme_use_package):
 * its ThemePackage and ThemeVariant (none while High Contrast is on, as
 * Windows turns visual styles off for it), or, as an override for
 * testing, $XD_THEME_PACKAGE (a path), $XD_THEME_VARIANT and
 * $XD_IMAGE_FILTER. The same package as before (not rewritten since) is
 * kept; a changed one is loaded anew. Returns the package it replaced, for
 * the caller to xd_pkg_free once it has redrawn whatever was drawn with
 * it (and holds no theme initialized before this call); nullptr if none
 * was. A package that fails to load is reported on stderr and none is
 * used. */
[[nodiscard]] xd_pkg *xd_settings_use_package(const xd_settings *s);

/* The active package xd_settings_use_package loaded (nullptr: none). */
const xd_pkg *xd_settings_active_package(void);

#endif
