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
 *   Advanced       the battery icon, locking before sleep, and logind's
 *                  lid and power button actions (system settings, shown
 *                  read-only)
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
} props;

static void changed(props *p) {
    if (!p->updating)
        gtk_widget_set_sensitive(p->apply, TRUE);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
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
static char *logind_string(props *p, const char *name) {
    GVariant *v = p->logind != nullptr ? g_dbus_proxy_get_cached_property(p->logind, name)
                                       : nullptr;
    char *s = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                  ? g_variant_dup_string(v, nullptr)
                  : nullptr;
    if (v != nullptr)
        g_variant_unref(v);
    return s;
}

/* CanSuspend, CanHibernate: "yes", "no", "na" (no hardware or swap) or
 * "challenge" (allowed with a password). */
static char *logind_can(props *p, const char *method) {
    if (p->logind == nullptr)
        return nullptr;
    GVariant *r = g_dbus_proxy_call_sync(p->logind, method, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000,
                                         nullptr, nullptr);
    char *s = nullptr;
    if (r != nullptr) {
        g_variant_get(r, "(s)", &s);
        g_variant_unref(r);
    }
    return s;
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

static void on_value([[maybe_unused]] GtkWidget *w, gpointer data) { changed(data); }

static GtkWidget *minutes_combo(props *p, int value) {
    GtkWidget *c = gtk_combo_box_text_new();
    for (size_t i = 0; i < XD_ARRAY_LEN(minutes); i++) {
        int m = minutes[i];
        char id[16];
        g_snprintf(id, sizeof(id), "%d", m);
        char *text = m == 0     ? g_strdup("Never")
                     : m == 1   ? g_strdup("After 1 min")
                     : m == 60  ? g_strdup("After 1 hour")
                     : m > 60   ? g_strdup_printf("After %d hours", m / 60)
                                : g_strdup_printf("After %d mins", m);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), id, text);
        g_free(text);
    }
    set_minutes(c, value);
    g_signal_connect(c, "changed", G_CALLBACK(on_value), p);
    return c;
}

static char *schemes_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "power-schemes.ini", nullptr);
}

static xd_power_scheme_def *active_scheme(props *p) {
    return xd_power_schemes_find(p->schemes,
                                 gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->scheme)));
}

static void show_scheme_title(props *p) {
    const xd_power_scheme_def *d = active_scheme(p);
    char *title = g_strdup_printf("Settings for %s power scheme", d != nullptr ? d->name : "the");
    gtk_frame_set_label(GTK_FRAME(p->scheme_frame), title);
    g_free(title);
    /* The last scheme can't go: there would be nothing to choose. */
    gtk_widget_set_sensitive(p->delete, d != nullptr && p->schemes->len > 1);
}

static xd_power_timers read_timers(GtkWidget *const combos[ROW_COUNT]) {
    return (xd_power_timers){get_minutes(combos[ROW_MONITOR]), get_minutes(combos[ROW_STANDBY]),
                             get_minutes(combos[ROW_HIBERNATE])};
}

static void show_timers(GtkWidget *const combos[ROW_COUNT], xd_power_timers t) {
    set_minutes(combos[ROW_MONITOR], t.monitor);
    set_minutes(combos[ROW_STANDBY], t.standby);
    set_minutes(combos[ROW_HIBERNATE], t.hibernate);
}

/* The schemes, `active` selected (by name). */
static void fill_schemes(props *p, const char *active) {
    p->updating = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->scheme));
    for (guint i = 0; i < p->schemes->len; i++) {
        const xd_power_scheme_def *d = g_ptr_array_index(p->schemes, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->scheme), d->name, d->name);
    }
    if (active == nullptr || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->scheme), active))
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->scheme), -1);
    p->updating = false;
    show_scheme_title(p);
}

/* Choosing a scheme shows its timers. */
static void show_scheme(props *p) {
    const xd_power_scheme_def *d = active_scheme(p);
    if (d == nullptr)
        return;
    bool was = p->updating;
    p->updating = true;
    show_timers(p->ac, d->ac);
    if (p->battery)
        show_timers(p->dc, d->dc);
    else
        p->dc_values = d->dc;
    p->updating = was;
}

static void on_scheme([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    show_scheme_title(p);
    show_scheme(p);
    changed(p);
}

static void error_box(props *p, const char *what, const GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error != nullptr ? error->message : "");
    xd_message_box(GTK_WINDOW(p->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

static bool save_schemes(props *p) {
    char *path = schemes_path();
    GError *error = nullptr;
    bool ok = xd_power_schemes_save(path, p->schemes, &error);
    g_free(path);
    if (!ok) {
        error_box(p, "The power schemes could not be saved.", error);
        g_error_free(error);
    }
    return ok;
}

static void on_name_changed(GtkEditable *e, gpointer ok) {
    gtk_widget_set_sensitive(ok, xd_power_scheme_name_valid(gtk_entry_get_text(GTK_ENTRY(e))));
}

/* Save As...: XP's Save Scheme box. The settings as they are on the page
 * become a scheme of that name, which is then the one selected. */
static void on_save_as([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), "Save Scheme");
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(p->window));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *l = gtk_label_new_with_mnemonic("_Save this power scheme as:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), 63);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    const xd_power_scheme_def *current = active_scheme(p);
    gtk_entry_set_text(GTK_ENTRY(entry), current != nullptr ? current->name : "");
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), entry);
    g_signal_connect(entry, "changed", G_CALLBACK(on_name_changed), ok);
    on_name_changed(GTK_EDITABLE(entry), ok);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);
    gtk_widget_show_all(d);

    while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (!xd_power_scheme_name_valid(name))
            continue;
        if (xd_power_schemes_find(p->schemes, name) != nullptr && current != nullptr &&
            strcmp(name, current->name) != 0) {
            char *q = g_strdup_printf("A power scheme named '%s' already exists. Do you want "
                                      "to replace it?",
                                      name);
            int r = xd_message_box(GTK_WINDOW(d), TITLE, XD_MB_WARNING, XD_MB_YES_NO, TRUE, q);
            g_free(q);
            if (r != GTK_RESPONSE_YES)
                continue;
        }
        xd_power_timers dc = p->battery ? read_timers(p->dc) : p->dc_values;
        char *saved = g_strdup(name);
        xd_power_schemes_put(p->schemes, saved, read_timers(p->ac), dc);
        if (save_schemes(p)) {
            fill_schemes(p, saved);
            changed(p);
        }
        g_free(saved);
        break;
    }
    gtk_widget_destroy(d);
}

static void on_delete([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    xd_power_scheme_def *d = active_scheme(p);
    if (d == nullptr || p->schemes->len <= 1)
        return;
    char *q = g_strdup_printf("Are you sure you want to delete the power scheme '%s'?", d->name);
    int r = xd_message_box(GTK_WINDOW(p->window), "Confirm Scheme Delete", XD_MB_QUESTION,
                           XD_MB_YES_NO, TRUE, q);
    g_free(q);
    if (r != GTK_RESPONSE_YES)
        return;
    g_ptr_array_remove(p->schemes, d);
    save_schemes(p);
    /* The first one left takes its place, applied with OK or Apply. */
    const xd_power_scheme_def *first = g_ptr_array_index(p->schemes, 0);
    fill_schemes(p, first->name);
    show_scheme(p);
    changed(p);
}

/* The active power profile (power-profiles-daemon), which System
 * Properties changes. */
static char *power_profile(void) {
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, nullptr, PROFILES,
        PROFILES_PATH, PROFILES, nullptr, nullptr);
    GVariant *v = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, "ActiveProfile")
                                   : nullptr;
    char *profile = nullptr;
    if (v != nullptr) {
        const char *id = g_variant_get_string(v, nullptr);
        profile = g_strdup(strcmp(id, "power-saver") == 0  ? "Power Saver"
                           : strcmp(id, "performance") == 0 ? "Performance"
                           : strcmp(id, "balanced") == 0    ? "Balanced"
                                                            : id);
        g_variant_unref(v);
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

static GtkWidget *schemes_page(props *p, const xd_settings *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       icon_row("preferences-system-power-management",
                                "Select the power scheme with the most appropriate settings for "
                                "this computer. Note that changing the settings below will "
                                "modify the selected scheme."),
                       FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new_with_mnemonic("Po_wer schemes");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(page), l, FALSE, FALSE, 0);
    p->scheme = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), p->scheme);
    gtk_box_pack_start(GTK_BOX(page), p->scheme, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *save_as = gtk_button_new_with_mnemonic("Sa_ve As...");
    p->delete = gtk_button_new_with_mnemonic("_Delete");
    gtk_widget_set_size_request(save_as, 75, -1);
    gtk_widget_set_size_request(p->delete, 75, -1);
    g_signal_connect(save_as, "clicked", G_CALLBACK(on_save_as), p);
    g_signal_connect(p->delete, "clicked", G_CALLBACK(on_delete), p);
    gtk_box_pack_end(GTK_BOX(buttons), p->delete, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), save_as, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    char *can_suspend = logind_can(p, "CanSuspend");
    char *can_hibernate = logind_can(p, "CanHibernate");
    p->row_ok[ROW_MONITOR] = true;
    p->row_ok[ROW_STANDBY] = p->can_standby = can(can_suspend);
    p->row_ok[ROW_HIBERNATE] = p->can_hibernate = can(can_hibernate);
    static const char *const texts[ROW_COUNT] = {"Turn off _monitor:", "System _standby:",
                                                 "System _hibernates:"};
    const int ac[ROW_COUNT] = {s->monitor_off, s->standby, s->hibernate};
    const int dc[ROW_COUNT] = {s->monitor_off_battery, s->standby_battery, s->hibernate_battery};
    p->dc_values = (xd_power_timers){dc[0], dc[1], dc[2]};
    int top = 0;
    /* The two columns alike, as XP draws them. */
    GtkSizeGroup *columns = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    if (p->battery) {
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
        p->ac[i] = minutes_combo(p, ac[i]);
        gtk_widget_set_hexpand(p->ac[i], TRUE);
        gtk_size_group_add_widget(columns, p->ac[i]);
        gtk_label_set_mnemonic_widget(GTK_LABEL(rl), p->ac[i]);
        gtk_widget_set_sensitive(rl, p->row_ok[i]);
        gtk_widget_set_sensitive(p->ac[i], p->row_ok[i]);
        gtk_grid_attach(GTK_GRID(grid), rl, 0, top + i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), p->ac[i], 1, top + i, 1, 1);
        if (p->battery) {
            p->dc[i] = minutes_combo(p, dc[i]);
            gtk_widget_set_hexpand(p->dc[i], TRUE);
            gtk_size_group_add_widget(columns, p->dc[i]);
            gtk_widget_set_sensitive(p->dc[i], p->row_ok[i]);
            gtk_grid_attach(GTK_GRID(grid), p->dc[i], 2, top + i, 1, 1);
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
    p->scheme_frame = frame("", box);
    gtk_box_pack_start(GTK_BOX(page), p->scheme_frame, FALSE, FALSE, 0);

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
    const char *active = s->power_scheme_custom[0] != '\0' &&
                                 xd_power_schemes_find(p->schemes, s->power_scheme_custom) != nullptr
                             ? s->power_scheme_custom
                             : xd_power_builtin_label(s->power_scheme);
    fill_schemes(p, active);
    g_signal_connect(p->scheme, "changed", G_CALLBACK(on_scheme), p);
    return page;
}

/* ---- Alarms ----------------------------------------------------------------------- */

static const char *action_text(xd_alarm_action a) {
    switch (a) {
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
static void show_alarm(props *p, int i) {
    const xd_battery_alarm *al = &p->alarms[i];
    bool was = p->updating;
    p->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->alarm_on[i]), al->on);
    gtk_range_set_value(GTK_RANGE(p->alarm_scale[i]), al->percent);
    p->updating = was;
    char text[16];
    g_snprintf(text, sizeof(text), "%d%%", al->percent);
    gtk_label_set_text(GTK_LABEL(p->alarm_level[i]), text);
    gtk_label_set_text(GTK_LABEL(p->alarm_notify[i]),
                       al->sound && al->message ? "Sound, Text"
                       : al->sound              ? "Sound"
                       : al->message            ? "Text"
                                                : "None");
    gtk_label_set_text(GTK_LABEL(p->alarm_action[i]), action_text(al->action));
    gtk_widget_set_sensitive(p->alarm_scale[i], al->on);
    gtk_widget_set_sensitive(p->alarm_button[i], al->on);
}

static void on_alarm_on(GtkToggleButton *b, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "alarm"));
    p->alarms[i].on = gtk_toggle_button_get_active(b);
    show_alarm(p, i);
    changed(p);
}

static void on_alarm_level(GtkRange *r, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "alarm"));
    double value = gtk_range_get_value(r);
    int level = (int)lround(value);
    /* The low alarm stays at or above the critical one. */
    if (i == ALARM_LOW)
        level = XD_MAX(level, p->alarms[ALARM_CRITICAL].percent);
    else
        level = XD_MIN(level, p->alarms[ALARM_LOW].percent);
    p->alarms[i].percent = level;
    show_alarm(p, i);
    changed(p);
}

typedef struct action_dialog {
    GtkWidget *when, *what;
} action_dialog;

static void on_when([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    action_dialog *a = data;
    gtk_widget_set_sensitive(a->what,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->when)));
}

/* Alarm Action...: XP's Low/Critical Battery Alarm Actions box. */
static void on_alarm_action(GtkButton *b, gpointer data) {
    props *p = data;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "alarm"));
    xd_battery_alarm *al = &p->alarms[i];
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), i == ALARM_LOW ? "Low Battery Alarm Actions"
                                                       : "Critical Battery Alarm Actions");
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(p->window));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
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
    action_dialog a = {0};
    a.when = gtk_check_button_new_with_mnemonic("_When the alarm goes off, the computer will:");
    a.what = gtk_combo_box_text_new();
    /* What logind can do; Shut down always. */
    if (p->can_standby)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a.what), "standby", "Stand by");
    if (p->can_hibernate)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a.what), "hibernate", "Hibernate");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a.what), "shutdown", "Shut down");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a.when), al->action != XD_ALARM_NOTHING);
    if (al->action == XD_ALARM_NOTHING ||
        !gtk_combo_box_set_active_id(GTK_COMBO_BOX(a.what), xd_alarm_action_name(al->action)))
        gtk_combo_box_set_active(GTK_COMBO_BOX(a.what), 0);
    gtk_widget_set_margin_start(a.what, 20);
    g_signal_connect(a.when, "toggled", G_CALLBACK(on_when), &a);
    on_when(nullptr, &a);
    gtk_box_pack_start(GTK_BOX(action), a.when, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(action), a.what, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Alarm action", action), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), page, TRUE, TRUE, 0);
    gtk_widget_show_all(d);

    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        al->sound = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sound));
        al->message = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(message));
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(a.what));
        al->action = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a.when)) || id == nullptr
                         ? XD_ALARM_NOTHING
                     : strcmp(id, "standby") == 0   ? XD_ALARM_STANDBY
                     : strcmp(id, "hibernate") == 0 ? XD_ALARM_HIBERNATE
                                                    : XD_ALARM_SHUTDOWN;
        show_alarm(p, i);
        changed(p);
    }
    gtk_widget_destroy(d);
}

/* "Notification:" and its value, in the summary grid. */
static GtkWidget *summary_row(GtkWidget *grid, int row, const char *what) {
    GtkWidget *k = gtk_label_new(what);
    gtk_label_set_xalign(GTK_LABEL(k), 0);
    GtkWidget *v = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    return v;
}

static GtkWidget *alarm_frame(props *p, int i) {
    static const char *const titles[] = {"Low battery alarm", "Critical battery alarm"};
    static const char *const checks[] = {
        "Activate _low battery alarm when power level reaches:",
        "Activate _critical battery alarm when power level reaches:"};
    static const char *const buttons[] = {"Alar_m Action...", "Alarm Act_ion..."};
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->alarm_on[i] = gtk_check_button_new_with_mnemonic(checks[i]);
    p->alarm_level[i] = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(top), p->alarm_on[i], FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top), p->alarm_level[i], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    GtkWidget *slider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->alarm_scale[i] = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(p->alarm_scale[i]), FALSE);
    for (int v = 0; v <= 100; v += 10)
        gtk_scale_add_mark(GTK_SCALE(p->alarm_scale[i]), v, GTK_POS_BOTTOM, nullptr);
    gtk_box_pack_start(GTK_BOX(slider), gtk_label_new("0%"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(slider), p->alarm_scale[i], TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(slider), gtk_label_new("100%"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), slider, FALSE, FALSE, 0);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    p->alarm_button[i] = gtk_button_new_with_mnemonic(buttons[i]);
    gtk_widget_set_valign(p->alarm_button[i], GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(bottom), p->alarm_button[i], FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    p->alarm_notify[i] = summary_row(grid, 0, "Notification:");
    p->alarm_action[i] = summary_row(grid, 1, "Action:");
    gtk_box_pack_start(GTK_BOX(bottom), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);

    for (GtkWidget *const *w = (GtkWidget *const[]){p->alarm_on[i], p->alarm_scale[i],
                                                     p->alarm_button[i], nullptr};
         *w != nullptr; w++)
        g_object_set_data(G_OBJECT(*w), "alarm", GINT_TO_POINTER(i));
    show_alarm(p, i);
    g_signal_connect(p->alarm_on[i], "toggled", G_CALLBACK(on_alarm_on), p);
    g_signal_connect(p->alarm_scale[i], "value-changed", G_CALLBACK(on_alarm_level), p);
    g_signal_connect(p->alarm_button[i], "clicked", G_CALLBACK(on_alarm_action), p);
    return frame(titles[i], box);
}

static GtkWidget *alarms_page(props *p, const xd_settings *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    p->alarms[ALARM_LOW] = s->low_alarm;
    p->alarms[ALARM_CRITICAL] = s->critical_alarm;
    gtk_box_pack_start(GTK_BOX(page), alarm_frame(p, ALARM_LOW), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), alarm_frame(p, ALARM_CRITICAL), FALSE, FALSE, 0);
    return page;
}

/* ---- Power Meter ------------------------------------------------------------------ */

static GVariant *prop(GDBusProxy *proxy, const char *name, const GVariantType *type) {
    GVariant *v = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
    if (v != nullptr && !g_variant_is_of_type(v, type)) {
        g_variant_unref(v);
        v = nullptr;
    }
    return v;
}

static double prop_double(GDBusProxy *proxy, const char *name) {
    GVariant *v = prop(proxy, name, G_VARIANT_TYPE_DOUBLE);
    double d = v != nullptr ? g_variant_get_double(v) : 0;
    if (v != nullptr)
        g_variant_unref(v);
    return d;
}

static guint32 prop_uint(GDBusProxy *proxy, const char *name) {
    GVariant *v = prop(proxy, name, G_VARIANT_TYPE_UINT32);
    guint32 u = v != nullptr ? g_variant_get_uint32(v) : 0;
    if (v != nullptr)
        g_variant_unref(v);
    return u;
}

static bool prop_bool(GDBusProxy *proxy, const char *name) {
    GVariant *v = prop(proxy, name, G_VARIANT_TYPE_BOOLEAN);
    bool b = v != nullptr && g_variant_get_boolean(v);
    if (v != nullptr)
        g_variant_unref(v);
    return b;
}

/* g_free it; "" if unset. */
static char *prop_string(GDBusProxy *proxy, const char *name) {
    GVariant *v = prop(proxy, name, G_VARIANT_TYPE_STRING);
    char *str = g_strdup(v != nullptr ? g_variant_get_string(v, nullptr) : "");
    if (v != nullptr)
        g_variant_unref(v);
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
    for (GList *l = kids; l != nullptr; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
}

static void detail_row(GtkWidget *grid, int row, const char *what, const char *value) {
    GtkWidget *k = gtk_label_new(what);
    gtk_label_set_xalign(GTK_LABEL(k), 0);
    GtkWidget *v = gtk_label_new(value[0] != '\0' ? value : "Unknown");
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
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
static void on_battery_clicked(GtkButton *b, gpointer data) {
    props *p = data;
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "battery"));
    if (i >= p->batteries->len)
        return;
    GDBusProxy *battery = g_object_ref(g_ptr_array_index(p->batteries, i));
    GtkWidget *d = gtk_dialog_new();
    char *title = g_strdup_printf("Battery #%u Details", i + 1);
    gtk_window_set_title(GTK_WINDOW(d), title);
    g_free(title);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(p->window));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(d), "_Refresh", RESPONSE_REFRESH);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    fill_details(grid, battery);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    /* UPower keeps its properties current: Refresh shows them again. */
    while (gtk_dialog_run(GTK_DIALOG(d)) == RESPONSE_REFRESH)
        fill_details(grid, battery);
    gtk_widget_destroy(d);
    g_object_unref(battery);
}

/* One battery's (or, without details, all of them together): its icon,
 * "#1", the charge and the state. */
static GtkWidget *meter_column(props *p, GDBusProxy *dev, int number) {
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    double pct = prop_double(dev, "Percentage");
    guint32 state = prop_uint(dev, "State");
    GtkWidget *img = gtk_image_new_from_icon_name(battery_icon(pct, state), GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    if (number > 0) {
        GtkWidget *b = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_container_add(GTK_CONTAINER(b), img);
        char *tip = g_strdup_printf("Click for details about battery #%d", number);
        gtk_widget_set_tooltip_text(b, tip);
        g_free(tip);
        g_object_set_data(G_OBJECT(b), "battery", GUINT_TO_POINTER((guint)number - 1));
        g_signal_connect(b, "clicked", G_CALLBACK(on_battery_clicked), p);
        gtk_box_pack_start(GTK_BOX(col), b, FALSE, FALSE, 0);
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

static void show_meter(props *p) {
    if (p->meter_box == nullptr)
        return;
    bool on_battery = prop_bool(p->upower, "OnBattery");
    gtk_label_set_text(GTK_LABEL(p->meter_source), on_battery ? "Batteries" : "AC power");
    char *total = g_strdup_printf("%.0f%%", prop_double(p->display, "Percentage"));
    gtk_label_set_text(GTK_LABEL(p->meter_total), total);
    g_free(total);
    clear(p->meter_box);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->meter_details))) {
        for (guint i = 0; i < p->batteries->len; i++)
            gtk_box_pack_start(GTK_BOX(p->meter_box),
                               meter_column(p, g_ptr_array_index(p->batteries, i), (int)i + 1),
                               FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(p->meter_box), meter_column(p, p->display, 0), FALSE, FALSE, 0);
    }
    gtk_widget_show_all(p->meter_box);
}

static void on_device_changed([[maybe_unused]] GDBusProxy *proxy,
                              [[maybe_unused]] GVariant *changed_props,
                              [[maybe_unused]] const char *const *invalidated, gpointer data) {
    show_meter(data);
}

static void drop_batteries(props *p) {
    for (guint i = 0; i < p->batteries->len; i++)
        g_signal_handlers_disconnect_by_data(g_ptr_array_index(p->batteries, i), p);
    g_ptr_array_set_size(p->batteries, 0);
}

/* UPower's batteries that power the computer (not a mouse's). */
static void find_batteries(props *p) {
    drop_batteries(p);
    GVariant *r = p->upower != nullptr
                      ? g_dbus_proxy_call_sync(p->upower, "EnumerateDevices", nullptr,
                                               G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr)
                      : nullptr;
    if (r == nullptr)
        return;
    GVariantIter *it = nullptr;
    const char *path = nullptr;
    g_variant_get(r, "(ao)", &it);
    while (g_variant_iter_next(it, "&o", &path)) {
        GDBusProxy *dev = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
                                                        nullptr, UPOWER, path, UPOWER_DEVICE,
                                                        nullptr, nullptr);
        if (dev == nullptr)
            continue;
        if (prop_uint(dev, "Type") == UP_TYPE_BATTERY && prop_bool(dev, "PowerSupply") &&
            prop_bool(dev, "IsPresent")) {
            g_signal_connect(dev, "g-properties-changed", G_CALLBACK(on_device_changed), p);
            g_ptr_array_add(p->batteries, dev);
        } else {
            g_object_unref(dev);
        }
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
}

static void on_upower_signal([[maybe_unused]] GDBusProxy *proxy,
                             [[maybe_unused]] const char *sender, const char *signal,
                             [[maybe_unused]] GVariant *params, gpointer data) {
    props *p = data;
    if (strcmp(signal, "DeviceAdded") == 0 || strcmp(signal, "DeviceRemoved") == 0) {
        find_batteries(p);
        show_meter(p);
    }
}

static void on_meter_details([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    show_meter(data);
}

static GtkWidget *meter_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    p->upower = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                              UPOWER, UPOWER_PATH, UPOWER, nullptr, nullptr);
    p->display = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
                                               nullptr, UPOWER, UPOWER_DISPLAY, UPOWER_DEVICE,
                                               nullptr, nullptr);
    p->batteries = g_ptr_array_new_with_free_func(g_object_unref);
    find_batteries(p);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    p->meter_source = summary_row(grid, 0, "Current power source:");
    p->meter_total = summary_row(grid, 1, "Total battery power remaining:");
    gtk_box_pack_start(GTK_BOX(page), frame("Power status", grid), FALSE, FALSE, 0);
    p->meter_details = gtk_check_button_new_with_mnemonic("_Show details for each battery.");
    gtk_box_pack_start(GTK_BOX(page), p->meter_details, FALSE, FALSE, 0);
    p->meter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_size_request(p->meter_box, -1, 110);
    gtk_box_pack_start(GTK_BOX(page), frame("Power status", p->meter_box), FALSE, FALSE, 0);
    show_meter(p);

    g_signal_connect(p->meter_details, "toggled", G_CALLBACK(on_meter_details), p);
    if (p->upower != nullptr) {
        g_signal_connect(p->upower, "g-signal", G_CALLBACK(on_upower_signal), p);
        g_signal_connect(p->upower, "g-properties-changed", G_CALLBACK(on_device_changed), p);
    }
    if (p->display != nullptr)
        g_signal_connect(p->display, "g-properties-changed", G_CALLBACK(on_device_changed), p);
    return page;
}

/* ---- Advanced --------------------------------------------------------------------- */

static GtkWidget *advanced_page(props *p, const xd_settings *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       icon_row("preferences-system-power-management",
                                "Select the power-saving settings you want to use."),
                       FALSE, FALSE, 0);
    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    p->battery_icon = gtk_check_button_new_with_mnemonic("Al_ways show icon on the taskbar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->battery_icon), s->battery_icon);
    gtk_widget_set_tooltip_text(p->battery_icon,
                                "Otherwise the power meter shows only while running on "
                                "batteries. Computers without a battery have none.");
    g_signal_connect(p->battery_icon, "toggled", G_CALLBACK(on_value), p);
    gtk_box_pack_start(GTK_BOX(opts), p->battery_icon, FALSE, FALSE, 0);
    p->password = gtk_check_button_new_with_mnemonic(
        "_Prompt for password when computer resumes from standby");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->password), s->resume_password);
    g_signal_connect(p->password, "toggled", G_CALLBACK(on_value), p);
    gtk_box_pack_start(GTK_BOX(opts), p->password, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Options", opts), FALSE, FALSE, 0);

    /* logind's, for every user: shown, not changed. */
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    static const char *const keys[][2] = {
        {"HandleLidSwitch", "When I c_lose the lid of my portable computer:"},
        {"HandlePowerKey", "When I press the power _button on my computer:"},
        {"HandleSuspendKey", "When I press the sl_eep button on my computer:"},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(keys); i++) {
        GtkWidget *l = gtk_label_new_with_mnemonic(keys[i][1]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_pack_start(GTK_BOX(buttons), l, FALSE, FALSE, 0);
        GtkWidget *c = gtk_combo_box_text_new();
        char *action = logind_string(p, keys[i][0]);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), action_label(action));
        gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
        gtk_widget_set_sensitive(c, FALSE);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), c);
        gtk_box_pack_start(GTK_BOX(buttons), c, FALSE, FALSE, 0);
        g_free(action);
    }
    gtk_box_pack_start(GTK_BOX(buttons),
                       label("These are system settings, the same for every user. An "
                             "administrator changes them in /etc/systemd/logind.conf."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Power buttons", buttons), FALSE, FALSE, 0);
    return page;
}

/* ---- Hibernate -------------------------------------------------------------------- */

static GtkWidget *hibernate_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       icon_row("drive-harddisk",
                                "When your computer hibernates, it stores whatever it has in "
                                "memory on your hard disk and then shuts down. When your "
                                "computer comes out of hibernation, it returns to its previous "
                                "state."),
                       FALSE, FALSE, 0);
    char *answer = logind_can(p, "CanHibernate");
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
            GtkWidget *a = gtk_label_new(rows[i].what);
            gtk_label_set_xalign(GTK_LABEL(a), 0);
            char *mb = g_strdup_printf("%llu MB", rows[i].bytes / (1024 * 1024));
            GtkWidget *b = gtk_label_new(mb);
            gtk_label_set_xalign(GTK_LABEL(b), 1);
            g_free(mb);
            gtk_grid_attach(GTK_GRID(space), a, 0, i, 1, 1);
            gtk_grid_attach(GTK_GRID(space), b, 1, i, 1, 1);
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

static bool apply(props *p) {
    /* Re-read: another applet may have saved since we opened. */
    xd_settings s;
    xd_settings_load(&s);
    xd_power_timers ac = read_timers(p->ac);
    xd_power_timers dc = p->battery ? read_timers(p->dc) : p->dc_values;
    /* "Changing the settings below will modify the selected scheme." */
    xd_power_scheme_def *d = active_scheme(p);
    if (d != nullptr) {
        d->ac = ac;
        d->dc = dc;
        if (!save_schemes(p))
            return false;
        if (d->builtin >= 0) {
            s.power_scheme = (xd_power_scheme)d->builtin;
            s.power_scheme_custom[0] = '\0';
        } else {
            g_strlcpy(s.power_scheme_custom, d->name, sizeof(s.power_scheme_custom));
        }
    }
    s.monitor_off = ac.monitor;
    s.monitor_off_battery = dc.monitor;
    /* What logind can't do keeps what it was. */
    if (p->row_ok[ROW_STANDBY]) {
        s.standby = ac.standby;
        s.standby_battery = dc.standby;
    }
    if (p->row_ok[ROW_HIBERNATE]) {
        s.hibernate = ac.hibernate;
        s.hibernate_battery = dc.hibernate;
    }
    s.battery_icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->battery_icon));
    s.resume_password = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->password));
    if (p->battery) {
        s.low_alarm = p->alarms[ALARM_LOW];
        s.critical_alarm = p->alarms[ALARM_CRITICAL];
    }
    if (!xd_settings_save(&s)) {
        xd_message_box(GTK_WINDOW(p->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The settings could not be saved.");
        return false;
    }
    gtk_widget_set_sensitive(p->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !apply(p))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
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
    static props p;
    xd_settings s;
    xd_settings_load(&s);
    GError *error = nullptr;
    p.logind = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                             LOGIN1, LOGIN1_PATH, LOGIN1_MANAGER, nullptr, &error);
    if (p.logind == nullptr) {
        g_printerr("xd-power: %s\n", error->message);
        g_error_free(error);
    }
    char *path = schemes_path();
    p.schemes = xd_power_schemes_load(path);
    g_free(path);
    p.battery = has_battery();

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), "Power Options Properties");
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "preferences-system-power-management");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), schemes_page(&p, &s),
                             gtk_label_new_with_mnemonic("Power _Schemes"));
    /* XP shows these two only on computers with a battery. */
    if (p.battery) {
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), alarms_page(&p, &s),
                                 gtk_label_new_with_mnemonic("_Alarms"));
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), meter_page(&p),
                                 gtk_label_new_with_mnemonic("Power _Meter"));
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), advanced_page(&p, &s),
                             gtk_label_new_with_mnemonic("Ad_vanced"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), hibernate_page(&p),
                             gtk_label_new_with_mnemonic("H_ibernate"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));
    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs),
                                  XD_CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(tabs)) - 1));
    gtk_main();
    if (p.logind != nullptr)
        g_object_unref(p.logind);
    if (p.batteries != nullptr) {
        drop_batteries(&p);
        g_ptr_array_unref(p.batteries);
    }
    if (p.upower != nullptr)
        g_object_unref(p.upower);
    if (p.display != nullptr)
        g_object_unref(p.display);
    g_ptr_array_unref(p.schemes);
    return EXIT_SUCCESS;
}
