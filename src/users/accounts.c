/* SPDX-License-Identifier: MIT */
#include "accounts.h"

void xd_accounts_delete_user(GDBusConnection *system_bus, gint64 uid, bool remove_files,
                             GCancellable *cancellable, GAsyncReadyCallback done, gpointer data) {
    g_dbus_connection_call(system_bus, XD_ACCOUNTS_NAME, XD_ACCOUNTS_PATH, XD_ACCOUNTS_NAME,
                           "DeleteUser", g_variant_new("(xb)", uid, remove_files), nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
                           cancellable, done, data);
}
