/* SPDX-License-Identifier: MIT
 * A printer's queue: XP's window of the documents waiting to print, with
 * its Printer and Document menus. Polled, as XP's was.
 */
#include <cups/cups.h>

#include "../gtkapp/gtkapp.h"
#include "printers.h"
#include "printlib.h"

static constexpr GdkModifierType NO_MODS = (GdkModifierType)0;

enum {
    Q_ID,
    Q_NAME,
    Q_STATUS,
    Q_OWNER,
    Q_PAGES,
    Q_SIZE,
    Q_SUBMITTED,
    Q_PORT,
    Q_STATE,
    Q_KEY, /* what the row shows, to leave unchanged rows alone */
    Q_COUNT
};

typedef struct queue {
    char *name;
    GtkWidget *window, *tree, *status;
    GtkWidget *pause_item, *doc_items[4];
    GtkListStore *store;
    GCancellable *cancel;
    guint poll;
    int busy; /* actions running, which may wait in a question or the password box */
    bool loading, again, paused, syncing, gone;
    bool seen; /* the printer, by a refresh (the window opens before) */
} queue;

/* What a refresh brings back from its thread. */
typedef struct snapshot {
    xd_printer *printer;
    GPtrArray *jobs;
} snapshot;

static void snapshot_free(snapshot *refreshed) {
    xd_printer_free(refreshed->printer);
    if (refreshed->jobs != nullptr)
        g_ptr_array_unref(refreshed->jobs);
    g_free(refreshed);
}

/* ---- the list ----------------------------------------------------------------------- */

static void set_row(queue *queue_window, GtkTreeIter *iter, const xd_print_job *document,
                    const char *port) {
    char *pages = xd_print_pages_text(document->pages_done, document->pages, document->state == 5);
    char *size = xd_print_size_text(document->size);
    GDateTime *dt = g_date_time_new_from_unix_local(document->created);
    char *when = dt != nullptr ? g_date_time_format(dt, "%X %x") : g_strdup("");
    if (dt != nullptr)
        g_date_time_unref(dt);
    const char *status = xd_print_job_status_text(document->state);
    char *key = g_strdup_printf("%d\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%d", document->id, document->name,
                                status, document->owner, pages, size, when, port, document->state);
    char *old = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(queue_window->store), iter, Q_KEY, &old, -1);
    /* Not set again at every poll when nothing changed: that redraws it. */
    if (g_strcmp0(old, key) != 0)
        gtk_list_store_set(queue_window->store, iter, Q_ID, document->id, Q_NAME, document->name,
                           Q_STATUS, status, Q_OWNER, document->owner, Q_PAGES, pages, Q_SIZE, size,
                           Q_SUBMITTED, when, Q_PORT, port, Q_STATE, document->state, Q_KEY, key,
                           -1);
    g_free(old);
    g_free(key);
    g_free(when);
    g_free(size);
    g_free(pages);
}

static void fill(queue *queue_window, GPtrArray *jobs, const char *port) {
    GHashTable *by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < jobs->len; i++) {
        xd_print_job *document = g_ptr_array_index(jobs, i);
        g_hash_table_insert(by_id, GINT_TO_POINTER(document->id), document);
    }
    GtkTreeModel *model = GTK_TREE_MODEL(queue_window->store);
    GtkTreeIter iter;
    gboolean more = gtk_tree_model_get_iter_first(model, &iter);
    while (more) {
        int id = 0;
        gtk_tree_model_get(model, &iter, Q_ID, &id, -1);
        xd_print_job *document = g_hash_table_lookup(by_id, GINT_TO_POINTER(id));
        if (document == nullptr) {
            more = gtk_list_store_remove(queue_window->store, &iter);
            continue;
        }
        set_row(queue_window, &iter, document, port);
        g_hash_table_remove(by_id, GINT_TO_POINTER(id));
        more = gtk_tree_model_iter_next(model, &iter);
    }
    /* New ones at the end: CUPS lists them oldest first. */
    for (guint i = 0; i < jobs->len; i++) {
        xd_print_job *document = g_ptr_array_index(jobs, i);
        if (!g_hash_table_contains(by_id, GINT_TO_POINTER(document->id)))
            continue;
        gtk_list_store_append(queue_window->store, &iter);
        set_row(queue_window, &iter, document, port);
    }
    g_hash_table_unref(by_id);
}

static GList *selected_ids(queue *queue_window) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(queue_window->tree));
    GList *rows = gtk_tree_selection_get_selected_rows(sel, nullptr);
    GList *ids = nullptr;
    for (GList *list = rows; list != nullptr; list = list->next) {
        GtkTreeIter iter;
        int id = 0;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(queue_window->store), &iter, list->data))
            gtk_tree_model_get(GTK_TREE_MODEL(queue_window->store), &iter, Q_ID, &id, -1);
        if (id > 0)
            ids = g_list_append(ids, GINT_TO_POINTER(id));
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return ids;
}

static void update_status(queue *queue_window) {
    int count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(queue_window->store), nullptr);
    char *text = g_strdup_printf("%d document(s) in queue", count);
    gtk_label_set_text(GTK_LABEL(queue_window->status), text);
    g_free(text);
    GList *ids = selected_ids(queue_window);
    for (size_t i = 0; i < G_N_ELEMENTS(queue_window->doc_items); i++)
        gtk_widget_set_sensitive(queue_window->doc_items[i], ids != nullptr);
    g_list_free(ids);
}

static void show_printer(queue *queue_window, const xd_printer *printer) {
    queue_window->paused = xd_printer_is_paused(printer->state, printer->reasons);
    const char *status =
        xd_printer_status_text(printer->state, printer->reasons, printer->accepting);
    /* XP's title says when the printer isn't simply ready. */
    char *title = strcmp(status, "Ready") == 0 || strcmp(status, "Printing") == 0
                      ? g_strdup(printer->name)
                      : g_strdup_printf("%s - %s", printer->name, status);
    gtk_window_set_title(GTK_WINDOW(queue_window->window), title);
    g_free(title);
    queue_window->syncing = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(queue_window->pause_item),
                                   queue_window->paused);
    queue_window->syncing = false;
}

static void reload(queue *queue_window);

static void on_loaded([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    snapshot *refreshed = g_task_propagate_pointer(G_TASK(res), &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }
    queue *queue_window = data;
    queue_window->loading = false;
    if (queue_window->again)
        reload(queue_window);
    if (refreshed == nullptr) {
        g_clear_error(&error);
        return; /* the scheduler went away; try again at the next poll */
    }
    if (refreshed->printer == nullptr) {
        snapshot_free(refreshed);
        if (!queue_window->seen) {
            /* It was gone before the window could show it. */
            char *msg = g_strdup_printf("The printer %s no longer exists.", queue_window->name);
            xd_message_box(GTK_WINDOW(queue_window->window), queue_window->name, XD_MB_ERROR,
                           XD_MB_OK, FALSE, msg);
            g_free(msg);
            gtk_widget_destroy(queue_window->window);
            return;
        }
        /* Deleted, here or elsewhere: nothing left to show. Not under an
         * action waiting in a question or the password box, though: it
         * still uses the window; it closes it when it's done. */
        if (queue_window->busy > 0)
            queue_window->gone = true;
        else
            gtk_widget_destroy(queue_window->window);
        return;
    }
    queue_window->seen = true;
    show_printer(queue_window, refreshed->printer);
    char *port = xd_printer_port_text(refreshed->printer->device_uri);
    if (refreshed->jobs != nullptr)
        fill(queue_window, refreshed->jobs, port);
    g_free(port);
    snapshot_free(refreshed);
    update_status(queue_window);
}

static void load_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *cancellable) {
    const char *name = data;
    GError *error = nullptr;
    snapshot *refreshed = g_new0(snapshot, 1);
    refreshed->printer = xd_printer_get(name, &error);
    if (refreshed->printer == nullptr && error != nullptr &&
        error->code != IPP_STATUS_ERROR_NOT_FOUND) {
        snapshot_free(refreshed);
        g_task_return_error(task, error);
        return;
    }
    g_clear_error(&error);
    if (refreshed->printer != nullptr)
        refreshed->jobs = xd_print_jobs_list(name, nullptr);
    g_task_return_pointer(task, refreshed, (GDestroyNotify)snapshot_free);
}

static void reload(queue *queue_window) {
    /* One at a time; a change made meanwhile is fetched right after. */
    if (queue_window->loading) {
        queue_window->again = true;
        return;
    }
    queue_window->loading = true;
    queue_window->again = false;
    GTask *task = g_task_new(nullptr, queue_window->cancel, on_loaded, queue_window);
    g_task_set_task_data(task, g_strdup(queue_window->name), g_free);
    g_task_run_in_thread(task, load_thread);
    g_object_unref(task);
}

static gboolean on_poll(gpointer data) {
    reload(data);
    return G_SOURCE_CONTINUE;
}

static void on_changed(gpointer data) { reload(data); }

/* ---- the Printer menu --------------------------------------------------------------- */

static GtkWindow *win(queue *queue_window) { return GTK_WINDOW(queue_window->window); }

/* Around an action: the window stays while it runs (see on_loaded). After
 * release, `queue_window` may be gone. */
static void hold(queue *queue_window) { queue_window->busy++; }

static void release(queue *queue_window) {
    if (--queue_window->busy == 0 && queue_window->gone)
        gtk_widget_destroy(queue_window->window);
}

static void cb_default([[maybe_unused]] GtkWidget *widget, gpointer data) {
    queue *queue_window = data;
    hold(queue_window);
    xd_printer_do_set_default(win(queue_window), queue_window->name);
    release(queue_window);
}

static void cb_prefs([[maybe_unused]] GtkWidget *widget, gpointer data) {
    queue *queue_window = data;
    hold(queue_window);
    xd_printer_preferences_open(win(queue_window), queue_window->name);
    release(queue_window);
}

/* Pause Printing failed: the check mark goes back. */
static void paused(bool ok, gpointer data) {
    queue *queue_window = data;
    if (!ok) {
        queue_window->syncing = true;
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(queue_window->pause_item),
                                       queue_window->paused);
        queue_window->syncing = false;
    }
    release(queue_window);
}

static void cb_pause(GtkCheckMenuItem *item, gpointer data) {
    queue *queue_window = data;
    if (queue_window->syncing)
        return;
    hold(queue_window);
    xd_printer_do_pause(win(queue_window), queue_window->name, gtk_check_menu_item_get_active(item),
                        paused, queue_window);
}

static void cb_cancel_all([[maybe_unused]] GtkWidget *widget, gpointer data) {
    queue *queue_window = data;
    hold(queue_window);
    xd_printer_do_cancel_all(win(queue_window), queue_window->name);
    release(queue_window);
}

static void cb_props([[maybe_unused]] GtkWidget *widget, gpointer data) {
    queue *queue_window = data;
    hold(queue_window);
    xd_printer_properties_open(win(queue_window), queue_window->name);
    release(queue_window);
}

static void cb_close([[maybe_unused]] GtkWidget *widget, gpointer data) {
    gtk_widget_destroy(((queue *)data)->window);
}

static void cb_refresh([[maybe_unused]] GtkWidget *widget, gpointer data) { reload(data); }

/* ---- the Document menu -------------------------------------------------------------- */

/* The selected documents and what to do with them, for the worker. */
typedef struct job_change {
    char *printer;
    GList *ids;
    xd_job_op op;
} job_change;

static void job_change_free(gpointer data) {
    job_change *job_request = data;
    g_free(job_request->printer);
    g_list_free(job_request->ids);
    g_free(job_request);
}

/* Each document in turn, stopping at the first that fails. */
static bool jobs_do(gpointer data, GError **error) {
    const job_change *job_request = data;
    for (const GList *list = job_request->ids; list != nullptr; list = list->next) {
        if (!xd_print_job_do(job_request->printer, GPOINTER_TO_INT(list->data), job_request->op,
                             error))
            return false;
    }
    return true;
}

static void jobs_done([[maybe_unused]] bool ok, gpointer data) { release(data); }

static void job_op(queue *queue_window, xd_job_op op) {
    GList *ids = selected_ids(queue_window);
    if (ids == nullptr)
        return;
    hold(queue_window);
    if (op == XD_JOB_CANCEL &&
        xd_message_box(win(queue_window), queue_window->name, XD_MB_QUESTION, XD_MB_YES_NO, TRUE,
                       g_list_length(ids) == 1
                           ? "Are you sure you want to cancel this document?"
                           : "Are you sure you want to cancel the selected documents?") !=
            GTK_RESPONSE_YES) {
        g_list_free(ids);
        release(queue_window);
        return;
    }
    job_change *job_request = g_new0(job_change, 1);
    job_request->printer = g_strdup(queue_window->name);
    job_request->ids = ids;
    job_request->op = op;
    xd_printers_run(win(queue_window), jobs_do, job_request, job_change_free, jobs_done,
                    queue_window);
}

static void cb_doc_pause([[maybe_unused]] GtkWidget *widget, gpointer data) {
    job_op(data, XD_JOB_PAUSE);
}
static void cb_doc_resume([[maybe_unused]] GtkWidget *widget, gpointer data) {
    job_op(data, XD_JOB_RESUME);
}
static void cb_doc_restart([[maybe_unused]] GtkWidget *widget, gpointer data) {
    job_op(data, XD_JOB_RESTART);
}
static void cb_doc_cancel([[maybe_unused]] GtkWidget *widget, gpointer data) {
    job_op(data, XD_JOB_CANCEL);
}

/* ---- the window --------------------------------------------------------------------- */

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, queue *queue_window,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, queue_window);
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

/* The Document menu's items, shared with the right-click menu. */
static void document_items(GtkWidget *menu, queue *queue_window, GtkAccelGroup *accel,
                           GtkWidget **out) {
    GtkWidget *items[4];
    items[0] = menu_item(menu, "_Pause", G_CALLBACK(cb_doc_pause), queue_window, accel, 0, NO_MODS);
    items[1] =
        menu_item(menu, "R_esume", G_CALLBACK(cb_doc_resume), queue_window, accel, 0, NO_MODS);
    items[2] =
        menu_item(menu, "Res_tart", G_CALLBACK(cb_doc_restart), queue_window, accel, 0, NO_MODS);
    items[3] = menu_item(menu, "_Cancel", G_CALLBACK(cb_doc_cancel), queue_window, accel,
                         accel != nullptr ? GDK_KEY_Delete : 0, NO_MODS);
    if (out != nullptr)
        memcpy(out, items, sizeof(items));
}

static GtkWidget *build_menus(queue *queue_window, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *printer = submenu(bar, "_Printer");
    menu_item(printer, "Set As Def_ault Printer", G_CALLBACK(cb_default), queue_window, accel, 0,
              NO_MODS);
    menu_item(printer, "Printing Pr_eferences...", G_CALLBACK(cb_prefs), queue_window, accel, 0,
              NO_MODS);
    queue_window->pause_item = gtk_check_menu_item_new_with_mnemonic("_Pause Printing");
    g_signal_connect(queue_window->pause_item, "toggled", G_CALLBACK(cb_pause), queue_window);
    gtk_menu_shell_append(GTK_MENU_SHELL(printer), queue_window->pause_item);
    menu_item(printer, "Ca_ncel All Documents", G_CALLBACK(cb_cancel_all), queue_window, accel, 0,
              NO_MODS);
    separator(printer);
    menu_item(printer, "P_roperties", G_CALLBACK(cb_props), queue_window, accel, 0, NO_MODS);
    separator(printer);
    menu_item(printer, "_Close", G_CALLBACK(cb_close), queue_window, accel, GDK_KEY_w,
              GDK_CONTROL_MASK);

    GtkWidget *doc = submenu(bar, "_Document");
    document_items(doc, queue_window, accel, queue_window->doc_items);

    GtkWidget *view = submenu(bar, "_View");
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), queue_window, accel, GDK_KEY_F5, NO_MODS);
    return bar;
}

static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static gboolean on_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    queue *queue_window = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path,
                                       nullptr, nullptr, nullptr))
        return FALSE;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
    if (!gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
    }
    gtk_tree_path_free(path);
    GtkWidget *menu = gtk_menu_new();
    document_items(menu, queue_window, nullptr, nullptr);
    gtk_menu_attach_to_widget(GTK_MENU(menu), widget, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static void on_selection([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    update_status(data);
}

static void column(GtkWidget *tree, const char *title, int col, int width, bool right) {
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", right ? 1.0 : 0.0, nullptr);
    GtkTreeViewColumn *view_column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(view_column, title);
    if (col == Q_NAME) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        g_object_set(pix, "icon-name", "text-x-generic", nullptr);
        gtk_tree_view_column_pack_start(view_column, pix, FALSE);
    }
    gtk_tree_view_column_pack_start(view_column, text, TRUE);
    gtk_tree_view_column_add_attribute(view_column, text, "text", col);
    gtk_tree_view_column_set_resizable(view_column, TRUE);
    gtk_tree_view_column_set_sizing(view_column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(view_column, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), view_column);
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    queue *queue_window = data;
    g_cancellable_cancel(queue_window->cancel);
    g_object_unref(queue_window->cancel);
    g_source_remove(queue_window->poll);
    xd_printers_unwatch(on_changed, queue_window);
    g_object_unref(queue_window->store);
    g_free(queue_window->name);
    g_free(queue_window);
}

/* The window opens at once; the first refresh fills it in (the
 * scheduler may take its time, or not answer at all). */
void xd_print_queue_open([[maybe_unused]] GtkWindow *parent, const char *name) {
    queue *queue_window = g_new0(queue, 1);
    queue_window->name = g_strdup(name);
    queue_window->cancel = g_cancellable_new();
    queue_window->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon_name(GTK_WINDOW(queue_window->window), "printer");
    gtk_window_set_default_size(GTK_WINDOW(queue_window->window), 720, 280);
    xd_printers_hold(queue_window->window);
    g_signal_connect(queue_window->window, "destroy", G_CALLBACK(on_destroy), queue_window);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(queue_window->window), accel);
    g_object_unref(accel);

    queue_window->store = gtk_list_store_new(
        Q_COUNT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(queue_window->window), box);
    gtk_box_pack_start(GTK_BOX(box), build_menus(queue_window, accel), FALSE, FALSE, 0);
    queue_window->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(queue_window->store));
    column(queue_window->tree, "Document Name", Q_NAME, 190, false);
    column(queue_window->tree, "Status", Q_STATUS, 80, false);
    column(queue_window->tree, "Owner", Q_OWNER, 80, false);
    column(queue_window->tree, "Pages", Q_PAGES, 50, true);
    column(queue_window->tree, "Size", Q_SIZE, 70, true);
    column(queue_window->tree, "Submitted", Q_SUBMITTED, 150, false);
    column(queue_window->tree, "Port", Q_PORT, 90, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(queue_window->tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection), queue_window);
    g_signal_connect(queue_window->tree, "button-press-event", G_CALLBACK(on_button), queue_window);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), queue_window->tree);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    queue_window->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(queue_window->status), 0);
    gtk_widget_set_margin_start(queue_window->status, 6);
    gtk_widget_set_margin_top(queue_window->status, 2);
    gtk_widget_set_margin_bottom(queue_window->status, 2);
    gtk_box_pack_start(GTK_BOX(box), queue_window->status, FALSE, FALSE, 0);

    gtk_window_set_title(GTK_WINDOW(queue_window->window), name);
    gtk_widget_show_all(queue_window->window);
    update_status(queue_window);
    reload(queue_window);
    xd_printers_watch(on_changed, queue_window);
    queue_window->poll = g_timeout_add(XD_PRINTERS_POLL_MS, on_poll, queue_window);
}
