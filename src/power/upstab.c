/* SPDX-License-Identifier: MIT */
#include "upstab.h"

#include "../gtkapp/gtkapp.h"
#include "upssource.h"

/* How often the tab asks NUT again (UPower tells it of changes). */
static constexpr guint NUT_REFRESH_SECONDS = 5;

/* The Status rows. */
enum { STATUS_SOURCE, STATUS_RUNTIME, STATUS_CAPACITY, STATUS_CONDITION, STATUS_COUNT };

struct xd_power_ups_tab {
    GtkWindow *window;
    GtkWidget *page;
    xd_ups_settings settings; /* as shown */
    bool can_hibernate;
    void (*on_changed)(void *data);
    void *data;
    GtkWidget *status[STATUS_COUNT];
    GtkWidget *manufacturer, *model, *configure;
    /* The selected UPS's source: UPower's device, or NUT's polling. */
    GCancellable *cancellable;
    GDBusProxy *upower_device;
    guint nut_timer;
    bool is_nut_reading;
};

static void set_label(GtkWidget *label, const char *text) {
    gtk_label_set_text(GTK_LABEL(label), text);
}

/* A reading shown; nullptr for no UPS selected. */
static void show_reading(xd_power_ups_tab *tab, const xd_ups_reading *reading) {
    if (reading == nullptr) {
        for (int i = 0; i < STATUS_COUNT; i++)
            set_label(tab->status[i], "Not Applicable");
        set_label(tab->manufacturer, "(None)");
        set_label(tab->model, "(None)");
        return;
    }
    char value[64];
    xd_ups_format_power_source(reading, value, sizeof(value));
    set_label(tab->status[STATUS_SOURCE], value);
    xd_ups_format_runtime(reading, value, sizeof(value));
    set_label(tab->status[STATUS_RUNTIME], value);
    xd_ups_format_capacity(reading, value, sizeof(value));
    set_label(tab->status[STATUS_CAPACITY], value);
    xd_ups_format_condition(reading, value, sizeof(value));
    set_label(tab->status[STATUS_CONDITION], value);
    set_label(tab->manufacturer,
              reading->manufacturer[0] != '\0' ? reading->manufacturer : "Unknown");
    set_label(tab->model, reading->model[0] != '\0' ? reading->model : "Unknown");
}

/* ---- the selected UPS's readings ------------------------------------------------- */

static void show_upower(xd_power_ups_tab *tab) {
    xd_ups_reading reading;
    xd_ups_read_upower(tab->upower_device, &reading);
    show_reading(tab, &reading);
}

static void on_upower_changed([[maybe_unused]] GDBusProxy *proxy,
                              [[maybe_unused]] GVariant *changed_properties,
                              [[maybe_unused]] const char *const *invalidated, gpointer data) {
    show_upower(data);
}

static void on_upower_device([[maybe_unused]] GObject *source, GAsyncResult *result,
                             gpointer data) {
    GError *error = nullptr;
    GDBusProxy *device = g_dbus_proxy_new_for_bus_finish(result, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return; /* another UPS selected, or the tab gone */
    }
    xd_power_ups_tab *tab = data;
    if (device == nullptr) {
        g_error_free(error);
        xd_ups_reading unreachable;
        xd_ups_reading_init(&unreachable);
        show_reading(tab, &unreachable);
        return;
    }
    tab->upower_device = device;
    g_signal_connect(device, "g-properties-changed", G_CALLBACK(on_upower_changed), tab);
    show_upower(tab);
}

static void read_nut(xd_power_ups_tab *tab);

static void on_nut_read([[maybe_unused]] GObject *source, GAsyncResult *result, gpointer data) {
    xd_ups_reading reading;
    if (!xd_ups_read_nut_finish(result, &reading))
        return; /* cancelled: the tab may be gone */
    xd_power_ups_tab *tab = data;
    tab->is_nut_reading = false;
    show_reading(tab, &reading);
}

static void read_nut(xd_power_ups_tab *tab) {
    const char *address = nullptr;
    if (tab->is_nut_reading ||
        xd_ups_get_device_kind(tab->settings.device, &address) != XD_UPS_DEVICE_NUT)
        return;
    tab->is_nut_reading = true;
    xd_ups_read_nut_async(address, tab->cancellable, on_nut_read, tab);
}

static gboolean on_nut_timer(gpointer data) {
    read_nut(data);
    return G_SOURCE_CONTINUE;
}

static void stop_source(xd_power_ups_tab *tab) {
    if (tab->cancellable != nullptr) {
        g_cancellable_cancel(tab->cancellable);
        g_clear_object(&tab->cancellable);
    }
    if (tab->upower_device != nullptr) {
        g_signal_handlers_disconnect_by_data(tab->upower_device, tab);
        g_clear_object(&tab->upower_device);
    }
    if (tab->nut_timer != 0) {
        g_source_remove(tab->nut_timer);
        tab->nut_timer = 0;
    }
    tab->is_nut_reading = false;
}

/* Watches the UPS selected now. */
static void start_source(xd_power_ups_tab *tab) {
    stop_source(tab);
    const char *address = nullptr;
    xd_ups_device_kind kind = xd_ups_get_device_kind(tab->settings.device, &address);
    gtk_widget_set_sensitive(tab->configure, kind != XD_UPS_DEVICE_NONE);
    if (kind == XD_UPS_DEVICE_NONE) {
        show_reading(tab, nullptr);
        return;
    }
    /* Until it answers. */
    xd_ups_reading unknown;
    xd_ups_reading_init(&unknown);
    show_reading(tab, &unknown);
    tab->cancellable = g_cancellable_new();
    if (kind == XD_UPS_DEVICE_UPOWER) {
        g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                 XD_UPOWER_NAME, address, XD_UPOWER_DEVICE_INTERFACE,
                                 tab->cancellable, on_upower_device, tab);
    } else {
        read_nut(tab);
        tab->nut_timer = g_timeout_add_seconds(NUT_REFRESH_SECONDS, on_nut_timer, tab);
    }
}

/* ---- dialogs --------------------------------------------------------------------- */

static GtkWidget *new_dialog(GtkWindow *parent, const char *title, const char *ok) {
    GtkWidget *dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(dialog), ok, GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    return dialog;
}

static GtkWidget *new_frame(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

static GtkWidget *new_left_label(const char *mnemonic_text) {
    GtkWidget *label = gtk_label_new_with_mnemonic(mnemonic_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    return label;
}

/* UPS Selection's list: (None), then the UPSes found. */
enum { SELECT_COLUMN_DEVICE, SELECT_COLUMN_DESCRIPTION, SELECT_COLUMN_COUNT };

typedef struct select_dialog {
    GtkListStore *store;
    GtkWidget *list, *host, *note;
} select_dialog;

/* The list again, with NUT's UPSes on the server typed. */
static void fill_selection(select_dialog *select, const char *selected) {
    gtk_list_store_clear(select->store);
    GPtrArray *found = g_ptr_array_new_with_free_func((GDestroyNotify)xd_ups_found_free);
    xd_ups_list_upower(found);
    char *host = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(select->host))));
    char *error = nullptr;
    bool is_answered = host[0] != '\0' && xd_ups_list_nut(host, found, &error);
    char *note = host[0] == '\0' ? g_strdup("")
                 : is_answered    ? g_strdup_printf("NUT on %s lists the UPSes it serves.", host)
                                  : g_strdup_printf("NUT didn't answer on %s (%s).", host,
                                                    error != nullptr ? error : "no reply");
    gtk_label_set_text(GTK_LABEL(select->note), note);
    g_free(note);
    g_free(error);
    g_free(host);

    GtkTreeIter iter;
    gtk_list_store_insert_with_values(select->store, &iter, -1, SELECT_COLUMN_DEVICE, "",
                                      SELECT_COLUMN_DESCRIPTION, "(None)", -1);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(select->list));
    gtk_tree_selection_select_iter(selection, &iter);
    bool is_listed = selected[0] == '\0';
    for (guint i = 0; i < found->len; i++) {
        const xd_ups_found *entry = g_ptr_array_index(found, i);
        gtk_list_store_insert_with_values(select->store, &iter, -1, SELECT_COLUMN_DEVICE,
                                          entry->device, SELECT_COLUMN_DESCRIPTION,
                                          entry->description, -1);
        if (strcmp(entry->device, selected) == 0) {
            gtk_tree_selection_select_iter(selection, &iter);
            is_listed = true;
        }
    }
    /* The one in use, though nothing answers for it now. */
    if (!is_listed) {
        const char *address = nullptr;
        xd_ups_get_device_kind(selected, &address);
        char *description = g_strdup_printf("%s (not found)", address);
        gtk_list_store_insert_with_values(select->store, &iter, -1, SELECT_COLUMN_DEVICE,
                                          selected, SELECT_COLUMN_DESCRIPTION, description, -1);
        gtk_tree_selection_select_iter(selection, &iter);
        g_free(description);
    }
    g_ptr_array_unref(found);
}

static void on_find([[maybe_unused]] GtkButton *button, gpointer data) {
    select_dialog *select = data;
    GtkTreeModel *model = nullptr;
    GtkTreeIter iter;
    char *selected = nullptr;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(select->list));
    if (gtk_tree_selection_get_selected(selection, &model, &iter))
        gtk_tree_model_get(model, &iter, SELECT_COLUMN_DEVICE, &selected, -1);
    fill_selection(select, selected != nullptr ? selected : "");
    g_free(selected);
}

/* The NUT server a device is on, for the Server box: "localhost" unless
 * it's another's. */
static char *get_nut_host(const char *device) {
    const char *address = nullptr;
    if (xd_ups_get_device_kind(device, &address) != XD_UPS_DEVICE_NUT)
        return g_strdup("localhost");
    const char *at = strrchr(address, '@');
    return g_strdup(at != nullptr && at[1] != '\0' ? at + 1 : "localhost");
}

static void on_select_activated([[maybe_unused]] GtkTreeView *view,
                                [[maybe_unused]] GtkTreePath *path,
                                [[maybe_unused]] GtkTreeViewColumn *column, gpointer dialog) {
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
}

/* Select...: XP's UPS Selection chose a maker, model and COM port; here
 * the UPS from those UPower and NUT know of. */
static void on_select([[maybe_unused]] GtkButton *button, gpointer data) {
    xd_power_ups_tab *tab = data;
    GtkWidget *dialog = new_dialog(tab->window, "UPS Selection", "Finish");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    select_dialog select = {0};
    select.store = gtk_list_store_new(SELECT_COLUMN_COUNT, G_TYPE_STRING, G_TYPE_STRING);
    select.list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(select.store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(select.list), FALSE);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(select.list), -1, nullptr,
                                                gtk_cell_renderer_text_new(), "text",
                                                SELECT_COLUMN_DESCRIPTION, nullptr);
    g_signal_connect(select.list, "row-activated", G_CALLBACK(on_select_activated), dialog);
    GtkWidget *label = new_left_label("_Select the UPS this computer is connected to:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), select.list);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 340, 120);
    gtk_container_add(GTK_CONTAINER(scroll), select.list);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget *server = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *host_label = new_left_label("NUT ser_ver:");
    select.host = gtk_entry_new();
    char *host = get_nut_host(tab->settings.device);
    gtk_entry_set_text(GTK_ENTRY(select.host), host);
    g_free(host);
    gtk_widget_set_tooltip_text(select.host, "The computer running NUT's upsd for the UPS (its "
                                             "name, or name:port)");
    gtk_label_set_mnemonic_widget(GTK_LABEL(host_label), select.host);
    GtkWidget *find = gtk_button_new_with_mnemonic("_Find");
    g_signal_connect(find, "clicked", G_CALLBACK(on_find), &select);
    gtk_box_pack_start(GTK_BOX(server), host_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(server), select.host, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(server), find, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), server, FALSE, FALSE, 0);
    select.note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(select.note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(select.note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(select.note), 48);
    gtk_box_pack_start(GTK_BOX(box), select.note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE,
                       0);
    fill_selection(&select, tab->settings.device);
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(select.list);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        GtkTreeModel *model = nullptr;
        GtkTreeIter iter;
        char *device = nullptr;
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(select.list));
        if (gtk_tree_selection_get_selected(selection, &model, &iter))
            gtk_tree_model_get(model, &iter, SELECT_COLUMN_DEVICE, &device, -1);
        if (device != nullptr && strcmp(device, tab->settings.device) != 0) {
            g_strlcpy(tab->settings.device, device, sizeof(tab->settings.device));
            start_source(tab);
            tab->on_changed(tab->data);
        }
        g_free(device);
    }
    gtk_widget_destroy(dialog);
    g_object_unref(select.store);
}

typedef struct configure_dialog {
    GtkWidget *notify, *delay_label, *delay, *interval_label, *interval;
    GtkWidget *critical_after, *minutes;
    GtkWidget *run_program, *program_button, *program_label;
    char program[sizeof(((xd_ups_settings *)nullptr)->program)];
} configure_dialog;

static void update_configure(configure_dialog *configure) {
    bool is_notify = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure->notify));
    for (GtkWidget *const *widget = (GtkWidget *const[]){configure->delay_label, configure->delay,
                                                          configure->interval_label,
                                                          configure->interval, nullptr};
         *widget != nullptr; widget++)
        gtk_widget_set_sensitive(*widget, is_notify);
    gtk_widget_set_sensitive(
        configure->minutes,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure->critical_after)));
    gtk_widget_set_sensitive(
        configure->program_button,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure->run_program)));
    gtk_label_set_text(GTK_LABEL(configure->program_label), configure->program);
}

static void on_configure_toggled([[maybe_unused]] GtkToggleButton *button, gpointer data) {
    update_configure(data);
}

static void on_browse([[maybe_unused]] GtkButton *button, gpointer entry) {
    GtkFileChooserNative *chooser =
        gtk_file_chooser_native_new("Browse", GTK_WINDOW(gtk_widget_get_toplevel(entry)),
                                    GTK_FILE_CHOOSER_ACTION_OPEN, "Open", "Cancel");
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        char *quoted = g_shell_quote(path);
        gtk_entry_set_text(GTK_ENTRY(entry), quoted);
        g_free(quoted);
        g_free(path);
    }
    g_object_unref(chooser);
}

/* The program's Configure...: Windows opened a scheduled task's
 * properties; here its command line. */
static void on_program([[maybe_unused]] GtkButton *button, gpointer data) {
    configure_dialog *configure = data;
    GtkWidget *dialog = new_dialog(GTK_WINDOW(gtk_widget_get_toplevel(configure->program_button)),
                                   "UPS Critical Alarm Program", "OK");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *label = new_left_label("_Run:");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
    gtk_entry_set_max_length(GTK_ENTRY(entry), sizeof(configure->program) - 1);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_text(GTK_ENTRY(entry), configure->program);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
    GtkWidget *browse = gtk_button_new_with_mnemonic("_Browse...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), entry);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE,
                       0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        g_strlcpy(configure->program, gtk_entry_get_text(GTK_ENTRY(entry)),
                  sizeof(configure->program));
        g_strstrip(configure->program);
        update_configure(configure);
    }
    gtk_widget_destroy(dialog);
}

static GtkWidget *new_spin(int minimum, int maximum, int value) {
    GtkWidget *spin = gtk_spin_button_new_with_range(minimum, maximum, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
    return spin;
}

static int get_spin(GtkWidget *spin) {
    return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
}

/* A label and its spin button, in a grid row. */
static GtkWidget *attach_spin(GtkWidget *grid, int row, const char *text, GtkWidget *spin,
                              int indent) {
    GtkWidget *label = new_left_label(text);
    gtk_widget_set_margin_start(label, indent);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), spin);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, row, 1, 1);
    return label;
}

/* Configure...: XP's UPS Configuration. */
static void on_configure([[maybe_unused]] GtkButton *button, gpointer data) {
    xd_power_ups_tab *tab = data;
    const xd_ups_settings *settings = &tab->settings;
    GtkWidget *dialog = new_dialog(tab->window, "UPS Configuration", "OK");
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    configure_dialog configure = {0};
    g_strlcpy(configure.program, settings->program, sizeof(configure.program));

    GtkWidget *notifications = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(notifications), 6);
    gtk_grid_set_column_spacing(GTK_GRID(notifications), 12);
    configure.notify = gtk_check_button_new_with_mnemonic("_Enable all notifications.");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configure.notify), settings->notify);
    gtk_grid_attach(GTK_GRID(notifications), configure.notify, 0, 0, 2, 1);
    configure.delay = new_spin(XD_UPS_DELAY_MIN, XD_UPS_DELAY_MAX, settings->notify_delay);
    configure.delay_label =
        attach_spin(notifications, 1, "_Seconds between power failure and first notification:",
                    configure.delay, 20);
    configure.interval =
        new_spin(XD_UPS_INTERVAL_MIN, XD_UPS_INTERVAL_MAX, settings->notify_interval);
    configure.interval_label = attach_spin(
        notifications, 2, "Seconds between su_bsequent power failure notifications:",
        configure.interval, 20);
    gtk_box_pack_start(GTK_BOX(page), notifications, FALSE, FALSE, 0);

    GtkWidget *critical = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(critical), 6);
    gtk_grid_set_column_spacing(GTK_GRID(critical), 12);
    GtkWidget *note = gtk_label_new("A critical alarm occurs when the UPS battery is almost "
                                    "exhausted or, if specified, after a specified time on "
                                    "battery power.");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 56);
    gtk_grid_attach(GTK_GRID(critical), note, 0, 0, 2, 1);
    configure.critical_after =
        gtk_check_button_new_with_mnemonic("_Minutes on battery before critical alarm:");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configure.critical_after),
                                 settings->critical_after);
    configure.minutes =
        new_spin(XD_UPS_MINUTES_MIN, XD_UPS_MINUTES_MAX, settings->critical_minutes);
    gtk_widget_set_hexpand(configure.critical_after, TRUE);
    gtk_grid_attach(GTK_GRID(critical), configure.critical_after, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(critical), configure.minutes, 1, 1, 1, 1);

    configure.run_program =
        gtk_check_button_new_with_mnemonic("_When the alarm occurs, run this program:");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configure.run_program),
                                 settings->run_program);
    gtk_grid_attach(GTK_GRID(critical), configure.run_program, 0, 2, 2, 1);
    GtkWidget *program_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(program_row, 20);
    configure.program_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(configure.program_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(configure.program_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(configure.program_label), TRUE);
    configure.program_button = gtk_button_new_with_mnemonic("C_onfigure...");
    g_signal_connect(configure.program_button, "clicked", G_CALLBACK(on_program), &configure);
    gtk_box_pack_start(GTK_BOX(program_row), configure.program_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(program_row), configure.program_button, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(critical), program_row, 0, 3, 2, 1);

    GtkWidget *action_label = new_left_label("_Next, instruct the computer to:");
    GtkWidget *action = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(action), xd_ups_action_name(XD_UPS_SHUTDOWN),
                              "Shut Down");
    if (tab->can_hibernate || settings->action == XD_UPS_HIBERNATE)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(action),
                                  xd_ups_action_name(XD_UPS_HIBERNATE), "Hibernate");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(action), xd_ups_action_name(settings->action));
    gtk_label_set_mnemonic_widget(GTK_LABEL(action_label), action);
    gtk_grid_attach(GTK_GRID(critical), action_label, 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(critical), action, 1, 4, 1, 1);

    /* NUT's upsmon, as root, turns the UPS off (its POWERDOWNFLAG and
     * the shutdown script's upsdrvctl shutdown); a user's desktop can't. */
    GtkWidget *turn_off = gtk_check_button_new_with_mnemonic("_Finally, turn off the UPS");
    gtk_widget_set_sensitive(turn_off, FALSE);
    gtk_widget_set_has_tooltip(turn_off, TRUE);
    gtk_widget_set_tooltip_text(turn_off,
                                "Turning off the UPS needs root: NUT's upsmon does it as the "
                                "computer shuts down, when its POWERDOWNFLAG is set "
                                "(upsmon.conf).");
    gtk_grid_attach(GTK_GRID(critical), turn_off, 0, 5, 2, 1);
    gtk_box_pack_start(GTK_BOX(page), new_frame("Critical alarm", critical), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), page, TRUE, TRUE,
                       0);
    for (GtkWidget *const *toggle = (GtkWidget *const[]){configure.notify,
                                                          configure.critical_after,
                                                          configure.run_program, nullptr};
         *toggle != nullptr; toggle++)
        g_signal_connect(*toggle, "toggled", G_CALLBACK(on_configure_toggled), &configure);
    update_configure(&configure);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        xd_ups_settings chosen = tab->settings;
        chosen.notify = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure.notify));
        chosen.notify_delay = get_spin(configure.delay);
        chosen.notify_interval = get_spin(configure.interval);
        chosen.critical_after =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure.critical_after));
        chosen.critical_minutes = get_spin(configure.minutes);
        chosen.run_program =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(configure.run_program));
        g_strlcpy(chosen.program, configure.program, sizeof(chosen.program));
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(action));
        chosen.action = id != nullptr && strcmp(id, xd_ups_action_name(XD_UPS_HIBERNATE)) == 0
                            ? XD_UPS_HIBERNATE
                            : XD_UPS_SHUTDOWN;
        const xd_ups_settings *was = &tab->settings;
        bool is_changed = chosen.notify != was->notify ||
                          chosen.notify_delay != was->notify_delay ||
                          chosen.notify_interval != was->notify_interval ||
                          chosen.critical_after != was->critical_after ||
                          chosen.critical_minutes != was->critical_minutes ||
                          chosen.run_program != was->run_program ||
                          strcmp(chosen.program, was->program) != 0 ||
                          chosen.action != was->action;
        if (is_changed) {
            tab->settings = chosen;
            tab->on_changed(tab->data);
        }
    }
    gtk_widget_destroy(dialog);
}

/* ---- the page -------------------------------------------------------------------- */

static GtkWidget *attach_value(GtkWidget *grid, int row, const char *what) {
    GtkWidget *key = new_left_label(what);
    GtkWidget *value = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(value), 0);
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    gtk_widget_set_can_focus(value, FALSE);
    gtk_grid_attach(GTK_GRID(grid), key, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
    return value;
}

xd_power_ups_tab *xd_power_ups_tab_new(GtkWindow *window, const xd_ups_settings *settings,
                                       bool can_hibernate, void (*on_changed)(void *data),
                                       void *data) {
    xd_power_ups_tab *tab = g_new0(xd_power_ups_tab, 1);
    tab->window = window;
    tab->settings = *settings;
    tab->can_hibernate = can_hibernate;
    tab->on_changed = on_changed;
    tab->data = data;
    tab->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(tab->page), 10);

    /* Status: the UPS's picture beside the four values. */
    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GIcon *icon = g_themed_icon_new("battery-ups");
    g_themed_icon_append_name(G_THEMED_ICON(icon), "uninterruptible-power-supply");
    g_themed_icon_append_name(G_THEMED_ICON(icon), "battery");
    GtkWidget *image = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DIALOG);
    g_object_unref(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 48);
    gtk_widget_set_valign(image, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(status), image, FALSE, FALSE, 0);
    GtkWidget *values = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(values), 4);
    gtk_grid_set_column_spacing(GTK_GRID(values), 24);
    static const char *const rows[STATUS_COUNT] = {
        [STATUS_SOURCE] = "Current power source:",
        [STATUS_RUNTIME] = "Estimated UPS runtime:",
        [STATUS_CAPACITY] = "Estimated UPS capacity:",
        [STATUS_CONDITION] = "Battery condition:",
    };
    for (int i = 0; i < STATUS_COUNT; i++)
        tab->status[i] = attach_value(values, i, rows[i]);
    gtk_box_pack_start(GTK_BOX(status), values, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab->page), new_frame("Status", status), FALSE, FALSE, 0);

    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *names = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(names), 4);
    gtk_grid_set_column_spacing(GTK_GRID(names), 24);
    tab->manufacturer = attach_value(names, 0, "Manufacturer:");
    tab->model = attach_value(names, 1, "Model:");
    gtk_box_pack_start(GTK_BOX(details), names, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    tab->configure = gtk_button_new_with_mnemonic("C_onfigure...");
    GtkWidget *select = gtk_button_new_with_mnemonic("S_elect...");
    gtk_widget_set_size_request(tab->configure, 75, -1);
    gtk_widget_set_size_request(select, 75, -1);
    g_signal_connect(tab->configure, "clicked", G_CALLBACK(on_configure), tab);
    g_signal_connect(select, "clicked", G_CALLBACK(on_select), tab);
    gtk_box_pack_start(GTK_BOX(buttons), tab->configure, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), select, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab->page), new_frame("Details", details), FALSE, FALSE, 0);

    GtkWidget *note = gtk_label_new("The desktop watches this UPS while you're logged on: it "
                                    "tells you when the power fails and shuts down or "
                                    "hibernates at the critical alarm. NUT's upsmon protects "
                                    "the computer when no one is.");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 56);
    gtk_box_pack_start(GTK_BOX(tab->page), note, FALSE, FALSE, 0);
    start_source(tab);
    return tab;
}

GtkWidget *xd_power_ups_tab_get_page(xd_power_ups_tab *tab) { return tab->page; }

void xd_power_ups_tab_get_settings(const xd_power_ups_tab *tab, xd_ups_settings *out) {
    *out = tab->settings;
}

void xd_power_ups_tab_free(xd_power_ups_tab *tab) {
    if (tab == nullptr)
        return;
    stop_source(tab);
    g_free(tab);
}
