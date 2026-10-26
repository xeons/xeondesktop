/* SPDX-License-Identifier: MIT
 * XP's Scheduled Task Wizard: a program (an installed application, or
 * Browse...), a name and when to run it, the start time and days, then
 * Finish writes the task's units and enables the timer.
 */
#include <gio/gdesktopappinfo.h>

#include "../core/common.h"
#include "schedtasks.h"

enum { A_ICON, A_NAME, A_COMMAND, A_WORKDIR, A_COUNT };
enum { PAGE_WELCOME, PAGE_PROGRAM, PAGE_NAME, PAGE_TIME, PAGE_FINISH };

/* The radio buttons of "Perform this task:", in XP's order. */
static const struct {
    xd_sched_kind kind;
    const char *label;
} kinds[] = {
    {XD_SCHED_DAILY, "_Daily"},
    {XD_SCHED_WEEKLY, "_Weekly"},
    {XD_SCHED_MONTHLY, "_Monthly"},
    {XD_SCHED_ONCE, "_One time only"},
    {XD_SCHED_STARTUP, "When my _computer starts"},
    {XD_SCHED_LOGON, "When I _log on"},
};

typedef struct wizard {
    sched_app *app;
    GtkWidget *dialog, *stack, *back, *next;
    GtkWidget *pages[PAGE_FINISH + 1];
    bool complete[PAGE_FINISH + 1];
    int page;
    GtkWidget *apps;
    GtkListStore *store;
    GtkWidget *name, *kind_radios[XD_ARRAY_LEN(kinds)];
    sched_editor editor;
    GtkWidget *time_title;
    GtkWidget *summary_icon, *summary_name, *summary_when, *advanced;
    char *created; /* the new task's unit base, once Finish worked */
    bool name_edited;
} wizard;

static void set_complete(wizard *task_wizard, int page, bool complete);

static xd_sched_kind chosen_kind(wizard *task_wizard) {
    for (size_t i = 0; i < XD_ARRAY_LEN(kinds); i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(task_wizard->kind_radios[i])))
            return kinds[i].kind;
    return XD_SCHED_DAILY;
}

static bool selected_app(wizard *task_wizard, GtkTreeIter *iter) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(task_wizard->apps));
    return gtk_tree_selection_get_selected(sel, nullptr, iter);
}

static GtkWidget *heading(const char *text) {
    GtkWidget *label_widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label_widget), 60);
    return label_widget;
}

static GtkWidget *page_box(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    return box;
}

/* ---- Welcome ----------------------------------------------------------------------- */

static GtkWidget *welcome_page(void) {
    GtkWidget *box = page_box();
    GtkWidget *title = heading(nullptr);
    gtk_label_set_markup(GTK_LABEL(title),
                         "<span size='large' weight='bold'>Welcome to the Scheduled Task "
                         "Wizard</span>");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       heading("This wizard helps you schedule a task to run a program at a "
                               "time you choose: every day, every week, every month, once, "
                               "when your computer starts or when you log on."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), heading("To continue, click Next."), FALSE, FALSE, 0);
    return box;
}

/* ---- the program -------------------------------------------------------------------- */

static int compare_apps(gconstpointer left, gconstpointer right) {
    return g_utf8_collate(g_app_info_get_display_name(*(GAppInfo *const *)left),
                          g_app_info_get_display_name(*(GAppInfo *const *)right));
}

static void fill_apps(wizard *task_wizard) {
    GList *all = g_app_info_get_all();
    GPtrArray *apps = g_ptr_array_new();
    for (GList *list = all; list != nullptr; list = list->next)
        if (g_app_info_should_show(list->data) && g_app_info_get_commandline(list->data) != nullptr)
            g_ptr_array_add(apps, list->data);
    g_ptr_array_sort(apps, compare_apps);
    for (guint i = 0; i < apps->len; i++) {
        GAppInfo *info = g_ptr_array_index(apps, i);
        char **argv = xd_task_argv_from_exec(g_app_info_get_commandline(info));
        if (argv == nullptr)
            continue;
        char *command = xd_task_command_line((const char *const *)argv);
        char *dir = G_IS_DESKTOP_APP_INFO(info)
                        ? g_desktop_app_info_get_string(G_DESKTOP_APP_INFO(info), "Path")
                        : nullptr;
        gtk_list_store_insert_with_values(
            task_wizard->store, nullptr, -1, A_ICON, g_app_info_get_icon(info), A_NAME,
            g_app_info_get_display_name(info), A_COMMAND, command, A_WORKDIR, dir, -1);
        g_free(dir);
        g_free(command);
        g_strfreev(argv);
    }
    g_ptr_array_unref(apps);
    g_list_free_full(all, g_object_unref);
}

static void on_app_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    wizard *task_wizard = data;
    GtkTreeIter iter;
    bool any = selected_app(task_wizard, &iter);
    set_complete(task_wizard, PAGE_PROGRAM, any);
    /* The task is named after the program, as in XP, until it's typed. */
    if (any && !task_wizard->name_edited) {
        char *name = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(task_wizard->store), &iter, A_NAME, &name, -1);
        gtk_entry_set_text(GTK_ENTRY(task_wizard->name), name);
        task_wizard->name_edited = false;
        g_free(name);
    }
}

static void on_browse([[maybe_unused]] GtkButton *button, gpointer data) {
    wizard *task_wizard = data;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Select Program to Schedule", GTK_WINDOW(task_wizard->dialog), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Open", "Cancel");
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(chooser), TRUE);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (path != nullptr) {
            const char *argv[] = {path, nullptr};
            char *command = xd_task_command_line(argv);
            char *base = g_path_get_basename(path);
            GIcon *icon = g_themed_icon_new("application-x-executable");
            GtkTreeIter iter;
            gtk_list_store_insert_with_values(task_wizard->store, &iter, 0, A_ICON, icon, A_NAME,
                                              base, A_COMMAND, command, -1);
            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(task_wizard->apps));
            gtk_tree_selection_select_iter(sel, &iter);
            GtkTreePath *tp = gtk_tree_model_get_path(GTK_TREE_MODEL(task_wizard->store), &iter);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(task_wizard->apps), tp, nullptr, FALSE, 0,
                                         0);
            gtk_tree_path_free(tp);
            g_object_unref(icon);
            g_free(base);
            g_free(command);
            g_free(path);
        }
    }
    g_object_unref(chooser);
}

static GtkWidget *program_page(wizard *task_wizard) {
    GtkWidget *box = page_box();
    gtk_box_pack_start(GTK_BOX(box),
                       heading("Click the program you want to run. To see more programs, "
                               "click Browse."),
                       FALSE, FALSE, 0);
    task_wizard->store =
        gtk_list_store_new(A_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    fill_apps(task_wizard);
    task_wizard->apps = gtk_tree_view_new_with_model(GTK_TREE_MODEL(task_wizard->store));
    g_object_unref(task_wizard->store);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(task_wizard->apps), A_NAME);
    GtkTreeViewColumn *app_column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(app_column, "Application");
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "stock-size", GTK_ICON_SIZE_MENU, nullptr);
    gtk_tree_view_column_pack_start(app_column, pix, FALSE);
    gtk_tree_view_column_add_attribute(app_column, pix, "gicon", A_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(app_column, text, TRUE);
    gtk_tree_view_column_add_attribute(app_column, text, "text", A_NAME);
    gtk_tree_view_column_set_expand(app_column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(task_wizard->apps), app_column);
    GtkTreeViewColumn *command_column = gtk_tree_view_column_new_with_attributes(
        "Command", gtk_cell_renderer_text_new(), "text", A_COMMAND, nullptr);
    gtk_tree_view_column_set_max_width(command_column, 220);
    gtk_tree_view_append_column(GTK_TREE_VIEW(task_wizard->apps), command_column);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(task_wizard->apps)), "changed",
                     G_CALLBACK(on_app_selected), task_wizard);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), task_wizard->apps);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    GtkWidget *browse = gtk_button_new_with_mnemonic("B_rowse...");
    gtk_widget_set_halign(browse, GTK_ALIGN_END);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), task_wizard);
    gtk_box_pack_start(GTK_BOX(box), browse, FALSE, FALSE, 0);
    return box;
}

/* ---- name and kind ------------------------------------------------------------------- */

static void on_name_changed([[maybe_unused]] GtkEditable *editable, gpointer data) {
    wizard *task_wizard = data;
    task_wizard->name_edited = true;
    set_complete(task_wizard, PAGE_NAME,
                 xd_task_name_valid(gtk_entry_get_text(GTK_ENTRY(task_wizard->name))));
}

static GtkWidget *name_page(wizard *task_wizard) {
    GtkWidget *box = page_box();
    GtkWidget *widget = gtk_label_new_with_mnemonic(
        "Type a _name for this task. The task name can be the same name as the program name.");
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    task_wizard->name = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(task_wizard->name), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), task_wizard->name);
    g_signal_connect(task_wizard->name, "changed", G_CALLBACK(on_name_changed), task_wizard);
    gtk_box_pack_start(GTK_BOX(box), task_wizard->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), heading("Perform this task:"), FALSE, FALSE, 6);
    GtkWidget *group = nullptr;
    for (size_t i = 0; i < XD_ARRAY_LEN(kinds); i++) {
        task_wizard->kind_radios[i] =
            group == nullptr ? gtk_radio_button_new_with_mnemonic(nullptr, kinds[i].label)
                             : gtk_radio_button_new_with_mnemonic_from_widget(
                                   GTK_RADIO_BUTTON(group), kinds[i].label);
        group = task_wizard->kind_radios[0];
        gtk_widget_set_margin_start(task_wizard->kind_radios[i], 12);
        gtk_box_pack_start(GTK_BOX(box), task_wizard->kind_radios[i], FALSE, FALSE, 0);
    }
    return box;
}

/* ---- the time ------------------------------------------------------------------------- */

static void time_changed(void *data) {
    wizard *task_wizard = data;
    xd_schedule schedule;
    sched_editor_get(&task_wizard->editor, &schedule);
    set_complete(task_wizard, PAGE_TIME, xd_schedule_valid(&schedule));
}

static GtkWidget *time_page(wizard *task_wizard) {
    GtkWidget *box = page_box();
    task_wizard->time_title = heading("");
    gtk_box_pack_start(GTK_BOX(box), task_wizard->time_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       sched_editor_new(&task_wizard->editor, false, time_changed, task_wizard),
                       FALSE, FALSE, 0);
    return box;
}

/* ---- finishing ------------------------------------------------------------------------- */

static GtkWidget *finish_page(wizard *task_wizard) {
    GtkWidget *box = page_box();
    gtk_box_pack_start(GTK_BOX(box), heading("You have successfully scheduled the following task:"),
                       FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    task_wizard->summary_icon = gtk_image_new();
    task_wizard->summary_name = heading("");
    gtk_box_pack_start(GTK_BOX(row), task_wizard->summary_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), task_wizard->summary_name, TRUE, TRUE, 0);
    gtk_widget_set_margin_start(row, 12);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), heading("It will run:"), FALSE, FALSE, 0);
    task_wizard->summary_when = heading("");
    gtk_widget_set_margin_start(task_wizard->summary_when, 12);
    gtk_box_pack_start(GTK_BOX(box), task_wizard->summary_when, FALSE, FALSE, 0);
    task_wizard->advanced = gtk_check_button_new_with_mnemonic(
        "_Open advanced properties for this task when I click Finish.");
    gtk_box_pack_end(GTK_BOX(box), task_wizard->advanced, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), heading("Click Finish to add this task to your schedule."),
                     FALSE, FALSE, 0);
    return box;
}

static void current_schedule(wizard *task_wizard, xd_schedule *schedule) {
    sched_editor_get(&task_wizard->editor, schedule);
    schedule->kind = chosen_kind(task_wizard);
}

/* Fills a page in from the earlier ones as it's shown. */
static void prepare(wizard *task_wizard, int page) {
    if (page == PAGE_TIME) {
        xd_sched_kind kind = chosen_kind(task_wizard);
        sched_editor_set_kind(&task_wizard->editor, kind);
        static const char *const titles[] = {
            [XD_SCHED_DAILY] = "Select the time you want this task to start. It runs every day.",
            [XD_SCHED_WEEKLY] = "Select the time and the days of the week you want this task "
                                "to start.",
            [XD_SCHED_MONTHLY] = "Select the time, the day and the months you want this task "
                                 "to start.",
            [XD_SCHED_ONCE] = "Select the time and the day you want this task to start.",
        };
        gtk_label_set_text(
            GTK_LABEL(task_wizard->time_title),
            (size_t)kind < XD_ARRAY_LEN(titles) && titles[kind] != nullptr ? titles[kind] : "");
        time_changed(task_wizard);
    } else if (page == PAGE_FINISH) {
        GtkTreeIter iter;
        GIcon *icon = nullptr;
        if (selected_app(task_wizard, &iter))
            gtk_tree_model_get(GTK_TREE_MODEL(task_wizard->store), &iter, A_ICON, &icon, -1);
        gtk_image_set_from_gicon(GTK_IMAGE(task_wizard->summary_icon), icon, GTK_ICON_SIZE_DND);
        if (icon != nullptr)
            g_object_unref(icon);
        gtk_label_set_text(GTK_LABEL(task_wizard->summary_name),
                           gtk_entry_get_text(GTK_ENTRY(task_wizard->name)));
        xd_schedule schedule;
        current_schedule(task_wizard, &schedule);
        char *text = xd_schedule_describe(&schedule);
        gtk_label_set_text(GTK_LABEL(task_wizard->summary_when), text);
        g_free(text);
    }
}

/* Startup and logon tasks have no time to pick. */
static bool skips_time(wizard *task_wizard) {
    xd_sched_kind kind = chosen_kind(task_wizard);
    return kind == XD_SCHED_STARTUP || kind == XD_SCHED_LOGON;
}

static void update_buttons(wizard *task_wizard) {
    gtk_widget_set_sensitive(task_wizard->back, task_wizard->page > PAGE_WELCOME);
    gtk_widget_set_sensitive(task_wizard->next, task_wizard->complete[task_wizard->page]);
    gtk_button_set_label(GTK_BUTTON(task_wizard->next),
                         task_wizard->page == PAGE_FINISH ? "Finish" : "_Next >");
}

static void set_complete(wizard *task_wizard, int page, bool complete) {
    task_wizard->complete[page] = complete;
    if (page == task_wizard->page)
        update_buttons(task_wizard);
}

static void go(wizard *task_wizard, int page) {
    task_wizard->page = page;
    prepare(task_wizard, page);
    gtk_stack_set_visible_child(GTK_STACK(task_wizard->stack), task_wizard->pages[page]);
    update_buttons(task_wizard);
    gtk_widget_grab_focus(task_wizard->next);
}

/* Finish: writes the task's units and schedules it. */
static void finish(wizard *task_wizard) {
    GtkTreeIter iter;
    if (!selected_app(task_wizard, &iter))
        return;
    char *command = nullptr, *workdir = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(task_wizard->store), &iter, A_COMMAND, &command, A_WORKDIR,
                       &workdir, -1);
    char **argv = sched_parse_command(GTK_WINDOW(task_wizard->dialog), command);
    /* The application's own folder (Path=), when it's usable. */
    if (workdir != nullptr && !xd_task_workdir_valid(workdir)) {
        g_free(workdir);
        workdir = nullptr;
    }
    if (argv != nullptr) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(task_wizard->name));
        /* XP's default: stop it after 72 hours. */
        xd_task task = {.name = name,
                        .argv = (const char *const *)argv,
                        .workdir = workdir,
                        .max_run_sec = 72 * 3600};
        current_schedule(task_wizard, &task.sched);
        char *base = xd_tasks_new_base(name);
        GError *error = nullptr;
        if (xd_tasks_save(task_wizard->app->bus, base, &task, true, &error)) {
            task_wizard->created = base;
            base = nullptr;
        } else {
            sched_error(GTK_WINDOW(task_wizard->dialog), "The task could not be scheduled.", error);
            g_error_free(error);
        }
        g_free(base);
    }
    g_strfreev(argv);
    g_free(workdir);
    g_free(command);
}

enum { RESPONSE_BACK = 1, RESPONSE_NEXT = 2 };

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    wizard *task_wizard = data;
    if (response == RESPONSE_BACK) {
        go(task_wizard, task_wizard->page == PAGE_FINISH && skips_time(task_wizard)
                            ? PAGE_NAME
                            : task_wizard->page - 1);
        return;
    }
    if (response == RESPONSE_NEXT && task_wizard->page != PAGE_FINISH) {
        go(task_wizard, task_wizard->page == PAGE_NAME && skips_time(task_wizard)
                            ? PAGE_FINISH
                            : task_wizard->page + 1);
        return;
    }
    if (response == RESPONSE_NEXT)
        finish(task_wizard);
    bool advanced = response == RESPONSE_NEXT &&
                    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(task_wizard->advanced));
    char *created = task_wizard->created;
    sched_app *app = task_wizard->app;
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
    g_free(task_wizard);
    app->refresh(app);
    if (created != nullptr && advanced)
        app->open_props(app, created);
    g_free(created);
}

void sched_wizard_open(sched_app *app) {
    wizard *task_wizard = g_new0(wizard, 1);
    task_wizard->app = app;
    task_wizard->dialog = gtk_dialog_new();
    GtkWindow *win = GTK_WINDOW(task_wizard->dialog);
    gtk_window_set_title(win, "Scheduled Task Wizard");
    gtk_window_set_icon_name(win, "x-office-calendar");
    gtk_window_set_transient_for(win, GTK_WINDOW(app->window));
    gtk_window_set_default_size(win, 520, 400);
    task_wizard->back =
        gtk_dialog_add_button(GTK_DIALOG(task_wizard->dialog), "< _Back", RESPONSE_BACK);
    task_wizard->next =
        gtk_dialog_add_button(GTK_DIALOG(task_wizard->dialog), "_Next >", RESPONSE_NEXT);
    gtk_dialog_add_button(GTK_DIALOG(task_wizard->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(task_wizard->dialog), RESPONSE_NEXT);

    task_wizard->stack = gtk_stack_new();
    GtkWidget *pages[] = {welcome_page(), program_page(task_wizard), name_page(task_wizard),
                          time_page(task_wizard), finish_page(task_wizard)};
    for (size_t i = 0; i < XD_ARRAY_LEN(pages); i++) {
        task_wizard->pages[i] = pages[i];
        gtk_stack_add_named(GTK_STACK(task_wizard->stack), pages[i], nullptr);
    }
    task_wizard->complete[PAGE_WELCOME] = task_wizard->complete[PAGE_TIME] =
        task_wizard->complete[PAGE_FINISH] = true;
    task_wizard->name_edited = false;
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(task_wizard->dialog));
    gtk_box_pack_start(GTK_BOX(content), task_wizard->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    g_signal_connect(task_wizard->dialog, "response", G_CALLBACK(on_response), task_wizard);
    gtk_widget_show_all(task_wizard->dialog);
    go(task_wizard, PAGE_WELCOME);
}
