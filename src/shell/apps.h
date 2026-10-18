/* SPDX-License-Identifier: MIT
 * Installed applications, through GIO's GDesktopAppInfo (the reference
 * implementation of the freedesktop desktop entry spec: localization,
 * OnlyShowIn/NotShowIn via $XDG_CURRENT_DESKTOP, TryExec, Exec field codes
 * and launching).
 */
#ifndef XD_SHELL_APPS_H
#define XD_SHELL_APPS_H

#include "../core/str.h"
#include "../ui/appid.h"

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
/* The Control Panel applets (X-XeonDesktop-ControlPanel, NoDisplay ones
 * included), sorted by name, for the Start menus' Control Panel. */
void xd_apps_load_control_panel(xd_app_list *list);

/* Loads one .desktop file (Quick Launch shortcuts). Strings go in `arena`;
 * release with xd_app_release. */
[[nodiscard]] bool xd_app_from_file(const char *path, xd_arena *arena, xd_app *out);
void xd_app_release(xd_app *app);

/* Launches the application detached from the shell (own session, default
 * signal handling). Returns false on failure. */
bool xd_app_launch(const xd_app *app);
/* As xd_app_launch, on graphics card `gpu` (an index into xd_gpu_current;
 * < 0: the program's own choice, as every other launch makes it). */
bool xd_app_launch_gpu(const xd_app *app, int gpu);
/* Launches a program with files (URIs, GList of char*; may be nullptr),
 * detached the same way. */
typedef struct _GList GList;
bool xd_app_launch_info(GDesktopAppInfo *info, GList *uris);
/* Runs one of the program's desktop actions (a jump list's Tasks), detached
 * the same way. */
bool xd_app_launch_action(GDesktopAppInfo *info, const char *action);

#endif
