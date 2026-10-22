/* SPDX-License-Identifier: MIT
 * xd-power: Windows XP's Power Options Properties.
 *
 *   Power Schemes  turn off the monitor, stand by and hibernate after so
 *                  long idle: [ScreenSaver] MonitorOff and [Power] in
 *                  desktop.ini, which the shell acts on (idle.c, sleep.c);
 *                  with a battery (UPower), XP's "Plugged in" and
 *                  "Running on batteries" columns; schemes of the user's
 *                  own with Save As and Delete (schemes.h)
 *   Alarms         (with a battery) the low and critical battery alarms:
 *                  their levels, and Alarm Action's sound, message and
 *                  stand by / hibernate / shut down, which the shell does
 *                  ([Power] LowAlarm*, CriticalAlarm*; sounds.c)
 *   Power Meter    (with a battery) the power source and charge, from
 *                  UPower, each battery's with "Show details for each
 *                  battery", and a battery's details when it's clicked
 *   Advanced       the battery icon, locking before sleep, and what closing
 *                  the lid and the power and sleep buttons do ([Power]
 *                  LidAction...; the shell does it, src/shell/buttons.c),
 *                  logind's own settings until the user chooses
 *   Hibernate      whether logind can hibernate, and the space it needs
 *
 *   xd-power [--page N]   the Nth tab shown, from 0 (Power Schemes)
 *
 * Not here: XP's Alarm Action "Run a program" and "Force stand by or
 * shutdown even if a program stops responding" (logind's inhibitors
 * decide that), and the UPS tab: XP's was for serial-port UPS units of
 * its own service; on Linux they're NUT's or apcupsd's, with their own
 * tools, and UPower shows a USB UPS as a battery in the Power Meter.
 */
#include <math.h>
#include <stdlib.h>
#include <sys/sysinfo.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "schemes.h"

#define LOGIN1 "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MANAGER "org.freedesktop.login1.Manager"
#define PROFILES "org.freedesktop.UPower.PowerProfiles"
#define PROFILES_PATH "/org/freedesktop/UPower/PowerProfiles"

#define UPOWER "org.freedesktop.UPower"
#define UPOWER_PATH "/org/freedesktop/UPower"
#define UPOWER_DISPLAY "/org/freedesktop/UPower/devices/DisplayDevice"
#define UPOWER_DEVICE "org.freedesktop.UPower.Device"

static constexpr char TITLE[] = "Power Options";

/* XP's choices; 0 = Never. */
static const int minutes[] = {1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 240, 300, 0};

/* The Power Schemes rows. */
enum { ROW_MONITOR, ROW_STANDBY, ROW_HIBERNATE, ROW_COUNT };

/* The two alarms. */
enum { ALARM_LOW, ALARM_CRITICAL, ALARM_COUNT };

/* UPower's Device.State, Type and Technology. */
enum { UP_CHARGING = 1, UP_DISCHARGING, UP_EMPTY, UP_FULLY_CHARGED, UP_PENDING_CHARGE,
       UP_PENDING_DISCHARGE };
enum { UP_TYPE_BATTERY = 2 };

typedef struct props {
    GtkWidget *window, *apply;
    bool updating;
    GDBusProxy *logind;
    /* Power Schemes */
    GPtrArray *schemes; /* xd_power_scheme_def * */
    bool battery;       /* this computer has one: XP's laptop columns */
    GtkWidget *scheme, *scheme_frame, *delete;
    GtkWidget *ac[ROW_COUNT], *dc[ROW_COUNT]; /* plugged in; on batteries (with a battery) */
    bool row_ok[ROW_COUNT];                   /* standby and hibernate need logind's yes */
    xd_power_timers dc_values;                /* without a battery: kept as they were */
    /* Alarms */
    bool can_standby, can_hibernate;
    xd_battery_alarm alarms[ALARM_COUNT]; /* as shown */
    GtkWidget *alarm_on[ALARM_COUNT], *alarm_scale[ALARM_COUNT], *alarm_level[ALARM_COUNT];
    GtkWidget *alarm_button[ALARM_COUNT], *alarm_notify[ALARM_COUNT], *alarm_action[ALARM_COUNT];
    /* Power Meter */
    GDBusProxy *upower, *display;
    GPtrArray *batteries; /* GDBusProxy *, UPower's batteries that power this computer */
    GtkWidget *meter_source, *meter_total, *meter_details, *meter_box;
    /* Advanced */
    GtkWidget *battery_icon, *password;
    GtkWidget *button[XD_POWER_BUTTON_COUNT]; /* Power buttons */
    char *button_shown[XD_POWER_BUTTON_COUNT]; /* the choice as opened or applied */
    bool button_unset[XD_POWER_BUTTON_COUNT];  /* then logind's */
} props;

static void changed(props *properties) {
    if (!properties->updating)
        gtk_widget_set_sensitive(properties->apply, TRUE);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(widget), child);
    return widget;
}

static GtkWidget *label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 56);
    return widget;
}

static GtkWidget *icon_row(const char *icon, const char *text) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), label(text), TRUE, TRUE, 0);
    return row;
}

/* ---- logind ----------------------------------------------------------------------- */

/* A logind property (a string), or nullptr. g_free it. */
static char *logind_string(props *properties, const char *name) {
    GVariant *variant = properties->logind != nullptr
                            ? g_dbus_proxy_get_cached_property(properties->logind, name)
                            : nullptr;
    char *value = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING)
                      ? g_variant_dup_string(variant, nullptr)
                      : nullptr;
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

/* CanSuspend, CanHibernate: "yes", "no", "na" (no hardware or swap) or
 * "challenge" (allowed with a password). */
static char *logind_can(props *properties, const char *method) {
    if (properties->logind == nullptr)
        return nullptr;
    GVariant *variant = g_dbus_proxy_call_sync(properties->logind, method, nullptr,
                                               G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    char *capability = nullptr;
    if (variant != nullptr) {
        g_variant_get(variant, "(s)", &capability);
        g_variant_unref(variant);
    }
    return capability;
}

static bool can(const char *answer) {
    return answer != nullptr && (strcmp(answer, "yes") == 0 || strcmp(answer, "challenge") == 0);
}

/* logind's HandleAction values in XP's words. */
static const char *action_label(const char *action) {
    static const char *const names[][2] = {
        {"ignore", "Do nothing"},
        {"poweroff", "Shut down"},
        {"reboot", "Restart"},
        {"halt", "Halt"},
        {"kexec", "Restart into another kernel"},
        {"suspend", "Stand by"},
        {"hibernate", "Hibernate"},
        {"hybrid-sleep", "Stand by and save to disk"},
        {"suspend-then-hibernate", "Stand by, then hibernate"},
        {"sleep", "Stand by or hibernate"},
        {"lock", "Lock the computer"},
        {"factory-reset", "Factory reset"},
    };
    for (size_t i = 0; action != nullptr && i < XD_ARRAY_LEN(names); i++) {
        if (strcmp(action, names[i][0]) == 0)
            return names[i][1];
    }
    return action != nullptr ? action : "(unknown)";
}

/* ---- Power Schemes ----------------------------------------------------------------- */

static void set_minutes(GtkWidget *combo, int value) {
    char id[16];
    g_snprintf(id, sizeof(id), "%d", value);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), id)) {
        /* Set by hand in desktop.ini: offer it too. */
        char *text = g_strdup_printf("After %d mins", value);
        gtk_combo_box_text_prepend(GTK_COMBO_BOX_TEXT(combo), id, text);
        g_free(text);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), id);
    }
}

static int get_minutes(GtkWidget *combo) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    return id != nullptr ? atoi(id) : 0;
}

static void on_value([[maybe_unused]] GtkWidget *widget, gpointer data) { changed(data); }

static GtkWidget *minutes_combo(props *properties, int value) {
    GtkWidget *widget = gtk_combo_box_text_new();
    for (size_t i = 0; i < XD_ARRAY_LEN(minutes); i++) {
        int minute_count = minutes[i];
        char id[16];
        g_snprintf(id, sizeof(id), "%d", minute_count);
        char *text = minute_count == 0    ? g_strdup("Never")
                     : minute_count == 1  ? g_strdup("After 1 min")
                     : minute_count == 60 ? g_strdup("After 1 hour")
                     : minute_count > 60  ? g_strdup_printf("After %d hours", minute_count / 60)
                                          : g_strdup_printf("After %d mins", minute_count);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), id, text);
        g_free(text);
    }
    set_minutes(widget, value);
    g_signal_connect(widget, "changed", G_CALLBACK(on_value), properties);
    return widget;
}

static char *schemes_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "power-schemes.ini", nullptr);
}

static xd_power_scheme_def *active_scheme(props *properties) {
    return xd_power_schemes_find(properties->schemes,
                                 gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->scheme)));
}

static void show_scheme_title(props *properties) {
    const xd_power_scheme_def *scheme = active_scheme(properties);
    char *title =
        g_strdup_printf("Settings for %s power scheme", scheme != nullptr ? scheme->name : "the");
    gtk_frame_set_label(GTK_FRAME(properties->scheme_frame), title);
    g_free(title);
    /* The last scheme can't go: there would be nothing to choose. */
    gtk_widget_set_sensitive(properties->delete, scheme != nullptr && properties->schemes->len > 1);
}

static xd_power_timers read_timers(GtkWidget *const combos[ROW_COUNT]) {
    return (xd_power_timers){get_minutes(combos[ROW_MONITOR]), get_minutes(combos[ROW_STANDBY]),
                             get_minutes(combos[ROW_HIBERNATE])};
}

static void show_timers(GtkWidget *const combos[ROW_COUNT], xd_power_timers power_timers) {
    set_minutes(combos[ROW_MONITOR], power_timers.monitor);
    set_minutes(combos[ROW_STANDBY], power_timers.standby);
    set_minutes(combos[ROW_HIBERNATE], power_timers.hibernate);
}

/* The schemes, `active` selected (by name). */
static void fill_schemes(props *properties, const char *active) {
    properties->updating = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(properties->scheme));
    for (guint i = 0; i < properties->schemes->len; i++) {
        const xd_power_scheme_def *scheme = g_ptr_array_index(properties->schemes, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->scheme), scheme->name,
                                  scheme->name);
    }
    if (active == nullptr ||
        !gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->scheme), active))
        gtk_combo_box_set_active(GTK_COMBO_BOX(properties->scheme), -1);
    properties->updating = false;
    show_scheme_title(properties);
}

/* Choosing a scheme shows its timers. */
static void show_scheme(props *properties) {
    const xd_power_scheme_def *scheme = active_scheme(properties);
    if (scheme == nullptr)
        return;
    bool was = properties->updating;
    properties->updating = true;
    show_timers(properties->ac, scheme->ac);
    if (properties->battery)
        show_timers(properties->dc, scheme->dc);
    else
        properties->dc_values = scheme->dc;
    properties->updating = was;
}

static void on_scheme([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    show_scheme_title(properties);
    show_scheme(properties);
    changed(properties);
}

static void error_box(props *properties, const char *what, const GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error != nullptr ? error->message : "");
    xd_message_box(GTK_WINDOW(properties->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

static bool save_schemes(props *properties) {
    char *path = schemes_path();
    GError *error = nullptr;
    bool ok = xd_power_schemes_save(path, properties->schemes, &error);
    g_free(path);
    if (!ok) {
        error_box(properties, "The power schemes could not be saved.", error);
        g_error_free(error);
    }
    return ok;
}

static void on_name_changed(GtkEditable *editable, gpointer ok) {
    gtk_widget_set_sensitive(ok,
                             xd_power_scheme_name_valid(gtk_entry_get_text(GTK_ENTRY(editable))));
}

/* Save As...: XP's Save Scheme box. The settings as they are on the page
 * become a scheme of that name, which is then the one selected. */
static void on_save_as([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    GtkWidget *dialog_window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog_window), "Save Scheme");
    gtk_window_set_transient_for(GTK_WINDOW(dialog_window), GTK_WINDOW(properties->window));
    gtk_window_set_modal(GTK_WINDOW(dialog_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog_window), FALSE);
    GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(dialog_window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(dialog_window), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog_window), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *save_label = gtk_label_new_with_mnemonic("_Save this power scheme as:");
    gtk_label_set_xalign(GTK_LABEL(save_label), 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), 63);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    const xd_power_scheme_def *current = active_scheme(properties);
    gtk_entry_set_text(GTK_ENTRY(entry), current != nullptr ? current->name : "");
    gtk_label_set_mnemonic_widget(GTK_LABEL(save_label), entry);
    g_signal_connect(entry, "changed", G_CALLBACK(on_name_changed), ok);
    on_name_changed(GTK_EDITABLE(entry), ok);
    gtk_box_pack_start(GTK_BOX(box), save_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog_window))), box, TRUE,
                       TRUE, 0);
    gtk_widget_show_all(dialog_window);

    while (gtk_dialog_run(GTK_DIALOG(dialog_window)) == GTK_RESPONSE_OK) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (!xd_power_scheme_name_valid(name))
            continue;
        if (xd_power_schemes_find(properties->schemes, name) != nullptr && current != nullptr &&
            strcmp(name, current->name) != 0) {
            char *question_text =
                g_strdup_printf("A power scheme named '%s' already exists. Do you want "
                                "to replace it?",
                                name);
            int response = xd_message_box(GTK_WINDOW(dialog_window), TITLE, XD_MB_WARNING,
                                          XD_MB_YES_NO, TRUE, question_text);
            g_free(question_text);
            if (response != GTK_RESPONSE_YES)
                continue;
        }
        xd_power_timers dc =
            properties->battery ? read_timers(properties->dc) : properties->dc_values;
        char *saved = g_strdup(name);
        xd_power_schemes_put(properties->schemes, saved, read_timers(properties->ac), dc);
        if (save_schemes(properties)) {
            fill_schemes(properties, saved);
            changed(properties);
        }
        g_free(saved);
        break;
    }
    gtk_widget_destroy(dialog_window);
}

static void on_delete([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    xd_power_scheme_def *scheme = active_scheme(properties);
    if (scheme == nullptr || properties->schemes->len <= 1)
        return;
    char *question_text =
        g_strdup_printf("Are you sure you want to delete the power scheme '%s'?", scheme->name);
    int response = xd_message_box(GTK_WINDOW(properties->window), "Confirm Scheme Delete",
                                  XD_MB_QUESTION, XD_MB_YES_NO, TRUE, question_text);
    g_free(question_text);
    if (response != GTK_RESPONSE_YES)
        return;
    g_ptr_array_remove(properties->schemes, scheme);
    save_schemes(properties);
    /* The first one left takes its place, applied with OK or Apply. */
    const xd_power_scheme_def *first = g_ptr_array_index(properties->schemes, 0);
    fill_schemes(properties, first->name);
    show_scheme(properties);
    changed(properties);
}

/* The active power profile (power-profiles-daemon), which System
 * Properties changes. */
static char *power_profile(void) {
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, nullptr, PROFILES,
        PROFILES_PATH, PROFILES, nullptr, nullptr);
    GVariant *variant =
        proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, "ActiveProfile") : nullptr;
    char *profile = nullptr;
    if (variant != nullptr) {
        const char *id = g_variant_get_string(variant, nullptr);
        profile = g_strdup(strcmp(id, "power-saver") == 0  ? "Power Saver"
                           : strcmp(id, "performance") == 0 ? "Performance"
                           : strcmp(id, "balanced") == 0    ? "Balanced"
                                                            : id);
        g_variant_unref(variant);
    }
    if (proxy != nullptr)
        g_object_unref(proxy);
    return profile;
}

/* UPower's display device is a battery: a portable computer, which XP
 * shows separate "Plugged in" and "Running on batteries" timers for. */
static bool has_battery(void) {
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, nullptr, UPOWER,
        UPOWER_DISPLAY, UPOWER_DEVICE, nullptr, nullptr);
    if (proxy == nullptr)
        return false;
    GVariant *present = g_dbus_proxy_get_cached_property(proxy, "IsPresent");
    GVariant *type = g_dbus_proxy_get_cached_property(proxy, "Type");
    bool battery = present != nullptr && type != nullptr &&
                   g_variant_is_of_type(present, G_VARIANT_TYPE_BOOLEAN) &&
                   g_variant_is_of_type(type, G_VARIANT_TYPE_UINT32) &&
                   g_variant_get_boolean(present) && g_variant_get_uint32(type) == 2;
    if (present != nullptr)
        g_variant_unref(present);
    if (type != nullptr)
        g_variant_unref(type);
    g_object_unref(proxy);
    return battery;
}

/* A column heading of the laptop layout: its icon over its name. */
static GtkWidget *column_head(const char *icon, const char *text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(text), FALSE, FALSE, 0);
    return box;
}

static GtkWidget *schemes_page(props *properties, const xd_settings *settings) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       icon_row("preferences-system-power-management",
                                "Select the power scheme with the most appropriate settings for "
                                "this computer. Note that changing the settings below will "
                                "modify the selected scheme."),
                       FALSE, FALSE, 0);
    GtkWidget *widget = gtk_label_new_with_mnemonic("Po_wer schemes");
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_box_pack_start(GTK_BOX(page), widget, FALSE, FALSE, 0);
    properties->scheme = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), properties->scheme);
    gtk_box_pack_start(GTK_BOX(page), properties->scheme, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *save_as = gtk_button_new_with_mnemonic("Sa_ve As...");
    properties->delete = gtk_button_new_with_mnemonic("_Delete");
    gtk_widget_set_size_request(save_as, 75, -1);
    gtk_widget_set_size_request(properties->delete, 75, -1);
    g_signal_connect(save_as, "clicked", G_CALLBACK(on_save_as), properties);
    g_signal_connect(properties->delete, "clicked", G_CALLBACK(on_delete), properties);
    gtk_box_pack_end(GTK_BOX(buttons), properties->delete, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), save_as, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    char *can_suspend = logind_can(properties, "CanSuspend");
    char *can_hibernate = logind_can(properties, "CanHibernate");
    properties->row_ok[ROW_MONITOR] = true;
    properties->row_ok[ROW_STANDBY] = properties->can_standby = can(can_suspend);
    properties->row_ok[ROW_HIBERNATE] = properties->can_hibernate = can(can_hibernate);
    static const char *const texts[ROW_COUNT] = {"Turn off _monitor:", "System _standby:",
                                                 "System _hibernates:"};
    const int ac[ROW_COUNT] = {settings->monitor_off, settings->standby, settings->hibernate};
    const int dc[ROW_COUNT] = {settings->monitor_off_battery, settings->standby_battery,
                               settings->hibernate_battery};
    properties->dc_values = (xd_power_timers){dc[0], dc[1], dc[2]};
    int top = 0;
    /* The two columns alike, as XP draws them. */
    GtkSizeGroup *columns = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    if (properties->battery) {
        GtkWidget *when = gtk_label_new("When computer is:");
        gtk_label_set_xalign(GTK_LABEL(when), 0);
        gtk_widget_set_valign(when, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), when, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), column_head("ac-adapter", "Plugged in"), 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), column_head("battery", "Running on batteries"), 2, 0, 1,
                        1);
        top = 1;
    }
    for (int i = 0; i < ROW_COUNT; i++) {
        GtkWidget *rl = gtk_label_new_with_mnemonic(texts[i]);
        gtk_label_set_xalign(GTK_LABEL(rl), 0);
        properties->ac[i] = minutes_combo(properties, ac[i]);
        gtk_widget_set_hexpand(properties->ac[i], TRUE);
        gtk_size_group_add_widget(columns, properties->ac[i]);
        gtk_label_set_mnemonic_widget(GTK_LABEL(rl), properties->ac[i]);
        gtk_widget_set_sensitive(rl, properties->row_ok[i]);
        gtk_widget_set_sensitive(properties->ac[i], properties->row_ok[i]);
        gtk_grid_attach(GTK_GRID(grid), rl, 0, top + i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), properties->ac[i], 1, top + i, 1, 1);
        if (properties->battery) {
            properties->dc[i] = minutes_combo(properties, dc[i]);
            gtk_widget_set_hexpand(properties->dc[i], TRUE);
            gtk_size_group_add_widget(columns, properties->dc[i]);
            gtk_widget_set_sensitive(properties->dc[i], properties->row_ok[i]);
            gtk_grid_attach(GTK_GRID(grid), properties->dc[i], 2, top + i, 1, 1);
        }
    }
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    if (!can(can_suspend) || !can(can_hibernate)) {
        char *text = g_strdup_printf(
            "%s%s", !can(can_suspend) ? "This computer can't stand by (logind says so). " : "",
            !can(can_hibernate) ? "Hibernation isn't set up (see the Hibernate tab)." : "");
        gtk_box_pack_start(GTK_BOX(box), label(text), FALSE, FALSE, 0);
        g_free(text);
    }
    g_object_unref(columns);
    g_free(can_suspend);
    g_free(can_hibernate);
    properties->scheme_frame = frame("", box);
    gtk_box_pack_start(GTK_BOX(page), properties->scheme_frame, FALSE, FALSE, 0);

    char *profile = power_profile();
    if (profile != nullptr) {
        char *text = g_strdup_printf("Power profile: %s. To change it, open System Properties, "
                                     "Hardware, Power Profiles.",
                                     profile);
        gtk_box_pack_start(GTK_BOX(page), label(text), FALSE, FALSE, 0);
        g_free(text);
        g_free(profile);
    }

    /* The timers shown are the ones in use; the scheme is the user's own
     * if it still exists, else the built-in one. */
    const char *active =
        settings->power_scheme_custom[0] != '\0' &&
                xd_power_schemes_find(properties->schemes, settings->power_scheme_custom) != nullptr
            ? settings->power_scheme_custom
            : xd_power_builtin_label(settings->power_scheme);
    fill_schemes(properties, active);
    g_signal_connect(properties->scheme, "changed", G_CALLBACK(on_scheme), properties);
    return page;
}

/* ---- Alarms ----------------------------------------------------------------------- */

static const char *action_text(xd_alarm_action alarm_action) {
    switch (alarm_action) {
    case XD_ALARM_STANDBY:
        return "Stand by";
    case XD_ALARM_HIBERNATE:
        return "Hibernate";
    case XD_ALARM_SHUTDOWN:
        return "Shut down";
    case XD_ALARM_NOTHING:
    case XD_ALARM_ACTION_COUNT:
        break;
    }
    return "No action";
}

/* The summary beside Alarm Action...: "Notification: Sound, Text". */
static void show_alarm(props *properties, int i) {
    const xd_battery_alarm *al = &properties->alarms[i];
    bool was = properties->updating;
    properties->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->alarm_on[i]), al->on);
    gtk_range_set_value(GTK_RANGE(properties->alarm_scale[i]), al->percent);
    properties->updating = was;
    char text[16];
    g_snprintf(text, sizeof(text), "%d%%", al->percent);
    gtk_label_set_text(GTK_LABEL(properties->alarm_level[i]), text);
    gtk_label_set_text(GTK_LABEL(properties->alarm_notify[i]), al->sound && al->message
                                                                   ? "Sound, Text"
                                                               : al->sound   ? "Sound"
                                                               : al->message ? "Text"
                                                                             : "None");
    gtk_label_set_text(GTK_LABEL(properties->alarm_action[i]), action_text(al->action));
    gtk_widget_set_sensitive(properties->alarm_scale[i], al->on);
    gtk_widget_set_sensitive(properties->alarm_button[i], al->on);
}

static void on_alarm_on(GtkToggleButton *toggle, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(toggle), "alarm"));
    properties->alarms[i].on = gtk_toggle_button_get_active(toggle);
    show_alarm(properties, i);
    changed(properties);
}

static void on_alarm_level(GtkRange *range, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(range), "alarm"));
    double value = gtk_range_get_value(range);
    int level = (int)lround(value);
    /* The low alarm stays at or above the critical one. */
    if (i == ALARM_LOW)
        level = XD_MAX(level, properties->alarms[ALARM_CRITICAL].percent);
    else
        level = XD_MIN(level, properties->alarms[ALARM_LOW].percent);
    properties->alarms[i].percent = level;
    show_alarm(properties, i);
    changed(properties);
}

typedef struct action_dialog {
    GtkWidget *when, *what;
} action_dialog;

static void on_when([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    action_dialog *action_controls = data;
    gtk_widget_set_sensitive(action_controls->what, gtk_toggle_button_get_active(
                                                        GTK_TOGGLE_BUTTON(action_controls->when)));
}

/* Alarm Action...: XP's Low/Critical Battery Alarm Actions box. */
static void on_alarm_action(GtkButton *button, gpointer data) {
    props *properties = data;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "alarm"));
    xd_battery_alarm *al = &properties->alarms[i];
    GtkWidget *widget = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(widget), i == ALARM_LOW ? "Low Battery Alarm Actions"
                                                            : "Critical Battery Alarm Actions");
    gtk_window_set_transient_for(GTK_WINDOW(widget), GTK_WINDOW(properties->window));
    gtk_window_set_modal(GTK_WINDOW(widget), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(widget), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(widget), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(widget), GTK_RESPONSE_OK);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *notify = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *sound = gtk_check_button_new_with_mnemonic("_Sound alarm");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sound), al->sound);
    GtkWidget *message = gtk_check_button_new_with_mnemonic("_Display message");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(message), al->message);
    gtk_box_pack_start(GTK_BOX(notify), sound, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(notify), message, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Notification", notify), FALSE, FALSE, 0);

    GtkWidget *action = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    action_dialog action_controls = {0};
    action_controls.when =
        gtk_check_button_new_with_mnemonic("_When the alarm goes off, the computer will:");
    action_controls.what = gtk_combo_box_text_new();
    /* What logind can do; Shut down always. */
    if (properties->can_standby)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(action_controls.what), "standby", "Stand by");
    if (properties->can_hibernate)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(action_controls.what), "hibernate",
                                  "Hibernate");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(action_controls.what), "shutdown", "Shut down");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(action_controls.when),
                                 al->action != XD_ALARM_NOTHING);
    if (al->action == XD_ALARM_NOTHING ||
        !gtk_combo_box_set_active_id(GTK_COMBO_BOX(action_controls.what),
                                     xd_alarm_action_name(al->action)))
        gtk_combo_box_set_active(GTK_COMBO_BOX(action_controls.what), 0);
    gtk_widget_set_margin_start(action_controls.what, 20);
    g_signal_connect(action_controls.when, "toggled", G_CALLBACK(on_when), &action_controls);
    on_when(nullptr, &action_controls);
    gtk_box_pack_start(GTK_BOX(action), action_controls.when, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(action), action_controls.what, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Alarm action", action), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(widget))), page, TRUE, TRUE,
                       0);
    gtk_widget_show_all(widget);

    if (gtk_dialog_run(GTK_DIALOG(widget)) == GTK_RESPONSE_OK) {
        al->sound = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sound));
        al->message = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(message));
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(action_controls.what));
        al->action =
            !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(action_controls.when)) || id == nullptr
                ? XD_ALARM_NOTHING
            : strcmp(id, "standby") == 0   ? XD_ALARM_STANDBY
            : strcmp(id, "hibernate") == 0 ? XD_ALARM_HIBERNATE
                                           : XD_ALARM_SHUTDOWN;
        show_alarm(properties, i);
        changed(properties);
    }
    gtk_widget_destroy(widget);
}

/* "Notification:" and its value, in the summary grid. */
static GtkWidget *summary_row(GtkWidget *grid, int row, const char *what) {
    GtkWidget *key_label = gtk_label_new(what);
    gtk_label_set_xalign(GTK_LABEL(key_label), 0);
    GtkWidget *value_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(value_label), 0);
    gtk_grid_attach(GTK_GRID(grid), key_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value_label, 1, row, 1, 1);
    return value_label;
}

static GtkWidget *alarm_frame(props *properties, int i) {
    static const char *const titles[] = {"Low battery alarm", "Critical battery alarm"};
    static const char *const checks[] = {
        "Activate _low battery alarm when power level reaches:",
        "Activate _critical battery alarm when power level reaches:"};
    static const char *const buttons[] = {"Alar_m Action...", "Alarm Act_ion..."};
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    properties->alarm_on[i] = gtk_check_button_new_with_mnemonic(checks[i]);
    properties->alarm_level[i] = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(top), properties->alarm_on[i], FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top), properties->alarm_level[i], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    GtkWidget *slider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    properties->alarm_scale[i] = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(properties->alarm_scale[i]), FALSE);
    for (int percent = 0; percent <= 100; percent += 10)
        gtk_scale_add_mark(GTK_SCALE(properties->alarm_scale[i]), percent, GTK_POS_BOTTOM, nullptr);
    gtk_box_pack_start(GTK_BOX(slider), gtk_label_new("0%"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(slider), properties->alarm_scale[i], TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(slider), gtk_label_new("100%"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), slider, FALSE, FALSE, 0);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    properties->alarm_button[i] = gtk_button_new_with_mnemonic(buttons[i]);
    gtk_widget_set_valign(properties->alarm_button[i], GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(bottom), properties->alarm_button[i], FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    properties->alarm_notify[i] = summary_row(grid, 0, "Notification:");
    properties->alarm_action[i] = summary_row(grid, 1, "Action:");
    gtk_box_pack_start(GTK_BOX(bottom), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);

    for (GtkWidget *const *widget =
             (GtkWidget *const[]){properties->alarm_on[i], properties->alarm_scale[i],
                                  properties->alarm_button[i], nullptr};
         *widget != nullptr; widget++)
        g_object_set_data(G_OBJECT(*widget), "alarm", GINT_TO_POINTER(i));
    show_alarm(properties, i);
    g_signal_connect(properties->alarm_on[i], "toggled", G_CALLBACK(on_alarm_on), properties);
    g_signal_connect(properties->alarm_scale[i], "value-changed", G_CALLBACK(on_alarm_level),
                     properties);
    g_signal_connect(properties->alarm_button[i], "clicked", G_CALLBACK(on_alarm_action),
                     properties);
    return frame(titles[i], box);
}

static GtkWidget *alarms_page(props *properties, const xd_settings *settings) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    properties->alarms[ALARM_LOW] = settings->low_alarm;
    properties->alarms[ALARM_CRITICAL] = settings->critical_alarm;
    gtk_box_pack_start(GTK_BOX(page), alarm_frame(properties, ALARM_LOW), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), alarm_frame(properties, ALARM_CRITICAL), FALSE, FALSE, 0);
    return page;
}

/* ---- Power Meter ------------------------------------------------------------------ */

static GVariant *prop(GDBusProxy *proxy, const char *name, const GVariantType *type) {
    GVariant *variant = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
    if (variant != nullptr && !g_variant_is_of_type(variant, type)) {
        g_variant_unref(variant);
        variant = nullptr;
    }
    return variant;
}

static double prop_double(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name, G_VARIANT_TYPE_DOUBLE);
    double value = variant != nullptr ? g_variant_get_double(variant) : 0;
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

static guint32 prop_uint(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name, G_VARIANT_TYPE_UINT32);
    guint32 value = variant != nullptr ? g_variant_get_uint32(variant) : 0;
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

static bool prop_bool(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name, G_VARIANT_TYPE_BOOLEAN);
    bool value = variant != nullptr && g_variant_get_boolean(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

/* g_free it; "" if unset. */
static char *prop_string(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name, G_VARIANT_TYPE_STRING);
    char *str = g_strdup(variant != nullptr ? g_variant_get_string(variant, nullptr) : "");
    if (variant != nullptr)
        g_variant_unref(variant);
    return str;
}

static const char *state_text(guint32 state) {
    switch (state) {
    case UP_CHARGING:
        return "Charging";
    case UP_DISCHARGING:
        return "Discharging";
    case UP_EMPTY:
        return "Empty";
    case UP_FULLY_CHARGED:
        return "Fully charged";
    case UP_PENDING_CHARGE:
    case UP_PENDING_DISCHARGE:
        return "Not charging";
    default:
        return "Unknown";
    }
}

/* The freedesktop icon for a charge, as the shell's meter names them. */
static const char *battery_icon(double percent, guint32 state) {
    static const char *const icons[][3] = {
        {"battery-full", "battery-full-charging", "battery-full-charged"},
        {"battery-good", "battery-good-charging", "battery-good-charging"},
        {"battery-low", "battery-low-charging", "battery-low-charging"},
        {"battery-caution", "battery-caution-charging", "battery-caution-charging"},
        {"battery-empty", "battery-empty-charging", "battery-empty-charging"},
    };
    int level = percent >= 95 ? 0 : percent >= 50 ? 1 : percent >= 20 ? 2 : percent >= 5 ? 3 : 4;
    int kind = state == UP_FULLY_CHARGED                                  ? 2
               : state == UP_CHARGING || state == UP_PENDING_CHARGE ? 1
                                                                          : 0;
    return icons[level][kind];
}

static void clear(GtkWidget *container) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *list = kids; list != nullptr; list = list->next)
        gtk_widget_destroy(list->data);
    g_list_free(kids);
}

static void detail_row(GtkWidget *grid, int row, const char *what, const char *value) {
    GtkWidget *key_label = gtk_label_new(what);
    gtk_label_set_xalign(GTK_LABEL(key_label), 0);
    GtkWidget *value_label = gtk_label_new(value[0] != '\0' ? value : "Unknown");
    gtk_label_set_xalign(GTK_LABEL(value_label), 0);
    gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
    gtk_grid_attach(GTK_GRID(grid), key_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value_label, 1, row, 1, 1);
}

static const char *chemistry(guint32 technology) {
    static const char *const names[] = {"Unknown",         "Lithium ion",    "Lithium polymer",
                                        "Lithium iron phosphate", "Lead acid", "Nickel cadmium",
                                        "Nickel metal hydride"};
    return technology < XD_ARRAY_LEN(names) ? names[technology] : names[0];
}

static void fill_details(GtkWidget *grid, GDBusProxy *battery) {
    clear(grid);
    char *model = prop_string(battery, "Model");
    char *serial = prop_string(battery, "Serial");
    char *vendor = prop_string(battery, "Vendor");
    detail_row(grid, 0, "Battery name:", model);
    detail_row(grid, 1, "Unique ID:", serial);
    detail_row(grid, 2, "Manufacturer:", vendor);
    detail_row(grid, 3, "Chemistry:", chemistry(prop_uint(battery, "Technology")));
    char *state = g_strdup_printf("%s (%.0f%%)", state_text(prop_uint(battery, "State")),
                                  prop_double(battery, "Percentage"));
    detail_row(grid, 4, "Power state:", state);
    gtk_widget_show_all(grid);
    g_free(state);
    g_free(vendor);
    g_free(serial);
    g_free(model);
}

enum { RESPONSE_REFRESH = 1 };

/* A battery clicked: XP's Battery Details box. */
static void on_battery_clicked(GtkButton *button, gpointer data) {
    props *properties = data;
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "battery"));
    if (i >= properties->batteries->len)
        return;
    GDBusProxy *battery = g_object_ref(g_ptr_array_index(properties->batteries, i));
    GtkWidget *widget = gtk_dialog_new();
    char *title = g_strdup_printf("Battery #%u Details", i + 1);
    gtk_window_set_title(GTK_WINDOW(widget), title);
    g_free(title);
    gtk_window_set_transient_for(GTK_WINDOW(widget), GTK_WINDOW(properties->window));
    gtk_window_set_modal(GTK_WINDOW(widget), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(widget), "_Refresh", RESPONSE_REFRESH);
    gtk_dialog_add_button(GTK_DIALOG(widget), "OK", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(widget), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    fill_details(grid, battery);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(widget))), grid, TRUE, TRUE,
                       0);
    gtk_widget_show_all(widget);
    /* UPower keeps its properties current: Refresh shows them again. */
    while (gtk_dialog_run(GTK_DIALOG(widget)) == RESPONSE_REFRESH)
        fill_details(grid, battery);
    gtk_widget_destroy(widget);
    g_object_unref(battery);
}

/* One battery's (or, without details, all of them together): its icon,
 * "#1", the charge and the state. */
static GtkWidget *meter_column(props *properties, GDBusProxy *dev, int number) {
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    double pct = prop_double(dev, "Percentage");
    guint32 state = prop_uint(dev, "State");
    GtkWidget *img = gtk_image_new_from_icon_name(battery_icon(pct, state), GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    if (number > 0) {
        GtkWidget *widget = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(widget), GTK_RELIEF_NONE);
        gtk_container_add(GTK_CONTAINER(widget), img);
        char *tip = g_strdup_printf("Click for details about battery #%d", number);
        gtk_widget_set_tooltip_text(widget, tip);
        g_free(tip);
        g_object_set_data(G_OBJECT(widget), "battery", GUINT_TO_POINTER((guint)number - 1));
        g_signal_connect(widget, "clicked", G_CALLBACK(on_battery_clicked), properties);
        gtk_box_pack_start(GTK_BOX(col), widget, FALSE, FALSE, 0);
        char *num = g_strdup_printf("#%d", number);
        gtk_box_pack_start(GTK_BOX(col), gtk_label_new(num), FALSE, FALSE, 0);
        g_free(num);
    } else {
        gtk_box_pack_start(GTK_BOX(col), img, FALSE, FALSE, 0);
    }
    char *text = g_strdup_printf("%.0f%%", pct);
    gtk_box_pack_start(GTK_BOX(col), gtk_label_new(text), FALSE, FALSE, 0);
    g_free(text);
    gtk_box_pack_start(GTK_BOX(col), gtk_label_new(state_text(state)), FALSE, FALSE, 0);
    return col;
}

static void show_meter(props *properties) {
    if (properties->meter_box == nullptr)
        return;
    bool on_battery = prop_bool(properties->upower, "OnBattery");
    gtk_label_set_text(GTK_LABEL(properties->meter_source), on_battery ? "Batteries" : "AC power");
    char *total = g_strdup_printf("%.0f%%", prop_double(properties->display, "Percentage"));
    gtk_label_set_text(GTK_LABEL(properties->meter_total), total);
    g_free(total);
    clear(properties->meter_box);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->meter_details))) {
        for (guint i = 0; i < properties->batteries->len; i++)
            gtk_box_pack_start(
                GTK_BOX(properties->meter_box),
                meter_column(properties, g_ptr_array_index(properties->batteries, i), (int)i + 1),
                FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(properties->meter_box),
                           meter_column(properties, properties->display, 0), FALSE, FALSE, 0);
    }
    gtk_widget_show_all(properties->meter_box);
}

static void on_device_changed([[maybe_unused]] GDBusProxy *proxy,
                              [[maybe_unused]] GVariant *changed_props,
                              [[maybe_unused]] const char *const *invalidated, gpointer data) {
    show_meter(data);
}

static void drop_batteries(props *properties) {
    for (guint i = 0; i < properties->batteries->len; i++)
        g_signal_handlers_disconnect_by_data(g_ptr_array_index(properties->batteries, i),
                                             properties);
    g_ptr_array_set_size(properties->batteries, 0);
}

/* UPower's batteries that power the computer (not a mouse's). */
static void find_batteries(props *properties) {
    drop_batteries(properties);
    GVariant *variant =
        properties->upower != nullptr
            ? g_dbus_proxy_call_sync(properties->upower, "EnumerateDevices", nullptr,
                                     G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr)
            : nullptr;
    if (variant == nullptr)
        return;
    GVariantIter *it = nullptr;
    const char *path = nullptr;
    g_variant_get(variant, "(ao)", &it);
    while (g_variant_iter_next(it, "&o", &path)) {
        GDBusProxy *dev = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
                                                        nullptr, UPOWER, path, UPOWER_DEVICE,
                                                        nullptr, nullptr);
        if (dev == nullptr)
            continue;
        if (prop_uint(dev, "Type") == UP_TYPE_BATTERY && prop_bool(dev, "PowerSupply") &&
            prop_bool(dev, "IsPresent")) {
            g_signal_connect(dev, "g-properties-changed", G_CALLBACK(on_device_changed),
                             properties);
            g_ptr_array_add(properties->batteries, dev);
        } else {
            g_object_unref(dev);
        }
    }
    g_variant_iter_free(it);
    g_variant_unref(variant);
}

static void on_upower_signal([[maybe_unused]] GDBusProxy *proxy,
                             [[maybe_unused]] const char *sender, const char *signal,
                             [[maybe_unused]] GVariant *params, gpointer data) {
    props *properties = data;
    if (strcmp(signal, "DeviceAdded") == 0 || strcmp(signal, "DeviceRemoved") == 0) {
        find_batteries(properties);
        show_meter(properties);
    }
}

static void on_meter_details([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    show_meter(data);
}

static GtkWidget *meter_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    properties->upower =
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, UPOWER,
                                      UPOWER_PATH, UPOWER, nullptr, nullptr);
    properties->display =
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, UPOWER,
                                      UPOWER_DISPLAY, UPOWER_DEVICE, nullptr, nullptr);
    properties->batteries = g_ptr_array_new_with_free_func(g_object_unref);
    find_batteries(properties);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    properties->meter_source = summary_row(grid, 0, "Current power source:");
    properties->meter_total = summary_row(grid, 1, "Total battery power remaining:");
    gtk_box_pack_start(GTK_BOX(page), frame("Power status", grid), FALSE, FALSE, 0);
    properties->meter_details =
        gtk_check_button_new_with_mnemonic("_Show details for each battery.");
    gtk_box_pack_start(GTK_BOX(page), properties->meter_details, FALSE, FALSE, 0);
    properties->meter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_size_request(properties->meter_box, -1, 110);
    gtk_box_pack_start(GTK_BOX(page), frame("Power status", properties->meter_box), FALSE, FALSE,
                       0);
    show_meter(properties);

    g_signal_connect(properties->meter_details, "toggled", G_CALLBACK(on_meter_details),
                     properties);
    if (properties->upower != nullptr) {
        g_signal_connect(properties->upower, "g-signal", G_CALLBACK(on_upower_signal), properties);
        g_signal_connect(properties->upower, "g-properties-changed", G_CALLBACK(on_device_changed),
                         properties);
    }
    if (properties->display != nullptr)
        g_signal_connect(properties->display, "g-properties-changed", G_CALLBACK(on_device_changed),
                         properties);
    return page;
}

/* ---- Advanced --------------------------------------------------------------------- */

static const char *button_label(xd_button_action button_action) {
    switch (button_action) {
    case XD_BUTTON_NOTHING:
        return "Do nothing";
    case XD_BUTTON_ASK:
        return "Ask me what to do";
    case XD_BUTTON_STANDBY:
        return "Stand by";
    case XD_BUTTON_HIBERNATE:
        return "Hibernate";
    case XD_BUTTON_SHUTDOWN:
        return "Shut down";
    case XD_BUTTON_LOCK:
        return "Lock the computer";
    case XD_BUTTON_LOGIND:
    case XD_BUTTON_ACTION_COUNT:
        break;
    }
    return "";
}

/* A Power buttons list: XP's choices (the lid can't ask; it can lock, as
 * Linux users expect), Stand by and Hibernate if logind can. Unset, it
 * shows what logind does, which it keeps until another is chosen. */
static GtkWidget *button_combo(props *properties, const xd_settings *settings,
                               xd_power_button power_button) {
    /* logind's settings, and its defaults where it doesn't say. */
    static const char *const keys[XD_POWER_BUTTON_COUNT][2] = {
        [XD_BUTTON_LID] = {"HandleLidSwitch", "suspend"},
        [XD_BUTTON_POWER] = {"HandlePowerKey", "poweroff"},
        [XD_BUTTON_SLEEP] = {"HandleSuspendKey", "suspend"},
    };
    GtkWidget *widget = gtk_combo_box_text_new();
    xd_button_action offered[] = {XD_BUTTON_NOTHING, XD_BUTTON_LOCK,      XD_BUTTON_ASK,
                                  XD_BUTTON_STANDBY, XD_BUTTON_HIBERNATE, XD_BUTTON_SHUTDOWN};
    for (size_t i = 0; i < XD_ARRAY_LEN(offered); i++) {
        xd_button_action button_action = offered[i];
        if ((button_action == XD_BUTTON_LOCK && power_button != XD_BUTTON_LID) ||
            (button_action == XD_BUTTON_ASK && power_button == XD_BUTTON_LID) ||
            (button_action == XD_BUTTON_STANDBY && !properties->can_standby) ||
            (button_action == XD_BUTTON_HIBERNATE && !properties->can_hibernate))
            continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), xd_button_action_name(button_action),
                                  button_label(button_action));
    }
    xd_button_action button_action = settings->power_buttons[power_button];
    if (button_action == XD_BUTTON_LOGIND) {
        char *handle = logind_string(properties, keys[power_button][0]);
        const char *handle_action = handle != nullptr ? handle : keys[power_button][1];
        if (!xd_button_action_from_logind(handle_action, &button_action)) {
            /* One we don't offer (Restart, Stand by, then hibernate...). */
            gtk_combo_box_text_prepend(GTK_COMBO_BOX_TEXT(widget), "logind",
                                       action_label(handle_action));
            button_action = XD_BUTTON_LOGIND;
        }
        g_free(handle);
    }
    const char *id = xd_button_action_name(button_action);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id)) {
        /* Set by hand, or what this computer can't do: shown as it is. */
        gtk_combo_box_text_prepend(GTK_COMBO_BOX_TEXT(widget), id, button_label(button_action));
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id);
    }
    properties->button_shown[power_button] = g_strdup(id);
    properties->button_unset[power_button] =
        settings->power_buttons[power_button] == XD_BUTTON_LOGIND;
    g_signal_connect(widget, "changed", G_CALLBACK(on_value), properties);
    return widget;
}

/* The choice to save: unset stays so while it shows logind's. */
static xd_button_action button_chosen(props *properties, xd_power_button power_button) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->button[power_button]));
    xd_button_action button_action;
    if (id == nullptr ||
        (properties->button_unset[power_button] &&
         strcmp(id, properties->button_shown[power_button]) == 0) ||
        !xd_button_action_from_name(id, &button_action))
        return XD_BUTTON_LOGIND;
    return button_action;
}

static GtkWidget *advanced_page(props *properties, const xd_settings *settings) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       icon_row("preferences-system-power-management",
                                "Select the power-saving settings you want to use."),
                       FALSE, FALSE, 0);
    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    properties->battery_icon =
        gtk_check_button_new_with_mnemonic("Al_ways show icon on the taskbar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->battery_icon),
                                 settings->battery_icon);
    gtk_widget_set_tooltip_text(properties->battery_icon,
                                "Otherwise the power meter shows only while running on "
                                "batteries. Computers without a battery have none.");
    g_signal_connect(properties->battery_icon, "toggled", G_CALLBACK(on_value), properties);
    gtk_box_pack_start(GTK_BOX(opts), properties->battery_icon, FALSE, FALSE, 0);
    properties->password = gtk_check_button_new_with_mnemonic(
        "_Prompt for password when computer resumes from standby");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->password),
                                 settings->resume_password);
    g_signal_connect(properties->password, "toggled", G_CALLBACK(on_value), properties);
    gtk_box_pack_start(GTK_BOX(opts), properties->password, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Options", opts), FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    static const char *const texts[XD_POWER_BUTTON_COUNT] = {
        [XD_BUTTON_LID] = "When I c_lose the lid of my portable computer:",
        [XD_BUTTON_POWER] = "When I press the power _button on my computer:",
        [XD_BUTTON_SLEEP] = "When I press the sl_eep button on my computer:",
    };
    for (int button = 0; button < XD_POWER_BUTTON_COUNT; button++) {
        GtkWidget *widget = gtk_label_new_with_mnemonic(texts[button]);
        gtk_label_set_xalign(GTK_LABEL(widget), 0);
        gtk_box_pack_start(GTK_BOX(buttons), widget, FALSE, FALSE, 0);
        properties->button[button] = button_combo(properties, settings, (xd_power_button)button);
        gtk_label_set_mnemonic_widget(GTK_LABEL(widget), properties->button[button]);
        gtk_box_pack_start(GTK_BOX(buttons), properties->button[button], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(buttons),
                       label("These are your own; logind's settings apply until you change "
                             "them. Docked or with another monitor, the lid does nothing."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Power buttons", buttons), FALSE, FALSE, 0);
    return page;
}

/* ---- Hibernate -------------------------------------------------------------------- */

static GtkWidget *hibernate_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       icon_row("drive-harddisk",
                                "When your computer hibernates, it stores whatever it has in "
                                "memory on your hard disk and then shuts down. When your "
                                "computer comes out of hibernation, it returns to its previous "
                                "state."),
                       FALSE, FALSE, 0);
    char *answer = logind_can(properties, "CanHibernate");
    GtkWidget *enable = gtk_check_button_new_with_mnemonic("_Enable hibernation");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable), can(answer));
    gtk_widget_set_sensitive(enable, FALSE);
    gtk_box_pack_start(GTK_BOX(page), enable, FALSE, FALSE, 0);

    GtkWidget *space = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(space), 4);
    gtk_grid_set_column_spacing(GTK_GRID(space), 24);
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        unsigned long long unit = si.mem_unit != 0 ? si.mem_unit : 1;
        unsigned long long ram = si.totalram * unit;
        /* What a hibernation image holds, about: memory in use, less buffers. */
        unsigned long long used = (si.totalram - si.freeram - si.bufferram) * unit;
        unsigned long long swap = si.totalswap * unit;
        const struct {
            const char *what;
            unsigned long long bytes;
        } rows[] = {
            {"Total memory:", ram},
            {"Swap space:", swap},
            {"Disk space required to hibernate:", used},
        };
        for (int i = 0; i < (int)XD_ARRAY_LEN(rows); i++) {
            GtkWidget *what_label = gtk_label_new(rows[i].what);
            gtk_label_set_xalign(GTK_LABEL(what_label), 0);
            char *mb = g_strdup_printf("%llu MB", rows[i].bytes / (1024 * 1024));
            GtkWidget *size_label = gtk_label_new(mb);
            gtk_label_set_xalign(GTK_LABEL(size_label), 1);
            g_free(mb);
            gtk_grid_attach(GTK_GRID(space), what_label, 0, i, 1, 1);
            gtk_grid_attach(GTK_GRID(space), size_label, 1, i, 1, 1);
        }
    }
    gtk_box_pack_start(GTK_BOX(page), frame("Disk space for hibernation", space), FALSE, FALSE,
                       0);
    const char *why = answer == nullptr
                          ? "logind couldn't be asked whether this computer can hibernate."
                      : strcmp(answer, "yes") == 0 || strcmp(answer, "challenge") == 0
                          ? "Hibernation is set up. Stand by and Hibernate are in the Shut Down "
                            "dialog."
                          : "Hibernation isn't available: it needs a swap partition or file at "
                            "least as large as the memory in use, and the boot loader's resume= "
                            "setting. An administrator sets it up.";
    gtk_box_pack_start(GTK_BOX(page), label(why), FALSE, FALSE, 0);
    g_free(answer);
    return page;
}

/* ---- applying --------------------------------------------------------------------- */

static bool apply(props *properties) {
    /* Re-read: another applet may have saved since we opened. */
    xd_settings settings;
    xd_settings_load(&settings);
    xd_power_timers ac = read_timers(properties->ac);
    xd_power_timers dc = properties->battery ? read_timers(properties->dc) : properties->dc_values;
    /* "Changing the settings below will modify the selected scheme." */
    xd_power_scheme_def *scheme = active_scheme(properties);
    if (scheme != nullptr) {
        scheme->ac = ac;
        scheme->dc = dc;
        if (!save_schemes(properties))
            return false;
        if (scheme->builtin >= 0) {
            settings.power_scheme = (xd_power_scheme)scheme->builtin;
            settings.power_scheme_custom[0] = '\0';
        } else {
            g_strlcpy(settings.power_scheme_custom, scheme->name,
                      sizeof(settings.power_scheme_custom));
        }
    }
    settings.monitor_off = ac.monitor;
    settings.monitor_off_battery = dc.monitor;
    /* What logind can't do keeps what it was. */
    if (properties->row_ok[ROW_STANDBY]) {
        settings.standby = ac.standby;
        settings.standby_battery = dc.standby;
    }
    if (properties->row_ok[ROW_HIBERNATE]) {
        settings.hibernate = ac.hibernate;
        settings.hibernate_battery = dc.hibernate;
    }
    settings.battery_icon =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->battery_icon));
    settings.resume_password =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->password));
    if (properties->battery) {
        settings.low_alarm = properties->alarms[ALARM_LOW];
        settings.critical_alarm = properties->alarms[ALARM_CRITICAL];
    }
    for (int button = 0; button < XD_POWER_BUTTON_COUNT; button++)
        settings.power_buttons[button] = button_chosen(properties, (xd_power_button)button);
    if (!xd_settings_save(&settings)) {
        xd_message_box(GTK_WINDOW(properties->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The settings could not be saved.");
        return false;
    }
    /* What's saved is what the lists started from now. */
    for (int button = 0; button < XD_POWER_BUTTON_COUNT; button++) {
        g_free(properties->button_shown[button]);
        properties->button_shown[button] =
            g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->button[button])));
        properties->button_unset[button] = settings.power_buttons[button] == XD_BUTTON_LOGIND;
    }
    gtk_widget_set_sensitive(properties->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    props *properties = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply) &&
        !apply(properties))
        return;
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

int main(int argc, char **argv) {
    int page = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
    }
    xd_gtk_app_preinit("org.xeondesktop.Power");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props properties;
    xd_settings settings;
    xd_settings_load(&settings);
    GError *error = nullptr;
    properties.logind =
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, LOGIN1,
                                      LOGIN1_PATH, LOGIN1_MANAGER, nullptr, &error);
    if (properties.logind == nullptr) {
        g_printerr("xd-power: %s\n", error->message);
        g_error_free(error);
    }
    char *path = schemes_path();
    properties.schemes = xd_power_schemes_load(path);
    g_free(path);
    properties.battery = has_battery();

    properties.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties.window), "Power Options Properties");
    gtk_window_set_icon_name(GTK_WINDOW(properties.window), "preferences-system-power-management");
    gtk_window_set_resizable(GTK_WINDOW(properties.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "Cancel", GTK_RESPONSE_CANCEL);
    properties.apply =
        gtk_dialog_add_button(GTK_DIALOG(properties.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(properties.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(properties.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), schemes_page(&properties, &settings),
                             gtk_label_new_with_mnemonic("Power _Schemes"));
    /* XP shows these two only on computers with a battery. */
    if (properties.battery) {
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), alarms_page(&properties, &settings),
                                 gtk_label_new_with_mnemonic("_Alarms"));
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), meter_page(&properties),
                                 gtk_label_new_with_mnemonic("Power _Meter"));
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), advanced_page(&properties, &settings),
                             gtk_label_new_with_mnemonic("Ad_vanced"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), hibernate_page(&properties),
                             gtk_label_new_with_mnemonic("H_ibernate"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties.window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties.window, GTK_NOTEBOOK(tabs));
    g_signal_connect(properties.window, "response", G_CALLBACK(on_response), &properties);
    g_signal_connect(properties.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(properties.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs),
                                  XD_CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(tabs)) - 1));
    gtk_main();
    if (properties.logind != nullptr)
        g_object_unref(properties.logind);
    if (properties.batteries != nullptr) {
        drop_batteries(&properties);
        g_ptr_array_unref(properties.batteries);
    }
    if (properties.upower != nullptr)
        g_object_unref(properties.upower);
    if (properties.display != nullptr)
        g_object_unref(properties.display);
    g_ptr_array_unref(properties.schemes);
    for (int button = 0; button < XD_POWER_BUTTON_COUNT; button++)
        g_free(properties.button_shown[button]);
    return EXIT_SUCCESS;
}
