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

static void found_free(found *found_printer) {
    g_free(found_printer->name);
    g_free(found_printer->location);
    g_free(found_printer->uri);
    g_free(found_printer->model);
    g_free(found_printer);
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

static search *search_ref(search *search_state) {
    g_atomic_int_inc(&search_state->refs);
    return search_state;
}

static void search_unref(search *search_state) {
    if (g_atomic_int_dec_and_test(&search_state->refs))
        g_free(search_state);
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
    found *found_printer = g_new0(found, 1);
    found_printer->name = g_strdup(info != nullptr && info[0] != '\0' ? info : dest->name);
    found_printer->location = g_strdup(loc != nullptr ? loc : "");
    found_printer->uri = g_strdup(uri);
    found_printer->model = g_strdup(model != nullptr ? model : "");
    g_ptr_array_add(list, found_printer);
    return 1;
}

static void search_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                          [[maybe_unused]] GCancellable *cancellable) {
    search *search_state = data;
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)found_free);
    /* Only printers discovered on the network, not the queues CUPS
     * already has. */
    cupsEnumDests(CUPS_DEST_FLAGS_NONE, SEARCH_MS, &search_state->cancel, CUPS_PRINTER_DISCOVERED,
                  CUPS_PRINTER_DISCOVERED, on_dest, list);
    g_task_return_pointer(task, list, (GDestroyNotify)g_ptr_array_unref);
}

static void on_searched([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    GPtrArray *list = g_task_propagate_pointer(G_TASK(res), nullptr);
    if (list == nullptr)
        return; /* the wizard is gone */
    add_wizard *printer_wizard = data;
    search_unref(printer_wizard->running);
    printer_wizard->running = nullptr;
    gtk_list_store_clear(printer_wizard->store);
    for (guint i = 0; i < list->len; i++) {
        const found *found_printer = g_ptr_array_index(list, i);
        gtk_list_store_insert_with_values(
            printer_wizard->store, nullptr, -1, F_NAME, found_printer->name, F_LOCATION,
            found_printer->location, F_URI, found_printer->uri, F_MODEL, found_printer->model, -1);
    }
    gtk_label_set_text(GTK_LABEL(printer_wizard->search_status),
                       list->len > 0 ? "" : "No printers were found on the network.");
    gtk_widget_set_sensitive(printer_wizard->search_again, TRUE);
    if (list->len == 0)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(printer_wizard->uri_radio), TRUE);
    else {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(printer_wizard->list));
        GtkTreePath *first = gtk_tree_path_new_first();
        gtk_tree_selection_select_path(sel, first);
        gtk_tree_path_free(first);
    }
    g_ptr_array_unref(list);
}

static void start_search(add_wizard *printer_wizard) {
    if (printer_wizard->running != nullptr)
        return;
    printer_wizard->running = g_new0(search, 1);
    printer_wizard->running->refs = 1;
    gtk_list_store_clear(printer_wizard->store);
    gtk_label_set_text(GTK_LABEL(printer_wizard->search_status),
                       "Searching the network for printers...");
    gtk_widget_set_sensitive(printer_wizard->search_again, FALSE);
    GTask *task = g_task_new(nullptr, printer_wizard->cancel, on_searched, printer_wizard);
    g_task_set_task_data(task, search_ref(printer_wizard->running), (GDestroyNotify)search_unref);
    g_task_run_in_thread(task, search_thread);
    g_object_unref(task);
}

static void cb_search_again([[maybe_unused]] GtkWidget *widget, gpointer data) {
    start_search(data);
}

/* ---- the pages ----------------------------------------------------------------------- */

static GtkWidget *text(const char *label_text) {
    GtkWidget *widget = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 56);
    return widget;
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

static void on_choice_toggled([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    add_wizard *printer_wizard = data;
    bool by_uri = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printer_wizard->uri_radio));
    gtk_widget_set_sensitive(printer_wizard->list, !by_uri);
    gtk_widget_set_sensitive(printer_wizard->uri, by_uri);
    if (by_uri)
        gtk_widget_grab_focus(printer_wizard->uri);
}

static void on_found_activated([[maybe_unused]] GtkTreeView *tree_view,
                               [[maybe_unused]] GtkTreePath *path,
                               [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    add_wizard *printer_wizard = data;
    xd_wizard_go(printer_wizard->w, PAGE_NAME);
}

static GtkWidget *find_page(add_wizard *printer_wizard) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    printer_wizard->found_radio =
        gtk_radio_button_new_with_mnemonic(nullptr, "_Printers found on the network:");
    gtk_box_pack_start(GTK_BOX(box), printer_wizard->found_radio, FALSE, FALSE, 0);

    printer_wizard->store =
        gtk_list_store_new(F_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    printer_wizard->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(printer_wizard->store));
    static const char *const titles[] = {"Name", "Location", "Model"};
    static const int cols[] = {F_NAME, F_LOCATION, F_MODEL};
    static const int widths[] = {150, 100, 140};
    for (int i = 0; i < 3; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *column =
            gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", cols[i], nullptr);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(column, widths[i]);
        gtk_tree_view_append_column(GTK_TREE_VIEW(printer_wizard->list), column);
    }
    g_signal_connect(printer_wizard->list, "row-activated", G_CALLBACK(on_found_activated),
                     printer_wizard);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_container_add(GTK_CONTAINER(scroll), printer_wizard->list);
    gtk_widget_set_margin_start(scroll, 20);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 20);
    printer_wizard->search_status = text("");
    gtk_box_pack_start(GTK_BOX(row), printer_wizard->search_status, TRUE, TRUE, 0);
    printer_wizard->search_again = gtk_button_new_with_mnemonic("_Search Again");
    g_signal_connect(printer_wizard->search_again, "clicked", G_CALLBACK(cb_search_again),
                     printer_wizard);
    gtk_box_pack_end(GTK_BOX(row), printer_wizard->search_again, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    printer_wizard->uri_radio = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(printer_wizard->found_radio),
        "Connect to a printer by its _address (URL):");
    gtk_widget_set_margin_top(printer_wizard->uri_radio, 6);
    gtk_box_pack_start(GTK_BOX(box), printer_wizard->uri_radio, FALSE, FALSE, 0);
    printer_wizard->uri = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(printer_wizard->uri), "ipp://printer.local/ipp/print");
    gtk_entry_set_activates_default(GTK_ENTRY(printer_wizard->uri), TRUE);
    gtk_widget_set_margin_start(printer_wizard->uri, 20);
    gtk_box_pack_start(GTK_BOX(box), printer_wizard->uri, FALSE, FALSE, 0);
    GtkWidget *eg = text("Example: ipp://192.168.1.20/ipp/print or ipps://printer.local/ipp/print");
    gtk_widget_set_margin_start(eg, 20);
    gtk_style_context_add_class(gtk_widget_get_style_context(eg), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), eg, FALSE, FALSE, 0);
    g_signal_connect(printer_wizard->uri_radio, "toggled", G_CALLBACK(on_choice_toggled),
                     printer_wizard);
    gtk_widget_set_sensitive(printer_wizard->uri, FALSE);
    return box;
}

static GtkWidget *grid_entry(GtkWidget *grid, int row, const char *title, GtkWidget **out) {
    GtkWidget *title_label = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    GtkWidget *entry_widget = gtk_entry_new();
    gtk_widget_set_hexpand(entry_widget, TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(entry_widget), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(title_label), entry_widget);
    gtk_grid_attach(GTK_GRID(grid), title_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_widget, 1, row, 1, 1);
    *out = entry_widget;
    return entry_widget;
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

static GtkWidget *name_page(add_wizard *printer_wizard) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box),
                       text("Type a name for this printer. Programs use it to find the "
                            "printer; spaces and the characters / \\ # ? ' \" can't be used."),
                       FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    grid_entry(grid, 0, "_Printer name:", &printer_wizard->name);
    grid_entry(grid, 1, "_Location:", &printer_wizard->location);
    grid_entry(grid, 2, "_Comment:", &printer_wizard->comment);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 4);
    yes_no(box, "Do you want to use this printer as the default printer?",
           &printer_wizard->default_yes);
    return box;
}

static GtkWidget *test_page(add_wizard *printer_wizard) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box),
                       text("To confirm that the printer is installed properly, you can print "
                            "a test page."),
                       FALSE, FALSE, 0);
    yes_no(box, "Do you want to print a test page?", &printer_wizard->test_yes);
    return box;
}

static GtkWidget *done_page(add_wizard *printer_wizard) {
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
        printer_wizard->summary[i] = text("");
        gtk_label_set_selectable(GTK_LABEL(printer_wizard->summary[i]), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(printer_wizard->summary[i]), PANGO_ELLIPSIZE_END);
        gtk_grid_attach(GTK_GRID(grid), printer_wizard->summary[i], 1, i, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), text("To close this wizard, click Finish."), FALSE, FALSE, 0);
    return box;
}

/* ---- going through it ----------------------------------------------------------------- */

/* The chosen printer's address and a name for it; g_free both. */
static bool chosen(add_wizard *printer_wizard, char **uri, char **model) {
    *uri = nullptr;
    *model = nullptr;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printer_wizard->uri_radio))) {
        char *typed_uri = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(printer_wizard->uri))));
        if (typed_uri[0] == '\0') {
            g_free(typed_uri);
            return false;
        }
        *uri = typed_uri;
        /* Named after its host until it says better. */
        char scheme[32], user[64], host[256], resource[256];
        int port = 0;
        if (httpSeparateURI(HTTP_URI_CODING_ALL, typed_uri, scheme, sizeof(scheme), user,
                            sizeof(user), host, sizeof(host), &port, resource,
                            sizeof(resource)) >= HTTP_URI_STATUS_OK)
            *model = g_strdup(host);
        return true;
    }
    GtkTreeModel *tree_model = nullptr;
    GtkTreeIter iter;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(printer_wizard->list));
    if (!gtk_tree_selection_get_selected(sel, &tree_model, &iter))
        return false;
    gtk_tree_model_get(tree_model, &iter, F_URI, uri, F_NAME, model, -1);
    return true;
}

/* Whether the address is one libcups can use for an IPP Everywhere
 * printer. */
static bool uri_usable(const char *uri) {
    char scheme[32], user[64], host[256], resource[256];
    int port = 0;
    http_uri_status_t http_uri_status =
        httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), user, sizeof(user), host,
                        sizeof(host), &port, resource, sizeof(resource));
    if (http_uri_status < HTTP_URI_STATUS_OK || host[0] == '\0')
        return false;
    static const char *const ok[] = {"ipp", "ipps", "http", "https", "dnssd"};
    for (size_t i = 0; i < G_N_ELEMENTS(ok); i++) {
        if (strcmp(scheme, ok[i]) == 0)
            return true;
    }
    return false;
}

static void error_box(add_wizard *printer_wizard, const char *msg) {
    xd_message_box(GTK_WINDOW(xd_wizard_window(printer_wizard->w)), "Add Printer Wizard",
                   XD_MB_ERROR, XD_MB_OK, FALSE, msg);
}

/* ---- waiting for the scheduler -------------------------------------------------------- */

/* While Next asks whether the name is taken and Finish adds the printer
 * (a slow or unreachable scheduler mustn't freeze the program): the
 * wizard greyed, Cancel too, under a busy cursor. */
static void set_busy(add_wizard *printer_wizard, bool busy) {
    GtkWidget *win = xd_wizard_window(printer_wizard->w);
    gtk_widget_set_sensitive(win, !busy);
    GdkWindow *gdk = gtk_widget_get_window(win);
    if (gdk == nullptr)
        return;
    GdkCursor *wait =
        busy ? gdk_cursor_new_from_name(gdk_window_get_display(gdk), "wait") : nullptr;
    gdk_window_set_cursor(gdk, wait);
    if (wait != nullptr)
        g_object_unref(wait);
}

/* The wizard's tasks share its cancellable, cancelled when it's
 * destroyed (the close button still works while greyed): their callbacks
 * then mustn't touch it. */
static bool wizard_gone(GAsyncResult *res) {
    return g_cancellable_is_cancelled(g_task_get_cancellable(G_TASK(res)));
}

static void name_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *cancellable) {
    g_task_return_boolean(task, xd_printer_exists(data));
}

static void on_name_checked([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    if (wizard_gone(res))
        return;
    add_wizard *printer_wizard = data;
    set_busy(printer_wizard, false);
    if (g_task_propagate_boolean(G_TASK(res), nullptr)) {
        error_box(printer_wizard, "A printer with this name already exists. Type another name.");
        gtk_widget_grab_focus(printer_wizard->name);
        return;
    }
    xd_wizard_go(printer_wizard->w, PAGE_TEST);
}

/* What Finish adds, for the worker. */
typedef struct adding {
    char *name, *uri, *info, *location;
    bool set_default, test_page;
    GError *error;      /* adding it */
    GError *afterwards; /* making it the default, its test page */
} adding;

static void adding_free(gpointer data) {
    adding *addition = data;
    g_free(addition->name);
    g_free(addition->uri);
    g_free(addition->info);
    g_free(addition->location);
    g_clear_error(&addition->error);
    g_clear_error(&addition->afterwards);
    g_free(addition);
}

static void add_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                       [[maybe_unused]] GCancellable *cancellable) {
    adding *addition = data;
    /* cupsd may want a printer administrator's password. */
    xd_cups_auth_worker_begin();
    /* The scheduler asks the printer about itself: a few seconds. */
    if (xd_printer_add(addition->name, addition->uri, addition->info, addition->location,
                       &addition->error)) {
        if (addition->set_default)
            xd_printer_set_default(addition->name, &addition->afterwards);
        if (addition->test_page && addition->afterwards == nullptr)
            xd_printer_print_test_page(addition->name, nullptr, &addition->afterwards);
    }
    xd_cups_auth_worker_end();
    g_task_return_boolean(task, TRUE);
}

static void on_added([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    adding *addition = g_task_get_task_data(G_TASK(res));
    add_wizard *printer_wizard = wizard_gone(res) ? nullptr : data;
    GtkWindow *win =
        printer_wizard != nullptr ? GTK_WINDOW(xd_wizard_window(printer_wizard->w)) : nullptr;
    if (printer_wizard != nullptr)
        set_busy(printer_wizard, false);
    if (addition->error != nullptr) {
        /* The wizard stays, to try again or change something. */
        xd_printers_error(win, addition->error);
        return;
    }
    xd_printers_changed();
    xd_printers_error(win, addition->afterwards);
    if (printer_wizard != nullptr)
        gtk_widget_destroy(GTK_WIDGET(win));
}

static int next_page([[maybe_unused]] xd_wizard *wizard, int page, gpointer data) {
    add_wizard *printer_wizard = data;
    if (page == PAGE_FIND) {
        char *uri = nullptr, *model = nullptr;
        if (!chosen(printer_wizard, &uri, &model)) {
            error_box(printer_wizard,
                      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printer_wizard->uri_radio))
                          ? "Type the printer's address."
                          : "Select a printer, or type its address.");
            return -1;
        }
        bool ok = uri_usable(uri);
        if (!ok)
            error_box(printer_wizard,
                      "The address is not valid. It looks like "
                      "ipp://printer.local/ipp/print: ipp:// or ipps://, the printer's host "
                      "name or IP address, and its path.");
        /* Suggest a name, unless the user typed one of their own. */
        const char *cur = gtk_entry_get_text(GTK_ENTRY(printer_wizard->name));
        if (ok && (cur[0] == '\0' || g_strcmp0(cur, printer_wizard->last_suggested) == 0)) {
            g_free(printer_wizard->last_suggested);
            printer_wizard->last_suggested = xd_printer_name_suggest(model);
            gtk_entry_set_text(GTK_ENTRY(printer_wizard->name), printer_wizard->last_suggested);
        }
        g_free(uri);
        g_free(model);
        return ok ? PAGE_NAME : -1;
    }
    if (page == PAGE_NAME) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(printer_wizard->name));
        if (!xd_printer_name_valid(name)) {
            error_box(printer_wizard,
                      "The printer name is not valid. It can't be empty, longer than 127 "
                      "characters, or contain spaces or the characters / \\ # ? ' \".");
            return -1;
        }
        /* Whether it's taken, from the scheduler: on_name_checked goes on. */
        set_busy(printer_wizard, true);
        GTask *task = g_task_new(nullptr, printer_wizard->cancel, on_name_checked, printer_wizard);
        g_task_set_task_data(task, g_strdup(name), g_free);
        g_task_run_in_thread(task, name_thread);
        g_object_unref(task);
        return -1;
    }
    return page + 1;
}

static void enter_page([[maybe_unused]] xd_wizard *wizard, int page, gpointer data) {
    add_wizard *printer_wizard = data;
    if (page == PAGE_FIND &&
        gtk_tree_model_iter_n_children(GTK_TREE_MODEL(printer_wizard->store), nullptr) == 0)
        start_search(printer_wizard);
    if (page == PAGE_NAME)
        gtk_widget_grab_focus(printer_wizard->name);
    if (page == PAGE_DONE) {
        char *uri = nullptr, *model = nullptr;
        chosen(printer_wizard, &uri, &model);
        bool def = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printer_wizard->default_yes));
        bool test = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printer_wizard->test_yes));
        const char *loc = gtk_entry_get_text(GTK_ENTRY(printer_wizard->location));
        const char *comment = gtk_entry_get_text(GTK_ENTRY(printer_wizard->comment));
        const char *values[] = {gtk_entry_get_text(GTK_ENTRY(printer_wizard->name)),
                                def ? "Yes" : "No",
                                test ? "Yes" : "No",
                                loc,
                                comment,
                                uri != nullptr ? uri : ""};
        for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
            gtk_label_set_text(GTK_LABEL(printer_wizard->summary[i]), values[i]);
        g_free(uri);
        g_free(model);
    }
}

static bool finish([[maybe_unused]] xd_wizard *wizard, gpointer data) {
    add_wizard *printer_wizard = data;
    char *uri = nullptr, *model = nullptr;
    if (!chosen(printer_wizard, &uri, &model))
        return false;
    g_free(model);
    adding *addition = g_new0(adding, 1);
    addition->name = g_strdup(gtk_entry_get_text(GTK_ENTRY(printer_wizard->name)));
    addition->uri = uri;
    addition->info = g_strdup(gtk_entry_get_text(GTK_ENTRY(printer_wizard->comment)));
    addition->location = g_strdup(gtk_entry_get_text(GTK_ENTRY(printer_wizard->location)));
    addition->set_default =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printer_wizard->default_yes));
    addition->test_page = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printer_wizard->test_yes));
    set_busy(printer_wizard, true);
    GTask *task = g_task_new(nullptr, printer_wizard->cancel, on_added, printer_wizard);
    g_task_set_task_data(task, addition, adding_free);
    g_task_run_in_thread(task, add_thread);
    g_object_unref(task);
    return false; /* on_added closes it */
}

static void add_wizard_free(gpointer data) {
    add_wizard *printer_wizard = data;
    g_cancellable_cancel(printer_wizard->cancel);
    g_object_unref(printer_wizard->cancel);
    if (printer_wizard->running != nullptr) {
        /* The thread finds out and stops; it frees its own reference. */
        g_atomic_int_set(&printer_wizard->running->cancel, 1);
        search_unref(printer_wizard->running);
    }
    g_object_unref(printer_wizard->store);
    g_free(printer_wizard->last_suggested);
    g_free(printer_wizard);
}

void xd_add_printer_wizard_open(GtkWindow *parent) {
    add_wizard *printer_wizard = g_new0(add_wizard, 1);
    printer_wizard->cancel = g_cancellable_new();
    printer_wizard->w =
        xd_wizard_new(parent, "Add Printer Wizard", "printer", printer_wizard, add_wizard_free);
    xd_printers_hold(xd_wizard_window(printer_wizard->w));
    xd_wizard_add_exterior(printer_wizard->w, "Welcome to the Add Printer Wizard", welcome_page(),
                           false);
    xd_wizard_add_interior(printer_wizard->w, "Find Your Printer",
                           "Choose a printer the wizard found on the network, or type its "
                           "address.",
                           find_page(printer_wizard), false);
    xd_wizard_add_interior(printer_wizard->w, "Name Your Printer",
                           "You must assign a name to this printer.", name_page(printer_wizard),
                           false);
    xd_wizard_add_interior(printer_wizard->w, "Print Test Page",
                           "To confirm that the printer is installed properly, you can print "
                           "a test page.",
                           test_page(printer_wizard), false);
    xd_wizard_add_exterior(printer_wizard->w, "Completing the Add Printer Wizard",
                           done_page(printer_wizard), true);
    xd_wizard_set_next_func(printer_wizard->w, next_page);
    xd_wizard_set_enter_func(printer_wizard->w, enter_page);
    xd_wizard_set_finish_func(printer_wizard->w, finish);
    xd_wizard_show(printer_wizard->w);
}
