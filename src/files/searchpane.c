/* SPDX-License-Identifier: MIT
 * Explorer's Search bar: XP's Search Companion and 2000's Search pane.
 * See searchpane.h.
 *
 * XP's Companion is a set of pages (a GtkStack): what to search for, the
 * criteria for all files, for pictures, music and video and for
 * documents, the search running, "Did you find what you wanted?", sorting
 * by category and the preferences. The criteria shared by its pages
 * (Look in, the dates, the size, More advanced options) are one block,
 * moved to whichever page shows them. No animated character: XP's were
 * Microsoft's art, and the Companion works without one.
 */
#include "searchpane.h"

#include <string.h>

#include "../settings/state.h"

#define HISTORY_FILE "search-names"
#define PREFERENCES_FILE "search-companion.ini"
enum { HISTORY_MAX = 50 };

typedef struct pane {
    xd_search_pane_style style;
    xd_search_pane_callbacks callbacks;
    GtkWidget *root, *stack;
    const char *criteria_page; /* the criteria page last shown */
    bool types_filled;
    bool autocomplete, advanced_behavior;
    GtkListStore *history; /* names searched, for AutoComplete */
    /* Names, on each criteria page. */
    GtkWidget *all_name, *media_name, *documents_name;
    GtkWidget *media_kinds[3], *media_advanced_check, *media_advanced_slot;
    GtkWidget *documents_when[4], *documents_advanced_check, *documents_advanced_slot;
    GtkWidget *all_slot;
    /* The block the criteria pages share. */
    GtkWidget *advanced;
    GtkWidget *contents_row, *contents;
    GtkWidget *look_in;
    GtkListStore *places; /* label, path */
    GtkWidget *dates_expander, *when[XD_SEARCH_WHEN_COUNT], *date_property, *date_from,
        *date_to;
    GtkWidget *size_expander, *size[4], *size_compare, *size_kb;
    GtkWidget *more_expander, *type, *system, *hidden, *subfolders, *case_sensitive, *regex;
    /* 2000's options. */
    GtkWidget *search_now, *stop_search, *options_link, *options_box;
    GtkWidget *date_check, *date_kind, *months_radio, *months, *days_radio, *days,
        *between_radio;
    GtkWidget *type_check, *size_check;
    GtkWidget *advanced_check, *advanced_box;
    /* Running and done. */
    GtkWidget *running_folder, *done_label, *autocomplete_link, *stop;
} pane;

enum { PLACE_LABEL, PLACE_PATH, PLACE_COUNT };

static pane *get_pane(GtkWidget *widget) {
    return g_object_get_data(G_OBJECT(widget), "xd-search-pane");
}

static void free_pane(gpointer data) {
    pane *search_pane = data;
    g_object_unref(search_pane->history);
    g_object_unref(search_pane->advanced);
    g_free(search_pane);
}

/* ---- building blocks ------------------------------------------------------------------- */

static GtkWidget *heading(const char *text) {
    GtkWidget *label = gtk_label_new(nullptr);
    char *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 28);
    return label;
}

static GtkWidget *text_label(const char *text) {
    GtkWidget *label = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
    return label;
}

/* XP's links: a line of text that's clicked, with an arrow before it. */
static GtkWidget *add_link(GtkWidget *box, const char *text, GCallback callback, gpointer data) {
    GtkWidget *button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name("go-next", GTK_ICON_SIZE_MENU),
                       FALSE, FALSE, 0);
    GtkWidget *label = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 28);
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(button), row);
    if (callback != nullptr)
        g_signal_connect(button, "clicked", callback, data);
    else
        gtk_widget_set_sensitive(button, FALSE);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    return button;
}

static GtkWidget *labelled_entry(GtkWidget *box, const char *text) {
    GtkWidget *label = text_label(text);
    GtkWidget *entry = gtk_entry_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    return entry;
}

static GtkWidget *page_box(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    return box;
}

static GtkWidget *radio(GtkWidget *box, GtkWidget *group, const char *text) {
    GtkWidget *button =
        group != nullptr ? gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(group),
                                                                         text)
                         : gtk_radio_button_new_with_mnemonic(nullptr, text);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    return button;
}

static GtkWidget *check(GtkWidget *box, const char *text, bool active) {
    GtkWidget *button = gtk_check_button_new_with_mnemonic(text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    return button;
}

static bool is_on(GtkWidget *toggle) {
    return toggle != nullptr && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
}

static void set_on(GtkWidget *toggle, bool on) {
    if (toggle != nullptr)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), on);
}

static char *today_text(int add_days) {
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *when = g_date_time_add_days(now, add_days);
    char *text = g_date_time_format(when, "%x");
    g_date_time_unref(when);
    g_date_time_unref(now);
    return text;
}

/* ---- preferences and AutoComplete ------------------------------------------------------- */

static GKeyFile *load_preferences(char **path_out) {
    xd_strbuf path = {0};
    xd_state_path(PREFERENCES_FILE, &path);
    GKeyFile *key_file = g_key_file_new();
    g_key_file_load_from_file(key_file, xd_strbuf_cstr(&path), G_KEY_FILE_NONE, nullptr);
    *path_out = g_strdup(xd_strbuf_cstr(&path));
    xd_strbuf_free(&path);
    return key_file;
}

static void save_preferences(pane *search_pane) {
    char *path = nullptr;
    GKeyFile *key_file = load_preferences(&path);
    g_key_file_set_boolean(key_file, "Search Companion", "AutoComplete",
                           search_pane->autocomplete);
    g_key_file_set_boolean(key_file, "Search Companion", "Advanced",
                           search_pane->advanced_behavior);
    g_key_file_save_to_file(key_file, path, nullptr);
    g_key_file_free(key_file);
    g_free(path);
}

static void read_preferences(pane *search_pane) {
    char *path = nullptr;
    GKeyFile *key_file = load_preferences(&path);
    GError *error = nullptr;
    search_pane->autocomplete =
        g_key_file_get_boolean(key_file, "Search Companion", "AutoComplete", &error);
    if (error != nullptr) {
        search_pane->autocomplete = true; /* XP's default */
        g_clear_error(&error);
    }
    search_pane->advanced_behavior =
        g_key_file_get_boolean(key_file, "Search Companion", "Advanced", nullptr);
    g_key_file_free(key_file);
    g_free(path);
}

static void load_history(pane *search_pane) {
    char **lines = nullptr;
    size_t count = 0;
    xd_state_read_lines(HISTORY_FILE, &lines, &count, HISTORY_MAX);
    for (size_t i = 0; i < count; i++)
        gtk_list_store_insert_with_values(search_pane->history, nullptr, -1, 0, lines[i], -1);
    xd_free_lines(lines, count);
}

/* A name searched for joins AutoComplete's list (most recent first). */
static void remember_name(pane *search_pane, const char *name) {
    if (name == nullptr || name[0] == '\0' || !search_pane->autocomplete)
        return;
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(lines, g_strdup(name));
    GtkTreeModel *model = GTK_TREE_MODEL(search_pane->history);
    GtkTreeIter iter;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter);
         more && lines->len < HISTORY_MAX; more = gtk_tree_model_iter_next(model, &iter)) {
        char *old = nullptr;
        gtk_tree_model_get(model, &iter, 0, &old, -1);
        if (g_strcmp0(old, name) != 0)
            g_ptr_array_add(lines, old);
        else
            g_free(old);
    }
    gtk_list_store_clear(search_pane->history);
    for (guint i = 0; i < lines->len; i++)
        gtk_list_store_insert_with_values(search_pane->history, nullptr, -1, 0,
                                          g_ptr_array_index(lines, i), -1);
    xd_state_write_lines(HISTORY_FILE, (char *const *)lines->pdata, lines->len);
    g_ptr_array_unref(lines);
}

static void set_completion(pane *search_pane, GtkWidget *entry) {
    if (entry == nullptr)
        return;
    if (!search_pane->autocomplete) {
        gtk_entry_set_completion(GTK_ENTRY(entry), nullptr);
        return;
    }
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(search_pane->history));
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_set_completion(GTK_ENTRY(entry), completion);
    g_object_unref(completion);
}

static void apply_autocomplete(pane *search_pane) {
    set_completion(search_pane, search_pane->all_name);
    set_completion(search_pane, search_pane->media_name);
    set_completion(search_pane, search_pane->documents_name);
    if (search_pane->autocomplete_link != nullptr) {
        GtkWidget *row = gtk_bin_get_child(GTK_BIN(search_pane->autocomplete_link));
        GList *children = gtk_container_get_children(GTK_CONTAINER(row));
        GtkWidget *label = g_list_last(children)->data;
        gtk_label_set_text_with_mnemonic(GTK_LABEL(label), search_pane->autocomplete
                                                               ? "Turn Auto_Complete off"
                                                               : "Turn Auto_Complete on");
        g_list_free(children);
    }
}

/* ---- Look in, Type of file ---------------------------------------------------------------- */

static void add_place(pane *search_pane, const char *label, const char *path, bool select) {
    GtkTreeModel *model = GTK_TREE_MODEL(search_pane->places);
    GtkTreeIter iter;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more;
         more = gtk_tree_model_iter_next(model, &iter)) {
        char *known = nullptr;
        gtk_tree_model_get(model, &iter, PLACE_PATH, &known, -1);
        bool same = g_strcmp0(known, path) == 0;
        g_free(known);
        if (same) {
            if (select)
                gtk_combo_box_set_active_iter(GTK_COMBO_BOX(search_pane->look_in), &iter);
            return;
        }
    }
    /* Before "Browse...", the last. */
    int count = gtk_tree_model_iter_n_children(model, nullptr);
    gtk_list_store_insert_with_values(search_pane->places, &iter, MAX(count - 1, 0), PLACE_LABEL,
                                      label, PLACE_PATH, path, -1);
    if (select)
        gtk_combo_box_set_active_iter(GTK_COMBO_BOX(search_pane->look_in), &iter);
}

static void on_look_in_changed(GtkComboBox *combo, gpointer data) {
    pane *search_pane = data;
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(combo, &iter))
        return;
    char *path = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(search_pane->places), &iter, PLACE_PATH, &path, -1);
    bool browse = path == nullptr;
    g_free(path);
    if (!browse)
        return;
    GtkWidget *top = gtk_widget_get_toplevel(search_pane->root);
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Browse For Folder", GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel", GTK_RESPONSE_CANCEL, "OK",
        GTK_RESPONSE_ACCEPT, nullptr);
    char *chosen = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    gtk_widget_destroy(dialog);
    if (chosen != nullptr) {
        char *name = g_filename_display_basename(chosen);
        add_place(search_pane, name, chosen, true);
        g_free(name);
        g_free(chosen);
    } else {
        gtk_combo_box_set_active(combo, 0);
    }
}

static GtkWidget *look_in_combo(pane *search_pane) {
    search_pane->places = gtk_list_store_new(PLACE_COUNT, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(search_pane->places));
    g_object_unref(search_pane->places);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(combo), text, "text", PLACE_LABEL);
    search_pane->look_in = combo;
    gtk_list_store_insert_with_values(search_pane->places, nullptr, -1, PLACE_LABEL, "Browse...",
                                      PLACE_PATH, nullptr, -1);
    const char *documents = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    add_place(search_pane, "My Documents", documents != nullptr ? documents : g_get_home_dir(),
              false);
    const char *desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (desktop != nullptr)
        add_place(search_pane, "Desktop", desktop, false);
    add_place(search_pane, "Local Hard Drives", "/", false);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_signal_connect(combo, "changed", G_CALLBACK(on_look_in_changed), search_pane);
    return combo;
}

typedef struct type_row {
    char *description, *content_type;
} type_row;

static gint compare_type_rows(gconstpointer left, gconstpointer right) {
    const type_row *first = left, *second = right;
    return g_utf8_collate(first->description, second->description);
}

/* "Type of file": shared-mime-info's types by their description, filled
 * when first wanted (there are many). */
static void fill_types(pane *search_pane) {
    if (search_pane->types_filled)
        return;
    search_pane->types_filled = true;
    GList *types = g_content_types_get_registered();
    GArray *rows = g_array_new(FALSE, FALSE, sizeof(type_row));
    for (GList *node = types; node != nullptr; node = node->next) {
        const char *content_type = node->data;
        if (g_str_has_prefix(content_type, "x-content/") ||
            g_str_has_prefix(content_type, "inode/") || strchr(content_type, '/') == nullptr)
            continue;
        type_row row = {g_content_type_get_description(content_type), g_strdup(content_type)};
        g_array_append_val(rows, row);
    }
    g_list_free_full(types, g_free);
    g_array_sort(rows, compare_type_rows);
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(search_pane->type);
    const char *last = nullptr;
    for (guint i = 0; i < rows->len; i++) {
        type_row *row = &g_array_index(rows, type_row, i);
        if (last == nullptr || strcmp(last, row->description) != 0)
            gtk_combo_box_text_append(combo, row->content_type, row->description);
        last = row->description;
    }
    for (guint i = 0; i < rows->len; i++) {
        g_free(g_array_index(rows, type_row, i).description);
        g_free(g_array_index(rows, type_row, i).content_type);
    }
    g_array_unref(rows);
}

static void on_types_wanted([[maybe_unused]] GObject *object, [[maybe_unused]] GParamSpec *spec,
                            gpointer data) {
    fill_types(data);
}

static GtkWidget *type_combo(pane *search_pane) {
    search_pane->type = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(search_pane->type), "",
                              "(All files and folders)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(search_pane->type), 0);
    gtk_combo_box_set_wrap_width(GTK_COMBO_BOX(search_pane->type), 0);
    g_signal_connect(search_pane->type, "notify::popup-shown", G_CALLBACK(on_types_wanted),
                     search_pane);
    return search_pane->type;
}

/* ---- gathering the criteria ---------------------------------------------------------------- */

static const char *entry_text(GtkWidget *entry) {
    return entry != nullptr ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
}

static char *active_place(pane *search_pane) {
    GtkTreeIter iter;
    char *path = nullptr;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(search_pane->look_in), &iter))
        gtk_tree_model_get(GTK_TREE_MODEL(search_pane->places), &iter, PLACE_PATH, &path, -1);
    return path != nullptr ? path : g_strdup(g_get_home_dir());
}

static void read_shared(pane *search_pane, xd_search_criteria *criteria, bool with_dates) {
    criteria->look_in = g_new0(char *, 2);
    criteria->look_in[0] = active_place(search_pane);
    const char *type = gtk_combo_box_get_active_id(GTK_COMBO_BOX(search_pane->type));
    if (type != nullptr && type[0] != '\0' &&
        (search_pane->style == XD_SEARCH_PANE_XP || is_on(search_pane->type_check)))
        criteria->content_type = g_strdup(type);
    criteria->subfolders = is_on(search_pane->subfolders);
    criteria->case_sensitive = is_on(search_pane->case_sensitive);
    criteria->regex = is_on(search_pane->regex);
    criteria->hidden = search_pane->hidden != nullptr && is_on(search_pane->hidden);
    criteria->system = search_pane->system == nullptr || is_on(search_pane->system);
    int property = gtk_combo_box_get_active(GTK_COMBO_BOX(search_pane->date_property));
    criteria->date_property = property == 1   ? XD_SEARCH_PROP_CREATED
                              : property == 2 ? XD_SEARCH_PROP_ACCESSED
                                              : XD_SEARCH_PROP_MODIFIED;
    if (search_pane->style == XD_SEARCH_PANE_2000) {
        if (is_on(search_pane->date_check)) {
            criteria->when = XD_SEARCH_WHEN_DATES;
            if (is_on(search_pane->between_radio)) {
                criteria->date_from = g_strdup(entry_text(search_pane->date_from));
                criteria->date_to = g_strdup(entry_text(search_pane->date_to));
            } else if (is_on(search_pane->months_radio)) {
                int months = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(search_pane->months));
                GDateTime *now = g_date_time_new_now_local();
                GDateTime *since = g_date_time_add_months(now, -months);
                criteria->date_from = g_date_time_format(since, "%Y-%m-%d");
                g_date_time_unref(since);
                g_date_time_unref(now);
            } else {
                int days = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(search_pane->days));
                GDateTime *now = g_date_time_new_now_local();
                GDateTime *since = g_date_time_add_days(now, -days);
                criteria->date_from = g_date_time_format(since, "%Y-%m-%d");
                g_date_time_unref(since);
                g_date_time_unref(now);
            }
        }
        if (is_on(search_pane->size_check)) {
            criteria->size = gtk_combo_box_get_active(GTK_COMBO_BOX(search_pane->size_compare)) == 1
                                 ? XD_SEARCH_SIZE_AT_MOST
                                 : XD_SEARCH_SIZE_AT_LEAST;
            criteria->size_kb = (uint64_t)gtk_spin_button_get_value_as_int(
                GTK_SPIN_BUTTON(search_pane->size_kb));
        }
        if (!is_on(search_pane->advanced_check)) {
            criteria->subfolders = true;
            criteria->case_sensitive = false;
            criteria->regex = false;
        }
        return;
    }
    if (with_dates) {
        for (int i = 0; i < XD_SEARCH_WHEN_COUNT; i++)
            if (is_on(search_pane->when[i]))
                criteria->when = (xd_search_when)i;
        if (criteria->when == XD_SEARCH_WHEN_DATES) {
            criteria->date_from = g_strdup(entry_text(search_pane->date_from));
            criteria->date_to = g_strdup(entry_text(search_pane->date_to));
        }
    }
    static const xd_search_size sizes[] = {XD_SEARCH_SIZE_ANY, XD_SEARCH_SIZE_SMALL,
                                           XD_SEARCH_SIZE_MEDIUM, XD_SEARCH_SIZE_LARGE};
    for (size_t i = 0; i < G_N_ELEMENTS(sizes); i++)
        if (is_on(search_pane->size[i]))
            criteria->size = sizes[i];
    if (gtk_widget_is_sensitive(search_pane->size_kb)) {
        criteria->size = gtk_combo_box_get_active(GTK_COMBO_BOX(search_pane->size_compare)) == 1
                             ? XD_SEARCH_SIZE_AT_MOST
                             : XD_SEARCH_SIZE_AT_LEAST;
        criteria->size_kb =
            (uint64_t)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(search_pane->size_kb));
    }
}

static void read_criteria(pane *search_pane, xd_search_criteria *criteria) {
    xd_search_criteria_init(criteria);
    const char *page = search_pane->criteria_page;
    if (search_pane->style == XD_SEARCH_PANE_2000 || g_strcmp0(page, "all") == 0) {
        criteria->name = g_strdup(entry_text(search_pane->all_name));
        criteria->contents = g_strdup(entry_text(search_pane->contents));
        read_shared(search_pane, criteria, true);
        return;
    }
    if (g_strcmp0(page, "media") == 0) {
        static const xd_search_kind kinds[] = {XD_SEARCH_KIND_PICTURE, XD_SEARCH_KIND_MUSIC,
                                               XD_SEARCH_KIND_VIDEO};
        for (size_t i = 0; i < G_N_ELEMENTS(kinds); i++)
            if (is_on(search_pane->media_kinds[i]))
                criteria->kinds |= kinds[i];
        /* None ticked: all three, as XP searched. */
        if (criteria->kinds == 0)
            criteria->kinds = XD_SEARCH_KIND_PICTURE | XD_SEARCH_KIND_MUSIC | XD_SEARCH_KIND_VIDEO;
        criteria->name = g_strdup(entry_text(search_pane->media_name));
        if (is_on(search_pane->media_advanced_check))
            read_shared(search_pane, criteria, true);
        else {
            criteria->look_in = g_new0(char *, 2);
            criteria->look_in[0] = active_place(search_pane);
        }
        return;
    }
    criteria->kinds = XD_SEARCH_KIND_DOCUMENT;
    criteria->name = g_strdup(entry_text(search_pane->documents_name));
    bool advanced = is_on(search_pane->documents_advanced_check);
    if (advanced) {
        criteria->contents = g_strdup(entry_text(search_pane->contents));
        read_shared(search_pane, criteria, false);
    } else {
        criteria->look_in = g_new0(char *, 2);
        criteria->look_in[0] = active_place(search_pane);
    }
    static const xd_search_when whens[] = {XD_SEARCH_WHEN_ANY, XD_SEARCH_WHEN_WEEK,
                                           XD_SEARCH_WHEN_MONTH, XD_SEARCH_WHEN_YEAR};
    for (size_t i = 0; i < G_N_ELEMENTS(whens); i++)
        if (is_on(search_pane->documents_when[i]))
            criteria->when = whens[i];
}

static void start_search(pane *search_pane) {
    xd_search_criteria criteria;
    read_criteria(search_pane, &criteria);
    remember_name(search_pane, criteria.name);
    if (search_pane->callbacks.search != nullptr)
        search_pane->callbacks.search(&criteria, search_pane->callbacks.data);
    xd_search_criteria_clear(&criteria);
}

/* ---- XP's pages --------------------------------------------------------------------------- */

static void show_page(pane *search_pane, const char *name) {
    gtk_stack_set_visible_child_name(GTK_STACK(search_pane->stack), name);
}

/* The shared block into this page's slot. */
static void place_advanced(pane *search_pane, GtkWidget *slot, bool contents, bool dates) {
    GtkWidget *parent = gtk_widget_get_parent(search_pane->advanced);
    if (parent != slot) {
        if (parent != nullptr)
            gtk_container_remove(GTK_CONTAINER(parent), search_pane->advanced);
        gtk_box_pack_start(GTK_BOX(slot), search_pane->advanced, FALSE, FALSE, 0);
    }
    gtk_widget_set_visible(search_pane->contents_row, contents);
    gtk_widget_set_visible(search_pane->dates_expander, dates);
    gtk_widget_show(search_pane->advanced);
}

static void update_media(pane *search_pane) {
    gtk_widget_set_visible(search_pane->media_advanced_slot,
                           is_on(search_pane->media_advanced_check));
}

static void update_documents(pane *search_pane) {
    gtk_widget_set_visible(search_pane->documents_advanced_slot,
                           is_on(search_pane->documents_advanced_check));
}

static void open_criteria(pane *search_pane, const char *page) {
    search_pane->criteria_page = page;
    if (strcmp(page, "all") == 0) {
        place_advanced(search_pane, search_pane->all_slot, true, true);
        show_page(search_pane, page);
        gtk_widget_grab_focus(search_pane->all_name);
    } else if (strcmp(page, "media") == 0) {
        place_advanced(search_pane, search_pane->media_advanced_slot, false, true);
        update_media(search_pane);
        show_page(search_pane, page);
        gtk_widget_grab_focus(search_pane->media_name);
    } else {
        place_advanced(search_pane, search_pane->documents_advanced_slot, true, false);
        update_documents(search_pane);
        show_page(search_pane, page);
        gtk_widget_grab_focus(search_pane->documents_name);
    }
}

static void on_media([[maybe_unused]] GtkButton *button, gpointer data) {
    open_criteria(data, "media");
}
static void on_documents([[maybe_unused]] GtkButton *button, gpointer data) {
    open_criteria(data, "documents");
}
static void on_all([[maybe_unused]] GtkButton *button, gpointer data) {
    open_criteria(data, "all");
}
static void on_back_to_start([[maybe_unused]] GtkButton *button, gpointer data) {
    pane *search_pane = data;
    show_page(search_pane, search_pane->advanced_behavior ? "all" : "start");
}
static void on_preferences([[maybe_unused]] GtkButton *button, gpointer data) {
    show_page(data, "preferences");
}
static void on_search_clicked([[maybe_unused]] GtkWidget *widget, gpointer data) {
    start_search(data);
}
static void on_stop_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    pane *search_pane = data;
    if (search_pane->callbacks.stop != nullptr)
        search_pane->callbacks.stop(search_pane->callbacks.data);
}
static void on_close_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    pane *search_pane = data;
    if (search_pane->callbacks.close != nullptr)
        search_pane->callbacks.close(search_pane->callbacks.data);
}
static void on_new_search([[maybe_unused]] GtkButton *button, gpointer data) {
    pane *search_pane = data;
    xd_search_pane_reset(search_pane->root);
    if (search_pane->callbacks.new_search != nullptr)
        search_pane->callbacks.new_search(search_pane->callbacks.data);
}
static void on_refine([[maybe_unused]] GtkButton *button, gpointer data) {
    pane *search_pane = data;
    open_criteria(search_pane, search_pane->criteria_page != nullptr ? search_pane->criteria_page
                                                                     : "all");
}
static void on_refine_options([[maybe_unused]] GtkButton *button, gpointer data) {
    pane *search_pane = data;
    on_refine(button, data);
    gtk_expander_set_expanded(GTK_EXPANDER(search_pane->more_expander), TRUE);
    if (strcmp(search_pane->criteria_page, "all") != 0) {
        set_on(search_pane->media_advanced_check, true);
        set_on(search_pane->documents_advanced_check, true);
    }
}
static void on_sort_page([[maybe_unused]] GtkButton *button, gpointer data) {
    show_page(data, "sort");
}
static void on_sort_by(GtkButton *button, gpointer data) {
    pane *search_pane = data;
    int column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "xd-column"));
    if (search_pane->callbacks.sort != nullptr)
        search_pane->callbacks.sort((XdSortColumn)column, search_pane->callbacks.data);
    show_page(search_pane, "done");
}
static void on_back_to_done([[maybe_unused]] GtkButton *button, gpointer data) {
    show_page(data, "done");
}
static void on_toggle_autocomplete([[maybe_unused]] GtkButton *button, gpointer data) {
    pane *search_pane = data;
    search_pane->autocomplete = !search_pane->autocomplete;
    save_preferences(search_pane);
    apply_autocomplete(search_pane);
}
static void on_behavior([[maybe_unused]] GtkButton *button, gpointer data) {
    show_page(data, "behavior");
}
static void on_behavior_chosen(GtkToggleButton *toggle, gpointer data) {
    pane *search_pane = data;
    if (!gtk_toggle_button_get_active(toggle))
        return;
    search_pane->advanced_behavior =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(toggle), "xd-advanced")) != 0;
    save_preferences(search_pane);
}
static void on_media_toggle([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    update_media(data);
}
static void on_documents_toggle([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    update_documents(data);
}

static void on_specify_size(GtkToggleButton *toggle, gpointer data) {
    pane *search_pane = data;
    bool on = gtk_toggle_button_get_active(toggle);
    gtk_widget_set_sensitive(search_pane->size_compare, on);
    gtk_widget_set_sensitive(search_pane->size_kb, on);
}

static void on_specify_dates(GtkToggleButton *toggle, gpointer data) {
    pane *search_pane = data;
    bool on = gtk_toggle_button_get_active(toggle);
    gtk_widget_set_sensitive(search_pane->date_property, on);
    gtk_widget_set_sensitive(search_pane->date_from, on);
    gtk_widget_set_sensitive(search_pane->date_to, on);
}

static GtkWidget *buttons_row(pane *search_pane, bool back) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(row, 6);
    GtkWidget *search = gtk_button_new_with_mnemonic("Sea_rch");
    g_signal_connect(search, "clicked", G_CALLBACK(on_search_clicked), search_pane);
    gtk_box_pack_end(GTK_BOX(row), search, FALSE, FALSE, 0);
    if (back) {
        GtkWidget *back_button = gtk_button_new_with_mnemonic("_Back");
        g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_to_start), search_pane);
        gtk_box_pack_end(GTK_BOX(row), back_button, FALSE, FALSE, 0);
    }
    return row;
}

static GtkWidget *date_entry(int add_days) {
    GtkWidget *entry = gtk_entry_new();
    char *text = today_text(add_days);
    gtk_entry_set_text(GTK_ENTRY(entry), text);
    g_free(text);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 11);
    return entry;
}

static GtkWidget *date_property_combo(void) {
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Modified Date");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Created Date");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Accessed Date");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    return combo;
}

/* Look in, When was it modified?, What size is it?, More advanced
 * options: the block XP's criteria pages share. */
static void build_advanced(pane *search_pane) {
    GtkWidget *block = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    search_pane->advanced = g_object_ref_sink(block);
    search_pane->contents_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    search_pane->contents = labelled_entry(search_pane->contents_row, "A _word or phrase in the file:");
    g_signal_connect(search_pane->contents, "activate", G_CALLBACK(on_search_clicked), search_pane);
    gtk_box_pack_start(GTK_BOX(block), search_pane->contents_row, FALSE, FALSE, 0);
    GtkWidget *look = text_label("_Look in:");
    gtk_box_pack_start(GTK_BOX(block), look, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(block), look_in_combo(search_pane), FALSE, FALSE, 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(look), search_pane->look_in);

    GtkWidget *dates = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    static const char *const when_labels[XD_SEARCH_WHEN_COUNT] = {
        "Don't remember", "Within the last week", "Past month", "Within the past year",
        "Specify dates"};
    for (int i = 0; i < XD_SEARCH_WHEN_COUNT; i++)
        search_pane->when[i] = radio(dates, i > 0 ? search_pane->when[0] : nullptr, when_labels[i]);
    GtkWidget *specify = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(specify, 20);
    search_pane->date_property = date_property_combo();
    gtk_box_pack_start(GTK_BOX(specify), search_pane->date_property, FALSE, FALSE, 0);
    GtkWidget *from_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(from_row), gtk_label_new("from"), FALSE, FALSE, 0);
    search_pane->date_from = date_entry(-30);
    gtk_box_pack_start(GTK_BOX(from_row), search_pane->date_from, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(specify), from_row, FALSE, FALSE, 0);
    GtkWidget *to_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(to_row), gtk_label_new("to"), FALSE, FALSE, 0);
    search_pane->date_to = date_entry(0);
    gtk_box_pack_start(GTK_BOX(to_row), search_pane->date_to, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(specify), to_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dates), specify, FALSE, FALSE, 0);
    g_signal_connect(search_pane->when[XD_SEARCH_WHEN_DATES], "toggled",
                     G_CALLBACK(on_specify_dates), search_pane);
    on_specify_dates(GTK_TOGGLE_BUTTON(search_pane->when[XD_SEARCH_WHEN_DATES]), search_pane);
    search_pane->dates_expander = gtk_expander_new_with_mnemonic("When was it _modified?");
    gtk_container_add(GTK_CONTAINER(search_pane->dates_expander), dates);
    gtk_box_pack_start(GTK_BOX(block), search_pane->dates_expander, FALSE, FALSE, 0);

    GtkWidget *sizes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    static const char *const size_labels[] = {"Don't remember", "Small (less than 100 KB)",
                                              "Medium (less than 1 MB)", "Large (more than 1 MB)"};
    for (size_t i = 0; i < G_N_ELEMENTS(size_labels); i++)
        search_pane->size[i] = radio(sizes, i > 0 ? search_pane->size[0] : nullptr, size_labels[i]);
    GtkWidget *specify_size = radio(sizes, search_pane->size[0], "Specify size (in KB)");
    GtkWidget *size_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(size_row, 20);
    search_pane->size_compare = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_pane->size_compare), "at least");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_pane->size_compare), "at most");
    gtk_combo_box_set_active(GTK_COMBO_BOX(search_pane->size_compare), 0);
    gtk_box_pack_start(GTK_BOX(size_row), search_pane->size_compare, FALSE, FALSE, 0);
    search_pane->size_kb = gtk_spin_button_new_with_range(0, 1e9, 1);
    gtk_box_pack_start(GTK_BOX(size_row), search_pane->size_kb, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sizes), size_row, FALSE, FALSE, 0);
    g_signal_connect(specify_size, "toggled", G_CALLBACK(on_specify_size), search_pane);
    on_specify_size(GTK_TOGGLE_BUTTON(specify_size), search_pane);
    search_pane->size_expander = gtk_expander_new_with_mnemonic("What _size is it?");
    gtk_container_add(GTK_CONTAINER(search_pane->size_expander), sizes);
    gtk_box_pack_start(GTK_BOX(block), search_pane->size_expander, FALSE, FALSE, 0);

    GtkWidget *more = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *type_label = text_label("Type of file:");
    gtk_box_pack_start(GTK_BOX(more), type_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(more), type_combo(search_pane), FALSE, FALSE, 0);
    search_pane->system = check(more, "Search s_ystem folders", true);
    search_pane->hidden = check(more, "Search _hidden files and folders", false);
    search_pane->subfolders = check(more, "Search su_bfolders", true);
    search_pane->case_sensitive = check(more, "_Case sensitive", false);
    /* Not XP's: the name and phrase are regular expressions (PCRE). */
    search_pane->regex = check(more, "Use regular e_xpressions", false);
    gtk_widget_set_tooltip_text(search_pane->regex,
                                "The file name and the word or phrase are regular expressions "
                                "(Perl's syntax), matched anywhere in the name or the file.");
    search_pane->more_expander = gtk_expander_new_with_mnemonic("More advanced _options");
    gtk_container_add(GTK_CONTAINER(search_pane->more_expander), more);
    gtk_box_pack_start(GTK_BOX(block), search_pane->more_expander, FALSE, FALSE, 0);
    gtk_widget_show_all(block);
}

static GtkWidget *name_entry(pane *search_pane, GtkWidget *box, const char *text) {
    GtkWidget *entry = labelled_entry(box, text);
    g_signal_connect(entry, "activate", G_CALLBACK(on_search_clicked), search_pane);
    return entry;
}

static void build_xp(pane *search_pane) {
    GtkStack *stack = GTK_STACK(search_pane->stack);
    /* What do you want to search for? */
    GtkWidget *start = page_box();
    gtk_box_pack_start(GTK_BOX(start), heading("What do you want to search for?"), FALSE, FALSE,
                       0);
    add_link(start, "_Pictures, music, or video", G_CALLBACK(on_media), search_pane);
    add_link(start, "_Documents (word processing, spreadsheet, etc.)", G_CALLBACK(on_documents),
         search_pane);
    add_link(start, "_All files and folders", G_CALLBACK(on_all), search_pane);
    /* No Active Directory or address book to search. */
    add_link(start, "Computers or _people", nullptr, nullptr);
    gtk_box_pack_start(GTK_BOX(start), heading("You may also want to..."), FALSE, FALSE, 6);
    add_link(start, "Change pr_eferences", G_CALLBACK(on_preferences), search_pane);
    gtk_stack_add_named(stack, start, "start");

    /* All files and folders. */
    GtkWidget *all = page_box();
    gtk_box_pack_start(GTK_BOX(all), heading("Search by any or all of the criteria below."), FALSE,
                       FALSE, 0);
    search_pane->all_name = name_entry(search_pane, all, "All or part of the file _name:");
    search_pane->all_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(all), search_pane->all_slot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(all), buttons_row(search_pane, true), FALSE, FALSE, 0);
    gtk_stack_add_named(stack, all, "all");

    /* Pictures, music, or video. */
    GtkWidget *media = page_box();
    gtk_box_pack_start(GTK_BOX(media),
                       heading("Search for all files of a certain type, or search by type and "
                               "name."),
                       FALSE, FALSE, 0);
    search_pane->media_kinds[0] = check(media, "Pictures and Photos", false);
    search_pane->media_kinds[1] = check(media, "Music", false);
    search_pane->media_kinds[2] = check(media, "Video", false);
    search_pane->media_name = name_entry(search_pane, media, "All or part of the file _name:");
    search_pane->media_advanced_check = check(media, "Use ad_vanced search options", false);
    g_signal_connect(search_pane->media_advanced_check, "toggled", G_CALLBACK(on_media_toggle),
                     search_pane);
    search_pane->media_advanced_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_no_show_all(search_pane->media_advanced_slot, TRUE);
    gtk_box_pack_start(GTK_BOX(media), search_pane->media_advanced_slot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(media), buttons_row(search_pane, true), FALSE, FALSE, 0);
    gtk_stack_add_named(stack, media, "media");

    /* Documents. */
    GtkWidget *documents = page_box();
    gtk_box_pack_start(GTK_BOX(documents), heading("Last time it was modified:"), FALSE, FALSE, 0);
    static const char *const when_labels[] = {"Don't remember", "Within the last week",
                                              "Past month", "Within the past year"};
    for (size_t i = 0; i < G_N_ELEMENTS(when_labels); i++)
        search_pane->documents_when[i] = radio(
            documents, i > 0 ? search_pane->documents_when[0] : nullptr, when_labels[i]);
    search_pane->documents_name =
        name_entry(search_pane, documents, "All or part of the document _name:");
    search_pane->documents_advanced_check =
        check(documents, "Use ad_vanced search options", false);
    g_signal_connect(search_pane->documents_advanced_check, "toggled",
                     G_CALLBACK(on_documents_toggle), search_pane);
    search_pane->documents_advanced_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_no_show_all(search_pane->documents_advanced_slot, TRUE);
    gtk_box_pack_start(GTK_BOX(documents), search_pane->documents_advanced_slot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(documents), buttons_row(search_pane, true), FALSE, FALSE, 0);
    gtk_stack_add_named(stack, documents, "documents");

    /* Running. */
    GtkWidget *running = page_box();
    gtk_box_pack_start(GTK_BOX(running), heading("Searching..."), FALSE, FALSE, 0);
    search_pane->running_folder = text_label("");
    gtk_label_set_ellipsize(GTK_LABEL(search_pane->running_folder), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_line_wrap(GTK_LABEL(search_pane->running_folder), FALSE);
    gtk_box_pack_start(GTK_BOX(running), search_pane->running_folder, FALSE, FALSE, 0);
    GtkWidget *stop_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    search_pane->stop = gtk_button_new_with_mnemonic("_Stop");
    g_signal_connect(search_pane->stop, "clicked", G_CALLBACK(on_stop_clicked), search_pane);
    gtk_box_pack_end(GTK_BOX(stop_row), search_pane->stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(running), stop_row, FALSE, FALSE, 0);
    gtk_stack_add_named(stack, running, "running");

    /* Did you find what you wanted? */
    GtkWidget *done = page_box();
    search_pane->done_label = text_label("");
    gtk_box_pack_start(GTK_BOX(done), search_pane->done_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(done), heading("Did you find what you wanted?"), FALSE, FALSE, 4);
    add_link(done, "_Yes, finished searching", G_CALLBACK(on_close_clicked), search_pane);
    gtk_box_pack_start(GTK_BOX(done), text_label("No, refine this search:"), FALSE, FALSE, 0);
    GtkWidget *refine = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(refine, 12);
    add_link(refine, "Change file _name or keywords", G_CALLBACK(on_refine), search_pane);
    add_link(refine, "_Look in more locations", G_CALLBACK(on_refine), search_pane);
    add_link(refine, "Change whether _hidden and system files are included",
         G_CALLBACK(on_refine_options), search_pane);
    add_link(refine, "Change which file _types are included", G_CALLBACK(on_refine_options),
         search_pane);
    gtk_box_pack_start(GTK_BOX(done), refine, FALSE, FALSE, 0);
    add_link(done, "S_ort results by category", G_CALLBACK(on_sort_page), search_pane);
    add_link(done, "Start a ne_w search", G_CALLBACK(on_new_search), search_pane);
    gtk_stack_add_named(stack, done, "done");

    /* Sort results by category. */
    GtkWidget *sort = page_box();
    gtk_box_pack_start(GTK_BOX(sort), heading("Sort results by category:"), FALSE, FALSE, 0);
    static const struct {
        const char *label;
        XdSortColumn column;
    } sorts[] = {{"_Name", XD_SORT_NAME},
                 {"_In Folder", XD_SORT_FOLDER},
                 {"_Size", XD_SORT_SIZE},
                 {"_Type", XD_SORT_TYPE},
                 {"Date _Modified", XD_SORT_MODIFIED}};
    for (size_t i = 0; i < G_N_ELEMENTS(sorts); i++) {
        GtkWidget *button = add_link(sort, sorts[i].label, G_CALLBACK(on_sort_by), search_pane);
        g_object_set_data(G_OBJECT(button), "xd-column", GINT_TO_POINTER(sorts[i].column));
    }
    add_link(sort, "_Back", G_CALLBACK(on_back_to_done), search_pane);
    gtk_stack_add_named(stack, sort, "sort");

    /* Preferences. */
    GtkWidget *preferences = page_box();
    gtk_box_pack_start(GTK_BOX(preferences), heading("How do you want to use Search Companion?"),
                       FALSE, FALSE, 0);
    /* There's no character to turn off: shown, but nothing to do. */
    add_link(preferences, "Without an animated screen character", nullptr, nullptr);
    search_pane->autocomplete_link =
        add_link(preferences, "Turn Auto_Complete off", G_CALLBACK(on_toggle_autocomplete),
             search_pane);
    add_link(preferences, "Change files and folders search _behavior", G_CALLBACK(on_behavior),
         search_pane);
    add_link(preferences, "_Back", G_CALLBACK(on_back_to_start), search_pane);
    gtk_stack_add_named(stack, preferences, "preferences");

    GtkWidget *behavior = page_box();
    gtk_box_pack_start(GTK_BOX(behavior),
                       heading("How do you want to search for files and folders?"), FALSE, FALSE,
                       0);
    GtkWidget *standard = radio(behavior, nullptr, "_Standard - with prompts that guide you");
    GtkWidget *advanced = radio(behavior, standard, "_Advanced - type the criteria yourself");
    set_on(search_pane->advanced_behavior ? advanced : standard, true);
    g_object_set_data(G_OBJECT(advanced), "xd-advanced", GINT_TO_POINTER(1));
    g_signal_connect(standard, "toggled", G_CALLBACK(on_behavior_chosen), search_pane);
    g_signal_connect(advanced, "toggled", G_CALLBACK(on_behavior_chosen), search_pane);
    add_link(behavior, "_Back", G_CALLBACK(on_preferences), search_pane);
    gtk_stack_add_named(stack, behavior, "behavior");
}

/* ---- 2000's pane ---------------------------------------------------------------------------- */

static void on_options_link([[maybe_unused]] GtkButton *button, gpointer data) {
    pane *search_pane = data;
    bool show = !gtk_widget_get_visible(search_pane->options_box);
    gtk_widget_set_visible(search_pane->options_box, show);
    gtk_button_set_label(GTK_BUTTON(search_pane->options_link),
                         show ? "Search Options <<" : "Search Options >>");
}

static void on_option_toggled(GtkToggleButton *toggle, gpointer data) {
    gtk_widget_set_sensitive(GTK_WIDGET(data), gtk_toggle_button_get_active(toggle));
}

static void on_2000_type_toggled(GtkToggleButton *toggle, gpointer data) {
    pane *search_pane = data;
    if (gtk_toggle_button_get_active(toggle))
        fill_types(search_pane);
}

/* An option: its check box, and what it governs greyed until ticked. */
static GtkWidget *option(GtkWidget *box, const char *label, GtkWidget *governed,
                         GtkWidget **check_out) {
    *check_out = check(box, label, false);
    gtk_widget_set_margin_start(governed, 20);
    gtk_widget_set_sensitive(governed, FALSE);
    g_signal_connect(*check_out, "toggled", G_CALLBACK(on_option_toggled), governed);
    gtk_box_pack_start(GTK_BOX(box), governed, FALSE, FALSE, 0);
    return governed;
}

static GtkWidget *spin_row(GtkWidget *radio_button, GtkWidget **spin, const char *before,
                           const char *after, double initial) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(row), radio_button, FALSE, FALSE, 0);
    (void)before;
    *spin = gtk_spin_button_new_with_range(1, 999, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(*spin), initial);
    gtk_box_pack_start(GTK_BOX(row), *spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(after), FALSE, FALSE, 0);
    return row;
}

static void build_2000(pane *search_pane) {
    GtkWidget *page = page_box();
    gtk_box_pack_start(GTK_BOX(page), heading("Search for Files and Folders"), FALSE, FALSE, 0);
    search_pane->all_name = name_entry(search_pane, page, "Search for files or folders _named:");
    search_pane->contents = labelled_entry(page, "Containing _text:");
    g_signal_connect(search_pane->contents, "activate", G_CALLBACK(on_search_clicked),
                     search_pane);
    GtkWidget *look = text_label("_Look in:");
    gtk_box_pack_start(GTK_BOX(page), look, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), look_in_combo(search_pane), FALSE, FALSE, 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(look), search_pane->look_in);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    search_pane->search_now = gtk_button_new_with_mnemonic("Search _Now");
    g_signal_connect(search_pane->search_now, "clicked", G_CALLBACK(on_search_clicked),
                     search_pane);
    search_pane->stop_search = gtk_button_new_with_mnemonic("Sto_p Search");
    gtk_widget_set_sensitive(search_pane->stop_search, FALSE);
    g_signal_connect(search_pane->stop_search, "clicked", G_CALLBACK(on_stop_clicked),
                     search_pane);
    gtk_box_pack_start(GTK_BOX(row), search_pane->search_now, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), search_pane->stop_search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 4);
    search_pane->running_folder = text_label("");
    gtk_label_set_ellipsize(GTK_LABEL(search_pane->running_folder), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_line_wrap(GTK_LABEL(search_pane->running_folder), FALSE);
    gtk_box_pack_start(GTK_BOX(page), search_pane->running_folder, FALSE, FALSE, 0);

    search_pane->options_link = gtk_button_new_with_label("Search Options >>");
    gtk_button_set_relief(GTK_BUTTON(search_pane->options_link), GTK_RELIEF_NONE);
    g_signal_connect(search_pane->options_link, "clicked", G_CALLBACK(on_options_link),
                     search_pane);
    gtk_widget_set_halign(search_pane->options_link, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), search_pane->options_link, FALSE, FALSE, 0);
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    search_pane->options_box = options;
    gtk_widget_set_no_show_all(options, TRUE);

    /* Date: files modified, created or last accessed, in the last months
     * or days, or between two dates. */
    GtkWidget *dates = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    search_pane->date_property = date_property_combo();
    gtk_box_pack_start(GTK_BOX(dates), search_pane->date_property, FALSE, FALSE, 0);
    search_pane->months_radio = gtk_radio_button_new_with_label(nullptr, "in the last");
    gtk_box_pack_start(GTK_BOX(dates),
                       spin_row(search_pane->months_radio, &search_pane->months, nullptr,
                                "months", 1),
                       FALSE, FALSE, 0);
    search_pane->days_radio =
        gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(search_pane->months_radio),
                                                    "in the last");
    gtk_box_pack_start(GTK_BOX(dates),
                       spin_row(search_pane->days_radio, &search_pane->days, nullptr, "days", 1),
                       FALSE, FALSE, 0);
    search_pane->between_radio = radio(dates, search_pane->months_radio, "between");
    search_pane->date_from = date_entry(-30);
    gtk_box_pack_start(GTK_BOX(dates), search_pane->date_from, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dates), text_label("and"), FALSE, FALSE, 0);
    search_pane->date_to = date_entry(0);
    gtk_box_pack_start(GTK_BOX(dates), search_pane->date_to, FALSE, FALSE, 0);
    option(options, "_Date", dates, &search_pane->date_check);

    GtkWidget *types = type_combo(search_pane);
    option(options, "T_ype", types, &search_pane->type_check);
    g_signal_connect(search_pane->type_check, "toggled", G_CALLBACK(on_2000_type_toggled),
                     search_pane);

    GtkWidget *size_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    search_pane->size_compare = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_pane->size_compare), "at least");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(search_pane->size_compare), "at most");
    gtk_combo_box_set_active(GTK_COMBO_BOX(search_pane->size_compare), 0);
    gtk_box_pack_start(GTK_BOX(size_row), search_pane->size_compare, FALSE, FALSE, 0);
    search_pane->size_kb = gtk_spin_button_new_with_range(0, 1e9, 1);
    gtk_box_pack_start(GTK_BOX(size_row), search_pane->size_kb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(size_row), gtk_label_new("KB"), FALSE, FALSE, 0);
    option(options, "Si_ze", size_row, &search_pane->size_check);

    GtkWidget *advanced = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    search_pane->subfolders = check(advanced, "Search su_bfolders", true);
    search_pane->case_sensitive = check(advanced, "_Case sensitive", false);
    search_pane->regex = check(advanced, "Use regular e_xpressions", false);
    gtk_widget_set_tooltip_text(search_pane->regex,
                                "The name and the text are regular expressions (Perl's syntax).");
    option(options, "Ad_vanced Options", advanced, &search_pane->advanced_check);
    gtk_widget_show_all(options);
    gtk_box_pack_start(GTK_BOX(page), options, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(search_pane->stack), page, "classic");
}

/* ---- the pane ------------------------------------------------------------------------------- */

GtkWidget *xd_search_pane_new(xd_search_pane_style style,
                              const xd_search_pane_callbacks *callbacks) {
    pane *search_pane = g_new0(pane, 1);
    search_pane->style = style;
    search_pane->callbacks = *callbacks;
    search_pane->history = gtk_list_store_new(1, G_TYPE_STRING);
    read_preferences(search_pane);
    load_history(search_pane);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    search_pane->root = outer;
    g_object_set_data_full(G_OBJECT(outer), "xd-search-pane", search_pane, free_pane);
    /* The Explorer bar's title and its close button. */
    GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(title_row), 3);
    GtkWidget *title = gtk_label_new(style == XD_SEARCH_PANE_XP ? "Search Companion" : "Search");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(title_row), title, TRUE, TRUE, 3);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close, "Close");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), search_pane);
    gtk_box_pack_end(GTK_BOX(title_row), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), title_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    search_pane->stack = gtk_stack_new();
    gtk_stack_set_vhomogeneous(GTK_STACK(search_pane->stack), FALSE);
    gtk_container_add(GTK_CONTAINER(scroll), search_pane->stack);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
    if (style == XD_SEARCH_PANE_XP) {
        build_advanced(search_pane);
        build_xp(search_pane);
    } else {
        /* 2000's own fields; the XP block isn't shown, only kept. */
        search_pane->advanced = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
        build_2000(search_pane);
    }
    apply_autocomplete(search_pane);
    gtk_widget_show_all(outer);
    xd_search_pane_reset(outer);
    return outer;
}

void xd_search_pane_set_folder(GtkWidget *widget, const char *path, const char *label) {
    pane *search_pane = get_pane(widget);
    if (path == nullptr)
        return;
    add_place(search_pane, label != nullptr ? label : path, path, true);
}

void xd_search_pane_reset(GtkWidget *widget) {
    pane *search_pane = get_pane(widget);
    if (search_pane->style == XD_SEARCH_PANE_2000) {
        show_page(search_pane, "classic");
        return;
    }
    if (search_pane->advanced_behavior)
        open_criteria(search_pane, "all");
    else
        show_page(search_pane, "start");
}

void xd_search_pane_set_running(GtkWidget *widget, bool running) {
    pane *search_pane = get_pane(widget);
    if (search_pane->style == XD_SEARCH_PANE_2000) {
        gtk_widget_set_sensitive(search_pane->search_now, !running);
        gtk_widget_set_sensitive(search_pane->stop_search, running);
        if (!running)
            gtk_label_set_text(GTK_LABEL(search_pane->running_folder), "");
        return;
    }
    if (running) {
        gtk_label_set_text(GTK_LABEL(search_pane->running_folder), "");
        show_page(search_pane, "running");
        gtk_widget_grab_focus(search_pane->stop);
    }
}

void xd_search_pane_set_progress(GtkWidget *widget, const char *folder) {
    pane *search_pane = get_pane(widget);
    char *display = g_filename_display_name(folder);
    gtk_label_set_text(GTK_LABEL(search_pane->running_folder), display);
    g_free(display);
}

void xd_search_pane_show_done(GtkWidget *widget, guint found) {
    pane *search_pane = get_pane(widget);
    xd_search_pane_set_running(widget, false);
    if (search_pane->style == XD_SEARCH_PANE_2000) {
        char *text = g_strdup_printf("%u file(s) found", found);
        gtk_label_set_text(GTK_LABEL(search_pane->running_folder), text);
        g_free(text);
        return;
    }
    char *text = found == 0 ? g_strdup("Search is complete. There are no results to display.")
                            : g_strdup_printf("Search is complete. There %s %u file%s.",
                                              found == 1 ? "is" : "are", found,
                                              found == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(search_pane->done_label), text);
    g_free(text);
    show_page(search_pane, "done");
}

void xd_search_pane_set_criteria(GtkWidget *widget, const xd_search_criteria *criteria) {
    pane *search_pane = get_pane(widget);
    if (search_pane->style == XD_SEARCH_PANE_XP) {
        bool media = (criteria->kinds & (XD_SEARCH_KIND_PICTURE | XD_SEARCH_KIND_MUSIC |
                                         XD_SEARCH_KIND_VIDEO)) != 0 &&
                     (criteria->kinds & ~(uint32_t)(XD_SEARCH_KIND_PICTURE | XD_SEARCH_KIND_MUSIC |
                                                    XD_SEARCH_KIND_VIDEO)) == 0;
        open_criteria(search_pane, media                                         ? "media"
                                   : criteria->kinds == XD_SEARCH_KIND_DOCUMENT ? "documents"
                                                                                : "all");
    }
    GtkWidget *name = search_pane->style == XD_SEARCH_PANE_2000 ||
                              strcmp(search_pane->criteria_page, "all") == 0
                          ? search_pane->all_name
                      : strcmp(search_pane->criteria_page, "media") == 0
                          ? search_pane->media_name
                          : search_pane->documents_name;
    gtk_entry_set_text(GTK_ENTRY(name), criteria->name != nullptr ? criteria->name : "");
    gtk_entry_set_text(GTK_ENTRY(search_pane->contents),
                       criteria->contents != nullptr ? criteria->contents : "");
    if (search_pane->style == XD_SEARCH_PANE_XP) {
        set_on(search_pane->media_kinds[0], (criteria->kinds & XD_SEARCH_KIND_PICTURE) != 0);
        set_on(search_pane->media_kinds[1], (criteria->kinds & XD_SEARCH_KIND_MUSIC) != 0);
        set_on(search_pane->media_kinds[2], (criteria->kinds & XD_SEARCH_KIND_VIDEO) != 0);
        bool advanced = (criteria->contents != nullptr && criteria->contents[0] != '\0') ||
                        criteria->size != XD_SEARCH_SIZE_ANY || criteria->hidden ||
                        criteria->regex || criteria->case_sensitive;
        set_on(search_pane->media_advanced_check, advanced);
        set_on(search_pane->documents_advanced_check, advanced);
        if (criteria->when < XD_SEARCH_WHEN_COUNT)
            set_on(search_pane->when[criteria->when], true);
        if (criteria->when < XD_SEARCH_WHEN_DATES)
            set_on(search_pane->documents_when[criteria->when], true);
        if (criteria->size <= XD_SEARCH_SIZE_LARGE)
            set_on(search_pane->size[criteria->size], true);
        set_on(search_pane->hidden, criteria->hidden);
        set_on(search_pane->system, criteria->system);
    }
    if (criteria->date_from != nullptr)
        gtk_entry_set_text(GTK_ENTRY(search_pane->date_from), criteria->date_from);
    if (criteria->date_to != nullptr)
        gtk_entry_set_text(GTK_ENTRY(search_pane->date_to), criteria->date_to);
    if (criteria->size == XD_SEARCH_SIZE_AT_LEAST || criteria->size == XD_SEARCH_SIZE_AT_MOST) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(search_pane->size_compare),
                                 criteria->size == XD_SEARCH_SIZE_AT_MOST ? 1 : 0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(search_pane->size_kb),
                                  (double)criteria->size_kb);
        set_on(search_pane->size_check, true);
    }
    if (criteria->content_type != nullptr) {
        fill_types(search_pane);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(search_pane->type), criteria->content_type);
        set_on(search_pane->type_check, true);
    }
    set_on(search_pane->subfolders, criteria->subfolders);
    set_on(search_pane->case_sensitive, criteria->case_sensitive);
    set_on(search_pane->regex, criteria->regex);
    set_on(search_pane->advanced_check, criteria->regex || criteria->case_sensitive ||
                                            !criteria->subfolders);
    if (criteria->look_in != nullptr && criteria->look_in[0] != nullptr) {
        char *name_shown = g_filename_display_basename(criteria->look_in[0]);
        add_place(search_pane, name_shown, criteria->look_in[0], true);
        g_free(name_shown);
    }
}

void xd_search_pane_focus(GtkWidget *widget) {
    pane *search_pane = get_pane(widget);
    if (search_pane->style == XD_SEARCH_PANE_2000) {
        gtk_widget_grab_focus(search_pane->all_name);
        return;
    }
    const char *page = gtk_stack_get_visible_child_name(GTK_STACK(search_pane->stack));
    if (g_strcmp0(page, "all") == 0)
        gtk_widget_grab_focus(search_pane->all_name);
    else if (g_strcmp0(page, "media") == 0)
        gtk_widget_grab_focus(search_pane->media_name);
    else if (g_strcmp0(page, "documents") == 0)
        gtk_widget_grab_focus(search_pane->documents_name);
    else
        gtk_widget_child_focus(search_pane->stack, GTK_DIR_TAB_FORWARD);
}
