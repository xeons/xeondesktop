/* SPDX-License-Identifier: MIT
 * xd-msconfig: Windows XP's System Configuration Utility (msconfig), for
 * what starts when you log on (startup.h).
 *
 *   General   Normal or Selective startup: everything on, or what the
 *             other tabs leave on (Load User Services, Load Startup Items)
 *   Services  your systemd user services that start with the session
 *   Startup   XDG autostart programs, the system's and your own
 *
 * XP's SYSTEM.INI, WIN.INI and BOOT.INI tabs, and Diagnostic Startup
 * ("basic devices and services only"), have nothing to stand for in a
 * desktop session, so they aren't here. As in Windows, changes count from
 * the next log on, and OK offers to log off.
 *
 *   xd-msconfig [--page N]   the Nth tab shown, from 0 (General)
 */
#include <gio/gdesktopappinfo.h>
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "startup.h"

#define TITLE "System Configuration Utility"

enum { PAGE_GENERAL, PAGE_SERVICES, PAGE_STARTUP };

/* Both lists: the check box, what it was when last applied, whether it
 * can change. */
enum { COL_ON, COL_ORIG, COL_CAN, COL_ICON, COL_NAME, COL_TEXT1, COL_TEXT2, COL_DATA, NCOLS };

typedef struct cfg {
    GtkWidget *window, *apply, *tabs;
    bool updating; /* we're setting the widgets ourselves */
    bool applied;  /* something changes at the next log on */
    /* General */
    GtkWidget *normal, *selective, *load_services, *load_startup;
    /* Services */
    GDBusConnection *bus;
    GtkListStore *services;
    GtkWidget *services_note;
    GPtrArray *service_items; /* xd_user_service * */
    char *remember_path;
    /* Startup */
    GtkListStore *startup;
    GtkWidget *startup_view, *remove;
    GPtrArray *startup_items; /* xd_autostart * */
    char *user_dir;
    char **system_dirs;
} cfg;

static void changed(cfg *c) {
    if (!c->updating)
        gtk_widget_set_sensitive(c->apply, TRUE);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static void error_box(cfg *c, const char *what, const GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error != nullptr ? error->message : "");
    xd_message_box(GTK_WINDOW(c->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

/* ---- the lists' check boxes ------------------------------------------------------ */

typedef enum { ALL_ON, SOME_ON, NONE_ON } list_state;

/* Over the rows that can change; an empty list counts as all on. */
static list_state state_of(GtkListStore *store) {
    GtkTreeModel *m = GTK_TREE_MODEL(store);
    GtkTreeIter it;
    bool any_on = false;
    bool any_off = false;
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        gboolean on = FALSE;
        gboolean can = FALSE;
        gtk_tree_model_get(m, &it, COL_ON, &on, COL_CAN, &can, -1);
        if (!can)
            continue;
        any_on = any_on || on;
        any_off = any_off || !on;
    }
    return !any_off ? ALL_ON : any_on ? SOME_ON : NONE_ON;
}

/* XP's General tab follows the others: Normal while everything is on;
 * else Selective, its boxes ticked, cleared, or greyed ticks for "some". */
static void show_check(GtkWidget *check, list_state s) {
    gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(check), s == SOME_ON);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), s != NONE_ON);
}

static void sync_general(cfg *c) {
    bool was = c->updating;
    c->updating = true;
    list_state services = state_of(c->services);
    list_state startup = state_of(c->startup);
    bool normal = services == ALL_ON && startup == ALL_ON;
    /* Selective stays chosen until something is turned off, as in XP. */
    if (!normal)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->selective), TRUE);
    else if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->selective)))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->normal), TRUE);
    show_check(c->load_services, services);
    show_check(c->load_startup, startup);
    bool selective = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->selective));
    gtk_widget_set_sensitive(c->load_services, selective);
    gtk_widget_set_sensitive(c->load_startup, selective);
    c->updating = was;
}

static void tick_all(cfg *c, GtkListStore *store, bool on) {
    GtkTreeModel *m = GTK_TREE_MODEL(store);
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        gboolean can = FALSE;
        gboolean was = FALSE;
        gtk_tree_model_get(m, &it, COL_CAN, &can, COL_ON, &was, -1);
        if (can && was != on) {
            gtk_list_store_set(store, &it, COL_ON, on, -1);
            changed(c);
        }
    }
}

static void set_all(cfg *c, GtkListStore *store, bool on) {
    tick_all(c, store, on);
    sync_general(c);
}

static void on_row_toggled(GtkListStore *store, const char *path, cfg *c) {
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &it, path))
        return;
    gboolean on = FALSE;
    gboolean can = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &it, COL_ON, &on, COL_CAN, &can, -1);
    if (!can)
        return;
    gtk_list_store_set(store, &it, COL_ON, !on, -1);
    changed(c);
    sync_general(c);
}

static void on_service_toggled([[maybe_unused]] GtkCellRendererToggle *r, char *path,
                               gpointer data) {
    cfg *c = data;
    on_row_toggled(c->services, path, c);
}

static void on_startup_toggled([[maybe_unused]] GtkCellRendererToggle *r, char *path,
                               gpointer data) {
    cfg *c = data;
    on_row_toggled(c->startup, path, c);
}

/* A list with a check box column, then `titles` (the first with the
 * row's icon when `icons`). */
static GtkWidget *check_list(cfg *c, GtkListStore *store, const char *const *titles, size_t n,
                             const int *widths, bool icons, GCallback toggled,
                             GtkWidget **view_out) {
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    GtkCellRenderer *check = gtk_cell_renderer_toggle_new();
    g_signal_connect(check, "toggled", toggled, c);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "", check, "active", COL_ON, "activatable", COL_CAN, "sensitive", COL_CAN, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    for (size_t i = 0; i < n; i++) {
        col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, titles[i]);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_fixed_width(col, widths[i]);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        if (i == 0 && icons) {
            GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
            gtk_tree_view_column_pack_start(col, pix, FALSE);
            gtk_tree_view_column_add_attribute(col, pix, "icon-name", COL_ICON);
        }
        GtkCellRenderer *text = gtk_cell_renderer_text_new();
        g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        gtk_tree_view_column_pack_start(col, text, TRUE);
        gtk_tree_view_column_add_attribute(col, text, "text", COL_NAME + (int)i);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    }
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sc, 480, 250);
    gtk_container_add(GTK_CONTAINER(sc), view);
    if (view_out != nullptr)
        *view_out = view;
    return sc;
}

/* ---- Services ------------------------------------------------------------------------ */

static void fill_services(cfg *c) {
    gtk_list_store_clear(c->services);
    if (c->service_items != nullptr)
        g_ptr_array_unref(c->service_items);
    c->service_items = nullptr;
    GError *error = nullptr;
    char **remembered = xd_startup_disabled_load(c->remember_path);
    if (c->bus != nullptr)
        c->service_items =
            xd_user_services_list(c->bus, (const char *const *)remembered, &error);
    g_strfreev(remembered);
    if (c->service_items == nullptr) {
        char *text = g_strdup_printf("Your user services can't be listed: %s",
                                     error != nullptr ? error->message
                                                      : "there is no session bus.");
        gtk_label_set_text(GTK_LABEL(c->services_note), text);
        gtk_widget_show(c->services_note);
        g_free(text);
        g_clear_error(&error);
        return;
    }
    bool any_global = false;
    for (guint i = 0; i < c->service_items->len; i++) {
        xd_user_service *s = c->service_items->pdata[i];
        any_global = any_global || s->global;
        gtk_list_store_insert_with_values(c->services, nullptr, -1, COL_ON, s->enabled, COL_ORIG,
                                          s->enabled, COL_CAN, !s->global, COL_NAME,
                                          s->description, COL_TEXT1, s->unit, COL_TEXT2,
                                          s->running ? "Running" : "Stopped", COL_DATA, s, -1);
    }
    gtk_label_set_text(GTK_LABEL(c->services_note),
                       "Greyed services start for every user; only an administrator can turn "
                       "them off.");
    gtk_widget_set_visible(c->services_note, any_global);
}

static void on_services_enable_all([[maybe_unused]] GtkButton *b, gpointer data) {
    cfg *c = data;
    set_all(c, c->services, true);
}

static void on_services_disable_all([[maybe_unused]] GtkButton *b, gpointer data) {
    cfg *c = data;
    set_all(c, c->services, false);
}

static GtkWidget *button_row(GtkWidget *left, GtkWidget *right) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (left != nullptr)
        gtk_box_pack_start(GTK_BOX(row), left, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), right, FALSE, FALSE, 0);
    return row;
}

static GtkWidget *two_buttons(const char *a, GCallback fa, const char *b, GCallback fb, cfg *c) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ba = gtk_button_new_with_mnemonic(a);
    GtkWidget *bb = gtk_button_new_with_mnemonic(b);
    g_signal_connect(ba, "clicked", fa, c);
    g_signal_connect(bb, "clicked", fb, c);
    gtk_box_pack_start(GTK_BOX(box), ba, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bb, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *services_page(cfg *c) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    c->services = gtk_list_store_new(NCOLS, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_POINTER);
    static const char *const titles[] = {"Service", "Unit", "Status"};
    static const int widths[] = {210, 160, 70};
    gtk_box_pack_start(GTK_BOX(page),
                       check_list(c, c->services, titles, G_N_ELEMENTS(titles), widths, false,
                                  G_CALLBACK(on_service_toggled), nullptr),
                       TRUE, TRUE, 0);
    c->services_note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(c->services_note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(c->services_note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(c->services_note), 70);
    gtk_widget_set_no_show_all(c->services_note, TRUE);
    gtk_box_pack_start(GTK_BOX(page), c->services_note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       button_row(nullptr, two_buttons("_Enable All",
                                                       G_CALLBACK(on_services_enable_all),
                                                       "D_isable All",
                                                       G_CALLBACK(on_services_disable_all), c)),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- Startup ------------------------------------------------------------------------- */

/* Pending ticks by file name, kept across a refill (after Add or Remove). */
static GHashTable *pending_ticks(cfg *c) {
    GHashTable *pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GtkTreeModel *m = GTK_TREE_MODEL(c->startup);
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        gboolean on = FALSE;
        gboolean orig = FALSE;
        xd_autostart *item = nullptr;
        gtk_tree_model_get(m, &it, COL_ON, &on, COL_ORIG, &orig, COL_DATA, &item, -1);
        if (on != orig)
            g_hash_table_insert(pending, g_strdup(item->file), GINT_TO_POINTER(on ? 1 : 2));
    }
    return pending;
}

/* The folder as you'd type it: ~ for your home. */
static char *location_text(const char *dir) {
    const char *home = g_get_home_dir();
    size_t n = strlen(home);
    if (n > 1 && strncmp(dir, home, n) == 0 && (dir[n] == '/' || dir[n] == '\0'))
        return g_strconcat("~", dir + n, nullptr);
    return g_strdup(dir);
}

static void fill_startup(cfg *c) {
    GHashTable *pending = c->startup_items != nullptr ? pending_ticks(c) : nullptr;
    gtk_list_store_clear(c->startup);
    if (c->startup_items != nullptr)
        g_ptr_array_unref(c->startup_items);
    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    /* Outside any session, the entries for ours. */
    c->startup_items = xd_autostart_list(c->user_dir, (const char *const *)c->system_dirs,
                                         desktop != nullptr && desktop[0] != '\0'
                                             ? nullptr
                                             : "XeonDesktop");
    for (guint i = 0; i < c->startup_items->len; i++) {
        xd_autostart *item = c->startup_items->pdata[i];
        int want = pending != nullptr
                       ? GPOINTER_TO_INT(g_hash_table_lookup(pending, item->file))
                       : 0;
        bool on = want == 0 ? item->enabled : want == 1;
        char *where = location_text(item->location);
        gtk_list_store_insert_with_values(
            c->startup, nullptr, -1, COL_ON, on, COL_ORIG, item->enabled, COL_CAN, TRUE, COL_ICON,
            item->icon != nullptr && item->icon[0] != '/' ? item->icon : nullptr,
            COL_NAME, item->name, COL_TEXT1, item->command, COL_TEXT2, where, COL_DATA, item, -1);
        g_free(where);
    }
    if (pending != nullptr)
        g_hash_table_destroy(pending);
    gtk_widget_set_sensitive(c->remove, FALSE);
    sync_general(c);
}

static xd_autostart *selected_item(cfg *c) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->startup_view));
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    xd_autostart *item = nullptr;
    if (gtk_tree_selection_get_selected(sel, &m, &it))
        gtk_tree_model_get(m, &it, COL_DATA, &item, -1);
    return item;
}

static void on_startup_selection([[maybe_unused]] GtkTreeSelection *sel, gpointer data) {
    cfg *c = data;
    xd_autostart *item = selected_item(c);
    /* Only your own entries can go; the system's can only be turned off. */
    gtk_widget_set_sensitive(c->remove, item != nullptr && item->user_only);
}

static void on_startup_enable_all([[maybe_unused]] GtkButton *b, gpointer data) {
    cfg *c = data;
    set_all(c, c->startup, true);
}

static void on_startup_disable_all([[maybe_unused]] GtkButton *b, gpointer data) {
    cfg *c = data;
    set_all(c, c->startup, false);
}

static void on_remove([[maybe_unused]] GtkButton *b, gpointer data) {
    cfg *c = data;
    xd_autostart *item = selected_item(c);
    if (item == nullptr || !item->user_only)
        return;
    char *q = g_strdup_printf("Are you sure you want to remove '%s' from the programs that "
                              "start when you log on?",
                              item->name);
    int r = xd_message_box(GTK_WINDOW(c->window), "Confirm Item Delete", XD_MB_QUESTION,
                           XD_MB_YES_NO, TRUE, q);
    g_free(q);
    if (r != GTK_RESPONSE_YES)
        return;
    GError *error = nullptr;
    if (!xd_autostart_remove(item, &error)) {
        error_box(c, "The item could not be removed.", error);
        g_error_free(error);
        return;
    }
    c->applied = true;
    fill_startup(c);
}

/* ---- Add... ---------------------------------------------------------------------------- */

enum { ADD_ICON, ADD_NAME, ADD_INFO, ADD_NCOLS };

typedef struct add_dialog {
    GtkWidget *dialog, *program, *command, *list, *entry;
} add_dialog;

static int by_app_name(gconstpointer a, gconstpointer b) {
    return g_utf8_collate(g_app_info_get_name(G_APP_INFO((gpointer)(uintptr_t)a)),
                          g_app_info_get_name(G_APP_INFO((gpointer)(uintptr_t)b)));
}

static GtkListStore *installed_programs(void) {
    GtkListStore *store = gtk_list_store_new(ADD_NCOLS, G_TYPE_ICON, G_TYPE_STRING,
                                             G_TYPE_DESKTOP_APP_INFO);
    GList *all = g_app_info_get_all();
    GList *shown = nullptr;
    for (GList *l = all; l != nullptr; l = l->next) {
        if (G_IS_DESKTOP_APP_INFO(l->data) && g_app_info_should_show(l->data))
            shown = g_list_prepend(shown, l->data);
    }
    shown = g_list_sort(shown, by_app_name);
    for (GList *l = shown; l != nullptr; l = l->next) {
        GAppInfo *ai = l->data;
        GIcon *icon = g_app_info_get_icon(ai);
        GIcon *fallback = icon == nullptr ? g_themed_icon_new("application-x-executable") : nullptr;
        gtk_list_store_insert_with_values(store, nullptr, -1, ADD_ICON,
                                          icon != nullptr ? icon : fallback, ADD_NAME,
                                          g_app_info_get_name(ai), ADD_INFO, ai, -1);
        if (fallback != nullptr)
            g_object_unref(fallback);
    }
    g_list_free(shown);
    g_list_free_full(all, g_object_unref);
    return store;
}

static void on_add_list_changed([[maybe_unused]] GtkTreeSelection *sel, gpointer data) {
    add_dialog *a = data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->program), TRUE);
}

static void on_add_entry_changed([[maybe_unused]] GtkEditable *e, gpointer data) {
    add_dialog *a = data;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->command), TRUE);
}

/* Choosing Command (Alt+C too) goes on to typing it. */
static void on_add_command(GtkToggleButton *b, gpointer data) {
    add_dialog *a = data;
    if (gtk_toggle_button_get_active(b) && !gtk_widget_has_focus(a->entry))
        gtk_widget_grab_focus(a->entry);
}

static void on_add_browse([[maybe_unused]] GtkButton *b, gpointer data) {
    add_dialog *a = data;
    GtkWidget *fc = gtk_file_chooser_dialog_new("Browse", GTK_WINDOW(a->dialog),
                                                GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
                                                GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT,
                                                nullptr);
    if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (path != nullptr) {
            char *quoted = g_shell_quote(path);
            /* Quoted only when it has to be. */
            gtk_entry_set_text(GTK_ENTRY(a->entry), strpbrk(path, " \t'\"\\$`") != nullptr
                                                        ? quoted
                                                        : path);
            g_free(quoted);
            g_free(path);
        }
    }
    gtk_widget_destroy(fc);
}

/* Adds what was chosen; false keeps the dialog open. */
static bool add_chosen(cfg *c, add_dialog *a) {
    GError *error = nullptr;
    char *file = nullptr;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->program))) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->list));
        GtkTreeModel *m = nullptr;
        GtkTreeIter it;
        GDesktopAppInfo *info = nullptr;
        if (gtk_tree_selection_get_selected(sel, &m, &it))
            gtk_tree_model_get(m, &it, ADD_INFO, &info, -1);
        if (info == nullptr) {
            xd_message_box(GTK_WINDOW(a->dialog), TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                           "Choose a program from the list.");
            return false;
        }
        file = xd_autostart_add_app(c->user_dir, info, &error);
        g_object_unref(info);
    } else {
        const char *text = gtk_entry_get_text(GTK_ENTRY(a->entry));
        char *trimmed = g_strstrip(g_strdup(text));
        bool empty = trimmed[0] == '\0';
        g_free(trimmed);
        if (empty) {
            xd_message_box(GTK_WINDOW(a->dialog), TITLE, XD_MB_INFO, XD_MB_OK, FALSE,
                           "Type the command to start.");
            return false;
        }
        file = xd_autostart_add_command(c->user_dir, text, &error);
    }
    if (file == nullptr) {
        error_box(c, "The item could not be added.", error);
        g_clear_error(&error);
        return false;
    }
    g_free(file);
    c->applied = true;
    fill_startup(c);
    return true;
}

static void on_add([[maybe_unused]] GtkButton *b, gpointer data) {
    cfg *c = data;
    add_dialog a = {0};
    a.dialog = gtk_dialog_new_with_buttons("Add Startup Item", GTK_WINDOW(c->window),
                                           (GtkDialogFlags)(GTK_DIALOG_MODAL |
                                                            GTK_DIALOG_DESTROY_WITH_PARENT),
                                           "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
                                           nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(a.dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(a.dialog), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *intro = gtk_label_new("Choose a program to start each time you log on, or type "
                                     "the command that starts it.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 50);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 4);

    a.program = gtk_radio_button_new_with_mnemonic(nullptr, "_Installed program:");
    gtk_box_pack_start(GTK_BOX(box), a.program, FALSE, FALSE, 0);
    GtkListStore *store = installed_programs();
    a.list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a.list), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(a.list), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(a.list), ADD_NAME);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, pix, FALSE);
    gtk_tree_view_column_add_attribute(col, pix, "gicon", ADD_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", ADD_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a.list), col);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sc, 340, 200);
    gtk_widget_set_margin_start(sc, 20);
    gtk_container_add(GTK_CONTAINER(sc), a.list);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);

    a.command = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(a.program),
                                                               "_Command:");
    gtk_box_pack_start(GTK_BOX(box), a.command, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 20);
    a.entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(a.entry), TRUE);
    GtkWidget *browse = gtk_button_new_with_mnemonic("_Browse...");
    gtk_box_pack_start(GTK_BOX(row), a.entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(a.dialog))), box, TRUE,
                       TRUE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a.list)), "changed",
                     G_CALLBACK(on_add_list_changed), &a);
    g_signal_connect(a.entry, "changed", G_CALLBACK(on_add_entry_changed), &a);
    g_signal_connect(a.command, "toggled", G_CALLBACK(on_add_command), &a);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_add_browse), &a);
    gtk_widget_show_all(a.dialog);
    gtk_widget_grab_focus(a.list);
    while (gtk_dialog_run(GTK_DIALOG(a.dialog)) == GTK_RESPONSE_OK && !add_chosen(c, &a)) {
    }
    gtk_widget_destroy(a.dialog);
}

static GtkWidget *startup_page(cfg *c) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    c->startup = gtk_list_store_new(NCOLS, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                                    G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                    G_TYPE_POINTER);
    static const char *const titles[] = {"Startup Item", "Command", "Location"};
    static const int widths[] = {160, 150, 140};
    gtk_box_pack_start(GTK_BOX(page),
                       check_list(c, c->startup, titles, G_N_ELEMENTS(titles), widths, true,
                                  G_CALLBACK(on_startup_toggled), &c->startup_view),
                       TRUE, TRUE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->startup_view)), "changed",
                     G_CALLBACK(on_startup_selection), c);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *add = gtk_button_new_with_mnemonic("A_dd...");
    c->remove = gtk_button_new_with_mnemonic("_Remove");
    gtk_widget_set_sensitive(c->remove, FALSE);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add), c);
    g_signal_connect(c->remove, "clicked", G_CALLBACK(on_remove), c);
    gtk_box_pack_start(GTK_BOX(left), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), c->remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       button_row(left, two_buttons("_Enable All",
                                                    G_CALLBACK(on_startup_enable_all),
                                                    "D_isable All",
                                                    G_CALLBACK(on_startup_disable_all), c)),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- General -------------------------------------------------------------------------- */

static void on_normal(GtkToggleButton *b, gpointer data) {
    cfg *c = data;
    if (c->updating || !gtk_toggle_button_get_active(b))
        return;
    /* Both lists, then the tab: between the two it would look Selective. */
    tick_all(c, c->services, true);
    tick_all(c, c->startup, true);
    sync_general(c);
}

/* Only when chosen: leaving it for Normal is on_normal's (and Normal isn't
 * on until its lists are). */
static void on_selective(GtkToggleButton *b, gpointer data) {
    cfg *c = data;
    if (!c->updating && gtk_toggle_button_get_active(b))
        sync_general(c);
}

/* A Selective box clicked: all of that list on, or (from a tick, greyed
 * or not) all off. */
static void on_load(GtkToggleButton *b, gpointer data) {
    cfg *c = data;
    if (c->updating)
        return;
    bool on = gtk_toggle_button_get_active(b) && !gtk_toggle_button_get_inconsistent(b);
    set_all(c, GTK_WIDGET(b) == c->load_services ? c->services : c->startup, on);
}


static GtkWidget *general_page(cfg *c) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    c->normal = gtk_radio_button_new_with_mnemonic(
        nullptr, "_Normal Startup - load all startup programs and services");
    c->selective = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(c->normal),
                                                                  "S_elective Startup");
    c->load_services = gtk_check_button_new_with_mnemonic("Load User Ser_vices");
    c->load_startup = gtk_check_button_new_with_mnemonic("Load Startu_p Items");
    gtk_box_pack_start(GTK_BOX(box), c->normal, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c->selective, FALSE, FALSE, 0);
    GtkWidget *sub = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(sub, 24);
    gtk_box_pack_start(GTK_BOX(sub), c->load_services, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sub), c->load_startup, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Startup Selection", box), FALSE, FALSE, 0);
    g_signal_connect(c->normal, "toggled", G_CALLBACK(on_normal), c);
    g_signal_connect(c->selective, "toggled", G_CALLBACK(on_selective), c);
    g_signal_connect(c->load_services, "toggled", G_CALLBACK(on_load), c);
    g_signal_connect(c->load_startup, "toggled", G_CALLBACK(on_load), c);
    return page;
}

/* ---- applying ------------------------------------------------------------------------- */

static void remember_service(cfg *c, const char *unit, bool disabled) {
    char **units = xd_startup_disabled_load(c->remember_path);
    GPtrArray *keep = g_ptr_array_new();
    for (size_t i = 0; units[i] != nullptr; i++) {
        if (strcmp(units[i], unit) != 0)
            g_ptr_array_add(keep, units[i]);
    }
    if (disabled)
        g_ptr_array_add(keep, (gpointer)(uintptr_t)unit);
    g_ptr_array_add(keep, nullptr);
    GError *error = nullptr;
    if (!xd_startup_disabled_save(c->remember_path, (const char *const *)keep->pdata, &error)) {
        g_printerr("xd-msconfig: %s\n", error->message);
        g_error_free(error);
    }
    g_ptr_array_unref(keep);
    g_strfreev(units);
}

static bool apply_services(cfg *c) {
    bool ok = true;
    GtkTreeModel *m = GTK_TREE_MODEL(c->services);
    GtkTreeIter it;
    for (bool more = gtk_tree_model_get_iter_first(m, &it); more;
         more = gtk_tree_model_iter_next(m, &it)) {
        gboolean on = FALSE;
        gboolean orig = FALSE;
        xd_user_service *s = nullptr;
        gtk_tree_model_get(m, &it, COL_ON, &on, COL_ORIG, &orig, COL_DATA, &s, -1);
        if (on == orig)
            continue;
        GError *error = nullptr;
        if (xd_user_service_set_enabled(c->bus, s->unit, on, &error)) {
            gtk_list_store_set(c->services, &it, COL_ORIG, on, -1);
            remember_service(c, s->unit, !on);
            c->applied = true;
        } else {
            char *what = g_strdup_printf("%s could not be %s.", s->description,
                                         on ? "turned on" : "turned off");
            error_box(c, what, error);
            g_free(what);
            g_clear_error(&error);
            gtk_list_store_set(c->services, &it, COL_ON, orig, -1);
            ok = false;
        }
    }
    return ok;
}

static bool apply_startup(cfg *c) {
    bool ok = true;
    GtkTreeModel *m = GTK_TREE_MODEL(c->startup);
    GtkTreeIter it;
    for (bool more = gtk_tree_model_get_iter_first(m, &it); more;
         more = gtk_tree_model_iter_next(m, &it)) {
        gboolean on = FALSE;
        gboolean orig = FALSE;
        xd_autostart *item = nullptr;
        gtk_tree_model_get(m, &it, COL_ON, &on, COL_ORIG, &orig, COL_DATA, &item, -1);
        if (on == orig)
            continue;
        GError *error = nullptr;
        if (xd_autostart_set_enabled(item, c->user_dir, on, &error)) {
            gtk_list_store_set(c->startup, &it, COL_ORIG, on, -1);
            item->enabled = on;
            c->applied = true;
        } else {
            char *what = g_strdup_printf("%s could not be %s.", item->name,
                                         on ? "turned on" : "turned off");
            error_box(c, what, error);
            g_free(what);
            g_clear_error(&error);
            gtk_list_store_set(c->startup, &it, COL_ON, orig, -1);
            ok = false;
        }
    }
    return ok;
}

static bool apply(cfg *c) {
    bool ok = apply_services(c);
    ok = apply_startup(c) && ok;
    gtk_widget_set_sensitive(c->apply, FALSE);
    sync_general(c);
    return ok;
}

/* xd-shutdown beside us (a build tree), else from PATH. */
static char *own_program(const char *name) {
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = self != nullptr ? g_path_get_dirname(self) : nullptr;
    char *local = dir != nullptr ? g_build_filename(dir, name, nullptr) : nullptr;
    g_free(self);
    g_free(dir);
    if (local != nullptr && g_file_test(local, G_FILE_TEST_IS_EXECUTABLE))
        return local;
    g_free(local);
    return g_strdup(name);
}

/* XP's "You must restart your computer...": here, log off. */
static void offer_log_off(cfg *c) {
    static const char *const labels[] = {"_Log Off", "E_xit Without Log Off"};
    int r = xd_message_box_choose(GTK_WINDOW(c->window), "System Configuration", XD_MB_QUESTION,
                                  "You must log off for some of the changes made by System "
                                  "Configuration to take effect.",
                                  labels, 2, 0, 1);
    if (r != 0)
        return;
    char *exe = own_program("xd-shutdown");
    char *argv[] = {exe, (char *)(uintptr_t) "--logoff", nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &error)) {
        g_printerr("xd-msconfig: cannot log off: %s\n", error->message);
        g_error_free(error);
    }
    g_free(exe);
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    cfg *c = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(c);
        return;
    }
    if (response == GTK_RESPONSE_OK) {
        if (gtk_widget_get_sensitive(c->apply) && !apply(c))
            return;
        if (c->applied)
            offer_log_off(c);
    }
    gtk_widget_destroy(GTK_WIDGET(d));
}

int main(int argc, char **argv) {
    int page = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
    }
    xd_gtk_app_preinit("org.xeondesktop.SystemConfiguration");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static cfg c;
    c.user_dir = g_build_filename(g_get_user_config_dir(), "autostart", nullptr);
    const char *const *sys = g_get_system_config_dirs();
    size_t n = sys != nullptr ? g_strv_length((char **)(uintptr_t)sys) : 0;
    c.system_dirs = g_new0(char *, n + 1);
    for (size_t i = 0; i < n; i++)
        c.system_dirs[i] = g_build_filename(sys[i], "autostart", nullptr);
    c.remember_path = g_build_filename(g_get_user_config_dir(), "xeondesktop", "startup.ini",
                                       nullptr);
    GError *error = nullptr;
    c.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (c.bus == nullptr) {
        g_printerr("xd-msconfig: %s\n", error->message);
        g_clear_error(&error);
    }

    c.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(c.window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(c.window), "preferences-system");
    gtk_window_set_resizable(GTK_WINDOW(c.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(c.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(c.window), "Cancel", GTK_RESPONSE_CANCEL);
    c.apply = gtk_dialog_add_button(GTK_DIALOG(c.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(c.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(c.window), GTK_RESPONSE_OK);

    c.tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(c.tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(c.tabs), general_page(&c),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(c.tabs), services_page(&c),
                             gtk_label_new_with_mnemonic("_Services"));
    gtk_notebook_append_page(GTK_NOTEBOOK(c.tabs), startup_page(&c),
                             gtk_label_new_with_mnemonic("Star_tup"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(c.window))), c.tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(c.window, GTK_NOTEBOOK(c.tabs));
    fill_services(&c);
    fill_startup(&c); /* and the General tab from both */
    g_signal_connect(c.window, "response", G_CALLBACK(on_response), &c);
    g_signal_connect(c.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(c.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(c.tabs), XD_CLAMP(page, PAGE_GENERAL, PAGE_STARTUP));
    gtk_main();

    if (c.service_items != nullptr)
        g_ptr_array_unref(c.service_items);
    if (c.startup_items != nullptr)
        g_ptr_array_unref(c.startup_items);
    g_object_unref(c.services);
    g_object_unref(c.startup);
    g_clear_object(&c.bus);
    g_strfreev(c.system_dirs);
    g_free(c.user_dir);
    g_free(c.remember_path);
    return EXIT_SUCCESS;
}
