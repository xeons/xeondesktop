/* SPDX-License-Identifier: MIT
 * Sleep, through systemd-logind on the system bus: Power Options' System
 * standby / System hibernates after so long idle (idle.c calls
 * xd_sleep_now), and "Prompt for password when computer resumes": a
 * delay inhibitor holds the sleep back until xd-lock has covered the
 * screens, as XP locked before standing by.
 *
 * logind decides (through polkit, non-interactively here) whether the
 * user may sleep, and respects other programs' inhibitors. In a nested
 * or test session (XEONDESKTOP_NESTED) nothing is suspended unless
 * DBUS_SYSTEM_BUS_ADDRESS points at a bus of the tester's own (a mock
 * logind); the request is only printed.
 */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "shell.h"

#define LOGIN1 "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MANAGER "org.freedesktop.login1.Manager"

/* How long the locker gets to cover the screens before the computer
 * sleeps (logind waits at most InhibitDelayMaxSec, 5 s by default). */
static constexpr guint LOCK_GRACE_MS = 1000;

struct xd_sleep {
    xd_shell *shell;
    GDBusConnection *bus;
    GCancellable *cancel;
    guint prepare_sub; /* PrepareForSleep */
    int inhibit_fd;    /* our delay inhibitor, -1 if none */
    bool inhibiting;   /* Inhibit() in flight */
    guint release;     /* drops the inhibitor once the locker is up */
    bool sleeping;     /* between PrepareForSleep(true) and (false) */
};

static bool simulated(void) {
    const char *nested = getenv("XEONDESKTOP_NESTED");
    bool real = nested != nullptr && strcmp(nested, "0") == 0;
    return !real && getenv("DBUS_SYSTEM_BUS_ADDRESS") == nullptr;
}

static void drop_inhibitor(xd_sleep *sl) {
    if (sl->release != 0) {
        g_source_remove(sl->release);
        sl->release = 0;
    }
    if (sl->inhibit_fd >= 0) {
        close(sl->inhibit_fd);
        sl->inhibit_fd = -1;
    }
}

static bool wants_lock(const xd_sleep *sl) { return sl->shell->settings.resume_password; }

static void inhibited(GObject *src, GAsyncResult *res, gpointer data) {
    GUnixFDList *fds = nullptr;
    GError *error = nullptr;
    GVariant *variant =
        g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(src), &fds, res, &error);
    if (variant == nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error); /* the shell is exiting: sl is gone */
        return;
    }
    xd_sleep *sl = data;
    sl->inhibiting = false;
    if (variant == nullptr) {
        fprintf(stderr, "xd-shell: cannot delay sleep to lock the screen: %s\n", error->message);
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
    /* Settings may have changed, or sleep begun, meanwhile. */
    if (!wants_lock(sl) || sl->sleeping || sl->inhibit_fd >= 0)
        close(fd);
    else
        sl->inhibit_fd = fd;
}

static void take_inhibitor(xd_sleep *sl) {
    if (sl->bus == nullptr || sl->inhibit_fd >= 0 || sl->inhibiting || sl->sleeping)
        return;
    sl->inhibiting = true;
    g_dbus_connection_call_with_unix_fd_list(
        sl->bus, LOGIN1, LOGIN1_PATH, LOGIN1_MANAGER, "Inhibit",
        g_variant_new("(ssss)", "sleep", "XeonDesktop", "Locking the screen before sleeping",
                      "delay"),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, sl->cancel, inhibited, sl);
}

void xd_sleep_update(xd_shell *shell) {
    xd_sleep *sl = shell->sleep;
    if (sl == nullptr)
        return;
    if (wants_lock(sl))
        take_inhibitor(sl);
    else if (!sl->sleeping)
        drop_inhibitor(sl);
}

static gboolean release_now(gpointer data) {
    xd_sleep *sl = data;
    sl->release = 0;
    drop_inhibitor(sl);
    return G_SOURCE_REMOVE;
}

/* The locker starts with no signals blocked (the shell blocks SIGTERM
 * and SIGINT for its signalfd). */
static void unblock_signals([[maybe_unused]] gpointer data) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
}

/* Spawned directly: a locker needs no activation token, and there's no
 * need for a round trip to the compositor while it's going to sleep. */
static void lock_screen(void) {
    char *argv[] = {xd_shell_own_program("xd-lock"), nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_DEFAULT, unblock_signals, nullptr, nullptr,
                       &error)) {
        fprintf(stderr, "xd-shell: cannot lock the screen: %s\n", error->message);
        g_error_free(error);
    }
    g_free(argv[0]);
}

static void prepare_for_sleep([[maybe_unused]] GDBusConnection *bus,
                              [[maybe_unused]] const char *sender,
                              [[maybe_unused]] const char *path,
                              [[maybe_unused]] const char *iface,
                              [[maybe_unused]] const char *signal, GVariant *params,
                              gpointer data) {
    xd_sleep *sl = data;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(b)")))
        return;
    gboolean start = FALSE;
    g_variant_get(params, "(b)", &start);
    if (start) {
        sl->sleeping = true;
        if (wants_lock(sl)) {
            lock_screen();
            /* Let the computer sleep once the locker is up. */
            if (sl->inhibit_fd >= 0 && sl->release == 0)
                sl->release = g_timeout_add(LOCK_GRACE_MS, release_now, sl);
        } else {
            drop_inhibitor(sl);
        }
    } else {
        /* Awake: hold the next sleep back again. */
        sl->sleeping = false;
        drop_inhibitor(sl);
        xd_sleep_update(sl->shell);
    }
}

static void got_bus([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GDBusConnection *bus = g_bus_get_finish(res, &error);
    if (bus == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fprintf(stderr, "xd-shell: no system bus, so no standby: %s\n", error->message);
        g_error_free(error);
        return;
    }
    xd_sleep *sl = data;
    sl->bus = bus;
    sl->prepare_sub = g_dbus_connection_signal_subscribe(
        bus, LOGIN1, LOGIN1_MANAGER, "PrepareForSleep", LOGIN1_PATH, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, prepare_for_sleep, sl, nullptr);
    xd_sleep_update(sl->shell);
}

void xd_sleep_init(xd_shell *shell) {
    if (simulated())
        return;
    xd_sleep *sl = g_new0(xd_sleep, 1);
    sl->shell = shell;
    sl->inhibit_fd = -1;
    sl->cancel = g_cancellable_new();
    shell->sleep = sl;
    g_bus_get(G_BUS_TYPE_SYSTEM, sl->cancel, got_bus, sl);
}

void xd_sleep_finish(xd_shell *shell) {
    xd_sleep *sl = shell->sleep;
    if (sl == nullptr)
        return;
    shell->sleep = nullptr;
    g_cancellable_cancel(sl->cancel);
    g_object_unref(sl->cancel);
    drop_inhibitor(sl);
    if (sl->bus != nullptr) {
        g_dbus_connection_signal_unsubscribe(sl->bus, sl->prepare_sub);
        g_object_unref(sl->bus);
    }
    g_free(sl);
}

static void slept(GObject *src, GAsyncResult *res, gpointer data) {
    const char *method = data;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant != nullptr) {
        g_variant_unref(variant);
    } else {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            fprintf(stderr, "xd-shell: %s: %s\n", method, error->message);
        g_error_free(error);
    }
}

/* Suspend, Hibernate or PowerOff. */
static void call_logind(xd_shell *shell, const char *method) {
    xd_sleep *sl = shell->sleep;
    if (sl == nullptr || sl->bus == nullptr) {
        fprintf(stderr, "xd-shell: %s; %s\n", method,
                simulated() ? "nested session, not doing it" : "no system bus, not doing it");
        return;
    }
    /* interactive = false: no password prompt; logind just refuses if
     * polkit wants one, or if a program blocks it. */
    g_dbus_connection_call(sl->bus, LOGIN1, LOGIN1_PATH, LOGIN1_MANAGER, method,
                           g_variant_new("(b)", FALSE), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                           sl->cancel, slept, (gpointer)(uintptr_t)method);
}

void xd_sleep_now(xd_shell *shell, bool hibernate) {
    call_logind(shell, hibernate ? "Hibernate" : "Suspend");
}

void xd_power_off_now(xd_shell *shell) { call_logind(shell, "PowerOff"); }
