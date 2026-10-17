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
    props *p = data;
    if (p->apply != nullptr)
        gtk_widget_set_sensitive(p->apply, TRUE);
    xd_schedule s;
    sched_editor_get(&p->editor, &s);
    char *text = s.kind == XD_SCHED_CUSTOM && p->calendar != nullptr
                     ? g_strdup_printf("OnCalendar=%s", p->calendar)
                     : xd_schedule_describe(&s);
    gtk_label_set_text(GTK_LABEL(p->summary), text);
    g_free(text);
    gtk_widget_set_sensitive(p->stop_hours, gtk_toggle_button_get_active(
                                                GTK_TOGGLE_BUTTON(p->stop)));
    gtk_widget_set_sensitive(p->stop_minutes, gtk_toggle_button_get_active(
                                                  GTK_TOGGLE_BUTTON(p->stop)));
}

static void on_changed([[maybe_unused]] GtkWidget *w, gpointer data) { changed(data); }

static GtkWidget *label(const char *text) {
    GtkWidget *l = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
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
    GtkWidget *l = label(nullptr);
    gtk_label_set_text(GTK_LABEL(l), text);
    gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
    return row;
}

static void on_browse([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Browse", GTK_WINDOW(p->dialog),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
                                                 GTK_RESPONSE_CANCEL, "_Open",
                                                 GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dlg), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path != nullptr) {
            const char *argv[] = {path, nullptr};
            char *command = xd_task_command_line(argv);
            gtk_entry_set_text(GTK_ENTRY(p->command), command);
            g_free(command);
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static GtkWidget *task_page(props *p, const xd_task_info *info) {
    GtkWidget *box = page_box();
    gtk_box_pack_start(GTK_BOX(box), title_row("x-office-calendar", info->name), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    GtkWidget *run = label("_Run:");
    p->command = gtk_entry_new();
    gtk_widget_set_hexpand(p->command, TRUE);
    gtk_widget_set_size_request(p->command, 340, -1);
    char *command = xd_task_command_line((const char *const *)info->argv);
    gtk_entry_set_text(GTK_ENTRY(p->command), command);
    g_free(command);
    gtk_label_set_mnemonic_widget(GTK_LABEL(run), p->command);
    gtk_grid_attach(GTK_GRID(grid), run, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->command, 1, 0, 1, 1);
    GtkWidget *browse = gtk_button_new_with_mnemonic("B_rowse...");
    gtk_widget_set_halign(browse, GTK_ALIGN_END);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), p);
    gtk_grid_attach(GTK_GRID(grid), browse, 1, 1, 1, 1);
    GtkWidget *start = label("Start i_n:");
    p->workdir = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(p->workdir), info->workdir != nullptr ? info->workdir : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->workdir), "Your home folder");
    gtk_label_set_mnemonic_widget(GTK_LABEL(start), p->workdir);
    gtk_grid_attach(GTK_GRID(grid), start, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->workdir, 1, 2, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    p->enabled = gtk_check_button_new_with_mnemonic(
        "_Enabled (scheduled task runs at specified time)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->enabled), info->enabled);
    gtk_box_pack_end(GTK_BOX(box), p->enabled, FALSE, FALSE, 0);
    if (!p->ours)
        gtk_box_pack_end(GTK_BOX(box),
                         label("This timer wasn't made with Scheduled Tasks. Its settings are "
                               "shown here but can't be changed."),
                         FALSE, FALSE, 0);
    g_signal_connect(p->command, "changed", G_CALLBACK(on_changed), p);
    g_signal_connect(p->workdir, "changed", G_CALLBACK(on_changed), p);
    g_signal_connect(p->enabled, "toggled", G_CALLBACK(on_changed), p);
    return box;
}

static GtkWidget *schedule_page(props *p, const xd_task_info *info) {
    GtkWidget *box = page_box();
    p->summary = label("");
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_container_set_border_width(GTK_CONTAINER(p->summary), 6);
    gtk_container_add(GTK_CONTAINER(frame), p->summary);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    GtkWidget *editor = sched_editor_new(&p->editor, true, nullptr, p);
    sched_editor_set(&p->editor, &info->sched, info->calendar);
    p->editor.changed = changed;
    gtk_box_pack_start(GTK_BOX(box), editor, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *settings_page(props *p, const xd_task_info *info) {
    GtkWidget *box = page_box();
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 8);
    p->stop = gtk_check_button_new_with_mnemonic("_Stop the task if it runs for:");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->stop), info->max_run_sec > 0);
    gtk_box_pack_start(GTK_BOX(inner), p->stop, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 24);
    guint64 max = info->max_run_sec > 0 ? info->max_run_sec : 72 * 3600;
    p->stop_hours = gtk_spin_button_new_with_range(0, 999, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->stop_hours), (double)(max / 3600));
    p->stop_minutes = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->stop_minutes), (double)(max % 3600 / 60));
    gtk_box_pack_start(GTK_BOX(row), p->stop_hours, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("hour(s)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->stop_minutes, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("minute(s)"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inner), row, FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new("Scheduled Task Completed");
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    g_signal_connect(p->stop, "toggled", G_CALLBACK(on_changed), p);
    g_signal_connect(p->stop_hours, "value-changed", G_CALLBACK(on_changed), p);
    g_signal_connect(p->stop_minutes, "value-changed", G_CALLBACK(on_changed), p);
    return box;
}

static bool apply(props *p) {
    GtkWindow *parent = GTK_WINDOW(p->dialog);
    xd_task t = {.name = p->name};
    sched_editor_get(&p->editor, &t.sched);
    if (t.sched.kind == XD_SCHED_CUSTOM) {
        sched_error(parent, "Choose how often the task runs on the Schedule tab.", nullptr);
        return false;
    }
    if (!xd_schedule_valid(&t.sched)) {
        sched_error(parent,
                    t.sched.kind == XD_SCHED_WEEKLY    ? "Select at least one day of the week."
                    : t.sched.kind == XD_SCHED_MONTHLY ? "Select at least one month."
                                                       : "The schedule is not valid.",
                    nullptr);
        return false;
    }
    char **argv = sched_parse_command(parent, gtk_entry_get_text(GTK_ENTRY(p->command)));
    char *dir = nullptr;
    if (argv == nullptr ||
        !sched_parse_workdir(parent, gtk_entry_get_text(GTK_ENTRY(p->workdir)), &dir)) {
        g_strfreev(argv);
        return false;
    }
    t.argv = (const char *const *)argv;
    t.workdir = dir;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->stop))) {
        int h = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->stop_hours));
        int m = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->stop_minutes));
        t.max_run_sec = (guint64)h * 3600 + (guint64)m * 60;
    }
    GError *error = nullptr;
    bool ok = xd_tasks_save(p->app->bus, p->base, &t,
                            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->enabled)), &error);
    if (!ok) {
        sched_error(parent, "The task's changes could not be saved.", error);
        g_error_free(error);
    }
    p->saved = p->saved || ok;
    if (ok)
        gtk_widget_set_sensitive(p->apply, FALSE);
    g_free(dir);
    g_strfreev(argv);
    return ok;
}

bool sched_props_run(sched_app *app, const xd_task_info *info) {
    props p = {.app = app, .ours = info->ours};
    p.base = g_strdup(info->base);
    p.name = g_strdup(info->name);
    p.calendar = g_strdup(info->calendar);
    p.dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.dialog), info->name);
    gtk_window_set_icon_name(GTK_WINDOW(p.dialog), "x-office-calendar");
    gtk_window_set_transient_for(GTK_WINDOW(p.dialog), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(p.dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(p.dialog), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.dialog), "Cancel", GTK_RESPONSE_CANCEL);
    if (p.ours) {
        p.apply = gtk_dialog_add_button(GTK_DIALOG(p.dialog), "_Apply", GTK_RESPONSE_APPLY);
        gtk_widget_set_sensitive(p.apply, FALSE);
    }
    gtk_dialog_set_default_response(GTK_DIALOG(p.dialog), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    GtkWidget *pages[] = {task_page(&p, info), schedule_page(&p, info), settings_page(&p, info)};
    static const char *const titles[] = {"_Task", "_Schedule", "S_ettings"};
    for (size_t i = 0; i < XD_ARRAY_LEN(pages); i++) {
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), pages[i],
                                 gtk_label_new_with_mnemonic(titles[i]));
        /* Read-only: everything but the text, which can still be copied. */
        if (!p.ours)
            gtk_widget_set_sensitive(pages[i], FALSE);
    }
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.dialog))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.dialog, GTK_NOTEBOOK(tabs));
    changed(&p);
    if (p.apply != nullptr)
        gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_widget_show_all(p.dialog);

    for (;;) {
        int r = gtk_dialog_run(GTK_DIALOG(p.dialog));
        if (r == GTK_RESPONSE_APPLY && p.ours) {
            apply(&p);
            continue;
        }
        if (r == GTK_RESPONSE_OK && p.ours && gtk_widget_get_sensitive(p.apply) && !apply(&p))
            continue;
        break;
    }
    gtk_widget_destroy(p.dialog);
    g_free(p.calendar);
    g_free(p.name);
    g_free(p.base);
    return p.saved;
}
