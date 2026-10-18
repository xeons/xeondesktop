/* SPDX-License-Identifier: MIT
 * Installs the generated GTK3 theme ("XeonDesktop") into
 * $XDG_DATA_HOME/themes, where GTK finds it via GTK_THEME=XeonDesktop.
 */
#ifndef XD_SETTINGS_GTKTHEME_H
#define XD_SETTINGS_GTKTHEME_H

#include <gio/gio.h>

#include "../theme/theme.h"

#define XD_GTK_THEME_NAME "XeonDesktop"
/* The same theme under a second name, for xd_gtk_theme_nudge. */
#define XD_GTK_THEME_ALT_NAME "XeonDesktop-alt"

/* Writes gtk-3.0/gtk.css and settings.ini, index.theme, the Qt palette
 * and the second name's theme atomically. *changed (if not nullptr): the
 * CSS differs from what was there. Returns false and describes the
 * problem in *err on failure. */
[[nodiscard]] bool xd_gtk_theme_install(xd_visual_style style, const xd_scheme *scheme,
                                        bool *changed, xd_strbuf *err);
/* Path of the installed gtk.css (whether or not it exists yet). */
void xd_gtk_theme_css_path(xd_strbuf *out);
/* Path of the Qt palette written with it (see qtpalette.h). g_free it. */
char *xd_gtk_theme_qt_palette_path(void);

/* GTK 3 programs started with GTK_THEME read the theme once, and again
 * only when gtk-theme-name changes, which on Wayland they follow from
 * org.gnome.desktop.interface gtk-theme (setting the same name again does
 * nothing). After a rewrite this switches that key between our two names
 * so that they reload it, unless another theme is set there. False if
 * nothing was written (no schema, another theme). `backend`: GSettings'
 * (nullptr = the default, dconf). */
bool xd_gtk_theme_nudge(GSettingsBackend *backend);

#endif
