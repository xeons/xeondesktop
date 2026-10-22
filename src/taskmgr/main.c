/* SPDX-License-Identifier: MIT
 * xd-taskmgr: the Windows 2000 Task Manager.
 *
 * Applications lists the open windows (End Task, Switch To, New Task),
 * Processes lists every process with selectable columns (End Process,
 * End Process Tree, Set Priority, Set Affinity), and Performance shows the
 * CPU and memory meters, history graphs and totals. Windows XP's
 * Networking (network.c) and Users (users.c) tabs follow. Ctrl+Alt+Delete
 * and Ctrl+Shift+Esc open it (handled by the compositor).
 */
#include <errno.h>
#include <gio/gdesktopappinfo.h>
#include <sched.h>
#include <signal.h>
#include <sys/pidfd.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/state.h"
#include "graph.h"
#include "sysinfo.h"
#include "taskmgr.h"
#include "trayicon.h"
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

enum { AP_TASK, AP_ICON16, AP_ICON32, AP_TITLE, AP_STATUS, AP_APP_ID, AP_COUNT };
enum { VIEW_LARGE, VIEW_SMALL, VIEW_DETAILS, VIEW_COUNT };
enum { SPEED_HIGH, SPEED_NORMAL, SPEED_LOW, SPEED_PAUSED, SPEED_COUNT };
static const guint speed_ms[SPEED_COUNT] = {500, 2000, 4000, 0};
enum { TAB_APPS, TAB_PROCS, TAB_PERF, TAB_NET, TAB_USERS };
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
    /* The CPU meter in the notification area; Hide When Minimized takes
     * the window off the taskbar while it's minimized, into the icon. */
    xd_trayicon *tray;
    gboolean hide_when_minimized, hidden;
    GtkWidget *hide_item;
    xd_task *own_task; /* our window in the window list */
    gboolean syncing;
    gboolean procs_stale; /* process list skipped while off screen */

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

    /* Networking and Users */
    xd_net_tab *net;
    xd_users_tab *users;

    /* Status bar */
    GtkWidget *st_procs, *st_cpu, *st_mem;
} taskmgr;

static void sample(taskmgr *task_manager);
static void refresh_processes(taskmgr *task_manager);

/* ---- helpers ---------------------------------------------------------------------- */

static void append_grouped(GString *out, guint64 value) {
    char digits[32];
    int digit_count = g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, value);
    for (int i = 0; i < digit_count; i++) {
        if (i > 0 && (digit_count - i) % 3 == 0)
            g_string_append_c(out, ',');
        g_string_append_c(out, digits[i]);
    }
}

/* A KiB amount in the chosen unit: "16,492 KB", "16.1 MB", "1.57 GB". */
static void append_memory(GString *out, guint64 kb, gboolean with_unit) {
    if (mem_unit == UNIT_KB)
        append_grouped(out, kb);
    else {
        double amount = (double)kb / (mem_unit == UNIT_MB ? 1024.0 : 1024.0 * 1024.0);
        g_string_append_printf(out, amount < 10 ? "%.2f" : amount < 1000 ? "%.1f" : "%.0f", amount);
    }
    if (with_unit) {
        g_string_append_c(out, ' ');
        g_string_append(out, unit_names[mem_unit]);
    }
}

static void set_memory(GtkWidget *label, guint64 kb) {
    GString *text = g_string_new(nullptr);
    append_memory(text, kb, FALSE);
    gtk_label_set_text(GTK_LABEL(label), text->str);
    g_string_free(text, TRUE);
}

static void set_number(GtkWidget *label, guint64 value) {
    GString *text = g_string_new(nullptr);
    append_grouped(text, value);
    gtk_label_set_text(GTK_LABEL(label), text->str);
    g_string_free(text, TRUE);
}

static void message(taskmgr *task_manager, xd_mb_icon icon, const char *title, const char *text) {
    xd_message_box(GTK_WINDOW(task_manager->window), title, icon, XD_MB_OK, FALSE, text);
}

/* Destructive questions: No is the default, as in Windows. */
static gboolean confirm(taskmgr *task_manager, const char *title, const char *text) {
    return xd_message_box(GTK_WINDOW(task_manager->window), title, XD_MB_WARNING, XD_MB_YES_NO,
                          TRUE, text) == GTK_RESPONSE_YES;
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

static void load_settings(taskmgr *task_manager, int *tab) {
    task_manager->speed = SPEED_NORMAL;
    task_manager->apps_view = VIEW_DETAILS;
    for (size_t i = 0; i < NCOLUMNS; i++)
        task_manager->col_on[i] = columns[i].on_by_default;
    *tab = TAB_APPS;
    GKeyFile *kf = g_key_file_new();
    xd_strbuf path = {0};
    settings_path(&path);
    if (g_key_file_load_from_file(kf, xd_strbuf_cstr(&path), G_KEY_FILE_NONE, nullptr)) {
        GError *err = nullptr;
        int value = g_key_file_get_integer(kf, "TaskManager", "Tab", &err);
        if (err == nullptr && value >= TAB_APPS && value <= TAB_USERS)
            *tab = value;
        g_clear_error(&err);
        value = g_key_file_get_integer(kf, "TaskManager", "UpdateSpeed", &err);
        if (err == nullptr && value >= 0 && value < SPEED_COUNT)
            task_manager->speed = value;
        g_clear_error(&err);
        value = g_key_file_get_integer(kf, "TaskManager", "ApplicationsView", &err);
        if (err == nullptr && value >= 0 && value < VIEW_COUNT)
            task_manager->apps_view = value;
        g_clear_error(&err);
        value = g_key_file_get_integer(kf, "TaskManager", "MemoryUnits", &err);
        if (err == nullptr && value >= 0 && value < UNIT_COUNT)
            mem_unit = value;
        g_clear_error(&err);
        task_manager->minimize_on_use =
            g_key_file_get_boolean(kf, "TaskManager", "MinimizeOnUse", nullptr);
        task_manager->on_top = g_key_file_get_boolean(kf, "TaskManager", "AlwaysOnTop", nullptr);
        if (g_key_file_has_key(kf, "TaskManager", "HideWhenMinimized", nullptr))
            task_manager->hide_when_minimized =
                g_key_file_get_boolean(kf, "TaskManager", "HideWhenMinimized", nullptr);
        task_manager->per_cpu =
            g_key_file_get_boolean(kf, "TaskManager", "OneGraphPerCPU", nullptr);
        task_manager->show_kernel =
            g_key_file_get_boolean(kf, "TaskManager", "ShowKernelTimes", nullptr);
        gsize count = 0;
        char **cols = g_key_file_get_string_list(kf, "TaskManager", "Columns", &count, nullptr);
        if (cols != nullptr) {
            for (size_t i = FIXED_COLUMNS; i < NCOLUMNS; i++)
                task_manager->col_on[i] =
                    g_strv_contains((const char *const *)cols, columns[i].key);
            g_strfreev(cols);
        }
    }
    xd_strbuf_free(&path);
    g_key_file_unref(kf);
}

static void save_settings(taskmgr *task_manager) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "TaskManager", "Tab",
                           gtk_notebook_get_current_page(GTK_NOTEBOOK(task_manager->notebook)));
    g_key_file_set_integer(kf, "TaskManager", "UpdateSpeed", task_manager->speed);
    g_key_file_set_integer(kf, "TaskManager", "ApplicationsView", task_manager->apps_view);
    g_key_file_set_integer(kf, "TaskManager", "MemoryUnits", mem_unit);
    g_key_file_set_boolean(kf, "TaskManager", "MinimizeOnUse", task_manager->minimize_on_use);
    g_key_file_set_boolean(kf, "TaskManager", "AlwaysOnTop", task_manager->on_top);
    g_key_file_set_boolean(kf, "TaskManager", "HideWhenMinimized",
                           task_manager->hide_when_minimized);
    g_key_file_set_boolean(kf, "TaskManager", "OneGraphPerCPU", task_manager->per_cpu);
    g_key_file_set_boolean(kf, "TaskManager", "ShowKernelTimes", task_manager->show_kernel);
    const char *on[NCOLUMNS];
    gsize count = 0;
    for (size_t i = FIXED_COLUMNS; i < NCOLUMNS; i++) {
        if (task_manager->col_on[i])
            on[count++] = columns[i].key;
    }
    g_key_file_set_string_list(kf, "TaskManager", "Columns", on, count);
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
            GIcon *app_icon = g_app_info_get_icon(G_APP_INFO(ai));
            gicon = app_icon != nullptr ? g_object_ref(app_icon) : nullptr;
            g_object_unref(ai);
        }
    }
    *small = load_icon(gicon, app_id, 16);
    *large = load_icon(gicon, app_id, 32);
    g_clear_object(&gicon);
}

static gboolean hide_idle(gpointer data) {
    taskmgr *task_manager = data;
    if (!task_manager->hidden && xd_trayicon_shown(task_manager->tray)) {
        task_manager->hidden = TRUE;
        gtk_widget_hide(task_manager->window);
    }
    return G_SOURCE_REMOVE;
}

static void on_task_changed(xd_task *task, gpointer data) {
    taskmgr *task_manager = data;
    /* Our own window (GApplication keeps us to one): minimized with Hide
     * When Minimized, it leaves the taskbar for the notification area. */
    if (g_strcmp0(task->app_id, "org.xeondesktop.TaskManager") == 0) {
        task_manager->own_task = task;
        if (task->minimized && task_manager->hide_when_minimized &&
            xd_trayicon_shown(task_manager->tray))
            g_idle_add(hide_idle, task_manager);
        refresh_processes(task_manager);
    }
    GtkTreeIter *iter = task->user;
    gboolean new_row = iter == nullptr;
    if (new_row) {
        iter = g_new(GtkTreeIter, 1);
        gtk_list_store_append(task_manager->apps, iter);
        task->user = iter;
    }
    const char *title = task->title != nullptr && *task->title != '\0' ? task->title
                        : task->app_id != nullptr                     ? task->app_id
                                                                      : "";
    /* The compositor adds " (Not Responding)" to a hung window's title,
     * as Windows does; its status says so. */
    gboolean hung = g_str_has_suffix(title, " (Not Responding)");
    gtk_list_store_set(task_manager->apps, iter, AP_TASK, task, AP_TITLE, title, AP_STATUS,
                       hung ? "Not Responding" : "Running", -1);

    /* Titles change often (terminals, browsers); the icons only with the
     * app id, and finding them reads .desktop files and the icon theme. */
    char *old_id = nullptr;
    if (!new_row)
        gtk_tree_model_get(GTK_TREE_MODEL(task_manager->apps), iter, AP_APP_ID, &old_id, -1);
    if (new_row || g_strcmp0(old_id, task->app_id) != 0) {
        GdkPixbuf *small = nullptr, *large = nullptr;
        task_icons(task->app_id, &small, &large);
        gtk_list_store_set(task_manager->apps, iter, AP_ICON16, small, AP_ICON32, large, AP_APP_ID,
                           task->app_id, -1);
        g_clear_object(&small);
        g_clear_object(&large);
    }
    g_free(old_id);
}

static void on_task_closed(xd_task *task, gpointer data) {
    taskmgr *task_manager = data;
    if (task == task_manager->own_task)
        task_manager->own_task = nullptr;
    GtkTreeIter *iter = task->user;
    if (iter != nullptr) {
        gtk_list_store_remove(task_manager->apps, iter);
        g_free(iter);
        task->user = nullptr;
    }
}

/* The windows selected on the Applications tab (xd_task*, in list
 * order; several with Ctrl or Shift, as in Windows). g_list_free it. */
static GList *selected_tasks(taskmgr *task_manager) {
    GtkWidget *view = task_manager->apps_views[task_manager->apps_view];
    GList *paths = task_manager->apps_view == VIEW_DETAILS
                       ? gtk_tree_selection_get_selected_rows(
                             gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), nullptr)
                       : gtk_icon_view_get_selected_items(GTK_ICON_VIEW(view));
    GList *tasks = nullptr;
    for (GList *list = paths; list != nullptr; list = list->next) {
        GtkTreeIter iter;
        xd_task *task = nullptr;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(task_manager->apps), &iter, list->data))
            gtk_tree_model_get(GTK_TREE_MODEL(task_manager->apps), &iter, AP_TASK, &task, -1);
        if (task != nullptr)
            tasks = g_list_prepend(tasks, task);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return g_list_reverse(tasks);
}

static xd_task *selected_task(taskmgr *task_manager) {
    GList *tasks = selected_tasks(task_manager);
    xd_task *first = tasks != nullptr ? tasks->data : nullptr;
    g_list_free(tasks);
    return first;
}

static void update_app_buttons(taskmgr *task_manager) {
    gboolean any = selected_task(task_manager) != nullptr;
    gtk_widget_set_sensitive(task_manager->end_task, any);
    gtk_widget_set_sensitive(task_manager->switch_to, any);
    /* Tile and Cascade need a selection and our compositor's arranger. */
    gboolean arrange = any && xd_tasklist_can_arrange(task_manager->tasks);
    GtkWidget *items[] = {task_manager->tile_h_item, task_manager->tile_v_item,
                          task_manager->cascade_item};
    for (size_t i = 0; i < G_N_ELEMENTS(items); i++)
        if (items[i] != nullptr)
            gtk_widget_set_sensitive(items[i], arrange);
}

static void switch_to(taskmgr *task_manager, gboolean minimize_self) {
    xd_task *task = selected_task(task_manager);
    if (task == nullptr)
        return;
    if (task->minimized)
        xd_task_set_minimized(task, FALSE);
    xd_task_activate(task);
    if (minimize_self && task_manager->minimize_on_use)
        gtk_window_iconify(GTK_WINDOW(task_manager->window));
}

static void cb_switch_to([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    switch_to(user_data, TRUE);
}
static void cb_bring_to_front([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    switch_to(user_data, FALSE);
}

/* End Task, Minimize and Maximize act on every selected window. */
static void cb_end_task([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    GList *tasks = selected_tasks(user_data);
    for (GList *list = tasks; list != nullptr; list = list->next)
        xd_task_close(list->data);
    g_list_free(tasks);
}

static void cb_minimize_task([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    GList *tasks = selected_tasks(user_data);
    for (GList *list = tasks; list != nullptr; list = list->next)
        xd_task_set_minimized(list->data, TRUE);
    g_list_free(tasks);
}

static void cb_maximize_task([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    GList *tasks = selected_tasks(user_data);
    for (GList *list = tasks; list != nullptr; list = list->next) {
        xd_task *task = list->data;
        if (task->minimized)
            xd_task_set_minimized(task, FALSE);
        xd_task_set_maximized(task, TRUE);
    }
    g_list_free(tasks);
}

/* Windows ▸ Tile and Cascade: the selected windows. */
static void arrange_selected(taskmgr *task_manager, xd_arrange_mode mode) {
    GList *tasks = selected_tasks(task_manager);
    xd_tasklist_arrange(task_manager->tasks, tasks, mode);
    g_list_free(tasks);
}

static void cb_tile_h([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    arrange_selected(user_data, XD_ARRANGE_TILE_HORIZONTALLY);
}
static void cb_tile_v([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    arrange_selected(user_data, XD_ARRANGE_TILE_VERTICALLY);
}
static void cb_cascade([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    arrange_selected(user_data, XD_ARRANGE_CASCADE);
}

static void cb_new_task([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    taskmgr *task_manager = user_data;
    char *run = own_program("xd-run");
    char *argv[] = {run, nullptr};
    GError *err = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &err)) {
        message(task_manager, XD_MB_ERROR, "Create New Task", err->message);
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

static void apps_context_menu(taskmgr *task_manager, GdkEvent *event) {
    GtkWidget *menu = gtk_menu_new();
    if (selected_task(task_manager) != nullptr) {
        menu_item(menu, "_Switch To", G_CALLBACK(cb_switch_to), task_manager, nullptr, 0, NO_MODS);
        menu_item(menu, "_Bring To Front", G_CALLBACK(cb_bring_to_front), task_manager, nullptr, 0,
                  NO_MODS);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "Mi_nimize", G_CALLBACK(cb_minimize_task), task_manager, nullptr, 0,
                  NO_MODS);
        menu_item(menu, "Ma_ximize", G_CALLBACK(cb_maximize_task), task_manager, nullptr, 0,
                  NO_MODS);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_item(menu, "_End Task", G_CALLBACK(cb_end_task), task_manager, nullptr, 0, NO_MODS);
    } else {
        menu_item(menu, "_New Task (Run...)", G_CALLBACK(cb_new_task), task_manager, nullptr, 0,
                  NO_MODS);
    }
    popup(menu, task_manager->apps_views[task_manager->apps_view], event);
}

/* Right-click selects what's under the pointer, then shows the menu. */
static gboolean on_apps_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    taskmgr *task_manager = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (GTK_IS_TREE_VIEW(widget)) {
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path,
                                          nullptr, nullptr, nullptr))
            gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)),
                                           path);
        else
            gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)));
    } else {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget), (int)ev->x, (int)ev->y);
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
        if (path != nullptr)
            gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
    }
    if (path != nullptr)
        gtk_tree_path_free(path);
    apps_context_menu(task_manager, (GdkEvent *)ev);
    return TRUE;
}

static void on_apps_selection([[maybe_unused]] GtkWidget *widget, gpointer data) {
    update_app_buttons(data);
}

static void on_apps_row_activated([[maybe_unused]] GtkTreeView *tv,
                                  [[maybe_unused]] GtkTreePath *path,
                                  [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    switch_to(data, TRUE);
}

static void on_apps_item_activated([[maybe_unused]] GtkIconView *iv,
                                   [[maybe_unused]] GtkTreePath *path, gpointer data) {
    switch_to(data, TRUE);
}

static void set_apps_view(taskmgr *task_manager, int view) {
    task_manager->apps_view = view;
    static const char *const names[VIEW_COUNT] = {"large", "small", "details"};
    gtk_stack_set_visible_child_name(GTK_STACK(task_manager->apps_stack), names[view]);
    update_app_buttons(task_manager);
}

static GtkWidget *build_apps(taskmgr *task_manager) {
    task_manager->apps =
        gtk_list_store_new(AP_COUNT, G_TYPE_POINTER, GDK_TYPE_PIXBUF, GDK_TYPE_PIXBUF,
                           G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    /* Details */
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(task_manager->apps));
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
                     G_CALLBACK(on_apps_selection), task_manager);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_apps_row_activated), task_manager);
    task_manager->apps_views[VIEW_DETAILS] = tree;

    /* Large and small icons */
    for (int view = VIEW_LARGE; view <= VIEW_SMALL; view++) {
        GtkWidget *iv = gtk_icon_view_new_with_model(GTK_TREE_MODEL(task_manager->apps));
        gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(iv),
                                        view == VIEW_LARGE ? AP_ICON32 : AP_ICON16);
        gtk_icon_view_set_text_column(GTK_ICON_VIEW(iv), AP_TITLE);
        gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(iv), GTK_SELECTION_MULTIPLE);
        if (view == VIEW_SMALL) {
            gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(iv), GTK_ORIENTATION_HORIZONTAL);
            gtk_icon_view_set_item_width(GTK_ICON_VIEW(iv), 160);
        } else {
            gtk_icon_view_set_item_width(GTK_ICON_VIEW(iv), 76);
        }
        gtk_icon_view_set_margin(GTK_ICON_VIEW(iv), 4);
        g_signal_connect(iv, "selection-changed", G_CALLBACK(on_apps_selection), task_manager);
        g_signal_connect(iv, "item-activated", G_CALLBACK(on_apps_item_activated), task_manager);
        task_manager->apps_views[view] = iv;
    }

    task_manager->apps_stack = gtk_stack_new();
    static const char *const names[VIEW_COUNT] = {"large", "small", "details"};
    for (int view = 0; view < VIEW_COUNT; view++) {
        g_signal_connect(task_manager->apps_views[view], "button-press-event",
                         G_CALLBACK(on_apps_button), task_manager);
        gtk_stack_add_named(GTK_STACK(task_manager->apps_stack),
                            xd_tm_scrolled(task_manager->apps_views[view]), names[view]);
    }
    gtk_box_pack_start(GTK_BOX(box), task_manager->apps_stack, TRUE, TRUE, 0);

    task_manager->end_task = gtk_button_new_with_mnemonic("_End Task");
    task_manager->switch_to = gtk_button_new_with_mnemonic("_Switch To");
    GtkWidget *new_task = gtk_button_new_with_mnemonic("_New Task...");
    g_signal_connect(task_manager->end_task, "clicked", G_CALLBACK(cb_end_task), task_manager);
    g_signal_connect(task_manager->switch_to, "clicked", G_CALLBACK(cb_switch_to), task_manager);
    g_signal_connect(new_task, "clicked", G_CALLBACK(cb_new_task), task_manager);
    GtkWidget *buttons[] = {task_manager->end_task, task_manager->switch_to, new_task};
    gtk_box_pack_start(GTK_BOX(box), xd_tm_button_row(buttons, 3), FALSE, FALSE, 0);

    task_manager->tasks =
        xd_tasklist_new(gdk_display_get_default(), on_task_changed, on_task_closed, task_manager);
    if (task_manager->on_top_item != nullptr)
        gtk_widget_set_sensitive(task_manager->on_top_item,
                                 xd_tasklist_can_set_on_top(task_manager->tasks));
    if (!xd_tasklist_available(task_manager->tasks)) {
        gtk_widget_set_tooltip_text(task_manager->apps_stack,
                                    "The window list needs the XeonDesktop compositor.");
    }
    return box;
}

/* ---- Processes tab ---------------------------------------------------------------- */

static void format_cell([[maybe_unused]] GtkTreeViewColumn *col, GtkCellRenderer *cell,
                        GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    const column_def *column = &columns[GPOINTER_TO_INT(data)];
    char buf[64];
    GString *cell_text = nullptr;
    switch (column->format) {
    case F_TEXT: {
        char *text = nullptr;
        gtk_tree_model_get(model, iter, column->store, &text, -1);
        g_object_set(cell, "text", text != nullptr ? text : "", nullptr);
        g_free(text);
        return;
    }
    case F_INT:
    case F_PERCENT:
    case F_PRIO: {
        int value = 0;
        gtk_tree_model_get(model, iter, column->store, &value, -1);
        if (column->format == F_PRIO)
            g_object_set(cell, "text", priority_name(value), nullptr);
        else {
            g_snprintf(buf, sizeof(buf), column->format == F_PERCENT ? "%02d" : "%d", value);
            g_object_set(cell, "text", buf, nullptr);
        }
        return;
    }
    case F_TIME: {
        guint64 ticks = 0;
        gtk_tree_model_get(model, iter, column->store, &ticks, -1);
        guint64 secs = ticks / (guint64)xd_sysinfo_hz();
        g_snprintf(buf, sizeof(buf), "%" G_GUINT64_FORMAT ":%02u:%02u", secs / 3600,
                   (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
        g_object_set(cell, "text", buf, nullptr);
        return;
    }
    case F_KB:
    case F_COUNT: {
        guint64 value = 0;
        if (column->store == PS_HANDLES) {
            int handle_count = 0;
            gtk_tree_model_get(model, iter, column->store, &handle_count, -1);
            value = handle_count > 0 ? (guint64)handle_count : 0;
        } else if (column->store == PS_THREADS) {
            int th = 0;
            gtk_tree_model_get(model, iter, column->store, &th, -1);
            value = th > 0 ? (guint64)th : 0;
        } else {
            gtk_tree_model_get(model, iter, column->store, &value, -1);
        }
        cell_text = g_string_new(nullptr);
        if (column->format == F_KB)
            append_memory(cell_text, value, TRUE);
        else
            append_grouped(cell_text, value);
        break;
    }
    case F_KB_DELTA: {
        gint64 value = 0;
        gtk_tree_model_get(model, iter, column->store, &value, -1);
        cell_text = g_string_new(value < 0 ? "-" : "");
        append_memory(cell_text, value < 0 ? (guint64)(-value) : (guint64)value, TRUE);
        break;
    }
    }
    g_object_set(cell, "text", cell_text->str, nullptr);
    g_string_free(cell_text, TRUE);
}

static void apply_columns(taskmgr *task_manager) {
    for (size_t i = 0; i < NCOLUMNS; i++)
        gtk_tree_view_column_set_visible(task_manager->proc_cols[i], task_manager->col_on[i]);
    gboolean handles = FALSE, io = FALSE, peak = FALSE;
    for (size_t i = 0; i < NCOLUMNS; i++) {
        if (task_manager->col_on[i] && columns[i].store == PS_HANDLES)
            handles = TRUE;
        if (task_manager->col_on[i] && (columns[i].store == PS_IOR || columns[i].store == PS_IOW))
            io = TRUE;
        if (task_manager->col_on[i] && columns[i].store == PS_PEAK)
            peak = TRUE;
    }
    xd_sysinfo_set_extras(task_manager->si, handles, io);
    xd_sysinfo_set_peak(task_manager->si, peak);
}

static void update_processes(taskmgr *task_manager) {
    guint count = 0;
    const xd_proc *procs = xd_sysinfo_procs(task_manager->si, &count);
    const xd_sysstats *st = xd_sysinfo_stats(task_manager->si);
    guint64 gen = ++task_manager->generation;
    guint64 total = st->total_delta;

    for (guint i = 0; i <= count; i++) {
        /* i == n: Windows' System Idle Process, from the idle time. */
        xd_proc idle = {
            .pid = 0,
            .name = "System Idle Process",
            .user = "SYSTEM",
            .cpu_ticks = st->idle_ticks,
            .handles = 0,
        };
        const xd_proc *proc = i < count ? &procs[i] : &idle;
        int cpu =
            i < count
                ? (total > 0 ? (int)((guint64)proc->cpu_delta * 100 + total / 2) / (int)total : 0)
                : (int)(st->idle * 100 + 0.5);
        cpu = CLAMP(cpu, 0, 99);
        GtkTreeIter *iter = g_hash_table_lookup(task_manager->rows, GINT_TO_POINTER(proc->pid));
        /* The kernel's lifetime peak (VmHWM), as Windows' Peak Mem Usage;
         * what we've seen covers processes without one. */
        guint64 old_mem = proc->mem_kb, old_faults = proc->faults,
                peak = MAX(proc->mem_kb, proc->peak_kb);
        if (iter == nullptr) {
            iter = g_new(GtkTreeIter, 1);
            gtk_list_store_append(task_manager->procs, iter);
            g_hash_table_insert(task_manager->rows, GINT_TO_POINTER(proc->pid), iter);
        } else {
            guint64 old_peak = 0;
            gtk_tree_model_get(GTK_TREE_MODEL(task_manager->procs), iter, PS_MEM, &old_mem,
                               PS_FAULTS, &old_faults, PS_PEAK, &old_peak, -1);
            peak = MAX(peak, old_peak);
        }
        gint64 mem_delta = (gint64)proc->mem_kb - (gint64)old_mem;
        guint64 fault_delta = proc->faults >= old_faults ? proc->faults - old_faults : 0;
        guint64 cpu_ticks = proc->cpu_ticks, mem = proc->mem_kb, vm = proc->vm_kb,
                faults = proc->faults;
        guint64 ior = proc->io_read, iow = proc->io_write;
        gtk_list_store_set(task_manager->procs, iter, PS_PID, proc->pid, PS_NAME, proc->name,
                           PS_USER, proc->user, PS_CPU, cpu, PS_TIME, cpu_ticks, PS_MEM, mem,
                           PS_MEM_DELTA, mem_delta, PS_PEAK, peak, PS_FAULTS, faults,
                           PS_FAULTS_DELTA, fault_delta, PS_VM, vm, PS_PRIO, proc->nice, PS_THREADS,
                           proc->threads, PS_HANDLES, proc->handles, PS_IOR, ior, PS_IOW, iow,
                           PS_GEN, gen, -1);
    }

    /* Drop processes that have exited. */
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, task_manager->rows);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        guint64 seen = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(task_manager->procs), value, PS_GEN, &seen, -1);
        if (seen != gen) {
            gtk_list_store_remove(task_manager->procs, value);
            g_hash_table_iter_remove(&it);
        }
    }
}

static int selected_pid(taskmgr *task_manager) {
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(task_manager->procs_tree)), nullptr, &iter))
        return -1;
    int pid = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(task_manager->procs), &iter, PS_PID, &pid, -1);
    return pid;
}

static void kill_failed(taskmgr *task_manager, int err) {
    char *text = g_strdup_printf("The operation could not be completed.\n\n%s", errno_text(err));
    message(task_manager, XD_MB_ERROR, "Unable to Terminate Process", text);
    g_free(text);
}

static void cb_end_process([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    taskmgr *task_manager = user_data;
    int pid = selected_pid(task_manager);
    if (pid <= 0)
        return;
    /* The process chosen, held before asking: by the answer it may have
     * ended and its PID gone to another. */
    int pidfd = pidfd_open(pid, 0);
    if (pidfd < 0) {
        if (errno != ESRCH)
            kill_failed(task_manager, errno);
        sample(task_manager);
        return;
    }
    if (!confirm(task_manager, "Task Manager Warning",
                 "WARNING: Terminating a process can cause undesired results including loss of "
                 "data and system instability. The process will not be given the chance to save "
                 "its state or data before it is terminated. Are you sure you want to terminate "
                 "the process?")) {
        close(pidfd);
        return;
    }
    if (pidfd_send_signal(pidfd, SIGKILL, nullptr, 0) != 0 && errno != ESRCH)
        kill_failed(task_manager, errno);
    else
        sample(task_manager);
    close(pidfd);
}

/* Children before parents, so a parent can't restart them. */
static void collect_tree(const xd_proc *procs, guint count, int pid, GArray *out, int depth) {
    if (depth > 64)
        return;
    for (guint i = 0; i < count; i++) {
        if (procs[i].ppid == pid && procs[i].pid != pid)
            collect_tree(procs, count, procs[i].pid, out, depth + 1);
    }
    g_array_append_val(out, pid);
}

static void cb_end_tree([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    taskmgr *task_manager = user_data;
    int pid = selected_pid(task_manager);
    if (pid <= 0)
        return;
    if (!confirm(task_manager, "Task Manager Warning",
                 "WARNING: Terminating a process tree can cause undesired results including loss "
                 "of data and system instability. The processes will not be given the chance to "
                 "save their state or data before they are terminated. Are you sure you want to "
                 "terminate the process tree?"))
        return;
    /* The tree as it is now, not as the last refresh saw it (while the
     * warning was up, its processes may have ended, their PIDs reused). */
    sample(task_manager);
    guint count = 0;
    const xd_proc *procs = xd_sysinfo_procs(task_manager->si, &count);
    GArray *pids = g_array_new(FALSE, FALSE, sizeof(int));
    collect_tree(procs, count, pid, pids, 0);
    int first_err = 0;
    for (guint i = 0; i < pids->len; i++) {
        if (kill(g_array_index(pids, int, i), SIGKILL) != 0 && first_err == 0 && errno != ESRCH)
            first_err = errno;
    }
    g_array_unref(pids);
    if (first_err != 0)
        kill_failed(task_manager, first_err);
    sample(task_manager);
}

static void cb_priority(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    if (task_manager->syncing || !gtk_check_menu_item_get_active(item))
        return;
    int pid = selected_pid(task_manager);
    int nice = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "nice"));
    if (pid <= 0)
        return;
    if (!confirm(task_manager, "Task Manager Warning",
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
        message(task_manager, XD_MB_ERROR, "Unable to Change Priority", text);
        g_free(text);
    }
    sample(task_manager);
}

static void cb_affinity([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    taskmgr *task_manager = user_data;
    int pid = selected_pid(task_manager);
    if (pid <= 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(pid, sizeof(set), &set) != 0) {
        message(task_manager, XD_MB_ERROR, "Unable to Access Process", errno_text(errno));
        return;
    }
    int ncpu = MAX(task_manager->ncpu, 1);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Processor Affinity", GTK_WINDOW(task_manager->window), GTK_DIALOG_MODAL, "OK",
        GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
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
            message(task_manager, XD_MB_ERROR, "Invalid Option",
                    "The process must have affinity with at least one processor.");
            continue;
        }
        if (sched_setaffinity(pid, sizeof(set), &set) != 0)
            message(task_manager, XD_MB_ERROR, "Unable to Change Affinity", errno_text(errno));
        break;
    }
    g_free(checks);
    gtk_widget_destroy(dlg);
}

static void procs_context_menu(taskmgr *task_manager, GdkEvent *event) {
    int pid = selected_pid(task_manager);
    if (pid <= 0)
        return;
    int nice = 0;
    guint count = 0;
    const xd_proc *procs = xd_sysinfo_procs(task_manager->si, &count);
    for (guint i = 0; i < count; i++) {
        if (procs[i].pid == pid)
            nice = procs[i].nice;
    }
    GtkWidget *menu = gtk_menu_new();
    menu_item(menu, "_End Process", G_CALLBACK(cb_end_process), task_manager, nullptr, 0, NO_MODS);
    menu_item(menu, "End Process _Tree", G_CALLBACK(cb_end_tree), task_manager, nullptr, 0,
              NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *prio = gtk_menu_new();
    GtkWidget *top = gtk_menu_item_new_with_mnemonic("Set _Priority");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), prio);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
    GSList *group = nullptr;
    task_manager->syncing = TRUE;
    for (size_t i = 0; i < G_N_ELEMENTS(priorities); i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(group, priorities[i].name);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "nice", GINT_TO_POINTER(priorities[i].nice));
        if (g_str_equal(priority_name(priorities[i].nice), priority_name(nice)))
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        g_signal_connect(item, "toggled", G_CALLBACK(cb_priority), task_manager);
        gtk_menu_shell_append(GTK_MENU_SHELL(prio), item);
    }
    task_manager->syncing = FALSE;
    if (task_manager->ncpu > 1)
        menu_item(menu, "Set _Affinity...", G_CALLBACK(cb_affinity), task_manager, nullptr, 0,
                  NO_MODS);
    popup(menu, task_manager->procs_tree, event);
}

static gboolean on_procs_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    taskmgr *task_manager = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path, nullptr,
                                      nullptr, nullptr)) {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), path);
        gtk_tree_path_free(path);
        procs_context_menu(task_manager, (GdkEvent *)ev);
    }
    return TRUE;
}

static void on_procs_selection(GtkTreeSelection *sel, gpointer data) {
    taskmgr *task_manager = data;
    gtk_widget_set_sensitive(task_manager->end_process,
                             gtk_tree_selection_get_selected(sel, nullptr, nullptr) &&
                                 selected_pid(task_manager) > 0);
}

static void cb_select_columns([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    taskmgr *task_manager = user_data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Select Columns", GTK_WINDOW(task_manager->window),
                                                 GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK, "Cancel",
                                                 GTK_RESPONSE_CANCEL, nullptr);
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
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checks[i]), task_manager->col_on[i]);
        gtk_box_pack_start(GTK_BOX(box), checks[i], FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        for (size_t i = FIXED_COLUMNS; i < NCOLUMNS; i++)
            task_manager->col_on[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks[i]));
        apply_columns(task_manager);
        save_settings(task_manager);
        sample(task_manager);
    }
    gtk_widget_destroy(dlg);
}

static GtkWidget *build_processes(taskmgr *task_manager) {
    task_manager->procs = gtk_list_store_new(
        PS_COUNT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT64,
        G_TYPE_UINT64, G_TYPE_INT64, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64,
        G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64);
    task_manager->rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, g_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    task_manager->procs_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(task_manager->procs));
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(task_manager->procs_tree), TRUE);
    /* Typing a name selects the process, as in Windows. */
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(task_manager->procs_tree), PS_NAME);
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
        task_manager->proc_cols[i] = col;
        gtk_tree_view_append_column(GTK_TREE_VIEW(task_manager->procs_tree), col);
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(task_manager->procs_tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_procs_selection), task_manager);
    g_signal_connect(task_manager->procs_tree, "button-press-event", G_CALLBACK(on_procs_button),
                     task_manager);
    gtk_box_pack_start(GTK_BOX(box), xd_tm_scrolled(task_manager->procs_tree), TRUE, TRUE, 0);
    task_manager->end_process = gtk_button_new_with_mnemonic("_End Process");
    gtk_widget_set_sensitive(task_manager->end_process, FALSE);
    g_signal_connect(task_manager->end_process, "clicked", G_CALLBACK(cb_end_process),
                     task_manager);
    GtkWidget *buttons[] = {task_manager->end_process};
    gtk_box_pack_start(GTK_BOX(box), xd_tm_button_row(buttons, 1), FALSE, FALSE, 0);
    return box;
}

/* ---- Performance tab -------------------------------------------------------------- */

static gboolean draw_cpu_meter(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    taskmgr *task_manager = data;
    double x, y, wd, ht;
    xd_tm_draw_well(widget, cairo, &x, &y, &wd, &ht);
    xd_meter_draw(cairo, x, y, wd, ht, task_manager->cpu_now, task_manager->kernel_now,
                  task_manager->show_kernel, task_manager->cpu_text);
    return TRUE;
}

static gboolean draw_mem_meter(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    taskmgr *task_manager = data;
    double x, y, wd, ht;
    xd_tm_draw_well(widget, cairo, &x, &y, &wd, &ht);
    xd_meter_draw(cairo, x, y, wd, ht, task_manager->mem_now, 0, FALSE, task_manager->mem_text);
    return TRUE;
}

static gboolean draw_history(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    taskmgr *task_manager = g_object_get_data(G_OBJECT(widget), "taskmgr");
    const xd_history *history = data;
    double x, y, wd, ht;
    xd_tm_draw_well(widget, cairo, &x, &y, &wd, &ht);
    xd_history_draw(cairo, x, y, wd, ht, history,
                    history != &task_manager->mem_hist && task_manager->show_kernel);
    return TRUE;
}

static GtkWidget *graph_area(taskmgr *task_manager, xd_history *history, int min_w, int min_h) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, min_w, min_h);
    g_object_set_data(G_OBJECT(area), "taskmgr", task_manager);
    g_signal_connect(area, "draw", G_CALLBACK(draw_history), history);
    return area;
}

/* A group of "Name  value" rows; values are right-aligned. */
static GtkWidget *info_group(const char *title, const char *const *names, GtkWidget **values,
                             int count) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (int i = 0; i < count; i++) {
        GtkWidget *name = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_set_hexpand(name, TRUE);
        values[i] = gtk_label_new("0");
        gtk_label_set_xalign(GTK_LABEL(values[i]), 1);
        gtk_grid_attach(GTK_GRID(grid), name, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), values[i], 1, i, 1, 1);
    }
    return xd_tm_group(title, grid);
}

static void rebuild_cpu_graphs(taskmgr *task_manager) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(task_manager->cpu_graphs_box));
    for (GList *list = kids; list != nullptr; list = list->next)
        gtk_widget_destroy(list->data);
    g_list_free(kids);
    for (int i = 0; i < task_manager->ncpu; i++)
        gtk_box_pack_start(GTK_BOX(task_manager->cpu_graphs_box),
                           graph_area(task_manager, &task_manager->cpu_each[i], 20, 60), TRUE, TRUE,
                           0);
    gtk_widget_show_all(task_manager->cpu_graphs_box);
}

static void show_cpu_graphs(taskmgr *task_manager) {
    gtk_widget_set_visible(task_manager->cpu_graph,
                           !task_manager->per_cpu || task_manager->ncpu <= 1);
    gtk_widget_set_visible(task_manager->cpu_graphs_box,
                           task_manager->per_cpu && task_manager->ncpu > 1);
}

static GtkWidget *build_performance(taskmgr *task_manager) {
    xd_history_init(&task_manager->cpu_hist, HISTORY);
    xd_history_init(&task_manager->mem_hist, HISTORY);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    GtkWidget *top = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(top), 6);
    gtk_grid_set_column_spacing(GTK_GRID(top), 8);
    gtk_grid_set_row_homogeneous(GTK_GRID(top), TRUE);
    task_manager->cpu_meter = gtk_drawing_area_new();
    gtk_widget_set_size_request(task_manager->cpu_meter, 70, 80);
    g_signal_connect(task_manager->cpu_meter, "draw", G_CALLBACK(draw_cpu_meter), task_manager);
    task_manager->mem_meter = gtk_drawing_area_new();
    gtk_widget_set_size_request(task_manager->mem_meter, 70, 80);
    g_signal_connect(task_manager->mem_meter, "draw", G_CALLBACK(draw_mem_meter), task_manager);
    task_manager->cpu_graph = graph_area(task_manager, &task_manager->cpu_hist, 120, 80);
    gtk_widget_set_hexpand(task_manager->cpu_graph, TRUE);
    gtk_widget_set_vexpand(task_manager->cpu_graph, TRUE);
    task_manager->cpu_graphs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_set_homogeneous(GTK_BOX(task_manager->cpu_graphs_box), TRUE);
    gtk_widget_set_no_show_all(task_manager->cpu_graphs_box, TRUE);
    GtkWidget *cpu_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(cpu_hbox), task_manager->cpu_graph, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cpu_hbox), task_manager->cpu_graphs_box, TRUE, TRUE, 0);
    task_manager->mem_graph = graph_area(task_manager, &task_manager->mem_hist, 120, 80);
    gtk_widget_set_hexpand(task_manager->mem_graph, TRUE);
    gtk_grid_attach(GTK_GRID(top), xd_tm_group("CPU Usage", task_manager->cpu_meter), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(top), xd_tm_group("CPU Usage History", cpu_hbox), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(top), xd_tm_group("MEM Usage", task_manager->mem_meter), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(top), xd_tm_group("Memory Usage History", task_manager->mem_graph), 1,
                    1, 1, 1);
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
    GtkWidget *value_labels[3];
    gtk_grid_attach(GTK_GRID(bottom), info_group("Totals", totals, value_labels, 3), 0, 0, 1, 1);
    task_manager->handles = value_labels[0];
    task_manager->threads = value_labels[1];
    task_manager->processes = value_labels[2];
    task_manager->phys_frame = info_group("Physical Memory", phys, value_labels, 3);
    gtk_widget_set_tooltip_text(task_manager->phys_frame,
                                "MemTotal, MemAvailable, and Cached + Buffers");
    gtk_grid_attach(GTK_GRID(bottom), task_manager->phys_frame, 1, 0, 1, 1);
    task_manager->phys_total = value_labels[0];
    task_manager->phys_avail = value_labels[1];
    task_manager->phys_cache = value_labels[2];
    task_manager->commit_frame = info_group("Committed Memory", commit, value_labels, 3);
    gtk_widget_set_tooltip_text(task_manager->commit_frame,
                                "Memory promised to processes, whether or not they've used it "
                                "yet (Committed_AS), and the limit (CommitLimit), which is only "
                                "enforced in strict overcommit mode.");
    gtk_grid_attach(GTK_GRID(bottom), task_manager->commit_frame, 0, 1, 1, 1);
    task_manager->commit_total = value_labels[0];
    task_manager->commit_limit = value_labels[1];
    task_manager->commit_peak = value_labels[2];
    task_manager->kern_frame = info_group("Kernel Memory", kern, value_labels, 3);
    gtk_widget_set_tooltip_text(task_manager->kern_frame,
                                "Slab memory the kernel can free under pressure (SReclaimable), "
                                "and slab, stacks and page tables it can't (SUnreclaim, "
                                "KernelStack, PageTables).");
    gtk_grid_attach(GTK_GRID(bottom), task_manager->kern_frame, 1, 1, 1, 1);
    task_manager->kern_total = value_labels[0];
    task_manager->kern_paged = value_labels[1];
    task_manager->kern_nonpaged = value_labels[2];
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    return box;
}

static void update_performance(taskmgr *task_manager) {
    const xd_sysstats *st = xd_sysinfo_stats(task_manager->si);
    if (st->ncpu != task_manager->ncpu) {
        for (int i = 0; i < task_manager->ncpu; i++)
            xd_history_clear(&task_manager->cpu_each[i]);
        g_free(task_manager->cpu_each);
        task_manager->ncpu = st->ncpu;
        task_manager->cpu_each = g_new0(xd_history, (gsize)MAX(task_manager->ncpu, 1));
        for (int i = 0; i < task_manager->ncpu; i++)
            xd_history_init(&task_manager->cpu_each[i], HISTORY);
        rebuild_cpu_graphs(task_manager);
        show_cpu_graphs(task_manager);
    }
    task_manager->cpu_now = st->cpu;
    task_manager->kernel_now = st->cpu_kernel;
    g_snprintf(task_manager->cpu_text, sizeof(task_manager->cpu_text), "%d %%",
               (int)(st->cpu * 100 + 0.5));
    xd_history_push(&task_manager->cpu_hist, st->cpu, st->cpu_kernel);
    for (int i = 0; i < task_manager->ncpu; i++)
        xd_history_push(&task_manager->cpu_each[i], st->cpu_each[i], st->kernel_each[i]);

    guint64 used = st->mem_total_kb > st->mem_avail_kb ? st->mem_total_kb - st->mem_avail_kb : 0;
    task_manager->mem_now = st->mem_total_kb > 0 ? (double)used / (double)st->mem_total_kb : 0;
    GString *ms = g_string_new(nullptr);
    append_memory(ms, used, TRUE);
    g_strlcpy(task_manager->mem_text, ms->str, sizeof(task_manager->mem_text));
    xd_history_push(&task_manager->mem_hist, task_manager->mem_now, 0);

    task_manager->peak_commit = MAX(task_manager->peak_commit, st->commit_kb);
    set_number(task_manager->handles, st->handles);
    set_number(task_manager->threads, st->threads);
    set_number(task_manager->processes, st->processes);
    set_memory(task_manager->phys_total, st->mem_total_kb);
    set_memory(task_manager->phys_avail, st->mem_avail_kb);
    set_memory(task_manager->phys_cache, st->cache_kb);
    set_memory(task_manager->commit_total, st->commit_kb);
    set_memory(task_manager->commit_limit, st->commit_limit_kb);
    set_memory(task_manager->commit_peak, task_manager->peak_commit);
    set_memory(task_manager->kern_total, st->kernel_paged_kb + st->kernel_nonpaged_kb);
    set_memory(task_manager->kern_paged, st->kernel_paged_kb);
    set_memory(task_manager->kern_nonpaged, st->kernel_nonpaged_kb);
    char title[48];
    static const char *const names[] = {"Physical Memory", "Committed Memory", "Kernel Memory"};
    GtkWidget *frames[] = {task_manager->phys_frame, task_manager->commit_frame,
                           task_manager->kern_frame};
    for (int i = 0; i < 3; i++) {
        g_snprintf(title, sizeof(title), "%s (%s)", names[i], unit_names[mem_unit]);
        /* gtk_frame_set_label makes a new label widget and relayouts the
         * window: only when the unit changes, not every tick. */
        if (g_strcmp0(gtk_frame_get_label(GTK_FRAME(frames[i])), title) != 0)
            gtk_frame_set_label(GTK_FRAME(frames[i]), title);
    }

    gtk_widget_queue_draw(task_manager->cpu_meter);
    gtk_widget_queue_draw(task_manager->mem_meter);
    gtk_widget_queue_draw(task_manager->cpu_graph);
    gtk_widget_queue_draw(task_manager->cpu_graphs_box);
    gtk_widget_queue_draw(task_manager->mem_graph);

    char buf[96];
    g_snprintf(buf, sizeof(buf), "Processes: %u", st->processes);
    gtk_label_set_text(GTK_LABEL(task_manager->st_procs), buf);
    g_snprintf(buf, sizeof(buf), "CPU Usage: %d%%", (int)(st->cpu * 100 + 0.5));
    gtk_label_set_text(GTK_LABEL(task_manager->st_cpu), buf);
    xd_trayicon_set_cpu(task_manager->tray, st->cpu);
    g_string_assign(ms, "Mem Usage: ");
    append_memory(ms, used, TRUE);
    g_string_append(ms, " / ");
    append_memory(ms, st->mem_total_kb, TRUE);
    gtk_label_set_text(GTK_LABEL(task_manager->st_mem), ms->str);
    g_string_free(ms, TRUE);
}

/* ---- sampling and menus ----------------------------------------------------------- */

/* Hidden in the notification area, or minimized. */
static gboolean off_screen(const taskmgr *task_manager) {
    return task_manager->hidden ||
           (task_manager->own_task != nullptr && task_manager->own_task->minimized);
}

static void sample(taskmgr *task_manager) {
    if (!xd_sysinfo_sample(task_manager->si))
        return;
    /* Updating every row of the process list is the costly part; while
     * nobody can see it, it waits for the window (refresh_processes). The
     * meters keep going for the history graphs and the tray icon. */
    task_manager->procs_stale = off_screen(task_manager);
    if (!task_manager->procs_stale)
        update_processes(task_manager);
    update_performance(task_manager);
    int tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(task_manager->notebook));
    xd_net_tab_sample(task_manager->net, tab == TAB_NET && !off_screen(task_manager));
    if (tab == TAB_USERS && !off_screen(task_manager))
        xd_users_tab_refresh(task_manager->users);
}

/* Back on screen: bring the process list up to the last sample. */
static void refresh_processes(taskmgr *task_manager) {
    if (task_manager->procs_stale && !off_screen(task_manager)) {
        task_manager->procs_stale = FALSE;
        update_processes(task_manager);
    }
}

static gboolean on_timer(gpointer data) {
    sample(data);
    return G_SOURCE_CONTINUE;
}

static void set_speed(taskmgr *task_manager, int speed) {
    task_manager->speed = speed;
    if (task_manager->timer != 0) {
        g_source_remove(task_manager->timer);
        task_manager->timer = 0;
    }
    if (speed_ms[speed] > 0)
        task_manager->timer = g_timeout_add(speed_ms[speed], on_timer, task_manager);
}

static void cb_speed(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    if (task_manager->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_speed(task_manager, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "speed")));
    save_settings(task_manager);
}

static void cb_refresh([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    sample(user_data);
}

static void cb_units(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    if (task_manager->syncing || !gtk_check_menu_item_get_active(item))
        return;
    mem_unit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "unit"));
    gtk_widget_queue_draw(task_manager->procs_tree);
    sample(task_manager);
    save_settings(task_manager);
}

static void cb_apps_view(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    if (task_manager->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_apps_view(task_manager, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "view")));
    save_settings(task_manager);
}

static void cb_cpu_history(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    if (task_manager->syncing || !gtk_check_menu_item_get_active(item))
        return;
    task_manager->per_cpu = item == GTK_CHECK_MENU_ITEM(task_manager->cpu_history_items[1]);
    show_cpu_graphs(task_manager);
    save_settings(task_manager);
}

static void cb_kernel(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    task_manager->show_kernel = gtk_check_menu_item_get_active(item);
    gtk_widget_queue_draw(task_manager->notebook);
    save_settings(task_manager);
}

static void cb_on_top(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    task_manager->on_top = gtk_check_menu_item_get_active(item);
    xd_tasklist_set_on_top(task_manager->tasks, task_manager->window, task_manager->on_top);
    xd_trayicon_set_on_top(task_manager->tray, task_manager->on_top);
    save_settings(task_manager);
}

/* Once the window is on screen, Always On Top as it was left. */
static gboolean apply_on_top(GtkWidget *widget, [[maybe_unused]] GdkEvent *ev, gpointer user_data) {
    taskmgr *task_manager = user_data;
    if (task_manager->on_top)
        xd_tasklist_set_on_top(task_manager->tasks, widget, TRUE);
    return FALSE;
}

static void cb_hide_when_minimized(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    task_manager->hide_when_minimized = gtk_check_menu_item_get_active(item);
    save_settings(task_manager);
}

/* ---- the notification area icon ---- */

/* A click on the icon, or Restore: back on screen (and on the taskbar). */
static void tray_restore(gpointer user_data) {
    taskmgr *task_manager = user_data;
    if (task_manager->hidden) {
        gtk_widget_show(task_manager->window);
    } else if (task_manager->own_task != nullptr) {
        xd_task_activate(task_manager->own_task); /* also unminimizes */
        return;
    }
    gtk_window_present(GTK_WINDOW(task_manager->window));
}

/* Shown again, from the icon or by starting Task Manager again. */
static void on_show([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    taskmgr *task_manager = user_data;
    task_manager->hidden = FALSE;
    refresh_processes(task_manager);
}

static void tray_set_on_top(gboolean on_top, gpointer user_data) {
    taskmgr *task_manager = user_data;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(task_manager->on_top_item), on_top);
}

static void tray_close(gpointer user_data) {
    taskmgr *task_manager = user_data;
    gtk_widget_destroy(task_manager->window);
}

/* Without the notification area there's nowhere to hide to. */
static void tray_shown(gboolean shown, gpointer user_data) {
    taskmgr *task_manager = user_data;
    gtk_widget_set_sensitive(task_manager->hide_item, shown);
    if (!shown && task_manager->hidden)
        tray_restore(task_manager);
}

static const xd_trayicon_ops tray_ops = {
    .restore = tray_restore,
    .set_on_top = tray_set_on_top,
    .close = tray_close,
    .shown = tray_shown,
};

static void cb_minimize_on_use(GtkCheckMenuItem *item, gpointer user_data) {
    taskmgr *task_manager = user_data;
    task_manager->minimize_on_use = gtk_check_menu_item_get_active(item);
    save_settings(task_manager);
}

static void cb_exit([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    taskmgr *task_manager = user_data;
    gtk_widget_destroy(task_manager->window);
}

static void cb_about([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    taskmgr *task_manager = user_data;
    gtk_show_about_dialog(GTK_WINDOW(task_manager->window), "program-name", "Task Manager",
                          "comments", "Part of XeonDesktop", "logo-icon-name",
                          "utilities-system-monitor", "license-type", GTK_LICENSE_MIT_X11, nullptr);
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

static GtkWidget *build_menus(taskmgr *task_manager, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    task_manager->syncing = TRUE;

    GtkWidget *file = submenu(bar, "_File");
    menu_item(file, "_New Task (Run...)", G_CALLBACK(cb_new_task), task_manager, nullptr, 0,
              NO_MODS);
    gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
    menu_item(file, "E_xit Task Manager", G_CALLBACK(cb_exit), task_manager, nullptr, 0, NO_MODS);

    GtkWidget *options = submenu(bar, "_Options");
    GtkWidget *on_top = gtk_check_menu_item_new_with_mnemonic("_Always On Top");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(on_top), task_manager->on_top);
    /* Only our compositor offers it (known once the window list connects). */
    gtk_widget_set_sensitive(on_top, FALSE);
    task_manager->on_top_item = on_top;
    g_signal_connect(on_top, "toggled", G_CALLBACK(cb_on_top), task_manager);
    gtk_menu_shell_append(GTK_MENU_SHELL(options), on_top);
    GtkWidget *mou = gtk_check_menu_item_new_with_mnemonic("_Minimize On Use");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mou), task_manager->minimize_on_use);
    g_signal_connect(mou, "toggled", G_CALLBACK(cb_minimize_on_use), task_manager);
    gtk_menu_shell_append(GTK_MENU_SHELL(options), mou);
    GtkWidget *hide = gtk_check_menu_item_new_with_mnemonic("_Hide When Minimized");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(hide), task_manager->hide_when_minimized);
    gtk_widget_set_sensitive(hide, FALSE); /* until the notification area shows our icon */
    g_signal_connect(hide, "toggled", G_CALLBACK(cb_hide_when_minimized), task_manager);
    gtk_menu_shell_append(GTK_MENU_SHELL(options), hide);
    task_manager->hide_item = hide;

    GtkWidget *view = submenu(bar, "_View");
    menu_item(view, "_Refresh Now", G_CALLBACK(cb_refresh), task_manager, accel, GDK_KEY_F5,
              NO_MODS);
    GtkWidget *speed_menu = gtk_menu_new();
    GtkWidget *speed = gtk_menu_item_new_with_mnemonic("_Update Speed");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(speed), speed_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), speed);
    static const char *const speeds[SPEED_COUNT] = {"_High", "_Normal", "_Low", "_Paused"};
    GSList *grp = nullptr;
    for (int i = 0; i < SPEED_COUNT; i++)
        task_manager->speed_items[i] =
            radio_item(speed_menu, &grp, speeds[i], i == task_manager->speed, G_CALLBACK(cb_speed),
                       task_manager, "speed", i);
    GtkWidget *units_menu = gtk_menu_new();
    GtkWidget *units = gtk_menu_item_new_with_mnemonic("_Memory Units");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(units), units_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), units);
    static const char *const unit_labels[UNIT_COUNT] = {"_Kilobytes (KB)", "_Megabytes (MB)",
                                                        "_Gigabytes (GB)"};
    grp = nullptr;
    for (int i = 0; i < UNIT_COUNT; i++)
        radio_item(units_menu, &grp, unit_labels[i], i == mem_unit, G_CALLBACK(cb_units),
                   task_manager, "unit", i);
    task_manager->view_sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(view), task_manager->view_sep);
    static const char *const views[VIEW_COUNT] = {"Lar_ge Icons", "S_mall Icons", "_Details"};
    grp = nullptr;
    for (int i = 0; i < VIEW_COUNT; i++)
        task_manager->view_apps[i] = radio_item(view, &grp, views[i], i == task_manager->apps_view,
                                                G_CALLBACK(cb_apps_view), task_manager, "view", i);
    task_manager->view_columns =
        menu_item(view, "_Select Columns...", G_CALLBACK(cb_select_columns), task_manager, nullptr,
                  0, NO_MODS);
    GtkWidget *hist_menu = gtk_menu_new();
    task_manager->view_cpu_history = gtk_menu_item_new_with_mnemonic("_CPU History");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(task_manager->view_cpu_history), hist_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), task_manager->view_cpu_history);
    grp = nullptr;
    task_manager->cpu_history_items[0] =
        radio_item(hist_menu, &grp, "_One Graph, All CPUs", !task_manager->per_cpu,
                   G_CALLBACK(cb_cpu_history), task_manager, "graph", 0);
    task_manager->cpu_history_items[1] =
        radio_item(hist_menu, &grp, "One Graph _Per CPU", task_manager->per_cpu,
                   G_CALLBACK(cb_cpu_history), task_manager, "graph", 1);
    task_manager->view_kernel = gtk_check_menu_item_new_with_mnemonic("Show _Kernel Times");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(task_manager->view_kernel),
                                   task_manager->show_kernel);
    g_signal_connect(task_manager->view_kernel, "toggled", G_CALLBACK(cb_kernel), task_manager);
    gtk_menu_shell_append(GTK_MENU_SHELL(view), task_manager->view_kernel);

    GtkWidget *windows = submenu(bar, "_Windows");
    task_manager->windows_menu = gtk_menu_get_attach_widget(GTK_MENU(windows));
    task_manager->tile_h_item = menu_item(windows, "Tile _Horizontally", G_CALLBACK(cb_tile_h),
                                          task_manager, nullptr, 0, NO_MODS);
    task_manager->tile_v_item = menu_item(windows, "Tile _Vertically", G_CALLBACK(cb_tile_v),
                                          task_manager, nullptr, 0, NO_MODS);
    menu_item(windows, "_Minimize", G_CALLBACK(cb_minimize_task), task_manager, nullptr, 0,
              NO_MODS);
    menu_item(windows, "Ma_ximize", G_CALLBACK(cb_maximize_task), task_manager, nullptr, 0,
              NO_MODS);
    task_manager->cascade_item =
        menu_item(windows, "_Cascade", G_CALLBACK(cb_cascade), task_manager, nullptr, 0, NO_MODS);
    /* Until something is selected (update_app_buttons). */
    gtk_widget_set_sensitive(task_manager->tile_h_item, FALSE);
    gtk_widget_set_sensitive(task_manager->tile_v_item, FALSE);
    gtk_widget_set_sensitive(task_manager->cascade_item, FALSE);
    menu_item(windows, "_Bring To Front", G_CALLBACK(cb_bring_to_front), task_manager, nullptr, 0,
              NO_MODS);

    GtkWidget *help = submenu(bar, "_Help");
    menu_item(help, "_About Task Manager", G_CALLBACK(cb_about), task_manager, nullptr, 0, NO_MODS);
    task_manager->syncing = FALSE;
    return bar;
}

/* The View and Windows menus change with the tab, as in Windows. */
static void on_tab([[maybe_unused]] GtkNotebook *nb, [[maybe_unused]] GtkWidget *page, guint num,
                   gpointer data) {
    taskmgr *task_manager = data;
    gboolean apps = num == TAB_APPS, procs = num == TAB_PROCS, perf = num == TAB_PERF;
    gtk_widget_set_visible(task_manager->windows_menu, apps);
    for (int i = 0; i < VIEW_COUNT; i++)
        gtk_widget_set_visible(task_manager->view_apps[i], apps);
    gtk_widget_set_visible(task_manager->view_columns, procs);
    gtk_widget_set_visible(task_manager->view_cpu_history, perf);
    gtk_widget_set_visible(task_manager->view_kernel, perf);
    if (num == TAB_USERS && task_manager->users != nullptr)
        xd_users_tab_refresh(task_manager->users);
    if (task_manager->window != nullptr && gtk_widget_get_realized(task_manager->window))
        save_settings(task_manager);
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data) {
    taskmgr *task_manager = data;
    (void)widget;
    if (ev->keyval != GDK_KEY_Delete)
        return FALSE;
    int tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(task_manager->notebook));
    if (tab == TAB_PROCS && gtk_widget_has_focus(task_manager->procs_tree)) {
        cb_end_process(nullptr, task_manager);
        return TRUE;
    }
    if (tab == TAB_APPS) {
        cb_end_task(nullptr, task_manager);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *status_pane(GtkWidget **label, int width) {
    GtkWidget *widget = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(widget), GTK_SHADOW_IN);
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), "xd-sunken");
    *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(*label), 0);
    gtk_widget_set_margin_start(*label, 3);
    gtk_widget_set_margin_end(*label, 3);
    gtk_container_add(GTK_CONTAINER(widget), *label);
    gtk_widget_set_size_request(widget, width, -1);
    return widget;
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    taskmgr *task_manager = data;
    save_settings(task_manager);
    xd_trayicon_free(task_manager->tray);
    if (task_manager->timer != 0)
        g_source_remove(task_manager->timer);
    xd_tasklist_free(task_manager->tasks);
    xd_sysinfo_free(task_manager->si);
    xd_net_tab_free(task_manager->net);
    xd_users_tab_free(task_manager->users);
    g_hash_table_unref(task_manager->rows);
    xd_history_clear(&task_manager->cpu_hist);
    xd_history_clear(&task_manager->mem_hist);
    for (int i = 0; i < task_manager->ncpu; i++)
        xd_history_clear(&task_manager->cpu_each[i]);
    g_free(task_manager->cpu_each);
    g_object_unref(task_manager->apps);
    g_object_unref(task_manager->procs);
    g_free(task_manager);
}

static void open_window(GtkApplication *app) {
    taskmgr *task_manager = g_new0(taskmgr, 1);
    task_manager->app = app;
    task_manager->si = xd_sysinfo_new();
    if (task_manager->si == nullptr) {
        g_printerr("xd-taskmgr: cannot read /proc\n");
        g_free(task_manager);
        return;
    }
    int tab = TAB_APPS;
    task_manager->hide_when_minimized = TRUE; /* as in Windows */
    load_settings(task_manager, &tab);

    task_manager->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(task_manager->window), "Task Manager");
    gtk_window_set_icon_name(GTK_WINDOW(task_manager->window), "utilities-system-monitor");
    gtk_window_set_default_size(GTK_WINDOW(task_manager->window), 430, 460);
    g_signal_connect(task_manager->window, "destroy", G_CALLBACK(on_destroy), task_manager);
    g_signal_connect(task_manager->window, "key-press-event", G_CALLBACK(on_key), task_manager);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(task_manager->window), accel);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(task_manager->window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), build_menus(task_manager, accel), FALSE, FALSE, 0);
    task_manager->notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(task_manager->notebook), 5);
    gtk_notebook_append_page(GTK_NOTEBOOK(task_manager->notebook), build_apps(task_manager),
                             gtk_label_new_with_mnemonic("Applications"));
    gtk_notebook_append_page(GTK_NOTEBOOK(task_manager->notebook), build_processes(task_manager),
                             gtk_label_new_with_mnemonic("Processes"));
    gtk_notebook_append_page(GTK_NOTEBOOK(task_manager->notebook), build_performance(task_manager),
                             gtk_label_new_with_mnemonic("Performance"));
    task_manager->net = xd_net_tab_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(task_manager->notebook),
                             xd_net_tab_widget(task_manager->net),
                             gtk_label_new_with_mnemonic("Networking"));
    task_manager->users = xd_users_tab_new(GTK_WINDOW(task_manager->window));
    gtk_notebook_append_page(GTK_NOTEBOOK(task_manager->notebook),
                             xd_users_tab_widget(task_manager->users),
                             gtk_label_new_with_mnemonic("Users"));
    gtk_box_pack_start(GTK_BOX(vbox), task_manager->notebook, TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(task_manager->window, GTK_NOTEBOOK(task_manager->notebook));

    GtkWidget *sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(sbar), 2);
    gtk_box_pack_start(GTK_BOX(sbar), status_pane(&task_manager->st_procs, 95), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sbar), status_pane(&task_manager->st_cpu, 115), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sbar), status_pane(&task_manager->st_mem, 180), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(vbox), sbar, FALSE, FALSE, 0);

    apply_columns(task_manager);
    g_signal_connect(task_manager->window, "map-event", G_CALLBACK(apply_on_top), task_manager);
    g_signal_connect(task_manager->window, "show", G_CALLBACK(on_show), task_manager);
    gtk_widget_show_all(task_manager->window);
    set_apps_view(task_manager, task_manager->apps_view);
    g_signal_connect(task_manager->notebook, "switch-page", G_CALLBACK(on_tab), task_manager);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(task_manager->notebook), tab);
    on_tab(GTK_NOTEBOOK(task_manager->notebook), nullptr, (guint)tab, task_manager);
    show_cpu_graphs(task_manager);
    task_manager->tray = xd_trayicon_new(&tray_ops, task_manager);
    xd_trayicon_set_on_top(task_manager->tray, task_manager->on_top);
    sample(task_manager);
    set_speed(task_manager, task_manager->speed);
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
