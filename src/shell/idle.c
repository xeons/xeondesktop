/* SPDX-License-Identifier: MIT
 * Idle timers: the screen saver after [ScreenSaver] Wait minutes,
 * monitors off after MonitorOff minutes, and standby or hibernation after
 * [Power] Standby / Hibernate minutes (sleep.c), all through
 * ext-idle-notify so video players (idle-inhibit) hold them off. Monitors
 * go off with wlr-output-power-management and come back on at the first
 * input.
 *
 * XD_IDLE_SECONDS=1 counts the settings in seconds instead (testing).
 */
#include <poll.h>
#include <stdlib.h>
#include <sys/pidfd.h>
#include <unistd.h>

#include "../saver/savers.h"
#include "shell.h"

static int timeout_ms(int minutes) {
    const char *test = getenv("XD_IDLE_SECONDS");
    bool seconds = test != nullptr && test[0] == '1';
    return minutes * (seconds ? 1000 : 60000);
}

/* ---- screen saver ------------------------------------------------------------ */

/* The saver is followed through a pidfd: the shell ignores SIGCHLD, so
 * children are reaped at once and a bare pid could soon name some other
 * process. */
static bool saver_running(xd_shell *shell) {
    if (shell->saver_pidfd < 0)
        return false;
    struct pollfd p = {.fd = shell->saver_pidfd, .events = POLLIN};
    if (poll(&p, 1, 0) == 0)
        return true;
    close(shell->saver_pidfd); /* readable: it exited */
    shell->saver_pidfd = -1;
    return false;
}

static void saver_idled(void *data, [[maybe_unused]] struct ext_idle_notification_v1 *n) {
    xd_shell *shell = data;
    if (saver_running(shell))
        return;
    pid_t pid = xd_shell_launch(shell, xd_shell_own_command(shell, "xd-lock", "--saver"));
    /* Fails if it already exited (and was reaped): then it isn't running. */
    shell->saver_pidfd = pid > 0 ? pidfd_open(pid, PIDFD_NONBLOCK) : -1;
}

static void saver_resumed([[maybe_unused]] void *data,
                          [[maybe_unused]] struct ext_idle_notification_v1 *n) {
    /* The saver ends itself on input. */
}

static const struct ext_idle_notification_v1_listener saver_listener = {
    .idled = saver_idled,
    .resumed = saver_resumed,
};

/* ---- monitor power ------------------------------------------------------------ */

static void power_mode([[maybe_unused]] void *data, [[maybe_unused]] struct zwlr_output_power_v1 *p,
                       [[maybe_unused]] uint32_t mode) {}

static void power_failed([[maybe_unused]] void *data, [[maybe_unused]] struct zwlr_output_power_v1 *p) {}

static const struct zwlr_output_power_v1_listener power_listener = {
    .mode = power_mode,
    .failed = power_failed,
};

static void set_monitors(xd_shell *shell, bool on) {
    if (shell->power_manager == nullptr)
        return;
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        struct zwlr_output_power_v1 *p =
            zwlr_output_power_manager_v1_get_output_power(shell->power_manager, bar->output);
        zwlr_output_power_v1_add_listener(p, &power_listener, shell);
        zwlr_output_power_v1_set_mode(p, on ? ZWLR_OUTPUT_POWER_V1_MODE_ON
                                            : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
        /* The mode sticks after the object goes. */
        zwlr_output_power_v1_destroy(p);
    }
    shell->monitors_off = !on;
}

static void monitor_idled(void *data, [[maybe_unused]] struct ext_idle_notification_v1 *n) {
    set_monitors(data, false);
}

static void monitor_resumed(void *data, [[maybe_unused]] struct ext_idle_notification_v1 *n) {
    xd_shell *shell = data;
    if (shell->monitors_off)
        set_monitors(shell, true);
}

static const struct ext_idle_notification_v1_listener monitor_listener = {
    .idled = monitor_idled,
    .resumed = monitor_resumed,
};

/* ---- standby and hibernation ----------------------------------------------------- */

static void standby_idled(void *data, [[maybe_unused]] struct ext_idle_notification_v1 *n) {
    xd_sleep_now(data, false);
}

static void hibernate_idled(void *data, [[maybe_unused]] struct ext_idle_notification_v1 *n) {
    xd_sleep_now(data, true);
}

static void sleep_resumed([[maybe_unused]] void *data,
                          [[maybe_unused]] struct ext_idle_notification_v1 *n) {}

static const struct ext_idle_notification_v1_listener standby_listener = {
    .idled = standby_idled,
    .resumed = sleep_resumed,
};

static const struct ext_idle_notification_v1_listener hibernate_listener = {
    .idled = hibernate_idled,
    .resumed = sleep_resumed,
};

/* ---- setup ------------------------------------------------------------------- */

static void drop_note(struct ext_idle_notification_v1 **note) {
    if (*note != nullptr) {
        ext_idle_notification_v1_destroy(*note);
        *note = nullptr;
    }
}

static struct ext_idle_notification_v1 *
add_note(xd_shell *shell, int minutes, const struct ext_idle_notification_v1_listener *listener) {
    struct ext_idle_notification_v1 *note = ext_idle_notifier_v1_get_idle_notification(
        shell->idle_notifier, (uint32_t)timeout_ms(minutes), shell->seat);
    ext_idle_notification_v1_add_listener(note, listener, shell);
    return note;
}

void xd_idle_update(xd_shell *shell) {
    drop_note(&shell->saver_note);
    drop_note(&shell->monitor_note);
    drop_note(&shell->standby_note);
    drop_note(&shell->hibernate_note);
    if (shell->idle_notifier == nullptr || shell->seat == nullptr)
        return;
    const xd_settings *s = &shell->settings;
    if (xd_saver_find(s->screensaver) != nullptr && s->screensaver_wait > 0)
        shell->saver_note = add_note(shell, s->screensaver_wait, &saver_listener);
    /* Power Options' "Plugged in" or "Running on batteries" column. */
    int monitor = shell->on_battery ? s->monitor_off_battery : s->monitor_off;
    int standby = shell->on_battery ? s->standby_battery : s->standby;
    int hibernate = shell->on_battery ? s->hibernate_battery : s->hibernate;
    if (monitor > 0)
        shell->monitor_note = add_note(shell, monitor, &monitor_listener);
    else if (shell->monitors_off)
        set_monitors(shell, true);
    if (standby > 0)
        shell->standby_note = add_note(shell, standby, &standby_listener);
    if (hibernate > 0)
        shell->hibernate_note = add_note(shell, hibernate, &hibernate_listener);
}
