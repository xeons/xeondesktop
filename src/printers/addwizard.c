/* SPDX-License-Identifier: MIT
 * XP's Add Printer Wizard, for what Linux needs it for: a printer on the
 * network. USB printers CUPS sets up by itself. The wizard lists the
 * printers DNS-SD finds (cupsEnumDests) or takes an address, then adds
 * an IPP Everywhere queue for it (CUPS-Add-Modify-Printer with the
 * "everywhere" model: the scheduler asks the printer what it can do; no
 * driver needed).
 */
#include <cups/cups.h>

#include "../cpl/wizard.h"
#include "../gtkapp/gtkapp.h"
#include "auth.h"
#include "printers.h"
#include "printlib.h"

enum { PAGE_WELCOME, PAGE_FIND, PAGE_NAME, PAGE_TEST, PAGE_DONE };
enum { F_NAME, F_LOCATION, F_URI, F_MODEL, F_COUNT };

/* How long a search waits for DNS-SD's answers. */
static constexpr int SEARCH_MS = 4000;

typedef struct found {
    char *name, *location, *uri, *model;
} found;

static void found_free(found *f) {
    g_free(f->name);
    g_free(f->location);
    g_free(f->uri);
    g_free(f->model);
    g_free(f);
}

/* Shared with the search thread, which may outlive the wizard. */
typedef struct search {
    gint refs;
    int cancel; /* cupsEnumDests stops when this is set */
} search;

typedef struct add_wizard {
    xd_wizard *w;
    GtkWidget *found_radio, *uri_radio, *list, *uri, *search_status, *search_again;
    GtkWidget *name, *location, *comment, *default_yes, *test_yes, *summary[6];
    GtkListStore *store;
    search *running;
    GCancellable *cancel;
    char *last_suggested;
} add_wizard;

static search *search_ref(search *s) {
    g_atomic_int_inc(&s->refs);
    return s;
}

static void search_unref(search *s) {
    if (g_atomic_int_dec_and_test(&s->refs))
        g_free(s);
}

/* ---- searching ----------------------------------------------------------------------- */

static int on_dest(void *data, unsigned flags, cups_dest_t *dest) {
    GPtrArray *list = data;
    const char *uri = cupsGetOption("device-uri", dest->num_options, dest->options);
    if ((flags & CUPS_DEST_FLAGS_REMOVED) != 0 || uri == nullptr)
        return 1;
    for (guint i = 0; i < list->len; i++) {
        if (strcmp(((found *)g_ptr_array_index(list, i))->uri, uri) == 0)
            return 1;
    }
    const char *info = cupsGetOption("printer-info", dest->num_options, dest->options);
    const char *loc = cupsGetOption("printer-location", dest->num_options, dest->options);
    const char *model = cupsGetOption("printer-make-and-model", dest->num_options, dest->options);
    found *f = g_new0(found, 1);
    f->name = g_strdup(info != nullptr && info[0] != '\0' ? info : dest->name);
    f->location = g_strdup(loc != nullptr ? loc : "");
    f->uri = g_strdup(uri);
    f->model = g_strdup(model != nullptr ? model : "");
    g_ptr_array_add(list, f);
    return 1;
}

static void search_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                          [[maybe_unused]] GCancellable *c) {
    search *s = data;
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)found_free);
    /* Only printers discovered on the network, not the queues CUPS
     * already has. */
    cupsEnumDests(CUPS_DEST_FLAGS_NONE, SEARCH_MS, &s->cancel, CUPS_PRINTER_DISCOVERED,
                  CUPS_PRINTER_DISCOVERED, on_dest, list);
    g_task_return_pointer(task, list, (GDestroyNotify)g_ptr_array_unref);
}

static void on_searched([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GPtrArray *list = g_task_propagate_pointer(G_TASK(res), nullptr);
    if (list == nullptr)
        return; /* the wizard is gone */
    add_wizard *a = data;
    search_unref(a->running);
    a->running = nullptr;
    gtk_list_store_clear(a->store);
    for (guint i = 0; i < list->len; i++) {
        const found *f = g_ptr_array_index(list, i);
        gtk_list_store_insert_with_values(a->store, nullptr, -1, F_NAME, f->name, F_LOCATION,
                                          f->location, F_URI, f->uri, F_MODEL, f->model, -1);
    }
    gtk_label_set_text(GTK_LABEL(a->search_status),
                       list->len > 0 ? "" : "No printers were found on the network.");
    gtk_widget_set_sensitive(a->search_again, TRUE);
    if (list->len == 0)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->uri_radio), TRUE);
    else {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->list));
        GtkTreePath *first = gtk_tree_path_new_first();
        gtk_tree_selection_select_path(sel, first);
        gtk_tree_path_free(first);
    }
    g_ptr_array_unref(list);
}

static void start_search(add_wizard *a) {
    if (a->running != nullptr)
        return;
    a->running = g_new0(search, 1);
    a->running->refs = 1;
    gtk_list_store_clear(a->store);
    gtk_label_set_text(GTK_LABEL(a->search_status), "Searching the network for printers...");
    gtk_widget_set_sensitive(a->search_again, FALSE);
    GTask *task = g_task_new(nullptr, a->cancel, on_searched, a);
    g_task_set_task_data(task, search_ref(a->running), (GDestroyNotify)search_unref);
    g_task_run_in_thread(task, search_thread);
    g_object_unref(task);
}

static void cb_search_again([[maybe_unused]] GtkWidget *w, gpointer data) {
    start_search(data);
}

/* ---- the pages ----------------------------------------------------------------------- */

static GtkWidget *text(const char *s) {
    GtkWidget *l = gtk_label_new(s);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
}

static GtkWidget *welcome_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_box_pack_start(GTK_BOX(box),
                       text("This wizard helps you install a printer or make printer "
                            "connections."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       text("If you have a printer that connects through a USB port, you "
                            "don't need this wizard: plug it in and it is installed for you "
                            "automatically."),
                       FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), text("To continue, click Next."), FALSE, FALSE, 0);
    return box;
}

static void on_choice_toggled([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    add_wizard *a = data;
    bool by_uri = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->uri_radio));
    gtk_widget_set_sensitive(a->list, !by_uri);
    gtk_widget_set_sensitive(a->uri, by_uri);
    if (by_uri)
        gtk_widget_grab_focus(a->uri);
}

static void on_found_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                               [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    add_wizard *a = data;
    xd_wizard_go(a->w, PAGE_NAME);
}

static GtkWidget *find_page(add_wizard *a) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    a->found_radio = gtk_radio_button_new_with_mnemonic(nullptr, "_Printers found on the network:");
    gtk_box_pack_start(GTK_BOX(box), a->found_radio, FALSE, FALSE, 0);

    a->store = gtk_list_store_new(F_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING);
    a->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));
    static const char *const titles[] = {"Name", "Location", "Model"};
    static const int cols[] = {F_NAME, F_LOCATION, F_MODEL};
    static const int widths[] = {150, 100, 140};
    for (int i = 0; i < 3; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(titles[i], r, "text",
                                                                        cols[i], nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(c, widths[i]);
        gtk_tree_view_append_column(GTK_TREE_VIEW(a->list), c);
    }
    g_signal_connect(a->list, "row-activated", G_CALLBACK(on_found_activated), a);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_container_add(GTK_CONTAINER(scroll), a->list);
    gtk_widget_set_margin_start(scroll, 20);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 20);
    a->search_status = text("");
    gtk_box_pack_start(GTK_BOX(row), a->search_status, TRUE, TRUE, 0);
    a->search_again = gtk_button_new_with_mnemonic("_Search Again");
    g_signal_connect(a->search_again, "clicked", G_CALLBACK(cb_search_again), a);
    gtk_box_pack_end(GTK_BOX(row), a->search_again, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    a->uri_radio = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(a->found_radio), "Connect to a printer by its _address (URL):");
    gtk_widget_set_margin_top(a->uri_radio, 6);
    gtk_box_pack_start(GTK_BOX(box), a->uri_radio, FALSE, FALSE, 0);
    a->uri = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(a->uri), "ipp://printer.local/ipp/print");
    gtk_entry_set_activates_default(GTK_ENTRY(a->uri), TRUE);
    gtk_widget_set_margin_start(a->uri, 20);
    gtk_box_pack_start(GTK_BOX(box), a->uri, FALSE, FALSE, 0);
    GtkWidget *eg = text("Example: ipp://192.168.1.20/ipp/print or ipps://printer.local/ipp/print");
    gtk_widget_set_margin_start(eg, 20);
    gtk_style_context_add_class(gtk_widget_get_style_context(eg), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), eg, FALSE, FALSE, 0);
    g_signal_connect(a->uri_radio, "toggled", G_CALLBACK(on_choice_toggled), a);
    gtk_widget_set_sensitive(a->uri, FALSE);
    return box;
}

static GtkWidget *grid_entry(GtkWidget *grid, int row, const char *title, GtkWidget **out) {
    GtkWidget *l = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    GtkWidget *e = gtk_entry_new();
    gtk_widget_set_hexpand(e, TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), e);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), e, 1, row, 1, 1);
    *out = e;
    return e;
}

/* "Do you want ...?" with Yes and No, Yes chosen. */
static GtkWidget *yes_no(GtkWidget *box, const char *question, GtkWidget **yes) {
    gtk_box_pack_start(GTK_BOX(box), text(question), FALSE, FALSE, 0);
    *yes = gtk_radio_button_new_with_mnemonic(nullptr, "_Yes");
    GtkWidget *no = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(*yes), "N_o");
    gtk_widget_set_margin_start(*yes, 20);
    gtk_widget_set_margin_start(no, 20);
    gtk_box_pack_start(GTK_BOX(box), *yes, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), no, FALSE, FALSE, 0);
    return no;
}

static GtkWidget *name_page(add_wizard *a) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box),
                       text("Type a name for this printer. Programs use it to find the "
                            "printer; spaces and the characters / \\ # ? ' \" can't be used."),
                       FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    grid_entry(grid, 0, "Printer _name:", &a->name);
    grid_entry(grid, 1, "_Location:", &a->location);
    grid_entry(grid, 2, "_Comment:", &a->comment);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 4);
    yes_no(box, "Do you want to use this printer as the default printer?", &a->default_yes);
    return box;
}

static GtkWidget *test_page(add_wizard *a) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box),
                       text("To confirm that the printer is installed properly, you can print "
                            "a test page."),
                       FALSE, FALSE, 0);
    yes_no(box, "Do you want to print a test page?", &a->test_yes);
    return box;
}

static GtkWidget *done_page(add_wizard *a) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_pack_start(GTK_BOX(box), text("You have successfully completed the Add Printer "
                                          "Wizard. You specified the following printer "
                                          "settings:"),
                       FALSE, FALSE, 0);
    static const char *const titles[] = {"Name:",    "Default:", "Test page:",
                                         "Location:", "Comment:", "Address:"};
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    for (int i = 0; i < (int)G_N_ELEMENTS(titles); i++) {
        gtk_grid_attach(GTK_GRID(grid), text(titles[i]), 0, i, 1, 1);
        a->summary[i] = text("");
        gtk_label_set_selectable(GTK_LABEL(a->summary[i]), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(a->summary[i]), PANGO_ELLIPSIZE_END);
        gtk_grid_attach(GTK_GRID(grid), a->summary[i], 1, i, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), text("To close this wizard, click Finish."), FALSE, FALSE, 0);
    return box;
}

/* ---- going through it ----------------------------------------------------------------- */

/* The chosen printer's address and a name for it; g_free both. */
static bool chosen(add_wizard *a, char **uri, char **model) {
    *uri = nullptr;
    *model = nullptr;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->uri_radio))) {
        char *u = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(a->uri))));
        if (u[0] == '\0') {
            g_free(u);
            return false;
        }
        *uri = u;
        /* Named after its host until it says better. */
        char scheme[32], user[64], host[256], resource[256];
        int port = 0;
        if (httpSeparateURI(HTTP_URI_CODING_ALL, u, scheme, sizeof(scheme), user, sizeof(user),
                            host, sizeof(host), &port, resource, sizeof(resource)) >=
            HTTP_URI_STATUS_OK)
            *model = g_strdup(host);
        return true;
    }
    GtkTreeModel *m = nullptr;
    GtkTreeIter iter;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->list));
    if (!gtk_tree_selection_get_selected(sel, &m, &iter))
        return false;
    gtk_tree_model_get(m, &iter, F_URI, uri, F_NAME, model, -1);
    return true;
}

/* Whether the address is one libcups can use for an IPP Everywhere
 * printer. */
static bool uri_usable(const char *uri) {
    char scheme[32], user[64], host[256], resource[256];
    int port = 0;
    http_uri_status_t s = httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), user,
                                          sizeof(user), host, sizeof(host), &port, resource,
                                          sizeof(resource));
    if (s < HTTP_URI_STATUS_OK || host[0] == '\0')
        return false;
    static const char *const ok[] = {"ipp", "ipps", "http", "https", "dnssd"};
    for (size_t i = 0; i < G_N_ELEMENTS(ok); i++) {
        if (strcmp(scheme, ok[i]) == 0)
            return true;
    }
    return false;
}

static void error_box(add_wizard *a, const char *msg) {
    xd_message_box(GTK_WINDOW(xd_wizard_window(a->w)), "Add Printer Wizard", XD_MB_ERROR,
                   XD_MB_OK, FALSE, msg);
}

static int next_page([[maybe_unused]] xd_wizard *w, int page, gpointer data) {
    add_wizard *a = data;
    if (page == PAGE_FIND) {
        char *uri = nullptr, *model = nullptr;
        if (!chosen(a, &uri, &model)) {
            error_box(a, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->uri_radio))
                             ? "Type the printer's address."
                             : "Select a printer, or type its address.");
            return -1;
        }
        bool ok = uri_usable(uri);
        if (!ok)
            error_box(a, "The address is not valid. It looks like "
                         "ipp://printer.local/ipp/print: ipp:// or ipps://, the printer's host "
                         "name or IP address, and its path.");
        /* Suggest a name, unless the user typed one of their own. */
        const char *cur = gtk_entry_get_text(GTK_ENTRY(a->name));
        if (ok && (cur[0] == '\0' || g_strcmp0(cur, a->last_suggested) == 0)) {
            g_free(a->last_suggested);
            a->last_suggested = xd_printer_name_suggest(model);
            gtk_entry_set_text(GTK_ENTRY(a->name), a->last_suggested);
        }
        g_free(uri);
        g_free(model);
        return ok ? PAGE_NAME : -1;
    }
    if (page == PAGE_NAME) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(a->name));
        if (!xd_printer_name_valid(name)) {
            error_box(a, "The printer name is not valid. It can't be empty, longer than 127 "
                         "characters, or contain spaces or the characters / \\ # ? ' \".");
            return -1;
        }
        cups_dest_t *existing = cupsGetNamedDest(CUPS_HTTP_DEFAULT, name, nullptr);
        if (existing != nullptr) {
            cupsFreeDests(1, existing);
            error_box(a, "A printer with this name already exists. Type another name.");
            return -1;
        }
    }
    return page + 1;
}

static void enter_page([[maybe_unused]] xd_wizard *w, int page, gpointer data) {
    add_wizard *a = data;
    if (page == PAGE_FIND && gtk_tree_model_iter_n_children(GTK_TREE_MODEL(a->store), nullptr) ==
                                 0)
        start_search(a);
    if (page == PAGE_NAME)
        gtk_widget_grab_focus(a->name);
    if (page == PAGE_DONE) {
        char *uri = nullptr, *model = nullptr;
        chosen(a, &uri, &model);
        bool def = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->default_yes));
        bool test = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->test_yes));
        const char *loc = gtk_entry_get_text(GTK_ENTRY(a->location));
        const char *comment = gtk_entry_get_text(GTK_ENTRY(a->comment));
        const char *values[] = {gtk_entry_get_text(GTK_ENTRY(a->name)), def ? "Yes" : "No",
                                test ? "Yes" : "No", loc, comment, uri != nullptr ? uri : ""};
        for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
            gtk_label_set_text(GTK_LABEL(a->summary[i]), values[i]);
        g_free(uri);
        g_free(model);
    }
}

static bool finish(xd_wizard *w, gpointer data) {
    add_wizard *a = data;
    GtkWindow *win = GTK_WINDOW(xd_wizard_window(w));
    char *uri = nullptr, *model = nullptr;
    if (!chosen(a, &uri, &model))
        return false;
    char *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(a->name)));
    GdkWindow *gdk = gtk_widget_get_window(GTK_WIDGET(win));
    GdkCursor *busy = gdk_cursor_new_from_name(gdk_window_get_display(gdk), "wait");
    gdk_window_set_cursor(gdk, busy);
    xd_wizard_set_next_sensitive(w, false);
    /* The scheduler asks the printer about itself: a few seconds. */
    while (gtk_events_pending())
        gtk_main_iteration();
    xd_cups_auth_reset();
    GError *error = nullptr;
    bool ok = xd_printer_add(name, uri, gtk_entry_get_text(GTK_ENTRY(a->comment)),
                             gtk_entry_get_text(GTK_ENTRY(a->location)), &error);
    gdk_window_set_cursor(gdk, nullptr);
    g_object_unref(busy);
    xd_wizard_set_next_sensitive(w, true);
    if (!ok) {
        xd_printers_error(win, error);
        g_error_free(error);
    } else {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->default_yes)))
            xd_printer_do_set_default(win, name);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->test_yes)) &&
            !xd_printer_print_test_page(name, nullptr, &error)) {
            xd_printers_error(win, error);
            g_clear_error(&error);
        }
        xd_printers_changed();
    }
    g_free(name);
    g_free(uri);
    g_free(model);
    return ok;
}

static void add_wizard_free(gpointer data) {
    add_wizard *a = data;
    g_cancellable_cancel(a->cancel);
    g_object_unref(a->cancel);
    if (a->running != nullptr) {
        /* The thread finds out and stops; it frees its own reference. */
        g_atomic_int_set(&a->running->cancel, 1);
        search_unref(a->running);
    }
    g_object_unref(a->store);
    g_free(a->last_suggested);
    g_free(a);
}

void xd_add_printer_wizard_open(GtkWindow *parent) {
    add_wizard *a = g_new0(add_wizard, 1);
    a->cancel = g_cancellable_new();
    a->w = xd_wizard_new(parent, "Add Printer Wizard", "printer", a, add_wizard_free);
    xd_printers_hold(xd_wizard_window(a->w));
    xd_wizard_add_exterior(a->w, "Welcome to the Add Printer Wizard", welcome_page(), false);
    xd_wizard_add_interior(a->w, "Find Your Printer",
                           "Choose a printer the wizard found on the network, or type its "
                           "address.",
                           find_page(a), false);
    xd_wizard_add_interior(a->w, "Name Your Printer",
                           "You must assign a name to this printer.", name_page(a), false);
    xd_wizard_add_interior(a->w, "Print Test Page",
                           "To confirm that the printer is installed properly, you can print "
                           "a test page.",
                           test_page(a), false);
    xd_wizard_add_exterior(a->w, "Completing the Add Printer Wizard", done_page(a), true);
    xd_wizard_set_next_func(a->w, next_page);
    xd_wizard_set_enter_func(a->w, enter_page);
    xd_wizard_set_finish_func(a->w, finish);
    xd_wizard_show(a->w);
}
