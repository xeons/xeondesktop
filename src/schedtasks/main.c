/* SPDX-License-Identifier: MIT
 * xd-schedtasks: Windows XP's Scheduled Tasks folder, on systemd user
 * timers (the user's own service manager, no root). A task is a
 * xd-task-NAME.timer and .service pair in $XDG_CONFIG_HOME/systemd/user;
 * the user's other timers are listed too, read-only.
 *
 * "Add Scheduled Task" opens the wizard; a task's context menu runs it,
 * ends it, deletes it or opens its Properties. Details shows XP's
 * columns: Name, Schedule, Next Run Time, Last Run Time, Status and Last
 * Result, all read from the manager over D-Bus and refreshed every few
 * seconds.
 */
#include <stdlib.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "schedtasks.h"

static constexpr GdkModifierType NO_MODS = (GdkModifierType)0;
static constexpr guint REFRESH_SECONDS = 3;

enum {
    C_ICON,
    C_NAME,
    C_SCHEDULE,
    C_NEXT,
    C_LAST,
    C_STATUS,
    C_RESULT,
    C_BASE, /* the unit base; "" for Add Scheduled Task */
    C_ADD,
    C_KEY, /* what the row shows, to leave unchanged rows alone */
    C_COUNT
};
enum { VIEW_LARGE, VIEW_LIST, VIEW_DETAILS, VIEW_COUNT };
static const char *const view_labels[VIEW_COUNT] = {"Lar_ge Icons", "_List", "_Details"};

typedef struct folder {
    sched_app app;
    GtkWidget *stack, *status;
    GtkWidget *views[VIEW_COUNT];
    GtkWidget *pages[VIEW_COUNT];
    GtkWidget *view_items[VIEW_COUNT];
    GtkWidget *run_item, *end_item, *delete_item, *props_item;
    GtkListStore *store;
    GPtrArray *tasks; /* xd_task_info, as last read */
    GHashTable *app_icons; /* program basename -> GIcon, from the installed applications */
    int view;
    bool others; /* list the user's other timers */
    bool syncing;
    bool listing;        /* a refresh is reading the manager in a thread */
    unsigned generation; /* bumped by every reload: a thread's older list is dropped */
} folder;

/* ---- the columns' text ---------------------------------------------------------------- */

static char *time_text(guint64 usec) {
    GDateTime *dt = g_date_time_new_from_unix_local((gint64)(usec / G_USEC_PER_SEC));
    char *text = dt != nullptr ? g_date_time_format(dt, "%-I:%M:%S %p %-m/%-d/%Y") : nullptr;
    if (dt != nullptr)
        g_date_time_unref(dt);
    return text != nullptr ? text : g_strdup("");
}

static char *next_text(const xd_task_info *task_info) {
    if (!task_info->enabled)
        return g_strdup("Disabled");
    if (task_info->sched.kind == XD_SCHED_STARTUP)
        return g_strdup("At system startup");
    if (task_info->sched.kind == XD_SCHED_LOGON)
        return g_strdup("At logon");
    return task_info->next_usec != 0 ? time_text(task_info->next_usec) : g_strdup("Never");
}

static const char *status_text(const xd_task_info *task_info) {
    if (task_info->running)
        return "Running";
    if (task_info->failed_start)
        return "Could not start";
    return task_info->enabled ? "Ready" : "Disabled";
}

/* XP's "0x0"; how it ended when it didn't exit on its own. */
static char *result_text(const xd_task_info *task_info) {
    if (!task_info->has_result)
        return g_strdup("");
    const char *result_code = task_info->result != nullptr ? task_info->result : "";
    if (strcmp(result_code, "timeout") == 0)
        return g_strdup("Stopped (time limit)");
    if (task_info->killed)
        return g_strdup_printf("Stopped (signal %d)", task_info->exit_status);
    return g_strdup_printf("0x%x", (unsigned)task_info->exit_status);
}

static GIcon *task_icon(folder *task_folder, const xd_task_info *task_info) {
    if (task_info->argv != nullptr && task_info->argv[0] != nullptr) {
        char *base = g_path_get_basename(task_info->argv[0]);
        GIcon *icon = g_hash_table_lookup(task_folder->app_icons, base);
        g_free(base);
        if (icon != nullptr)
            return g_object_ref(icon);
    }
    return g_themed_icon_new_with_default_fallbacks(task_info->ours ? "application-x-executable"
                                                                    : "x-office-calendar");
}

/* The icons of installed programs, by the program's file name. */
static void load_app_icons(folder *task_folder) {
    task_folder->app_icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    GList *all = g_app_info_get_all();
    for (GList *list = all; list != nullptr; list = list->next) {
        GIcon *icon = g_app_info_get_icon(list->data);
        char **argv = xd_task_argv_from_exec(g_app_info_get_commandline(list->data));
        if (icon != nullptr && argv != nullptr) {
            char *base = g_path_get_basename(argv[0]);
            if (!g_hash_table_contains(task_folder->app_icons, base))
                g_hash_table_insert(task_folder->app_icons, base, g_object_ref(icon));
            else
                g_free(base);
        }
        g_strfreev(argv);
    }
    g_list_free_full(all, g_object_unref);
}

/* ---- the list --------------------------------------------------------------------------- */

static const xd_task_info *find_task(folder *task_folder, const char *base) {
    for (guint i = 0; task_folder->tasks != nullptr && i < task_folder->tasks->len; i++) {
        const xd_task_info *task_info = g_ptr_array_index(task_folder->tasks, i);
        if (strcmp(task_info->base, base) == 0)
            return task_info;
    }
    return nullptr;
}

static void set_row(folder *task_folder, GtkTreeIter *iter, const xd_task_info *task_info) {
    char *sched = task_info->sched.kind == XD_SCHED_CUSTOM && task_info->calendar != nullptr
                      ? g_strdup(task_info->calendar)
                      : xd_schedule_describe(&task_info->sched);
    char *next = next_text(task_info);
    char *last = task_info->last_usec != 0 ? time_text(task_info->last_usec) : g_strdup("Never");
    char *result = result_text(task_info);
    GIcon *icon = task_icon(task_folder, task_info);
    char *icon_name = g_icon_to_string(icon);
    char *key = g_strjoin("\n", task_info->name, sched, next, last, status_text(task_info), result,
                          task_info->base, icon_name != nullptr ? icon_name : "", nullptr);
    char *old = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(task_folder->store), iter, C_KEY, &old, -1);
    /* Unchanged rows aren't set again: every set redraws the row. */
    if (g_strcmp0(old, key) != 0)
        gtk_list_store_set(task_folder->store, iter, C_ICON, icon, C_NAME, task_info->name,
                           C_SCHEDULE, sched, C_NEXT, next, C_LAST, last, C_STATUS,
                           status_text(task_info), C_RESULT, result, C_BASE, task_info->base, C_ADD,
                           FALSE, C_KEY, key, -1);
    g_free(old);
    g_free(key);
    g_free(icon_name);
    g_object_unref(icon);
    g_free(result);
    g_free(last);
    g_free(next);
    g_free(sched);
}

static void update_status(folder *task_folder);

/* Shows `tasks` (taken), updating the rows in place so selection and
 * scrolling stay. */
static void show_tasks(folder *task_folder, GPtrArray *tasks) {
    GPtrArray *old = task_folder->tasks;
    task_folder->tasks = tasks;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GtkTreeModel *model = GTK_TREE_MODEL(task_folder->store);
    /* The rows first: the store is sorted by name, so a renamed row moves,
     * and walking it while setting would skip rows. Its iterators stay
     * valid as rows move. */
    GArray *rows = g_array_new(FALSE, FALSE, sizeof(GtkTreeIter));
    GtkTreeIter iter;
    for (bool valid = gtk_tree_model_get_iter_first(model, &iter); valid;
         valid = gtk_tree_model_iter_next(model, &iter))
        g_array_append_val(rows, iter);
    for (guint i = 0; i < rows->len; i++) {
        GtkTreeIter *row = &g_array_index(rows, GtkTreeIter, i);
        char *base = nullptr;
        gboolean add = FALSE;
        gtk_tree_model_get(model, row, C_BASE, &base, C_ADD, &add, -1);
        const xd_task_info *task_info = add ? nullptr : find_task(task_folder, base);
        g_free(base);
        if (add)
            continue;
        if (task_info == nullptr) {
            gtk_list_store_remove(task_folder->store, row);
        } else {
            set_row(task_folder, row, task_info);
            g_hash_table_add(seen, task_info->base);
        }
    }
    g_array_unref(rows);
    for (guint i = 0; i < task_folder->tasks->len; i++) {
        const xd_task_info *task_info = g_ptr_array_index(task_folder->tasks, i);
        if (g_hash_table_contains(seen, task_info->base))
            continue;
        gtk_list_store_append(task_folder->store, &iter);
        set_row(task_folder, &iter, task_info);
    }
    g_hash_table_unref(seen);
    if (old != nullptr)
        g_ptr_array_unref(old);
    update_status(task_folder);
}

/* Reads the tasks now: after a change, which the caller looks up next. */
static void reload(folder *task_folder) {
    task_folder->generation++;
    show_tasks(task_folder, xd_tasks_list(task_folder->app.bus, task_folder->others));
}

static void refresh(sched_app *app) { reload((folder *)app); }

/* The periodic refresh reads the manager in a thread: several calls per
 * timer, which shouldn't hold up the window. */
typedef struct listing {
    GDBusConnection *bus;
    bool others;
    unsigned generation;
} listing;

static void listing_free(listing *list_job) {
    if (list_job->bus != nullptr)
        g_object_unref(list_job->bus);
    g_free(list_job);
}

static void list_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *cancellable) {
    const listing *list_job = data;
    g_task_return_pointer(task, xd_tasks_list(list_job->bus, list_job->others),
                          (GDestroyNotify)g_ptr_array_unref);
}

static void on_listed([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    folder *task_folder = data;
    const listing *list_job = g_task_get_task_data(G_TASK(res));
    GPtrArray *tasks = g_task_propagate_pointer(G_TASK(res), nullptr);
    task_folder->listing = false;
    /* A reload since has newer news. */
    if (list_job->generation == task_folder->generation)
        show_tasks(task_folder, tasks);
    else
        g_ptr_array_unref(tasks);
}

static gboolean tick(gpointer data) {
    folder *task_folder = data;
    if (task_folder->listing)
        return G_SOURCE_CONTINUE;
    task_folder->listing = true;
    listing *list_job = g_new0(listing, 1);
    *list_job =
        (listing){task_folder->app.bus != nullptr ? g_object_ref(task_folder->app.bus) : nullptr,
                  task_folder->others, task_folder->generation};
    GTask *task = g_task_new(nullptr, nullptr, on_listed, task_folder);
    g_task_set_task_data(task, list_job, (GDestroyNotify)listing_free);
    g_task_run_in_thread(task, list_thread);
    g_object_unref(task);
    return G_SOURCE_CONTINUE;
}

/* ---- the selection --------------------------------------------------------------------- */

static GList *selected_paths(folder *task_folder) {
    GtkWidget *view_widget = task_folder->views[task_folder->view];
    if (GTK_IS_ICON_VIEW(view_widget))
        return gtk_icon_view_get_selected_items(GTK_ICON_VIEW(view_widget));
    return gtk_tree_selection_get_selected_rows(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(view_widget)), nullptr);
}

/* The selected tasks (not Add Scheduled Task), as unit bases. */
static GPtrArray *selected_bases(folder *task_folder, bool *add) {
    GPtrArray *bases = g_ptr_array_new_with_free_func(g_free);
    if (add != nullptr)
        *add = false;
    GList *sel = selected_paths(task_folder);
    for (GList *list = sel; list != nullptr; list = list->next) {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(task_folder->store), &iter, list->data))
            continue;
        char *base = nullptr;
        gboolean is_add = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(task_folder->store), &iter, C_BASE, &base, C_ADD, &is_add,
                           -1);
        if (is_add) {
            if (add != nullptr)
                *add = true;
            g_free(base);
        } else {
            g_ptr_array_add(bases, base);
        }
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    return bases;
}

static void update_status(folder *task_folder) {
    GPtrArray *bases = selected_bases(task_folder, nullptr);
    bool any = bases->len > 0, running = false, ours = any;
    for (guint i = 0; i < bases->len; i++) {
        const xd_task_info *task_info = find_task(task_folder, g_ptr_array_index(bases, i));
        running = running || (task_info != nullptr && task_info->running);
        ours = ours && task_info != nullptr && task_info->ours;
    }
    gtk_widget_set_sensitive(task_folder->run_item, any);
    gtk_widget_set_sensitive(task_folder->end_item, running);
    gtk_widget_set_sensitive(task_folder->delete_item, ours);
    gtk_widget_set_sensitive(task_folder->props_item, bases->len == 1);
    guint count = bases->len;
    char *text = count > 0
                     ? g_strdup_printf("%u object(s) selected", count)
                     : g_strdup_printf("%u object(s)",
                                       task_folder->tasks != nullptr ? task_folder->tasks->len : 0);
    gtk_label_set_text(GTK_LABEL(task_folder->status), text);
    g_free(text);
    g_ptr_array_unref(bases);
}

/* ---- commands ----------------------------------------------------------------------------- */

static void cb_add([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *task_folder = data;
    sched_wizard_open(&task_folder->app);
}

static void open_props(sched_app *app, const char *base) {
    folder *task_folder = (folder *)app;
    reload(task_folder);
    const xd_task_info *task_info = find_task(task_folder, base);
    if (task_info == nullptr)
        return;
    sched_props_run(app, task_info);
    reload(task_folder);
}

static void cb_props([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *task_folder = data;
    GPtrArray *bases = selected_bases(task_folder, nullptr);
    if (bases->len == 1)
        open_props(&task_folder->app, g_ptr_array_index(bases, 0));
    g_ptr_array_unref(bases);
}

typedef bool (*task_action)(GDBusConnection *bus, const xd_task_info *info, GError **error);

static void for_selected(folder *task_folder, task_action action, const char *what) {
    GPtrArray *bases = selected_bases(task_folder, nullptr);
    for (guint i = 0; i < bases->len; i++) {
        const xd_task_info *task_info = find_task(task_folder, g_ptr_array_index(bases, i));
        GError *error = nullptr;
        if (task_info != nullptr && !action(task_folder->app.bus, task_info, &error)) {
            char *msg = g_strdup_printf("The task \"%s\" could not be %s.", task_info->name, what);
            sched_error(GTK_WINDOW(task_folder->app.window), msg, error);
            g_free(msg);
            g_error_free(error);
        }
    }
    g_ptr_array_unref(bases);
    reload(task_folder);
}

static void cb_run([[maybe_unused]] GtkWidget *widget, gpointer data) {
    for_selected(data, xd_tasks_run, "run");
}

static void cb_end([[maybe_unused]] GtkWidget *widget, gpointer data) {
    for_selected(data, xd_tasks_end, "ended");
}

static void cb_delete([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *task_folder = data;
    if (!gtk_widget_get_sensitive(task_folder->delete_item))
        return;
    GPtrArray *bases = selected_bases(task_folder, nullptr);
    if (bases->len == 0) {
        g_ptr_array_unref(bases);
        return;
    }
    const xd_task_info *first = find_task(task_folder, g_ptr_array_index(bases, 0));
    char *question = bases->len == 1 && first != nullptr
                         ? g_strdup_printf("Are you sure you want to send '%s' to the Recycle Bin?",
                                           first->name)
                         : g_strdup_printf("Are you sure you want to send these %u items to the "
                                           "Recycle Bin?",
                                           bases->len);
    int answer = xd_message_box(GTK_WINDOW(task_folder->app.window), "Confirm File Delete",
                                XD_MB_QUESTION, XD_MB_YES_NO, TRUE, question);
    g_free(question);
    g_ptr_array_unref(bases);
    if (answer == GTK_RESPONSE_YES)
        for_selected(task_folder, xd_tasks_delete, "deleted");
}

static void cb_close([[maybe_unused]] GtkWidget *widget, gpointer data) {
    gtk_widget_destroy(((folder *)data)->app.window);
}

static void cb_refresh([[maybe_unused]] GtkWidget *widget, gpointer data) { reload(data); }

static void on_others(GtkCheckMenuItem *item, gpointer data) {
    folder *task_folder = data;
    task_folder->others = gtk_check_menu_item_get_active(item);
    reload(task_folder);
}

/* Double-click: the wizard on Add Scheduled Task, Properties on a task. */
static void open_path(folder *task_folder, GtkTreePath *path) {
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(task_folder->store), &iter, path))
        return;
    char *base = nullptr;
    gboolean add = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(task_folder->store), &iter, C_BASE, &base, C_ADD, &add, -1);
    if (add)
        sched_wizard_open(&task_folder->app);
    else
        open_props(&task_folder->app, base);
    g_free(base);
}

static void cb_open([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *task_folder = data;
    bool add = false;
    GPtrArray *bases = selected_bases(task_folder, &add);
    if (add)
        sched_wizard_open(&task_folder->app);
    else if (bases->len == 1)
        open_props(&task_folder->app, g_ptr_array_index(bases, 0));
    g_ptr_array_unref(bases);
}

static void on_icon_activated([[maybe_unused]] GtkIconView *icon_view, GtkTreePath *path,
                              gpointer data) {
    open_path(data, path);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tree_view, GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    open_path(data, path);
}

/* ---- the context menu --------------------------------------------------------------------- */

static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static GtkWidget *popup_item(GtkWidget *menu, const char *label, GCallback cb, folder *task_folder,
                             bool sensitive) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, task_folder);
    gtk_widget_set_sensitive(item, sensitive);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static void popup(folder *task_folder, GdkEventButton *ev, bool add) {
    GtkWidget *menu = gtk_menu_new();
    if (add) {
        GtkWidget *open = popup_item(menu, "_Open", G_CALLBACK(cb_add), task_folder, true);
        gtk_style_context_add_class(gtk_widget_get_style_context(open), "default");
    } else {
        popup_item(menu, "R_un", G_CALLBACK(cb_run), task_folder,
                   gtk_widget_get_sensitive(task_folder->run_item));
        popup_item(menu, "_End Task", G_CALLBACK(cb_end), task_folder,
                   gtk_widget_get_sensitive(task_folder->end_item));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        popup_item(menu, "_Delete", G_CALLBACK(cb_delete), task_folder,
                   gtk_widget_get_sensitive(task_folder->delete_item));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        popup_item(menu, "P_roperties", G_CALLBACK(cb_props), task_folder,
                   gtk_widget_get_sensitive(task_folder->props_item));
    }
    gtk_menu_attach_to_widget(GTK_MENU(menu), task_folder->views[task_folder->view], nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

static gboolean on_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    folder *task_folder = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    /* Select what's under the pointer first, as Explorer does. */
    GtkTreePath *path = nullptr;
    if (GTK_IS_ICON_VIEW(widget)) {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget), (int)ev->x, (int)ev->y);
        if (path != nullptr && !gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), path)) {
            gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
            gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
        }
    } else if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path,
                                             nullptr, nullptr, nullptr)) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
        }
    }
    if (path == nullptr)
        return FALSE;
    gtk_tree_path_free(path);
    bool add = false;
    GPtrArray *bases = selected_bases(task_folder, &add);
    g_ptr_array_unref(bases);
    popup(task_folder, ev, add);
    return TRUE;
}

/* ---- the views ------------------------------------------------------------------------------ */

static void on_selection_changed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    update_status(data);
}

static void on_tree_selection([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    update_status(data);
}

static GtkWidget *icon_view(folder *task_folder, bool large) {
    GtkWidget *view_widget = gtk_icon_view_new_with_model(GTK_TREE_MODEL(task_folder->store));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(view_widget), GTK_SELECTION_MULTIPLE);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "stock-size", large ? GTK_ICON_SIZE_DIALOG : GTK_ICON_SIZE_MENU, nullptr);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    if (large) {
        g_object_set(text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 90, "alignment",
                     PANGO_ALIGN_CENTER, "xalign", 0.5, nullptr);
        gtk_icon_view_set_item_width(GTK_ICON_VIEW(view_widget), 96);
    } else {
        g_object_set(text, "xalign", 0.0, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 28,
                     nullptr);
        gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(view_widget), GTK_ORIENTATION_HORIZONTAL);
        gtk_icon_view_set_item_padding(GTK_ICON_VIEW(view_widget), 1);
        gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(view_widget), 0);
    }
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(view_widget), pix, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(view_widget), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(view_widget), pix, "gicon", C_ICON);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(view_widget), text, "text", C_NAME);
    g_signal_connect(view_widget, "item-activated", G_CALLBACK(on_icon_activated), task_folder);
    g_signal_connect(view_widget, "selection-changed", G_CALLBACK(on_selection_changed),
                     task_folder);
    g_signal_connect(view_widget, "button-press-event", G_CALLBACK(on_button), task_folder);
    return view_widget;
}

static void text_column(GtkWidget *tree, const char *title, int col, int width, bool icon) {
    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(column, title);
    if (icon) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(column, pix, FALSE);
        gtk_tree_view_column_add_attribute(column, pix, "gicon", C_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(column, text, TRUE);
    gtk_tree_view_column_add_attribute(column, text, "text", col);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
}

static GtkWidget *details_view(folder *task_folder) {
    GtkWidget *view_widget = gtk_tree_view_new_with_model(GTK_TREE_MODEL(task_folder->store));
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(view_widget), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(view_widget), C_NAME);
    text_column(view_widget, "Name", C_NAME, 170, true);
    text_column(view_widget, "Schedule", C_SCHEDULE, 230, false);
    text_column(view_widget, "Next Run Time", C_NEXT, 150, false);
    text_column(view_widget, "Last Run Time", C_LAST, 150, false);
    text_column(view_widget, "Status", C_STATUS, 90, false);
    text_column(view_widget, "Last Result", C_RESULT, 90, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view_widget));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_tree_selection), task_folder);
    g_signal_connect(view_widget, "row-activated", G_CALLBACK(on_row_activated), task_folder);
    g_signal_connect(view_widget, "button-press-event", G_CALLBACK(on_button), task_folder);
    return view_widget;
}

static void set_view(folder *task_folder, int view) {
    /* Keep the selection across views. */
    GList *sel = selected_paths(task_folder);
    task_folder->view = view;
    gtk_stack_set_visible_child(GTK_STACK(task_folder->stack), task_folder->pages[view]);
    GtkWidget *view_widget = task_folder->views[view];
    if (GTK_IS_ICON_VIEW(view_widget))
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(view_widget));
    else
        gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(view_widget)));
    for (GList *list = sel; list != nullptr; list = list->next) {
        if (GTK_IS_ICON_VIEW(view_widget))
            gtk_icon_view_select_path(GTK_ICON_VIEW(view_widget), list->data);
        else
            gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(view_widget)),
                                           list->data);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    task_folder->syncing = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(task_folder->view_items[view]), TRUE);
    task_folder->syncing = false;
    update_status(task_folder);
}

static void on_view_item(GtkCheckMenuItem *item, gpointer data) {
    folder *task_folder = data;
    if (task_folder->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_view(task_folder, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-view")));
}

/* ---- menus ------------------------------------------------------------------------------------ */

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, folder *task_folder,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, task_folder);
    if (key != 0)
        gtk_widget_add_accelerator(item, "activate", accel, key, mods, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static GtkWidget *submenu(GtkWidget *bar, const char *label) {
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    return menu;
}

static void separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static GtkWidget *build_menus(folder *task_folder, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    menu_item(file, "_Open", G_CALLBACK(cb_open), task_folder, accel, 0, NO_MODS);
    GtkWidget *new_menu = gtk_menu_new();
    GtkWidget *new_item = gtk_menu_item_new_with_mnemonic("Ne_w");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(new_item), new_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), new_item);
    menu_item(new_menu, "_Scheduled Task", G_CALLBACK(cb_add), task_folder, accel, 0, NO_MODS);
    separator(file);
    task_folder->run_item =
        menu_item(file, "R_un", G_CALLBACK(cb_run), task_folder, accel, 0, NO_MODS);
    task_folder->end_item =
        menu_item(file, "_End Task", G_CALLBACK(cb_end), task_folder, accel, 0, NO_MODS);
    separator(file);
    task_folder->delete_item = menu_item(file, "_Delete", G_CALLBACK(cb_delete), task_folder, accel,
                                         GDK_KEY_Delete, NO_MODS);
    task_folder->props_item = menu_item(file, "P_roperties", G_CALLBACK(cb_props), task_folder,
                                        accel, GDK_KEY_Return, GDK_MOD1_MASK);
    separator(file);
    menu_item(file, "_Close", G_CALLBACK(cb_close), task_folder, accel, GDK_KEY_w,
              GDK_CONTROL_MASK);

    GtkWidget *view = submenu(bar, "_View");
    GSList *group = nullptr;
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *radio = gtk_radio_menu_item_new_with_mnemonic(group, view_labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
        g_object_set_data(G_OBJECT(radio), "xd-view", GINT_TO_POINTER(i));
        g_signal_connect(radio, "toggled", G_CALLBACK(on_view_item), task_folder);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), radio);
        task_folder->view_items[i] = radio;
    }
    separator(view);
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), task_folder, accel, GDK_KEY_F5, NO_MODS);

    GtkWidget *adv = submenu(bar, "_Advanced");
    GtkWidget *others = gtk_check_menu_item_new_with_mnemonic("Show _Other Programs' Timers");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(others), task_folder->others);
    g_signal_connect(others, "toggled", G_CALLBACK(on_others), task_folder);
    gtk_menu_shell_append(GTK_MENU_SHELL(adv), others);
    return bar;
}

/* Add Scheduled Task first, then the tasks by name. */
static int compare_rows(GtkTreeModel *model, GtkTreeIter *left, GtkTreeIter *right,
                        [[maybe_unused]] gpointer data) {
    char *na = nullptr, *nb = nullptr;
    gboolean add_a = FALSE, add_b = FALSE;
    gtk_tree_model_get(model, left, C_NAME, &na, C_ADD, &add_a, -1);
    gtk_tree_model_get(model, right, C_NAME, &nb, C_ADD, &add_b, -1);
    int order = add_a != add_b ? (add_a ? -1 : 1)
                               : g_utf8_collate(na != nullptr ? na : "", nb != nullptr ? nb : "");
    g_free(na);
    g_free(nb);
    return order;
}

static void add_item(folder *task_folder) {
    GIcon *icon = g_themed_icon_new("appointment-new");
    g_themed_icon_append_name(G_THEMED_ICON(icon), "document-new");
    gtk_list_store_insert_with_values(task_folder->store, nullptr, 0, C_ICON, icon, C_NAME,
                                      "Add Scheduled Task", C_SCHEDULE, "", C_NEXT, "", C_LAST, "",
                                      C_STATUS, "", C_RESULT, "", C_BASE, "", C_ADD, TRUE, -1);
    g_object_unref(icon);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.ScheduledTasks");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static folder task_folder;
    task_folder.app.refresh = refresh;
    task_folder.app.open_props = open_props;
    task_folder.others = true;
    GError *error = nullptr;
    task_folder.app.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (task_folder.app.bus == nullptr) {
        g_printerr("xd-schedtasks: %s\n", error->message);
        g_error_free(error);
    }
    load_app_icons(&task_folder);

    task_folder.app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(task_folder.app.window), SCHED_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(task_folder.app.window), "x-office-calendar");
    gtk_window_set_default_size(GTK_WINDOW(task_folder.app.window), 960, 440);
    g_signal_connect(task_folder.app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(task_folder.app.window), accel);

    task_folder.store = gtk_list_store_new(
        C_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);
    gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(task_folder.store), compare_rows,
                                            nullptr, nullptr);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(task_folder.store),
                                         GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                         GTK_SORT_ASCENDING);
    add_item(&task_folder);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(task_folder.app.window), box);
    gtk_box_pack_start(GTK_BOX(box), build_menus(&task_folder, accel), FALSE, FALSE, 0);
    task_folder.stack = gtk_stack_new();
    task_folder.views[VIEW_LARGE] = icon_view(&task_folder, true);
    task_folder.views[VIEW_LIST] = icon_view(&task_folder, false);
    task_folder.views[VIEW_DETAILS] = details_view(&task_folder);
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(scroll), task_folder.views[i]);
        gtk_stack_add_named(GTK_STACK(task_folder.stack), scroll, view_labels[i]);
        task_folder.pages[i] = scroll;
    }
    gtk_box_pack_start(GTK_BOX(box), task_folder.stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    task_folder.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(task_folder.status), 0);
    gtk_widget_set_margin_start(task_folder.status, 6);
    gtk_widget_set_margin_top(task_folder.status, 2);
    gtk_widget_set_margin_bottom(task_folder.status, 2);
    gtk_box_pack_start(GTK_BOX(box), task_folder.status, FALSE, FALSE, 0);

    reload(&task_folder);
    gtk_widget_show_all(task_folder.app.window);
    set_view(&task_folder, VIEW_DETAILS);
    gtk_widget_grab_focus(task_folder.views[VIEW_DETAILS]);
    g_timeout_add_seconds(REFRESH_SECONDS, tick, &task_folder);
    gtk_main();
    if (task_folder.tasks != nullptr)
        g_ptr_array_unref(task_folder.tasks);
    g_hash_table_unref(task_folder.app_icons);
    if (task_folder.app.bus != nullptr)
        g_object_unref(task_folder.app.bus);
    return EXIT_SUCCESS;
}
