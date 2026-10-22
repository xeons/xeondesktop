/* SPDX-License-Identifier: MIT
 * A task's Properties, as XP's three tabs:
 *
 *   Task       the command to run, the folder it starts in, Enabled
 *   Schedule   the schedule editor
 *   Settings   stop the task if it runs too long (RuntimeMaxSec=)
 *
 * OK and Apply rewrite both units from these. Other programs' timers are
 * shown read-only.
 */
#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "schedtasks.h"

void sched_error(GtkWindow *parent, const char *what, const GError *error) {
    char *text = error != nullptr ? g_strdup_printf("%s\n\n%s", what, error->message)
                                  : g_strdup(what);
    xd_message_box(parent, SCHED_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

char **sched_parse_command(GtkWindow *parent, const char *text) {
    char **argv = nullptr;
    GError *error = nullptr;
    if (text == nullptr || !g_shell_parse_argv(text, nullptr, &argv, &error)) {
        sched_error(parent, "The command to run could not be read.", error);
        if (error != nullptr)
            g_error_free(error);
        return nullptr;
    }
    if (strchr(argv[0], '/') == nullptr) {
        char *path = g_find_program_in_path(argv[0]);
        if (path != nullptr) {
            g_free(argv[0]);
            argv[0] = path;
        }
    }
    return argv;
}

bool sched_parse_workdir(GtkWindow *parent, const char *text, char **out) {
    *out = nullptr;
    char *dir = g_strstrip(g_strdup(text != nullptr ? text : ""));
    if (dir[0] == '\0') {
        g_free(dir);
        return true;
    }
    if (dir[0] == '~' && (dir[1] == '/' || dir[1] == '\0')) {
        char *abs = g_build_filename(g_get_home_dir(), dir + 1, nullptr);
        g_free(dir);
        dir = abs;
    }
    if (!xd_task_workdir_valid(dir) || !g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        char *msg = g_strdup_printf("The folder \"%s\" in Start in does not exist.", dir);
        sched_error(parent, msg, nullptr);
        g_free(msg);
        g_free(dir);
        return false;
    }
    *out = dir;
    return true;
}

typedef struct props {
    sched_app *app;
    GtkWidget *dialog, *apply;
    char *base, *name; /* copies: the folder may refresh its list meanwhile */
    bool ours, saved;
    GtkWidget *command, *workdir, *enabled;
    GtkWidget *summary;
    sched_editor editor;
    char *calendar;
    GtkWidget *stop, *stop_hours, *stop_minutes;
} props;

static void changed(void *data) {
    props *properties = data;
    if (properties->apply != nullptr)
        gtk_widget_set_sensitive(properties->apply, TRUE);
    xd_schedule schedule;
    sched_editor_get(&properties->editor, &schedule);
    char *text = schedule.kind == XD_SCHED_CUSTOM && properties->calendar != nullptr
                     ? g_strdup_printf("OnCalendar=%s", properties->calendar)
                     : xd_schedule_describe(&schedule);
    gtk_label_set_text(GTK_LABEL(properties->summary), text);
    g_free(text);
    gtk_widget_set_sensitive(properties->stop_hours,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->stop)));
    gtk_widget_set_sensitive(properties->stop_minutes,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->stop)));
}

static void on_changed([[maybe_unused]] GtkWidget *widget, gpointer data) { changed(data); }

static GtkWidget *label(const char *text) {
    GtkWidget *label_widget = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label_widget), 56);
    return label_widget;
}

static GtkWidget *page_box(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    return box;
}

static GtkWidget *title_row(const char *icon, const char *text) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND), FALSE,
                       FALSE, 0);
    GtkWidget *label_widget = label(nullptr);
    gtk_label_set_text(GTK_LABEL(label_widget), text);
    gtk_box_pack_start(GTK_BOX(row), label_widget, TRUE, TRUE, 0);
    return row;
}

static void on_browse([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Browse", GTK_WINDOW(properties->dialog), GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
        GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dlg), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path != nullptr) {
            const char *argv[] = {path, nullptr};
            char *command = xd_task_command_line(argv);
            gtk_entry_set_text(GTK_ENTRY(properties->command), command);
            g_free(command);
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static GtkWidget *task_page(props *properties, const xd_task_info *info) {
    GtkWidget *box = page_box();
    gtk_box_pack_start(GTK_BOX(box), title_row("x-office-calendar", info->name), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    GtkWidget *run = label("_Run:");
    properties->command = gtk_entry_new();
    gtk_widget_set_hexpand(properties->command, TRUE);
    gtk_widget_set_size_request(properties->command, 340, -1);
    char *command = xd_task_command_line((const char *const *)info->argv);
    gtk_entry_set_text(GTK_ENTRY(properties->command), command);
    g_free(command);
    gtk_label_set_mnemonic_widget(GTK_LABEL(run), properties->command);
    gtk_grid_attach(GTK_GRID(grid), run, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), properties->command, 1, 0, 1, 1);
    GtkWidget *browse = gtk_button_new_with_mnemonic("B_rowse...");
    gtk_widget_set_halign(browse, GTK_ALIGN_END);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), properties);
    gtk_grid_attach(GTK_GRID(grid), browse, 1, 1, 1, 1);
    GtkWidget *start = label("Start i_n:");
    properties->workdir = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(properties->workdir),
                       info->workdir != nullptr ? info->workdir : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(properties->workdir), "Your home folder");
    gtk_label_set_mnemonic_widget(GTK_LABEL(start), properties->workdir);
    gtk_grid_attach(GTK_GRID(grid), start, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), properties->workdir, 1, 2, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    properties->enabled =
        gtk_check_button_new_with_mnemonic("_Enabled (scheduled task runs at specified time)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->enabled), info->enabled);
    gtk_box_pack_end(GTK_BOX(box), properties->enabled, FALSE, FALSE, 0);
    if (!properties->ours)
        gtk_box_pack_end(GTK_BOX(box),
                         label("This timer wasn't made with Scheduled Tasks. Its settings are "
                               "shown here but can't be changed."),
                         FALSE, FALSE, 0);
    g_signal_connect(properties->command, "changed", G_CALLBACK(on_changed), properties);
    g_signal_connect(properties->workdir, "changed", G_CALLBACK(on_changed), properties);
    g_signal_connect(properties->enabled, "toggled", G_CALLBACK(on_changed), properties);
    return box;
}

static GtkWidget *schedule_page(props *properties, const xd_task_info *info) {
    GtkWidget *box = page_box();
    properties->summary = label("");
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_container_set_border_width(GTK_CONTAINER(properties->summary), 6);
    gtk_container_add(GTK_CONTAINER(frame), properties->summary);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    GtkWidget *editor = sched_editor_new(&properties->editor, true, nullptr, properties);
    sched_editor_set(&properties->editor, &info->sched, info->calendar);
    properties->editor.changed = changed;
    gtk_box_pack_start(GTK_BOX(box), editor, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *settings_page(props *properties, const xd_task_info *info) {
    GtkWidget *box = page_box();
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 8);
    properties->stop = gtk_check_button_new_with_mnemonic("_Stop the task if it runs for:");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->stop), info->max_run_sec > 0);
    gtk_box_pack_start(GTK_BOX(inner), properties->stop, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 24);
    guint64 max = info->max_run_sec > 0 ? info->max_run_sec : 72 * 3600;
    properties->stop_hours = gtk_spin_button_new_with_range(0, 999, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(properties->stop_hours), (double)(max / 3600));
    properties->stop_minutes = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(properties->stop_minutes), (double)(max % 3600 / 60));
    gtk_box_pack_start(GTK_BOX(row), properties->stop_hours, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("hour(s)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), properties->stop_minutes, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("minute(s)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), row, FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new("Scheduled Task Completed");
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    g_signal_connect(properties->stop, "toggled", G_CALLBACK(on_changed), properties);
    g_signal_connect(properties->stop_hours, "value-changed", G_CALLBACK(on_changed), properties);
    g_signal_connect(properties->stop_minutes, "value-changed", G_CALLBACK(on_changed), properties);
    return box;
}

static bool apply(props *properties) {
    GtkWindow *parent = GTK_WINDOW(properties->dialog);
    xd_task task = {.name = properties->name};
    sched_editor_get(&properties->editor, &task.sched);
    if (task.sched.kind == XD_SCHED_CUSTOM) {
        sched_error(parent, "Choose how often the task runs on the Schedule tab.", nullptr);
        return false;
    }
    if (!xd_schedule_valid(&task.sched)) {
        sched_error(parent,
                    task.sched.kind == XD_SCHED_WEEKLY    ? "Select at least one day of the week."
                    : task.sched.kind == XD_SCHED_MONTHLY ? "Select at least one month."
                                                          : "The schedule is not valid.",
                    nullptr);
        return false;
    }
    char **argv = sched_parse_command(parent, gtk_entry_get_text(GTK_ENTRY(properties->command)));
    char *dir = nullptr;
    if (argv == nullptr ||
        !sched_parse_workdir(parent, gtk_entry_get_text(GTK_ENTRY(properties->workdir)), &dir)) {
        g_strfreev(argv);
        return false;
    }
    task.argv = (const char *const *)argv;
    task.workdir = dir;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->stop))) {
        int limit_hours = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(properties->stop_hours));
        int limit_minutes =
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(properties->stop_minutes));
        task.max_run_sec = (guint64)limit_hours * 3600 + (guint64)limit_minutes * 60;
    }
    GError *error = nullptr;
    bool ok =
        xd_tasks_save(properties->app->bus, properties->base, &task,
                      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->enabled)), &error);
    if (!ok) {
        sched_error(parent, "The task's changes could not be saved.", error);
        g_error_free(error);
    }
    properties->saved = properties->saved || ok;
    if (ok)
        gtk_widget_set_sensitive(properties->apply, FALSE);
    g_free(dir);
    g_strfreev(argv);
    return ok;
}

bool sched_props_run(sched_app *app, const xd_task_info *info) {
    props properties = {.app = app, .ours = info->ours};
    properties.base = g_strdup(info->base);
    properties.name = g_strdup(info->name);
    properties.calendar = g_strdup(info->calendar);
    properties.dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties.dialog), info->name);
    gtk_window_set_icon_name(GTK_WINDOW(properties.dialog), "x-office-calendar");
    gtk_window_set_transient_for(GTK_WINDOW(properties.dialog), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(properties.dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(properties.dialog), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(properties.dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties.dialog), "Cancel", GTK_RESPONSE_CANCEL);
    if (properties.ours) {
        properties.apply =
            gtk_dialog_add_button(GTK_DIALOG(properties.dialog), "_Apply", GTK_RESPONSE_APPLY);
        gtk_widget_set_sensitive(properties.apply, FALSE);
    }
    gtk_dialog_set_default_response(GTK_DIALOG(properties.dialog), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    GtkWidget *pages[] = {task_page(&properties, info), schedule_page(&properties, info),
                          settings_page(&properties, info)};
    static const char *const titles[] = {"_Task", "_Schedule", "S_ettings"};
    for (size_t i = 0; i < XD_ARRAY_LEN(pages); i++) {
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), pages[i],
                                 gtk_label_new_with_mnemonic(titles[i]));
        /* Read-only: everything but the text, which can still be copied. */
        if (!properties.ours)
            gtk_widget_set_sensitive(pages[i], FALSE);
    }
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties.dialog))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties.dialog, GTK_NOTEBOOK(tabs));
    changed(&properties);
    if (properties.apply != nullptr)
        gtk_widget_set_sensitive(properties.apply, FALSE);
    gtk_widget_show_all(properties.dialog);

    for (;;) {
        int response = gtk_dialog_run(GTK_DIALOG(properties.dialog));
        if (response == GTK_RESPONSE_APPLY && properties.ours) {
            apply(&properties);
            continue;
        }
        if (response == GTK_RESPONSE_OK && properties.ours &&
            gtk_widget_get_sensitive(properties.apply) && !apply(&properties))
            continue;
        break;
    }
    gtk_widget_destroy(properties.dialog);
    g_free(properties.calendar);
    g_free(properties.name);
    g_free(properties.base);
    return properties.saved;
}
