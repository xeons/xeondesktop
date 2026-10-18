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

static void snapshot_free(snapshot *s) {
    xd_printer_free(s->printer);
    if (s->jobs != nullptr)
        g_ptr_array_unref(s->jobs);
    g_free(s);
}

/* ---- the list ----------------------------------------------------------------------- */

static void set_row(queue *q, GtkTreeIter *iter, const xd_print_job *j, const char *port) {
    char *pages = xd_print_pages_text(j->pages_done, j->pages, j->state == 5);
    char *size = xd_print_size_text(j->size);
    GDateTime *dt = g_date_time_new_from_unix_local(j->created);
    char *when = dt != nullptr ? g_date_time_format(dt, "%X %x") : g_strdup("");
    if (dt != nullptr)
        g_date_time_unref(dt);
    const char *status = xd_print_job_status_text(j->state);
    char *key = g_strdup_printf("%d\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%d", j->id, j->name, status,
                                j->owner, pages, size, when, port, j->state);
    char *old = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(q->store), iter, Q_KEY, &old, -1);
    /* Not set again at every poll when nothing changed: that redraws it. */
    if (g_strcmp0(old, key) != 0)
        gtk_list_store_set(q->store, iter, Q_ID, j->id, Q_NAME, j->name, Q_STATUS, status,
                           Q_OWNER, j->owner, Q_PAGES, pages, Q_SIZE, size, Q_SUBMITTED, when,
                           Q_PORT, port, Q_STATE, j->state, Q_KEY, key, -1);
    g_free(old);
    g_free(key);
    g_free(when);
    g_free(size);
    g_free(pages);
}

static void fill(queue *q, GPtrArray *jobs, const char *port) {
    GHashTable *by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < jobs->len; i++) {
        xd_print_job *j = g_ptr_array_index(jobs, i);
        g_hash_table_insert(by_id, GINT_TO_POINTER(j->id), j);
    }
    GtkTreeModel *m = GTK_TREE_MODEL(q->store);
    GtkTreeIter iter;
    gboolean more = gtk_tree_model_get_iter_first(m, &iter);
    while (more) {
        int id = 0;
        gtk_tree_model_get(m, &iter, Q_ID, &id, -1);
        xd_print_job *j = g_hash_table_lookup(by_id, GINT_TO_POINTER(id));
        if (j == nullptr) {
            more = gtk_list_store_remove(q->store, &iter);
            continue;
        }
        set_row(q, &iter, j, port);
        g_hash_table_remove(by_id, GINT_TO_POINTER(id));
        more = gtk_tree_model_iter_next(m, &iter);
    }
    /* New ones at the end: CUPS lists them oldest first. */
    for (guint i = 0; i < jobs->len; i++) {
        xd_print_job *j = g_ptr_array_index(jobs, i);
        if (!g_hash_table_contains(by_id, GINT_TO_POINTER(j->id)))
            continue;
        gtk_list_store_append(q->store, &iter);
        set_row(q, &iter, j, port);
    }
    g_hash_table_unref(by_id);
}

static GList *selected_ids(queue *q) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(q->tree));
    GList *rows = gtk_tree_selection_get_selected_rows(sel, nullptr);
    GList *ids = nullptr;
    for (GList *l = rows; l != nullptr; l = l->next) {
        GtkTreeIter iter;
        int id = 0;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(q->store), &iter, l->data))
            gtk_tree_model_get(GTK_TREE_MODEL(q->store), &iter, Q_ID, &id, -1);
        if (id > 0)
            ids = g_list_append(ids, GINT_TO_POINTER(id));
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return ids;
}

static void update_status(queue *q) {
    int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(q->store), nullptr);
    char *text = g_strdup_printf("%d document(s) in queue", n);
    gtk_label_set_text(GTK_LABEL(q->status), text);
    g_free(text);
    GList *ids = selected_ids(q);
    for (size_t i = 0; i < G_N_ELEMENTS(q->doc_items); i++)
        gtk_widget_set_sensitive(q->doc_items[i], ids != nullptr);
    g_list_free(ids);
}

static void show_printer(queue *q, const xd_printer *p) {
    q->paused = xd_printer_is_paused(p->state, p->reasons);
    const char *status = xd_printer_status_text(p->state, p->reasons, p->accepting);
    /* XP's title says when the printer isn't simply ready. */
    char *title = strcmp(status, "Ready") == 0 || strcmp(status, "Printing") == 0
                      ? g_strdup(p->name)
                      : g_strdup_printf("%s - %s", p->name, status);
    gtk_window_set_title(GTK_WINDOW(q->window), title);
    g_free(title);
    q->syncing = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(q->pause_item), q->paused);
    q->syncing = false;
}

static void reload(queue *q);

static void on_loaded([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    snapshot *s = g_task_propagate_pointer(G_TASK(res), &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        return;
    }
    queue *q = data;
    q->loading = false;
    if (q->again)
        reload(q);
    if (s == nullptr) {
        g_clear_error(&error);
        return; /* the scheduler went away; try again at the next poll */
    }
    if (s->printer == nullptr) {
        snapshot_free(s);
        if (!q->seen) {
            /* It was gone before the window could show it. */
            char *msg = g_strdup_printf("The printer %s no longer exists.", q->name);
            xd_message_box(GTK_WINDOW(q->window), q->name, XD_MB_ERROR, XD_MB_OK, FALSE, msg);
            g_free(msg);
            gtk_widget_destroy(q->window);
            return;
        }
        /* Deleted, here or elsewhere: nothing left to show. Not under an
         * action waiting in a question or the password box, though: it
         * still uses the window; it closes it when it's done. */
        if (q->busy > 0)
            q->gone = true;
        else
            gtk_widget_destroy(q->window);
        return;
    }
    q->seen = true;
    show_printer(q, s->printer);
    char *port = xd_printer_port_text(s->printer->device_uri);
    if (s->jobs != nullptr)
        fill(q, s->jobs, port);
    g_free(port);
    snapshot_free(s);
    update_status(q);
}

static void load_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *c) {
    const char *name = data;
    GError *error = nullptr;
    snapshot *s = g_new0(snapshot, 1);
    s->printer = xd_printer_get(name, &error);
    if (s->printer == nullptr && error != nullptr && error->code != IPP_STATUS_ERROR_NOT_FOUND) {
        snapshot_free(s);
        g_task_return_error(task, error);
        return;
    }
    g_clear_error(&error);
    if (s->printer != nullptr)
        s->jobs = xd_print_jobs_list(name, nullptr);
    g_task_return_pointer(task, s, (GDestroyNotify)snapshot_free);
}

static void reload(queue *q) {
    /* One at a time; a change made meanwhile is fetched right after. */
    if (q->loading) {
        q->again = true;
        return;
    }
    q->loading = true;
    q->again = false;
    GTask *task = g_task_new(nullptr, q->cancel, on_loaded, q);
    g_task_set_task_data(task, g_strdup(q->name), g_free);
    g_task_run_in_thread(task, load_thread);
    g_object_unref(task);
}

static gboolean on_poll(gpointer data) {
    reload(data);
    return G_SOURCE_CONTINUE;
}

static void on_changed(gpointer data) { reload(data); }

/* ---- the Printer menu --------------------------------------------------------------- */

static GtkWindow *win(queue *q) { return GTK_WINDOW(q->window); }

/* Around an action: the window stays while it runs (see on_loaded). After
 * release, `q` may be gone. */
static void hold(queue *q) { q->busy++; }

static void release(queue *q) {
    if (--q->busy == 0 && q->gone)
        gtk_widget_destroy(q->window);
}

static void cb_default([[maybe_unused]] GtkWidget *w, gpointer data) {
    queue *q = data;
    hold(q);
    xd_printer_do_set_default(win(q), q->name);
    release(q);
}

static void cb_prefs([[maybe_unused]] GtkWidget *w, gpointer data) {
    queue *q = data;
    hold(q);
    xd_printer_preferences_open(win(q), q->name);
    release(q);
}

/* Pause Printing failed: the check mark goes back. */
static void paused(bool ok, gpointer data) {
    queue *q = data;
    if (!ok) {
        q->syncing = true;
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(q->pause_item), q->paused);
        q->syncing = false;
    }
    release(q);
}

static void cb_pause(GtkCheckMenuItem *item, gpointer data) {
    queue *q = data;
    if (q->syncing)
        return;
    hold(q);
    xd_printer_do_pause(win(q), q->name, gtk_check_menu_item_get_active(item), paused, q);
}

static void cb_cancel_all([[maybe_unused]] GtkWidget *w, gpointer data) {
    queue *q = data;
    hold(q);
    xd_printer_do_cancel_all(win(q), q->name);
    release(q);
}

static void cb_props([[maybe_unused]] GtkWidget *w, gpointer data) {
    queue *q = data;
    hold(q);
    xd_printer_properties_open(win(q), q->name);
    release(q);
}

static void cb_close([[maybe_unused]] GtkWidget *w, gpointer data) {
    gtk_widget_destroy(((queue *)data)->window);
}

static void cb_refresh([[maybe_unused]] GtkWidget *w, gpointer data) { reload(data); }

/* ---- the Document menu -------------------------------------------------------------- */

/* The selected documents and what to do with them, for the worker. */
typedef struct job_change {
    char *printer;
    GList *ids;
    xd_job_op op;
} job_change;

static void job_change_free(gpointer data) {
    job_change *c = data;
    g_free(c->printer);
    g_list_free(c->ids);
    g_free(c);
}

/* Each document in turn, stopping at the first that fails. */
static bool jobs_do(gpointer data, GError **error) {
    const job_change *c = data;
    for (const GList *l = c->ids; l != nullptr; l = l->next) {
        if (!xd_print_job_do(c->printer, GPOINTER_TO_INT(l->data), c->op, error))
            return false;
    }
    return true;
}

static void jobs_done([[maybe_unused]] bool ok, gpointer data) { release(data); }

static void job_op(queue *q, xd_job_op op) {
    GList *ids = selected_ids(q);
    if (ids == nullptr)
        return;
    hold(q);
    if (op == XD_JOB_CANCEL &&
        xd_message_box(win(q), q->name, XD_MB_QUESTION, XD_MB_YES_NO, TRUE,
                       g_list_length(ids) == 1
                           ? "Are you sure you want to cancel this document?"
                           : "Are you sure you want to cancel the selected documents?") !=
            GTK_RESPONSE_YES) {
        g_list_free(ids);
        release(q);
        return;
    }
    job_change *c = g_new0(job_change, 1);
    c->printer = g_strdup(q->name);
    c->ids = ids;
    c->op = op;
    xd_printers_run(win(q), jobs_do, c, job_change_free, jobs_done, q);
}

static void cb_doc_pause([[maybe_unused]] GtkWidget *w, gpointer data) {
    job_op(data, XD_JOB_PAUSE);
}
static void cb_doc_resume([[maybe_unused]] GtkWidget *w, gpointer data) {
    job_op(data, XD_JOB_RESUME);
}
static void cb_doc_restart([[maybe_unused]] GtkWidget *w, gpointer data) {
    job_op(data, XD_JOB_RESTART);
}
static void cb_doc_cancel([[maybe_unused]] GtkWidget *w, gpointer data) {
    job_op(data, XD_JOB_CANCEL);
}

/* ---- the window --------------------------------------------------------------------- */

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, queue *q,
                            GtkAccelGroup *accel, guint key, GdkModifierType mods) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", cb, q);
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
static void document_items(GtkWidget *menu, queue *q, GtkAccelGroup *accel, GtkWidget **out) {
    GtkWidget *items[4];
    items[0] = menu_item(menu, "_Pause", G_CALLBACK(cb_doc_pause), q, accel, 0, NO_MODS);
    items[1] = menu_item(menu, "R_esume", G_CALLBACK(cb_doc_resume), q, accel, 0, NO_MODS);
    items[2] = menu_item(menu, "Res_tart", G_CALLBACK(cb_doc_restart), q, accel, 0, NO_MODS);
    items[3] = menu_item(menu, "_Cancel", G_CALLBACK(cb_doc_cancel), q, accel,
                         accel != nullptr ? GDK_KEY_Delete : 0, NO_MODS);
    if (out != nullptr)
        memcpy(out, items, sizeof(items));
}

static GtkWidget *build_menus(queue *q, GtkAccelGroup *accel) {
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *printer = submenu(bar, "_Printer");
    menu_item(printer, "Set As Def_ault Printer", G_CALLBACK(cb_default), q, accel, 0, NO_MODS);
    menu_item(printer, "Printing Pr_eferences...", G_CALLBACK(cb_prefs), q, accel, 0, NO_MODS);
    q->pause_item = gtk_check_menu_item_new_with_mnemonic("_Pause Printing");
    g_signal_connect(q->pause_item, "toggled", G_CALLBACK(cb_pause), q);
    gtk_menu_shell_append(GTK_MENU_SHELL(printer), q->pause_item);
    menu_item(printer, "Ca_ncel All Documents", G_CALLBACK(cb_cancel_all), q, accel, 0, NO_MODS);
    separator(printer);
    menu_item(printer, "P_roperties", G_CALLBACK(cb_props), q, accel, 0, NO_MODS);
    separator(printer);
    menu_item(printer, "_Close", G_CALLBACK(cb_close), q, accel, GDK_KEY_w, GDK_CONTROL_MASK);

    GtkWidget *doc = submenu(bar, "_Document");
    document_items(doc, q, accel, q->doc_items);

    GtkWidget *view = submenu(bar, "_View");
    menu_item(view, "_Refresh", G_CALLBACK(cb_refresh), q, accel, GDK_KEY_F5, NO_MODS);
    return bar;
}

static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    queue *q = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, nullptr,
                                       nullptr, nullptr))
        return FALSE;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
    if (!gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
    }
    gtk_tree_path_free(path);
    GtkWidget *menu = gtk_menu_new();
    document_items(menu, q, nullptr, nullptr);
    gtk_menu_attach_to_widget(GTK_MENU(menu), w, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu), nullptr);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

static void on_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    update_status(data);
}

static void column(GtkWidget *tree, const char *title, int col, int width, bool right) {
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", right ? 1.0 : 0.0, nullptr);
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, title);
    if (col == Q_NAME) {
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        g_object_set(pix, "icon-name", "text-x-generic", nullptr);
        gtk_tree_view_column_pack_start(c, pix, FALSE);
    }
    gtk_tree_view_column_pack_start(c, text, TRUE);
    gtk_tree_view_column_add_attribute(c, text, "text", col);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    queue *q = data;
    g_cancellable_cancel(q->cancel);
    g_object_unref(q->cancel);
    g_source_remove(q->poll);
    xd_printers_unwatch(on_changed, q);
    g_object_unref(q->store);
    g_free(q->name);
    g_free(q);
}

/* The window opens at once; the first refresh fills it in (the
 * scheduler may take its time, or not answer at all). */
void xd_print_queue_open([[maybe_unused]] GtkWindow *parent, const char *name) {
    queue *q = g_new0(queue, 1);
    q->name = g_strdup(name);
    q->cancel = g_cancellable_new();
    q->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon_name(GTK_WINDOW(q->window), "printer");
    gtk_window_set_default_size(GTK_WINDOW(q->window), 720, 280);
    xd_printers_hold(q->window);
    g_signal_connect(q->window, "destroy", G_CALLBACK(on_destroy), q);
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(q->window), accel);
    g_object_unref(accel);

    q->store = gtk_list_store_new(Q_COUNT, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(q->window), box);
    gtk_box_pack_start(GTK_BOX(box), build_menus(q, accel), FALSE, FALSE, 0);
    q->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(q->store));
    column(q->tree, "Document Name", Q_NAME, 190, false);
    column(q->tree, "Status", Q_STATUS, 80, false);
    column(q->tree, "Owner", Q_OWNER, 80, false);
    column(q->tree, "Pages", Q_PAGES, 50, true);
    column(q->tree, "Size", Q_SIZE, 70, true);
    column(q->tree, "Submitted", Q_SUBMITTED, 150, false);
    column(q->tree, "Port", Q_PORT, 90, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(q->tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection), q);
    g_signal_connect(q->tree, "button-press-event", G_CALLBACK(on_button), q);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), q->tree);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    q->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(q->status), 0);
    gtk_widget_set_margin_start(q->status, 6);
    gtk_widget_set_margin_top(q->status, 2);
    gtk_widget_set_margin_bottom(q->status, 2);
    gtk_box_pack_start(GTK_BOX(box), q->status, FALSE, FALSE, 0);

    gtk_window_set_title(GTK_WINDOW(q->window), name);
    gtk_widget_show_all(q->window);
    update_status(q);
    reload(q);
    xd_printers_watch(on_changed, q);
    q->poll = g_timeout_add(XD_PRINTERS_POLL_MS, on_poll, q);
}
