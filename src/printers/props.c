/* SPDX-License-Identifier: MIT
 * A printer's Properties (General: location, comment, model, features,
 * Print Test Page; Sharing: printer-is-shared, and the scheduler's own
 * printer sharing; Ports: where it prints, its device URI, from the
 * other printers' ports, CUPS-Get-Devices' or typed) and Printing Preferences
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
    /* Properties only: the printer, all printers (whose ports the Ports
     * tab lists) and whether the scheduler shares printers at all. */
    xd_printer *printer;
    GPtrArray *all;
    int server_sharing;
    dest_info d;
    GError *error;
} fetched;

static void fetched_free(gpointer data) {
    fetched *f = data;
    xd_printer_free(f->printer);
    if (f->all != nullptr)
        g_ptr_array_unref(f->all);
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
    f->server_sharing = -1;
    if (o->printer) {
        f->printer = xd_printer_get(o->name, &f->error);
        if (f->printer != nullptr) {
            f->all = xd_printers_list(nullptr);
            f->server_sharing = xd_cups_server_sharing(o->name);
        }
    }
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
    /* The window holds the program now, if it opened. */
    xd_printers_hold_end();
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
    xd_printers_hold_begin();
    GTask *task = g_task_new(nullptr, nullptr, on_fetched, nullptr);
    g_task_set_task_data(task, o, fetch_free);
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}

/* ---- changes, made in a worker thread ---------------------------------------------- */

/* Called on the main thread once a change is done, if its dialog is
 * still open (xd_printers_run has shown what went wrong). */
typedef void (*changed_fn)(GtkWidget *dialog, bool ok, gpointer user);

typedef struct busy {
    GtkWidget *dialog; /* a weak pointer */
    changed_fn done;
    gpointer user;
} busy;

static void set_busy(GtkWidget *dialog, bool on) {
    g_object_set_data(G_OBJECT(dialog), "xd-busy", GINT_TO_POINTER(on));
    gtk_widget_set_sensitive(dialog, !on);
    GdkWindow *gdk = gtk_widget_get_window(dialog);
    if (gdk == nullptr)
        return;
    GdkCursor *wait = on ? gdk_cursor_new_from_name(gdk_window_get_display(gdk), "wait") : nullptr;
    gdk_window_set_cursor(gdk, wait);
    if (wait != nullptr)
        g_object_unref(wait);
}

static bool is_busy(GtkWidget *dialog) {
    return g_object_get_data(G_OBJECT(dialog), "xd-busy") != nullptr;
}

/* The dialog stays while a change is under way. */
static gboolean on_busy_delete(GtkWidget *dialog, [[maybe_unused]] GdkEvent *e,
                               [[maybe_unused]] gpointer data) {
    return is_busy(dialog);
}

static void busy_done(bool ok, gpointer data) {
    busy *b = data;
    if (b->dialog != nullptr) {
        g_object_remove_weak_pointer(G_OBJECT(b->dialog), (gpointer *)&b->dialog);
        set_busy(b->dialog, false);
        b->done(b->dialog, ok, b->user);
    }
    g_free(b);
}

/* xd_printers_run, with `dialog` greyed meanwhile; then done(dialog, ok,
 * user) if it's still open. */
static void run_change(GtkWidget *dialog, xd_printers_change_fn fn, gpointer data,
                       GDestroyNotify free, changed_fn done, gpointer user) {
    busy *b = g_new0(busy, 1);
    b->dialog = dialog;
    g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&b->dialog);
    b->done = done;
    b->user = user;
    set_busy(dialog, true);
    xd_printers_run(GTK_WINDOW(dialog), fn, data, free, busy_done, b);
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
    bool closing; /* OK was pressed: close once saved */
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

/* The options to save, for the worker: names and values (a nullptr
 * value removes the option). */
typedef struct prefs_saving {
    char *name;
    GPtrArray *names, *values;
} prefs_saving;

static void prefs_saving_free(gpointer data) {
    prefs_saving *s = data;
    g_free(s->name);
    g_ptr_array_unref(s->names);
    g_ptr_array_unref(s->values);
    g_free(s);
}

static bool prefs_save_now(gpointer data, GError **error) {
    const prefs_saving *s = data;
    return xd_printer_set_user_options(s->name, (const char *const *)s->names->pdata,
                                       (const char *const *)s->values->pdata, s->names->len,
                                       error);
}

static void prefs_saved(GtkWidget *dialog, bool ok, gpointer user) {
    prefs *p = user;
    if (!ok) {
        p->closing = false;
        return;
    }
    gtk_widget_set_sensitive(p->apply, FALSE);
    if (p->closing)
        gtk_widget_destroy(dialog);
}

/* Saves in a worker (lpoptions, through the scheduler); OK closes the
 * window once that's done. */
static void prefs_save(prefs *p) {
    prefs_saving *s = g_new0(prefs_saving, 1);
    s->name = g_strdup(p->name);
    s->names = g_ptr_array_new_with_free_func(g_free);
    s->values = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < p->choices->len; i++) {
        const choice *c = g_ptr_array_index(p->choices, i);
        const char *v = choice_value(c);
        g_ptr_array_add(s->names, g_strdup(c->option));
        /* The printer's default needs no entry: if the administrator
         * changes it, the user gets the new one. */
        g_ptr_array_add(s->values, strcmp(v, c->fallback) == 0 ? nullptr : g_strdup(v));
    }
    int copies = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->copies));
    g_ptr_array_add(s->names, g_strdup(CUPS_COPIES));
    g_ptr_array_add(s->values,
                    copies == p->copies_default ? nullptr : g_strdup_printf("%d", copies));
    run_change(p->window, prefs_save_now, s, prefs_saving_free, prefs_saved, p);
}

static void on_prefs_response(GtkDialog *d, int response, gpointer data) {
    prefs *p = data;
    if (is_busy(GTK_WIDGET(d)))
        return;
    if (response == GTK_RESPONSE_APPLY) {
        prefs_save(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply)) {
        p->closing = true;
        prefs_save(p);
        return;
    }
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
    g_signal_connect(p->window, "delete-event", G_CALLBACK(on_busy_delete), nullptr);
    g_signal_connect(p->window, "destroy", G_CALLBACK(on_prefs_destroy), p);
    gtk_widget_show_all(p->window);
    gtk_widget_set_sensitive(p->apply, FALSE);
}

void xd_printer_preferences_open(GtkWindow *parent, const char *name) {
    fetch_and_open(parent, name, false, prefs_fetched);
}

/* ==== Properties ======================================================================= */

/* The Ports tab's list: a port, its checked box (the one this printer
 * prints to), and the printers that print to it. */
enum { PORT_CHECKED, PORT_NAME, PORT_DESCRIPTION, PORT_PRINTERS, PORT_URI, PORT_COUNT };

typedef struct props {
    char *name;
    GtkWidget *window, *apply, *location, *comment;
    /* Ports */
    GtkListStore *ports;
    GtkWidget *port_list, *configure;
    char *saved_uri;
    /* Sharing */
    GtkWidget *share_intro, *sharing_off, *share_no, *share_yes, *share_name;
    int server_sharing; /* as xd_cups_server_sharing */
    bool saved_shared;
    bool closing; /* OK was pressed: close once saved */
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

static bool print_test_page(gpointer name, GError **error) {
    return xd_printer_print_test_page(name, nullptr, error);
}

static void test_page_sent(GtkWidget *dialog, bool ok, gpointer user) {
    const props *p = user;
    if (!ok)
        return;
    char *msg = g_strdup_printf(
        "A test page is now being sent to the printer.\n\nDepending on the speed of your "
        "printer, it may take a minute or two before the page is printed. The test page "
        "briefly demonstrates the printer's ability to print graphics and text.\n\nIf the "
        "test page does not print, open the printer's queue (%s) to see what went wrong.",
        p->name);
    xd_message_box(GTK_WINDOW(dialog), p->name, XD_MB_INFO, XD_MB_OK, FALSE, msg);
    g_free(msg);
}

static void cb_test_page([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    run_change(p->window, print_test_page, g_strdup(p->name), g_free, test_page_sent, p);
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

/* The top of the Sharing and Ports tabs: the printer and its name. */
static GtkWidget *tab_page(const xd_printer *pr) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_pack_start(GTK_BOX(top), gtk_image_new_from_icon_name("printer", GTK_ICON_SIZE_DND),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), label(pr->name), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    return page;
}

/* ---- Sharing ---------------------------------------------------------------------- */

/* With the scheduler not sharing printers, XP SP2's note and link take
 * the place of the introduction, and the choices are greyed. */
static void sharing_update(props *p) {
    bool off = p->server_sharing == 0;
    gtk_widget_set_visible(p->sharing_off, off);
    gtk_widget_set_visible(p->share_intro, !off);
    gtk_widget_set_sensitive(p->share_no, !off);
    gtk_widget_set_sensitive(p->share_yes, !off);
    gtk_widget_set_sensitive(p->share_name,
                             !off && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->share_yes)));
}

static void on_share_toggled([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    sharing_update(p);
    props_changed(nullptr, p);
}

static bool turn_on_sharing([[maybe_unused]] gpointer data, GError **error) {
    return xd_cups_set_server_sharing(true, error);
}

static void sharing_turned_on([[maybe_unused]] GtkWidget *dialog, bool ok, gpointer user) {
    props *p = user;
    if (!ok)
        return;
    p->server_sharing = 1;
    sharing_update(p);
    gtk_widget_grab_focus(p->share_yes);
}

static gboolean on_sharing_link([[maybe_unused]] GtkLabel *l, [[maybe_unused]] const char *uri,
                                gpointer data) {
    props *p = data;
    run_change(p->window, turn_on_sharing, nullptr, nullptr, sharing_turned_on, p);
    return TRUE;
}

static GtkWidget *sharing_page(props *p, const xd_printer *pr) {
    GtkWidget *page = tab_page(pr);
    p->share_intro = label("You can share this printer with other users on your network. To "
                           "enable sharing for this printer, click Share this printer.");
    gtk_box_pack_start(GTK_BOX(page), p->share_intro, FALSE, FALSE, 0);

    p->sharing_off = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(p->sharing_off),
                       label("As a security measure, the print spooler (CUPS) doesn't share "
                             "this computer's printers with the network: other computers "
                             "can't print to them."),
                       FALSE, FALSE, 0);
    GtkWidget *link = label(nullptr);
    gtk_label_set_markup(GTK_LABEL(link),
                         "<a href=\"share\">If you understand the security risks but want to "
                         "share printers, click here.</a>");
    gtk_label_set_track_visited_links(GTK_LABEL(link), FALSE);
    g_signal_connect(link, "activate-link", G_CALLBACK(on_sharing_link), p);
    gtk_box_pack_start(GTK_BOX(p->sharing_off), link, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), p->sharing_off, FALSE, FALSE, 0);

    GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(choices, 8);
    p->share_no = gtk_radio_button_new_with_mnemonic(nullptr, "Do _not share this printer");
    p->share_yes = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(p->share_no),
                                                                  "_Share this printer");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pr->shared ? p->share_yes : p->share_no),
                                 TRUE);
    p->saved_shared = pr->shared;
    gtk_box_pack_start(GTK_BOX(choices), p->share_no, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(choices), p->share_yes, FALSE, FALSE, 0);
    /* CUPS shares a printer under its own name: the share name is shown,
     * not edited. */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 24);
    p->share_name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(p->share_name), pr->name);
    gtk_editable_set_editable(GTK_EDITABLE(p->share_name), FALSE);
    grid_row(grid, 0, "Share na_me:", p->share_name);
    gtk_box_pack_start(GTK_BOX(choices), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), choices, FALSE, FALSE, 0);
    g_signal_connect(p->share_yes, "toggled", G_CALLBACK(on_share_toggled), p);
    return page;
}

/* ---- Ports ------------------------------------------------------------------------ */

static bool port_find(props *p, const char *uri, GtkTreeIter *it) {
    GtkTreeModel *m = GTK_TREE_MODEL(p->ports);
    for (bool ok = gtk_tree_model_get_iter_first(m, it); ok; ok = gtk_tree_model_iter_next(m, it)) {
        char *u = nullptr;
        gtk_tree_model_get(m, it, PORT_URI, &u, -1);
        bool same = g_strcmp0(u, uri) == 0;
        g_free(u);
        if (same)
            return true;
    }
    return false;
}

static void port_set(props *p, GtkTreeIter *it, const char *uri) {
    char *port = xd_printer_port_text(uri);
    gtk_list_store_set(p->ports, it, PORT_NAME, port, PORT_DESCRIPTION,
                       xd_printer_port_description(uri), PORT_URI, uri, -1);
    g_free(port);
}

static void port_add(props *p, const char *uri, const char *printers, GtkTreeIter *it) {
    gtk_list_store_insert_with_values(p->ports, it, -1, PORT_CHECKED, FALSE, PORT_PRINTERS,
                                      printers, -1);
    port_set(p, it, uri);
}

/* Checks `target`'s box and no other: CUPS prints to one port (XP's
 * printer pooling off). */
static void port_check(props *p, GtkTreeIter *target) {
    GtkTreeModel *m = GTK_TREE_MODEL(p->ports);
    GtkTreePath *want = gtk_tree_model_get_path(m, target);
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        GtkTreePath *path = gtk_tree_model_get_path(m, &it);
        gtk_list_store_set(p->ports, &it, PORT_CHECKED, gtk_tree_path_compare(path, want) == 0,
                           -1);
        gtk_tree_path_free(path);
    }
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->port_list)), want);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(p->port_list), want, nullptr, FALSE, 0, 0);
    gtk_tree_path_free(want);
}

/* The checked port's device URI, or nullptr. */
static char *port_checked(props *p) {
    GtkTreeModel *m = GTK_TREE_MODEL(p->ports);
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        gboolean on = FALSE;
        char *uri = nullptr;
        gtk_tree_model_get(m, &it, PORT_CHECKED, &on, PORT_URI, &uri, -1);
        if (on)
            return uri;
        g_free(uri);
    }
    return nullptr;
}

/* Makes `uri` the printer's port: its row, else a new one. */
static void port_choose(props *p, const char *uri) {
    GtkTreeIter it;
    if (!port_find(p, uri, &it))
        port_add(p, uri, "", &it);
    port_check(p, &it);
    props_changed(nullptr, p);
}

static void on_port_toggled([[maybe_unused]] GtkCellRendererToggle *r, char *path,
                            gpointer data) {
    props *p = data;
    GtkTreeIter it;
    gboolean on = FALSE;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(p->ports), &it, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(p->ports), &it, PORT_CHECKED, &on, -1);
    /* Unchecking the only port would leave the printer none. */
    if (on)
        return;
    port_check(p, &it);
    props_changed(nullptr, p);
}

/* Space checks the selected port, as on XP's list. */
static gboolean on_port_key([[maybe_unused]] GtkWidget *w, GdkEventKey *e, gpointer data) {
    props *p = data;
    if (e->keyval != GDK_KEY_space || (e->state & gtk_accelerator_get_default_mod_mask()) != 0)
        return FALSE;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->port_list));
    if (!gtk_tree_selection_get_selected(sel, nullptr, &it))
        return FALSE;
    gboolean on = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(p->ports), &it, PORT_CHECKED, &on, -1);
    if (!on) {
        port_check(p, &it);
        props_changed(nullptr, p);
    }
    return TRUE;
}

static void on_port_selected(GtkTreeSelection *sel, gpointer data) {
    props *p = data;
    gtk_widget_set_sensitive(p->configure, gtk_tree_selection_get_selected(sel, nullptr, nullptr));
}

static void port_error(GtkWidget *parent, const char *title) {
    xd_message_box(GTK_WINDOW(parent), title, XD_MB_ERROR, XD_MB_OK, FALSE,
                   "The device URI is not complete. Type where the printer is, as in "
                   "socket://192.168.1.5:9100, ipp://printer.local/ipp/print or "
                   "usb://Maker/Model.");
}

/* A dialog's "Device URI:" box. */
static GtkWidget *uri_entry(GtkWidget *grid, int row) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 44);
    grid_row(grid, row, "Device _URI:", e);
    return e;
}

static GtkWidget *small_dialog(props *p, const char *title, const char *ok) {
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), title);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(p->window));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(d), ok, GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    return d;
}

/* Configure Port...: the selected port's device URI, changed. A port no
 * other printer uses changes in place; else this printer gets a port of
 * its own. Either way it's the one checked. */
static void cb_configure_port([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->port_list));
    if (!gtk_tree_selection_get_selected(sel, nullptr, &it))
        return;
    char *uri = nullptr, *printers = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(p->ports), &it, PORT_URI, &uri, PORT_PRINTERS, &printers,
                       -1);
    GtkWidget *d = small_dialog(p, "Configure Port", "OK");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    GtkWidget *note = label("Where the port prints, as the print spooler (CUPS) names it.");
    gtk_grid_attach(GTK_GRID(grid), note, 0, 0, 2, 1);
    GtkWidget *e = uri_entry(grid, 1);
    gtk_entry_set_text(GTK_ENTRY(e), uri);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid);
    gtk_widget_show_all(d);
    while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(e));
        if (!xd_printer_device_uri_complete(text)) {
            port_error(d, "Configure Port");
            gtk_widget_grab_focus(e);
            continue;
        }
        GtkTreeIter same;
        if (strcmp(text, uri) == 0 || port_find(p, text, &same)) {
            port_choose(p, text);
        } else if (printers[0] == '\0' || strcmp(printers, p->name) == 0) {
            port_set(p, &it, text);
            port_check(p, &it);
            props_changed(nullptr, p);
        } else {
            port_choose(p, text);
        }
        break;
    }
    gtk_widget_destroy(d);
    g_free(uri);
    g_free(printers);
}

/* Add Port...: the ports CUPS-Get-Devices finds, or one typed. */
enum { FOUND_PORT, FOUND_DESCRIPTION, FOUND_URI, FOUND_COUNT };

/* How long CUPS's backends look for printers. */
static constexpr int SCAN_SECONDS = 10;

typedef struct add_port {
    props *p;
    GtkWidget *dialog, *list, *status, *uri;
    GtkListStore *store;
} add_port;

typedef struct scan {
    GtkWidget *dialog; /* a weak pointer */
    GPtrArray *found;
    GError *error;
} scan;

static void scan_free(gpointer data) {
    scan *s = data;
    if (s->dialog != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(s->dialog), (gpointer *)&s->dialog);
    if (s->found != nullptr)
        g_ptr_array_unref(s->found);
    g_clear_error(&s->error);
    g_free(s);
}

static void scan_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *c) {
    scan *s = data;
    xd_cups_auth_worker_begin();
    s->found = xd_print_devices_list(SCAN_SECONDS, &s->error);
    xd_cups_auth_worker_end();
    g_task_return_boolean(task, TRUE);
}

static void on_scanned([[maybe_unused]] GObject *src, GAsyncResult *res,
                       [[maybe_unused]] gpointer data) {
    const scan *s = g_task_get_task_data(G_TASK(res));
    if (s->dialog == nullptr)
        return;
    add_port *a = g_object_get_data(G_OBJECT(s->dialog), "xd-add-port");
    if (s->found == nullptr) {
        bool canceled = s->error != nullptr && s->error->domain == XD_CUPS_ERROR &&
                        s->error->code == XD_CUPS_ERROR_CANCELED;
        gtk_label_set_text(GTK_LABEL(a->status),
                           canceled || s->error == nullptr ? "Type the port's device URI."
                                                           : s->error->message);
        return;
    }
    for (guint i = 0; i < s->found->len; i++) {
        const xd_print_device *d = g_ptr_array_index(s->found, i);
        char *start = xd_printer_device_uri_start(d->uri);
        /* A kind of port with no address yet goes by its start. */
        char *port = xd_printer_device_uri_complete(d->uri) ? xd_printer_port_text(d->uri)
                                                            : g_strdup(start);
        const char *desc = d->model[0] != '\0' && strcmp(d->model, d->info) != 0 &&
                                   strstr(d->info, d->model) == nullptr
                               ? d->model
                               : d->info;
        gtk_list_store_insert_with_values(a->store, nullptr, -1, FOUND_PORT, port,
                                          FOUND_DESCRIPTION, desc, FOUND_URI, d->uri, -1);
        g_free(port);
        g_free(start);
    }
    gtk_label_set_text(GTK_LABEL(a->status),
                       s->found->len > 0 ? "" : "No ports were found. Type the port's device URI.");
}

static void on_found_selected(GtkTreeSelection *sel, gpointer data) {
    add_port *a = data;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, nullptr, &it))
        return;
    char *uri = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(a->store), &it, FOUND_URI, &uri, -1);
    char *start = xd_printer_device_uri_start(uri);
    gtk_entry_set_text(GTK_ENTRY(a->uri), start);
    gtk_editable_set_position(GTK_EDITABLE(a->uri), -1);
    g_free(start);
    g_free(uri);
}

static void on_found_activated([[maybe_unused]] GtkTreeView *v,
                               [[maybe_unused]] GtkTreePath *path,
                               [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    add_port *a = data;
    gtk_dialog_response(GTK_DIALOG(a->dialog), GTK_RESPONSE_OK);
}

static void on_add_port_response(GtkDialog *d, int response, gpointer data) {
    add_port *a = data;
    if (response == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(a->uri));
        if (!xd_printer_device_uri_complete(text)) {
            port_error(GTK_WIDGET(d), "Printer Ports");
            gtk_widget_grab_focus(a->uri);
            gtk_editable_set_position(GTK_EDITABLE(a->uri), -1);
            return;
        }
        port_choose(a->p, text);
    }
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void add_port_free(gpointer data) {
    add_port *a = data;
    g_object_unref(a->store);
    g_free(a);
}

static void cb_add_port([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    add_port *a = g_new0(add_port, 1);
    a->p = p;
    a->dialog = small_dialog(p, "Printer Ports", "New _Port");
    g_object_set_data_full(G_OBJECT(a->dialog), "xd-add-port", a, add_port_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *l = gtk_label_new_with_mnemonic("_Available ports:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);

    a->store = gtk_list_store_new(FOUND_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    a->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), a->list);
    static const char *const titles[] = {"Port", "Description"};
    static const int widths[] = {150, 230};
    for (int i = 0; i < 2; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *c =
            gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(c, widths[i]);
        gtk_tree_view_append_column(GTK_TREE_VIEW(a->list), c);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->list)), "changed",
                     G_CALLBACK(on_found_selected), a);
    g_signal_connect(a->list, "row-activated", G_CALLBACK(on_found_activated), a);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 150);
    gtk_container_add(GTK_CONTAINER(scroll), a->list);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    a->status = label("Searching for ports...");
    gtk_box_pack_start(GTK_BOX(box), a->status, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    a->uri = uri_entry(grid, 0);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(a->dialog))), box);
    g_signal_connect(a->dialog, "response", G_CALLBACK(on_add_port_response), a);
    gtk_widget_show_all(a->dialog);
    gtk_widget_grab_focus(a->uri);

    /* The backends take a while; meanwhile a URI can be typed. */
    scan *s = g_new0(scan, 1);
    s->dialog = a->dialog;
    g_object_add_weak_pointer(G_OBJECT(a->dialog), (gpointer *)&s->dialog);
    GTask *task = g_task_new(nullptr, nullptr, on_scanned, nullptr);
    g_task_set_task_data(task, s, scan_free);
    g_task_run_in_thread(task, scan_thread);
    g_object_unref(task);
}

static GtkWidget *ports_page(props *p, const xd_printer *pr, GPtrArray *all) {
    GtkWidget *page = tab_page(pr);
    GtkWidget *intro = gtk_label_new_with_mnemonic(
        "_Print to the following port(s). Documents will print to the first free checked port.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 52);
    gtk_box_pack_start(GTK_BOX(page), intro, FALSE, FALSE, 0);

    /* Every printer's port, with the printers that print to it: XP's list
     * of the computer's ports. */
    p->ports = gtk_list_store_new(PORT_COUNT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING);
    p->port_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->ports));
    gtk_label_set_mnemonic_widget(GTK_LABEL(intro), p->port_list);
    p->saved_uri = g_strdup(pr->device_uri);
    for (guint i = 0; all != nullptr && i < all->len; i++) {
        const xd_printer *o = g_ptr_array_index(all, i);
        if (o->device_uri[0] == '\0')
            continue;
        GtkTreeIter it;
        if (!port_find(p, o->device_uri, &it)) {
            port_add(p, o->device_uri, o->name, &it);
            continue;
        }
        char *names = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(p->ports), &it, PORT_PRINTERS, &names, -1);
        char *more = g_strdup_printf("%s, %s", names, o->name);
        gtk_list_store_set(p->ports, &it, PORT_PRINTERS, more, -1);
        g_free(more);
        g_free(names);
    }
    GtkTreeIter own;
    if (!port_find(p, pr->device_uri, &own))
        port_add(p, pr->device_uri, pr->name, &own);
    gtk_list_store_set(p->ports, &own, PORT_CHECKED, TRUE, -1);

    GtkCellRenderer *check = gtk_cell_renderer_toggle_new();
    g_signal_connect(check, "toggled", G_CALLBACK(on_port_toggled), p);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "Port");
    gtk_tree_view_column_pack_start(col, check, FALSE);
    gtk_tree_view_column_add_attribute(col, check, "active", PORT_CHECKED);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", PORT_NAME);
    static const char *const titles[] = {"Port", "Description", "Printer"};
    static const int columns[] = {PORT_NAME, PORT_DESCRIPTION, PORT_PRINTERS};
    /* The Printer column takes what's left: no sideways scrolling. */
    static const int widths[] = {125, 140, -1};
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            GtkCellRenderer *r = gtk_cell_renderer_text_new();
            g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            col = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", columns[i],
                                                           nullptr);
        }
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        if (widths[i] > 0)
            gtk_tree_view_column_set_fixed_width(col, widths[i]);
        else
            gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(p->port_list), col);
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->port_list));
    g_signal_connect(sel, "changed", G_CALLBACK(on_port_selected), p);
    g_signal_connect(p->port_list, "key-press-event", G_CALLBACK(on_port_key), p);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 160);
    gtk_container_add(GTK_CONTAINER(scroll), p->port_list);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    /* XP's Delete Port has no counterpart: CUPS's ports are its backends'
     * devices, not something to remove. */
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add = gtk_button_new_with_mnemonic("Add Por_t...");
    g_signal_connect(add, "clicked", G_CALLBACK(cb_add_port), p);
    p->configure = gtk_button_new_with_mnemonic("_Configure Port...");
    g_signal_connect(p->configure, "clicked", G_CALLBACK(cb_configure_port), p);
    gtk_box_pack_end(GTK_BOX(buttons), p->configure, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    port_check(p, &own);
    return page;
}

/* ---- saving ----------------------------------------------------------------------- */

/* What Apply sends, for the worker. */
typedef struct props_saving {
    char *name, *info, *location, *uri;
    int shared;
} props_saving;

static void props_saving_free(gpointer data) {
    props_saving *s = data;
    g_free(s->name);
    g_free(s->info);
    g_free(s->location);
    g_free(s->uri);
    g_free(s);
}

static bool props_save_now(gpointer data, GError **error) {
    const props_saving *s = data;
    xd_printer_changes c = {
        .info = s->info, .location = s->location, .device_uri = s->uri, .shared = s->shared};
    return xd_printer_modify(s->name, &c, error);
}

static void props_saved(GtkWidget *dialog, bool ok, gpointer user) {
    props *p = user;
    if (!ok) {
        p->closing = false;
        return;
    }
    g_free(p->saved_uri);
    p->saved_uri = port_checked(p);
    p->saved_shared = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->share_yes));
    gtk_widget_set_sensitive(p->apply, FALSE);
    if (p->closing)
        gtk_widget_destroy(dialog);
}

/* One CUPS-Add-Modify-Printer with what changed (the Comment and
 * Location always), in a worker; OK closes the window once it's done. */
static void props_save(props *p) {
    props_saving *s = g_new0(props_saving, 1);
    s->name = g_strdup(p->name);
    s->info = g_strdup(gtk_entry_get_text(GTK_ENTRY(p->comment)));
    s->location = g_strdup(gtk_entry_get_text(GTK_ENTRY(p->location)));
    s->uri = port_checked(p);
    if (s->uri != nullptr && g_strcmp0(s->uri, p->saved_uri) == 0)
        g_clear_pointer(&s->uri, g_free);
    bool shared = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->share_yes));
    s->shared = shared != p->saved_shared ? shared : -1;
    run_change(p->window, props_save_now, s, props_saving_free, props_saved, p);
}

static void on_props_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (is_busy(GTK_WIDGET(d)))
        return;
    if (response == GTK_RESPONSE_APPLY) {
        props_save(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply)) {
        p->closing = true;
        props_save(p);
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_props_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    g_object_unref(p->ports);
    g_free(p->saved_uri);
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
    p->server_sharing = f->server_sharing;
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

    /* XP's order: General, Sharing, Ports. */
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(p, pr, &f->d),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), sharing_page(p, pr),
                             gtk_label_new_with_mnemonic("S_haring"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), ports_page(p, pr, f->all),
                             gtk_label_new_with_mnemonic("P_orts"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p->window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p->window, GTK_NOTEBOOK(tabs));
    g_signal_connect(p->window, "response", G_CALLBACK(on_props_response), p);
    g_signal_connect(p->window, "delete-event", G_CALLBACK(on_busy_delete), nullptr);
    g_signal_connect(p->window, "destroy", G_CALLBACK(on_props_destroy), p);
    gtk_widget_show_all(p->window);
    sharing_update(p);
    gtk_widget_set_sensitive(p->apply, FALSE);
}

void xd_printer_properties_open(GtkWindow *parent, const char *name) {
    fetch_and_open(parent, name, true, props_fetched);
}
