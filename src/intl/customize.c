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

static void on_any([[maybe_unused]] GtkWidget *w, gpointer data);

/* A combo box you can type in: `current` first, then the usual choices. */
static GtkWidget *editable(customize *c, const char *current, const char *const *choices,
                           int max) {
    GtkWidget *w = gtk_combo_box_text_new_with_entry();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    if (current != nullptr) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), current);
        g_hash_table_add(seen, (gpointer)(uintptr_t)current);
    }
    for (const char *const *ch = choices; ch != nullptr && *ch != nullptr; ch++) {
        if (!g_hash_table_contains(seen, *ch)) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), *ch);
            g_hash_table_add(seen, (gpointer)(uintptr_t)*ch);
        }
    }
    g_hash_table_unref(seen);
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(w));
    gtk_entry_set_max_length(GTK_ENTRY(entry), max);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), MIN(max, 16));
    set_entry_of(w, current);
    g_signal_connect(w, "changed", G_CALLBACK(on_any), c);
    return w;
}

static GtkWidget *sample(void) {
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(e), FALSE);
    gtk_widget_set_can_focus(e, FALSE);
    gtk_widget_set_hexpand(e, TRUE);
    return e;
}

static void row(GtkWidget *grid, int y, const char *mnemonic, GtkWidget *w) {
    GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, y, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w, 1, y, 1, 1);
}

static GtkWidget *grid_new(void) {
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    return g;
}

static GtkWidget *samples_frame(GtkWidget **a, const char *la, GtkWidget **b, const char *lb) {
    GtkWidget *g = grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(g), 8);
    *a = sample();
    GtkWidget *l = gtk_label_new(la);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(g), l, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), *a, 1, 0, 1, 1);
    if (b != nullptr) {
        *b = sample();
        l = gtk_label_new(lb);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(g), l, 2, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(g), *b, 3, 0, 1, 1);
    }
    GtkWidget *f = gtk_frame_new("Samples");
    gtk_container_add(GTK_CONTAINER(f), g);
    return f;
}

static GtkWidget *page(void) {
    GtkWidget *p = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(p), 10);
    return p;
}

/* Digit grouping, shown as it groups 123456789 with `sep`. */
static void fill_grouping(GtkWidget *combo, const char *active, const char *own, const char *sep) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    for (size_t i = 0; i < G_N_ELEMENTS(groupings); i++) {
        char *s = xd_intl_group_digits("123456789", groupings[i], sep);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), groupings[i], s);
        g_free(s);
    }
    bool listed = false;
    for (size_t i = 0; i < G_N_ELEMENTS(groupings) && own != nullptr; i++)
        listed = listed || strcmp(groupings[i], own) == 0;
    if (own != nullptr && !listed) {
        char *s = xd_intl_group_digits("123456789", own, sep);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), own, s);
        g_free(s);
    }
    if (active == nullptr || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

/* "3;3" groups as "3" does: the last size repeats. */
static const char *grouping_id(const char *g) {
    return g != nullptr && strcmp(g, "3;3") == 0 ? "3" : g;
}

static GtkWidget *grouping_combo(customize *c, const char *active, const char *sep) {
    GtkWidget *w = gtk_combo_box_text_new();
    fill_grouping(w, grouping_id(active), grouping_id(c->base.grouping), sep);
    g_signal_connect(w, "changed", G_CALLBACK(on_any), c);
    return w;
}

static void fill_formats(GtkWidget *combo, bool negative, int active, const char *symbol) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    int n = negative ? XD_INTL_NEGATIVE_FORMATS : XD_INTL_POSITIVE_FORMATS;
    for (int i = 0; i < n; i++) {
        char *look = xd_intl_currency_format_look(negative, i, symbol);
        char id[8];
        g_snprintf(id, sizeof(id), "%d", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, look);
        g_free(look);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), CLAMP(active, 0, n - 1));
}

static int active_int(GtkWidget *combo) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    return id != nullptr ? atoi(id) : 0;
}

/* ---- reading and showing --------------------------------------------------------------- */

/* The settings as the dialog has them now. */
static void read_effective(customize *c, xd_intl_custom *e) {
    xd_intl_custom_init(e);
    e->decimal = g_strdup(entry_of(c->decimal));
    e->thousands = g_strdup(entry_of(c->thousands));
    e->grouping = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(c->grouping)));
    e->measurement = active_int(c->measurement);
    e->currency = g_strdup(entry_of(c->currency));
    e->positive_format = active_int(c->pos_format);
    e->negative_format = active_int(c->neg_format);
    e->mon_decimal = g_strdup(entry_of(c->mon_decimal));
    e->frac_digits = active_int(c->frac);
    e->mon_thousands = g_strdup(entry_of(c->mon_thousands));
    e->mon_grouping = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(c->mon_grouping)));
    e->time_format = g_strdup(entry_of(c->time_format));
    e->am = g_strdup(entry_of(c->am));
    e->pm = g_strdup(entry_of(c->pm));
    e->date_format = g_strdup(entry_of(c->date_format));
}

static void show_samples(customize *c) {
    xd_intl_custom e;
    read_effective(c, &e);
    time_t now = time(nullptr);
    char *s[] = {
        xd_intl_number_sample(&e, false),   xd_intl_number_sample(&e, true),
        xd_intl_currency_sample(&e, false), xd_intl_currency_sample(&e, true),
        xd_intl_time_sample(c->locale, &e, now), xd_intl_date_sample(c->locale, &e, now, false),
    };
    GtkWidget *w[] = {c->num_pos, c->num_neg, c->cur_pos, c->cur_neg, c->time_sample,
                      c->date_sample};
    for (size_t i = 0; i < G_N_ELEMENTS(s); i++) {
        gtk_entry_set_text(GTK_ENTRY(w[i]), s[i]);
        g_free(s[i]);
    }
    xd_intl_custom_clear(&e);
}

/* A separator field changed: the format follows. */
static void follow_separator(GtkWidget *sep, GtkWidget *format, char **was) {
    const char *now = entry_of(sep);
    /* Typing over it passes through "": the old one is still the one in
     * the format. */
    if (now[0] == '\0')
        return;
    if (*was != nullptr && (*was)[0] != '\0' && strcmp(*was, now) != 0) {
        char *f = xd_intl_picture_replace_separator(entry_of(format), *was, now);
        set_entry_of(format, f);
        g_free(f);
    }
    g_free(*was);
    *was = g_strdup(now);
}

/* A format changed: its separator field shows its separator. */
static void show_separator(GtkWidget *format, GtkWidget *sep, char **was) {
    char *s = xd_intl_picture_separator(entry_of(format));
    if (s[0] == '\0') { /* none (yet) */
        g_free(s);
        return;
    }
    if (strcmp(s, entry_of(sep)) != 0)
        set_entry_of(sep, s);
    g_free(*was);
    *was = s;
}

static void on_any(GtkWidget *w, gpointer data) {
    customize *c = data;
    if (c->updating)
        return;
    c->updating = true;
    if (w == c->time_sep)
        follow_separator(c->time_sep, c->time_format, &c->time_sep_was);
    else if (w == c->time_format)
        show_separator(c->time_format, c->time_sep, &c->time_sep_was);
    else if (w == c->date_sep)
        follow_separator(c->date_sep, c->date_format, &c->date_sep_was);
    else if (w == c->date_format)
        show_separator(c->date_format, c->date_sep, &c->date_sep_was);
    else if (w == c->thousands || w == c->mon_thousands) {
        GtkWidget *g = w == c->thousands ? c->grouping : c->mon_grouping;
        char *id = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(g)));
        const char *own = w == c->thousands ? c->base.grouping : c->base.mon_grouping;
        fill_grouping(g, id, grouping_id(own), entry_of(w));
        g_free(id);
    } else if (w == c->currency) {
        fill_formats(c->pos_format, false, active_int(c->pos_format), entry_of(c->currency));
        fill_formats(c->neg_format, true, active_int(c->neg_format), entry_of(c->currency));
    }
    c->updating = false;
    show_samples(c);
}

/* ---- the tabs ------------------------------------------------------------------------ */

static GtkWidget *numbers_page(customize *c, const xd_intl_custom *e) {
    GtkWidget *p = page();
    gtk_box_pack_start(GTK_BOX(p), samples_frame(&c->num_pos, "Positive:", &c->num_neg, "Negative:"),
                       FALSE, FALSE, 0);
    GtkWidget *g = grid_new();
    static const char *const decimals[] = {".", ",", nullptr};
    static const char *const separators[] = {",", ".", " ", "'", nullptr};
    c->decimal = editable(c, e->decimal, decimals, 3);
    row(g, 0, "_Decimal symbol:", c->decimal);
    c->thousands = editable(c, e->thousands, separators, 3);
    row(g, 1, "Digit grouping s_ymbol:", c->thousands);
    c->grouping = grouping_combo(c, e->grouping, e->thousands);
    row(g, 2, "Digit _grouping:", c->grouping);
    c->measurement = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c->measurement), "1", "Metric");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c->measurement), "2", "U.S.");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(c->measurement), e->measurement == 2 ? "2" : "1");
    g_signal_connect(c->measurement, "changed", G_CALLBACK(on_any), c);
    row(g, 3, "_Measurement system:", c->measurement);
    gtk_box_pack_start(GTK_BOX(p), g, FALSE, FALSE, 0);
    return p;
}

static GtkWidget *currency_page(customize *c, const xd_intl_custom *e) {
    GtkWidget *p = page();
    gtk_box_pack_start(GTK_BOX(p), samples_frame(&c->cur_pos, "Positive:", &c->cur_neg, "Negative:"),
                       FALSE, FALSE, 0);
    GtkWidget *g = grid_new();
    static const char *const symbols[] = {"$", "€", "£", "¥", nullptr};
    static const char *const decimals[] = {".", ",", nullptr};
    static const char *const separators[] = {",", ".", " ", "'", nullptr};
    c->currency = editable(c, e->currency, symbols, 5);
    row(g, 0, "Currency _symbol:", c->currency);
    c->pos_format = gtk_combo_box_text_new();
    fill_formats(c->pos_format, false, e->positive_format, e->currency);
    g_signal_connect(c->pos_format, "changed", G_CALLBACK(on_any), c);
    row(g, 1, "_Position of currency symbol:", c->pos_format);
    c->neg_format = gtk_combo_box_text_new();
    fill_formats(c->neg_format, true, e->negative_format, e->currency);
    g_signal_connect(c->neg_format, "changed", G_CALLBACK(on_any), c);
    row(g, 2, "_Negative currency format:", c->neg_format);
    c->mon_decimal = editable(c, e->mon_decimal, decimals, 3);
    row(g, 3, "_Decimal symbol:", c->mon_decimal);
    c->frac = gtk_combo_box_text_new();
    for (int i = 0; i <= 9; i++) {
        char id[4];
        g_snprintf(id, sizeof(id), "%d", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c->frac), id, id);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(c->frac), CLAMP(e->frac_digits, 0, 9));
    g_signal_connect(c->frac, "changed", G_CALLBACK(on_any), c);
    row(g, 4, "No. of digits _after decimal:", c->frac);
    c->mon_thousands = editable(c, e->mon_thousands, separators, 3);
    row(g, 5, "Digit grouping s_ymbol:", c->mon_thousands);
    c->mon_grouping = grouping_combo(c, e->mon_grouping, e->mon_thousands);
    row(g, 6, "Digit _grouping:", c->mon_grouping);
    gtk_box_pack_start(GTK_BOX(p), g, FALSE, FALSE, 0);
    return p;
}

static GtkWidget *legend(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
}

static GtkWidget *time_page(customize *c, const xd_intl_custom *e) {
    GtkWidget *p = page();
    gtk_box_pack_start(GTK_BOX(p), samples_frame(&c->time_sample, "Time sample:", nullptr, nullptr),
                       FALSE, FALSE, 0);
    GtkWidget *g = grid_new();
    static const char *const formats[] = {"h:mm:ss tt", "hh:mm:ss tt", "H:mm:ss", "HH:mm:ss",
                                          nullptr};
    static const char *const separators[] = {":", ".", nullptr};
    static const char *const ams[] = {"AM", nullptr};
    static const char *const pms[] = {"PM", nullptr};
    c->time_format = editable(c, e->time_format, formats, 40);
    row(g, 0, "_Time format:", c->time_format);
    c->time_sep_was = xd_intl_picture_separator(e->time_format);
    c->time_sep = editable(c, c->time_sep_was, separators, 3);
    row(g, 1, "Time _separator:", c->time_sep);
    c->am = editable(c, e->am, ams, 12);
    row(g, 2, "_AM symbol:", c->am);
    c->pm = editable(c, e->pm, pms, 12);
    row(g, 3, "_PM symbol:", c->pm);
    gtk_box_pack_start(GTK_BOX(p), g, FALSE, FALSE, 0);
    GtkWidget *f = gtk_frame_new(nullptr);
    GtkWidget *lg = legend("h = hour   m = minute   s = second   t = A or P\n\n"
                           "h/H = 12/24 hour\n"
                           "hh, mm, ss = leading zero\n"
                           "h, m, s = no leading zero");
    /* In a box: a frame's own label child would be styled as its title. */
    GtkWidget *inside = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(inside), 8);
    gtk_box_pack_start(GTK_BOX(inside), lg, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(f), inside);
    gtk_box_pack_start(GTK_BOX(p), f, FALSE, FALSE, 0);
    return p;
}

static GtkWidget *date_page(customize *c, const xd_intl_custom *e) {
    GtkWidget *p = page();
    GtkWidget *g = grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(g), 8);
    GtkWidget *f = gtk_frame_new("Short date");
    gtk_container_add(GTK_CONTAINER(f), g);
    static const char *const formats[] = {"M/d/yyyy", "M/d/yy",    "MM/dd/yy",  "MM/dd/yyyy",
                                          "yy/MM/dd", "yyyy-MM-dd", "dd-MMM-yy", "dd.MM.yyyy",
                                          "d.M.yyyy", "dd/MM/yyyy", nullptr};
    static const char *const separators[] = {"/", "-", ".", nullptr};
    c->date_sample = sample();
    GtkWidget *l = gtk_label_new("Short date sample:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(g), l, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g), c->date_sample, 1, 0, 1, 1);
    c->date_format = editable(c, e->date_format, formats, 40);
    row(g, 1, "S_hort date format:", c->date_format);
    c->date_sep_was = xd_intl_picture_separator(e->date_format);
    c->date_sep = editable(c, c->date_sep_was, separators, 3);
    row(g, 2, "_Date separator:", c->date_sep);
    gtk_box_pack_start(GTK_BOX(p), f, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p),
                       legend("d = day   M = month   y = year\n\n"
                              "d, M = no leading zero   dd, MM = leading zero\n"
                              "ddd, MMM = abbreviated name   dddd, MMMM = full name\n"
                              "yy = two-digit year   yyyy = four-digit year"),
                       FALSE, FALSE, 0);
    return p;
}

/* ---- keeping ------------------------------------------------------------------------ */

/* `value` if it isn't the locale's own (`own`). */
static char *changed_text(const char *value, const char *own) {
    return value != nullptr && g_strcmp0(value, own) != 0 ? g_strdup(value) : nullptr;
}

static bool keep(customize *c, xd_intl_custom *custom) {
    xd_intl_custom e;
    read_effective(c, &e);
    const char *problem = e.decimal[0] == '\0'       ? "Type a decimal symbol."
                          : e.mon_decimal[0] == '\0' ? "Type a decimal symbol for currency."
                          : e.time_format[0] == '\0' ? "Type a time format."
                          : e.date_format[0] == '\0' ? "Type a short date format."
                                                     : nullptr;
    if (problem != nullptr) {
        xd_message_box(GTK_WINDOW(c->dialog), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, problem);
        xd_intl_custom_clear(&e);
        return false;
    }
    const xd_intl_custom *b = &c->base;
    xd_intl_custom_clear(custom);
    custom->decimal = changed_text(e.decimal, b->decimal);
    custom->thousands = changed_text(e.thousands, b->thousands);
    custom->grouping = changed_text(grouping_id(e.grouping), grouping_id(b->grouping));
    custom->measurement = e.measurement != b->measurement ? e.measurement : 0;
    custom->currency = changed_text(e.currency, b->currency);
    custom->mon_decimal = changed_text(e.mon_decimal, b->mon_decimal);
    custom->mon_thousands = changed_text(e.mon_thousands, b->mon_thousands);
    custom->mon_grouping = changed_text(grouping_id(e.mon_grouping), grouping_id(b->mon_grouping));
    custom->frac_digits = e.frac_digits != b->frac_digits ? e.frac_digits : -1;
    custom->positive_format = e.positive_format != b->positive_format ? e.positive_format : -1;
    custom->negative_format = e.negative_format != b->negative_format ? e.negative_format : -1;
    custom->time_format = changed_text(e.time_format, b->time_format);
    custom->am = changed_text(e.am, b->am);
    custom->pm = changed_text(e.pm, b->pm);
    custom->date_format = changed_text(e.date_format, b->date_format);
    xd_intl_custom_clear(&e);
    return true;
}

void xd_intl_customize(GtkWindow *parent, const char *locale, xd_intl_custom *custom,
                       void (*applied)(void *data), void *data) {
    customize c = {.locale = locale};
    xd_intl_custom e;
    if (!xd_intl_custom_from_locale(locale, &c.base) ||
        !xd_intl_custom_effective(locale, custom, &e)) {
        xd_message_box(parent, TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "These formats aren't installed, so they can't be customized.");
        xd_intl_custom_clear(&c.base);
        return;
    }
    c.dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(c.dialog), TITLE);
    gtk_window_set_transient_for(GTK_WINDOW(c.dialog), parent);
    gtk_window_set_modal(GTK_WINDOW(c.dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(c.dialog), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(c.dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(c.dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(c.dialog), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(c.dialog), GTK_RESPONSE_OK);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    c.updating = true;
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), numbers_page(&c, &e),
                             gtk_label_new_with_mnemonic("_Numbers"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), currency_page(&c, &e),
                             gtk_label_new_with_mnemonic("_Currency"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), time_page(&c, &e),
                             gtk_label_new_with_mnemonic("_Time"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), date_page(&c, &e),
                             gtk_label_new_with_mnemonic("_Date"));
    c.updating = false;
    xd_intl_custom_clear(&e);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(c.dialog))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(c.dialog, GTK_NOTEBOOK(tabs));
    show_samples(&c);
    gtk_widget_show_all(c.dialog);
    for (int r; (r = gtk_dialog_run(GTK_DIALOG(c.dialog))) == GTK_RESPONSE_OK ||
                r == GTK_RESPONSE_APPLY;) {
        if (!keep(&c, custom))
            continue;
        applied(data);
        if (r == GTK_RESPONSE_OK)
            break;
    }
    gtk_widget_destroy(c.dialog);
    g_free(c.time_sep_was);
    g_free(c.date_sep_was);
    xd_intl_custom_clear(&c.base);
}
