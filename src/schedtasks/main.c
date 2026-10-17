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
} folder;

/* ---- the columns' text ---------------------------------------------------------------- */

static char *time_text(guint64 usec) {
    GDateTime *dt = g_date_time_new_from_unix_local((gint64)(usec / G_USEC_PER_SEC));
    char *text = dt != nullptr ? g_date_time_format(dt, "%-I:%M:%S %p %-m/%-d/%Y") : nullptr;
    if (dt != nullptr)
        g_date_time_unref(dt);
    return text != nullptr ? text : g_strdup("");
}

static char *next_text(const xd_task_info *t) {
    if (!t->enabled)
        return g_strdup("Disabled");
    if (t->sched.kind == XD_SCHED_STARTUP)
        return g_strdup("At system startup");
    if (t->sched.kind == XD_SCHED_LOGON)
        return g_strdup("At logon");
    return t->next_usec != 0 ? time_text(t->next_usec) : g_strdup("Never");
}

static const char *status_text(const xd_task_info *t) {
    if (t->running)
        return "Running";
    if (t->failed_start)
        return "Could not start";
    return t->enabled ? "Ready" : "Disabled";
}

/* XP's "0x0"; how it ended when it didn't exit on its own. */
static char *result_text(const xd_task_info *t) {
    if (!t->has_result)
        return g_strdup("");
    const char *r = t->result != nullptr ? t->result : "";
    if (strcmp(r, "timeout") == 0)
        return g_strdup("Stopped (time limit)");
    if (t->killed)
        return g_strdup_printf("Stopped (signal %d)", t->exit_status);
    return g_strdup_printf("0x%x", (unsigned)t->exit_status);
}

static GIcon *task_icon(folder *f, const xd_task_info *t) {
    if (t->argv != nullptr && t->argv[0] != nullptr) {
        char *base = g_path_get_basename(t->argv[0]);
        GIcon *icon = g_hash_table_lookup(f->app_icons, base);
        g_free(base);
        if (icon != nullptr)
            return g_object_ref(icon);
    }
    return g_themed_icon_new_with_default_fallbacks(t->ours ? "application-x-executable"
                                                            : "x-office-calendar");
}

/* The icons of installed programs, by the program's file name. */
static void load_app_icons(folder *f) {
    f->app_icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    GList *all = g_app_info_get_all();
    for (GList *l = all; l != nullptr; l = l->next) {
        GIcon *icon = g_app_info_get_icon(l->data);
        char **argv = xd_task_argv_from_exec(g_app_info_get_commandline(l->data));
        if (icon != nullptr && argv != nullptr) {
            char *base = g_path_get_basename(argv[0]);
            if (!g_hash_table_contains(f->app_icons, base))
                g_hash_table_insert(f->app_icons, base, g_object_ref(icon));
            else
                g_free(base);
        }
        g_strfreev(argv);
    }
    g_list_free_full(all, g_object_unref);
}

/* ---- the list --------------------------------------------------------------------------- */

static const xd_task_info *find_task(folder *f, const char *base) {
    for (guint i = 0; f->tasks != nullptr && i < f->tasks->len; i++) {
        const xd_task_info *t = g_ptr_array_index(f->tasks, i);
        if (strcmp(t->base, base) == 0)
            return t;
    }
    return nullptr;
}

static void set_row(folder *f, GtkTreeIter *iter, const xd_task_info *t) {
    char *sched = t->sched.kind == XD_SCHED_CUSTOM && t->calendar != nullptr
                      ? g_strdup(t->calendar)
                      : xd_schedule_describe(&t->sched);
    char *next = next_text(t);
    char *last = t->last_usec != 0 ? time_text(t->last_usec) : g_strdup("Never");
    char *result = result_text(t);
    GIcon *icon = task_icon(f, t);
    gtk_list_store_set(f->store, iter, C_ICON, icon, C_NAME, t->name, C_SCHEDULE, sched, C_NEXT,
                       next, C_LAST, last, C_STATUS, status_text(t), C_RESULT, result, C_BASE,
                       t->base, C_ADD, FALSE, -1);
    g_object_unref(icon);
    g_free(result);
    g_free(last);
    g_free(next);
    g_free(sched);
}

static void update_status(folder *f);

/* Updates the rows in place, so selection and scrolling stay. */
static void reload(folder *f) {
    GPtrArray *old = f->tasks;
    f->tasks = xd_tasks_list(f->app.bus, f->others);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GtkTreeModel *m = GTK_TREE_MODEL(f->store);
    GtkTreeIter iter;
    bool valid = gtk_tree_model_get_iter_first(m, &iter);
    while (valid) {
        char *base = nullptr;
        gboolean add = FALSE;
        gtk_tree_model_get(m, &iter, C_BASE, &base, C_ADD, &add, -1);
        const xd_task_info *t = add ? nullptr : find_task(f, base);
        g_free(base);
        if (add) {
            valid = gtk_tree_model_iter_next(m, &iter);
        } else if (t == nullptr) {
            valid = gtk_list_store_remove(f->store, &iter);
        } else {
            set_row(f, &iter, t);
            g_hash_table_add(seen, t->base);
            valid = gtk_tree_model_iter_next(m, &iter);
        }
    }
    for (guint i = 0; i < f->tasks->len; i++) {
        const xd_task_info *t = g_ptr_array_index(f->tasks, i);
        if (g_hash_table_contains(seen, t->base))
            continue;
        gtk_list_store_append(f->store, &iter);
        set_row(f, &iter, t);
    }
    g_hash_table_unref(seen);
    if (old != nullptr)
        g_ptr_array_unref(old);
    update_status(f);
}

static void refresh(sched_app *app) { reload((folder *)app); }

static gboolean tick(gpointer data) {
    reload(data);
    return G_SOURCE_CONTINUE;
}

/* ---- the selection --------------------------------------------------------------------- */

static GList *selected_paths(folder *f) {
    GtkWidget *v = f->views[f->view];
    if (GTK_IS_ICON_VIEW(v))
        return gtk_icon_view_get_selected_items(GTK_ICON_VIEW(v));
    return gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(v)),
                                                nullptr);
}

/* The selected tasks (not Add Scheduled Task), as unit bases. */
static GPtrArray *selected_bases(folder *f, bool *add) {
    GPtrArray *bases = g_ptr_array_new_with_free_func(g_free);
    if (add != nullptr)
        *add = false;
    GList *sel = selected_paths(f);
    for (GList *l = sel; l != nullptr; l = l->next) {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(f->store), &iter, l->data))
            continue;
        char *base = nullptr;
        gboolean is_add = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(f->store), &iter, C_BASE, &base, C_ADD, &is_add, -1);
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

static void update_status(folder *f) {
    GPtrArray *bases = selected_bases(f, nullptr);
    bool any = bases->len > 0, running = false, ours = any;
    for (guint i = 0; i < bases->len; i++) {
        const xd_task_info *t = find_task(f, g_ptr_array_index(bases, i));
        running = running || (t != nullptr && t->running);
        ours = ours && t != nullptr && t->ours;
    }
    gtk_widget_set_sensitive(f->run_item, any);
    gtk_widget_set_sensitive(f->end_item, running);
    gtk_widget_set_sensitive(f->delete_item, ours);
    gtk_widget_set_sensitive(f->props_item, bases->len == 1);
    guint n = bases->len;
    char *text = n > 0 ? g_strdup_printf("%u object(s) selected", n)
                       : g_strdup_printf("%u object(s)", f->tasks != nullptr ? f->tasks->len : 0);
    gtk_label_set_text(GTK_LABEL(f->status), text);
    g_free(text);
    g_ptr_array_unref(bases);
}

/* ---- commands ----------------------------------------------------------------------------- */

static void cb_add([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    sched_wizard_open(&f->app);
}

static void open_props(sched_app *app, const char *base) {
    folder *f = (folder *)app;
    reload(f);
    const xd_task_info *t = find_task(f, base);
    if (t == nullptr)
        return;
    sched_props_run(app, t);
    reload(f);
}

static void cb_props([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    GPtrArray *bases = selected_bases(f, nullptr);
    if (bases->len == 1)
        open_props(&f->app, g_ptr_array_index(bases, 0));
    g_ptr_array_unref(bases);
}

typedef bool (*task_action)(GDBusConnection *bus, const xd_task_info *info, GError **error);

static void for_selected(folder *f, task_action action, const char *what) {
    GPtrArray *bases = selected_bases(f, nullptr);
    for (guint i = 0; i < bases->len; i++) {
        const xd_task_info *t = find_task(f, g_ptr_array_index(bases, i));
        GError *error = nullptr;
        if (t != nullptr && !action(f->app.bus, t, &error)) {
            char *msg = g_strdup_printf("The task \"%s\" could not be %s.", t->name, what);
            sched_error(GTK_WINDOW(f->app.window), msg, error);
            g_free(msg);
            g_error_free(error);
        }
    }
    g_ptr_array_unref(bases);
    reload(f);
}

static void cb_run([[maybe_unused]] GtkWidget *w, gpointer data) {
    for_selected(data, xd_tasks_run, "run");
}

static void cb_end([[maybe_unused]] GtkWidget *w, gpointer data) {
    for_selected(data, xd_tasks_end, "ended");
}

static void cb_delete([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    if (!gtk_widget_get_sensitive(f->delete_item))
        return;
    GPtrArray *bases = selected_bases(f, nullptr);
    if (bases->len == 0) {
        g_ptr_array_unref(bases);
        return;
    }
    const xd_task_info *first = find_task(f, g_ptr_array_index(bases, 0));
    char *question = bases->len == 1 && first != nullptr
                         ? g_strdup_printf("Are you sure you want to send '%s' to the Recycle Bin?",
                                           first->name)
                         : g_strdup_printf("Are you sure you want to send these %u items to the "
                                           "Recycle Bin?",
                                           bases->len);
    int answer = xd_message_box(GTK_WINDOW(f->app.window), "Confirm File Delete", XD_MB_QUESTION,
                                XD_MB_YES_NO, TRUE, question);
    g_free(question);
    g_ptr_array_unref(bases);
    if (answer == GTK_RESPONSE_YES)
        for_selected(f, xd_tasks_delete, "deleted");
}

static void cb_close([[maybe_unused]] GtkWidget *w, gpointer data) {
    gtk_widget_destroy(((folder *)data)->app.window);
}

static void cb_refresh([[maybe_unused]] GtkWidget *w, gpointer data) { reload(data); }

static void on_others(GtkCheckMenuItem *item, gpointer data) {
    folder *f = data;
    f->others = gtk_check_menu_item_get_active(item);
    reload(f);
}

/* Double-click: the wizard on Add Scheduled Task, Properties on a task. */
static void open_path(folder *f, GtkTreePath *path) {
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(f->store), &iter, path))
        return;
    char *base = nullptr;
    gboolean add = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(f->store), &iter, C_BASE, &base, C_ADD, &add, -1);
    if (add)
        sched_wizard_open(&f->app);
    else
        open_props(&f->app, base);
    g_free(base);
}

static void cb_open([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    bool add = false;
    GPtrArray *bases = selected_bases(f, &add);
    if (add)
        sched_wizard_open(&f->app);
    else if (bases->len == 1)
        open_props(&f->app, g_ptr_array_index(bases, 0));
    g_ptr_array_unref(bases);
}

static void on_icon_activated([[maybe_unused]] GtkIconView *v, GtkTreePath *path, gpointer data) {
    open_path(data, path);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *v, GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
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

static GtkWidget *popup_item(GtkWidget *menu, const char *label, GCallback cb, folder *f,
                             bool sensitive) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, f);
    gtk_widget_set_sensitive(item, sensitive);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static void popup(folder *f, GdkEventButton *ev, bool add) {
    GtkWidget *menu = gtk_menu_new();
    if (add) {
        GtkWidget *open = popup_item(menu, "_Open", G_CALLBACK(cb_add), f, true);
        gtk_style_context_add_class(gtk_widget_get_style_context(open), "default");
    } else {
        popup_item(menu, "R_un", G_CALLBACK(cb_run), f, gtk_widget_get_sensitive(f->run_item));
        popup_item(menu, "_End Task", G_CALLBACK(cb_end), f,
                   gtk_widget_get_sensitive(f->end_item));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        popup_item(menu, "_Delete", G_CALLBACK(cb_delete), f,
                   gtk_widget_get_sensitive(f->delete_item));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        popup_item(menu, "P_roperties", G_CALLBACK(cb_props), f,
                   gtk_widget_get_sensitive(f->props_item));
    }
    gtk_menu_attach_to_widget(GTK_MENU(menu), f->views[f->view], nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    folder *f = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    /* Select what's under the pointer first, as Explorer does. */
    GtkTreePath *path = nullptr;
    if (GTK_IS_ICON_VIEW(w)) {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w), (int)ev->x, (int)ev->y);
        if (path != nullptr && !gtk_icon_view_path_is_selected(GTK_ICON_VIEW(w), path)) {
            gtk_icon_view_unselect_all(GTK_ICON_VIEW(w));
            gtk_icon_view_select_path(GTK_ICON_VIEW(w), path);
        }
    } else if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path,
                                             nullptr, nullptr, nullptr)) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
        if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
        }
    }
    if (path == nullptr)
        return FALSE;
    gtk_tree_path_free(path);
    bool add = false;
    GPtrArray *bases = selected_bases(f, &add);
    g_ptr_array_unref(bases);
    popup(f, ev, add);
    return TRUE;
}

/* ---- the views ------------------------------------------------------------------------------ */

static void on_selection_changed([[maybe_unused]] GtkWidget *w, gpointer data) {
    update_status(data);
}

static void on_tree_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    update_status(data);
}

static GtkWidget *icon_view(folder *f, bool large) {
    GtkWidget *v = gtk_icon_view_new_with_model(GTK_TREE_MODEL(f->store));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(v), GTK_SELECTION_MULTIPLE);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "stock-size", large ? GTK_ICON_SIZE_DIALOG : GTK_ICON_SIZE_MENU, nullptr);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    if (large) {
        g_object_set(text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 90, "alignment",
                     PANGO_ALIGN_CENTER, "xalign", 0.5, nullptr);
        gtk_icon_view_set_item_width(GTK_ICON_VIEW(v), 96);
    } else {
        g_object_set(text, "xalign", 0.0, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 28,
                     nullptr);
        gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(v), GTK_ORIENTATION_HORIZONTAL);
        gtk_icon_view_set_item_padding(GTK_ICON_VIEW(v), 1);
        gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(v), 0);
    }
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v), pix, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(v), pix, "gicon", C_ICON);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(v), text, "text", C_NAME);
    g_signal_connect(v, "item-activated", G_CALLBACK(on_icon_activated), f);
    g_signal_connect(v, "selection-changed", G_CALLBACK(on_selection_changed), f);
    g_signal_connect(v, "button-press-event", G_CALLBACK(on_button), f);
    return v;
}

static void text_column(GtkWidget *tree, const char *title, int col, int width, bool icon) {
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, title);
    if (icon) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(c, pix, FALSE);
        gtk_tree_view_column_add_attribute(c, pix, "gicon", C_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(c, text, TRUE);
    gtk_tree_view_column_add_attribute(c, text, "text", col);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
}

static GtkWidget *details_view(folder *f) {
    GtkWidget *v = gtk_tree_view_new_with_model(GTK_TREE_MODEL(f->store));
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(v), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(v), C_NAME);
    text_column(v, "Name", C_NAME, 170, true);
    text_column(v, "Schedule", C_SCHEDULE, 230, false);
    text_column(v, "Next Run Time", C_NEXT, 150, false);
    text_column(v, "Last Run Time", C_LAST, 150, false);
    text_column(v, "Status", C_STATUS, 90, false);
    text_column(v, "Last Result", C_RESULT, 90, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(v));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_tree_selection), f);
    g_signal_connect(v, "row-activated", G_CALLBACK(on_row_activated), f);
    g_signal_connect(v, "button-press-event", G_CALLBACK(on_button), f);
    return v;
}

static void set_view(folder *f, int view) {
    /* Keep the selection across views. */
    GList *sel = selected_paths(f);
    f->view = view;
    gtk_stack_set_visible_child(GTK_STACK(f->stack), f->pages[view]);
    GtkWidget *v = f->views[view];
    if (GTK_IS_ICON_VIEW(v))
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(v));
    else
        gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(v)));
    for (GList *l = sel; l != nullptr; l = l->next) {
        if (GTK_IS_ICON_VIEW(v))
            gtk_icon_view_select_path(GTK_ICON_VIEW(v), l->data);
        else
            gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(v)),
                                           l->data);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    f->syncing = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(f->view_items[view]), TRUE);
    f->syncing = false;
    update_status(f);
}

static void on_view_item(GtkCheckMenuItem *item, gpointer data) {
    folder *f = data;
    if (f->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_view(f, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-view")));
}

/* ---- menus ------------------------------------------------------------------------------------ */

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, folder *f,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, f);
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

static GtkWidget *build_menus(folder *f, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    menu_item(file, "_Open", G_CALLBACK(cb_open), f, accel, 0, NO_MODS);
    GtkWidget *new_menu = gtk_menu_new();
    GtkWidget *new_item = gtk_menu_item_new_with_mnemonic("Ne_w");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(new_item), new_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), new_item);
    menu_item(new_menu, "_Scheduled Task", G_CALLBACK(cb_add), f, accel, 0, NO_MODS);
    separator(file);
    f->run_item = menu_item(file, "R_un", G_CALLBACK(cb_run), f, accel, 0, NO_MODS);
    f->end_item = menu_item(file, "_End Task", G_CALLBACK(cb_end), f, accel, 0, NO_MODS);
    separator(file);
    f->delete_item = menu_item(file, "_Delete", G_CALLBACK(cb_delete), f, accel, GDK_KEY_Delete,
                               NO_MODS);
    f->props_item = menu_item(file, "P_roperties", G_CALLBACK(cb_props), f, accel,
                              GDK_KEY_Return, GDK_MOD1_MASK);
    separator(file);
    menu_item(file, "_Close", G_CALLBACK(cb_close), f, accel, GDK_KEY_w, GDK_CONTROL_MASK);

    GtkWidget *view = submenu(bar, "_View");
    GSList *group = nullptr;
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *radio = gtk_radio_menu_item_new_with_mnemonic(group, view_labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
        g_object_set_data(G_OBJECT(radio), "xd-view", GINT_TO_POINTER(i));
        g_signal_connect(radio, "toggled", G_CALLBACK(on_view_item), f);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), radio);
        f->view_items[i] = radio;
    }
    separator(view);
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), f, accel, GDK_KEY_F5, NO_MODS);

    GtkWidget *adv = submenu(bar, "_Advanced");
    GtkWidget *others = gtk_check_menu_item_new_with_mnemonic("Show _Other Programs' Timers");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(others), f->others);
    g_signal_connect(others, "toggled", G_CALLBACK(on_others), f);
    gtk_menu_shell_append(GTK_MENU_SHELL(adv), others);
    return bar;
}

/* Add Scheduled Task first, then the tasks by name. */
static int compare_rows(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b,
                        [[maybe_unused]] gpointer data) {
    char *na = nullptr, *nb = nullptr;
    gboolean add_a = FALSE, add_b = FALSE;
    gtk_tree_model_get(m, a, C_NAME, &na, C_ADD, &add_a, -1);
    gtk_tree_model_get(m, b, C_NAME, &nb, C_ADD, &add_b, -1);
    int c = add_a != add_b ? (add_a ? -1 : 1)
                           : g_utf8_collate(na != nullptr ? na : "", nb != nullptr ? nb : "");
    g_free(na);
    g_free(nb);
    return c;
}

static void add_item(folder *f) {
    GIcon *icon = g_themed_icon_new("appointment-new");
    g_themed_icon_append_name(G_THEMED_ICON(icon), "document-new");
    gtk_list_store_insert_with_values(f->store, nullptr, 0, C_ICON, icon, C_NAME,
                                      "Add Scheduled Task", C_SCHEDULE, "", C_NEXT, "", C_LAST, "",
                                      C_STATUS, "", C_RESULT, "", C_BASE, "", C_ADD, TRUE, -1);
    g_object_unref(icon);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.ScheduledTasks");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static folder f;
    f.app.refresh = refresh;
    f.app.open_props = open_props;
    f.others = true;
    GError *error = nullptr;
    f.app.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (f.app.bus == nullptr) {
        g_printerr("xd-schedtasks: %s\n", error->message);
        g_error_free(error);
    }
    load_app_icons(&f);

    f.app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(f.app.window), SCHED_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(f.app.window), "x-office-calendar");
    gtk_window_set_default_size(GTK_WINDOW(f.app.window), 960, 440);
    g_signal_connect(f.app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(f.app.window), accel);

    f.store = gtk_list_store_new(C_COUNT, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_BOOLEAN);
    gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(f.store), compare_rows, nullptr,
                                            nullptr);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(f.store),
                                         GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                         GTK_SORT_ASCENDING);
    add_item(&f);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(f.app.window), box);
    gtk_box_pack_start(GTK_BOX(box), build_menus(&f, accel), FALSE, FALSE, 0);
    f.stack = gtk_stack_new();
    f.views[VIEW_LARGE] = icon_view(&f, true);
    f.views[VIEW_LIST] = icon_view(&f, false);
    f.views[VIEW_DETAILS] = details_view(&f);
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(scroll), f.views[i]);
        gtk_stack_add_named(GTK_STACK(f.stack), scroll, view_labels[i]);
        f.pages[i] = scroll;
    }
    gtk_box_pack_start(GTK_BOX(box), f.stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    f.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(f.status), 0);
    gtk_widget_set_margin_start(f.status, 6);
    gtk_widget_set_margin_top(f.status, 2);
    gtk_widget_set_margin_bottom(f.status, 2);
    gtk_box_pack_start(GTK_BOX(box), f.status, FALSE, FALSE, 0);

    reload(&f);
    gtk_widget_show_all(f.app.window);
    set_view(&f, VIEW_DETAILS);
    gtk_widget_grab_focus(f.views[VIEW_DETAILS]);
    g_timeout_add_seconds(REFRESH_SECONDS, tick, &f);
    gtk_main();
    if (f.tasks != nullptr)
        g_ptr_array_unref(f.tasks);
    g_hash_table_unref(f.app_icons);
    if (f.app.bus != nullptr)
        g_object_unref(f.app.bus);
    return EXIT_SUCCESS;
}
