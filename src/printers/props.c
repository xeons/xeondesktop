/* SPDX-License-Identifier: MIT
 * A printer's Properties (General: location, comment, model, features,
 * Print Test Page; Ports: where it prints) and Printing Preferences
 * (Layout, Paper/Quality). The choices come from what the printer
 * supports (cupsCopyDestInfo, cupsFindDestSupported); preferences are the
 * user's own, kept in ~/.cups/lpoptions as lpoptions(1) keeps them.
 */
#include <cups/cups.h>

#include "../gtkapp/gtkapp.h"
#include "auth.h"
#include "printers.h"
#include "printlib.h"

/* The printer as libcups's destination API sees it, with what it
 * supports. */
typedef struct dest_info {
    cups_dest_t *dest;
    cups_dinfo_t *dinfo;
} dest_info;

static bool dest_open(dest_info *d, const char *name) {
    d->dest = cupsGetNamedDest(CUPS_HTTP_DEFAULT, name, nullptr);
    d->dinfo = d->dest != nullptr ? cupsCopyDestInfo(CUPS_HTTP_DEFAULT, d->dest) : nullptr;
    return d->dest != nullptr;
}

static void dest_close(dest_info *d) {
    if (d->dinfo != nullptr)
        cupsFreeDestInfo(d->dinfo);
    if (d->dest != nullptr)
        cupsFreeDests(1, d->dest);
    d->dest = nullptr;
    d->dinfo = nullptr;
}

/* What Properties and Printing Preferences show, fetched in a worker
 * thread before they open: a scheduler that can't be reached holds
 * libcups for up to its 30-second connection timeout, and
 * cupsCopyDestInfo asks the printer itself. (Once that worked, the
 * scheduler answers the main thread's dest calls building the pages;
 * they mostly read what was fetched.) */
typedef struct fetched {
    xd_printer *printer; /* Properties only */
    dest_info d;
    GError *error;
} fetched;

static void fetched_free(gpointer data) {
    fetched *f = data;
    xd_printer_free(f->printer);
    dest_close(&f->d);
    g_clear_error(&f->error);
    g_free(f);
}

/* Opens the window from what was fetched; it may take f->d. `parent` is
 * nullptr if it closed meanwhile. */
typedef void (*fetched_fn)(GtkWindow *parent, const char *name, fetched *f);

typedef struct fetch {
    GtkWidget *parent; /* a weak pointer */
    char *name;
    bool printer;
    fetched_fn done;
} fetch;

static void fetch_free(gpointer data) {
    fetch *o = data;
    if (o->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(o->parent), (gpointer *)&o->parent);
    g_free(o->name);
    g_free(o);
}

static void fetch_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                         [[maybe_unused]] GCancellable *c) {
    const fetch *o = data;
    xd_cups_thread_init();
    fetched *f = g_new0(fetched, 1);
    if (o->printer)
        f->printer = xd_printer_get(o->name, &f->error);
    if (f->error == nullptr)
        dest_open(&f->d, o->name);
    g_task_return_pointer(task, f, fetched_free);
}

static void on_fetched([[maybe_unused]] GObject *src, GAsyncResult *res,
                       [[maybe_unused]] gpointer data) {
    const fetch *o = g_task_get_task_data(G_TASK(res));
    fetched *f = g_task_propagate_pointer(G_TASK(res), nullptr);
    o->done(o->parent != nullptr ? GTK_WINDOW(o->parent) : nullptr, o->name, f);
    fetched_free(f);
}

static void fetch_and_open(GtkWindow *parent, const char *name, bool printer, fetched_fn done) {
    fetch *o = g_new0(fetch, 1);
    o->name = g_strdup(name);
    o->printer = printer;
    o->done = done;
    if (parent != nullptr) {
        o->parent = GTK_WIDGET(parent);
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&o->parent);
    }
    GTask *task = g_task_new(nullptr, nullptr, on_fetched, nullptr);
    g_task_set_task_data(task, o, fetch_free);
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}

/* An IPP value as text: enums and integers as numbers (as lpoptions
 * keeps them), keywords as they are. */
static char *value_text(ipp_attribute_t *a, int i) {
    ipp_tag_t tag = ippGetValueTag(a);
    if (tag == IPP_TAG_INTEGER || tag == IPP_TAG_ENUM)
        return g_strdup_printf("%d", ippGetInteger(a, i));
    if (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME || tag == IPP_TAG_TEXT ||
        tag == IPP_TAG_URI || tag == IPP_TAG_MIMETYPE)
        return g_strdup(ippGetString(a, i, nullptr));
    char buf[256];
    ippAttributeString(a, buf, sizeof(buf));
    return g_strdup(buf);
}

/* A size's name: the driver's, else PWG's common one ("Letter", "A4"). */
static const char *media_label(const char *pwg_name, const char *localized) {
    if (localized != nullptr && strcmp(localized, pwg_name) != 0)
        return localized;
    pwg_media_t *pwg = pwgMediaForPWG(pwg_name);
    return pwg != nullptr && pwg->ppd != nullptr ? pwg->ppd : pwg_name;
}

/* The paper sizes, as PWG names and the driver's names for them: from
 * media-col-database (IPP printers), else media-supported (drivers). */
static void media_sizes(dest_info *d, GPtrArray *values, GPtrArray *labels) {
    if (d->dinfo == nullptr)
        return;
    int n = cupsGetDestMediaCount(CUPS_HTTP_DEFAULT, d->dest, d->dinfo, CUPS_MEDIA_FLAGS_DEFAULT);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (int i = 0; i < n; i++) {
        cups_size_t size;
        if (!cupsGetDestMediaByIndex(CUPS_HTTP_DEFAULT, d->dest, d->dinfo, i,
                                     CUPS_MEDIA_FLAGS_DEFAULT, &size) ||
            g_hash_table_contains(seen, size.media))
            continue;
        const char *l = cupsLocalizeDestMedia(CUPS_HTTP_DEFAULT, d->dest, d->dinfo,
                                              CUPS_MEDIA_FLAGS_DEFAULT, &size);
        g_hash_table_add(seen, g_strdup(size.media));
        g_ptr_array_add(values, g_strdup(size.media));
        g_ptr_array_add(labels, g_strdup(media_label(size.media, l)));
    }
    ipp_attribute_t *a = n > 0 ? nullptr
                               : cupsFindDestSupported(CUPS_HTTP_DEFAULT, d->dest, d->dinfo,
                                                       CUPS_MEDIA);
    for (int i = 0; a != nullptr && i < ippGetCount(a); i++) {
        const char *m = ippGetString(a, i, nullptr);
        if (m == nullptr || g_hash_table_contains(seen, m))
            continue;
        const char *l = cupsLocalizeDestValue(CUPS_HTTP_DEFAULT, d->dest, d->dinfo, CUPS_MEDIA, m);
        g_hash_table_add(seen, g_strdup(m));
        g_ptr_array_add(values, g_strdup(m));
        g_ptr_array_add(labels, g_strdup(media_label(m, l)));
    }
    g_hash_table_unref(seen);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 52);
    return l;
}

/* ==== Printing Preferences ============================================================ */

/* One option: radio buttons (XP's Layout tab) or a drop-down list. */
typedef struct choice {
    char *option;
    char *fallback;    /* the printer's default, which needs no lpoptions entry */
    GPtrArray *values; /* char* */
    GPtrArray *radios; /* GtkWidget*, or nullptr for the combo */
    GtkWidget *combo;
} choice;

static void choice_free(choice *c) {
    g_free(c->option);
    g_free(c->fallback);
    g_ptr_array_unref(c->values);
    if (c->radios != nullptr)
        g_ptr_array_unref(c->radios);
    g_free(c);
}

static const char *choice_value(const choice *c) {
    if (c->combo != nullptr) {
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(c->combo));
        return id != nullptr ? id : c->fallback;
    }
    for (guint i = 0; i < c->radios->len; i++) {
        if (gtk_toggle_button_get_active(g_ptr_array_index(c->radios, i)))
            return g_ptr_array_index(c->values, i);
    }
    return c->fallback;
}

typedef struct prefs {
    char *name;
    dest_info d;
    GtkWidget *window, *apply, *copies;
    int copies_default;
    GPtrArray *choices;
} prefs;

static void prefs_changed([[maybe_unused]] GtkWidget *w, gpointer data) {
    prefs *p = data;
    gtk_widget_set_sensitive(p->apply, TRUE);
}

/* The option's label for `value`: ours where XP had its own words, else
 * the driver's (cupsLocalizeDestValue), else the value. */
static char *value_label(prefs *p, const char *option, const char *value) {
    const char *ours = nullptr;
    long n = strtol(value, nullptr, 10);
    if (strcmp(option, "orientation-requested") == 0)
        ours = xd_print_orientation_label((int)n);
    else if (strcmp(option, "sides") == 0)
        ours = xd_print_sides_label(value);
    else if (strcmp(option, "print-color-mode") == 0)
        ours = xd_print_color_label(value);
    else if (strcmp(option, "print-quality") == 0)
        ours = xd_print_quality_label((int)n);
    if (ours != nullptr)
        return g_strdup(ours);
    const char *l = p->d.dinfo != nullptr
                        ? cupsLocalizeDestValue(CUPS_HTTP_DEFAULT, p->d.dest, p->d.dinfo, option,
                                                value)
                        : nullptr;
    /* Untranslated: the keyword, readable ("photographic-glossy" as
     * "Photographic glossy"). */
    if (l == nullptr || strcmp(l, value) == 0)
        return xd_print_keyword_label(value);
    return g_strdup(l);
}

/* The user's value: lpoptions' (in the destination's options), else the
 * printer's default. */
static char *current_value(prefs *p, const char *option, const char *fallback) {
    const char *v = cupsGetOption(option, p->d.dest->num_options, p->d.dest->options);
    return g_strdup(v != nullptr ? v : fallback);
}

static char *default_value(prefs *p, const char *option) {
    ipp_attribute_t *a = p->d.dinfo != nullptr
                             ? cupsFindDestDefault(CUPS_HTTP_DEFAULT, p->d.dest, p->d.dinfo, option)
                             : nullptr;
    return a != nullptr && ippGetCount(a) > 0 ? value_text(a, 0) : nullptr;
}

/* Adds an option's row: `values` (taken) with their labels. Nothing when
 * the printer offers no choice. */
static void add_choice(prefs *p, GtkWidget *grid, int *row, const char *title, const char *option,
                       GPtrArray *values, GPtrArray *labels, const char *fallback, bool radios) {
    if (values->len < 2) {
        g_ptr_array_unref(values);
        g_ptr_array_unref(labels);
        return;
    }
    choice *c = g_new0(choice, 1);
    c->option = g_strdup(option);
    c->fallback = g_strdup(fallback != nullptr ? fallback : g_ptr_array_index(values, 0));
    c->values = values;
    char *cur = current_value(p, option, c->fallback);
    GtkWidget *l = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_valign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, *row, 1, 1);
    if (radios) {
        c->radios = g_ptr_array_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *first = nullptr;
        for (guint i = 0; i < values->len; i++) {
            GtkWidget *r = gtk_radio_button_new_with_label_from_widget(
                first != nullptr ? GTK_RADIO_BUTTON(first) : nullptr,
                g_ptr_array_index(labels, i));
            if (first == nullptr) {
                first = r;
                gtk_label_set_mnemonic_widget(GTK_LABEL(l), r);
            }
            if (strcmp(g_ptr_array_index(values, i), cur) == 0)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r), TRUE);
            g_signal_connect(r, "toggled", G_CALLBACK(prefs_changed), p);
            g_ptr_array_add(c->radios, r);
            gtk_box_pack_start(GTK_BOX(box), r, FALSE, FALSE, 0);
        }
        gtk_grid_attach(GTK_GRID(grid), box, 1, *row, 1, 1);
    } else {
        c->combo = gtk_combo_box_text_new();
        for (guint i = 0; i < values->len; i++)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c->combo), g_ptr_array_index(values, i),
                                      g_ptr_array_index(labels, i));
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c->combo), cur))
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(c->combo), c->fallback);
        gtk_widget_set_hexpand(c->combo, TRUE);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), c->combo);
        g_signal_connect(c->combo, "changed", G_CALLBACK(prefs_changed), p);
        gtk_grid_attach(GTK_GRID(grid), c->combo, 1, *row, 1, 1);
    }
    g_free(cur);
    g_ptr_array_unref(labels);
    g_ptr_array_add(p->choices, c);
    (*row)++;
}

/* An option whose choices are its -supported values. */
static void add_supported(prefs *p, GtkWidget *grid, int *row, const char *title,
                          const char *option, bool radios) {
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    ipp_attribute_t *a = p->d.dinfo != nullptr
                             ? cupsFindDestSupported(CUPS_HTTP_DEFAULT, p->d.dest, p->d.dinfo,
                                                     option)
                             : nullptr;
    int n = a != nullptr ? ippGetCount(a) : 0;
    bool range = a != nullptr && ippGetValueTag(a) == IPP_TAG_RANGE;
    if (range && strcmp(option, "number-up") == 0) {
        /* A range: XP's usual choices within it. */
        int upper = 0;
        int lower = ippGetRange(a, 0, &upper);
        static const int ups[] = {1, 2, 4, 6, 9, 16};
        for (size_t i = 0; i < G_N_ELEMENTS(ups); i++) {
            if (ups[i] >= lower && ups[i] <= upper) {
                g_ptr_array_add(values, g_strdup_printf("%d", ups[i]));
                g_ptr_array_add(labels, g_strdup_printf("%d", ups[i]));
            }
        }
    } else if (!range) {
        for (int i = 0; i < n; i++) {
            char *v = value_text(a, i);
            /* Orientations XP didn't offer ("none" lets the application
             * decide). */
            if (strcmp(option, "orientation-requested") == 0 &&
                xd_print_orientation_label(atoi(v)) == nullptr) {
                g_free(v);
                continue;
            }
            g_ptr_array_add(labels, value_label(p, option, v));
            g_ptr_array_add(values, v);
        }
    }
    char *fallback = default_value(p, option);
    add_choice(p, grid, row, title, option, values, labels, fallback, radios);
    g_free(fallback);
}

/* Paper sizes, by their PWG names, named as the driver names them. */
static void add_media(prefs *p, GtkWidget *grid, int *row) {
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    char *fallback = nullptr;
    if (p->d.dinfo != nullptr) {
        media_sizes(&p->d, values, labels);
        cups_size_t def;
        if (cupsGetDestMediaDefault(CUPS_HTTP_DEFAULT, p->d.dest, p->d.dinfo,
                                    CUPS_MEDIA_FLAGS_DEFAULT, &def))
            fallback = g_strdup(def.media);
        else
            fallback = default_value(p, CUPS_MEDIA);
    }
    add_choice(p, grid, row, "Paper _Size:", CUPS_MEDIA, values, labels, fallback, false);
    g_free(fallback);
    /* lpoptions may name the size another way ("A4", "Letter"): show the
     * size it means. */
    choice *c = p->choices->len > 0 ? g_ptr_array_index(p->choices, p->choices->len - 1) : nullptr;
    const char *user = cupsGetOption(CUPS_MEDIA, p->d.dest->num_options, p->d.dest->options);
    cups_size_t size;
    if (c != nullptr && strcmp(c->option, CUPS_MEDIA) == 0 && user != nullptr &&
        cupsGetDestMediaByName(CUPS_HTTP_DEFAULT, p->d.dest, p->d.dinfo, user,
                               CUPS_MEDIA_FLAGS_DEFAULT, &size))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(c->combo), size.media);
}

/* Page Order: CUPS's outputorder option, which every driver honours. */
static void add_page_order(prefs *p, GtkWidget *grid, int *row) {
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(values, g_strdup("normal"));
    g_ptr_array_add(labels, g_strdup("Front to Back"));
    g_ptr_array_add(values, g_strdup("reverse"));
    g_ptr_array_add(labels, g_strdup("Back to Front"));
    add_choice(p, grid, row, "Page _Order:", "outputorder", values, labels, "normal", true);
}

static GtkWidget *options_grid(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    return grid;
}

static GtkWidget *layout_page(prefs *p) {
    GtkWidget *grid = options_grid();
    int row = 0;
    add_supported(p, grid, &row, "Orientation:", "orientation-requested", true);
    add_supported(p, grid, &row, "Print on Both Sides:", "sides", true);
    add_page_order(p, grid, &row);
    add_supported(p, grid, &row, "Pages Per Sheet:", "number-up", false);

    /* Copies: a range from copies-supported. */
    ipp_attribute_t *a = p->d.dinfo != nullptr
                             ? cupsFindDestSupported(CUPS_HTTP_DEFAULT, p->d.dest, p->d.dinfo,
                                                     CUPS_COPIES)
                             : nullptr;
    int upper = 999;
    if (a != nullptr && ippGetValueTag(a) == IPP_TAG_RANGE)
        ippGetRange(a, 0, &upper);
    char *def = default_value(p, CUPS_COPIES);
    p->copies_default = def != nullptr ? atoi(def) : 1;
    g_free(def);
    char *cur = current_value(p, CUPS_COPIES, nullptr);
    int copies = cur != nullptr ? atoi(cur) : p->copies_default;
    g_free(cur);
    GtkWidget *l = gtk_label_new_with_mnemonic("_Copies:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    p->copies = gtk_spin_button_new_with_range(1, MAX(upper, 1), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->copies), MAX(copies, 1));
    gtk_widget_set_halign(p->copies, GTK_ALIGN_START);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), p->copies);
    g_signal_connect(p->copies, "value-changed", G_CALLBACK(prefs_changed), p);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->copies, 1, row, 1, 1);
    return grid;
}

static GtkWidget *paper_page(prefs *p) {
    GtkWidget *grid = options_grid();
    int row = 0;
    add_supported(p, grid, &row, "Paper Sou_rce:", "media-source", false);
    add_supported(p, grid, &row, "_Media:", "media-type", false);
    add_media(p, grid, &row);
    add_supported(p, grid, &row, "Color:", "print-color-mode", true);
    add_supported(p, grid, &row, "_Quality:", "print-quality", false);
    if (row == 0)
        gtk_grid_attach(GTK_GRID(grid), label("This printer has no paper or quality settings."),
                        0, 0, 2, 1);
    return grid;
}

static bool prefs_save(prefs *p) {
    guint n = p->choices->len + 1;
    const char **names = g_new0(const char *, n);
    const char **values = g_new0(const char *, n);
    for (guint i = 0; i < p->choices->len; i++) {
        const choice *c = g_ptr_array_index(p->choices, i);
        const char *v = choice_value(c);
        names[i] = c->option;
        /* The printer's default needs no entry: if the administrator
         * changes it, the user gets the new one. */
        values[i] = strcmp(v, c->fallback) == 0 ? nullptr : v;
    }
    int copies = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->copies));
    char buf[16];
    g_snprintf(buf, sizeof(buf), "%d", copies);
    names[n - 1] = CUPS_COPIES;
    values[n - 1] = copies == p->copies_default ? nullptr : buf;
    GError *error = nullptr;
    bool ok = xd_printer_set_user_options(p->name, names, values, n, &error);
    g_free(names);
    g_free(values);
    if (!ok) {
        xd_printers_error(GTK_WINDOW(p->window), error);
        g_error_free(error);
        return false;
    }
    gtk_widget_set_sensitive(p->apply, FALSE);
    return true;
}

static void on_prefs_response(GtkDialog *d, int response, gpointer data) {
    prefs *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        prefs_save(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !prefs_save(p))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_prefs_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    prefs *p = data;
    g_ptr_array_unref(p->choices);
    dest_close(&p->d);
    g_free(p->name);
    g_free(p);
}

static void prefs_fetched(GtkWindow *parent, const char *name, fetched *f) {
    if (f->d.dest == nullptr) {
        char *msg = g_strdup_printf("The printer %s no longer exists.", name);
        xd_message_box(parent, "Printing Preferences", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
        return;
    }
    prefs *p = g_new0(prefs, 1);
    p->d = f->d;
    f->d = (dest_info){0};
    p->name = g_strdup(name);
    p->choices = g_ptr_array_new_with_free_func((GDestroyNotify)choice_free);
    p->window = gtk_dialog_new();
    char *title = g_strdup_printf("%s Printing Preferences", name);
    gtk_window_set_title(GTK_WINDOW(p->window), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(p->window), "printer");
    gtk_window_set_resizable(GTK_WINDOW(p->window), FALSE);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(p->window), parent);
    xd_printers_hold(p->window);
    gtk_dialog_add_button(GTK_DIALOG(p->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p->window), "Cancel", GTK_RESPONSE_CANCEL);
    p->apply = gtk_dialog_add_button(GTK_DIALOG(p->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(p->window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), layout_page(p),
                             gtk_label_new_with_mnemonic("_Layout"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), paper_page(p),
                             gtk_label_new_with_mnemonic("_Paper/Quality"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p->window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p->window, GTK_NOTEBOOK(tabs));
    g_signal_connect(p->window, "response", G_CALLBACK(on_prefs_response), p);
    g_signal_connect(p->window, "destroy", G_CALLBACK(on_prefs_destroy), p);
    gtk_widget_show_all(p->window);
    gtk_widget_set_sensitive(p->apply, FALSE);
}

void xd_printer_preferences_open(GtkWindow *parent, const char *name) {
    fetch_and_open(parent, name, false, prefs_fetched);
}

/* ==== Properties ======================================================================= */

typedef struct props {
    char *name;
    GtkWidget *window, *apply, *location, *comment;
} props;

static void props_changed([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    gtk_widget_set_sensitive(p->apply, TRUE);
}

static const char *yes_no(bool b) { return b ? "Yes" : "No"; }

/* The highest resolution the printer offers, as "1200 dpi"; g_free it. */
static char *max_resolution(dest_info *d) {
    ipp_attribute_t *a = d->dinfo != nullptr
                             ? cupsFindDestSupported(CUPS_HTTP_DEFAULT, d->dest, d->dinfo,
                                                     "printer-resolution")
                             : nullptr;
    int best = 0;
    for (int i = 0; a != nullptr && i < ippGetCount(a); i++) {
        int y = 0;
        ipp_res_t units = IPP_RES_PER_INCH;
        int x = ippGetResolution(a, i, &y, &units);
        int dpi = units == IPP_RES_PER_CM ? (int)(x * 2.54) : x;
        best = MAX(best, dpi);
    }
    return best > 0 ? g_strdup_printf("%d dpi", best) : g_strdup("Unknown");
}

static void cb_preferences([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    xd_printer_preferences_open(GTK_WINDOW(p->window), p->name);
}

static void cb_test_page([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    xd_cups_auth_reset();
    GError *error = nullptr;
    if (!xd_printer_print_test_page(p->name, nullptr, &error)) {
        xd_printers_error(GTK_WINDOW(p->window), error);
        g_error_free(error);
        return;
    }
    xd_printers_changed();
    char *msg = g_strdup_printf(
        "A test page is now being sent to the printer.\n\nDepending on the speed of your "
        "printer, it may take a minute or two before the page is printed. The test page "
        "briefly demonstrates the printer's ability to print graphics and text.\n\nIf the "
        "test page does not print, open the printer's queue (%s) to see what went wrong.",
        p->name);
    xd_message_box(GTK_WINDOW(p->window), p->name, XD_MB_INFO, XD_MB_OK, FALSE, msg);
    g_free(msg);
}

static void grid_row(GtkWidget *grid, int row, const char *title, GtkWidget *value) {
    GtkWidget *l = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (GTK_IS_ENTRY(value))
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), value);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
}

static GtkWidget *entry(props *p, const char *text) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), text != nullptr ? text : "");
    g_signal_connect(e, "changed", G_CALLBACK(props_changed), p);
    return e;
}

static GtkWidget *general_page(props *p, const xd_printer *pr, dest_info *d) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    cairo_surface_t *s = xd_printer_icon(pr, 32, gtk_widget_get_scale_factor(p->window));
    gtk_box_pack_start(GTK_BOX(top), gtk_image_new_from_surface(s), FALSE, FALSE, 0);
    cairo_surface_destroy(s);
    /* CUPS can't rename a printer: the name is shown, not edited. */
    GtkWidget *name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name), pr->name);
    gtk_editable_set_editable(GTK_EDITABLE(name), FALSE);
    gtk_box_pack_start(GTK_BOX(top), name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    p->location = entry(p, pr->location);
    p->comment = entry(p, pr->info);
    grid_row(grid, 0, "_Location:", p->location);
    grid_row(grid, 1, "_Comment:", p->comment);
    grid_row(grid, 2, "Mo_del:", label(pr->model[0] != '\0' ? pr->model : "Unknown"));
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    /* Features: what the printer says it can do. */
    GtkWidget *feat = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(feat), 4);
    gtk_grid_set_column_spacing(GTK_GRID(feat), 24);
    bool color = d->dinfo != nullptr &&
                 cupsCheckDestSupported(CUPS_HTTP_DEFAULT, d->dest, d->dinfo, "print-color-mode",
                                        "color");
    bool duplex = d->dinfo != nullptr &&
                  cupsCheckDestSupported(CUPS_HTTP_DEFAULT, d->dest, d->dinfo, "sides",
                                         "two-sided-long-edge");
    bool staple = d->dinfo != nullptr &&
                  cupsCheckDestSupported(CUPS_HTTP_DEFAULT, d->dest, d->dinfo, CUPS_FINISHINGS,
                                         CUPS_FINISHINGS_STAPLE);
    char *res = max_resolution(d);
    char *texts[] = {
        g_strdup_printf("Color: %s", yes_no(color)),
        g_strdup_printf("Double-sided: %s", yes_no(duplex)),
        g_strdup_printf("Staple: %s", yes_no(staple)),
        g_strdup_printf("Maximum resolution: %s", res),
    };
    g_free(res);
    for (int i = 0; i < (int)G_N_ELEMENTS(texts); i++) {
        gtk_grid_attach(GTK_GRID(feat), label(texts[i]), 0, i, 1, 1);
        g_free(texts[i]);
    }
    /* Paper available. */
    GtkWidget *papers = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(papers), label("Paper available:"), FALSE, FALSE, 0);
    GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    media_sizes(d, values, labels);
    for (guint i = 0; i < labels->len; i++)
        gtk_list_store_insert_with_values(store, nullptr, -1, 0, g_ptr_array_index(labels, i), -1);
    g_ptr_array_unref(values);
    g_ptr_array_unref(labels);
    GtkWidget *list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(list), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list),
                                gtk_tree_view_column_new_with_attributes(
                                    "", gtk_cell_renderer_text_new(), "text", 0, nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 170, 80);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_box_pack_start(GTK_BOX(papers), scroll, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(feat), papers, 1, 0, 1, 5);
    gtk_box_pack_start(GTK_BOX(page), frame("Features", feat), FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *pref = gtk_button_new_with_mnemonic("Printing Pr_eferences...");
    g_signal_connect(pref, "clicked", G_CALLBACK(cb_preferences), p);
    GtkWidget *test = gtk_button_new_with_mnemonic("Print _Test Page");
    g_signal_connect(test, "clicked", G_CALLBACK(cb_test_page), p);
    gtk_box_pack_end(GTK_BOX(buttons), test, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), pref, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    return page;
}

static GtkWidget *ports_page(const xd_printer *pr) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_pack_start(GTK_BOX(top), gtk_image_new_from_icon_name("printer", GTK_ICON_SIZE_DND),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), label(pr->name), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), label("Documents print to this port:"), FALSE, FALSE, 0);

    GtkListStore *store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    char *port = xd_printer_port_text(pr->device_uri);
    gtk_list_store_insert_with_values(store, nullptr, -1, 0, port, 1,
                                      xd_printer_port_description(pr->device_uri), 2, pr->name,
                                      -1);
    g_free(port);
    GtkWidget *list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    static const char *const titles[] = {"Port", "Description", "Printer"};
    static const int widths[] = {120, 170, 120};
    for (int i = 0; i < 3; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i,
                                                                        nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(c, widths[i]);
        gtk_tree_view_append_column(GTK_TREE_VIEW(list), c);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 140);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    GtkWidget *uri = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(uri), pr->device_uri);
    gtk_editable_set_editable(GTK_EDITABLE(uri), FALSE);
    grid_row(grid, 0, "Device _URI:", uri);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       label("The port is set when the printer is added. To print somewhere "
                             "else, add the printer again with the Add Printer Wizard."),
                       FALSE, FALSE, 0);
    return page;
}

static bool props_save(props *p) {
    xd_cups_auth_reset();
    GError *error = nullptr;
    if (!xd_printer_modify(p->name, gtk_entry_get_text(GTK_ENTRY(p->comment)),
                           gtk_entry_get_text(GTK_ENTRY(p->location)), &error)) {
        xd_printers_error(GTK_WINDOW(p->window), error);
        g_error_free(error);
        return false;
    }
    gtk_widget_set_sensitive(p->apply, FALSE);
    xd_printers_changed();
    return true;
}

static void on_props_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        props_save(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !props_save(p))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_props_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    g_free(p->name);
    g_free(p);
}

static void props_fetched(GtkWindow *parent, const char *name, fetched *f) {
    const xd_printer *pr = f->printer;
    if (pr == nullptr) {
        xd_printers_error(parent, f->error);
        return;
    }
    props *p = g_new0(props, 1);
    p->name = g_strdup(name);
    p->window = gtk_dialog_new();
    char *title = g_strdup_printf("%s Properties", name);
    gtk_window_set_title(GTK_WINDOW(p->window), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(p->window), "printer");
    gtk_window_set_resizable(GTK_WINDOW(p->window), FALSE);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(p->window), parent);
    xd_printers_hold(p->window);
    gtk_dialog_add_button(GTK_DIALOG(p->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p->window), "Cancel", GTK_RESPONSE_CANCEL);
    p->apply = gtk_dialog_add_button(GTK_DIALOG(p->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(p->window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(p, pr, &f->d),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), ports_page(pr),
                             gtk_label_new_with_mnemonic("P_orts"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p->window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p->window, GTK_NOTEBOOK(tabs));
    g_signal_connect(p->window, "response", G_CALLBACK(on_props_response), p);
    g_signal_connect(p->window, "destroy", G_CALLBACK(on_props_destroy), p);
    gtk_widget_show_all(p->window);
    gtk_widget_set_sensitive(p->apply, FALSE);
}

void xd_printer_properties_open(GtkWindow *parent, const char *name) {
    fetch_and_open(parent, name, true, props_fetched);
}
