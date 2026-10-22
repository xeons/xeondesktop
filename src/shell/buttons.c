/* SPDX-License-Identifier: MIT
 * Power Options' Power buttons: what closing the lid, the power button and
 * the sleep button do for this user ([Power] LidAction, PowerButtonAction,
 * SleepButtonAction), carried out as GNOME's and KDE's sessions do. For
 * each one set, the shell holds logind's "block" inhibitor for it
 * (handle-lid-switch, handle-power-key, handle-suspend-key and
 * handle-hibernate-key), so that logind leaves it alone, and the
 * compositor, which sees the lid switch and the keys, tells us of them
 * (xd-window-arranger-v1's power_button). One left unset stays logind's
 * (HandleLidSwitch...), so nothing changes for a user who never sets them.
 *
 * Here rather than in the compositor: the shell already stands by,
 * hibernates and shuts down through logind (sleep.c), starts the Shut Down
 * dialog and the locker; and should it die, its inhibitor goes with it, so
 * logind takes the buttons back instead of their doing nothing.
 *
 * Closing the lid does nothing while logind says the computer is docked or
 * has another monitor connected (its Docked property), as logind's own
 * HandleLidSwitchDocked=ignore.
 *
 * Only in a real session: not under another compositor, whose desktop
 * handles the buttons, nor in a nested or test session, unless its system
 * bus is the tester's own (a mock logind), as with sleep.c's actions.
 */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <stdlib.h>
#include <unistd.h>

#include "shell.h"

#define LOGIN1 "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MANAGER "org.freedesktop.login1.Manager"

/* Room for every button's: xd_power_buttons_inhibit_what. */
enum { WHAT_MAX = 128 };

struct xd_buttons {
    xd_shell *shell;
    GDBusConnection *bus;
    GCancellable *cancel;
    int fd;               /* our block inhibitor, -1 if none */
    char held[WHAT_MAX];  /* what it inhibits */
    char asked[WHAT_MAX]; /* what the Inhibit() in flight asks for */
    bool inhibiting;
};

/* logind's names in an inhibitor's "what", by button. */
static const char *const handles[XD_POWER_BUTTON_COUNT] = {
    [XD_BUTTON_LID] = "handle-lid-switch",
    [XD_BUTTON_POWER] = "handle-power-key",
    [XD_BUTTON_SLEEP] = "handle-suspend-key",
};

static bool allowed(const xd_shell *shell) {
    if (shell->standalone)
        return false;
    const char *nested = getenv("XEONDESKTOP_NESTED");
    if (nested != nullptr && strcmp(nested, "0") == 0)
        return true;
    /* A test session's own bus; never the machine's logind from a nested
     * session, whose buttons belong to the desktop it runs in. */
    const char *bus = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    return bus != nullptr && strstr(bus, "/run/dbus/system_bus_socket") == nullptr;
}

static void drop(xd_buttons *buttons) {
    if (buttons->fd >= 0)
        close(buttons->fd);
    buttons->fd = -1;
    buttons->held[0] = '\0';
}

static void request(xd_buttons *buttons);

static void inhibited(GObject *src, GAsyncResult *res, gpointer data) {
    GUnixFDList *fds = nullptr;
    GError *error = nullptr;
    GVariant *variant =
        g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(src), &fds, res, &error);
    if (variant == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error); /* the shell is exiting: b is gone */
        return;
    }
    xd_buttons *buttons = data;
    buttons->inhibiting = false;
    if (variant == nullptr) {
        /* logind keeps them: its own settings act. */
        fprintf(stderr, "xd-shell: cannot take over the power buttons: %s\n", error->message);
        g_error_free(error);
        return;
    }
    gint32 index = -1;
    g_variant_get(variant, "(h)", &index);
    g_variant_unref(variant);
    int fd = fds != nullptr ? g_unix_fd_list_get(fds, index, nullptr) : -1;
    g_clear_object(&fds);
    if (fd < 0)
        return;
    /* The new one first, then the old one goes: no moment without. */
    int old = buttons->fd;
    buttons->fd = fd;
    g_strlcpy(buttons->held, buttons->asked, sizeof(buttons->held));
    if (old >= 0)
        close(old);
    request(buttons); /* the settings may have changed meanwhile */
}

/* Holds an inhibitor for what the settings set, if it isn't held. */
static void request(xd_buttons *buttons) {
    if (buttons->bus == nullptr || buttons->inhibiting)
        return;
    char what[WHAT_MAX];
    xd_power_buttons_inhibit_what(&buttons->shell->settings, what, sizeof(what));
    if (strcmp(what, buttons->held) == 0)
        return;
    if (what[0] == '\0') {
        drop(buttons); /* all logind's again */
        return;
    }
    buttons->inhibiting = true;
    g_strlcpy(buttons->asked, what, sizeof(buttons->asked));
    g_dbus_connection_call_with_unix_fd_list(
        buttons->bus, LOGIN1, LOGIN1_PATH, LOGIN1_MANAGER, "Inhibit",
        g_variant_new("(ssss)", what, "XeonDesktop",
                      "Power Options handles the lid and the power buttons", "block"),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, buttons->cancel, inhibited,
        buttons);
}

void xd_buttons_update(xd_shell *shell) {
    if (shell->buttons != nullptr)
        request(shell->buttons);
}

static void got_bus([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GDBusConnection *bus = g_bus_get_finish(res, &error);
    if (bus == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fprintf(stderr, "xd-shell: no system bus, so logind keeps the power buttons: %s\n",
                    error->message);
        g_error_free(error);
        return;
    }
    xd_buttons *buttons = data;
    buttons->bus = bus;
    request(buttons);
}

void xd_buttons_init(xd_shell *shell) {
    if (!allowed(shell))
        return;
    xd_buttons *buttons = g_new0(xd_buttons, 1);
    buttons->shell = shell;
    buttons->fd = -1;
    buttons->cancel = g_cancellable_new();
    shell->buttons = buttons;
    g_bus_get(G_BUS_TYPE_SYSTEM, buttons->cancel, got_bus, buttons);
}

void xd_buttons_finish(xd_shell *shell) {
    xd_buttons *buttons = shell->buttons;
    if (buttons == nullptr)
        return;
    shell->buttons = nullptr;
    g_cancellable_cancel(buttons->cancel);
    g_object_unref(buttons->cancel);
    drop(buttons);
    if (buttons->bus != nullptr)
        g_object_unref(buttons->bus);
    g_free(buttons);
}

static void act(xd_shell *shell, xd_button_action action) {
    switch (action) {
    case XD_BUTTON_ASK:
        xd_shell_launch(shell, xd_shell_own_command(shell, "xd-shutdown", nullptr));
        break;
    case XD_BUTTON_STANDBY:
        xd_sleep_now(shell, false);
        break;
    case XD_BUTTON_HIBERNATE:
        xd_sleep_now(shell, true);
        break;
    case XD_BUTTON_SHUTDOWN:
        xd_power_off_now(shell);
        break;
    case XD_BUTTON_LOCK:
        xd_shell_launch(shell, xd_shell_own_command(shell, "xd-lock", nullptr));
        break;
    case XD_BUTTON_LOGIND:
    case XD_BUTTON_NOTHING:
    case XD_BUTTON_ACTION_COUNT:
        break;
    }
}

static void got_docked(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GVariant *response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (response == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }
    xd_buttons *buttons = data;
    bool docked;
    if (response != nullptr) {
        GVariant *value = nullptr;
        g_variant_get(response, "(v)", &value);
        docked =
            g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
        g_variant_unref(value);
        g_variant_unref(response);
    } else {
        /* A logind without it: another monitor is what we can tell. */
        g_error_free(error);
        docked = wl_list_length(&buttons->shell->taskbars) > 1;
    }
    act(buttons->shell, xd_power_button_action(&buttons->shell->settings, XD_BUTTON_LID, docked));
}

void xd_buttons_event(xd_shell *shell, xd_power_button button) {
    xd_buttons *buttons = shell->buttons;
    /* Without our inhibitor for it, logind has acted already. */
    if (buttons == nullptr || buttons->fd < 0 ||
        strstr(buttons->held, handles[button]) == nullptr ||
        shell->settings.power_buttons[button] == XD_BUTTON_LOGIND)
        return;
    if (button != XD_BUTTON_LID) {
        act(shell, xd_power_button_action(&shell->settings, button, false));
        return;
    }
    g_dbus_connection_call(buttons->bus, LOGIN1, LOGIN1_PATH, "org.freedesktop.DBus.Properties",
                           "Get", g_variant_new("(ss)", LOGIN1_MANAGER, "Docked"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, buttons->cancel,
                           got_docked, buttons);
}
