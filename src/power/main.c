/* SPDX-License-Identifier: MIT
 * xd-power: Windows XP's Power Options Properties.
 *
 *   Power Schemes  turn off the monitor, stand by and hibernate after so
 *                  long idle: [ScreenSaver] MonitorOff and [Power] in
 *                  desktop.ini, which the shell acts on (idle.c, sleep.c)
 *   Advanced       the battery icon, locking before sleep, and logind's
 *                  lid and power button actions (system settings, shown
 *                  read-only)
 *   Hibernate      whether logind can hibernate, and the space it needs
 *
 *   xd-power [--page N]   0 Power Schemes ... 2 Hibernate
 */
#include <stdlib.h>
#include <sys/sysinfo.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"

#define LOGIN1 "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MANAGER "org.freedesktop.login1.Manager"
#define PROFILES "org.freedesktop.UPower.PowerProfiles"
#define PROFILES_PATH "/org/freedesktop/UPower/PowerProfiles"

static constexpr char TITLE[] = "Power Options";

/* XP's power schemes, plugged in: monitor, standby, hibernate minutes. */
static const struct {
    const char *label;
    int monitor, standby, hibernate;
} schemes[XD_POWER_SCHEME_COUNT] = {
    [XD_POWER_HOME] = {"Home/Office Desk", 20, 0, 0},
    [XD_POWER_PORTABLE] = {"Portable/Laptop", 15, 20, 180},
    [XD_POWER_PRESENTATION] = {"Presentation", 0, 0, 0},
    [XD_POWER_ALWAYS_ON] = {"Always On", 20, 0, 0},
    [XD_POWER_MINIMAL] = {"Minimal Power Management", 15, 0, 0},
    [XD_POWER_MAX_BATTERY] = {"Max Battery", 15, 20, 45},
};

/* XP's choices; 0 = Never. */
static const int minutes[] = {1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 240, 300, 0};

typedef struct props {
    GtkWidget *window, *apply;
    bool updating;
    GDBusProxy *logind;
    /* Power Schemes */
    GtkWidget *scheme, *scheme_frame, *monitor, *standby, *hibernate;
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

static void show_scheme_title(props *p) {
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(p->scheme));
    char *title = g_strdup_printf("Settings for %s power scheme",
                                  i >= 0 && i < XD_POWER_SCHEME_COUNT ? schemes[i].label : "the");
    gtk_frame_set_label(GTK_FRAME(p->scheme_frame), title);
    g_free(title);
}

static void on_scheme([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    show_scheme_title(p);
    if (p->updating)
        return;
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(p->scheme));
    if (i < 0 || i >= XD_POWER_SCHEME_COUNT)
        return;
    set_minutes(p->monitor, schemes[i].monitor);
    if (gtk_widget_get_sensitive(p->standby))
        set_minutes(p->standby, schemes[i].standby);
    if (gtk_widget_get_sensitive(p->hibernate))
        set_minutes(p->hibernate, schemes[i].hibernate);
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
    for (int i = 0; i < XD_POWER_SCHEME_COUNT; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(p->scheme), schemes[i].label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), p->scheme);
    gtk_box_pack_start(GTK_BOX(page), p->scheme, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    char *can_suspend = logind_can(p, "CanSuspend");
    char *can_hibernate = logind_can(p, "CanHibernate");
    struct {
        const char *text;
        GtkWidget **combo;
        int value;
        bool available;
    } rows[] = {
        {"Turn off _monitor:", &p->monitor, s->monitor_off, true},
        {"System _standby:", &p->standby, s->standby, can(can_suspend)},
        {"System _hibernates:", &p->hibernate, s->hibernate, can(can_hibernate)},
    };
    for (int i = 0; i < (int)XD_ARRAY_LEN(rows); i++) {
        GtkWidget *rl = gtk_label_new_with_mnemonic(rows[i].text);
        gtk_label_set_xalign(GTK_LABEL(rl), 0);
        *rows[i].combo = minutes_combo(p, rows[i].value);
        gtk_widget_set_hexpand(*rows[i].combo, TRUE);
        gtk_label_set_mnemonic_widget(GTK_LABEL(rl), *rows[i].combo);
        gtk_widget_set_sensitive(rl, rows[i].available);
        gtk_widget_set_sensitive(*rows[i].combo, rows[i].available);
        gtk_grid_attach(GTK_GRID(grid), rl, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), *rows[i].combo, 1, i, 1, 1);
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

    p->updating = true;
    gtk_combo_box_set_active(GTK_COMBO_BOX(p->scheme), s->power_scheme);
    p->updating = false;
    show_scheme_title(p);
    g_signal_connect(p->scheme, "changed", G_CALLBACK(on_scheme), p);
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
    int scheme = gtk_combo_box_get_active(GTK_COMBO_BOX(p->scheme));
    if (scheme >= 0 && scheme < XD_POWER_SCHEME_COUNT)
        s.power_scheme = (xd_power_scheme)scheme;
    s.monitor_off = get_minutes(p->monitor);
    if (gtk_widget_get_sensitive(p->standby))
        s.standby = get_minutes(p->standby);
    if (gtk_widget_get_sensitive(p->hibernate))
        s.hibernate = get_minutes(p->hibernate);
    s.battery_icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->battery_icon));
    s.resume_password = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->password));
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
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), XD_CLAMP(page, 0, 2));
    gtk_main();
    if (p.logind != nullptr)
        g_object_unref(p.logind);
    return EXIT_SUCCESS;
}
