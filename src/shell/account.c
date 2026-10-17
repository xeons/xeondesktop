/* SPDX-License-Identifier: MIT
 * The user's account as the Start menus show it: the full name in the XP
 * menu's header, and Switch User.
 *
 * Full name: AccountsService's RealName (what User Accounts sets), else
 * the GECOS field's first part, else the login name. AccountsService is
 * asked again each time a Start menu opens, so a renamed account shows
 * without restarting the shell.
 *
 * Switch User goes to the display manager's greeter while this session
 * keeps running, as XP's Fast User Switching did: LightDM and SDDM offer
 * org.freedesktop.DisplayManager's Seat.SwitchToGreeter, GDM its
 * LocalDisplayFactory.CreateTransientDisplay. Without either it stays
 * greyed out. The screen is locked first (GNOME and KDE do the same): the
 * session stays on its own VT, and switching back to that by hand must
 * not skip the password. In a nested or test session nothing is asked of
 * the system bus unless DBUS_SYSTEM_BUS_ADDRESS points at the tester's
 * own (a mock display manager), as for sleep.
 */
#include <gio/gio.h>
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

#include "shell.h"

#define ACCOUNTS "org.freedesktop.Accounts"
#define ACCOUNTS_PATH "/org/freedesktop/Accounts"
#define ACCOUNTS_USER "org.freedesktop.Accounts.User"
#define FDO_DM "org.freedesktop.DisplayManager"
#define FDO_DM_PATH "/org/freedesktop/DisplayManager"
#define FDO_DM_SEAT "org.freedesktop.DisplayManager.Seat"
#define GDM "org.gnome.DisplayManager"
#define GDM_FACTORY_PATH "/org/gnome/DisplayManager/LocalDisplayFactory"
#define GDM_FACTORY "org.gnome.DisplayManager.LocalDisplayFactory"

/* How long the locker gets to cover the screens before the greeter. */
static constexpr guint LOCK_GRACE_MS = 1000;

struct xd_account {
    xd_shell *shell;
    GCancellable *cancel;
    GDBusConnection *bus;
    guint watch_fdo, watch_gdm;
    bool have_fdo, have_gdm; /* the display manager's name has an owner */
    char *login;             /* login name */
    char *gecos;             /* GECOS full name, or nullptr */
    char *real_name;         /* AccountsService's RealName, or nullptr */
    guint switch_timer;
};

static bool simulated(void) {
    const char *nested = getenv("XEONDESKTOP_NESTED");
    bool real = nested != nullptr && strcmp(nested, "0") == 0;
    return !real && getenv("DBUS_SYSTEM_BUS_ADDRESS") == nullptr;
}

static char *gecos_name(const char *gecos) {
    if (gecos == nullptr)
        return nullptr;
    /* "Full Name,Room,Work phone,Home phone,Other" */
    const char *comma = strchr(gecos, ',');
    char *name = g_strstrip(g_strndup(gecos, comma != nullptr ? (gsize)(comma - gecos)
                                                               : strlen(gecos)));
    if (name[0] == '\0' || !g_utf8_validate(name, -1, nullptr)) {
        g_free(name);
        return nullptr;
    }
    return name;
}

const char *xd_account_full_name(xd_shell *shell) {
    static const char *const fallback = "User";
    xd_account *a = shell->account;
    if (a == nullptr) {
        const char *user = getenv("USER");
        return user != nullptr ? user : fallback;
    }
    if (a->real_name != nullptr && a->real_name[0] != '\0')
        return a->real_name;
    if (a->gecos != nullptr)
        return a->gecos;
    return a->login;
}

/* ---- AccountsService's RealName ---- */

static void got_real_name(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        g_error_free(error);
        return;
    }
    xd_account *a = data;
    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
        const char *name = g_variant_get_string(v, nullptr);
        if (g_strcmp0(name, a->real_name) != 0) {
            g_free(a->real_name);
            a->real_name = g_strdup(name);
            xd_start7_user_changed(a->shell);
        }
    }
    g_variant_unref(v);
    g_variant_unref(r);
}

static void found_user(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        /* No AccountsService: GECOS it is. */
        g_error_free(error);
        return;
    }
    xd_account *a = data;
    const char *path = nullptr;
    g_variant_get(r, "(&o)", &path);
    g_dbus_connection_call(a->bus, ACCOUNTS, path, "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", ACCOUNTS_USER, "RealName"), G_VARIANT_TYPE("(v)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, a->cancel, got_real_name, a);
    g_variant_unref(r);
}

void xd_account_refresh(xd_shell *shell) {
    xd_account *a = shell->account;
    if (a == nullptr || a->bus == nullptr)
        return;
    /* NO_AUTO_START: a menu opening shouldn't start a system service. */
    g_dbus_connection_call(a->bus, ACCOUNTS, ACCOUNTS_PATH, ACCOUNTS, "FindUserById",
                           g_variant_new("(x)", (gint64)getuid()), G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, a->cancel, found_user, a);
}

/* ---- Switch User ---- */

static void dm_appeared([[maybe_unused]] GDBusConnection *bus, const char *name,
                        [[maybe_unused]] const char *owner, gpointer data) {
    xd_account *a = data;
    if (strcmp(name, FDO_DM) == 0)
        a->have_fdo = true;
    else
        a->have_gdm = true;
}

static void dm_vanished([[maybe_unused]] GDBusConnection *bus, const char *name, gpointer data) {
    xd_account *a = data;
    if (strcmp(name, FDO_DM) == 0)
        a->have_fdo = false;
    else
        a->have_gdm = false;
}

bool xd_account_can_switch_user(xd_shell *shell) {
    const xd_account *a = shell->account;
    return a != nullptr && (a->have_fdo || a->have_gdm);
}

static void switched(GObject *src, GAsyncResult *res, [[maybe_unused]] gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r != nullptr) {
        g_variant_unref(r);
        return;
    }
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fprintf(stderr, "xd-shell: cannot switch user: %s\n", error->message);
    g_error_free(error);
}

static void switch_to_greeter(xd_account *a, const char *seat) {
    g_dbus_connection_call(a->bus, FDO_DM, seat, FDO_DM_SEAT, "SwitchToGreeter", nullptr, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, a->cancel, switched, a);
}

/* Without XDG_SEAT_PATH (set by LightDM and SDDM in the session): the
 * display manager's first seat. */
static void got_seats(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fprintf(stderr, "xd-shell: cannot switch user: %s\n", error->message);
        g_error_free(error);
        return;
    }
    xd_account *a = data;
    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH_ARRAY) && g_variant_n_children(v) > 0) {
        const char *seat = nullptr;
        g_variant_get_child(v, 0, "&o", &seat);
        switch_to_greeter(a, seat);
    } else {
        fprintf(stderr, "xd-shell: cannot switch user: the display manager has no seats\n");
    }
    g_variant_unref(v);
    g_variant_unref(r);
}

static gboolean switch_now(gpointer data) {
    xd_account *a = data;
    a->switch_timer = 0;
    if (a->bus == nullptr)
        return G_SOURCE_REMOVE;
    if (a->have_fdo) {
        const char *seat = getenv("XDG_SEAT_PATH");
        if (seat != nullptr && g_variant_is_object_path(seat))
            switch_to_greeter(a, seat);
        else
            g_dbus_connection_call(a->bus, FDO_DM, FDO_DM_PATH, "org.freedesktop.DBus.Properties",
                                   "Get", g_variant_new("(ss)", FDO_DM, "Seats"),
                                   G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, a->cancel,
                                   got_seats, a);
    } else if (a->have_gdm) {
        g_dbus_connection_call(a->bus, GDM, GDM_FACTORY_PATH, GDM_FACTORY,
                               "CreateTransientDisplay", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE,
                               -1, a->cancel, switched, a);
    }
    return G_SOURCE_REMOVE;
}

void xd_account_switch_user(xd_shell *shell) {
    xd_account *a = shell->account;
    if (!xd_account_can_switch_user(shell) || a->switch_timer != 0)
        return;
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-lock", nullptr));
    a->switch_timer = g_timeout_add(LOCK_GRACE_MS, switch_now, a);
}

/* ---- setup ---- */

static void got_bus([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GDBusConnection *bus = g_bus_get_finish(res, &error);
    if (bus == nullptr) {
        /* sleep.c says so already. */
        g_error_free(error);
        return;
    }
    xd_account *a = data;
    a->bus = bus;
    a->watch_fdo = g_bus_watch_name_on_connection(bus, FDO_DM, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                  dm_appeared, dm_vanished, a, nullptr);
    a->watch_gdm = g_bus_watch_name_on_connection(bus, GDM, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                  dm_appeared, dm_vanished, a, nullptr);
    xd_account_refresh(a->shell);
}

void xd_account_init(xd_shell *shell) {
    xd_account *a = g_new0(xd_account, 1);
    a->shell = shell;
    struct passwd *pw = getpwuid(getuid());
    const char *user = getenv("USER");
    a->login = g_strdup(pw != nullptr ? pw->pw_name : user != nullptr ? user : "User");
    a->gecos = pw != nullptr ? gecos_name(pw->pw_gecos) : nullptr;
    a->cancel = g_cancellable_new();
    shell->account = a;
    if (!simulated())
        g_bus_get(G_BUS_TYPE_SYSTEM, a->cancel, got_bus, a);
}

void xd_account_finish(xd_shell *shell) {
    xd_account *a = shell->account;
    if (a == nullptr)
        return;
    shell->account = nullptr;
    g_cancellable_cancel(a->cancel);
    g_object_unref(a->cancel);
    if (a->switch_timer != 0)
        g_source_remove(a->switch_timer);
    if (a->watch_fdo != 0)
        g_bus_unwatch_name(a->watch_fdo);
    if (a->watch_gdm != 0)
        g_bus_unwatch_name(a->watch_gdm);
    if (a->bus != nullptr)
        g_object_unref(a->bus);
    g_free(a->login);
    g_free(a->gecos);
    g_free(a->real_name);
    g_free(a);
}
