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

static void read_props(battery *b) {
    GVariant *v;
    bool present = false;
    unsigned type = 0;
    if ((v = g_dbus_proxy_get_cached_property(b->proxy, "IsPresent")) != nullptr) {
        present = g_variant_get_boolean(v);
        g_variant_unref(v);
    }
    if ((v = g_dbus_proxy_get_cached_property(b->proxy, "Type")) != nullptr) {
        type = g_variant_get_uint32(v);
        g_variant_unref(v);
    }
    b->present = present && type == TYPE_BATTERY;
    if ((v = g_dbus_proxy_get_cached_property(b->proxy, "Percentage")) != nullptr) {
        b->percent = g_variant_get_double(v);
        g_variant_unref(v);
    }
    if ((v = g_dbus_proxy_get_cached_property(b->proxy, "State")) != nullptr) {
        b->state = g_variant_get_uint32(v);
        g_variant_unref(v);
    }
    if ((v = g_dbus_proxy_get_cached_property(b->proxy, "TimeToEmpty")) != nullptr) {
        b->to_empty = g_variant_get_int64(v);
        g_variant_unref(v);
    }
    if ((v = g_dbus_proxy_get_cached_property(b->proxy, "TimeToFull")) != nullptr) {
        b->to_full = g_variant_get_int64(v);
        g_variant_unref(v);
    }
    /* battery-{empty,caution,low,good,full}[-charging], as freedesktop
     * icon themes name them. */
    const char *level = b->percent >= 95   ? "full"
                        : b->percent >= 50 ? "good"
                        : b->percent >= 20 ? "low"
                        : b->percent >= 5  ? "caution"
                                           : "empty";
    bool plugged = b->state == CHARGING || b->state == FULLY_CHARGED ||
                   b->state == PENDING_CHARGE;
    g_snprintf(b->icon, sizeof(b->icon), "battery-%s%s", level,
               b->state == FULLY_CHARGED ? "-charged" : plugged ? "-charging" : "");
    /* Unplugging switches to Power Options' battery timers, and back. */
    bool on_battery = b->present && (b->state == DISCHARGING || b->state == EMPTY);
    if (on_battery != b->shell->on_battery) {
        b->shell->on_battery = on_battery;
        xd_idle_update(b->shell);
    }
    xd_tray_changed(b->shell);
    xd_shell_battery_sound(b->percent, b->present && (b->state == DISCHARGING || b->state == EMPTY));
}

static void props_changed([[maybe_unused]] GDBusProxy *p, [[maybe_unused]] GVariant *changed,
                          [[maybe_unused]] const char *const *invalidated, gpointer data) {
    read_props(data);
}

static void got_proxy([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, nullptr);
    if (proxy == nullptr)
        return; /* no UPower: no power meter */
    battery *b = data;
    b->proxy = proxy;
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(props_changed), b);
    read_props(b);
}

static const char *icon(void *data) {
    battery *b = data;
    /* Power Options' "Always show icon on the taskbar"; without it, XP
     * showed the meter only while running on batteries. */
    bool on_battery = b->state == DISCHARGING || b->state == EMPTY;
    return b->present && (b->shell->settings.battery_icon || on_battery) ? b->icon : nullptr;
}

/* "2 hr 13 min" */
static void append_duration(GString *s, int64_t secs) {
    int64_t mins = (secs + 30) / 60;
    if (mins >= 60)
        g_string_append_printf(s, "%" G_GINT64_FORMAT " hr ", mins / 60);
    g_string_append_printf(s, "%" G_GINT64_FORMAT " min", mins % 60);
}

/* XP's wording: "2 hr 13 min (85%) remaining", "85% remaining",
 * "Fully charged (100%)", "Charging (85%)". */
static char *tooltip(void *data) {
    battery *b = data;
    int pct = (int)lround(b->percent);
    GString *s = g_string_new(nullptr);
    switch (b->state) {
    case FULLY_CHARGED:
        g_string_append_printf(s, "Fully charged (%d%%)", pct);
        break;
    case CHARGING:
    case PENDING_CHARGE:
        g_string_append_printf(s, "Charging (%d%%)", pct);
        if (b->to_full > 0) {
            g_string_append(s, ", ");
            append_duration(s, b->to_full);
            g_string_append(s, " until full");
        }
        break;
    default:
        if (b->to_empty > 0) {
            append_duration(s, b->to_empty);
            g_string_append_printf(s, " (%d%%) remaining", pct);
        } else {
            g_string_append_printf(s, "%d%% remaining", pct);
        }
        break;
    }
    return g_string_free(s, FALSE);
}

static void power_options(xd_shell *shell, [[maybe_unused]] void *d) {
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-power", nullptr));
}

static void menu(void *data, xd_menu_tree *tree, xd_menu_model *m) {
    battery *b = data;
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, "Adjust Power Properties", nullptr);
    it->action = power_options;
    it->bold = true;
    (void)b;
}

static void destroy(void *data) {
    battery *b = data;
    g_cancellable_cancel(b->cancel);
    g_object_unref(b->cancel);
    if (b->proxy != nullptr)
        g_object_unref(b->proxy);
    g_free(b);
}

static void draw(void *data, const xd_theme *t, cairo_t *cr, xd_rect r) {
    battery *b = data;
    bool plugged = b->state == CHARGING || b->state == FULLY_CHARGED ||
                   b->state == PENDING_CHARGE;
    xd_theme_draw_tray_icon(t, cr, r, XD_TRAY_ICON_BATTERY, (int)lround(b->percent),
                            plugged ? XD_TRAY_ICON_CHARGING : 0);
}

static const xd_sysicon_ops ops = {
    .draw = draw,
    .icon = icon,
    .tooltip = tooltip,
    .menu = menu,
    .destroy = destroy,
};

void xd_sys_battery_init(xd_shell *shell) {
    battery *b = g_new0(battery, 1);
    b->shell = shell;
    b->cancel = g_cancellable_new();
    xd_tray_add_sysicon(shell, "battery", &ops, b);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, UPOWER,
                             DISPLAY_DEVICE, DEVICE_IFACE, b->cancel, got_proxy, b);
}
