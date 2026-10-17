/* SPDX-License-Identifier: MIT
 * XP's font viewer: one font's name, details and samples at 12 to 72
 * points, drawn from its own file.
 */
#ifndef XD_FONTS_VIEWER_H
#define XD_FONTS_VIEWER_H

#include <gtk/gtk.h>

/* Opens a viewer window for face `index` of `file`; nullptr (after
 * telling the user) when it isn't a font fontconfig can read. */
GtkWidget *xd_font_viewer_new(GtkWindow *parent, const char *file, int index);

#endif
