/* SPDX-License-Identifier: MIT
 * Customize Regional Options. XP's fields that glibc's formats have no
 * place for (digits after the decimal and the negative format of plain
 * numbers, leading zeros, the list separator, the calendar's two-digit
 * years, the long date) aren't offered: no program would follow them.
 */
#include <stdlib.h>
#include <string.h>

#include "../gtkapp/gtkapp.h"
#include "customize.h"

static constexpr char TITLE[] = "Customize Regional Options";

/* Digit grouping choices, localedef's way; the locale's own is added if
 * it's another. */
static const char *const groupings[] = {"3", "-1", "3;2"};

typedef struct customize {
    const char *locale;
    xd_intl_custom base; /* the locale's own */
    bool updating;
    GtkWidget *dialog;
    /* Numbers */
    GtkWidget *num_pos, *num_neg, *decimal, *thousands, *grouping, *measurement;
    /* Currency */
    GtkWidget *cur_pos, *cur_neg, *currency, *pos_format, *neg_format, *mon_decimal, *frac,
        *mon_thousands, *mon_grouping;
    /* Time */
    GtkWidget *time_sample, *time_format, *time_sep, *am, *pm;
    char *time_sep_was;
    /* Date */
    GtkWidget *date_sample, *date_format, *date_sep;
    char *date_sep_was;
} customize;

/* ---- widgets ------------------------------------------------------------------------ */

static const char *entry_of(GtkWidget *combo) {
    return gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))));
}

static void set_entry_of(GtkWidget *combo, const char *text) {
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), text != nullptr ? text : "");
}

static void on_any([[maybe_unused]] GtkWidget *changed, gpointer data);

/* A combo box you can type in: `current` first, then the usual choices. */
static GtkWidget *editable(customize *customizer, const char *current, const char *const *choices,
                           int max) {
    GtkWidget *widget = gtk_combo_box_text_new_with_entry();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    if (current != nullptr) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), current);
        g_hash_table_add(seen, (gpointer)(uintptr_t)current);
    }
    for (const char *const *ch = choices; ch != nullptr && *ch != nullptr; ch++) {
        if (!g_hash_table_contains(seen, *ch)) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), *ch);
            g_hash_table_add(seen, (gpointer)(uintptr_t)*ch);
        }
    }
    g_hash_table_unref(seen);
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(widget));
    gtk_entry_set_max_length(GTK_ENTRY(entry), max);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), MIN(max, 16));
    set_entry_of(widget, current);
    g_signal_connect(widget, "changed", G_CALLBACK(on_any), customizer);
    return widget;
}

static GtkWidget *sample(void) {
    GtkWidget *entry_widget = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(entry_widget), FALSE);
    gtk_widget_set_can_focus(entry_widget, FALSE);
    gtk_widget_set_hexpand(entry_widget, TRUE);
    return entry_widget;
}

static void row(GtkWidget *grid, int y, const char *mnemonic, GtkWidget *field) {
    GtkWidget *label = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), field);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 0, y, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), field, 1, y, 1, 1);
}

static GtkWidget *grid_new(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    return grid;
}

static GtkWidget *samples_frame(GtkWidget **first, const char *la, GtkWidget **second,
                                const char *lb) {
    GtkWidget *grid = grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    *first = sample();
    GtkWidget *label = gtk_label_new(la);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), *first, 1, 0, 1, 1);
    if (second != nullptr) {
        *second = sample();
        label = gtk_label_new(lb);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_grid_attach(GTK_GRID(grid), label, 2, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), *second, 3, 0, 1, 1);
    }
    GtkWidget *frame = gtk_frame_new("Samples");
    gtk_container_add(GTK_CONTAINER(frame), grid);
    return frame;
}

static GtkWidget *page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    return box;
}

/* Digit grouping, shown as it groups 123456789 with `sep`. */
static void fill_grouping(GtkWidget *combo, const char *active, const char *own, const char *sep) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    for (size_t i = 0; i < G_N_ELEMENTS(groupings); i++) {
        char *sample = xd_intl_group_digits("123456789", groupings[i], sep);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), groupings[i], sample);
        g_free(sample);
    }
    bool listed = false;
    for (size_t i = 0; i < G_N_ELEMENTS(groupings) && own != nullptr; i++)
        listed = listed || strcmp(groupings[i], own) == 0;
    if (own != nullptr && !listed) {
        char *sample = xd_intl_group_digits("123456789", own, sep);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), own, sample);
        g_free(sample);
    }
    if (active == nullptr || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

/* "3;3" groups as "3" does: the last size repeats. */
static const char *grouping_id(const char *grouping) {
    return grouping != nullptr && strcmp(grouping, "3;3") == 0 ? "3" : grouping;
}

static GtkWidget *grouping_combo(customize *customizer, const char *active, const char *sep) {
    GtkWidget *widget = gtk_combo_box_text_new();
    fill_grouping(widget, grouping_id(active), grouping_id(customizer->base.grouping), sep);
    g_signal_connect(widget, "changed", G_CALLBACK(on_any), customizer);
    return widget;
}

static void fill_formats(GtkWidget *combo, bool negative, int active, const char *symbol) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    int format_count = negative ? XD_INTL_NEGATIVE_FORMATS : XD_INTL_POSITIVE_FORMATS;
    for (int i = 0; i < format_count; i++) {
        char *look = xd_intl_currency_format_look(negative, i, symbol);
        char id[8];
        g_snprintf(id, sizeof(id), "%d", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, look);
        g_free(look);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), CLAMP(active, 0, format_count - 1));
}

static int active_int(GtkWidget *combo) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    return id != nullptr ? atoi(id) : 0;
}

/* ---- reading and showing --------------------------------------------------------------- */

/* The settings as the dialog has them now. */
static void read_effective(customize *customizer, xd_intl_custom *effective) {
    xd_intl_custom_init(effective);
    effective->decimal = g_strdup(entry_of(customizer->decimal));
    effective->thousands = g_strdup(entry_of(customizer->thousands));
    effective->grouping =
        g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(customizer->grouping)));
    effective->measurement = active_int(customizer->measurement);
    effective->currency = g_strdup(entry_of(customizer->currency));
    effective->positive_format = active_int(customizer->pos_format);
    effective->negative_format = active_int(customizer->neg_format);
    effective->mon_decimal = g_strdup(entry_of(customizer->mon_decimal));
    effective->frac_digits = active_int(customizer->frac);
    effective->mon_thousands = g_strdup(entry_of(customizer->mon_thousands));
    effective->mon_grouping =
        g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(customizer->mon_grouping)));
    effective->time_format = g_strdup(entry_of(customizer->time_format));
    effective->am = g_strdup(entry_of(customizer->am));
    effective->pm = g_strdup(entry_of(customizer->pm));
    effective->date_format = g_strdup(entry_of(customizer->date_format));
}

static void show_samples(customize *customizer) {
    xd_intl_custom effective;
    read_effective(customizer, &effective);
    time_t now = time(nullptr);
    char *samples[] = {
        xd_intl_number_sample(&effective, false),
        xd_intl_number_sample(&effective, true),
        xd_intl_currency_sample(&effective, false),
        xd_intl_currency_sample(&effective, true),
        xd_intl_time_sample(customizer->locale, &effective, now),
        xd_intl_date_sample(customizer->locale, &effective, now, false),
    };
    GtkWidget *entries[] = {customizer->num_pos, customizer->num_neg,     customizer->cur_pos,
                            customizer->cur_neg, customizer->time_sample, customizer->date_sample};
    for (size_t i = 0; i < G_N_ELEMENTS(samples); i++) {
        gtk_entry_set_text(GTK_ENTRY(entries[i]), samples[i]);
        g_free(samples[i]);
    }
    xd_intl_custom_clear(&effective);
}

/* A separator field changed: the format follows. */
static void follow_separator(GtkWidget *sep, GtkWidget *format, char **was) {
    const char *now = entry_of(sep);
    /* Typing over it passes through "": the old one is still the one in
     * the format. */
    if (now[0] == '\0')
        return;
    if (*was != nullptr && (*was)[0] != '\0' && strcmp(*was, now) != 0) {
        char *new_format = xd_intl_picture_replace_separator(entry_of(format), *was, now);
        set_entry_of(format, new_format);
        g_free(new_format);
    }
    g_free(*was);
    *was = g_strdup(now);
}

/* A format changed: its separator field shows its separator. */
static void show_separator(GtkWidget *format, GtkWidget *sep, char **was) {
    char *separator = xd_intl_picture_separator(entry_of(format));
    if (separator[0] == '\0') { /* none (yet) */
        g_free(separator);
        return;
    }
    if (strcmp(separator, entry_of(sep)) != 0)
        set_entry_of(sep, separator);
    g_free(*was);
    *was = separator;
}

static void on_any(GtkWidget *changed, gpointer data) {
    customize *customizer = data;
    if (customizer->updating)
        return;
    customizer->updating = true;
    if (changed == customizer->time_sep)
        follow_separator(customizer->time_sep, customizer->time_format, &customizer->time_sep_was);
    else if (changed == customizer->time_format)
        show_separator(customizer->time_format, customizer->time_sep, &customizer->time_sep_was);
    else if (changed == customizer->date_sep)
        follow_separator(customizer->date_sep, customizer->date_format, &customizer->date_sep_was);
    else if (changed == customizer->date_format)
        show_separator(customizer->date_format, customizer->date_sep, &customizer->date_sep_was);
    else if (changed == customizer->thousands || changed == customizer->mon_thousands) {
        GtkWidget *combo =
            changed == customizer->thousands ? customizer->grouping : customizer->mon_grouping;
        char *id = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
        const char *own = changed == customizer->thousands ? customizer->base.grouping
                                                           : customizer->base.mon_grouping;
        fill_grouping(combo, id, grouping_id(own), entry_of(changed));
        g_free(id);
    } else if (changed == customizer->currency) {
        fill_formats(customizer->pos_format, false, active_int(customizer->pos_format),
                     entry_of(customizer->currency));
        fill_formats(customizer->neg_format, true, active_int(customizer->neg_format),
                     entry_of(customizer->currency));
    }
    customizer->updating = false;
    show_samples(customizer);
}

/* ---- the tabs ------------------------------------------------------------------------ */

static GtkWidget *numbers_page(customize *customizer, const xd_intl_custom *custom) {
    GtkWidget *box = page();
    gtk_box_pack_start(
        GTK_BOX(box),
        samples_frame(&customizer->num_pos, "Positive:", &customizer->num_neg, "Negative:"), FALSE,
        FALSE, 0);
    GtkWidget *grid = grid_new();
    static const char *const decimals[] = {".", ",", nullptr};
    static const char *const separators[] = {",", ".", " ", "'", nullptr};
    customizer->decimal = editable(customizer, custom->decimal, decimals, 3);
    row(grid, 0, "_Decimal symbol:", customizer->decimal);
    customizer->thousands = editable(customizer, custom->thousands, separators, 3);
    row(grid, 1, "Digit grouping s_ymbol:", customizer->thousands);
    customizer->grouping = grouping_combo(customizer, custom->grouping, custom->thousands);
    row(grid, 2, "Digit _grouping:", customizer->grouping);
    customizer->measurement = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(customizer->measurement), "1", "Metric");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(customizer->measurement), "2", "U.S.");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(customizer->measurement),
                                custom->measurement == 2 ? "2" : "1");
    g_signal_connect(customizer->measurement, "changed", G_CALLBACK(on_any), customizer);
    row(grid, 3, "_Measurement system:", customizer->measurement);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *currency_page(customize *customizer, const xd_intl_custom *custom) {
    GtkWidget *box = page();
    gtk_box_pack_start(
        GTK_BOX(box),
        samples_frame(&customizer->cur_pos, "Positive:", &customizer->cur_neg, "Negative:"), FALSE,
        FALSE, 0);
    GtkWidget *grid = grid_new();
    static const char *const symbols[] = {"$", "€", "£", "¥", nullptr};
    static const char *const decimals[] = {".", ",", nullptr};
    static const char *const separators[] = {",", ".", " ", "'", nullptr};
    customizer->currency = editable(customizer, custom->currency, symbols, 5);
    row(grid, 0, "Currency _symbol:", customizer->currency);
    customizer->pos_format = gtk_combo_box_text_new();
    fill_formats(customizer->pos_format, false, custom->positive_format, custom->currency);
    g_signal_connect(customizer->pos_format, "changed", G_CALLBACK(on_any), customizer);
    row(grid, 1, "_Position of currency symbol:", customizer->pos_format);
    customizer->neg_format = gtk_combo_box_text_new();
    fill_formats(customizer->neg_format, true, custom->negative_format, custom->currency);
    g_signal_connect(customizer->neg_format, "changed", G_CALLBACK(on_any), customizer);
    row(grid, 2, "_Negative currency format:", customizer->neg_format);
    customizer->mon_decimal = editable(customizer, custom->mon_decimal, decimals, 3);
    row(grid, 3, "_Decimal symbol:", customizer->mon_decimal);
    customizer->frac = gtk_combo_box_text_new();
    for (int i = 0; i <= 9; i++) {
        char id[4];
        g_snprintf(id, sizeof(id), "%d", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(customizer->frac), id, id);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(customizer->frac), CLAMP(custom->frac_digits, 0, 9));
    g_signal_connect(customizer->frac, "changed", G_CALLBACK(on_any), customizer);
    row(grid, 4, "No. of digits _after decimal:", customizer->frac);
    customizer->mon_thousands = editable(customizer, custom->mon_thousands, separators, 3);
    row(grid, 5, "Digit grouping s_ymbol:", customizer->mon_thousands);
    customizer->mon_grouping =
        grouping_combo(customizer, custom->mon_grouping, custom->mon_thousands);
    row(grid, 6, "Digit _grouping:", customizer->mon_grouping);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *legend(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 56);
    return label;
}

static GtkWidget *time_page(customize *customizer, const xd_intl_custom *custom) {
    GtkWidget *box = page();
    gtk_box_pack_start(GTK_BOX(box),
                       samples_frame(&customizer->time_sample, "Time sample:", nullptr, nullptr),
                       FALSE, FALSE, 0);
    GtkWidget *grid = grid_new();
    static const char *const formats[] = {"h:mm:ss tt", "hh:mm:ss tt", "H:mm:ss", "HH:mm:ss",
                                          nullptr};
    static const char *const separators[] = {":", ".", nullptr};
    static const char *const ams[] = {"AM", nullptr};
    static const char *const pms[] = {"PM", nullptr};
    customizer->time_format = editable(customizer, custom->time_format, formats, 40);
    row(grid, 0, "_Time format:", customizer->time_format);
    customizer->time_sep_was = xd_intl_picture_separator(custom->time_format);
    customizer->time_sep = editable(customizer, customizer->time_sep_was, separators, 3);
    row(grid, 1, "Time _separator:", customizer->time_sep);
    customizer->am = editable(customizer, custom->am, ams, 12);
    row(grid, 2, "_AM symbol:", customizer->am);
    customizer->pm = editable(customizer, custom->pm, pms, 12);
    row(grid, 3, "_PM symbol:", customizer->pm);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(nullptr);
    GtkWidget *lg = legend("h = hour   m = minute   s = second   t = A or P\n\n"
                           "h/H = 12/24 hour\n"
                           "hh, mm, ss = leading zero\n"
                           "h, m, s = no leading zero");
    /* In a box: a frame's own label child would be styled as its title. */
    GtkWidget *inside = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(inside), 8);
    gtk_box_pack_start(GTK_BOX(inside), lg, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), inside);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *date_page(customize *customizer, const xd_intl_custom *custom) {
    GtkWidget *box = page();
    GtkWidget *grid = grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    GtkWidget *frame = gtk_frame_new("Short date");
    gtk_container_add(GTK_CONTAINER(frame), grid);
    static const char *const formats[] = {"M/d/yyyy", "M/d/yy",    "MM/dd/yy",  "MM/dd/yyyy",
                                          "yy/MM/dd", "yyyy-MM-dd", "dd-MMM-yy", "dd.MM.yyyy",
                                          "d.M.yyyy", "dd/MM/yyyy", nullptr};
    static const char *const separators[] = {"/", "-", ".", nullptr};
    customizer->date_sample = sample();
    GtkWidget *label = gtk_label_new("Short date sample:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), customizer->date_sample, 1, 0, 1, 1);
    customizer->date_format = editable(customizer, custom->date_format, formats, 40);
    row(grid, 1, "S_hort date format:", customizer->date_format);
    customizer->date_sep_was = xd_intl_picture_separator(custom->date_format);
    customizer->date_sep = editable(customizer, customizer->date_sep_was, separators, 3);
    row(grid, 2, "_Date separator:", customizer->date_sep);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       legend("d = day   M = month   y = year\n\n"
                              "d, M = no leading zero   dd, MM = leading zero\n"
                              "ddd, MMM = abbreviated name   dddd, MMMM = full name\n"
                              "yy = two-digit year   yyyy = four-digit year"),
                       FALSE, FALSE, 0);
    return box;
}

/* ---- keeping ------------------------------------------------------------------------ */

/* `value` if it isn't the locale's own (`own`). */
static char *changed_text(const char *value, const char *own) {
    return value != nullptr && g_strcmp0(value, own) != 0 ? g_strdup(value) : nullptr;
}

static bool keep(customize *customizer, xd_intl_custom *custom) {
    xd_intl_custom effective;
    read_effective(customizer, &effective);
    const char *problem = effective.decimal[0] == '\0'       ? "Type a decimal symbol."
                          : effective.mon_decimal[0] == '\0' ? "Type a decimal symbol for currency."
                          : effective.time_format[0] == '\0' ? "Type a time format."
                          : effective.date_format[0] == '\0' ? "Type a short date format."
                                                             : nullptr;
    if (problem != nullptr) {
        xd_message_box(GTK_WINDOW(customizer->dialog), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       problem);
        xd_intl_custom_clear(&effective);
        return false;
    }
    const xd_intl_custom *base = &customizer->base;
    xd_intl_custom_clear(custom);
    custom->decimal = changed_text(effective.decimal, base->decimal);
    custom->thousands = changed_text(effective.thousands, base->thousands);
    custom->grouping = changed_text(grouping_id(effective.grouping), grouping_id(base->grouping));
    custom->measurement = effective.measurement != base->measurement ? effective.measurement : 0;
    custom->currency = changed_text(effective.currency, base->currency);
    custom->mon_decimal = changed_text(effective.mon_decimal, base->mon_decimal);
    custom->mon_thousands = changed_text(effective.mon_thousands, base->mon_thousands);
    custom->mon_grouping =
        changed_text(grouping_id(effective.mon_grouping), grouping_id(base->mon_grouping));
    custom->frac_digits = effective.frac_digits != base->frac_digits ? effective.frac_digits : -1;
    custom->positive_format =
        effective.positive_format != base->positive_format ? effective.positive_format : -1;
    custom->negative_format =
        effective.negative_format != base->negative_format ? effective.negative_format : -1;
    custom->time_format = changed_text(effective.time_format, base->time_format);
    custom->am = changed_text(effective.am, base->am);
    custom->pm = changed_text(effective.pm, base->pm);
    custom->date_format = changed_text(effective.date_format, base->date_format);
    xd_intl_custom_clear(&effective);
    return true;
}

void xd_intl_customize(GtkWindow *parent, const char *locale, xd_intl_custom *custom,
                       void (*applied)(void *data), void *data) {
    customize customizer = {.locale = locale};
    xd_intl_custom effective;
    if (!xd_intl_custom_from_locale(locale, &customizer.base) ||
        !xd_intl_custom_effective(locale, custom, &effective)) {
        xd_message_box(parent, TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "These formats aren't installed, so they can't be customized.");
        xd_intl_custom_clear(&customizer.base);
        return;
    }
    customizer.dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(customizer.dialog), TITLE);
    gtk_window_set_transient_for(GTK_WINDOW(customizer.dialog), parent);
    gtk_window_set_modal(GTK_WINDOW(customizer.dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(customizer.dialog), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(customizer.dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(customizer.dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(customizer.dialog), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(customizer.dialog), GTK_RESPONSE_OK);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    customizer.updating = true;
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), numbers_page(&customizer, &effective),
                             gtk_label_new_with_mnemonic("_Numbers"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), currency_page(&customizer, &effective),
                             gtk_label_new_with_mnemonic("_Currency"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), time_page(&customizer, &effective),
                             gtk_label_new_with_mnemonic("_Time"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), date_page(&customizer, &effective),
                             gtk_label_new_with_mnemonic("_Date"));
    customizer.updating = false;
    xd_intl_custom_clear(&effective);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(customizer.dialog))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(customizer.dialog, GTK_NOTEBOOK(tabs));
    show_samples(&customizer);
    gtk_widget_show_all(customizer.dialog);
    for (int response;
         (response = gtk_dialog_run(GTK_DIALOG(customizer.dialog))) == GTK_RESPONSE_OK ||
         response == GTK_RESPONSE_APPLY;) {
        if (!keep(&customizer, custom))
            continue;
        applied(data);
        if (response == GTK_RESPONSE_OK)
            break;
    }
    gtk_widget_destroy(customizer.dialog);
    g_free(customizer.time_sep_was);
    g_free(customizer.date_sep_was);
    xd_intl_custom_clear(&customizer.base);
}
