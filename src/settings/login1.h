/* SPDX-License-Identifier: MIT
 * logind (org.freedesktop.login1) for the programs that deal with the
 * user's session: xd-lock answers its Lock and Unlock, xd-shutdown ends
 * the session through it when our compositor isn't the session's, and the
 * Task Manager's Users tab lists and logs off sessions. Not part of
 * libxdsettings: only those programs link GIO for it.
 *
 * In a nested or test session nothing that changes state is asked of the
 * system's logind unless DBUS_SYSTEM_BUS_ADDRESS points at the tester's
 * own (a mock), as for the shell's sleep and Switch User.
 */
#ifndef XD_SETTINGS_LOGIN1_H
#define XD_SETTINGS_LOGIN1_H

#include <gio/gio.h>

#define XD_LOGIN1 "org.freedesktop.login1"
#define XD_LOGIN1_PATH "/org/freedesktop/login1"
#define XD_LOGIN1_MANAGER "org.freedesktop.login1.Manager"
#define XD_LOGIN1_SESSION "org.freedesktop.login1.Session"

/* True in a nested or test session without a mock system bus: session
 * changes are only printed. A real session has XEONDESKTOP_NESTED=0. */
bool xd_login1_simulated(void);

/* The object path of the caller's own session: XDG_SESSION_ID's (set by
 * pam_systemd), else the one logind finds for our pid. Blocks for one or
 * two calls; g_free the result. */
char *xd_login1_own_session(GDBusConnection *bus, GCancellable *cancel, GError **error);

/* A string property of a session object ("Id", "Name"...), or nullptr. */
char *xd_login1_session_string(GDBusConnection *bus, const char *path, const char *property,
                               GCancellable *cancel, GError **error);

#endif
