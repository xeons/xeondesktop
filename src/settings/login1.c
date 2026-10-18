/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "login1.h"

/* A call can wait for a polkit prompt elsewhere; ours are quick lookups. */
static constexpr int CALL_TIMEOUT_MS = 5000;

bool xd_login1_simulated(void) {
    const char *nested = getenv("XEONDESKTOP_NESTED");
    bool real = nested != nullptr && strcmp(nested, "0") == 0;
    return !real && getenv("DBUS_SYSTEM_BUS_ADDRESS") == nullptr;
}

char *xd_login1_own_session(GDBusConnection *bus, GCancellable *cancel, GError **error) {
    const char *id = getenv("XDG_SESSION_ID");
    GVariant *r;
    if (id != nullptr && id[0] != '\0')
        r = g_dbus_connection_call_sync(bus, XD_LOGIN1, XD_LOGIN1_PATH, XD_LOGIN1_MANAGER,
                                        "GetSession", g_variant_new("(s)", id),
                                        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
                                        CALL_TIMEOUT_MS, cancel, error);
    else
        r = g_dbus_connection_call_sync(bus, XD_LOGIN1, XD_LOGIN1_PATH, XD_LOGIN1_MANAGER,
                                        "GetSessionByPID",
                                        g_variant_new("(u)", (guint32)getpid()),
                                        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
                                        CALL_TIMEOUT_MS, cancel, error);
    if (r == nullptr)
        return nullptr;
    char *path = nullptr;
    g_variant_get(r, "(o)", &path);
    g_variant_unref(r);
    return path;
}

char *xd_login1_session_string(GDBusConnection *bus, const char *path, const char *property,
                               GCancellable *cancel, GError **error) {
    GVariant *r = g_dbus_connection_call_sync(
        bus, XD_LOGIN1, path, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", XD_LOGIN1_SESSION, property), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, cancel, error);
    if (r == nullptr)
        return nullptr;
    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    char *s = g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string(v, nullptr)
                                                             : nullptr;
    if (s == nullptr)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is not a string", property);
    g_variant_unref(v);
    g_variant_unref(r);
    return s;
}
