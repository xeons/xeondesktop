/* SPDX-License-Identifier: MIT
 * The shell's own sound events: Device Connect and Disconnect, as USB
 * devices come and go (udev), and XP's battery alarms. Menus play theirs
 * in menu.c, the Windows Logon sound is main.c's.
 */
#include <glib-unix.h>
#include <libudev.h>

#include "../settings/soundplay.h"
#include "shell.h"

/* XP's Power Options alarms: low at 10%, critical at 3%. */
static constexpr double LOW_BATTERY = 10;
static constexpr double CRITICAL_BATTERY = 3;
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

void xd_shell_battery_sound(double percent, bool discharging) {
    /* Each alarm once on the way down; charging starts them again. */
    static bool low_played, critical_played;
    if (!discharging) {
        low_played = critical_played = false;
        return;
    }
    if (percent <= CRITICAL_BATTERY && !critical_played) {
        critical_played = low_played = true;
        xd_sound_play_event(XD_SOUND_CRITICAL_BATTERY);
    } else if (percent <= LOW_BATTERY && !low_played) {
        low_played = true;
        xd_sound_play_event(XD_SOUND_LOW_BATTERY);
    }
}
