/* SPDX-License-Identifier: MIT
 * The shell's own sound events: Device Connect and Disconnect, as USB
 * devices come and go (udev), and Power Options' battery alarms, which
 * may also show a message and stand by, hibernate or shut down. Menus
 * play theirs in menu.c, the Windows Logon sound is main.c's.
 */
#include <glib-unix.h>
#include <libudev.h>

#include "../settings/soundplay.h"
#include "shell.h"

/* A hub and what's on it arrive together: one sound. */
static constexpr gint64 DEVICE_QUIET_US = 500 * G_TIME_SPAN_MILLISECOND;

static struct udev *udev;
static struct udev_monitor *monitor;

static gboolean on_udev(gint fd, [[maybe_unused]] GIOCondition cond,
                        [[maybe_unused]] gpointer data) {
    (void)fd;
    static gint64 last_add, last_remove;
    struct udev_device *dev = udev_monitor_receive_device(monitor);
    if (dev == nullptr)
        return G_SOURCE_CONTINUE;
    const char *action = udev_device_get_action(dev);
    gint64 now = g_get_monotonic_time();
    if (action != nullptr && strcmp(action, "add") == 0 && now - last_add > DEVICE_QUIET_US) {
        last_add = now;
        xd_sound_play_event(XD_SOUND_DEVICE_CONNECT);
    } else if (action != nullptr && strcmp(action, "remove") == 0 &&
               now - last_remove > DEVICE_QUIET_US) {
        last_remove = now;
        xd_sound_play_event(XD_SOUND_DEVICE_DISCONNECT);
    }
    udev_device_unref(dev);
    return G_SOURCE_CONTINUE;
}

void xd_shell_sounds_init(void) {
    udev = udev_new();
    monitor = udev != nullptr ? udev_monitor_new_from_netlink(udev, "udev") : nullptr;
    /* Whole USB devices, not each of their interfaces. */
    if (monitor == nullptr ||
        udev_monitor_filter_add_match_subsystem_devtype(monitor, "usb", "usb_device") < 0 ||
        udev_monitor_enable_receiving(monitor) < 0) {
        g_warning("no udev: no Device Connect and Disconnect sounds");
        return;
    }
    g_unix_fd_add(udev_monitor_get_fd(monitor), G_IO_IN, on_udev, nullptr);
}

/* Display message: XP's box, without a sound of its own (Sound alarm is
 * the alarm's other choice). */
static void alarm_message(xd_shell *shell, bool critical, double percent) {
    char *text = g_strdup_printf(
        critical ? "Your battery is critically low (%.0f%% remaining). Plug in your computer "
                   "now, or save your work and shut it down."
                 : "Your battery is running low (%.0f%% remaining). You might want to plug in "
                   "your computer.",
        percent);
    char *exe = xd_shell_own_program("xd-message");
    char *q_exe = g_shell_quote(exe), *q_text = g_shell_quote(text);
    char *cmd = g_strdup_printf("%s --quiet --warning '%s' %s", q_exe,
                                critical ? "Critical Battery Alarm" : "Low Battery Alarm", q_text);
    xd_shell_launch(shell, cmd);
    g_free(cmd);
    g_free(q_text);
    g_free(q_exe);
    g_free(exe);
    g_free(text);
}

void xd_shell_battery_alarm(xd_shell *shell, double percent, bool discharging) {
    static xd_alarm_done done;
    xd_alarm_fired fired = xd_battery_alarm_check(&shell->settings, &done, percent, discharging);
    if (fired == XD_ALARM_FIRED_NONE)
        return;
    bool critical = fired == XD_ALARM_FIRED_CRITICAL;
    const xd_battery_alarm *al = critical ? &shell->settings.critical_alarm
                                          : &shell->settings.low_alarm;
    fprintf(stderr, "xd-shell: %s battery alarm at %.0f%%, action %s\n",
            critical ? "critical" : "low", percent, xd_alarm_action_name(al->action));
    if (al->sound)
        xd_sound_play_event(critical ? XD_SOUND_CRITICAL_BATTERY : XD_SOUND_LOW_BATTERY);
    if (al->message)
        alarm_message(shell, critical, percent);
    switch (al->action) {
    case XD_ALARM_STANDBY:
        xd_sleep_now(shell, false);
        break;
    case XD_ALARM_HIBERNATE:
        xd_sleep_now(shell, true);
        break;
    case XD_ALARM_SHUTDOWN:
        xd_power_off_now(shell);
        break;
    case XD_ALARM_NOTHING:
    case XD_ALARM_ACTION_COUNT:
        break;
    }
}
