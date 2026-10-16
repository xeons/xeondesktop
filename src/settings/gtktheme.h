/* SPDX-License-Identifier: MIT
 * Installs the generated GTK3 theme ("XeonDesktop") into
 * $XDG_DATA_HOME/themes, where GTK finds it via GTK_THEME=XeonDesktop.
 */
#ifndef XD_SETTINGS_GTKTHEME_H
#define XD_SETTINGS_GTKTHEME_H

#include "../theme/theme.h"

#define XD_GTK_THEME_NAME "XeonDesktop"

/* Writes gtk-3.0/gtk.css and index.theme atomically. Returns false and
 * describes the problem in *err on failure. */
[[nodiscard]] bool xd_gtk_theme_install(xd_visual_style style, const xd_scheme *scheme,
                                        xd_strbuf *err);
/* Path of the installed gtk.css (whether or not it exists yet). */
void xd_gtk_theme_css_path(xd_strbuf *out);

#endif
