/* SPDX-License-Identifier: MIT
 * The schedule editor: XP's Schedule tab (and the wizard's time page).
 * A start time, then what the kind needs: the days of the week, a day
 * of the month and the months, or a date.
 */
#include "../core/common.h"
#include "schedtasks.h"

static const char *const kind_labels[XD_SCHED_COUNT] = {
    [XD_SCHED_DAILY] = "Daily",
    [XD_SCHED_WEEKLY] = "Weekly",
    [XD_SCHED_MONTHLY] = "Monthly",
    [XD_SCHED_ONCE] = "Once",
    [XD_SCHED_STARTUP] = "At System Startup",
    [XD_SCHED_LOGON] = "At Logon",
    [XD_SCHED_CUSTOM] = "Custom",
};

static const char *const page_names[XD_SCHED_COUNT] = {
    [XD_SCHED_DAILY] = "daily",   [XD_SCHED_WEEKLY] = "weekly",   [XD_SCHED_MONTHLY] = "monthly",
    [XD_SCHED_ONCE] = "once",     [XD_SCHED_STARTUP] = "startup", [XD_SCHED_LOGON] = "logon",
    [XD_SCHED_CUSTOM] = "custom",
};

static const char *const long_days[7] = {"Monday", "Tuesday", "Wednesday", "Thursday",
                                         "Friday", "Saturday", "Sunday"};
static const char *const short_months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void changed(sched_editor *editor) {
    if (editor->changed != nullptr)
        editor->changed(editor->data);
}

static void on_changed([[maybe_unused]] GtkWidget *widget, gpointer data) { changed(data); }

static void show_kind(sched_editor *editor, xd_sched_kind kind) {
    editor->kind_shown = kind;
    gtk_stack_set_visible_child_name(GTK_STACK(editor->stack), page_names[kind]);
    bool timed = kind != XD_SCHED_STARTUP && kind != XD_SCHED_LOGON && kind != XD_SCHED_CUSTOM;
    gtk_widget_set_sensitive(editor->time_row, timed);
}

static void on_kind(GtkComboBox *combo, gpointer data) {
    sched_editor *editor = data;
    /* The rows are the kinds, in order. */
    int kind = gtk_combo_box_get_active(combo);
    if (kind < 0 || kind >= XD_SCHED_COUNT)
        return;
    show_kind(editor, (xd_sched_kind)kind);
    changed(editor);
}

static gboolean two_digits(GtkSpinButton *spin, [[maybe_unused]] gpointer data) {
    char text[8];
    g_snprintf(text, sizeof(text), "%02d", gtk_spin_button_get_value_as_int(spin));
    gtk_entry_set_text(GTK_ENTRY(spin), text);
    return TRUE;
}

static GtkWidget *time_spin(sched_editor *editor, int max) {
    GtkWidget *spin = gtk_spin_button_new_with_range(0, max, 1);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(spin), 2);
    g_signal_connect(spin, "output", G_CALLBACK(two_digits), nullptr);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_changed), editor);
    return spin;
}

static GtkWidget *note(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_yalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 48);
    return label;
}

static GtkWidget *check(sched_editor *editor, const char *label) {
    GtkWidget *check_button = gtk_check_button_new_with_label(label);
    g_signal_connect(check_button, "toggled", G_CALLBACK(on_changed), editor);
    return check_button;
}

static GtkWidget *weekly_page(sched_editor *editor) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), note("Every week on:"), FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (int weekday = 0; weekday < 7; weekday++) {
        editor->days[weekday] = check(editor, long_days[weekday]);
        gtk_grid_attach(GTK_GRID(grid), editor->days[weekday], weekday % 4, weekday / 4, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

static void on_day_kind([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    sched_editor *editor = data;
    gtk_widget_set_sensitive(editor->day,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->day_radio)));
    changed(editor);
}

static GtkWidget *monthly_page(sched_editor *editor) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    editor->day_radio = gtk_radio_button_new_with_mnemonic(nullptr, "_Day");
    editor->day = gtk_spin_button_new_with_range(1, 31, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(editor->day), TRUE);
    g_signal_connect(editor->day, "value-changed", G_CALLBACK(on_changed), editor);
    gtk_box_pack_start(GTK_BOX(row), editor->day_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), editor->day, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("of the month(s)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    editor->last_radio = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(editor->day_radio), "The _last day of the month(s)");
    gtk_box_pack_start(GTK_BOX(box), editor->last_radio, FALSE, FALSE, 0);
    g_signal_connect(editor->day_radio, "toggled", G_CALLBACK(on_day_kind), editor);
    gtk_box_pack_start(GTK_BOX(box), note("In these months:"), FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (int month_index = 0; month_index < 12; month_index++) {
        editor->months[month_index] = check(editor, short_months[month_index]);
        gtk_grid_attach(GTK_GRID(grid), editor->months[month_index], month_index % 6,
                        month_index / 6, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *once_page(sched_editor *editor) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), note("Start date:"), FALSE, FALSE, 0);
    editor->calendar = gtk_calendar_new();
    gtk_calendar_set_display_options(
        GTK_CALENDAR(editor->calendar),
        (GtkCalendarDisplayOptions)(GTK_CALENDAR_SHOW_HEADING | GTK_CALENDAR_SHOW_DAY_NAMES));
    gtk_widget_set_halign(editor->calendar, GTK_ALIGN_START);
    g_signal_connect(editor->calendar, "day-selected", G_CALLBACK(on_changed), editor);
    gtk_box_pack_start(GTK_BOX(box), editor->calendar, FALSE, FALSE, 0);
    return box;
}

GtkWidget *sched_editor_new(sched_editor *editor, bool with_kind, void (*on_change)(void *),
                            void *data) {
    *editor = (sched_editor){.changed = nullptr, .data = data};
    editor->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    if (with_kind) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *kind_label = gtk_label_new_with_mnemonic("Schedule _Task:");
        gtk_label_set_xalign(GTK_LABEL(kind_label), 0);
        editor->kind = gtk_combo_box_text_new();
        for (int k = 0; k < XD_SCHED_CUSTOM; k++) {
            char id[4];
            g_snprintf(id, sizeof(id), "%d", k);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->kind), id, kind_labels[k]);
        }
        gtk_label_set_mnemonic_widget(GTK_LABEL(kind_label), editor->kind);
        gtk_box_pack_start(GTK_BOX(col), kind_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(col), editor->kind, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(top), col, FALSE, FALSE, 0);
    }
    editor->time_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *time_label = gtk_label_new_with_mnemonic("Start t_ime:");
    gtk_label_set_xalign(GTK_LABEL(time_label), 0);
    GtkWidget *hm = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    editor->hour = time_spin(editor, 23);
    editor->minute = time_spin(editor, 59);
    gtk_label_set_mnemonic_widget(GTK_LABEL(time_label), editor->hour);
    gtk_box_pack_start(GTK_BOX(hm), editor->hour, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hm), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hm), editor->minute, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(editor->time_row), time_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(editor->time_row), hm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), editor->time_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(editor->box), top, FALSE, FALSE, 0);

    editor->stack = gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(editor->stack), FALSE);
    gtk_stack_add_named(GTK_STACK(editor->stack),
                        note("The task runs every day at the start time."),
                        page_names[XD_SCHED_DAILY]);
    gtk_stack_add_named(GTK_STACK(editor->stack), weekly_page(editor), page_names[XD_SCHED_WEEKLY]);
    gtk_stack_add_named(GTK_STACK(editor->stack), monthly_page(editor),
                        page_names[XD_SCHED_MONTHLY]);
    gtk_stack_add_named(GTK_STACK(editor->stack), once_page(editor), page_names[XD_SCHED_ONCE]);
    gtk_stack_add_named(GTK_STACK(editor->stack),
                        note("The task runs when your user's service manager starts: at your "
                             "first logon after the computer starts."),
                        page_names[XD_SCHED_STARTUP]);
    gtk_stack_add_named(GTK_STACK(editor->stack), note("The task runs each time you log on."),
                        page_names[XD_SCHED_LOGON]);
    editor->custom = note("");
    gtk_label_set_selectable(GTK_LABEL(editor->custom), TRUE);
    gtk_stack_add_named(GTK_STACK(editor->stack), editor->custom, page_names[XD_SCHED_CUSTOM]);
    gtk_box_pack_start(GTK_BOX(editor->box), editor->stack, FALSE, FALSE, 0);
    if (editor->kind != nullptr)
        g_signal_connect(editor->kind, "changed", G_CALLBACK(on_kind), editor);

    /* A stack only shows children that are visible themselves. */
    gtk_widget_show_all(editor->box);
    xd_schedule schedule = {
        .kind = XD_SCHED_DAILY, .hour = 9, .weekdays = 1, .day = 1, .months = XD_SCHED_ALL_MONTHS};
    sched_editor_set(editor, &schedule, nullptr);
    /* Set up quietly; changes from now on are the user's. */
    editor->changed = on_change;
    return editor->box;
}

void sched_editor_set_kind(sched_editor *editor, xd_sched_kind kind) {
    if (editor->kind != nullptr) {
        if (kind == XD_SCHED_CUSTOM && !editor->has_custom) {
            char id[4];
            g_snprintf(id, sizeof(id), "%d", (int)XD_SCHED_CUSTOM);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->kind), id,
                                      kind_labels[XD_SCHED_CUSTOM]);
            editor->has_custom = true;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(editor->kind), (int)kind);
    }
    show_kind(editor, kind);
}

void sched_editor_set(sched_editor *editor, const xd_schedule *schedule, const char *custom) {
    void (*saved)(void *) = editor->changed;
    editor->changed = nullptr;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->hour), schedule->hour);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->minute), schedule->minute);
    for (int weekday = 0; weekday < 7; weekday++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->days[weekday]),
                                     (schedule->weekdays & (1u << weekday)) != 0);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(schedule->day == 0 ? editor->last_radio : editor->day_radio), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->day), schedule->day == 0 ? 1 : schedule->day);
    gtk_widget_set_sensitive(editor->day, schedule->day != 0);
    for (int month_index = 0; month_index < 12; month_index++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->months[month_index]),
                                     (schedule->months & (1u << month_index)) != 0);
    if (schedule->kind == XD_SCHED_ONCE) {
        gtk_calendar_select_month(GTK_CALENDAR(editor->calendar), (guint)(schedule->month - 1),
                                  (guint)schedule->year);
        gtk_calendar_select_day(GTK_CALENDAR(editor->calendar), (guint)schedule->mday);
    }
    char *text = g_strdup_printf("This task's schedule was set up outside Scheduled Tasks:\n%s",
                                 custom != nullptr ? custom : "(none)");
    gtk_label_set_text(GTK_LABEL(editor->custom), text);
    g_free(text);
    sched_editor_set_kind(editor, schedule->kind);
    editor->changed = saved;
}

void sched_editor_get(const sched_editor *editor, xd_schedule *schedule) {
    *schedule = (xd_schedule){
        .kind = editor->kind_shown,
        .hour = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(editor->hour)),
        .minute = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(editor->minute)),
    };
    for (int weekday = 0; weekday < 7; weekday++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->days[weekday])))
            schedule->weekdays |= 1u << weekday;
    schedule->day = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->last_radio))
                        ? 0
                        : gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(editor->day));
    for (int month_index = 0; month_index < 12; month_index++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->months[month_index])))
            schedule->months |= 1u << month_index;
    guint y = 0, mo = 0, calendar_day = 0;
    gtk_calendar_get_date(GTK_CALENDAR(editor->calendar), &y, &mo, &calendar_day);
    schedule->year = (int)y;
    schedule->month = (int)mo + 1;
    schedule->mday = (int)calendar_day;
}
