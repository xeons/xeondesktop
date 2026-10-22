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

static bool dest_open(dest_info *destination, const char *name) {
    destination->dest = cupsGetNamedDest(CUPS_HTTP_DEFAULT, name, nullptr);
    destination->dinfo = destination->dest != nullptr
                             ? cupsCopyDestInfo(CUPS_HTTP_DEFAULT, destination->dest)
                             : nullptr;
    return destination->dest != nullptr;
}

static void dest_close(dest_info *destination) {
    if (destination->dinfo != nullptr)
        cupsFreeDestInfo(destination->dinfo);
    if (destination->dest != nullptr)
        cupsFreeDests(1, destination->dest);
    destination->dest = nullptr;
    destination->dinfo = nullptr;
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
    fetched *fetched_info = data;
    xd_printer_free(fetched_info->printer);
    if (fetched_info->all != nullptr)
        g_ptr_array_unref(fetched_info->all);
    dest_close(&fetched_info->d);
    g_clear_error(&fetched_info->error);
    g_free(fetched_info);
}

/* Opens the window from what was fetched; it may take f->d. `parent` is
 * nullptr if it closed meanwhile. */
typedef void (*fetched_fn)(GtkWindow *parent, const char *name, fetched *fetched);

typedef struct fetch {
    GtkWidget *parent; /* a weak pointer */
    char *name;
    bool printer;
    fetched_fn done;
} fetch;

static void fetch_free(gpointer data) {
    fetch *fetch_request = data;
    if (fetch_request->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(fetch_request->parent),
                                     (gpointer *)&fetch_request->parent);
    g_free(fetch_request->name);
    g_free(fetch_request);
}

static void fetch_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                         [[maybe_unused]] GCancellable *cancellable) {
    const fetch *fetch_request = data;
    xd_cups_thread_init();
    fetched *fetched_info = g_new0(fetched, 1);
    fetched_info->server_sharing = -1;
    if (fetch_request->printer) {
        fetched_info->printer = xd_printer_get(fetch_request->name, &fetched_info->error);
        if (fetched_info->printer != nullptr) {
            fetched_info->all = xd_printers_list(nullptr);
            fetched_info->server_sharing = xd_cups_server_sharing(fetch_request->name);
        }
    }
    if (fetched_info->error == nullptr)
        dest_open(&fetched_info->d, fetch_request->name);
    g_task_return_pointer(task, fetched_info, fetched_free);
}

static void on_fetched([[maybe_unused]] GObject *src, GAsyncResult *res,
                       [[maybe_unused]] gpointer data) {
    const fetch *fetch_request = g_task_get_task_data(G_TASK(res));
    fetched *fetched_info = g_task_propagate_pointer(G_TASK(res), nullptr);
    fetch_request->done(fetch_request->parent != nullptr ? GTK_WINDOW(fetch_request->parent)
                                                         : nullptr,
                        fetch_request->name, fetched_info);
    fetched_free(fetched_info);
    /* The window holds the program now, if it opened. */
    xd_printers_hold_end();
}

static void fetch_and_open(GtkWindow *parent, const char *name, bool printer, fetched_fn done) {
    fetch *fetch_request = g_new0(fetch, 1);
    fetch_request->name = g_strdup(name);
    fetch_request->printer = printer;
    fetch_request->done = done;
    if (parent != nullptr) {
        fetch_request->parent = GTK_WIDGET(parent);
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&fetch_request->parent);
    }
    xd_printers_hold_begin();
    GTask *task = g_task_new(nullptr, nullptr, on_fetched, nullptr);
    g_task_set_task_data(task, fetch_request, fetch_free);
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
static gboolean on_busy_delete(GtkWidget *dialog, [[maybe_unused]] GdkEvent *gdk_event,
                               [[maybe_unused]] gpointer data) {
    return is_busy(dialog);
}

static void busy_done(bool ok, gpointer data) {
    busy *busy_dialog = data;
    if (busy_dialog->dialog != nullptr) {
        g_object_remove_weak_pointer(G_OBJECT(busy_dialog->dialog),
                                     (gpointer *)&busy_dialog->dialog);
        set_busy(busy_dialog->dialog, false);
        busy_dialog->done(busy_dialog->dialog, ok, busy_dialog->user);
    }
    g_free(busy_dialog);
}

/* xd_printers_run, with `dialog` greyed meanwhile; then done(dialog, ok,
 * user) if it's still open. */
static void run_change(GtkWidget *dialog, xd_printers_change_fn fn, gpointer data,
                       GDestroyNotify free, changed_fn done, gpointer user) {
    busy *busy_dialog = g_new0(busy, 1);
    busy_dialog->dialog = dialog;
    g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&busy_dialog->dialog);
    busy_dialog->done = done;
    busy_dialog->user = user;
    set_busy(dialog, true);
    xd_printers_run(GTK_WINDOW(dialog), fn, data, free, busy_done, busy_dialog);
}

/* An IPP value as text: enums and integers as numbers (as lpoptions
 * keeps them), keywords as they are. */
static char *value_text(ipp_attribute_t *attribute, int i) {
    ipp_tag_t tag = ippGetValueTag(attribute);
    if (tag == IPP_TAG_INTEGER || tag == IPP_TAG_ENUM)
        return g_strdup_printf("%d", ippGetInteger(attribute, i));
    if (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME || tag == IPP_TAG_TEXT ||
        tag == IPP_TAG_URI || tag == IPP_TAG_MIMETYPE)
        return g_strdup(ippGetString(attribute, i, nullptr));
    char buf[256];
    ippAttributeString(attribute, buf, sizeof(buf));
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
static void media_sizes(dest_info *destination, GPtrArray *values, GPtrArray *labels) {
    if (destination->dinfo == nullptr)
        return;
    int count = cupsGetDestMediaCount(CUPS_HTTP_DEFAULT, destination->dest, destination->dinfo,
                                      CUPS_MEDIA_FLAGS_DEFAULT);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (int i = 0; i < count; i++) {
        cups_size_t size;
        if (!cupsGetDestMediaByIndex(CUPS_HTTP_DEFAULT, destination->dest, destination->dinfo, i,
                                     CUPS_MEDIA_FLAGS_DEFAULT, &size) ||
            g_hash_table_contains(seen, size.media))
            continue;
        const char *localized =
            cupsLocalizeDestMedia(CUPS_HTTP_DEFAULT, destination->dest, destination->dinfo,
                                  CUPS_MEDIA_FLAGS_DEFAULT, &size);
        g_hash_table_add(seen, g_strdup(size.media));
        g_ptr_array_add(values, g_strdup(size.media));
        g_ptr_array_add(labels, g_strdup(media_label(size.media, localized)));
    }
    ipp_attribute_t *attribute = count > 0
                                     ? nullptr
                                     : cupsFindDestSupported(CUPS_HTTP_DEFAULT, destination->dest,
                                                             destination->dinfo, CUPS_MEDIA);
    for (int i = 0; attribute != nullptr && i < ippGetCount(attribute); i++) {
        const char *media = ippGetString(attribute, i, nullptr);
        if (media == nullptr || g_hash_table_contains(seen, media))
            continue;
        const char *localized = cupsLocalizeDestValue(CUPS_HTTP_DEFAULT, destination->dest,
                                                      destination->dinfo, CUPS_MEDIA, media);
        g_hash_table_add(seen, g_strdup(media));
        g_ptr_array_add(values, g_strdup(media));
        g_ptr_array_add(labels, g_strdup(media_label(media, localized)));
    }
    g_hash_table_unref(seen);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(widget), child);
    return widget;
}

static GtkWidget *label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 52);
    return widget;
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

static void choice_free(choice *option_choice) {
    g_free(option_choice->option);
    g_free(option_choice->fallback);
    g_ptr_array_unref(option_choice->values);
    if (option_choice->radios != nullptr)
        g_ptr_array_unref(option_choice->radios);
    g_free(option_choice);
}

static const char *choice_value(const choice *option_choice) {
    if (option_choice->combo != nullptr) {
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(option_choice->combo));
        return id != nullptr ? id : option_choice->fallback;
    }
    for (guint i = 0; i < option_choice->radios->len; i++) {
        if (gtk_toggle_button_get_active(g_ptr_array_index(option_choice->radios, i)))
            return g_ptr_array_index(option_choice->values, i);
    }
    return option_choice->fallback;
}

typedef struct prefs {
    char *name;
    dest_info d;
    GtkWidget *window, *apply, *copies;
    int copies_default;
    GPtrArray *choices;
    bool closing; /* OK was pressed: close once saved */
} prefs;

static void prefs_changed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    prefs *preferences = data;
    gtk_widget_set_sensitive(preferences->apply, TRUE);
}

/* The option's label for `value`: ours where XP had its own words, else
 * the driver's (cupsLocalizeDestValue), else the value. */
static char *value_label(prefs *preferences, const char *option, const char *value) {
    const char *ours = nullptr;
    long count = strtol(value, nullptr, 10);
    if (strcmp(option, "orientation-requested") == 0)
        ours = xd_print_orientation_label((int)count);
    else if (strcmp(option, "sides") == 0)
        ours = xd_print_sides_label(value);
    else if (strcmp(option, "print-color-mode") == 0)
        ours = xd_print_color_label(value);
    else if (strcmp(option, "print-quality") == 0)
        ours = xd_print_quality_label((int)count);
    if (ours != nullptr)
        return g_strdup(ours);
    const char *localized = preferences->d.dinfo != nullptr
                                ? cupsLocalizeDestValue(CUPS_HTTP_DEFAULT, preferences->d.dest,
                                                        preferences->d.dinfo, option, value)
                                : nullptr;
    /* Untranslated: the keyword, readable ("photographic-glossy" as
     * "Photographic glossy"). */
    if (localized == nullptr || strcmp(localized, value) == 0)
        return xd_print_keyword_label(value);
    return g_strdup(localized);
}

/* The user's value: lpoptions' (in the destination's options), else the
 * printer's default. */
static char *current_value(prefs *preferences, const char *option, const char *fallback) {
    const char *value =
        cupsGetOption(option, preferences->d.dest->num_options, preferences->d.dest->options);
    return g_strdup(value != nullptr ? value : fallback);
}

static char *default_value(prefs *preferences, const char *option) {
    ipp_attribute_t *attribute = preferences->d.dinfo != nullptr
                                     ? cupsFindDestDefault(CUPS_HTTP_DEFAULT, preferences->d.dest,
                                                           preferences->d.dinfo, option)
                                     : nullptr;
    return attribute != nullptr && ippGetCount(attribute) > 0 ? value_text(attribute, 0) : nullptr;
}

/* Adds an option's row: `values` (taken) with their labels. Nothing when
 * the printer offers no choice. */
static void add_choice(prefs *preferences, GtkWidget *grid, int *row, const char *title,
                       const char *option, GPtrArray *values, GPtrArray *labels,
                       const char *fallback, bool radios) {
    if (values->len < 2) {
        g_ptr_array_unref(values);
        g_ptr_array_unref(labels);
        return;
    }
    choice *option_choice = g_new0(choice, 1);
    option_choice->option = g_strdup(option);
    option_choice->fallback =
        g_strdup(fallback != nullptr ? fallback : g_ptr_array_index(values, 0));
    option_choice->values = values;
    char *cur = current_value(preferences, option, option_choice->fallback);
    GtkWidget *title_label = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0);
    gtk_widget_set_valign(title_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), title_label, 0, *row, 1, 1);
    if (radios) {
        option_choice->radios = g_ptr_array_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *first = nullptr;
        for (guint i = 0; i < values->len; i++) {
            GtkWidget *radio = gtk_radio_button_new_with_label_from_widget(
                first != nullptr ? GTK_RADIO_BUTTON(first) : nullptr, g_ptr_array_index(labels, i));
            if (first == nullptr) {
                first = radio;
                gtk_label_set_mnemonic_widget(GTK_LABEL(title_label), radio);
            }
            if (strcmp(g_ptr_array_index(values, i), cur) == 0)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);
            g_signal_connect(radio, "toggled", G_CALLBACK(prefs_changed), preferences);
            g_ptr_array_add(option_choice->radios, radio);
            gtk_box_pack_start(GTK_BOX(box), radio, FALSE, FALSE, 0);
        }
        gtk_grid_attach(GTK_GRID(grid), box, 1, *row, 1, 1);
    } else {
        option_choice->combo = gtk_combo_box_text_new();
        for (guint i = 0; i < values->len; i++)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(option_choice->combo),
                                      g_ptr_array_index(values, i), g_ptr_array_index(labels, i));
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(option_choice->combo), cur))
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(option_choice->combo),
                                        option_choice->fallback);
        gtk_widget_set_hexpand(option_choice->combo, TRUE);
        gtk_label_set_mnemonic_widget(GTK_LABEL(title_label), option_choice->combo);
        g_signal_connect(option_choice->combo, "changed", G_CALLBACK(prefs_changed), preferences);
        gtk_grid_attach(GTK_GRID(grid), option_choice->combo, 1, *row, 1, 1);
    }
    g_free(cur);
    g_ptr_array_unref(labels);
    g_ptr_array_add(preferences->choices, option_choice);
    (*row)++;
}

/* An option whose choices are its -supported values. */
static void add_supported(prefs *preferences, GtkWidget *grid, int *row, const char *title,
                          const char *option, bool radios) {
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    ipp_attribute_t *attribute = preferences->d.dinfo != nullptr
                                     ? cupsFindDestSupported(CUPS_HTTP_DEFAULT, preferences->d.dest,
                                                             preferences->d.dinfo, option)
                                     : nullptr;
    int count = attribute != nullptr ? ippGetCount(attribute) : 0;
    bool range = attribute != nullptr && ippGetValueTag(attribute) == IPP_TAG_RANGE;
    if (range && strcmp(option, "number-up") == 0) {
        /* A range: XP's usual choices within it. */
        int upper = 0;
        int lower = ippGetRange(attribute, 0, &upper);
        static const int ups[] = {1, 2, 4, 6, 9, 16};
        for (size_t i = 0; i < G_N_ELEMENTS(ups); i++) {
            if (ups[i] >= lower && ups[i] <= upper) {
                g_ptr_array_add(values, g_strdup_printf("%d", ups[i]));
                g_ptr_array_add(labels, g_strdup_printf("%d", ups[i]));
            }
        }
    } else if (!range) {
        for (int i = 0; i < count; i++) {
            char *value = value_text(attribute, i);
            /* Orientations XP didn't offer ("none" lets the application
             * decide). */
            if (strcmp(option, "orientation-requested") == 0 &&
                xd_print_orientation_label(atoi(value)) == nullptr) {
                g_free(value);
                continue;
            }
            g_ptr_array_add(labels, value_label(preferences, option, value));
            g_ptr_array_add(values, value);
        }
    }
    char *fallback = default_value(preferences, option);
    add_choice(preferences, grid, row, title, option, values, labels, fallback, radios);
    g_free(fallback);
}

/* Paper sizes, by their PWG names, named as the driver names them. */
static void add_media(prefs *preferences, GtkWidget *grid, int *row) {
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    char *fallback = nullptr;
    if (preferences->d.dinfo != nullptr) {
        media_sizes(&preferences->d, values, labels);
        cups_size_t def;
        if (cupsGetDestMediaDefault(CUPS_HTTP_DEFAULT, preferences->d.dest, preferences->d.dinfo,
                                    CUPS_MEDIA_FLAGS_DEFAULT, &def))
            fallback = g_strdup(def.media);
        else
            fallback = default_value(preferences, CUPS_MEDIA);
    }
    add_choice(preferences, grid, row, "Paper _Size:", CUPS_MEDIA, values, labels, fallback, false);
    g_free(fallback);
    /* lpoptions may name the size another way ("A4", "Letter"): show the
     * size it means. */
    choice *option_choice =
        preferences->choices->len > 0
            ? g_ptr_array_index(preferences->choices, preferences->choices->len - 1)
            : nullptr;
    const char *user =
        cupsGetOption(CUPS_MEDIA, preferences->d.dest->num_options, preferences->d.dest->options);
    cups_size_t size;
    if (option_choice != nullptr && strcmp(option_choice->option, CUPS_MEDIA) == 0 &&
        user != nullptr &&
        cupsGetDestMediaByName(CUPS_HTTP_DEFAULT, preferences->d.dest, preferences->d.dinfo, user,
                               CUPS_MEDIA_FLAGS_DEFAULT, &size))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(option_choice->combo), size.media);
}

/* Page Order: CUPS's outputorder option, which every driver honours. */
static void add_page_order(prefs *preferences, GtkWidget *grid, int *row) {
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(values, g_strdup("normal"));
    g_ptr_array_add(labels, g_strdup("Front to Back"));
    g_ptr_array_add(values, g_strdup("reverse"));
    g_ptr_array_add(labels, g_strdup("Back to Front"));
    add_choice(preferences, grid, row, "Page _Order:", "outputorder", values, labels, "normal",
               true);
}

static GtkWidget *options_grid(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    return grid;
}

static GtkWidget *layout_page(prefs *preferences) {
    GtkWidget *grid = options_grid();
    int row = 0;
    add_supported(preferences, grid, &row, "Orientation:", "orientation-requested", true);
    add_supported(preferences, grid, &row, "Print on Both Sides:", "sides", true);
    add_page_order(preferences, grid, &row);
    add_supported(preferences, grid, &row, "Pages Per Sheet:", "number-up", false);

    /* Copies: a range from copies-supported. */
    ipp_attribute_t *attribute = preferences->d.dinfo != nullptr
                                     ? cupsFindDestSupported(CUPS_HTTP_DEFAULT, preferences->d.dest,
                                                             preferences->d.dinfo, CUPS_COPIES)
                                     : nullptr;
    int upper = 999;
    if (attribute != nullptr && ippGetValueTag(attribute) == IPP_TAG_RANGE)
        ippGetRange(attribute, 0, &upper);
    char *def = default_value(preferences, CUPS_COPIES);
    preferences->copies_default = def != nullptr ? atoi(def) : 1;
    g_free(def);
    char *cur = current_value(preferences, CUPS_COPIES, nullptr);
    int copies = cur != nullptr ? atoi(cur) : preferences->copies_default;
    g_free(cur);
    GtkWidget *widget = gtk_label_new_with_mnemonic("_Copies:");
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    preferences->copies = gtk_spin_button_new_with_range(1, MAX(upper, 1), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(preferences->copies), MAX(copies, 1));
    gtk_widget_set_halign(preferences->copies, GTK_ALIGN_START);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), preferences->copies);
    g_signal_connect(preferences->copies, "value-changed", G_CALLBACK(prefs_changed), preferences);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), preferences->copies, 1, row, 1, 1);
    return grid;
}

static GtkWidget *paper_page(prefs *preferences) {
    GtkWidget *grid = options_grid();
    int row = 0;
    add_supported(preferences, grid, &row, "Paper Sou_rce:", "media-source", false);
    add_supported(preferences, grid, &row, "_Media:", "media-type", false);
    add_media(preferences, grid, &row);
    add_supported(preferences, grid, &row, "Color:", "print-color-mode", true);
    add_supported(preferences, grid, &row, "_Quality:", "print-quality", false);
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
    prefs_saving *saving = data;
    g_free(saving->name);
    g_ptr_array_unref(saving->names);
    g_ptr_array_unref(saving->values);
    g_free(saving);
}

static bool prefs_save_now(gpointer data, GError **error) {
    const prefs_saving *saving = data;
    return xd_printer_set_user_options(saving->name, (const char *const *)saving->names->pdata,
                                       (const char *const *)saving->values->pdata,
                                       saving->names->len, error);
}

static void prefs_saved(GtkWidget *dialog, bool ok, gpointer user) {
    prefs *preferences = user;
    if (!ok) {
        preferences->closing = false;
        return;
    }
    gtk_widget_set_sensitive(preferences->apply, FALSE);
    if (preferences->closing)
        gtk_widget_destroy(dialog);
}

/* Saves in a worker (lpoptions, through the scheduler); OK closes the
 * window once that's done. */
static void prefs_save(prefs *preferences) {
    prefs_saving *saving = g_new0(prefs_saving, 1);
    saving->name = g_strdup(preferences->name);
    saving->names = g_ptr_array_new_with_free_func(g_free);
    saving->values = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < preferences->choices->len; i++) {
        const choice *option_choice = g_ptr_array_index(preferences->choices, i);
        const char *value = choice_value(option_choice);
        g_ptr_array_add(saving->names, g_strdup(option_choice->option));
        /* The printer's default needs no entry: if the administrator
         * changes it, the user gets the new one. */
        g_ptr_array_add(saving->values,
                        strcmp(value, option_choice->fallback) == 0 ? nullptr : g_strdup(value));
    }
    int copies = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(preferences->copies));
    g_ptr_array_add(saving->names, g_strdup(CUPS_COPIES));
    g_ptr_array_add(saving->values, copies == preferences->copies_default
                                        ? nullptr
                                        : g_strdup_printf("%d", copies));
    run_change(preferences->window, prefs_save_now, saving, prefs_saving_free, prefs_saved,
               preferences);
}

static void on_prefs_response(GtkDialog *dialog_widget, int response, gpointer data) {
    prefs *preferences = data;
    if (is_busy(GTK_WIDGET(dialog_widget)))
        return;
    if (response == GTK_RESPONSE_APPLY) {
        prefs_save(preferences);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(preferences->apply)) {
        preferences->closing = true;
        prefs_save(preferences);
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

static void on_prefs_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    prefs *preferences = data;
    g_ptr_array_unref(preferences->choices);
    dest_close(&preferences->d);
    g_free(preferences->name);
    g_free(preferences);
}

static void prefs_fetched(GtkWindow *parent, const char *name, fetched *fetched_info) {
    if (fetched_info->d.dest == nullptr) {
        char *msg = g_strdup_printf("The printer %s no longer exists.", name);
        xd_message_box(parent, "Printing Preferences", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
        return;
    }
    prefs *preferences = g_new0(prefs, 1);
    preferences->d = fetched_info->d;
    fetched_info->d = (dest_info){0};
    preferences->name = g_strdup(name);
    preferences->choices = g_ptr_array_new_with_free_func((GDestroyNotify)choice_free);
    preferences->window = gtk_dialog_new();
    char *title = g_strdup_printf("%s Printing Preferences", name);
    gtk_window_set_title(GTK_WINDOW(preferences->window), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(preferences->window), "printer");
    gtk_window_set_resizable(GTK_WINDOW(preferences->window), FALSE);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(preferences->window), parent);
    xd_printers_hold(preferences->window);
    gtk_dialog_add_button(GTK_DIALOG(preferences->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(preferences->window), "Cancel", GTK_RESPONSE_CANCEL);
    preferences->apply =
        gtk_dialog_add_button(GTK_DIALOG(preferences->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(preferences->window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), layout_page(preferences),
                             gtk_label_new_with_mnemonic("_Layout"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), paper_page(preferences),
                             gtk_label_new_with_mnemonic("_Paper/Quality"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(preferences->window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(preferences->window, GTK_NOTEBOOK(tabs));
    g_signal_connect(preferences->window, "response", G_CALLBACK(on_prefs_response), preferences);
    g_signal_connect(preferences->window, "delete-event", G_CALLBACK(on_busy_delete), nullptr);
    g_signal_connect(preferences->window, "destroy", G_CALLBACK(on_prefs_destroy), preferences);
    gtk_widget_show_all(preferences->window);
    gtk_widget_set_sensitive(preferences->apply, FALSE);
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

static void props_changed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    gtk_widget_set_sensitive(properties->apply, TRUE);
}

static const char *yes_no(bool value) { return value ? "Yes" : "No"; }

/* The highest resolution the printer offers, as "1200 dpi"; g_free it. */
static char *max_resolution(dest_info *destination) {
    ipp_attribute_t *attribute =
        destination->dinfo != nullptr
            ? cupsFindDestSupported(CUPS_HTTP_DEFAULT, destination->dest, destination->dinfo,
                                    "printer-resolution")
            : nullptr;
    int best = 0;
    for (int i = 0; attribute != nullptr && i < ippGetCount(attribute); i++) {
        int y = 0;
        ipp_res_t units = IPP_RES_PER_INCH;
        int x = ippGetResolution(attribute, i, &y, &units);
        int dpi = units == IPP_RES_PER_CM ? (int)(x * 2.54) : x;
        best = MAX(best, dpi);
    }
    return best > 0 ? g_strdup_printf("%d dpi", best) : g_strdup("Unknown");
}

static void cb_preferences([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    xd_printer_preferences_open(GTK_WINDOW(properties->window), properties->name);
}

static bool print_test_page(gpointer name, GError **error) {
    return xd_printer_print_test_page(name, nullptr, error);
}

static void test_page_sent(GtkWidget *dialog, bool ok, gpointer user) {
    const props *properties = user;
    if (!ok)
        return;
    char *msg = g_strdup_printf(
        "A test page is now being sent to the printer.\n\nDepending on the speed of your "
        "printer, it may take a minute or two before the page is printed. The test page "
        "briefly demonstrates the printer's ability to print graphics and text.\n\nIf the "
        "test page does not print, open the printer's queue (%s) to see what went wrong.",
        properties->name);
    xd_message_box(GTK_WINDOW(dialog), properties->name, XD_MB_INFO, XD_MB_OK, FALSE, msg);
    g_free(msg);
}

static void cb_test_page([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    run_change(properties->window, print_test_page, g_strdup(properties->name), g_free,
               test_page_sent, properties);
}

static void grid_row(GtkWidget *grid, int row, const char *title, GtkWidget *value) {
    GtkWidget *widget = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (GTK_IS_ENTRY(value))
        gtk_label_set_mnemonic_widget(GTK_LABEL(widget), value);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
}

static GtkWidget *entry(props *properties, const char *text) {
    GtkWidget *widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), text != nullptr ? text : "");
    g_signal_connect(widget, "changed", G_CALLBACK(props_changed), properties);
    return widget;
}

static GtkWidget *general_page(props *properties, const xd_printer *pr, dest_info *destination) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    cairo_surface_t *surface =
        xd_printer_icon(pr, 32, gtk_widget_get_scale_factor(properties->window));
    gtk_box_pack_start(GTK_BOX(top), gtk_image_new_from_surface(surface), FALSE, FALSE, 0);
    cairo_surface_destroy(surface);
    /* CUPS can't rename a printer: the name is shown, not edited. */
    GtkWidget *name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name), pr->name);
    gtk_editable_set_editable(GTK_EDITABLE(name), FALSE);
    gtk_box_pack_start(GTK_BOX(top), name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    properties->location = entry(properties, pr->location);
    properties->comment = entry(properties, pr->info);
    grid_row(grid, 0, "_Location:", properties->location);
    grid_row(grid, 1, "_Comment:", properties->comment);
    grid_row(grid, 2, "Mo_del:", label(pr->model[0] != '\0' ? pr->model : "Unknown"));
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    /* Features: what the printer says it can do. */
    GtkWidget *feat = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(feat), 4);
    gtk_grid_set_column_spacing(GTK_GRID(feat), 24);
    bool color = destination->dinfo != nullptr &&
                 cupsCheckDestSupported(CUPS_HTTP_DEFAULT, destination->dest, destination->dinfo,
                                        "print-color-mode", "color");
    bool duplex = destination->dinfo != nullptr &&
                  cupsCheckDestSupported(CUPS_HTTP_DEFAULT, destination->dest, destination->dinfo,
                                         "sides", "two-sided-long-edge");
    bool staple = destination->dinfo != nullptr &&
                  cupsCheckDestSupported(CUPS_HTTP_DEFAULT, destination->dest, destination->dinfo,
                                         CUPS_FINISHINGS, CUPS_FINISHINGS_STAPLE);
    char *res = max_resolution(destination);
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
    media_sizes(destination, values, labels);
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
    g_signal_connect(pref, "clicked", G_CALLBACK(cb_preferences), properties);
    GtkWidget *test = gtk_button_new_with_mnemonic("Print _Test Page");
    g_signal_connect(test, "clicked", G_CALLBACK(cb_test_page), properties);
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
static void sharing_update(props *properties) {
    bool off = properties->server_sharing == 0;
    gtk_widget_set_visible(properties->sharing_off, off);
    gtk_widget_set_visible(properties->share_intro, !off);
    gtk_widget_set_sensitive(properties->share_no, !off);
    gtk_widget_set_sensitive(properties->share_yes, !off);
    gtk_widget_set_sensitive(
        properties->share_name,
        !off && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->share_yes)));
}

static void on_share_toggled([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    sharing_update(properties);
    props_changed(nullptr, properties);
}

static bool turn_on_sharing([[maybe_unused]] gpointer data, GError **error) {
    return xd_cups_set_server_sharing(true, error);
}

static void sharing_turned_on([[maybe_unused]] GtkWidget *dialog, bool ok, gpointer user) {
    props *properties = user;
    if (!ok)
        return;
    properties->server_sharing = 1;
    sharing_update(properties);
    gtk_widget_grab_focus(properties->share_yes);
}

static gboolean on_sharing_link([[maybe_unused]] GtkLabel *label, [[maybe_unused]] const char *uri,
                                gpointer data) {
    props *properties = data;
    run_change(properties->window, turn_on_sharing, nullptr, nullptr, sharing_turned_on,
               properties);
    return TRUE;
}

static GtkWidget *sharing_page(props *properties, const xd_printer *pr) {
    GtkWidget *page = tab_page(pr);
    properties->share_intro =
        label("You can share this printer with other users on your network. To "
              "enable sharing for this printer, click Share this printer.");
    gtk_box_pack_start(GTK_BOX(page), properties->share_intro, FALSE, FALSE, 0);

    properties->sharing_off = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(properties->sharing_off),
                       label("As a security measure, the print spooler (CUPS) doesn't share "
                             "this computer's printers with the network: other computers "
                             "can't print to them."),
                       FALSE, FALSE, 0);
    GtkWidget *link = label(nullptr);
    gtk_label_set_markup(GTK_LABEL(link),
                         "<a href=\"share\">If you understand the security risks but want to "
                         "share printers, click here.</a>");
    gtk_label_set_track_visited_links(GTK_LABEL(link), FALSE);
    g_signal_connect(link, "activate-link", G_CALLBACK(on_sharing_link), properties);
    gtk_box_pack_start(GTK_BOX(properties->sharing_off), link, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), properties->sharing_off, FALSE, FALSE, 0);

    GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(choices, 8);
    properties->share_no =
        gtk_radio_button_new_with_mnemonic(nullptr, "Do _not share this printer");
    properties->share_yes = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(properties->share_no), "_Share this printer");
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(pr->shared ? properties->share_yes : properties->share_no), TRUE);
    properties->saved_shared = pr->shared;
    gtk_box_pack_start(GTK_BOX(choices), properties->share_no, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(choices), properties->share_yes, FALSE, FALSE, 0);
    /* CUPS shares a printer under its own name: the share name is shown,
     * not edited. */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 24);
    properties->share_name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(properties->share_name), pr->name);
    gtk_editable_set_editable(GTK_EDITABLE(properties->share_name), FALSE);
    grid_row(grid, 0, "Share na_me:", properties->share_name);
    gtk_box_pack_start(GTK_BOX(choices), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), choices, FALSE, FALSE, 0);
    g_signal_connect(properties->share_yes, "toggled", G_CALLBACK(on_share_toggled), properties);
    return page;
}

/* ---- Ports ------------------------------------------------------------------------ */

static bool port_find(props *properties, const char *uri, GtkTreeIter *it) {
    GtkTreeModel *model = GTK_TREE_MODEL(properties->ports);
    for (bool ok = gtk_tree_model_get_iter_first(model, it); ok;
         ok = gtk_tree_model_iter_next(model, it)) {
        char *row_uri = nullptr;
        gtk_tree_model_get(model, it, PORT_URI, &row_uri, -1);
        bool same = g_strcmp0(row_uri, uri) == 0;
        g_free(row_uri);
        if (same)
            return true;
    }
    return false;
}

static void port_set(props *properties, GtkTreeIter *it, const char *uri) {
    char *port = xd_printer_port_text(uri);
    gtk_list_store_set(properties->ports, it, PORT_NAME, port, PORT_DESCRIPTION,
                       xd_printer_port_description(uri), PORT_URI, uri, -1);
    g_free(port);
}

static void port_add(props *properties, const char *uri, const char *printers, GtkTreeIter *it) {
    gtk_list_store_insert_with_values(properties->ports, it, -1, PORT_CHECKED, FALSE, PORT_PRINTERS,
                                      printers, -1);
    port_set(properties, it, uri);
}

/* Checks `target`'s box and no other: CUPS prints to one port (XP's
 * printer pooling off). */
static void port_check(props *properties, GtkTreeIter *target) {
    GtkTreeModel *model = GTK_TREE_MODEL(properties->ports);
    GtkTreePath *want = gtk_tree_model_get_path(model, target);
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &it);
        gtk_list_store_set(properties->ports, &it, PORT_CHECKED,
                           gtk_tree_path_compare(path, want) == 0, -1);
        gtk_tree_path_free(path);
    }
    gtk_tree_selection_select_path(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->port_list)), want);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(properties->port_list), want, nullptr, FALSE, 0, 0);
    gtk_tree_path_free(want);
}

/* The checked port's device URI, or nullptr. */
static char *port_checked(props *properties) {
    GtkTreeModel *model = GTK_TREE_MODEL(properties->ports);
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        gboolean on = FALSE;
        char *uri = nullptr;
        gtk_tree_model_get(model, &it, PORT_CHECKED, &on, PORT_URI, &uri, -1);
        if (on)
            return uri;
        g_free(uri);
    }
    return nullptr;
}

/* Makes `uri` the printer's port: its row, else a new one. */
static void port_choose(props *properties, const char *uri) {
    GtkTreeIter it;
    if (!port_find(properties, uri, &it))
        port_add(properties, uri, "", &it);
    port_check(properties, &it);
    props_changed(nullptr, properties);
}

static void on_port_toggled([[maybe_unused]] GtkCellRendererToggle *renderer, char *path,
                            gpointer data) {
    props *properties = data;
    GtkTreeIter it;
    gboolean on = FALSE;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(properties->ports), &it, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(properties->ports), &it, PORT_CHECKED, &on, -1);
    /* Unchecking the only port would leave the printer none. */
    if (on)
        return;
    port_check(properties, &it);
    props_changed(nullptr, properties);
}

/* Space checks the selected port, as on XP's list. */
static gboolean on_port_key([[maybe_unused]] GtkWidget *widget, GdkEventKey *key_event,
                            gpointer data) {
    props *properties = data;
    if (key_event->keyval != GDK_KEY_space ||
        (key_event->state & gtk_accelerator_get_default_mod_mask()) != 0)
        return FALSE;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->port_list));
    if (!gtk_tree_selection_get_selected(sel, nullptr, &it))
        return FALSE;
    gboolean on = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(properties->ports), &it, PORT_CHECKED, &on, -1);
    if (!on) {
        port_check(properties, &it);
        props_changed(nullptr, properties);
    }
    return TRUE;
}

static void on_port_selected(GtkTreeSelection *sel, gpointer data) {
    props *properties = data;
    gtk_widget_set_sensitive(properties->configure,
                             gtk_tree_selection_get_selected(sel, nullptr, nullptr));
}

static void port_error(GtkWidget *parent, const char *title) {
    xd_message_box(GTK_WINDOW(parent), title, XD_MB_ERROR, XD_MB_OK, FALSE,
                   "The device URI is not complete. Type where the printer is, as in "
                   "socket://192.168.1.5:9100, ipp://printer.local/ipp/print or "
                   "usb://Maker/Model.");
}

/* A dialog's "Device URI:" box. */
static GtkWidget *uri_entry(GtkWidget *grid, int row) {
    GtkWidget *widget = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(widget), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(widget), 44);
    grid_row(grid, row, "Device _URI:", widget);
    return widget;
}

static GtkWidget *small_dialog(props *properties, const char *title, const char *ok) {
    GtkWidget *widget = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(widget), title);
    gtk_window_set_transient_for(GTK_WINDOW(widget), GTK_WINDOW(properties->window));
    gtk_window_set_modal(GTK_WINDOW(widget), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(widget), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(widget), ok, GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(widget), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(widget), GTK_RESPONSE_OK);
    return widget;
}

/* Configure Port...: the selected port's device URI, changed. A port no
 * other printer uses changes in place; else this printer gets a port of
 * its own. Either way it's the one checked. */
static void cb_configure_port([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->port_list));
    if (!gtk_tree_selection_get_selected(sel, nullptr, &it))
        return;
    char *uri = nullptr, *printers = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(properties->ports), &it, PORT_URI, &uri, PORT_PRINTERS,
                       &printers, -1);
    GtkWidget *dialog_widget = small_dialog(properties, "Configure Port", "OK");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    GtkWidget *note = label("Where the port prints, as the print spooler (CUPS) names it.");
    gtk_grid_attach(GTK_GRID(grid), note, 0, 0, 2, 1);
    GtkWidget *port_entry = uri_entry(grid, 1);
    gtk_entry_set_text(GTK_ENTRY(port_entry), uri);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog_widget))), grid);
    gtk_widget_show_all(dialog_widget);
    while (gtk_dialog_run(GTK_DIALOG(dialog_widget)) == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(port_entry));
        if (!xd_printer_device_uri_complete(text)) {
            port_error(dialog_widget, "Configure Port");
            gtk_widget_grab_focus(port_entry);
            continue;
        }
        GtkTreeIter same;
        if (strcmp(text, uri) == 0 || port_find(properties, text, &same)) {
            port_choose(properties, text);
        } else if (printers[0] == '\0' || strcmp(printers, properties->name) == 0) {
            port_set(properties, &it, text);
            port_check(properties, &it);
            props_changed(nullptr, properties);
        } else {
            port_choose(properties, text);
        }
        break;
    }
    gtk_widget_destroy(dialog_widget);
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
    scan *port_scan = data;
    if (port_scan->dialog != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(port_scan->dialog), (gpointer *)&port_scan->dialog);
    if (port_scan->found != nullptr)
        g_ptr_array_unref(port_scan->found);
    g_clear_error(&port_scan->error);
    g_free(port_scan);
}

static void scan_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *cancellable) {
    scan *port_scan = data;
    xd_cups_auth_worker_begin();
    port_scan->found = xd_print_devices_list(SCAN_SECONDS, &port_scan->error);
    xd_cups_auth_worker_end();
    g_task_return_boolean(task, TRUE);
}

static void on_scanned([[maybe_unused]] GObject *src, GAsyncResult *res,
                       [[maybe_unused]] gpointer data) {
    const scan *port_scan = g_task_get_task_data(G_TASK(res));
    if (port_scan->dialog == nullptr)
        return;
    add_port *port_dialog = g_object_get_data(G_OBJECT(port_scan->dialog), "xd-add-port");
    if (port_scan->found == nullptr) {
        bool canceled = port_scan->error != nullptr && port_scan->error->domain == XD_CUPS_ERROR &&
                        port_scan->error->code == XD_CUPS_ERROR_CANCELED;
        gtk_label_set_text(GTK_LABEL(port_dialog->status), canceled || port_scan->error == nullptr
                                                               ? "Type the port's device URI."
                                                               : port_scan->error->message);
        return;
    }
    for (guint i = 0; i < port_scan->found->len; i++) {
        const xd_print_device *print_device = g_ptr_array_index(port_scan->found, i);
        char *start = xd_printer_device_uri_start(print_device->uri);
        /* A kind of port with no address yet goes by its start. */
        char *port = xd_printer_device_uri_complete(print_device->uri)
                         ? xd_printer_port_text(print_device->uri)
                         : g_strdup(start);
        const char *desc = print_device->model[0] != '\0' &&
                                   strcmp(print_device->model, print_device->info) != 0 &&
                                   strstr(print_device->info, print_device->model) == nullptr
                               ? print_device->model
                               : print_device->info;
        gtk_list_store_insert_with_values(port_dialog->store, nullptr, -1, FOUND_PORT, port,
                                          FOUND_DESCRIPTION, desc, FOUND_URI, print_device->uri,
                                          -1);
        g_free(port);
        g_free(start);
    }
    gtk_label_set_text(
        GTK_LABEL(port_dialog->status),
        port_scan->found->len > 0 ? "" : "No ports were found. Type the port's device URI.");
}

static void on_found_selected(GtkTreeSelection *sel, gpointer data) {
    add_port *port_dialog = data;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, nullptr, &it))
        return;
    char *uri = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(port_dialog->store), &it, FOUND_URI, &uri, -1);
    char *start = xd_printer_device_uri_start(uri);
    gtk_entry_set_text(GTK_ENTRY(port_dialog->uri), start);
    gtk_editable_set_position(GTK_EDITABLE(port_dialog->uri), -1);
    g_free(start);
    g_free(uri);
}

static void on_found_activated([[maybe_unused]] GtkTreeView *tree_view,
                               [[maybe_unused]] GtkTreePath *path,
                               [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    add_port *port_dialog = data;
    gtk_dialog_response(GTK_DIALOG(port_dialog->dialog), GTK_RESPONSE_OK);
}

static void on_add_port_response(GtkDialog *dialog_widget, int response, gpointer data) {
    add_port *port_dialog = data;
    if (response == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(port_dialog->uri));
        if (!xd_printer_device_uri_complete(text)) {
            port_error(GTK_WIDGET(dialog_widget), "Printer Ports");
            gtk_widget_grab_focus(port_dialog->uri);
            gtk_editable_set_position(GTK_EDITABLE(port_dialog->uri), -1);
            return;
        }
        port_choose(port_dialog->p, text);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

static void add_port_free(gpointer data) {
    add_port *port_dialog = data;
    g_object_unref(port_dialog->store);
    g_free(port_dialog);
}

static void cb_add_port([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    add_port *port_dialog = g_new0(add_port, 1);
    port_dialog->p = properties;
    port_dialog->dialog = small_dialog(properties, "Printer Ports", "New _Port");
    g_object_set_data_full(G_OBJECT(port_dialog->dialog), "xd-add-port", port_dialog,
                           add_port_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *ports_label = gtk_label_new_with_mnemonic("_Available ports:");
    gtk_label_set_xalign(GTK_LABEL(ports_label), 0);
    gtk_box_pack_start(GTK_BOX(box), ports_label, FALSE, FALSE, 0);

    port_dialog->store =
        gtk_list_store_new(FOUND_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    port_dialog->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(port_dialog->store));
    gtk_label_set_mnemonic_widget(GTK_LABEL(ports_label), port_dialog->list);
    static const char *const titles[] = {"Port", "Description"};
    static const int widths[] = {150, 230};
    for (int i = 0; i < 2; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *column =
            gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, nullptr);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(column, widths[i]);
        gtk_tree_view_append_column(GTK_TREE_VIEW(port_dialog->list), column);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(port_dialog->list)), "changed",
                     G_CALLBACK(on_found_selected), port_dialog);
    g_signal_connect(port_dialog->list, "row-activated", G_CALLBACK(on_found_activated),
                     port_dialog);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 150);
    gtk_container_add(GTK_CONTAINER(scroll), port_dialog->list);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    port_dialog->status = label("Searching for ports...");
    gtk_box_pack_start(GTK_BOX(box), port_dialog->status, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    port_dialog->uri = uri_entry(grid, 0);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(port_dialog->dialog))),
                      box);
    g_signal_connect(port_dialog->dialog, "response", G_CALLBACK(on_add_port_response),
                     port_dialog);
    gtk_widget_show_all(port_dialog->dialog);
    gtk_widget_grab_focus(port_dialog->uri);

    /* The backends take a while; meanwhile a URI can be typed. */
    scan *port_scan = g_new0(scan, 1);
    port_scan->dialog = port_dialog->dialog;
    g_object_add_weak_pointer(G_OBJECT(port_dialog->dialog), (gpointer *)&port_scan->dialog);
    GTask *task = g_task_new(nullptr, nullptr, on_scanned, nullptr);
    g_task_set_task_data(task, port_scan, scan_free);
    g_task_run_in_thread(task, scan_thread);
    g_object_unref(task);
}

static GtkWidget *ports_page(props *properties, const xd_printer *pr, GPtrArray *all) {
    GtkWidget *page = tab_page(pr);
    GtkWidget *intro = gtk_label_new_with_mnemonic(
        "_Print to the following port(s). Documents will print to the first free checked port.");
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 52);
    gtk_box_pack_start(GTK_BOX(page), intro, FALSE, FALSE, 0);

    /* Every printer's port, with the printers that print to it: XP's list
     * of the computer's ports. */
    properties->ports = gtk_list_store_new(PORT_COUNT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING,
                                           G_TYPE_STRING, G_TYPE_STRING);
    properties->port_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(properties->ports));
    gtk_label_set_mnemonic_widget(GTK_LABEL(intro), properties->port_list);
    properties->saved_uri = g_strdup(pr->device_uri);
    for (guint i = 0; all != nullptr && i < all->len; i++) {
        const xd_printer *printer = g_ptr_array_index(all, i);
        if (printer->device_uri[0] == '\0')
            continue;
        GtkTreeIter it;
        if (!port_find(properties, printer->device_uri, &it)) {
            port_add(properties, printer->device_uri, printer->name, &it);
            continue;
        }
        char *names = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(properties->ports), &it, PORT_PRINTERS, &names, -1);
        char *more = g_strdup_printf("%s, %s", names, printer->name);
        gtk_list_store_set(properties->ports, &it, PORT_PRINTERS, more, -1);
        g_free(more);
        g_free(names);
    }
    GtkTreeIter own;
    if (!port_find(properties, pr->device_uri, &own))
        port_add(properties, pr->device_uri, pr->name, &own);
    gtk_list_store_set(properties->ports, &own, PORT_CHECKED, TRUE, -1);

    GtkCellRenderer *check = gtk_cell_renderer_toggle_new();
    g_signal_connect(check, "toggled", G_CALLBACK(on_port_toggled), properties);
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
            GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
            g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            col = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", columns[i],
                                                           nullptr);
        }
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        if (widths[i] > 0)
            gtk_tree_view_column_set_fixed_width(col, widths[i]);
        else
            gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(properties->port_list), col);
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->port_list));
    g_signal_connect(sel, "changed", G_CALLBACK(on_port_selected), properties);
    g_signal_connect(properties->port_list, "key-press-event", G_CALLBACK(on_port_key), properties);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 160);
    gtk_container_add(GTK_CONTAINER(scroll), properties->port_list);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    /* XP's Delete Port has no counterpart: CUPS's ports are its backends'
     * devices, not something to remove. */
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add = gtk_button_new_with_mnemonic("Add Por_t...");
    g_signal_connect(add, "clicked", G_CALLBACK(cb_add_port), properties);
    properties->configure = gtk_button_new_with_mnemonic("_Configure Port...");
    g_signal_connect(properties->configure, "clicked", G_CALLBACK(cb_configure_port), properties);
    gtk_box_pack_end(GTK_BOX(buttons), properties->configure, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    port_check(properties, &own);
    return page;
}

/* ---- saving ----------------------------------------------------------------------- */

/* What Apply sends, for the worker. */
typedef struct props_saving {
    char *name, *info, *location, *uri;
    int shared;
} props_saving;

static void props_saving_free(gpointer data) {
    props_saving *saving = data;
    g_free(saving->name);
    g_free(saving->info);
    g_free(saving->location);
    g_free(saving->uri);
    g_free(saving);
}

static bool props_save_now(gpointer data, GError **error) {
    const props_saving *saving = data;
    xd_printer_changes printer_changes = {.info = saving->info,
                                          .location = saving->location,
                                          .device_uri = saving->uri,
                                          .shared = saving->shared};
    return xd_printer_modify(saving->name, &printer_changes, error);
}

static void props_saved(GtkWidget *dialog, bool ok, gpointer user) {
    props *properties = user;
    if (!ok) {
        properties->closing = false;
        return;
    }
    g_free(properties->saved_uri);
    properties->saved_uri = port_checked(properties);
    properties->saved_shared =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->share_yes));
    gtk_widget_set_sensitive(properties->apply, FALSE);
    if (properties->closing)
        gtk_widget_destroy(dialog);
}

/* One CUPS-Add-Modify-Printer with what changed (the Comment and
 * Location always), in a worker; OK closes the window once it's done. */
static void props_save(props *properties) {
    props_saving *saving = g_new0(props_saving, 1);
    saving->name = g_strdup(properties->name);
    saving->info = g_strdup(gtk_entry_get_text(GTK_ENTRY(properties->comment)));
    saving->location = g_strdup(gtk_entry_get_text(GTK_ENTRY(properties->location)));
    saving->uri = port_checked(properties);
    if (saving->uri != nullptr && g_strcmp0(saving->uri, properties->saved_uri) == 0)
        g_clear_pointer(&saving->uri, g_free);
    bool shared = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->share_yes));
    saving->shared = shared != properties->saved_shared ? shared : -1;
    run_change(properties->window, props_save_now, saving, props_saving_free, props_saved,
               properties);
}

static void on_props_response(GtkDialog *dialog_widget, int response, gpointer data) {
    props *properties = data;
    if (is_busy(GTK_WIDGET(dialog_widget)))
        return;
    if (response == GTK_RESPONSE_APPLY) {
        props_save(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply)) {
        properties->closing = true;
        props_save(properties);
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

static void on_props_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    g_object_unref(properties->ports);
    g_free(properties->saved_uri);
    g_free(properties->name);
    g_free(properties);
}

static void props_fetched(GtkWindow *parent, const char *name, fetched *fetched_info) {
    const xd_printer *pr = fetched_info->printer;
    if (pr == nullptr) {
        xd_printers_error(parent, fetched_info->error);
        return;
    }
    props *properties = g_new0(props, 1);
    properties->name = g_strdup(name);
    properties->server_sharing = fetched_info->server_sharing;
    properties->window = gtk_dialog_new();
    char *title = g_strdup_printf("%s Properties", name);
    gtk_window_set_title(GTK_WINDOW(properties->window), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(properties->window), "printer");
    gtk_window_set_resizable(GTK_WINDOW(properties->window), FALSE);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(properties->window), parent);
    xd_printers_hold(properties->window);
    gtk_dialog_add_button(GTK_DIALOG(properties->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties->window), "Cancel", GTK_RESPONSE_CANCEL);
    properties->apply =
        gtk_dialog_add_button(GTK_DIALOG(properties->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(properties->window), GTK_RESPONSE_OK);

    /* XP's order: General, Sharing, Ports. */
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(properties, pr, &fetched_info->d),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), sharing_page(properties, pr),
                             gtk_label_new_with_mnemonic("S_haring"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), ports_page(properties, pr, fetched_info->all),
                             gtk_label_new_with_mnemonic("P_orts"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties->window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties->window, GTK_NOTEBOOK(tabs));
    g_signal_connect(properties->window, "response", G_CALLBACK(on_props_response), properties);
    g_signal_connect(properties->window, "delete-event", G_CALLBACK(on_busy_delete), nullptr);
    g_signal_connect(properties->window, "destroy", G_CALLBACK(on_props_destroy), properties);
    gtk_widget_show_all(properties->window);
    sharing_update(properties);
    gtk_widget_set_sensitive(properties->apply, FALSE);
}

void xd_printer_properties_open(GtkWindow *parent, const char *name) {
    fetch_and_open(parent, name, true, props_fetched);
}
