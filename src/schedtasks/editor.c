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

static void changed(sched_editor *e) {
    if (e->changed != nullptr)
        e->changed(e->data);
}

static void on_changed([[maybe_unused]] GtkWidget *w, gpointer data) { changed(data); }

static void show_kind(sched_editor *e, xd_sched_kind kind) {
    e->kind_shown = kind;
    gtk_stack_set_visible_child_name(GTK_STACK(e->stack), page_names[kind]);
    bool timed = kind != XD_SCHED_STARTUP && kind != XD_SCHED_LOGON && kind != XD_SCHED_CUSTOM;
    gtk_widget_set_sensitive(e->time_row, timed);
}

static void on_kind(GtkComboBox *c, gpointer data) {
    sched_editor *e = data;
    /* The rows are the kinds, in order. */
    int kind = gtk_combo_box_get_active(c);
    if (kind < 0 || kind >= XD_SCHED_COUNT)
        return;
    show_kind(e, (xd_sched_kind)kind);
    changed(e);
}

static gboolean two_digits(GtkSpinButton *b, [[maybe_unused]] gpointer data) {
    char text[8];
    g_snprintf(text, sizeof(text), "%02d", gtk_spin_button_get_value_as_int(b));
    gtk_entry_set_text(GTK_ENTRY(b), text);
    return TRUE;
}

static GtkWidget *time_spin(sched_editor *e, int max) {
    GtkWidget *s = gtk_spin_button_new_with_range(0, max, 1);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(s), TRUE);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(s), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(s), 2);
    g_signal_connect(s, "output", G_CALLBACK(two_digits), nullptr);
    g_signal_connect(s, "value-changed", G_CALLBACK(on_changed), e);
    return s;
}

static GtkWidget *note(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_yalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 48);
    return l;
}

static GtkWidget *check(sched_editor *e, const char *label) {
    GtkWidget *c = gtk_check_button_new_with_label(label);
    g_signal_connect(c, "toggled", G_CALLBACK(on_changed), e);
    return c;
}

static GtkWidget *weekly_page(sched_editor *e) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), note("Every week on:"), FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (int d = 0; d < 7; d++) {
        e->days[d] = check(e, long_days[d]);
        gtk_grid_attach(GTK_GRID(grid), e->days[d], d % 4, d / 4, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

static void on_day_kind([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    sched_editor *e = data;
    gtk_widget_set_sensitive(e->day,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->day_radio)));
    changed(e);
}

static GtkWidget *monthly_page(sched_editor *e) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    e->day_radio = gtk_radio_button_new_with_mnemonic(nullptr, "_Day");
    e->day = gtk_spin_button_new_with_range(1, 31, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(e->day), TRUE);
    g_signal_connect(e->day, "value-changed", G_CALLBACK(on_changed), e);
    gtk_box_pack_start(GTK_BOX(row), e->day_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), e->day, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("of the month(s)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    e->last_radio = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(e->day_radio), "The _last day of the month(s)");
    gtk_box_pack_start(GTK_BOX(box), e->last_radio, FALSE, FALSE, 0);
    g_signal_connect(e->day_radio, "toggled", G_CALLBACK(on_day_kind), e);
    gtk_box_pack_start(GTK_BOX(box), note("In these months:"), FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (int m = 0; m < 12; m++) {
        e->months[m] = check(e, short_months[m]);
        gtk_grid_attach(GTK_GRID(grid), e->months[m], m % 6, m / 6, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *once_page(sched_editor *e) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), note("Start date:"), FALSE, FALSE, 0);
    e->calendar = gtk_calendar_new();
    gtk_calendar_set_display_options(
        GTK_CALENDAR(e->calendar),
        (GtkCalendarDisplayOptions)(GTK_CALENDAR_SHOW_HEADING | GTK_CALENDAR_SHOW_DAY_NAMES));
    gtk_widget_set_halign(e->calendar, GTK_ALIGN_START);
    g_signal_connect(e->calendar, "day-selected", G_CALLBACK(on_changed), e);
    gtk_box_pack_start(GTK_BOX(box), e->calendar, FALSE, FALSE, 0);
    return box;
}

GtkWidget *sched_editor_new(sched_editor *e, bool with_kind, void (*on_change)(void *),
                            void *data) {
    *e = (sched_editor){.changed = nullptr, .data = data};
    e->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    if (with_kind) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *l = gtk_label_new_with_mnemonic("Schedule _Task:");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        e->kind = gtk_combo_box_text_new();
        for (int k = 0; k < XD_SCHED_CUSTOM; k++) {
            char id[4];
            g_snprintf(id, sizeof(id), "%d", k);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(e->kind), id, kind_labels[k]);
        }
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), e->kind);
        gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(col), e->kind, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(top), col, FALSE, FALSE, 0);
    }
    e->time_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *l = gtk_label_new_with_mnemonic("Start t_ime:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    GtkWidget *hm = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    e->hour = time_spin(e, 23);
    e->minute = time_spin(e, 59);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), e->hour);
    gtk_box_pack_start(GTK_BOX(hm), e->hour, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hm), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hm), e->minute, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(e->time_row), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(e->time_row), hm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), e->time_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(e->box), top, FALSE, FALSE, 0);

    e->stack = gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(e->stack), FALSE);
    gtk_stack_add_named(GTK_STACK(e->stack), note("The task runs every day at the start time."),
                        page_names[XD_SCHED_DAILY]);
    gtk_stack_add_named(GTK_STACK(e->stack), weekly_page(e), page_names[XD_SCHED_WEEKLY]);
    gtk_stack_add_named(GTK_STACK(e->stack), monthly_page(e), page_names[XD_SCHED_MONTHLY]);
    gtk_stack_add_named(GTK_STACK(e->stack), once_page(e), page_names[XD_SCHED_ONCE]);
    gtk_stack_add_named(GTK_STACK(e->stack),
                        note("The task runs when your user's service manager starts: at your "
                             "first logon after the computer starts."),
                        page_names[XD_SCHED_STARTUP]);
    gtk_stack_add_named(GTK_STACK(e->stack), note("The task runs each time you log on."),
                        page_names[XD_SCHED_LOGON]);
    e->custom = note("");
    gtk_label_set_selectable(GTK_LABEL(e->custom), TRUE);
    gtk_stack_add_named(GTK_STACK(e->stack), e->custom, page_names[XD_SCHED_CUSTOM]);
    gtk_box_pack_start(GTK_BOX(e->box), e->stack, FALSE, FALSE, 0);
    if (e->kind != nullptr)
        g_signal_connect(e->kind, "changed", G_CALLBACK(on_kind), e);

    /* A stack only shows children that are visible themselves. */
    gtk_widget_show_all(e->box);
    xd_schedule s = {.kind = XD_SCHED_DAILY, .hour = 9, .weekdays = 1, .day = 1,
                     .months = XD_SCHED_ALL_MONTHS};
    sched_editor_set(e, &s, nullptr);
    /* Set up quietly; changes from now on are the user's. */
    e->changed = on_change;
    return e->box;
}

void sched_editor_set_kind(sched_editor *e, xd_sched_kind kind) {
    if (e->kind != nullptr) {
        if (kind == XD_SCHED_CUSTOM && !e->has_custom) {
            char id[4];
            g_snprintf(id, sizeof(id), "%d", (int)XD_SCHED_CUSTOM);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(e->kind), id,
                                      kind_labels[XD_SCHED_CUSTOM]);
            e->has_custom = true;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(e->kind), (int)kind);
    }
    show_kind(e, kind);
}

void sched_editor_set(sched_editor *e, const xd_schedule *s, const char *custom) {
    void (*saved)(void *) = e->changed;
    e->changed = nullptr;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->hour), s->hour);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->minute), s->minute);
    for (int d = 0; d < 7; d++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->days[d]), (s->weekdays & (1u << d)) != 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->day == 0 ? e->last_radio : e->day_radio),
                                 TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->day), s->day == 0 ? 1 : s->day);
    gtk_widget_set_sensitive(e->day, s->day != 0);
    for (int m = 0; m < 12; m++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(e->months[m]),
                                     (s->months & (1u << m)) != 0);
    if (s->kind == XD_SCHED_ONCE) {
        gtk_calendar_select_month(GTK_CALENDAR(e->calendar), (guint)(s->month - 1),
                                  (guint)s->year);
        gtk_calendar_select_day(GTK_CALENDAR(e->calendar), (guint)s->mday);
    }
    char *text = g_strdup_printf("This task's schedule was set up outside Scheduled Tasks:\n%s",
                                 custom != nullptr ? custom : "(none)");
    gtk_label_set_text(GTK_LABEL(e->custom), text);
    g_free(text);
    sched_editor_set_kind(e, s->kind);
    e->changed = saved;
}

void sched_editor_get(const sched_editor *e, xd_schedule *s) {
    *s = (xd_schedule){
        .kind = e->kind_shown,
        .hour = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->hour)),
        .minute = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->minute)),
    };
    for (int d = 0; d < 7; d++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->days[d])))
            s->weekdays |= 1u << d;
    s->day = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->last_radio))
                 ? 0
                 : gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->day));
    for (int m = 0; m < 12; m++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(e->months[m])))
            s->months |= 1u << m;
    guint y = 0, mo = 0, d = 0;
    gtk_calendar_get_date(GTK_CALENDAR(e->calendar), &y, &mo, &d);
    s->year = (int)y;
    s->month = (int)mo + 1;
    s->mday = (int)d;
}
