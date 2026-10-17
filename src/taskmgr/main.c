/* SPDX-License-Identifier: MIT
 * xd-taskmgr: the Windows 2000 Task Manager.
 *
 * Applications lists the open windows (End Task, Switch To, New Task),
 * Processes lists every process with selectable columns (End Process,
 * End Process Tree, Set Priority, Set Affinity), and Performance shows the
 * CPU and memory meters, history graphs and totals. Ctrl+Alt+Delete and
 * Ctrl+Shift+Esc open it (handled by the compositor).
 */
#include <errno.h>
#include <gio/gdesktopappinfo.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/state.h"
#include "graph.h"
#include "sysinfo.h"
#include "tasks.h"

static constexpr GdkModifierType NO_MODS = (GdkModifierType)0;
static constexpr int HISTORY = 1024;

/* ---- Processes columns ------------------------------------------------------------ */

enum {
    PS_PID,
    PS_NAME,
    PS_USER,
    PS_CPU,
    PS_TIME,
    PS_MEM,
    PS_MEM_DELTA,
    PS_PEAK,
    PS_FAULTS,
    PS_FAULTS_DELTA,
    PS_VM,
    PS_PRIO,
    PS_THREADS,
    PS_HANDLES,
    PS_IOR,
    PS_IOW,
    PS_GEN,
    PS_COUNT,
};

typedef enum { F_TEXT, F_INT, F_PERCENT, F_TIME, F_KB, F_KB_DELTA, F_COUNT, F_PRIO } fmt;

typedef struct column_def {
    const char *key;    /* in taskmgr.ini */
    const char *title;  /* header */
    const char *choice; /* in Select Columns */
    int store;
    fmt format;
    gboolean on_by_default;
    int width; /* default, px */
} column_def;

static const column_def columns[] = {
    {"pid", "PID", nullptr, PS_PID, F_INT, TRUE, 48},
    {"name", "Image Name", nullptr, PS_NAME, F_TEXT, TRUE, 120},
    {"user", "User Name", "User Name", PS_USER, F_TEXT, FALSE, 90},
    {"cpu", "CPU", "CPU Usage", PS_CPU, F_PERCENT, TRUE, 48},
    {"time", "CPU Time", "CPU Time", PS_TIME, F_TIME, TRUE, 80},
    {"mem", "Mem Usage", "Memory Usage", PS_MEM, F_KB, TRUE, 84},
    {"memdelta", "Mem Delta", "Memory Usage Delta", PS_MEM_DELTA, F_KB_DELTA, FALSE, 84},
    {"peak", "Peak Mem Usage", "Peak Memory Usage", PS_PEAK, F_KB, FALSE, 104},
    {"faults", "Page Faults", "Page Faults", PS_FAULTS, F_COUNT, FALSE, 84},
    {"pfdelta", "PF Delta", "Page Faults Delta", PS_FAULTS_DELTA, F_COUNT, FALSE, 72},
    {"vm", "VM Size", "Virtual Memory Size", PS_VM, F_KB, FALSE, 84},
    {"prio", "Base Pri", "Base Priority", PS_PRIO, F_PRIO, FALSE, 80},
    {"handles", "Open Files", "Open Files", PS_HANDLES, F_COUNT, FALSE, 76},
    {"threads", "Threads", "Thread Count", PS_THREADS, F_COUNT, FALSE, 68},
    {"ioread", "I/O Read Bytes", "I/O Read Bytes", PS_IOR, F_COUNT, FALSE, 110},
    {"iowrite", "I/O Write Bytes", "I/O Write Bytes", PS_IOW, F_COUNT, FALSE, 110},
};
enum { NCOLUMNS = G_N_ELEMENTS(columns), FIXED_COLUMNS = 2 /* PID, Image Name */ };

/* Windows priority classes and the nice values they map to. */
static const struct {
    const char *name;
    int nice;
} priorities[] = {
    {"_Realtime", -20}, {"_High", -10},       {"_AboveNormal", -5},
    {"_Normal", 0},     {"_BelowNormal", 5}, {"_Low", 19},
};

static const char *priority_name(int nice) {
    if (nice <= -20)
        return "Realtime";
    if (nice <= -10)
        return "High";
    if (nice < 0)
        return "AboveNormal";
    if (nice == 0)
        return "Normal";
    if (nice < 10)
        return "BelowNormal";
    return "Low";
}

/* ---- state ----------------------------------------------------------------------- */

enum { AP_TASK, AP_ICON16, AP_ICON32, AP_TITLE, AP_STATUS, AP_COUNT };
enum { VIEW_LARGE, VIEW_SMALL, VIEW_DETAILS, VIEW_COUNT };
enum { SPEED_HIGH, SPEED_NORMAL, SPEED_LOW, SPEED_PAUSED, SPEED_COUNT };
static const guint speed_ms[SPEED_COUNT] = {500, 2000, 4000, 0};
enum { TAB_APPS, TAB_PROCS, TAB_PERF };
enum { UNIT_KB, UNIT_MB, UNIT_GB, UNIT_COUNT };
static const char *const unit_names[UNIT_COUNT] = {"KB", "MB", "GB"};
static int mem_unit = UNIT_MB; /* View > Memory Units */

typedef struct info_row {
    GtkWidget *label;
} info_row;

typedef struct taskmgr {
    GtkApplication *app;
    GtkWidget *window, *notebook;
    xd_sysinfo *si;
    guint timer;
    int speed;
    gboolean minimize_on_use, per_cpu, show_kernel, on_top;
    gboolean syncing;

    /* menus whose visibility follows the tab */
    GtkWidget *windows_menu, *view_sep, *view_apps[VIEW_COUNT], *view_columns, *view_cpu_history,
        *view_kernel, *speed_items[SPEED_COUNT], *cpu_history_items[2];

    /* Applications */
    xd_tasklist *tasks;
    GtkWidget *tile_h_item, *tile_v_item, *cascade_item; /* Windows menu */
    GtkWidget *on_top_item;                               /* Options > Always On Top */
    GtkListStore *apps;
    GtkWidget *apps_stack, *apps_views[VIEW_COUNT];
    int apps_view;
    GtkWidget *end_task, *switch_to;

    /* Processes */
    GtkListStore *procs;
    GtkWidget *procs_tree;
    GtkTreeViewColumn *proc_cols[NCOLUMNS];
    gboolean col_on[NCOLUMNS];
    GHashTable *rows; /* pid -> GtkTreeIter* */
    guint64 generation;
    GtkWidget *end_process;

    /* Performance */
    xd_history cpu_hist, mem_hist;
    xd_history *cpu_each;
    int ncpu;
    GtkWidget *cpu_meter, *mem_meter, *cpu_graph, *cpu_graphs_box, *mem_graph;
    double cpu_now, kernel_now, mem_now;
    char cpu_text[16], mem_text[32];
    GtkWidget *phys_frame, *commit_frame, *kern_frame;
    GtkWidget *handles, *threads, *processes, *phys_total, *phys_avail, *phys_cache, *commit_total,
        *commit_limit, *commit_peak, *kern_total, *kern_paged, *kern_nonpaged;
    guint64 peak_commit;

    /* Status bar */
    GtkWidget *st_procs, *st_cpu, *st_mem;
} taskmgr;

static void sample(taskmgr *t);

/* ---- helpers ---------------------------------------------------------------------- */

static void append_grouped(GString *out, guint64 v) {
    char digits[32];
    int n = g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, v);
    for (int i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0)
            g_string_append_c(out, ',');
        g_string_append_c(out, digits[i]);
    }
}

/* A KiB amount in the chosen unit: "16,492 KB", "16.1 MB", "1.57 GB". */
static void append_memory(GString *out, guint64 kb, gboolean with_unit) {
    if (mem_unit == UNIT_KB)
        append_grouped(out, kb);
    else {
        double v = (double)kb / (mem_unit == UNIT_MB ? 1024.0 : 1024.0 * 1024.0);
        g_string_append_printf(out, v < 10 ? "%.2f" : v < 1000 ? "%.1f" : "%.0f", v);
    }
    if (with_unit) {
        g_string_append_c(out, ' ');
        g_string_append(out, unit_names[mem_unit]);
    }
}

static void set_memory(GtkWidget *label, guint64 kb) {
    GString *s = g_string_new(nullptr);
    append_memory(s, kb, FALSE);
    gtk_label_set_text(GTK_LABEL(label), s->str);
    g_string_free(s, TRUE);
}

static void set_number(GtkWidget *label, guint64 v) {
    GString *s = g_string_new(nullptr);
    append_grouped(s, v);
    gtk_label_set_text(GTK_LABEL(label), s->str);
    g_string_free(s, TRUE);
}

static void message(taskmgr *t, xd_mb_icon icon, const char *title, const char *text) {
    xd_message_box(GTK_WINDOW(t->window), title, icon, XD_MB_OK, FALSE, text);
}

/* Destructive questions: No is the default, as in Windows. */
static gboolean confirm(taskmgr *t, const char *title, const char *text) {
    return xd_message_box(GTK_WINDOW(t->window), title, XD_MB_WARNING, XD_MB_YES_NO, TRUE, text) ==
           GTK_RESPONSE_YES;
}

/* "Access is denied." for permission errors, as Windows says it. */
static const char *errno_text(int err) {
    return err == EPERM || err == EACCES ? "Access is denied." : g_strerror(err);
}

/* Our programs live next to this one in a build tree; else use PATH. */
static char *own_program(const char *name) {
    char *exe = g_file_read_link("/proc/self/exe", nullptr);
    if (exe != nullptr) {
        char *dir = g_path_get_dirname(exe);
        char *path = g_build_filename(dir, name, nullptr);
        g_free(dir);
        g_free(exe);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
            return path;
        g_free(path);
    }
    return g_strdup(name);
}

/* ---- settings (taskmgr.ini in the state directory) -------------------------------- */

static void settings_path(xd_strbuf *out) { xd_state_path("taskmgr.ini", out); }

static void load_settings(taskmgr *t, int *tab) {
    t->speed = SPEED_NORMAL;
    t->apps_view = VIEW_DETAILS;
    for (size_t i = 0; i < NCOLUMNS; i++)
        t->col_on[i] = columns[i].on_by_default;
    *tab = TAB_APPS;
    GKeyFile *kf = g_key_file_new();
    xd_strbuf path = {0};
    settings_path(&path);
    if (g_key_file_load_from_file(kf, xd_strbuf_cstr(&path), G_KEY_FILE_NONE, nullptr)) {
        GError *err = nullptr;
        int v = g_key_file_get_integer(kf, "TaskManager", "Tab", &err);
        if (err == nullptr && v >= TAB_APPS && v <= TAB_PERF)
            *tab = v;
        g_clear_error(&err);
        v = g_key_file_get_integer(kf, "TaskManager", "UpdateSpeed", &err);
        if (err == nullptr && v >= 0 && v < SPEED_COUNT)
            t->speed = v;
        g_clear_error(&err);
        v = g_key_file_get_integer(kf, "TaskManager", "ApplicationsView", &err);
        if (err == nullptr && v >= 0 && v < VIEW_COUNT)
            t->apps_view = v;
        g_clear_error(&err);
        v = g_key_file_get_integer(kf, "TaskManager", "MemoryUnits", &err);
        if (err == nullptr && v >= 0 && v < UNIT_COUNT)
            mem_unit = v;
        g_clear_error(&err);
        t->minimize_on_use = g_key_file_get_boolean(kf, "TaskManager", "MinimizeOnUse", nullptr);
        t->on_top = g_key_file_get_boolean(kf, "TaskManager", "AlwaysOnTop", nullptr);
        t->per_cpu = g_key_file_get_boolean(kf, "TaskManager", "OneGraphPerCPU", nullptr);
        t->show_kernel = g_key_file_get_boolean(kf, "TaskManager", "ShowKernelTimes", nullptr);
        gsize n = 0;
        char **cols = g_key_file_get_string_list(kf, "TaskManager", "Columns", &n, nullptr);
        if (cols != nullptr) {
            for (size_t i = FIXED_COLUMNS; i < NCOLUMNS; i++)
                t->col_on[i] = g_strv_contains((const char *const *)cols, columns[i].key);
            g_strfreev(cols);
        }
    }
    xd_strbuf_free(&path);
    g_key_file_unref(kf);
}

static void save_settings(taskmgr *t) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "TaskManager", "Tab",
                           gtk_notebook_get_current_page(GTK_NOTEBOOK(t->notebook)));
    g_key_file_set_integer(kf, "TaskManager", "UpdateSpeed", t->speed);
    g_key_file_set_integer(kf, "TaskManager", "ApplicationsView", t->apps_view);
    g_key_file_set_integer(kf, "TaskManager", "MemoryUnits", mem_unit);
    g_key_file_set_boolean(kf, "TaskManager", "MinimizeOnUse", t->minimize_on_use);
    g_key_file_set_boolean(kf, "TaskManager", "AlwaysOnTop", t->on_top);
    g_key_file_set_boolean(kf, "TaskManager", "OneGraphPerCPU", t->per_cpu);
    g_key_file_set_boolean(kf, "TaskManager", "ShowKernelTimes", t->show_kernel);
    const char *on[NCOLUMNS];
    gsize n = 0;
    for (size_t i = FIXED_COLUMNS; i < NCOLUMNS; i++) {
        if (t->col_on[i])
            on[n++] = columns[i].key;
    }
    g_key_file_set_string_list(kf, "TaskManager", "Columns", on, n);
    xd_strbuf path = {0};
    settings_path(&path);
    GError *err = nullptr;
    if (!g_key_file_save_to_file(kf, xd_strbuf_cstr(&path), &err)) {
        g_warning("cannot save %s: %s", xd_strbuf_cstr(&path), err->message);
        g_clear_error(&err);
    }
    xd_strbuf_free(&path);
    g_key_file_unref(kf);
}

/* ---- Applications tab ------------------------------------------------------------- */

static GdkPixbuf *load_icon(GIcon *gicon, const char *name, int size) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GtkIconInfo *info = nullptr;
    if (gicon != nullptr)
        info = gtk_icon_theme_lookup_by_gicon(theme, gicon, size, GTK_ICON_LOOKUP_FORCE_SIZE);
    if (info == nullptr && name != nullptr && *name != '\0')
        info = gtk_icon_theme_lookup_icon(theme, name, size, GTK_ICON_LOOKUP_FORCE_SIZE);
    if (info == nullptr)
        info = gtk_icon_theme_lookup_icon(theme, "application-x-executable", size,
                                          GTK_ICON_LOOKUP_FORCE_SIZE);
    if (info == nullptr)
        return nullptr;
    GdkPixbuf *pb = gtk_icon_info_load_icon(info, nullptr);
    g_object_unref(info);
    return pb;
}

/* The icon of the window's application, from its .desktop file. */
static void task_icons(const char *app_id, GdkPixbuf **small, GdkPixbuf **large) {
    GIcon *gicon = nullptr;
    if (app_id != nullptr && *app_id != '\0') {
        char *id = g_strconcat(app_id, ".desktop", nullptr);
        GDesktopAppInfo *ai = g_desktop_app_info_new(id);
        if (ai == nullptr) {
            char *lower = g_ascii_strdown(id, -1);
            ai = g_desktop_app_info_new(lower);
            g_free(lower);
        }
        g_free(id);
        if (ai != nullptr) {
            GIcon *i = g_app_info_get_icon(G_APP_INFO(ai));
            gicon = i != nullptr ? g_object_ref(i) : nullptr;
            g_object_unref(ai);
        }
    }
    *small = load_icon(gicon, app_id, 16);
    *large = load_icon(gicon, app_id, 32);
    g_clear_object(&gicon);
}

static void on_task_changed(xd_task *task, gpointer data) {
    taskmgr *t = data;
    GtkTreeIter *iter = task->user;
    if (iter == nullptr) {
        iter = g_new(GtkTreeIter, 1);
        gtk_list_store_append(t->apps, iter);
        task->user = iter;
    }
    GdkPixbuf *small = nullptr, *large = nullptr;
    task_icons(task->app_id, &small, &large);
    const char *title = task->title != nullptr && *task->title != '\0' ? task->title
                        : task->app_id != nullptr                     ? task->app_id
                                                                      : "";
    /* The compositor adds " (Not Responding)" to a hung window's title,
     * as Windows does; its status says so. */
    gboolean hung = g_str_has_suffix(title, " (Not Responding)");
    gtk_list_store_set(t->apps, iter, AP_TASK, task, AP_ICON16, small, AP_ICON32, large, AP_TITLE,
                       title, AP_STATUS, hung ? "Not Responding" : "Running", -1);
    g_clear_object(&small);
    g_clear_object(&large);
}

static void on_task_closed(xd_task *task, gpointer data) {
    taskmgr *t = data;
    GtkTreeIter *iter = task->user;
    if (iter != nullptr) {
        gtk_list_store_remove(t->apps, iter);
        g_free(iter);
        task->user = nullptr;
    }
}

/* The windows selected on the Applications tab (xd_task*, in list
 * order; several with Ctrl or Shift, as in Windows). g_list_free it. */
static GList *selected_tasks(taskmgr *t) {
    GtkWidget *view = t->apps_views[t->apps_view];
    GList *paths = t->apps_view == VIEW_DETAILS
                       ? gtk_tree_selection_get_selected_rows(
                             gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), nullptr)
                       : gtk_icon_view_get_selected_items(GTK_ICON_VIEW(view));
    GList *tasks = nullptr;
    for (GList *l = paths; l != nullptr; l = l->next) {
        GtkTreeIter iter;
        xd_task *task = nullptr;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(t->apps), &iter, l->data))
            gtk_tree_model_get(GTK_TREE_MODEL(t->apps), &iter, AP_TASK, &task, -1);
        if (task != nullptr)
            tasks = g_list_prepend(tasks, task);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return g_list_reverse(tasks);
}

static xd_task *selected_task(taskmgr *t) {
    GList *tasks = selected_tasks(t);
    xd_task *first = tasks != nullptr ? tasks->data : nullptr;
    g_list_free(tasks);
    return first;
}

static void update_app_buttons(taskmgr *t) {
    gboolean any = selected_task(t) != nullptr;
    gtk_widget_set_sensitive(t->end_task, any);
    gtk_widget_set_sensitive(t->switch_to, any);
    /* Tile and Cascade need a selection and our compositor's arranger. */
    gboolean arrange = any && xd_tasklist_can_arrange(t->tasks);
    GtkWidget *items[] = {t->tile_h_item, t->tile_v_item, t->cascade_item};
    for (size_t i = 0; i < G_N_ELEMENTS(items); i++)
        if (items[i] != nullptr)
            gtk_widget_set_sensitive(items[i], arrange);
}

static void switch_to(taskmgr *t, gboolean minimize_self) {
    xd_task *task = selected_task(t);
    if (task == nullptr)
        return;
    if (task->minimized)
        xd_task_set_minimized(task, FALSE);
    xd_task_activate(task);
    if (minimize_self && t->minimize_on_use)
        gtk_window_iconify(GTK_WINDOW(t->window));
}

static void cb_switch_to([[maybe_unused]] GtkWidget *w, gpointer d) { switch_to(d, TRUE); }
static void cb_bring_to_front([[maybe_unused]] GtkWidget *w, gpointer d) { switch_to(d, FALSE); }

/* End Task, Minimize and Maximize act on every selected window. */
static void cb_end_task([[maybe_unused]] GtkWidget *w, gpointer d) {
    GList *tasks = selected_tasks(d);
    for (GList *l = tasks; l != nullptr; l = l->next)
        xd_task_close(l->data);
    g_list_free(tasks);
}

static void cb_minimize_task([[maybe_unused]] GtkWidget *w, gpointer d) {
    GList *tasks = selected_tasks(d);
    for (GList *l = tasks; l != nullptr; l = l->next)
        xd_task_set_minimized(l->data, TRUE);
    g_list_free(tasks);
}

static void cb_maximize_task([[maybe_unused]] GtkWidget *w, gpointer d) {
    GList *tasks = selected_tasks(d);
    for (GList *l = tasks; l != nullptr; l = l->next) {
        xd_task *task = l->data;
        if (task->minimized)
            xd_task_set_minimized(task, FALSE);
        xd_task_set_maximized(task, TRUE);
    }
    g_list_free(tasks);
}

/* Windows ▸ Tile and Cascade: the selected windows. */
static void arrange_selected(taskmgr *t, xd_arrange_mode mode) {
    GList *tasks = selected_tasks(t);
    xd_tasklist_arrange(t->tasks, tasks, mode);
    g_list_free(tasks);
}

static void cb_tile_h([[maybe_unused]] GtkWidget *w, gpointer d) {
    arrange_selected(d, XD_ARRANGE_TILE_HORIZONTALLY);
}
static void cb_tile_v([[maybe_unused]] GtkWidget *w, gpointer d) {
    arrange_selected(d, XD_ARRANGE_TILE_VERTICALLY);
}
static void cb_cascade([[maybe_unused]] GtkWidget *w, gpointer d) {
    arrange_selected(d, XD_ARRANGE_CASCADE);
}

static void cb_new_task([[maybe_unused]] GtkWidget *w, gpointer d) {
    taskmgr *t = d;
    char *run = own_program("xd-run");
    char *argv[] = {run, nullptr};
    GError *err = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &err)) {
        message(t, XD_MB_ERROR, "Create New Task", err->message);
        g_clear_error(&err);
    }
    g_free(run);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    if (cb != nullptr)
        g_signal_connect(item, "activate", cb, data);
    else
        gtk_widget_set_sensitive(item, FALSE);
    if (accel != nullptr && key != 0)
        gtk_widget_add_accelerator(item, "activate", accel, key, mods, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

/* GTK deactivates a menu before running the chosen item, so destroying it
 * on "deactivate" would drop the click; destroy it once that's done. */
static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu_later(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static void popup(GtkWidget *menu, GtkWidget *attach, GdkEvent *event) {
    gtk_menu_attach_to_widget(GTK_MENU(menu), attach, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    if (event != nullptr)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), attach, GDK_GRAVITY_CENTER,
                                 GDK_GRAVITY_NORTH_WEST, nullptr);
}

static void apps_context_menu(taskmgr *t, GdkEvent *event) {
    GtkWidget *menu = gtk_menu_new();
    if (selected_task(t) != nullptr) {
        menu_item(menu, "_Switch To", G_CALLBACK(cb_switch_to), t, nullptr, 0, NO_MODS);
        menu_item(menu, "_Bring To Front", G_CALLBACK(cb_bring_to_front), t, nullptr, 0,
                  NO_MODS);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "Mi_nimize", G_CALLBACK(cb_minimize_task), t, nullptr, 0, NO_MODS);
        menu_item(menu, "Ma_ximize", G_CALLBACK(cb_maximize_task), t, nullptr, 0, NO_MODS);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "_End Task", G_CALLBACK(cb_end_task), t, nullptr, 0, NO_MODS);
    } else {
        menu_item(menu, "_New Task (Run...)", G_CALLBACK(cb_new_task), t, nullptr, 0, NO_MODS);
    }
    popup(menu, t->apps_views[t->apps_view], event);
}

/* Right-click selects what's under the pointer, then shows the menu. */
static gboolean on_apps_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    taskmgr *t = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (GTK_IS_TREE_VIEW(w)) {
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path,
                                          nullptr, nullptr, nullptr))
            gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
        else
            gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)));
    } else {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w), (int)ev->x, (int)ev->y);
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(w));
        if (path != nullptr)
            gtk_icon_view_select_path(GTK_ICON_VIEW(w), path);
    }
    if (path != nullptr)
        gtk_tree_path_free(path);
    apps_context_menu(t, (GdkEvent *)ev);
    return TRUE;
}

static void on_apps_selection([[maybe_unused]] GtkWidget *w, gpointer data) {
    update_app_buttons(data);
}

static void on_apps_row_activated([[maybe_unused]] GtkTreeView *tv,
                                  [[maybe_unused]] GtkTreePath *p,
                                  [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    switch_to(data, TRUE);
}

static void on_apps_item_activated([[maybe_unused]] GtkIconView *iv,
                                   [[maybe_unused]] GtkTreePath *p, gpointer data) {
    switch_to(data, TRUE);
}

static void set_apps_view(taskmgr *t, int view) {
    t->apps_view = view;
    static const char *const names[VIEW_COUNT] = {"large", "small", "details"};
    gtk_stack_set_visible_child_name(GTK_STACK(t->apps_stack), names[view]);
    update_app_buttons(t);
}

static GtkWidget *scrolled(GtkWidget *child) {
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

/* Buttons along the bottom right of a tab. */
static GtkWidget *button_row(GtkWidget **buttons, int n) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (int i = n - 1; i >= 0; i--)
        gtk_box_pack_end(GTK_BOX(row), buttons[i], FALSE, FALSE, 0);
    for (int i = 0; i < n; i++)
        gtk_widget_set_size_request(buttons[i], 84, -1);
    return row;
}

static GtkWidget *build_apps(taskmgr *t) {
    t->apps = gtk_list_store_new(AP_COUNT, G_TYPE_POINTER, GDK_TYPE_PIXBUF, GDK_TYPE_PIXBUF,
                                 G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    /* Details */
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(t->apps));
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "Task");
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_sort_column_id(col, AP_TITLE);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, pix, FALSE);
    gtk_tree_view_column_add_attribute(col, pix, "pixbuf", AP_ICON16);
    GtkCellRenderer *txt = gtk_cell_renderer_text_new();
    g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(col, txt, TRUE);
    gtk_tree_view_column_add_attribute(col, txt, "text", AP_TITLE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
    col = gtk_tree_view_column_new_with_attributes("Status", gtk_cell_renderer_text_new(), "text",
                                                   AP_STATUS, nullptr);
    gtk_tree_view_column_set_min_width(col, 90);
    gtk_tree_view_column_set_sort_column_id(col, AP_STATUS);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)),
                                GTK_SELECTION_MULTIPLE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), "changed",
                     G_CALLBACK(on_apps_selection), t);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_apps_row_activated), t);
    t->apps_views[VIEW_DETAILS] = tree;

    /* Large and small icons */
    for (int v = VIEW_LARGE; v <= VIEW_SMALL; v++) {
        GtkWidget *iv = gtk_icon_view_new_with_model(GTK_TREE_MODEL(t->apps));
        gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(iv), v == VIEW_LARGE ? AP_ICON32 : AP_ICON16);
        gtk_icon_view_set_text_column(GTK_ICON_VIEW(iv), AP_TITLE);
        gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(iv), GTK_SELECTION_MULTIPLE);
        if (v == VIEW_SMALL) {
            gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(iv), GTK_ORIENTATION_HORIZONTAL);
            gtk_icon_view_set_item_width(GTK_ICON_VIEW(iv), 160);
        } else {
            gtk_icon_view_set_item_width(GTK_ICON_VIEW(iv), 76);
        }
        gtk_icon_view_set_margin(GTK_ICON_VIEW(iv), 4);
        g_signal_connect(iv, "selection-changed", G_CALLBACK(on_apps_selection), t);
        g_signal_connect(iv, "item-activated", G_CALLBACK(on_apps_item_activated), t);
        t->apps_views[v] = iv;
    }

    t->apps_stack = gtk_stack_new();
    static const char *const names[VIEW_COUNT] = {"large", "small", "details"};
    for (int v = 0; v < VIEW_COUNT; v++) {
        g_signal_connect(t->apps_views[v], "button-press-event", G_CALLBACK(on_apps_button), t);
        gtk_stack_add_named(GTK_STACK(t->apps_stack), scrolled(t->apps_views[v]), names[v]);
    }
    gtk_box_pack_start(GTK_BOX(box), t->apps_stack, TRUE, TRUE, 0);

    t->end_task = gtk_button_new_with_mnemonic("_End Task");
    t->switch_to = gtk_button_new_with_mnemonic("_Switch To");
    GtkWidget *new_task = gtk_button_new_with_mnemonic("_New Task...");
    g_signal_connect(t->end_task, "clicked", G_CALLBACK(cb_end_task), t);
    g_signal_connect(t->switch_to, "clicked", G_CALLBACK(cb_switch_to), t);
    g_signal_connect(new_task, "clicked", G_CALLBACK(cb_new_task), t);
    GtkWidget *buttons[] = {t->end_task, t->switch_to, new_task};
    gtk_box_pack_start(GTK_BOX(box), button_row(buttons, 3), FALSE, FALSE, 0);

    t->tasks = xd_tasklist_new(gdk_display_get_default(), on_task_changed, on_task_closed, t);
    if (t->on_top_item != nullptr)
        gtk_widget_set_sensitive(t->on_top_item, xd_tasklist_can_set_on_top(t->tasks));
    if (!xd_tasklist_available(t->tasks)) {
        gtk_widget_set_tooltip_text(t->apps_stack,
                                    "The window list needs the XeonDesktop compositor.");
    }
    return box;
}

/* ---- Processes tab ---------------------------------------------------------------- */

static void format_cell([[maybe_unused]] GtkTreeViewColumn *col, GtkCellRenderer *cell,
                        GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    const column_def *c = &columns[GPOINTER_TO_INT(data)];
    char buf[64];
    GString *s = nullptr;
    switch (c->format) {
    case F_TEXT: {
        char *text = nullptr;
        gtk_tree_model_get(model, iter, c->store, &text, -1);
        g_object_set(cell, "text", text != nullptr ? text : "", nullptr);
        g_free(text);
        return;
    }
    case F_INT:
    case F_PERCENT:
    case F_PRIO: {
        int v = 0;
        gtk_tree_model_get(model, iter, c->store, &v, -1);
        if (c->format == F_PRIO)
            g_object_set(cell, "text", priority_name(v), nullptr);
        else {
            g_snprintf(buf, sizeof(buf), c->format == F_PERCENT ? "%02d" : "%d", v);
            g_object_set(cell, "text", buf, nullptr);
        }
        return;
    }
    case F_TIME: {
        guint64 ticks = 0;
        gtk_tree_model_get(model, iter, c->store, &ticks, -1);
        guint64 secs = ticks / (guint64)xd_sysinfo_hz();
        g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT ":%02u:%02u", secs / 3600,
                   (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
        g_object_set(cell, "text", buf, nullptr);
        return;
    }
    case F_KB:
    case F_COUNT: {
        guint64 v = 0;
        if (c->store == PS_HANDLES) {
            int h = 0;
            gtk_tree_model_get(model, iter, c->store, &h, -1);
            v = h > 0 ? (guint64)h : 0;
        } else if (c->store == PS_THREADS) {
            int th = 0;
            gtk_tree_model_get(model, iter, c->store, &th, -1);
            v = th > 0 ? (guint64)th : 0;
        } else {
            gtk_tree_model_get(model, iter, c->store, &v, -1);
        }
        s = g_string_new(nullptr);
        if (c->format == F_KB)
            append_memory(s, v, TRUE);
        else
            append_grouped(s, v);
        break;
    }
    case F_KB_DELTA: {
        gint64 v = 0;
        gtk_tree_model_get(model, iter, c->store, &v, -1);
        s = g_string_new(v < 0 ? "-" : "");
        append_memory(s, v < 0 ? (guint64)(-v) : (guint64)v, TRUE);
        break;
    }
    }
    g_object_set(cell, "text", s->str, nullptr);
    g_string_free(s, TRUE);
}

static void apply_columns(taskmgr *t) {
    for (size_t i = 0; i < NCOLUMNS; i++)
        gtk_tree_view_column_set_visible(t->proc_cols[i], t->col_on[i]);
    gboolean handles = FALSE, io = FALSE;
    for (size_t i = 0; i < NCOLUMNS; i++) {
        if (t->col_on[i] && columns[i].store == PS_HANDLES)
            handles = TRUE;
        if (t->col_on[i] && (columns[i].store == PS_IOR || columns[i].store == PS_IOW))
            io = TRUE;
    }
    xd_sysinfo_set_extras(t->si, handles, io);
}

static void update_processes(taskmgr *t) {
    guint n = 0;
    const xd_proc *procs = xd_sysinfo_procs(t->si, &n);
    const xd_sysstats *st = xd_sysinfo_stats(t->si);
    guint64 gen = ++t->generation;
    guint64 total = st->total_delta;

    for (guint i = 0; i <= n; i++) {
        /* i == n: Windows' System Idle Process, from the idle time. */
        xd_proc idle = {
            .pid = 0,
            .name = "System Idle Process",
            .user = "SYSTEM",
            .cpu_ticks = st->idle_ticks,
            .handles = 0,
        };
        const xd_proc *p = i < n ? &procs[i] : &idle;
        int cpu = i < n ? (total > 0 ? (int)((guint64)p->cpu_delta * 100 + total / 2) / (int)total
                                     : 0)
                        : (int)(st->idle * 100 + 0.5);
        cpu = CLAMP(cpu, 0, 99);
        GtkTreeIter *iter = g_hash_table_lookup(t->rows, GINT_TO_POINTER(p->pid));
        guint64 old_mem = p->mem_kb, old_faults = p->faults, peak = p->mem_kb;
        if (iter == nullptr) {
            iter = g_new(GtkTreeIter, 1);
            gtk_list_store_append(t->procs, iter);
            g_hash_table_insert(t->rows, GINT_TO_POINTER(p->pid), iter);
        } else {
            guint64 old_peak = 0;
            gtk_tree_model_get(GTK_TREE_MODEL(t->procs), iter, PS_MEM, &old_mem, PS_FAULTS,
                               &old_faults, PS_PEAK, &old_peak, -1);
            peak = MAX(peak, old_peak);
        }
        gint64 mem_delta = (gint64)p->mem_kb - (gint64)old_mem;
        guint64 fault_delta = p->faults >= old_faults ? p->faults - old_faults : 0;
        guint64 cpu_ticks = p->cpu_ticks, mem = p->mem_kb, vm = p->vm_kb, faults = p->faults;
        guint64 ior = p->io_read, iow = p->io_write;
        gtk_list_store_set(t->procs, iter, PS_PID, p->pid, PS_NAME, p->name, PS_USER, p->user,
                           PS_CPU, cpu, PS_TIME, cpu_ticks, PS_MEM, mem, PS_MEM_DELTA, mem_delta,
                           PS_PEAK, peak, PS_FAULTS, faults, PS_FAULTS_DELTA, fault_delta, PS_VM,
                           vm, PS_PRIO, p->nice, PS_THREADS, p->threads, PS_HANDLES, p->handles,
                           PS_IOR, ior, PS_IOW, iow, PS_GEN, gen, -1);
    }

    /* Drop processes that have exited. */
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, t->rows);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        guint64 seen = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(t->procs), value, PS_GEN, &seen, -1);
        if (seen != gen) {
            gtk_list_store_remove(t->procs, value);
            g_hash_table_iter_remove(&it);
        }
    }
}

static int selected_pid(taskmgr *t) {
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(t->procs_tree)),
                                         nullptr, &iter))
        return -1;
    int pid = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(t->procs), &iter, PS_PID, &pid, -1);
    return pid;
}

static void kill_failed(taskmgr *t, int err) {
    char *text = g_strdup_printf("The operation could not be completed.\n\n%s", errno_text(err));
    message(t, XD_MB_ERROR, "Unable to Terminate Process", text);
    g_free(text);
}

static void cb_end_process([[maybe_unused]] GtkWidget *w, gpointer d) {
    taskmgr *t = d;
    int pid = selected_pid(t);
    if (pid <= 0)
        return;
    if (!confirm(t, "Task Manager Warning",
                 "WARNING: Terminating a process can cause undesired results including loss of "
                 "data and system instability. The process will not be given the chance to save "
                 "its state or data before it is terminated. Are you sure you want to terminate "
                 "the process?"))
        return;
    if (kill(pid, SIGKILL) != 0)
        kill_failed(t, errno);
    else
        sample(t);
}

/* Children before parents, so a parent can't restart them. */
static void collect_tree(const xd_proc *procs, guint n, int pid, GArray *out, int depth) {
    if (depth > 64)
        return;
    for (guint i = 0; i < n; i++) {
        if (procs[i].ppid == pid && procs[i].pid != pid)
            collect_tree(procs, n, procs[i].pid, out, depth + 1);
    }
    g_array_append_val(out, pid);
}

static void cb_end_tree([[maybe_unused]] GtkWidget *w, gpointer d) {
    taskmgr *t = d;
    int pid = selected_pid(t);
    if (pid <= 0)
        return;
    if (!confirm(t, "Task Manager Warning",
                 "WARNING: Terminating a process tree can cause undesired results including loss "
                 "of data and system instability. The processes will not be given the chance to "
                 "save their state or data before they are terminated. Are you sure you want to "
                 "terminate the process tree?"))
        return;
    guint n = 0;
    const xd_proc *procs = xd_sysinfo_procs(t->si, &n);
    GArray *pids = g_array_new(FALSE, FALSE, sizeof(int));
    collect_tree(procs, n, pid, pids, 0);
    int first_err = 0;
    for (guint i = 0; i < pids->len; i++) {
        if (kill(g_array_index(pids, int, i), SIGKILL) != 0 && first_err == 0 && errno != ESRCH)
            first_err = errno;
    }
    g_array_unref(pids);
    if (first_err != 0)
        kill_failed(t, first_err);
    sample(t);
}

static void cb_priority(GtkCheckMenuItem *item, gpointer d) {
    taskmgr *t = d;
    if (t->syncing || !gtk_check_menu_item_get_active(item))
        return;
    int pid = selected_pid(t);
    int nice = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "nice"));
    if (pid <= 0)
        return;
    if (!confirm(t, "Task Manager Warning",
                 "WARNING: Changing the priority class of this process may cause undesired "
                 "results including system instability. Are you sure you want to change the "
                 "priority class?"))
        return;
    if (setpriority(PRIO_PROCESS, (id_t)pid, nice) != 0) {
        char *text = g_strdup_printf("The operation could not be completed.\n\n%s%s",
                                     errno_text(errno),
                                     nice < 0 ? "\n\nRaising priority needs administrator "
                                                "rights."
                                              : "");
        message(t, XD_MB_ERROR, "Unable to Change Priority", text);
        g_free(text);
    }
    sample(t);
}

static void cb_affinity([[maybe_unused]] GtkWidget *w, gpointer d) {
    taskmgr *t = d;
    int pid = selected_pid(t);
    if (pid <= 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(pid, sizeof(set), &set) != 0) {
        message(t, XD_MB_ERROR, "Unable to Access Process", errno_text(errno));
        return;
    }
    int ncpu = MAX(t->ncpu, 1);
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Processor Affinity", GTK_WINDOW(t->window),
                                                 GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK,
                                                 "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *intro = gtk_label_new("The Processor Affinity setting controls which CPUs the "
                                     "process will be allowed to execute on.");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 44);
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    GtkWidget **checks = g_new0(GtkWidget *, (gsize)ncpu);
    for (int i = 0; i < ncpu; i++) {
        char label[24];
        g_snprintf(label, sizeof(label), "CPU %d", i);
        checks[i] = gtk_check_button_new_with_label(label);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checks[i]), CPU_ISSET((size_t)i, &set));
        gtk_grid_attach(GTK_GRID(grid), checks[i], i % 4, i / 4, 1, 1);
    }
    gtk_widget_show_all(dlg);
    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        CPU_ZERO(&set);
        int count = 0;
        for (int i = 0; i < ncpu; i++) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks[i]))) {
                CPU_SET((size_t)i, &set);
                count++;
            }
        }
        if (count == 0) {
            message(t, XD_MB_ERROR, "Invalid Option",
                    "The process must have affinity with at least one processor.");
            continue;
        }
        if (sched_setaffinity(pid, sizeof(set), &set) != 0)
            message(t, XD_MB_ERROR, "Unable to Change Affinity", errno_text(errno));
        break;
    }
    g_free(checks);
    gtk_widget_destroy(dlg);
}

static void procs_context_menu(taskmgr *t, GdkEvent *event) {
    int pid = selected_pid(t);
    if (pid <= 0)
        return;
    int nice = 0;
    guint n = 0;
    const xd_proc *procs = xd_sysinfo_procs(t->si, &n);
    for (guint i = 0; i < n; i++) {
        if (procs[i].pid == pid)
            nice = procs[i].nice;
    }
    GtkWidget *menu = gtk_menu_new();
    menu_item(menu, "_End Process", G_CALLBACK(cb_end_process), t, nullptr, 0, NO_MODS);
    menu_item(menu, "End Process _Tree", G_CALLBACK(cb_end_tree), t, nullptr, 0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *prio = gtk_menu_new();
    GtkWidget *top = gtk_menu_item_new_with_mnemonic("Set _Priority");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), prio);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
    GSList *group = nullptr;
    t->syncing = TRUE;
    for (size_t i = 0; i < G_N_ELEMENTS(priorities); i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(group, priorities[i].name);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "nice", GINT_TO_POINTER(priorities[i].nice));
        if (g_str_equal(priority_name(priorities[i].nice), priority_name(nice)))
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        g_signal_connect(item, "toggled", G_CALLBACK(cb_priority), t);
        gtk_menu_shell_append(GTK_MENU_SHELL(prio), item);
    }
    t->syncing = FALSE;
    if (t->ncpu > 1)
        menu_item(menu, "Set _Affinity...", G_CALLBACK(cb_affinity), t, nullptr, 0, NO_MODS);
    popup(menu, t->procs_tree, event);
}

static gboolean on_procs_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    taskmgr *t = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, nullptr,
                                      nullptr, nullptr)) {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
        gtk_tree_path_free(path);
        procs_context_menu(t, (GdkEvent *)ev);
    }
    return TRUE;
}

static void on_procs_selection(GtkTreeSelection *sel, gpointer data) {
    taskmgr *t = data;
    gtk_widget_set_sensitive(t->end_process,
                             gtk_tree_selection_get_selected(sel, nullptr, nullptr) &&
                                 selected_pid(t) > 0);
}

static void cb_select_columns([[maybe_unused]] GtkWidget *w, gpointer d) {
    taskmgr *t = d;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Select Columns", GTK_WINDOW(t->window),
                                                 GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK,
                                                 "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *intro = gtk_label_new("Select the columns that will appear on the Process page "
                                     "of the Task Manager.");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 40);
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 6);
    GtkWidget *checks[NCOLUMNS] = {nullptr};
    for (size_t i = FIXED_COLUMNS; i < NCOLUMNS; i++) {
        checks[i] = gtk_check_button_new_with_label(columns[i].choice);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checks[i]), t->col_on[i]);
        gtk_box_pack_start(GTK_BOX(box), checks[i], FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        for (size_t i = FIXED_COLUMNS; i < NCOLUMNS; i++)
            t->col_on[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks[i]));
        apply_columns(t);
        save_settings(t);
        sample(t);
    }
    gtk_widget_destroy(dlg);
}

static GtkWidget *build_processes(taskmgr *t) {
    t->procs = gtk_list_store_new(
        PS_COUNT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT64,
        G_TYPE_UINT64, G_TYPE_INT64, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64,
        G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64);
    t->rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, g_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    t->procs_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(t->procs));
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(t->procs_tree), TRUE);
    /* Typing a name selects the process, as in Windows. */
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(t->procs_tree), PS_NAME);
    for (size_t i = 0; i < NCOLUMNS; i++) {
        GtkCellRenderer *cell = gtk_cell_renderer_text_new();
        if (columns[i].format != F_TEXT && columns[i].format != F_PRIO)
            g_object_set(cell, "xalign", 1.0, nullptr);
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, columns[i].title);
        gtk_tree_view_column_pack_start(col, cell, TRUE);
        gtk_tree_view_column_set_cell_data_func(col, cell, format_cell, GINT_TO_POINTER((int)i),
                                                nullptr);
        gtk_tree_view_column_set_sort_column_id(col, columns[i].store);
        gtk_tree_view_column_set_resizable(col, TRUE);
        /* Fixed widths, as Windows starts them; also faster with many rows. */
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, columns[i].width);
        if (columns[i].format != F_TEXT && columns[i].format != F_PRIO)
            gtk_tree_view_column_set_alignment(col, 1.0);
        t->proc_cols[i] = col;
        gtk_tree_view_append_column(GTK_TREE_VIEW(t->procs_tree), col);
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(t->procs_tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_procs_selection), t);
    g_signal_connect(t->procs_tree, "button-press-event", G_CALLBACK(on_procs_button), t);
    gtk_box_pack_start(GTK_BOX(box), scrolled(t->procs_tree), TRUE, TRUE, 0);
    t->end_process = gtk_button_new_with_mnemonic("_End Process");
    gtk_widget_set_sensitive(t->end_process, FALSE);
    g_signal_connect(t->end_process, "clicked", G_CALLBACK(cb_end_process), t);
    GtkWidget *buttons[] = {t->end_process};
    gtk_box_pack_start(GTK_BOX(box), button_row(buttons, 1), FALSE, FALSE, 0);
    return box;
}

/* ---- Performance tab -------------------------------------------------------------- */

/* A black well with a thin sunken edge. Returns the inside rectangle. */
static void draw_well(GtkWidget *w, cairo_t *cr, double *x, double *y, double *wd, double *ht) {
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA shadow = {0.5, 0.5, 0.5, 1}, light = {1, 1, 1, 1};
    gtk_style_context_lookup_color(sc, "borders", &shadow);
    cairo_set_line_width(cr, 1);
    gdk_cairo_set_source_rgba(cr, &shadow);
    cairo_move_to(cr, 0.5, height - 1);
    cairo_line_to(cr, 0.5, 0.5);
    cairo_line_to(cr, width - 1, 0.5);
    cairo_stroke(cr);
    gdk_cairo_set_source_rgba(cr, &light);
    cairo_move_to(cr, width - 0.5, 0);
    cairo_line_to(cr, width - 0.5, height - 0.5);
    cairo_line_to(cr, 0, height - 0.5);
    cairo_stroke(cr);
    *x = 1;
    *y = 1;
    *wd = width - 2;
    *ht = height - 2;
}

static gboolean draw_cpu_meter(GtkWidget *w, cairo_t *cr, gpointer data) {
    taskmgr *t = data;
    double x, y, wd, ht;
    draw_well(w, cr, &x, &y, &wd, &ht);
    xd_meter_draw(cr, x, y, wd, ht, t->cpu_now, t->kernel_now, t->show_kernel, t->cpu_text);
    return TRUE;
}

static gboolean draw_mem_meter(GtkWidget *w, cairo_t *cr, gpointer data) {
    taskmgr *t = data;
    double x, y, wd, ht;
    draw_well(w, cr, &x, &y, &wd, &ht);
    xd_meter_draw(cr, x, y, wd, ht, t->mem_now, 0, FALSE, t->mem_text);
    return TRUE;
}

static gboolean draw_history(GtkWidget *w, cairo_t *cr, gpointer data) {
    taskmgr *t = g_object_get_data(G_OBJECT(w), "taskmgr");
    const xd_history *h = data;
    double x, y, wd, ht;
    draw_well(w, cr, &x, &y, &wd, &ht);
    xd_history_draw(cr, x, y, wd, ht, h, h != &t->mem_hist && t->show_kernel);
    return TRUE;
}

static GtkWidget *graph_area(taskmgr *t, xd_history *h, int min_w, int min_h) {
    GtkWidget *a = gtk_drawing_area_new();
    gtk_widget_set_size_request(a, min_w, min_h);
    g_object_set_data(G_OBJECT(a), "taskmgr", t);
    g_signal_connect(a, "draw", G_CALLBACK(draw_history), h);
    return a;
}

static GtkWidget *group(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_widget_set_margin_start(child, 8);
    gtk_widget_set_margin_end(child, 8);
    gtk_widget_set_margin_top(child, 4);
    gtk_widget_set_margin_bottom(child, 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

/* A group of "Name  value" rows; values are right-aligned. */
static GtkWidget *info_group(const char *title, const char *const *names, GtkWidget **values,
                             int n) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (int i = 0; i < n; i++) {
        GtkWidget *name = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_set_hexpand(name, TRUE);
        values[i] = gtk_label_new("0");
        gtk_label_set_xalign(GTK_LABEL(values[i]), 1);
        gtk_grid_attach(GTK_GRID(grid), name, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), values[i], 1, i, 1, 1);
    }
    return group(title, grid);
}

static void rebuild_cpu_graphs(taskmgr *t) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(t->cpu_graphs_box));
    for (GList *l = kids; l != nullptr; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    for (int i = 0; i < t->ncpu; i++)
        gtk_box_pack_start(GTK_BOX(t->cpu_graphs_box), graph_area(t, &t->cpu_each[i], 20, 60),
                           TRUE, TRUE, 0);
    gtk_widget_show_all(t->cpu_graphs_box);
}

static void show_cpu_graphs(taskmgr *t) {
    gtk_widget_set_visible(t->cpu_graph, !t->per_cpu || t->ncpu <= 1);
    gtk_widget_set_visible(t->cpu_graphs_box, t->per_cpu && t->ncpu > 1);
}

static GtkWidget *build_performance(taskmgr *t) {
    xd_history_init(&t->cpu_hist, HISTORY);
    xd_history_init(&t->mem_hist, HISTORY);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    GtkWidget *top = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(top), 6);
    gtk_grid_set_column_spacing(GTK_GRID(top), 8);
    gtk_grid_set_row_homogeneous(GTK_GRID(top), TRUE);
    t->cpu_meter = gtk_drawing_area_new();
    gtk_widget_set_size_request(t->cpu_meter, 70, 80);
    g_signal_connect(t->cpu_meter, "draw", G_CALLBACK(draw_cpu_meter), t);
    t->mem_meter = gtk_drawing_area_new();
    gtk_widget_set_size_request(t->mem_meter, 70, 80);
    g_signal_connect(t->mem_meter, "draw", G_CALLBACK(draw_mem_meter), t);
    t->cpu_graph = graph_area(t, &t->cpu_hist, 120, 80);
    gtk_widget_set_hexpand(t->cpu_graph, TRUE);
    gtk_widget_set_vexpand(t->cpu_graph, TRUE);
    t->cpu_graphs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_set_homogeneous(GTK_BOX(t->cpu_graphs_box), TRUE);
    gtk_widget_set_no_show_all(t->cpu_graphs_box, TRUE);
    GtkWidget *cpu_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(cpu_hbox), t->cpu_graph, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cpu_hbox), t->cpu_graphs_box, TRUE, TRUE, 0);
    t->mem_graph = graph_area(t, &t->mem_hist, 120, 80);
    gtk_widget_set_hexpand(t->mem_graph, TRUE);
    gtk_grid_attach(GTK_GRID(top), group("CPU Usage", t->cpu_meter), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(top), group("CPU Usage History", cpu_hbox), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(top), group("MEM Usage", t->mem_meter), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(top), group("Memory Usage History", t->mem_graph), 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), top, TRUE, TRUE, 0);

    GtkWidget *bottom = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bottom), 6);
    gtk_grid_set_column_spacing(GTK_GRID(bottom), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(bottom), TRUE);
    /* Windows' layout with Linux's terms: Committed_AS/CommitLimit for the
     * commit charge, and reclaimable/unreclaimable slab for kernel memory. */
    static const char *const totals[] = {"Open Files", "Threads", "Processes"};
    static const char *const phys[] = {"Total", "Available", "Cache"};
    static const char *const commit[] = {"Committed", "Limit", "Peak"};
    static const char *const kern[] = {"Total", "Reclaimable", "Unreclaimable"};
    GtkWidget *v[3];
    gtk_grid_attach(GTK_GRID(bottom), info_group("Totals", totals, v, 3), 0, 0, 1, 1);
    t->handles = v[0];
    t->threads = v[1];
    t->processes = v[2];
    t->phys_frame = info_group("Physical Memory", phys, v, 3);
    gtk_widget_set_tooltip_text(t->phys_frame, "MemTotal, MemAvailable, and Cached + Buffers");
    gtk_grid_attach(GTK_GRID(bottom), t->phys_frame, 1, 0, 1, 1);
    t->phys_total = v[0];
    t->phys_avail = v[1];
    t->phys_cache = v[2];
    t->commit_frame = info_group("Committed Memory", commit, v, 3);
    gtk_widget_set_tooltip_text(t->commit_frame,
                                "Memory promised to processes, whether or not they've used it "
                                "yet (Committed_AS), and the limit (CommitLimit), which is only "
                                "enforced in strict overcommit mode.");
    gtk_grid_attach(GTK_GRID(bottom), t->commit_frame, 0, 1, 1, 1);
    t->commit_total = v[0];
    t->commit_limit = v[1];
    t->commit_peak = v[2];
    t->kern_frame = info_group("Kernel Memory", kern, v, 3);
    gtk_widget_set_tooltip_text(t->kern_frame,
                                "Slab memory the kernel can free under pressure (SReclaimable), "
                                "and slab, stacks and page tables it can't (SUnreclaim, "
                                "KernelStack, PageTables).");
    gtk_grid_attach(GTK_GRID(bottom), t->kern_frame, 1, 1, 1, 1);
    t->kern_total = v[0];
    t->kern_paged = v[1];
    t->kern_nonpaged = v[2];
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    return box;
}

static void update_performance(taskmgr *t) {
    const xd_sysstats *st = xd_sysinfo_stats(t->si);
    if (st->ncpu != t->ncpu) {
        for (int i = 0; i < t->ncpu; i++)
            xd_history_clear(&t->cpu_each[i]);
        g_free(t->cpu_each);
        t->ncpu = st->ncpu;
        t->cpu_each = g_new0(xd_history, (gsize)MAX(t->ncpu, 1));
        for (int i = 0; i < t->ncpu; i++)
            xd_history_init(&t->cpu_each[i], HISTORY);
        rebuild_cpu_graphs(t);
        show_cpu_graphs(t);
    }
    t->cpu_now = st->cpu;
    t->kernel_now = st->cpu_kernel;
    g_snprintf(t->cpu_text, sizeof(t->cpu_text), "%d %%", (int)(st->cpu * 100 + 0.5));
    xd_history_push(&t->cpu_hist, st->cpu, st->cpu_kernel);
    for (int i = 0; i < t->ncpu; i++)
        xd_history_push(&t->cpu_each[i], st->cpu_each[i], st->kernel_each[i]);

    guint64 used = st->mem_total_kb > st->mem_avail_kb ? st->mem_total_kb - st->mem_avail_kb : 0;
    t->mem_now = st->mem_total_kb > 0 ? (double)used / (double)st->mem_total_kb : 0;
    GString *ms = g_string_new(nullptr);
    append_memory(ms, used, TRUE);
    g_strlcpy(t->mem_text, ms->str, sizeof(t->mem_text));
    xd_history_push(&t->mem_hist, t->mem_now, 0);

    t->peak_commit = MAX(t->peak_commit, st->commit_kb);
    set_number(t->handles, st->handles);
    set_number(t->threads, st->threads);
    set_number(t->processes, st->processes);
    set_memory(t->phys_total, st->mem_total_kb);
    set_memory(t->phys_avail, st->mem_avail_kb);
    set_memory(t->phys_cache, st->cache_kb);
    set_memory(t->commit_total, st->commit_kb);
    set_memory(t->commit_limit, st->commit_limit_kb);
    set_memory(t->commit_peak, t->peak_commit);
    set_memory(t->kern_total, st->kernel_paged_kb + st->kernel_nonpaged_kb);
    set_memory(t->kern_paged, st->kernel_paged_kb);
    set_memory(t->kern_nonpaged, st->kernel_nonpaged_kb);
    char title[48];
    static const char *const names[] = {"Physical Memory", "Committed Memory", "Kernel Memory"};
    GtkWidget *frames[] = {t->phys_frame, t->commit_frame, t->kern_frame};
    for (int i = 0; i < 3; i++) {
        g_snprintf(title, sizeof(title), "%s (%s)", names[i], unit_names[mem_unit]);
        gtk_frame_set_label(GTK_FRAME(frames[i]), title);
    }

    gtk_widget_queue_draw(t->cpu_meter);
    gtk_widget_queue_draw(t->mem_meter);
    gtk_widget_queue_draw(t->cpu_graph);
    gtk_widget_queue_draw(t->cpu_graphs_box);
    gtk_widget_queue_draw(t->mem_graph);

    char buf[96];
    g_snprintf(buf, sizeof(buf), "Processes: %u", st->processes);
    gtk_label_set_text(GTK_LABEL(t->st_procs), buf);
    g_snprintf(buf, sizeof(buf), "CPU Usage: %d%%", (int)(st->cpu * 100 + 0.5));
    gtk_label_set_text(GTK_LABEL(t->st_cpu), buf);
    g_string_assign(ms, "Mem Usage: ");
    append_memory(ms, used, TRUE);
    g_string_append(ms, " / ");
    append_memory(ms, st->mem_total_kb, TRUE);
    gtk_label_set_text(GTK_LABEL(t->st_mem), ms->str);
    g_string_free(ms, TRUE);
}

/* ---- sampling and menus ----------------------------------------------------------- */

static void sample(taskmgr *t) {
    if (!xd_sysinfo_sample(t->si))
        return;
    update_processes(t);
    update_performance(t);
}

static gboolean on_timer(gpointer data) {
    sample(data);
    return G_SOURCE_CONTINUE;
}

static void set_speed(taskmgr *t, int speed) {
    t->speed = speed;
    if (t->timer != 0) {
        g_source_remove(t->timer);
        t->timer = 0;
    }
    if (speed_ms[speed] > 0)
        t->timer = g_timeout_add(speed_ms[speed], on_timer, t);
}

static void cb_speed(GtkCheckMenuItem *item, gpointer d) {
    taskmgr *t = d;
    if (t->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_speed(t, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "speed")));
    save_settings(t);
}

static void cb_refresh([[maybe_unused]] GtkWidget *w, gpointer d) { sample(d); }

static void cb_units(GtkCheckMenuItem *item, gpointer d) {
    taskmgr *t = d;
    if (t->syncing || !gtk_check_menu_item_get_active(item))
        return;
    mem_unit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "unit"));
    gtk_widget_queue_draw(t->procs_tree);
    sample(t);
    save_settings(t);
}

static void cb_apps_view(GtkCheckMenuItem *item, gpointer d) {
    taskmgr *t = d;
    if (t->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_apps_view(t, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "view")));
    save_settings(t);
}

static void cb_cpu_history(GtkCheckMenuItem *item, gpointer d) {
    taskmgr *t = d;
    if (t->syncing || !gtk_check_menu_item_get_active(item))
        return;
    t->per_cpu = item == GTK_CHECK_MENU_ITEM(t->cpu_history_items[1]);
    show_cpu_graphs(t);
    save_settings(t);
}

static void cb_kernel(GtkCheckMenuItem *item, gpointer d) {
    taskmgr *t = d;
    t->show_kernel = gtk_check_menu_item_get_active(item);
    gtk_widget_queue_draw(t->notebook);
    save_settings(t);
}

static void cb_on_top(GtkCheckMenuItem *item, gpointer d) {
    taskmgr *t = d;
    t->on_top = gtk_check_menu_item_get_active(item);
    xd_tasklist_set_on_top(t->tasks, t->window, t->on_top);
    save_settings(t);
}

/* Once the window is on screen, Always On Top as it was left. */
static gboolean apply_on_top(GtkWidget *w, [[maybe_unused]] GdkEvent *ev, gpointer d) {
    taskmgr *t = d;
    if (t->on_top)
        xd_tasklist_set_on_top(t->tasks, w, TRUE);
    return FALSE;
}

static void cb_minimize_on_use(GtkCheckMenuItem *item, gpointer d) {
    taskmgr *t = d;
    t->minimize_on_use = gtk_check_menu_item_get_active(item);
    save_settings(t);
}

static void cb_exit([[maybe_unused]] GtkWidget *w, gpointer d) {
    taskmgr *t = d;
    gtk_widget_destroy(t->window);
}

static void cb_about([[maybe_unused]] GtkWidget *w, gpointer d) {
    taskmgr *t = d;
    gtk_show_about_dialog(GTK_WINDOW(t->window), "program-name", "Task Manager", "comments",
                          "Part of XeonDesktop", "logo-icon-name", "utilities-system-monitor",
                          "license-type", GTK_LICENSE_MIT_X11, nullptr);
}

static GtkWidget *submenu(GtkWidget *bar, const char *label) {
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    return menu;
}

static GtkWidget *radio_item(GtkWidget *menu, GSList **group, const char *label, gboolean active,
                             GCallback cb, gpointer data, const char *key, int value) {
    GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(*group, label);
    *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
    g_object_set_data(G_OBJECT(item), key, GINT_TO_POINTER(value));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);
    g_signal_connect(item, "toggled", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static GtkWidget *build_menus(taskmgr *t, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    t->syncing = TRUE;

    GtkWidget *file = submenu(bar, "_File");
    menu_item(file, "_New Task (Run...)", G_CALLBACK(cb_new_task), t, nullptr, 0, NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "E_xit Task Manager", G_CALLBACK(cb_exit), t, nullptr, 0, NO_MODS);

    GtkWidget *options = submenu(bar, "_Options");
    GtkWidget *on_top = gtk_check_menu_item_new_with_mnemonic("_Always On Top");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(on_top), t->on_top);
    /* Only our compositor offers it (known once the window list connects). */
    gtk_widget_set_sensitive(on_top, FALSE);
    t->on_top_item = on_top;
    g_signal_connect(on_top, "toggled", G_CALLBACK(cb_on_top), t);
    gtk_menu_shell_append(GTK_MENU_SHELL(options), on_top);
    GtkWidget *mou = gtk_check_menu_item_new_with_mnemonic("_Minimize On Use");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mou), t->minimize_on_use);
    g_signal_connect(mou, "toggled", G_CALLBACK(cb_minimize_on_use), t);
    gtk_menu_shell_append(GTK_MENU_SHELL(options), mou);
    GtkWidget *hide = gtk_check_menu_item_new_with_mnemonic("_Hide When Minimized");
    gtk_widget_set_sensitive(hide, FALSE); /* needs the notification area */
    gtk_menu_shell_append(GTK_MENU_SHELL(options), hide);

    GtkWidget *view = submenu(bar, "_View");
    menu_item(view, "_Refresh Now", G_CALLBACK(cb_refresh), t, accel, GDK_KEY_F5, NO_MODS);
    GtkWidget *speed_menu = gtk_menu_new();
    GtkWidget *speed = gtk_menu_item_new_with_mnemonic("_Update Speed");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(speed), speed_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), speed);
    static const char *const speeds[SPEED_COUNT] = {"_High", "_Normal", "_Low", "_Paused"};
    GSList *grp = nullptr;
    for (int i = 0; i < SPEED_COUNT; i++)
        t->speed_items[i] = radio_item(speed_menu, &grp, speeds[i], i == t->speed,
                                       G_CALLBACK(cb_speed), t, "speed", i);
    GtkWidget *units_menu = gtk_menu_new();
    GtkWidget *units = gtk_menu_item_new_with_mnemonic("_Memory Units");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(units), units_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), units);
    static const char *const unit_labels[UNIT_COUNT] = {"_Kilobytes (KB)", "_Megabytes (MB)",
                                                        "_Gigabytes (GB)"};
    grp = nullptr;
    for (int i = 0; i < UNIT_COUNT; i++)
        radio_item(units_menu, &grp, unit_labels[i], i == mem_unit, G_CALLBACK(cb_units), t,
                   "unit", i);
    t->view_sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(view), t->view_sep);
    static const char *const views[VIEW_COUNT] = {"Lar_ge Icons", "S_mall Icons", "_Details"};
    grp = nullptr;
    for (int i = 0; i < VIEW_COUNT; i++)
        t->view_apps[i] = radio_item(view, &grp, views[i], i == t->apps_view,
                                     G_CALLBACK(cb_apps_view), t, "view", i);
    t->view_columns =
        menu_item(view, "_Select Columns...", G_CALLBACK(cb_select_columns), t, nullptr, 0, NO_MODS);
    GtkWidget *hist_menu = gtk_menu_new();
    t->view_cpu_history = gtk_menu_item_new_with_mnemonic("_CPU History");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(t->view_cpu_history), hist_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), t->view_cpu_history);
    grp = nullptr;
    t->cpu_history_items[0] = radio_item(hist_menu, &grp, "_One Graph, All CPUs", !t->per_cpu,
                                         G_CALLBACK(cb_cpu_history), t, "graph", 0);
    t->cpu_history_items[1] = radio_item(hist_menu, &grp, "One Graph _Per CPU", t->per_cpu,
                                         G_CALLBACK(cb_cpu_history), t, "graph", 1);
    t->view_kernel = gtk_check_menu_item_new_with_mnemonic("Show _Kernel Times");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(t->view_kernel), t->show_kernel);
    g_signal_connect(t->view_kernel, "toggled", G_CALLBACK(cb_kernel), t);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), t->view_kernel);

    GtkWidget *windows = submenu(bar, "_Windows");
    t->windows_menu = gtk_menu_get_attach_widget(GTK_MENU(windows));
    t->tile_h_item = menu_item(windows, "Tile _Horizontally", G_CALLBACK(cb_tile_h), t, nullptr,
                               0, NO_MODS);
    t->tile_v_item = menu_item(windows, "Tile _Vertically", G_CALLBACK(cb_tile_v), t, nullptr, 0,
                               NO_MODS);
    menu_item(windows, "_Minimize", G_CALLBACK(cb_minimize_task), t, nullptr, 0, NO_MODS);
    menu_item(windows, "Ma_ximize", G_CALLBACK(cb_maximize_task), t, nullptr, 0, NO_MODS);
    t->cascade_item = menu_item(windows, "_Cascade", G_CALLBACK(cb_cascade), t, nullptr, 0,
                                NO_MODS);
    /* Until something is selected (update_app_buttons). */
    gtk_widget_set_sensitive(t->tile_h_item, FALSE);
    gtk_widget_set_sensitive(t->tile_v_item, FALSE);
    gtk_widget_set_sensitive(t->cascade_item, FALSE);
    menu_item(windows, "_Bring To Front", G_CALLBACK(cb_bring_to_front), t, nullptr, 0, NO_MODS);

    GtkWidget *help = submenu(bar, "_Help");
    menu_item(help, "_About Task Manager", G_CALLBACK(cb_about), t, nullptr, 0, NO_MODS);
    t->syncing = FALSE;
    return bar;
}

/* The View and Windows menus change with the tab, as in Windows. */
static void on_tab([[maybe_unused]] GtkNotebook *nb, [[maybe_unused]] GtkWidget *page, guint num,
                   gpointer data) {
    taskmgr *t = data;
    gboolean apps = num == TAB_APPS, procs = num == TAB_PROCS, perf = num == TAB_PERF;
    gtk_widget_set_visible(t->windows_menu, apps);
    for (int i = 0; i < VIEW_COUNT; i++)
        gtk_widget_set_visible(t->view_apps[i], apps);
    gtk_widget_set_visible(t->view_columns, procs);
    gtk_widget_set_visible(t->view_cpu_history, perf);
    gtk_widget_set_visible(t->view_kernel, perf);
    if (t->window != nullptr && gtk_widget_get_realized(t->window))
        save_settings(t);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    taskmgr *t = data;
    (void)w;
    if (ev->keyval != GDK_KEY_Delete)
        return FALSE;
    int tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(t->notebook));
    if (tab == TAB_PROCS && gtk_widget_has_focus(t->procs_tree)) {
        cb_end_process(nullptr, t);
        return TRUE;
    }
    if (tab == TAB_APPS) {
        cb_end_task(nullptr, t);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *status_pane(GtkWidget **label, int width) {
    GtkWidget *f = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(f), GTK_SHADOW_IN);
    gtk_style_context_add_class(gtk_widget_get_style_context(f), "xd-sunken");
    *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(*label), 0);
    gtk_widget_set_margin_start(*label, 3);
    gtk_widget_set_margin_end(*label, 3);
    gtk_container_add(GTK_CONTAINER(f), *label);
    gtk_widget_set_size_request(f, width, -1);
    return f;
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    taskmgr *t = data;
    save_settings(t);
    if (t->timer != 0)
        g_source_remove(t->timer);
    xd_tasklist_free(t->tasks);
    xd_sysinfo_free(t->si);
    g_hash_table_unref(t->rows);
    xd_history_clear(&t->cpu_hist);
    xd_history_clear(&t->mem_hist);
    for (int i = 0; i < t->ncpu; i++)
        xd_history_clear(&t->cpu_each[i]);
    g_free(t->cpu_each);
    g_object_unref(t->apps);
    g_object_unref(t->procs);
    g_free(t);
}

static void open_window(GtkApplication *app) {
    taskmgr *t = g_new0(taskmgr, 1);
    t->app = app;
    t->si = xd_sysinfo_new();
    if (t->si == nullptr) {
        g_printerr("xd-taskmgr: cannot read /proc\n");
        g_free(t);
        return;
    }
    int tab = TAB_APPS;
    load_settings(t, &tab);

    t->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(t->window), "Task Manager");
    gtk_window_set_icon_name(GTK_WINDOW(t->window), "utilities-system-monitor");
    gtk_window_set_default_size(GTK_WINDOW(t->window), 430, 460);
    g_signal_connect(t->window, "destroy", G_CALLBACK(on_destroy), t);
    g_signal_connect(t->window, "key-press-event", G_CALLBACK(on_key), t);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(t->window), accel);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(t->window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), build_menus(t, accel), FALSE, FALSE, 0);
    t->notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(t->notebook), 5);
    gtk_notebook_append_page(GTK_NOTEBOOK(t->notebook), build_apps(t),
                             gtk_label_new_with_mnemonic("Applications"));
    gtk_notebook_append_page(GTK_NOTEBOOK(t->notebook), build_processes(t),
                             gtk_label_new_with_mnemonic("Processes"));
    gtk_notebook_append_page(GTK_NOTEBOOK(t->notebook), build_performance(t),
                             gtk_label_new_with_mnemonic("Performance"));
    gtk_box_pack_start(GTK_BOX(vbox), t->notebook, TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(t->window, GTK_NOTEBOOK(t->notebook));

    GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(sbar), 2);
    gtk_box_pack_start(GTK_BOX(sbar), status_pane(&t->st_procs, 95), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sbar), status_pane(&t->st_cpu, 115), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sbar), status_pane(&t->st_mem, 180), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(vbox), sbar, FALSE, FALSE, 0);

    apply_columns(t);
    g_signal_connect(t->window, "map-event", G_CALLBACK(apply_on_top), t);
    gtk_widget_show_all(t->window);
    set_apps_view(t, t->apps_view);
    g_signal_connect(t->notebook, "switch-page", G_CALLBACK(on_tab), t);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(t->notebook), tab);
    on_tab(GTK_NOTEBOOK(t->notebook), nullptr, (guint)tab, t);
    show_cpu_graphs(t);
    sample(t);
    set_speed(t, t->speed);
}

static void on_activate(GtkApplication *app, [[maybe_unused]] gpointer data) {
    GList *windows = gtk_application_get_windows(app);
    if (windows != nullptr) {
        gtk_window_present(windows->data);
        return;
    }
    open_window(app);
}

static void on_startup([[maybe_unused]] GApplication *app, [[maybe_unused]] gpointer data) {
    xd_gtk_app_follow_theme();
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.TaskManager");
    GtkApplication *app = gtk_application_new("org.xeondesktop.TaskManager",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
