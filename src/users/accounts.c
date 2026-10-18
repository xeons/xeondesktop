/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "accounts.h"

void xd_accounts_delete_user(GDBusConnection *system_bus, gint64 uid, bool remove_files,
                             GCancellable *cancellable, GAsyncReadyCallback done, gpointer data) {
    g_dbus_connection_call(system_bus, XD_ACCOUNTS_NAME, XD_ACCOUNTS_PATH, XD_ACCOUNTS_NAME,
                           "DeleteUser", g_variant_new("(xb)", uid, remove_files), nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
                           cancellable, done, data);
}

char *xd_accounts_display_manager_unit(GDBusConnection *system_bus) {
    if (system_bus == nullptr)
        return nullptr;
    /* GetUnit only finds a loaded unit, so a display manager installed
     * but not in use (a session started from a console) isn't named. */
    GVariant *r = g_dbus_connection_call_sync(
        system_bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "GetUnit",
        g_variant_new("(s)", "display-manager.service"), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr);
    if (r == nullptr)
        return nullptr;
    const char *path = nullptr;
    g_variant_get(r, "(&o)", &path);
    GVariant *id = g_dbus_connection_call_sync(
        system_bus, "org.freedesktop.systemd1", path, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.systemd1.Unit", "Id"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr);
    g_variant_unref(r);
    if (id == nullptr)
        return nullptr;
    GVariant *v = nullptr;
    g_variant_get(id, "(v)", &v);
    char *unit = g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string(v, nullptr)
                                                                : nullptr;
    g_variant_unref(v);
    g_variant_unref(id);
    /* The alias itself means systemd couldn't say what it stands for. */
    if (unit != nullptr && (unit[0] == '\0' || strcmp(unit, "display-manager.service") == 0))
        g_clear_pointer(&unit, g_free);
    return unit;
}

/* Display managers by unit name, as their projects write them. */
static const struct {
    const char *unit, *name;
} display_managers[] = {
    {"gdm", "GDM"},
    {"gdm3", "GDM"},
    {"sddm", "SDDM"},
    {"lightdm", "LightDM"},
    {"lxdm", "LXDM"},
    {"ly", "Ly"},
    {"greetd", "greetd"},
    {"xdm", "XDM"},
    {"slim", "SLiM"},
    {"entrance", "Entrance"},
    {"cosmic-greeter", "COSMIC Greeter"},
    {"plasmalogin", "Plasma Login"},
};

char *xd_accounts_autologin_note(const char *unit) {
    static const char general[] =
        "Automatic logon takes effect only with a logon screen (display manager) that reads "
        "AccountsService's setting, as GDM does. SDDM and LightDM keep their own setting, which "
        "an administrator changes in their configuration files.";
    if (unit == nullptr || unit[0] == '\0')
        return g_strdup(general);
    char *stem = g_strdup(unit);
    char *dot = strrchr(stem, '.');
    if (dot != nullptr && strcmp(dot, ".service") == 0)
        *dot = '\0';
    const char *name = nullptr;
    for (size_t i = 0; i < G_N_ELEMENTS(display_managers) && name == nullptr; i++) {
        if (strcmp(stem, display_managers[i].unit) == 0)
            name = display_managers[i].name;
    }
    char *note;
    if (name != nullptr && strcmp(name, "GDM") == 0)
        note = g_strdup("This computer's logon screen is GDM, which follows this setting.");
    else if (name != nullptr)
        note = g_strdup_printf("This computer's logon screen is %s, which keeps its own automatic "
                               "logon setting: this one has no effect until an administrator "
                               "sets it in %s's configuration too.",
                               name, name);
    else
        note = g_strdup_printf("This computer's logon screen is %s. %s", stem, general);
    g_free(stem);
    return note;
}
