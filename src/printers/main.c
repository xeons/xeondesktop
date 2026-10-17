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

static void set_row(folder *f, GtkTreeIter *iter, const xd_printer *p) {
    int scale = gtk_widget_get_scale_factor(f->window);
    cairo_surface_t *big = xd_printer_icon(p, 48, scale);
    cairo_surface_t *small = xd_printer_icon(p, 16, scale);
    const char *status = xd_printer_status_text(p->state, p->reasons, p->accepting);
    char *docs = g_strdup_printf("%d", p->jobs);
    char *tile = g_markup_printf_escaped("%s\n<span alpha='70%%'>%d\n%s</span>", p->name, p->jobs,
                                         status);
    gtk_list_store_set(f->store, iter, C_ICON, big, C_SMALL_ICON, small, C_NAME, p->name, C_TILE,
                       tile, C_DOCS, docs, C_STATUS, status, C_COMMENT, p->info, C_LOCATION,
                       p->location, C_MODEL, p->model, C_PAUSED,
                       xd_printer_is_paused(p->state, p->reasons), -1);
    g_free(tile);
    g_free(docs);
    cairo_surface_destroy(small);
    cairo_surface_destroy(big);
}

/* Updates the rows in place, so the selection and scroll position stay. */
static void fill(folder *f, GPtrArray *printers) {
    GHashTable *by_name = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < printers->len; i++) {
        xd_printer *p = g_ptr_array_index(printers, i);
        g_hash_table_insert(by_name, p->name, p);
    }
    GtkTreeModel *m = GTK_TREE_MODEL(f->store);
    GtkTreeIter iter;
    gboolean more = gtk_tree_model_get_iter_first(m, &iter);
    while (more) {
        char *name = nullptr;
        gtk_tree_model_get(m, &iter, C_NAME, &name, -1);
        xd_printer *p = g_hash_table_lookup(by_name, name);
        g_free(name);
        if (p == nullptr) {
            more = gtk_list_store_remove(f->store, &iter);
            continue;
        }
        set_row(f, &iter, p);
        g_hash_table_remove(by_name, p->name);
        more = gtk_tree_model_iter_next(m, &iter);
    }
    for (guint i = 0; i < printers->len; i++) {
        xd_printer *p = g_ptr_array_index(printers, i);
        if (!g_hash_table_contains(by_name, p->name))
            continue;
        gtk_list_store_append(f->store, &iter);
        set_row(f, &iter, p);
    }
    g_hash_table_unref(by_name);
}

static GList *selected_paths(folder *f) {
    GtkWidget *v = f->views[f->view];
    if (GTK_IS_ICON_VIEW(v))
        return gtk_icon_view_get_selected_items(GTK_ICON_VIEW(v));
    return gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(v)),
                                                nullptr);
}

/* The selected printer's name (the first, if several), or nullptr. */
static char *selected_name(folder *f, bool *paused) {
    GList *sel = selected_paths(f);
    char *name = nullptr;
    GtkTreeIter iter;
    if (sel != nullptr && gtk_tree_model_get_iter(GTK_TREE_MODEL(f->store), &iter, sel->data)) {
        gboolean p = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(f->store), &iter, C_NAME, &name, C_PAUSED, &p, -1);
        if (paused != nullptr)
            *paused = p;
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    return name;
}

static void update_status(folder *f) {
    bool paused = false;
    char *name = selected_name(f, &paused);
    bool one = name != nullptr;
    GtkWidget *items[] = {f->open_item,   f->default_item, f->prefs_item, f->pause_item,
                          f->cancel_item, f->delete_item,  f->props_item};
    for (size_t i = 0; i < G_N_ELEMENTS(items); i++)
        gtk_widget_set_sensitive(items[i], one);
    gtk_menu_item_set_label(GTK_MENU_ITEM(f->pause_item),
                            paused ? "Resume _Printing" : "_Pause Printing");
    int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(f->store), nullptr);
    char *text = one ? g_strdup_printf("%s", name) : g_strdup_printf("%d object(s)", n);
    gtk_label_set_text(GTK_LABEL(f->status), text);
    g_free(text);
    g_free(name);
}

static void reload(folder *f);

static void on_listed([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    GPtrArray *printers = g_task_propagate_pointer(G_TASK(res), &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return; /* the window is gone */
    }
    folder *f = data;
    f->loading = false;
    if (f->again)
        reload(f);
    if (printers == nullptr) {
        gtk_list_store_clear(f->store);
        /* Once, not at every poll. */
        if (!f->error_shown) {
            f->error_shown = true;
            xd_printers_error(GTK_WINDOW(f->window), error);
        }
        g_clear_error(&error);
        update_status(f);
        return;
    }
    f->error_shown = false;
    fill(f, printers);
    g_ptr_array_unref(printers);
    update_status(f);
}

static void list_thread(GTask *task, [[maybe_unused]] gpointer src, [[maybe_unused]] gpointer data,
                        [[maybe_unused]] GCancellable *c) {
    GError *error = nullptr;
    GPtrArray *printers = xd_printers_list(&error);
    if (printers != nullptr)
        g_task_return_pointer(task, printers, (GDestroyNotify)g_ptr_array_unref);
    else
        g_task_return_error(task, error);
}

static void reload(folder *f) {
    /* One at a time; a change made meanwhile is fetched right after. */
    if (f->loading) {
        f->again = true;
        return;
    }
    f->loading = true;
    f->again = false;
    GTask *task = g_task_new(nullptr, f->cancel, on_listed, f);
    g_task_run_in_thread(task, list_thread);
    g_object_unref(task);
}

static gboolean on_poll(gpointer data) {
    reload(data);
    return G_SOURCE_CONTINUE;
}

static void on_changed(gpointer data) { reload(data); }

/* ---- actions ------------------------------------------------------------------------ */

static GtkWindow *win(folder *f) { return GTK_WINDOW(f->window); }

static void cb_open([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    GList *sel = selected_paths(f);
    for (GList *l = sel; l != nullptr; l = l->next) {
        GtkTreeIter iter;
        char *name = nullptr;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(f->store), &iter, l->data))
            gtk_tree_model_get(GTK_TREE_MODEL(f->store), &iter, C_NAME, &name, -1);
        if (name != nullptr)
            xd_print_queue_open(nullptr, name);
        g_free(name);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
}

static void cb_default([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    char *name = selected_name(f, nullptr);
    if (name != nullptr)
        xd_printer_do_set_default(win(f), name);
    g_free(name);
}

static void cb_prefs([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    char *name = selected_name(f, nullptr);
    if (name != nullptr)
        xd_printer_preferences_open(win(f), name);
    g_free(name);
}

static void cb_pause([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    bool paused = false;
    char *name = selected_name(f, &paused);
    if (name != nullptr)
        xd_printer_do_pause(win(f), name, !paused);
    g_free(name);
}

static void cb_cancel_all([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    char *name = selected_name(f, nullptr);
    if (name != nullptr)
        xd_printer_do_cancel_all(win(f), name);
    g_free(name);
}

static void cb_delete([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    char *name = selected_name(f, nullptr);
    if (name != nullptr)
        xd_printer_do_delete(win(f), name);
    g_free(name);
}

static void cb_props([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    char *name = selected_name(f, nullptr);
    if (name != nullptr)
        xd_printer_properties_open(win(f), name);
    g_free(name);
}

static void cb_add([[maybe_unused]] GtkWidget *w, gpointer data) {
    xd_add_printer_wizard_open(win(data));
}

static void cb_refresh([[maybe_unused]] GtkWidget *w, gpointer data) { reload(data); }

static void cb_close([[maybe_unused]] GtkWidget *w, gpointer data) {
    gtk_widget_destroy(((folder *)data)->window);
}

static void on_icon_activated([[maybe_unused]] GtkIconView *v, [[maybe_unused]] GtkTreePath *p,
                              gpointer data) {
    cb_open(nullptr, data);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    cb_open(nullptr, data);
}

/* ---- views -------------------------------------------------------------------------- */

static void set_view(folder *f, int view) {
    f->view = view;
    gtk_stack_set_visible_child(GTK_STACK(f->stack), f->pages[view]);
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

static void on_selection_changed([[maybe_unused]] GtkWidget *w, gpointer data) {
    update_status(data);
}

static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static GtkWidget *popup_item(GtkWidget *menu, const char *label, GCallback cb, folder *f) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, f);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static void separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* Right-click: a printer's menu, or the folder's on empty space. */
static void popup(folder *f, GdkEventButton *ev, bool on_item) {
    GtkWidget *menu = gtk_menu_new();
    if (on_item) {
        bool paused = false;
        g_free(selected_name(f, &paused));
        GtkWidget *open = popup_item(menu, "_Open", G_CALLBACK(cb_open), f);
        popup_item(menu, "Set as Def_ault Printer", G_CALLBACK(cb_default), f);
        popup_item(menu, "Printing Pr_eferences...", G_CALLBACK(cb_prefs), f);
        popup_item(menu, paused ? "Resume _Printing" : "_Pause Printing", G_CALLBACK(cb_pause),
                   f);
        popup_item(menu, "Ca_ncel All Documents", G_CALLBACK(cb_cancel_all), f);
        separator(menu);
        popup_item(menu, "_Delete", G_CALLBACK(cb_delete), f);
        separator(menu);
        popup_item(menu, "P_roperties", G_CALLBACK(cb_props), f);
        /* The double-click action, in bold as Explorer shows it. */
        gtk_label_set_markup_with_mnemonic(GTK_LABEL(gtk_bin_get_child(GTK_BIN(open))),
                                           "<b>_Open</b>");
    } else {
        popup_item(menu, "_Add Printer", G_CALLBACK(cb_add), f);
        separator(menu);
        popup_item(menu, "R_efresh", G_CALLBACK(cb_refresh), f);
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
    GtkTreePath *path = nullptr;
    if (GTK_IS_ICON_VIEW(w)) {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w), (int)ev->x, (int)ev->y);
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(w));
        if (path != nullptr)
            gtk_icon_view_select_path(GTK_ICON_VIEW(w), path);
    } else {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
        gtk_tree_selection_unselect_all(sel);
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path,
                                          nullptr, nullptr, nullptr))
            gtk_tree_selection_select_path(sel, path);
    }
    popup(f, ev, path != nullptr);
    if (path != nullptr)
        gtk_tree_path_free(path);
    return TRUE;
}

static GtkWidget *icon_view(folder *f, bool tiles) {
    GtkWidget *v = gtk_icon_view_new_with_model(GTK_TREE_MODEL(f->store));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(v), GTK_SELECTION_SINGLE);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    if (tiles) {
        /* XP's Tiles: icon, then name, documents and status beside it. */
        gtk_icon_view_set_item_orientation(GTK_ICON_VIEW(v), GTK_ORIENTATION_HORIZONTAL);
        gtk_icon_view_set_item_width(GTK_ICON_VIEW(v), 250);
        g_object_set(text, "xalign", 0.0, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 22,
                     nullptr);
    } else {
        g_object_set(text, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 90, "alignment",
                     PANGO_ALIGN_CENTER, "xalign", 0.5, nullptr);
        gtk_icon_view_set_item_width(GTK_ICON_VIEW(v), 96);
    }
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v), pix, FALSE);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(v), pix, "surface", C_ICON);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(v), text, tiles ? "markup" : "text",
                                  tiles ? C_TILE : C_NAME);
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
        gtk_tree_view_column_add_attribute(c, pix, "surface", C_SMALL_ICON);
    }
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    if (col == C_DOCS)
        g_object_set(text, "xalign", 1.0, nullptr);
    gtk_tree_view_column_pack_start(c, text, TRUE);
    gtk_tree_view_column_add_attribute(c, text, "text", col);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_column_set_sort_column_id(c, col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
}

static GtkWidget *details_view(folder *f) {
    GtkWidget *v = gtk_tree_view_new_with_model(GTK_TREE_MODEL(f->store));
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(v), C_NAME);
    text_column(v, "Name", C_NAME, 200, true);
    text_column(v, "Documents", C_DOCS, 75, false);
    text_column(v, "Status", C_STATUS, 90, false);
    text_column(v, "Comments", C_COMMENT, 140, false);
    text_column(v, "Location", C_LOCATION, 110, false);
    text_column(v, "Model", C_MODEL, 180, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(v));
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), f);
    g_signal_connect(v, "row-activated", G_CALLBACK(on_row_activated), f);
    g_signal_connect(v, "button-press-event", G_CALLBACK(on_button), f);
    return v;
}

/* ---- menus -------------------------------------------------------------------------- */

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

static GtkWidget *build_menus(folder *f, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = submenu(bar, "_File");
    menu_item(file, "_Add Printer", G_CALLBACK(cb_add), f, accel, 0, NO_MODS);
    separator(file);
    f->open_item = menu_item(file, "_Open", G_CALLBACK(cb_open), f, accel, 0, NO_MODS);
    f->default_item = menu_item(file, "Set as Def_ault Printer", G_CALLBACK(cb_default), f,
                                accel, 0, NO_MODS);
    f->prefs_item = menu_item(file, "Printing Pr_eferences...", G_CALLBACK(cb_prefs), f, accel,
                              0, NO_MODS);
    f->pause_item = menu_item(file, "_Pause Printing", G_CALLBACK(cb_pause), f, accel, 0,
                              NO_MODS);
    f->cancel_item = menu_item(file, "Ca_ncel All Documents", G_CALLBACK(cb_cancel_all), f,
                               accel, 0, NO_MODS);
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
    return bar;
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    folder *f = data;
    g_cancellable_cancel(f->cancel);
    g_clear_object(&f->cancel);
    g_source_remove(f->poll);
    xd_printers_unwatch(on_changed, f);
    g_clear_object(&f->store);
}

static void open_folder(void) {
    static folder f;
    f.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(f.window), "Printers and Faxes");
    gtk_window_set_icon_name(GTK_WINDOW(f.window), "printer");
    gtk_window_set_default_size(GTK_WINDOW(f.window), 640, 420);
    xd_printers_hold(f.window);
    g_signal_connect(f.window, "destroy", G_CALLBACK(on_destroy), &f);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(f.window), accel);
    g_object_unref(accel);

    f.store = gtk_list_store_new(C_COUNT, CAIRO_GOBJECT_TYPE_SURFACE, CAIRO_GOBJECT_TYPE_SURFACE,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(f.store), C_NAME, GTK_SORT_ASCENDING);
    f.cancel = g_cancellable_new();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(f.window), box);
    gtk_box_pack_start(GTK_BOX(box), build_menus(&f, accel), FALSE, FALSE, 0);
    f.stack = gtk_stack_new();
    f.views[VIEW_TILES] = icon_view(&f, true);
    f.views[VIEW_ICONS] = icon_view(&f, false);
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
    gtk_label_set_ellipsize(GTK_LABEL(f.status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(f.status, 6);
    gtk_widget_set_margin_top(f.status, 2);
    gtk_widget_set_margin_bottom(f.status, 2);
    gtk_box_pack_start(GTK_BOX(box), f.status, FALSE, FALSE, 0);

    gtk_widget_show_all(f.window);
    set_view(&f, VIEW_TILES);
    gtk_widget_grab_focus(f.views[VIEW_TILES]);
    reload(&f);
    xd_printers_watch(on_changed, &f);
    f.poll = g_timeout_add(XD_PRINTERS_POLL_MS, on_poll, &f);
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
