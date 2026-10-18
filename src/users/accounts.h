/* SPDX-License-Identifier: MIT
 * AccountsService (org.freedesktop.Accounts) calls and logon facts shared
 * by User Accounts (xd-users) and System Properties' User Profiles
 * (xd-sysdm).
 */
#ifndef XD_USERS_ACCOUNTS_H
#define XD_USERS_ACCOUNTS_H

#include <gio/gio.h>

#define XD_ACCOUNTS_NAME "org.freedesktop.Accounts"
#define XD_ACCOUNTS_PATH "/org/freedesktop/Accounts"
#define XD_ACCOUNTS_USER_IFACE "org.freedesktop.Accounts.User"

/* Deletes the account `uid` (and its home folder if `remove_files`)
 * without waiting: AccountsService asks polkit, whose password prompt may
 * be up for a while. Finish with g_dbus_connection_call_finish(). */
void xd_accounts_delete_user(GDBusConnection *system_bus, gint64 uid, bool remove_files,
                             GCancellable *cancellable, GAsyncReadyCallback done, gpointer data);

#endif
