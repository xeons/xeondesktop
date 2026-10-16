/* SPDX-License-Identifier: MIT
 * Installed applications, through GIO's GDesktopAppInfo (the reference
 * implementation of the freedesktop desktop entry spec: localization,
 * OnlyShowIn/NotShowIn via $XDG_CURRENT_DESKTOP, TryExec, Exec field codes
 * and launching).
 */
#ifndef XD_SHELL_APPS_H
#define XD_SHELL_APPS_H

#include "../core/str.h"

typedef struct _GDesktopAppInfo GDesktopAppInfo;

typedef struct xd_app {
    const char *id;     /* desktop file id, e.g. "firefox.desktop" */
    const char *name;   /* localized */
    const char *icon;   /* icon name or absolute path; may be nullptr */
    const char *folder; /* Programs folder, e.g. "Accessories" */
    const char *folder_icon;
    const char *action; /* X-XeonDesktop-Action, e.g. "show-desktop"; may be nullptr */
    const char *keywords; /* localized Keywords joined with spaces, for search */
    bool visible;         /* g_app_info_should_show */
    GDesktopAppInfo *info; /* owned reference */
} xd_app;

typedef struct xd_app_list {
    xd_arena arena; /* owns every string */
    xd_app *items;
    size_t count, cap;
} xd_app_list;

/* All installed applications that should be shown, sorted by folder then
 * name. */
void xd_apps_load(xd_app_list *list);
void xd_apps_free(xd_app_list *list);

/* Loads one .desktop file (Quick Launch shortcuts). Strings go in `arena`;
 * release with xd_app_release. */
[[nodiscard]] bool xd_app_from_file(const char *path, xd_arena *arena, xd_app *out);
void xd_app_release(xd_app *app);

/* The Icon key (theme name or path) of the application a window belongs
 * to, found from its app id (Wayland) or WM_CLASS (X11): the desktop file
 * of that name, else one whose StartupWMClass or id matches regardless of
 * case, else the reverse-DNS id's last part ("org.gnome.Nautilus" ->
 * "nautilus"). nullptr if none; g_free the result. */
char *xd_app_icon_for_window(const char *app_id);

/* Launches the application detached from the shell (own session, default
 * signal handling). Returns false on failure. */
bool xd_app_launch(const xd_app *app);

#endif
