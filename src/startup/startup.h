/* SPDX-License-Identifier: MIT
 * System Configuration Utility (xd-msconfig): what starts when you log
 * on, without any GTK, so it can be tested on its own.
 *
 * Startup programs are XDG autostart entries: the system's
 * (/etc/xdg/autostart, every $XDG_CONFIG_DIRS) and your own
 * (~/.config/autostart), where an entry of yours with the same file name
 * replaces the system's. systemd's xdg-autostart generator starts them in
 * the session. Turning off a system entry writes your own copy of it with
 * Hidden=true (the spec's way); turning it back on removes that copy.
 *
 * User services are your systemd user units that start with the session
 * (enabled into default.target or graphical-session.target), through the
 * user manager's D-Bus API on the session bus.
 */
#ifndef XD_STARTUP_STARTUP_H
#define XD_STARTUP_STARTUP_H

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

/* ---- Startup programs (XDG autostart) ---- */

typedef struct xd_autostart {
    char *file;     /* its file name, the entry's identity: "foo.desktop" */
    char *name;     /* Name, localized */
    char *command;  /* Exec */
    char *icon;     /* Icon as written, or nullptr */
    char *path;     /* the entry shown: the system's, or your own one */
    char *location; /* the folder of `path` */
    bool user_only; /* your own entry, no system one: Remove deletes it */
    bool enabled;   /* starts when you log on */
} xd_autostart;

void xd_autostart_free(xd_autostart *item);

/* The entries that apply to this desktop (`desktop`: one name for
 * OnlyShowIn/NotShowIn, or nullptr for $XDG_CURRENT_DESKTOP), by name.
 * Left out, as systemd's generator leaves them out: no such program
 * (TryExec), X-systemd-skip, GNOME's startup phases (gnome-session runs
 * those). `user_dir` is ~/.config/autostart, `system_dirs` the system's in
 * order of precedence. An array of xd_autostart *. */
GPtrArray *xd_autostart_list(const char *user_dir, const char *const *system_dirs,
                             const char *desktop);

/* Turns an entry on or off for the next log on: your own entry's Hidden
 * (and GNOME's X-GNOME-Autostart-enabled), or for a system entry your
 * copy of it with Hidden=true, which turning it on again removes when it
 * differs in nothing else. */
bool xd_autostart_set_enabled(const xd_autostart *item, const char *user_dir, bool enabled,
                              GError **error);

/* Adds an installed program (a copy of its desktop entry) or a command
 * line, as your own entry; returns its file name (g_free). */
char *xd_autostart_add_app(const char *user_dir, GDesktopAppInfo *app, GError **error);
char *xd_autostart_add_command(const char *user_dir, const char *command, GError **error);

/* Deletes your own entry (user_only). */
bool xd_autostart_remove(const xd_autostart *item, GError **error);

/* An argument list as a desktop entry's Exec value: quoted where the spec
 * reserves characters, '%' doubled. g_free. */
char *xd_exec_quote(const char *const *argv);

/* ---- User services (systemd user units) ---- */

typedef struct xd_user_service {
    char *unit;        /* "foo.service" */
    char *description; /* Description, or the unit's name */
    bool running;      /* active now */
    bool enabled;      /* starts with the session */
    bool global;       /* enabled for every user (/etc/systemd/user): not yours to change */
} xd_user_service;

void xd_user_service_free(xd_user_service *s);

/* Whether a unit enabled through these links starts with the session:
 * one is in default.target's, graphical-session.target's or
 * xeondesktop-session.target's .wants folder. *global: none of them is in
 * `user_unit_dir` (~/.config/systemd/user). */
bool xd_service_links_start_session(const char *const *links, const char *user_unit_dir,
                                    bool *global);

/* The user services that start with the session, and the ones turned
 * off here before (`remembered`, kept by the caller), by description. An
 * array of xd_user_service *; nullptr with `error` set when the user
 * manager can't be asked. */
GPtrArray *xd_user_services_list(GDBusConnection *bus, const char *const *remembered,
                                 GError **error);

/* Enables or disables a service for the next log on (and reloads the
 * manager). Disabling one enabled for every user fails, saying so. */
bool xd_user_service_set_enabled(GDBusConnection *bus, const char *unit, bool enabled,
                                 GError **error);

/* The services turned off here, which "Normal startup" and the list bring
 * back: [Services] Disabled in ~/.config/xeondesktop/startup.ini
 * (`path`). */
char **xd_startup_disabled_load(const char *path);
bool xd_startup_disabled_save(const char *path, const char *const *units, GError **error);

#endif
