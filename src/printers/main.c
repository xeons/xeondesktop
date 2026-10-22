/* SPDX-License-Identifier: MIT
 * xd-printers: Windows XP's Printers and Faxes folder, on CUPS. Lists the
 * printers the scheduler has (Tiles, Icons, Details), the default one
 * with XP's check mark; opens a printer's queue, its Properties and
 * Printing Preferences, pauses and deletes printers, and adds network
 * printers with the Add Printer Wizard. Changes go through cupsd's own
 * authorization, which may ask for a password.
 *
 *   xd-printers                    the folder
 *   xd-printers --queue NAME       a printer's queue
 *   xd-printers --properties NAME  its Properties
 *   xd-printers --preferences NAME its Printing Preferences
 *   xd-printers --add              the Add Printer Wizard
 *
 * CUPS_SERVER chooses the scheduler, as for every CUPS program.
 */
#include <cairo-gobject.h>
#include <stdlib.h>

#include "../gtkapp/gtkapp.h"
#include "auth.h"
#include "printers.h"
#include "printlib.h"

static constexpr GdkModifierType NO_MODS = (GdkModifierType)0;

enum {
    C_ICON,       /* cairo surface, 48 px */
    C_SMALL_ICON, /* 16 px */
    C_NAME,
    C_TILE, /* markup: name, documents, status */
    C_DOCS,
    C_STATUS,
    C_COMMENT,
    C_LOCATION,
    C_MODEL,
    C_PAUSED,
    C_KEY, /* what the row shows, to leave unchanged rows alone */
    C_COUNT
};
enum { VIEW_TILES, VIEW_ICONS, VIEW_DETAILS, VIEW_COUNT };
static const char *const view_labels[VIEW_COUNT] = {"T_iles", "Ico_ns", "_Details"};

typedef struct folder {
    GtkWidget *window, *stack, *status;
    GtkWidget *views[VIEW_COUNT];
    GtkWidget *pages[VIEW_COUNT];
    GtkWidget *view_items[VIEW_COUNT];
    /* File menu items that need a printer selected. */
    GtkWidget *open_item, *default_item, *prefs_item, *pause_item, *cancel_item, *delete_item,
        *props_item;
    GtkListStore *store;
    GCancellable *cancel;
    guint poll;
    int view;
    bool syncing, loading, again, error_shown;
} folder;

/* ---- the list ----------------------------------------------------------------------- */

static void set_row(folder *printers_folder, GtkTreeIter *iter, const xd_printer *printer) {
    int scale = gtk_widget_get_scale_factor(printers_folder->window);
    /* Everything the row and its icons show (the icon theme too): the poll
     * finds most rows as they were, and setting them again would redraw
     * them, with new icons, every few seconds. */
    char *theme = nullptr;
    g_object_get(gtk_widget_get_settings(printers_folder->window), "gtk-icon-theme-name", &theme,
                 nullptr);
    char *key =
        g_strdup_printf("%s\n%d\n%d\n%s\n%d\n%s\n%s\n%s\n%s\n%d%d%d\n%s", printer->name,
                        printer->jobs, printer->state, printer->reasons, printer->accepting,
                        printer->info, printer->location, printer->model, printer->device_uri,
                        printer->is_default, printer->fax, scale, theme != nullptr ? theme : "");
    g_free(theme);
    char *old = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(printers_folder->store), iter, C_KEY, &old, -1);
    bool same = g_strcmp0(old, key) == 0;
    g_free(old);
    if (same) {
        g_free(key);
        return;
    }
    cairo_surface_t *big = xd_printer_icon(printer, 48, scale);
    cairo_surface_t *small = xd_printer_icon(printer, 16, scale);
    const char *status =
        xd_printer_status_text(printer->state, printer->reasons, printer->accepting);
    char *docs = g_strdup_printf("%d", printer->jobs);
    char *tile = g_markup_printf_escaped("%s\n<span alpha='70%%'>%d\n%s</span>", printer->name,
                                         printer->jobs, status);
    gtk_list_store_set(printers_folder->store, iter, C_ICON, big, C_SMALL_ICON, small, C_NAME,
                       printer->name, C_TILE, tile, C_DOCS, docs, C_STATUS, status, C_COMMENT,
                       printer->info, C_LOCATION, printer->location, C_MODEL, printer->model,
                       C_PAUSED, xd_printer_is_paused(printer->state, printer->reasons), C_KEY, key,
                       -1);
    g_free(key);
    g_free(tile);
    g_free(docs);
    cairo_surface_destroy(small);
    cairo_surface_destroy(big);
}

/* Updates the rows in place, so the selection and scroll position stay. */
static void fill(folder *printers_folder, GPtrArray *printers) {
    GHashTable *by_name = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < printers->len; i++) {
        xd_printer *printer = g_ptr_array_index(printers, i);
        g_hash_table_insert(by_name, printer->name, printer);
    }
    GtkTreeModel *model = GTK_TREE_MODEL(printers_folder->store);
    GtkTreeIter iter;
    gboolean more = gtk_tree_model_get_iter_first(model, &iter);
    while (more) {
        char *name = nullptr;
        gtk_tree_model_get(model, &iter, C_NAME, &name, -1);
        xd_printer *printer = g_hash_table_lookup(by_name, name);
        g_free(name);
        if (printer == nullptr) {
            more = gtk_list_store_remove(printers_folder->store, &iter);
            continue;
        }
        set_row(printers_folder, &iter, printer);
        g_hash_table_remove(by_name, printer->name);
        more = gtk_tree_model_iter_next(model, &iter);
    }
    for (guint i = 0; i < printers->len; i++) {
        xd_printer *printer = g_ptr_array_index(printers, i);
        if (!g_hash_table_contains(by_name, printer->name))
            continue;
        gtk_list_store_append(printers_folder->store, &iter);
        set_row(printers_folder, &iter, printer);
    }
    g_hash_table_unref(by_name);
}

static GList *selected_paths(folder *printers_folder) {
    GtkWidget *widget = printers_folder->views[printers_folder->view];
    if (GTK_IS_ICON_VIEW(widget))
        return gtk_icon_view_get_selected_items(GTK_ICON_VIEW(widget));
    return gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)),
                                                nullptr);
}

/* The selected printer's name (the first, if several), or nullptr. */
static char *selected_name(folder *printers_folder, bool *paused) {
    GList *sel = selected_paths(printers_folder);
    char *name = nullptr;
    GtkTreeIter iter;
    if (sel != nullptr &&
        gtk_tree_model_get_iter(GTK_TREE_MODEL(printers_folder->store), &iter, sel->data)) {
        gboolean is_paused = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(printers_folder->store), &iter, C_NAME, &name, C_PAUSED,
                           &is_paused, -1);
        if (paused != nullptr)
            *paused = is_paused;
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    return name;
}

static void update_status(folder *printers_folder) {
    bool paused = false;
    char *name = selected_name(printers_folder, &paused);
    bool one = name != nullptr;
    GtkWidget *items[] = {printers_folder->open_item,   printers_folder->default_item,
                          printers_folder->prefs_item,  printers_folder->pause_item,
                          printers_folder->cancel_item, printers_folder->delete_item,
                          printers_folder->props_item};
    for (size_t i = 0; i < G_N_ELEMENTS(items); i++)
        gtk_widget_set_sensitive(items[i], one);
    gtk_menu_item_set_label(GTK_MENU_ITEM(printers_folder->pause_item),
                            paused ? "Resume _Printing" : "_Pause Printing");
    int count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(printers_folder->store), nullptr);
    char *text = one ? g_strdup_printf("%s", name) : g_strdup_printf("%d object(s)", count);
    gtk_label_set_text(GTK_LABEL(printers_folder->status), text);
    g_free(text);
    g_free(name);
}

static void reload(folder *printers_folder);

static void on_listed([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GPtrArray *printers = g_task_propagate_pointer(G_TASK(res), &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return; /* the window is gone */
    }
    folder *printers_folder = data;
    printers_folder->loading = false;
    if (printers_folder->again)
        reload(printers_folder);
    if (printers == nullptr) {
        gtk_list_store_clear(printers_folder->store);
        /* Once, not at every poll. */
        if (!printers_folder->error_shown) {
            printers_folder->error_shown = true;
            xd_printers_error(GTK_WINDOW(printers_folder->window), error);
        }
        g_clear_error(&error);
        update_status(printers_folder);
        return;
    }
    printers_folder->error_shown = false;
    fill(printers_folder, printers);
    g_ptr_array_unref(printers);
    update_status(printers_folder);
}

static void list_thread(GTask *task, [[maybe_unused]] gpointer src, [[maybe_unused]] gpointer data,
                        [[maybe_unused]] GCancellable *cancellable) {
    GError *error = nullptr;
    GPtrArray *printers = xd_printers_list(&error);
    if (printers != nullptr)
        g_task_return_pointer(task, printers, (GDestroyNotify)g_ptr_array_unref);
    else
        g_task_return_error(task, error);
}

static void reload(folder *printers_folder) {
    /* One at a time; a change made meanwhile is fetched right after. */
    if (printers_folder->loading) {
        printers_folder->again = true;
        return;
    }
    printers_folder->loading = true;
    printers_folder->again = false;
    GTask *task = g_task_new(nullptr, printers_folder->cancel, on_listed, printers_folder);
    g_task_run_in_thread(task, list_thread);
    g_object_unref(task);
}

static gboolean on_poll(gpointer data) {
    reload(data);
    return G_SOURCE_CONTINUE;
}

static void on_changed(gpointer data) { reload(data); }

/* ---- actions ------------------------------------------------------------------------ */

static GtkWindow *win(folder *printers_folder) { return GTK_WINDOW(printers_folder->window); }

static void cb_open([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *printers_folder = data;
    GList *sel = selected_paths(printers_folder);
    for (GList *list = sel; list != nullptr; list = list->next) {
        GtkTreeIter iter;
        char *name = nullptr;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(printers_folder->store), &iter, list->data))
            gtk_tree_model_get(GTK_TREE_MODEL(printers_folder->store), &iter, C_NAME, &name, -1);
        if (name != nullptr)
            xd_print_queue_open(nullptr, name);
        g_free(name);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
}

static void cb_default([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *printers_folder = data;
    char *name = selected_name(printers_folder, nullptr);
    if (name != nullptr)
        xd_printer_do_set_default(win(printers_folder), name);
    g_free(name);
}

static void cb_prefs([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *printers_folder = data;
    char *name = selected_name(printers_folder, nullptr);
    if (name != nullptr)
        xd_printer_preferences_open(win(printers_folder), name);
    g_free(name);
}

static void cb_pause([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *printers_folder = data;
    bool paused = false;
    char *name = selected_name(printers_folder, &paused);
    if (name != nullptr)
        xd_printer_do_pause(win(printers_folder), name, !paused, nullptr, nullptr);
    g_free(name);
}

static void cb_cancel_all([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *printers_folder = data;
    char *name = selected_name(printers_folder, nullptr);
    if (name != nullptr)
        xd_printer_do_cancel_all(win(printers_folder), name);
    g_free(name);
}

static void cb_delete([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *printers_folder = data;
    char *name = selected_name(printers_folder, nullptr);
    if (name != nullptr)
        xd_printer_do_delete(win(printers_folder), name);
    g_free(name);
}

static void cb_props([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *printers_folder = data;
    char *name = selected_name(printers_folder, nullptr);
    if (name != nullptr)
        xd_printer_properties_open(win(printers_folder), name);
    g_free(name);
}

static void cb_add([[maybe_unused]] GtkWidget *widget, gpointer data) {
    xd_add_printer_wizard_open(win(data));
}

static void cb_refresh([[maybe_unused]] GtkWidget *widget, gpointer data) { reload(data); }

static void cb_close([[maybe_unused]] GtkWidget *widget, gpointer data) {
    gtk_widget_destroy(((folder *)data)->window);
}

static void on_icon_activated([[maybe_unused]] GtkIconView *icon_view,
                              [[maybe_unused]] GtkTreePath *path, gpointer data) {
    cb_open(nullptr, data);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tree_view,
                             [[maybe_unused]] GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    cb_open(nullptr, data);
}

/* ---- views -------------------------------------------------------------------------- */

static void set_view(folder *printers_folder, int view) {
    printers_folder->view = view;
    gtk_stack_set_visible_child(GTK_STACK(printers_folder->stack), printers_folder->pages[view]);
    printers_folder->syncing = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(printers_folder->view_items[view]), TRUE);
    printers_folder->syncing = false;
    update_status(printers_folder);
}

static void on_view_item(GtkCheckMenuItem *item, gpointer data) {
    folder *printers_folder = data;
    if (printers_folder->syncing || !gtk_check_menu_item_get_active(item))
        return;
    set_view(printers_folder, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "xd-view")));
}

static void on_selection_changed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    update_status(data);
}

static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static GtkWidget *popup_item(GtkWidget *menu, const char *label, GCallback cb,
                             folder *printers_folder) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, printers_folder);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static void separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* Right-click: a printer's menu, or the folder's on empty space. */
static void popup(folder *printers_folder, GdkEventButton *ev, bool on_item) {
    GtkWidget *menu = gtk_menu_new();
    if (on_item) {
        bool paused = false;
        g_free(selected_name(printers_folder, &paused));
        GtkWidget *open = popup_item(menu, "_Open", G_CALLBACK(cb_open), printers_folder);
        popup_item(menu, "Set as Def_ault Printer", G_CALLBACK(cb_default), printers_folder);
        popup_item(menu, "Printing Pr_eferences...", G_CALLBACK(cb_prefs), printers_folder);
        popup_item(menu, paused ? "Resume _Printing" : "_Pause Printing", G_CALLBACK(cb_pause),
                   printers_folder);
        popup_item(menu, "Ca_ncel All Documents", G_CALLBACK(cb_cancel_all), printers_folder);
        separator(menu);
        popup_item(menu, "_Delete", G_CALLBACK(cb_delete), printers_folder);
        separator(menu);
        popup_item(menu, "P_roperties", G_CALLBACK(cb_props), printers_folder);
        /* The double-click action, in bold as Explorer shows it. */
        gtk_label_set_markup_with_mnemonic(GTK_LABEL(gtk_bin_get_child(GTK_BIN(open))),
                                           "<b>_Open</b>");
    } else {
        popup_item(menu, "_Add Printer", G_CALLBACK(cb_add), printers_folder);
        separator(menu);
        popup_item(menu, "R_efresh", G_CALLBACK(cb_refresh), printers_folder);
    }
    gtk_menu_attach_to_widget(GTK_MENU(menu), printers_folder->views[printers_folder->view],
                              nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

static gboolean on_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    folder *printers_folder = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (GTK_IS_ICON_VIEW(widget)) {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget), (int)ev->x, (int)ev->y);
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
        if (path != nullptr)
            gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
    } else {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        gtk_tree_selection_unselect_all(sel);
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path,
                                          nullptr, nullptr, nullptr))
            gtk_tree_selection_select_path(sel, path);
    }
    popup(printers_folder, ev, path != nullptr);
    if (path != nullptr)
        gtk_tree_path_free(path);
    return TRUE;
}

static GtkWidget *icon_view(folder *printers_folder, bool tiles) {
    GtkWidget *widget = gtk_icon_view_new_with_model(GTK_TREE_MODEL(printers_folder->store));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(widget), GTK_SELECTION_SINGLE);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    if (tiles) {
        /* XP's Tiles: icon, then name, documents and status beside it. */
        gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(widget), GTK_ORIENTATION_HORIZONTAL);
        gtk_icon_view_set_item_width(GTK_ICON_VIEW(widget), 250);
        g_object_set(text, "xalign", 0.0, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 22,
                     nullptr);
    } else {
        g_object_set(text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 90, "alignment",
                     PANGO_ALIGN_CENTER, "xalign", 0.5, nullptr);
        gtk_icon_view_set_item_width(GTK_ICON_VIEW(widget), 96);
    }
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(widget), pix, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(widget), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(widget), pix, "surface", C_ICON);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(widget), text, tiles ? "markup" : "text",
                                  tiles ? C_TILE : C_NAME);
    g_signal_connect(widget, "item-activated", G_CALLBACK(on_icon_activated), printers_folder);
    g_signal_connect(widget, "selection-changed", G_CALLBACK(on_selection_changed),
                     printers_folder);
    g_signal_connect(widget, "button-press-event", G_CALLBACK(on_button), printers_folder);
    return widget;
}

static void text_column(GtkWidget *tree, const char *title, int col, int width, bool icon) {
    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(column, title);
    if (icon) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(column, pix, FALSE);
        gtk_tree_view_column_add_attribute(column, pix, "surface", C_SMALL_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    if (col == C_DOCS)
        g_object_set(text, "xalign", 1.0, nullptr);
    gtk_tree_view_column_pack_start(column, text, TRUE);
    gtk_tree_view_column_add_attribute(column, text, "text", col);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, width);
    gtk_tree_view_column_set_sort_column_id(column, col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
}

static GtkWidget *details_view(folder *printers_folder) {
    GtkWidget *widget = gtk_tree_view_new_with_model(GTK_TREE_MODEL(printers_folder->store));
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(widget), C_NAME);
    text_column(widget, "Name", C_NAME, 200, true);
    text_column(widget, "Documents", C_DOCS, 75, false);
    text_column(widget, "Status", C_STATUS, 90, false);
    text_column(widget, "Comments", C_COMMENT, 140, false);
    text_column(widget, "Location", C_LOCATION, 110, false);
    text_column(widget, "Model", C_MODEL, 180, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), printers_folder);
    g_signal_connect(widget, "row-activated", G_CALLBACK(on_row_activated), printers_folder);
    g_signal_connect(widget, "button-press-event", G_CALLBACK(on_button), printers_folder);
    return widget;
}

/* ---- menus -------------------------------------------------------------------------- */

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb,
                            folder *printers_folder, GtkAccelGroup *accel, guint key,
                            GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, printers_folder);
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

static GtkWidget *build_menus(folder *printers_folder, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    menu_item(file, "_Add Printer", G_CALLBACK(cb_add), printers_folder, accel, 0, NO_MODS);
    separator(file);
    printers_folder->open_item =
        menu_item(file, "_Open", G_CALLBACK(cb_open), printers_folder, accel, 0, NO_MODS);
    printers_folder->default_item =
        menu_item(file, "Set as Def_ault Printer", G_CALLBACK(cb_default), printers_folder, accel,
                  0, NO_MODS);
    printers_folder->prefs_item = menu_item(file, "Printing Pr_eferences...", G_CALLBACK(cb_prefs),
                                            printers_folder, accel, 0, NO_MODS);
    printers_folder->pause_item = menu_item(file, "_Pause Printing", G_CALLBACK(cb_pause),
                                            printers_folder, accel, 0, NO_MODS);
    printers_folder->cancel_item =
        menu_item(file, "Ca_ncel All Documents", G_CALLBACK(cb_cancel_all), printers_folder, accel,
                  0, NO_MODS);
    separator(file);
    printers_folder->delete_item = menu_item(file, "_Delete", G_CALLBACK(cb_delete),
                                             printers_folder, accel, GDK_KEY_Delete, NO_MODS);
    printers_folder->props_item = menu_item(file, "P_roperties", G_CALLBACK(cb_props),
                                            printers_folder, accel, GDK_KEY_Return, GDK_MOD1_MASK);
    separator(file);
    menu_item(file, "_Close", G_CALLBACK(cb_close), printers_folder, accel, GDK_KEY_w,
              GDK_CONTROL_MASK);

    GtkWidget *view = submenu(bar, "_View");
    GSList *group = nullptr;
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *radio = gtk_radio_menu_item_new_with_mnemonic(group, view_labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
        g_object_set_data(G_OBJECT(radio), "xd-view", GINT_TO_POINTER(i));
        g_signal_connect(radio, "toggled", G_CALLBACK(on_view_item), printers_folder);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), radio);
        printers_folder->view_items[i] = radio;
    }
    separator(view);
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), printers_folder, accel, GDK_KEY_F5,
              NO_MODS);
    return bar;
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    folder *printers_folder = data;
    g_cancellable_cancel(printers_folder->cancel);
    g_clear_object(&printers_folder->cancel);
    g_source_remove(printers_folder->poll);
    xd_printers_unwatch(on_changed, printers_folder);
    g_clear_object(&printers_folder->store);
}

static void open_folder(void) {
    static folder printers_folder;
    printers_folder.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(printers_folder.window), "Printers and Faxes");
    gtk_window_set_icon_name(GTK_WINDOW(printers_folder.window), "printer");
    gtk_window_set_default_size(GTK_WINDOW(printers_folder.window), 640, 420);
    xd_printers_hold(printers_folder.window);
    g_signal_connect(printers_folder.window, "destroy", G_CALLBACK(on_destroy), &printers_folder);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(printers_folder.window), accel);
    g_object_unref(accel);

    printers_folder.store = gtk_list_store_new(
        C_COUNT, CAIRO_GOBJECT_TYPE_SURFACE, CAIRO_GOBJECT_TYPE_SURFACE, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_BOOLEAN, G_TYPE_STRING);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(printers_folder.store), C_NAME,
                                         GTK_SORT_ASCENDING);
    printers_folder.cancel = g_cancellable_new();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(printers_folder.window), box);
    gtk_box_pack_start(GTK_BOX(box), build_menus(&printers_folder, accel), FALSE, FALSE, 0);
    printers_folder.stack = gtk_stack_new();
    printers_folder.views[VIEW_TILES] = icon_view(&printers_folder, true);
    printers_folder.views[VIEW_ICONS] = icon_view(&printers_folder, false);
    printers_folder.views[VIEW_DETAILS] = details_view(&printers_folder);
    for (int i = 0; i < VIEW_COUNT; i++) {
        GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(scroll), printers_folder.views[i]);
        gtk_stack_add_named(GTK_STACK(printers_folder.stack), scroll, view_labels[i]);
        printers_folder.pages[i] = scroll;
    }
    gtk_box_pack_start(GTK_BOX(box), printers_folder.stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    printers_folder.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(printers_folder.status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(printers_folder.status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(printers_folder.status, 6);
    gtk_widget_set_margin_top(printers_folder.status, 2);
    gtk_widget_set_margin_bottom(printers_folder.status, 2);
    gtk_box_pack_start(GTK_BOX(box), printers_folder.status, FALSE, FALSE, 0);

    gtk_widget_show_all(printers_folder.window);
    set_view(&printers_folder, VIEW_TILES);
    gtk_widget_grab_focus(printers_folder.views[VIEW_TILES]);
    reload(&printers_folder);
    xd_printers_watch(on_changed, &printers_folder);
    printers_folder.poll = g_timeout_add(XD_PRINTERS_POLL_MS, on_poll, &printers_folder);
}

int main(int argc, char **argv) {
    char *queue = nullptr, *props = nullptr, *prefs = nullptr;
    gboolean add = FALSE;
    GOptionEntry entries[] = {
        {"queue", 0, 0, G_OPTION_ARG_STRING, &queue, "Open a printer's queue", "NAME"},
        {"properties", 0, 0, G_OPTION_ARG_STRING, &props, "Open a printer's Properties", "NAME"},
        {"preferences", 0, 0, G_OPTION_ARG_STRING, &prefs, "Open its Printing Preferences",
         "NAME"},
        {"add", 0, 0, G_OPTION_ARG_NONE, &add, "Open the Add Printer Wizard", nullptr},
        G_OPTION_ENTRY_NULL,
    };
    xd_gtk_app_preinit("org.xeondesktop.Printers");
    GError *error = nullptr;
    if (!gtk_init_with_args(&argc, &argv, "- Printers and Faxes", entries, nullptr, &error)) {
        g_printerr("xd-printers: %s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }
    xd_gtk_app_follow_theme();
    xd_cups_auth_init();
    if (queue != nullptr)
        xd_print_queue_open(nullptr, queue);
    else if (props != nullptr)
        xd_printer_properties_open(nullptr, props);
    else if (prefs != nullptr)
        xd_printer_preferences_open(nullptr, prefs);
    else if (add)
        xd_add_printer_wizard_open(nullptr);
    else
        open_folder();
    g_free(queue);
    g_free(props);
    g_free(prefs);
    /* The printer asked for didn't exist (the error box has said so). */
    if (!xd_printers_any_open())
        return EXIT_FAILURE;
    gtk_main();
    return EXIT_SUCCESS;
}
