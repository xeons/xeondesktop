/* SPDX-License-Identifier: MIT
 * The power meter in the notification area: UPower's display device (the
 * combined battery), shown only on machines with a battery, as XP does.
 */
#include <gio/gio.h>
#include <math.h>

#include "shell.h"

#define UPOWER "org.freedesktop.UPower"
#define DISPLAY_DEVICE "/org/freedesktop/UPower/devices/DisplayDevice"
#define DEVICE_IFACE "org.freedesktop.UPower.Device"

/* UPower's Device.State and Device.Type. */
enum { CHARGING = 1, DISCHARGING = 2, EMPTY = 3, FULLY_CHARGED = 4, PENDING_CHARGE = 5 };
enum { TYPE_BATTERY = 2 };

typedef struct battery {
    xd_shell *shell;
    GDBusProxy *proxy;
    GCancellable *cancel;
    bool present;
    double percent;
    unsigned state;
    int64_t to_empty, to_full; /* seconds, 0 = unknown */
    char icon[64];
} battery;

static void read_props(battery *meter) {
    GVariant *variant;
    bool present = false;
    unsigned type = 0;
    if ((variant = g_dbus_proxy_get_cached_property(meter->proxy, "IsPresent")) != nullptr) {
        present = g_variant_get_boolean(variant);
        g_variant_unref(variant);
    }
    if ((variant = g_dbus_proxy_get_cached_property(meter->proxy, "Type")) != nullptr) {
        type = g_variant_get_uint32(variant);
        g_variant_unref(variant);
    }
    meter->present = present && type == TYPE_BATTERY;
    if ((variant = g_dbus_proxy_get_cached_property(meter->proxy, "Percentage")) != nullptr) {
        meter->percent = g_variant_get_double(variant);
        g_variant_unref(variant);
    }
    if ((variant = g_dbus_proxy_get_cached_property(meter->proxy, "State")) != nullptr) {
        meter->state = g_variant_get_uint32(variant);
        g_variant_unref(variant);
    }
    if ((variant = g_dbus_proxy_get_cached_property(meter->proxy, "TimeToEmpty")) != nullptr) {
        meter->to_empty = g_variant_get_int64(variant);
        g_variant_unref(variant);
    }
    if ((variant = g_dbus_proxy_get_cached_property(meter->proxy, "TimeToFull")) != nullptr) {
        meter->to_full = g_variant_get_int64(variant);
        g_variant_unref(variant);
    }
    /* battery-{empty,caution,low,good,full}[-charging], as freedesktop
     * icon themes name them. */
    const char *level = meter->percent >= 95   ? "full"
                        : meter->percent >= 50 ? "good"
                        : meter->percent >= 20 ? "low"
                        : meter->percent >= 5  ? "caution"
                                               : "empty";
    bool plugged =
        meter->state == CHARGING || meter->state == FULLY_CHARGED || meter->state == PENDING_CHARGE;
    g_snprintf(meter->icon, sizeof(meter->icon), "battery-%s%s", level,
               meter->state == FULLY_CHARGED ? "-charged"
               : plugged                     ? "-charging"
                                             : "");
    /* Unplugging switches to Power Options' battery timers, and back. */
    bool on_battery = meter->present && (meter->state == DISCHARGING || meter->state == EMPTY);
    if (on_battery != meter->shell->on_battery) {
        meter->shell->on_battery = on_battery;
        xd_idle_update(meter->shell);
    }
    xd_tray_changed(meter->shell);
    xd_shell_battery_alarm(meter->shell, meter->percent, on_battery);
}

static void props_changed([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *changed,
                          [[maybe_unused]] const char *const *invalidated, gpointer data) {
    read_props(data);
}

static void got_proxy([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, nullptr);
    if (proxy == nullptr)
        return; /* no UPower: no power meter */
    battery *meter = data;
    meter->proxy = proxy;
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(props_changed), meter);
    read_props(meter);
}

static const char *icon(void *data) {
    battery *meter = data;
    /* Power Options' "Always show icon on the taskbar"; without it, XP
     * showed the meter only while running on batteries. */
    bool on_battery = meter->state == DISCHARGING || meter->state == EMPTY;
    return meter->present && (meter->shell->settings.battery_icon || on_battery) ? meter->icon
                                                                                 : nullptr;
}

/* "2 hr 13 min" */
static void append_duration(GString *string, int64_t secs) {
    int64_t mins = (secs + 30) / 60;
    if (mins >= 60)
        g_string_append_printf(string, "%" G_GINT64_FORMAT " hr ", mins / 60);
    g_string_append_printf(string, "%" G_GINT64_FORMAT " min", mins % 60);
}

/* XP's wording: "2 hr 13 min (85%) remaining", "85% remaining",
 * "Fully charged (100%)", "Charging (85%)". */
static char *tooltip(void *data) {
    battery *meter = data;
    int pct = (int)lround(meter->percent);
    GString *string = g_string_new(nullptr);
    switch (meter->state) {
    case FULLY_CHARGED:
        g_string_append_printf(string, "Fully charged (%d%%)", pct);
        break;
    case CHARGING:
    case PENDING_CHARGE:
        g_string_append_printf(string, "Charging (%d%%)", pct);
        if (meter->to_full > 0) {
            g_string_append(string, ", ");
            append_duration(string, meter->to_full);
            g_string_append(string, " until full");
        }
        break;
    default:
        if (meter->to_empty > 0) {
            append_duration(string, meter->to_empty);
            g_string_append_printf(string, " (%d%%) remaining", pct);
        } else {
            g_string_append_printf(string, "%d%% remaining", pct);
        }
        break;
    }
    return g_string_free(string, FALSE);
}

static void power_options(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-power", nullptr));
}

/* Power Options' Power Meter tab (the third, with a battery). */
static void power_meter(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-power", "--page 2"));
}

static void open_meter(void *data) {
    battery *meter = data;
    power_meter(meter->shell, meter);
}

static void menu([[maybe_unused]] void *data, xd_menu_tree *tree, xd_menu_model *menu_model) {
    xd_menu_item *it =
        xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Adjust Power Properties", nullptr);
    it->action = power_options;
    it->bold = true;
    it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Open Power Meter", nullptr);
    it->action = power_meter;
}

static void destroy(void *data) {
    battery *meter = data;
    g_cancellable_cancel(meter->cancel);
    g_object_unref(meter->cancel);
    if (meter->proxy != nullptr)
        g_object_unref(meter->proxy);
    g_free(meter);
}

static void draw(void *data, const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    battery *meter = data;
    bool plugged =
        meter->state == CHARGING || meter->state == FULLY_CHARGED || meter->state == PENDING_CHARGE;
    xd_theme_draw_tray_icon(theme, cairo, rect, XD_TRAY_ICON_BATTERY, (int)lround(meter->percent),
                            plugged ? XD_TRAY_ICON_CHARGING : 0);
}

static const xd_sysicon_ops ops = {
    .draw = draw,
    .icon = icon,
    .tooltip = tooltip,
    .menu = menu,
    .open = open_meter,
    .destroy = destroy,
};

void xd_sys_battery_init(xd_shell *shell) {
    battery *meter = g_new0(battery, 1);
    meter->shell = shell;
    meter->cancel = g_cancellable_new();
    xd_tray_add_sysicon(shell, "battery", &ops, meter);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, UPOWER,
                             DISPLAY_DEVICE, DEVICE_IFACE, meter->cancel, got_proxy, meter);
}
