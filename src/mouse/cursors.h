/* SPDX-License-Identifier: MIT
 * Installed XCursor themes, for Mouse Properties' Pointers tab: icon
 * theme directories with a cursors/ folder, where libXcursor looks.
 */
#ifndef XD_MOUSE_CURSORS_H
#define XD_MOUSE_CURSORS_H

#include <glib.h>

typedef struct xd_cursor_theme {
    char *dir;  /* directory name: what XCURSOR_THEME and [Mouse] use */
    char *name; /* index.theme's (translated) Name, else dir */
} xd_cursor_theme;

/* The search path, in libXcursor's order: $XDG_DATA_HOME/icons,
 * ~/.icons, each $XDG_DATA_DIRS/icons, /usr/share/pixmaps. Free with
 * g_strfreev. */
char **xd_cursor_theme_dirs(void);
/* Themes found in `dirs` (the first of each name wins), sorted by name.
 * A GPtrArray of xd_cursor_theme, freed with g_ptr_array_unref. */
GPtrArray *xd_cursor_themes_list(const char *const *dirs);

#endif
