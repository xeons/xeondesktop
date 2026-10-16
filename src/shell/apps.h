/* SPDX-License-Identifier: MIT
 * Installed applications, from freedesktop .desktop entries.
 */
#ifndef XD_SHELL_APPS_H
#define XD_SHELL_APPS_H

#include "../core/str.h"

typedef struct xd_app {
    const char *id;     /* desktop file id, e.g. "firefox.desktop" */
    const char *name;   /* localized */
    const char *icon;   /* icon name or absolute path; may be nullptr */
    const char *exec;   /* command line with field codes removed */
    const char *folder; /* Programs folder, e.g. "Accessories" */
    const char *folder_icon;
    const char *action; /* X-XeonDesktop-Action, e.g. "show-desktop"; may be nullptr */
    bool terminal;
} xd_app;

typedef struct xd_app_list {
    xd_arena arena; /* owns every string */
    xd_app *items;
    size_t count, cap;
} xd_app_list;

/* Scans $XDG_DATA_HOME and $XDG_DATA_DIRS. Entries are sorted by folder,
 * then by name. */
void xd_apps_load(xd_app_list *list, const char *desktop_name);
void xd_apps_free(xd_app_list *list);

/* Exposed for tests: parse one entry. Returns false if it should not be
 * shown. Strings are allocated from `arena`. */
[[nodiscard]] bool xd_app_parse(xd_str text, xd_str id, const char *desktop_name,
                                xd_arena *arena, xd_app *out);

/* Strips/expands desktop entry field codes (%f %U %c %i ...). */
[[nodiscard]] char *xd_app_expand_exec(xd_arena *arena, xd_str exec, const char *name,
                                       const char *icon);

#endif
