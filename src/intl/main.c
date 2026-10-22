/* SPDX-License-Identifier: MIT
 * xd-intl: Windows XP's Regional and Language Options.
 *
 *   Regional Options  the user's standards and formats (LC_NUMERIC,
 *                     LC_TIME, ...; see intl.h), with samples;
 *                     Customize... changes them (customize.c), compiled
 *                     into a locale of the user's own with localedef
 *   Languages         Details... opens the Keyboard applet's input
 *                     languages and text services (only if xd-keyboard
 *                     is installed)
 *   Advanced          the system locale and the keyboard of the logon
 *                     screen and text consoles, through systemd's localed
 *                     (org.freedesktop.locale1); changing them goes
 *                     through polkit, so xd-polkit asks for a password
 *
 * Only installed locales are offered: the UTF-8 ones in glibc's SUPPORTED
 * list that newlocale() can load (what `localectl list-locales` shows).
 * Keyboard layouts come from xkeyboard-config through libxkbregistry.
 */
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <xkbcommon/xkbregistry.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "customize.h"
#include "intl.h"

#define LOCALE1 "org.freedesktop.locale1"
#define LOCALE1_PATH "/org/freedesktop/locale1"
#define SUPPORTED "/usr/share/i18n/SUPPORTED"

typedef struct props {
    GtkWidget *window, *apply;
    GDBusProxy *l1;
    bool updating; /* we're setting the widgets ourselves */
    bool busy;      /* Apply is waiting for the service */
    char **locales; /* installed */
    /* Regional Options */
    GtkWidget *formats, *number, *currency, *time, *short_date, *long_date, *note;
    GtkWidget *customize;
    char *user_formats;     /* as applied (the locale, without the customization) */
    xd_intl_custom custom;  /* Customize's changes to the formats chosen */
    bool custom_changed;    /* since applied */
    /* Advanced */
    GtkWidget *system_locale, *layout, *variant, *convert, *console;
    struct rxkb_context *xkb;
} props;

static void changed(props *properties) {
    if (!properties->updating)
        gtk_widget_set_sensitive(properties->apply, TRUE);
}

static char *formats_path(void) {
    return g_build_filename(g_get_user_config_dir(), "environment.d",
                            "60-xeondesktop-locale.conf", nullptr);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *frame_widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame_widget), child);
    return frame_widget;
}

static GtkWidget *text(const char *label_text) {
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 56);
    return label;
}

/* A combo of locales by their names, with ids; `current` is selected,
 * and added if it isn't installed (so it's still shown as it is). */
static int compare_display(gconstpointer left, gconstpointer right, gpointer names) {
    return g_utf8_collate(g_hash_table_lookup(names, *(const char *const *)left),
                          g_hash_table_lookup(names, *(const char *const *)right));
}

static void fill_locales(props *properties, GtkWidget *combo, const char *current) {
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, nullptr, g_free);
    GPtrArray *list = g_ptr_array_new();
    for (char **locale = properties->locales; *locale != nullptr; locale++) {
        g_ptr_array_add(list, *locale);
        g_hash_table_insert(names, *locale, xd_intl_display_name(*locale));
    }
    if (current != nullptr && !g_strv_contains((const char *const *)properties->locales, current)) {
        g_ptr_array_add(list, (gpointer)(uintptr_t)current);
        g_hash_table_insert(names, (gpointer)(uintptr_t)current, xd_intl_display_name(current));
    }
    g_ptr_array_sort_with_data(list, compare_display, names);
    properties->updating = true;
    for (guint i = 0; i < list->len; i++) {
        const char *id = g_ptr_array_index(list, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, g_hash_table_lookup(names, id));
    }
    if (current != nullptr)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), current);
    properties->updating = false;
    g_ptr_array_free(list, TRUE);
    g_hash_table_unref(names);
}

/* ---- Regional Options ---------------------------------------------------------- */

static char *custom_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "formats.ini", nullptr);
}

/* Samples are read-only text boxes, as in Windows. */
static void entry_text(GtkWidget *widget, const char *sample) {
    gtk_entry_set_text(GTK_ENTRY(widget), sample);
}

static void show_samples(props *properties) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->formats));
    xd_intl_samples intl_samples = {0};
    time_t now = time(nullptr);
    xd_intl_custom custom;
    bool ok;
    if (id != nullptr && !xd_intl_custom_empty(&properties->custom) &&
        xd_intl_custom_effective(id, &properties->custom, &custom)) {
        /* Customized: as Customize shows them. */
        intl_samples.number = xd_intl_number_sample(&custom, false);
        intl_samples.currency = xd_intl_currency_sample(&custom, false);
        intl_samples.time = xd_intl_time_sample(id, &custom, now);
        intl_samples.short_date = xd_intl_date_sample(id, &custom, now, false);
        intl_samples.long_date = xd_intl_date_sample(id, &custom, now, true);
        xd_intl_custom_clear(&custom);
        ok = true;
    } else {
        ok = id != nullptr && xd_intl_samples_get(id, now, &intl_samples);
    }
    entry_text(properties->number, ok ? intl_samples.number : "");
    entry_text(properties->currency, ok ? intl_samples.currency : "");
    entry_text(properties->time, ok ? intl_samples.time : "");
    entry_text(properties->short_date, ok ? intl_samples.short_date : "");
    entry_text(properties->long_date, ok ? intl_samples.long_date : "");
    if (ok)
        xd_intl_samples_free(&intl_samples);
}

static void on_formats([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    /* Other formats start without the customization, as in XP. */
    if (!xd_intl_custom_empty(&properties->custom)) {
        xd_intl_custom_clear(&properties->custom);
        properties->custom_changed = true;
    }
    show_samples(properties);
    changed(properties);
}

static void customized(void *data) {
    props *properties = data;
    properties->custom_changed = true;
    show_samples(properties);
    changed(properties);
}

static void on_customize([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->formats));
    if (id != nullptr)
        xd_intl_customize(GTK_WINDOW(properties->window), id, &properties->custom, customized,
                          properties);
}

static GtkWidget *sample_row(GtkGrid *grid, int row, const char *label) {
    GtkWidget *label_widget = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    GtkWidget *value = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(value), FALSE);
    gtk_widget_set_can_focus(value, FALSE);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(grid, label_widget, 0, row, 1, 1);
    gtk_grid_attach(grid, value, 1, row, 1, 1);
    return value;
}

static GtkWidget *regional_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box),
                       text("This option affects how some programs format numbers, "
                            "currencies, dates, and time."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       text("Select an item to match its preferences, or click Customize to "
                            "choose your own formats:"),
                       FALSE, FALSE, 0);
    GtkWidget *choose = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    properties->formats = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(choose), properties->formats, TRUE, TRUE, 0);
    properties->customize = gtk_button_new_with_mnemonic("C_ustomize...");
    g_signal_connect(properties->customize, "clicked", G_CALLBACK(on_customize), properties);
    gtk_box_pack_start(GTK_BOX(choose), properties->customize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), choose, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    properties->number = sample_row(GTK_GRID(grid), 0, "Number:");
    properties->currency = sample_row(GTK_GRID(grid), 1, "Currency:");
    properties->time = sample_row(GTK_GRID(grid), 2, "Time:");
    properties->short_date = sample_row(GTK_GRID(grid), 3, "Short date:");
    properties->long_date = sample_row(GTK_GRID(grid), 4, "Long date:");
    gtk_box_pack_start(GTK_BOX(box), frame("Samples", grid), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Standards and formats", box), FALSE, FALSE, 0);

    /* Filled in on Apply; empty, it keeps its line so the window doesn't
     * grow. */
    properties->note = text("");
    gtk_box_pack_start(GTK_BOX(page), properties->note, FALSE, FALSE, 0);

    /* The formats as set: ours, else what this session got. */
    char *path = formats_path();
    char *set = xd_intl_read_formats(path);
    g_free(path);
    if (set == nullptr)
        set = g_strdup(setlocale(LC_TIME, nullptr));
    properties->user_formats = xd_intl_custom_base(set);
    g_free(set);
    /* The customization, if it's of these formats. */
    path = custom_path();
    char *of = nullptr;
    xd_intl_custom_load(path, &of, &properties->custom);
    g_free(path);
    if (g_strcmp0(of, properties->user_formats) != 0)
        xd_intl_custom_clear(&properties->custom);
    g_free(of);
    fill_locales(properties, properties->formats, properties->user_formats);
    g_signal_connect(properties->formats, "changed", G_CALLBACK(on_formats), properties);
    return page;
}

/* ---- Languages ------------------------------------------------------------------- */

/* One of our programs: next to this one (a build tree), else on PATH. */
static char *find_own(const char *program) {
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = self != nullptr ? g_path_get_dirname(self) : nullptr;
    char *local = dir != nullptr ? g_build_filename(dir, program, nullptr) : nullptr;
    g_free(self);
    g_free(dir);
    if (local != nullptr && g_file_test(local, G_FILE_TEST_IS_EXECUTABLE))
        return local;
    g_free(local);
    return g_find_program_in_path(program);
}

/* XP's Text Services and Input Languages: Keyboard Properties' Input
 * Languages tab, beside its Text Services. */
static void on_details(GtkButton *button, [[maybe_unused]] gpointer data) {
    char *exe = find_own("xd-keyboard");
    const char *argv[] = {exe, "--page", "1", nullptr};
    GError *error = nullptr;
    if (exe == nullptr || !g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr,
                                         G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error)) {
        xd_message_box(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
                       "Regional and Language Options", XD_MB_ERROR, XD_MB_OK, FALSE,
                       error != nullptr ? error->message : "The Keyboard applet isn't installed.");
        g_clear_error(&error);
    }
    g_free(exe);
}

static GtkWidget *languages_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box),
                       text("To view or change the languages and methods you can use to enter "
                            "text, click Details."),
                       FALSE, FALSE, 0);
    GtkWidget *details = gtk_button_new_with_mnemonic("_Details...");
    gtk_widget_set_halign(details, GTK_ALIGN_END);
    gtk_widget_set_size_request(details, 75, -1);
    g_signal_connect(details, "clicked", G_CALLBACK(on_details), nullptr);
    gtk_box_pack_start(GTK_BOX(box), details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Text services and input languages", box), FALSE,
                       FALSE, 0);
    return page;
}

/* ---- Advanced -------------------------------------------------------------------- */

static GVariant *l1_prop(props *properties, const char *name) {
    return properties->l1 != nullptr ? g_dbus_proxy_get_cached_property(properties->l1, name)
                                     : nullptr;
}

static char *l1_string(props *properties, const char *name) {
    GVariant *variant = l1_prop(properties, name);
    char *text = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING)
                     ? g_variant_dup_string(variant, nullptr)
                     : g_strdup("");
    if (variant != nullptr)
        g_variant_unref(variant);
    return text;
}

static char **l1_locale(props *properties) {
    GVariant *variant = l1_prop(properties, "Locale");
    char **locale = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING_ARRAY)
                        ? g_variant_dup_strv(variant, nullptr)
                        : g_new0(char *, 1);
    if (variant != nullptr)
        g_variant_unref(variant);
    return locale;
}

static int compare_text(GtkTreeModel *model, GtkTreeIter *left, GtkTreeIter *right,
                        [[maybe_unused]] gpointer data) {
    char *left_text = nullptr, *right_text = nullptr, *xid = nullptr, *yid = nullptr;
    gtk_tree_model_get(model, left, 0, &left_text, 1, &xid, -1);
    gtk_tree_model_get(model, right, 0, &right_text, 1, &yid, -1);
    /* The empty id, "(Standard)", goes first. */
    bool xs = xid == nullptr || xid[0] == '\0', ys = yid == nullptr || yid[0] == '\0';
    int order = xs != ys ? (xs ? -1 : 1)
                         : g_utf8_collate(left_text != nullptr ? left_text : "",
                                          right_text != nullptr ? right_text : "");
    g_free(left_text);
    g_free(right_text);
    g_free(xid);
    g_free(yid);
    return order;
}

/* Combo boxes sorted by their text (GtkComboBoxText's columns: text, id). */
static GtkWidget *sorted_combo(void) {
    GtkWidget *combo = gtk_combo_box_text_new();
    GtkTreeSortable *sortable = GTK_TREE_SORTABLE(gtk_combo_box_get_model(GTK_COMBO_BOX(combo)));
    gtk_tree_sortable_set_sort_func(sortable, 0, compare_text, nullptr, nullptr);
    gtk_tree_sortable_set_sort_column_id(sortable, 0, GTK_SORT_ASCENDING);
    return combo;
}

/* The variants of the chosen layout; "" is the layout itself. */
static void fill_variants(props *properties, const char *selected) {
    const char *layout = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->layout));
    properties->updating = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(properties->variant));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->variant), "", "(Standard)");
    bool found = selected == nullptr || selected[0] == '\0';
    for (struct rxkb_layout *layout_entry =
             properties->xkb != nullptr ? rxkb_layout_first(properties->xkb) : nullptr;
         layout_entry != nullptr && layout != nullptr;
         layout_entry = rxkb_layout_next(layout_entry)) {
        const char *variant_name = rxkb_layout_get_variant(layout_entry);
        if (variant_name == nullptr || strcmp(rxkb_layout_get_name(layout_entry), layout) != 0)
            continue;
        const char *desc = rxkb_layout_get_description(layout_entry);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->variant), variant_name,
                                  desc != nullptr ? desc : variant_name);
        found = found || strcmp(variant_name, selected) == 0;
    }
    if (!found) /* not one xkeyboard-config knows; keep it */
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->variant), selected, selected);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->variant),
                                selected != nullptr ? selected : "");
    properties->updating = false;
}

static void on_layout([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    fill_variants(properties, "");
    changed(properties);
}

static void on_changed_widget([[maybe_unused]] GtkWidget *widget, gpointer data) { changed(data); }

static void show_console(props *properties) {
    char *keymap = l1_string(properties, "VConsoleKeymap");
    char *text = g_strdup_printf("Text console keymap: %s",
                                 keymap[0] != '\0' ? keymap : "(the kernel's default)");
    gtk_label_set_text(GTK_LABEL(properties->console), text);
    g_free(text);
    g_free(keymap);
}

static void show_keyboard(props *properties) {
    char *layouts = l1_string(properties, "X11Layout"),
         *variants = l1_string(properties, "X11Variant");
    char *layout = xd_intl_first(layouts), *variant = xd_intl_first(variants);
    properties->updating = true;
    if (layout[0] != '\0' &&
        !gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->layout), layout)) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->layout), layout, layout);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->layout), layout);
    }
    properties->updating = false;
    fill_variants(properties, variant);
    show_console(properties);
    g_free(layouts);
    g_free(variants);
    g_free(layout);
    g_free(variant);
}

static GtkWidget *advanced_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box),
                       text("The system locale is the language and formats of the logon "
                            "screen, system services, and programs that don't choose their "
                            "own. Select the one that matches the language of your programs:"),
                       FALSE, FALSE, 0);
    properties->system_locale = gtk_combo_box_text_new();
    char **locale = l1_locale(properties);
    fill_locales(properties, properties->system_locale, xd_intl_lang((const char *const *)locale));
    g_strfreev(locale);
    g_signal_connect(properties->system_locale, "changed", G_CALLBACK(on_changed_widget),
                     properties);
    gtk_box_pack_start(GTK_BOX(box), properties->system_locale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("System locale", box), FALSE, FALSE, 0);

    GtkWidget *kb = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(kb), 8);
    gtk_grid_set_column_spacing(GTK_GRID(kb), 12);
    GtkWidget *what = text("The keyboard layout of the logon screen and of new user accounts:");
    gtk_grid_attach(GTK_GRID(kb), what, 0, 0, 2, 1);
    properties->layout = sorted_combo();
    properties->variant = sorted_combo();
    gtk_widget_set_hexpand(properties->layout, TRUE);
    properties->xkb = rxkb_context_new(RXKB_CONTEXT_NO_FLAGS);
    if (properties->xkb != nullptr && !rxkb_context_parse_default_ruleset(properties->xkb)) {
        rxkb_context_unref(properties->xkb);
        properties->xkb = nullptr;
    }
    for (struct rxkb_layout *layout_entry =
             properties->xkb != nullptr ? rxkb_layout_first(properties->xkb) : nullptr;
         layout_entry != nullptr; layout_entry = rxkb_layout_next(layout_entry)) {
        if (rxkb_layout_get_variant(layout_entry) != nullptr)
            continue;
        const char *desc = rxkb_layout_get_description(layout_entry);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->layout),
                                  rxkb_layout_get_name(layout_entry),
                                  desc != nullptr ? desc : rxkb_layout_get_name(layout_entry));
    }
    GtkWidget *ll = gtk_label_new_with_mnemonic("_Layout:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(ll), properties->layout);
    gtk_label_set_xalign(GTK_LABEL(ll), 0);
    GtkWidget *vl = gtk_label_new_with_mnemonic("_Variant:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(vl), properties->variant);
    gtk_label_set_xalign(GTK_LABEL(vl), 0);
    gtk_grid_attach(GTK_GRID(kb), ll, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(kb), properties->layout, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(kb), vl, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(kb), properties->variant, 1, 2, 1, 1);
    properties->convert =
        gtk_check_button_new_with_mnemonic("Use the _closest keymap on text consoles too");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(properties->convert), TRUE);
    gtk_grid_attach(GTK_GRID(kb), properties->convert, 0, 3, 2, 1);
    properties->console = text("");
    gtk_grid_attach(GTK_GRID(kb), properties->console, 0, 4, 2, 1);
    gtk_box_pack_start(GTK_BOX(page), frame("System keyboard", kb), FALSE, FALSE, 0);
    show_keyboard(properties);
    g_signal_connect(properties->layout, "changed", G_CALLBACK(on_layout), properties);
    g_signal_connect(properties->variant, "changed", G_CALLBACK(on_changed_widget), properties);

    if (properties->l1 == nullptr) {
        gtk_widget_set_sensitive(properties->system_locale, FALSE);
        gtk_widget_set_sensitive(kb, FALSE);
        gtk_box_pack_start(GTK_BOX(page),
                           text("The system settings can't be changed: systemd-localed isn't "
                                "available."),
                           FALSE, FALSE, 0);
    }
    return page;
}

/* ---- applying -------------------------------------------------------------------- */

static void error_box(props *properties, const char *what, const char *detail) {
    char *text = g_strdup_printf("%s\n\n%s", what, detail);
    xd_message_box(GTK_WINDOW(properties->window), "Regional and Language Options", XD_MB_ERROR,
                   XD_MB_OK, FALSE, text);
    g_free(text);
}

/* Apply waits for localed (and its password prompt, which has no time
 * limit) in a main loop of its own, so the window keeps drawing and
 * answering the compositor instead of turning "Not Responding". It takes
 * no input meanwhile. */
typedef struct pending_call {
    GMainLoop *loop;
    GVariant *result;
    GError *error;
} pending_call;

static void call_done(GObject *src, GAsyncResult *res, gpointer data) {
    pending_call *call_state = data;
    call_state->result = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &call_state->error);
    g_main_loop_quit(call_state->loop);
}

static bool call(props *properties, const char *method, GVariant *args, const char *what) {
    pending_call call_state = {.loop = g_main_loop_new(nullptr, FALSE)};
    properties->busy = true;
    gtk_widget_set_sensitive(properties->window, FALSE);
    g_dbus_proxy_call(properties->l1, method, args, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, nullptr,
                      call_done, &call_state);
    g_main_loop_run(call_state.loop);
    g_main_loop_unref(call_state.loop);
    gtk_widget_set_sensitive(properties->window, TRUE);
    properties->busy = false;
    if (call_state.result == nullptr) {
        g_dbus_error_strip_remote_error(call_state.error);
        error_box(properties, what, call_state.error->message);
        g_error_free(call_state.error);
        return false;
    }
    g_variant_unref(call_state.result);
    return true;
}

/* Nor may the window be destroyed under Apply. */
static gboolean on_delete([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] GdkEvent *event_data,
                          gpointer data) {
    return ((props *)data)->busy;
}

/* ---- compiling customized formats ----------------------------------------------------- */

static char *locale_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "xeondesktop", "locale", nullptr);
}

/* Removes a compiled locale (files, and LC_MESSAGES's folder). Programs
 * already using its files keep them: they're unlinked, not changed. */
static void remove_tree(GFile *dir) {
    GFileEnumerator *file_enumerator = g_file_enumerate_children(
        dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    GFileInfo *info;
    while (file_enumerator != nullptr &&
           (info = g_file_enumerator_next_file(file_enumerator, nullptr, nullptr)) != nullptr) {
        GFile *child = g_file_get_child(dir, g_file_info_get_name(info));
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
            remove_tree(child);
        else
            g_file_delete(child, nullptr, nullptr);
        g_object_unref(child);
        g_object_unref(info);
    }
    g_clear_object(&file_enumerator);
    g_file_delete(dir, nullptr, nullptr);
}

static void remove_path(const char *path) {
    GFile *file = g_file_new_for_path(path);
    if (g_file_query_exists(file, nullptr))
        remove_tree(file);
    g_object_unref(file);
}

/* localedef, waited for the way localed is (call()). */
typedef struct pending_run {
    GMainLoop *loop;
    bool ok;
    char *why;
} pending_run;

static void run_done(GObject *src, GAsyncResult *res, gpointer data) {
    pending_run *run = data;
    GSubprocess *sp = G_SUBPROCESS(src);
    GError *error = nullptr;
    char *err = nullptr;
    if (g_subprocess_communicate_utf8_finish(sp, res, nullptr, &err, &error)) {
        /* 1 is warnings only: the locale is written. */
        run->ok = g_subprocess_get_if_exited(sp) && g_subprocess_get_exit_status(sp) <= 1;
        run->why = err;
    } else {
        run->why = g_strdup(error->message);
        g_error_free(error);
    }
    g_main_loop_quit(run->loop);
}

static bool localedef(const char *source, const char *charmap, const char *out, char **why) {
    const char *argv[] = {"localedef", "--no-archive", "-i", source, "-f", charmap, out, nullptr};
    GError *error = nullptr;
    GSubprocess *sp = g_subprocess_newv(
        argv, (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &error);
    if (sp == nullptr) {
        *why = g_strdup(error->message);
        g_error_free(error);
        return false;
    }
    pending_run run = {.loop = g_main_loop_new(nullptr, FALSE)};
    g_subprocess_communicate_utf8_async(sp, nullptr, nullptr, run_done, &run);
    g_main_loop_run(run.loop);
    g_main_loop_unref(run.loop);
    g_object_unref(sp);
    char *check = g_build_filename(out, "LC_NUMERIC", nullptr);
    run.ok = run.ok && g_file_test(check, G_FILE_TEST_IS_REGULAR);
    g_free(check);
    if (run.ok)
        g_free(run.why);
    else
        *why = run.why != nullptr && run.why[0] != '\0' ? run.why : g_strdup("localedef failed.");
    return run.ok;
}

/* Built in, or not a name we'd find: nothing to compile. */
static bool needs_compiling(const char *name) {
    return name != nullptr && name[0] != '\0' && strcmp(name, "C") != 0 &&
           strcmp(name, "POSIX") != 0 && g_ascii_strncasecmp(name, "C.", 2) != 0 &&
           !g_str_has_suffix(name, "@xeondesktop") && xd_intl_valid_name(name);
}

/* The customized formats as a compiled locale in `dir`, and the
 * session's language beside it (LOCPATH hides the system's). */
static bool compile_custom(props *properties, const char *id, const char *dir, char **why) {
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        *why = g_strdup_printf("%s can't be created: %s", dir, g_strerror(errno));
        return false;
    }
    char *found = g_find_program_in_path("localedef");
    if (found == nullptr) {
        *why = g_strdup("localedef, from the C library's locale tools, isn't installed.");
        return false;
    }
    g_free(found);
    bool ok = true;
    static const char *const vars[] = {"LANG", "LC_CTYPE", "LC_COLLATE", "LC_MESSAGES"};
    for (size_t i = 0; ok && i < G_N_ELEMENTS(vars); i++) {
        const char *name = g_getenv(vars[i]);
        if (!needs_compiling(name))
            continue;
        char *out = g_build_filename(dir, name, nullptr);
        char *done = g_build_filename(out, "LC_CTYPE", nullptr);
        if (!g_file_test(done, G_FILE_TEST_IS_REGULAR)) {
            char *source = nullptr, *charmap = nullptr;
            xd_intl_source_of(name, &source, &charmap);
            ok = localedef(source, charmap, out, why);
            g_free(source);
            g_free(charmap);
        }
        g_free(done);
        g_free(out);
    }
    char *text = ok ? xd_intl_custom_source(id, &properties->custom) : nullptr;
    if (ok && text == nullptr) {
        *why = g_strdup_printf("%s isn't installed.", id);
        ok = false;
    }
    if (ok) {
        /* Built beside the one in use, then swapped in. */
        char *name = xd_intl_custom_name(id);
        char *src = g_build_filename(dir, "xeondesktop.src", nullptr);
        char *out = g_build_filename(dir, name, nullptr);
        char *fresh = g_strconcat(out, ".new", nullptr);
        char *old = g_strconcat(out, ".old", nullptr);
        GError *error = nullptr;
        remove_path(fresh);
        ok = g_file_set_contents(src, text, -1, &error);
        if (!ok) {
            *why = g_strdup(error->message);
            g_error_free(error);
        }
        char *charmap = nullptr, *source = nullptr;
        xd_intl_source_of(id, &source, &charmap);
        ok = ok && localedef(src, charmap, fresh, why);
        if (ok) {
            remove_path(old);
            if (g_file_test(out, G_FILE_TEST_EXISTS))
                g_rename(out, old);
            ok = g_rename(fresh, out) == 0;
            if (!ok)
                *why = g_strdup_printf("%s can't be replaced: %s", out, g_strerror(errno));
            remove_path(old);
        }
        g_free(source);
        g_free(charmap);
        g_free(old);
        g_free(fresh);
        g_free(out);
        g_free(src);
        g_free(name);
    }
    g_free(text);
    return ok;
}

static bool apply_formats(props *properties) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->formats));
    if (id == nullptr ||
        (g_strcmp0(id, properties->user_formats) == 0 && !properties->custom_changed))
        return true;
    char *path = formats_path();
    char *cpath = custom_path();
    GError *error = nullptr;
    char *why = nullptr;
    bool ok;
    if (xd_intl_custom_empty(&properties->custom)) {
        /* The compiled locales stay: programs of this session use them
         * until you log off. */
        ok = xd_intl_write_formats(path, id, &error) &&
             xd_intl_custom_save(cpath, nullptr, &properties->custom, &error);
    } else {
        char *dir = locale_dir();
        properties->busy = true;
        gtk_widget_set_sensitive(properties->window, FALSE);
        GdkWindow *win = gtk_widget_get_window(properties->window);
        GdkCursor *wait = win != nullptr ? gdk_cursor_new_from_name(gdk_window_get_display(win),
                                                                    "wait")
                                         : nullptr;
        if (win != nullptr)
            gdk_window_set_cursor(win, wait);
        ok = compile_custom(properties, id, dir, &why);
        if (win != nullptr)
            gdk_window_set_cursor(win, nullptr);
        g_clear_object(&wait);
        gtk_widget_set_sensitive(properties->window, TRUE);
        properties->busy = false;
        if (ok) {
            char *name = xd_intl_custom_name(id);
            ok = xd_intl_write_formats_full(path, name, dir, &error) &&
                 xd_intl_custom_save(cpath, id, &properties->custom, &error);
            g_free(name);
        }
        g_free(dir);
    }
    g_free(path);
    g_free(cpath);
    if (!ok) {
        error_box(properties, "Your formats could not be saved.",
                  why != nullptr     ? why
                  : error != nullptr ? error->message
                                     : "");
        g_clear_error(&error);
        g_free(why);
        return false;
    }
    g_free(properties->user_formats);
    properties->user_formats = g_strdup(id);
    properties->custom_changed = false;
    gtk_label_set_text(GTK_LABEL(properties->note),
                       "Programs use the new formats after you log off and log on again.");
    return true;
}

static bool apply_system(props *properties) {
    if (properties->l1 == nullptr)
        return true;
    bool ok = true;
    const char *lang = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->system_locale));
    char **locale = l1_locale(properties);
    if (lang != nullptr && g_strcmp0(lang, xd_intl_lang((const char *const *)locale)) != 0) {
        char **set = xd_intl_replace_lang((const char *const *)locale, lang);
        ok = call(properties, "SetLocale", g_variant_new("(^asb)", set, TRUE),
                  "The system locale could not be changed.");
        g_strfreev(set);
    }
    g_strfreev(locale);

    const char *layout = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->layout));
    const char *variant = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->variant));
    char *layouts = l1_string(properties, "X11Layout"),
         *variants = l1_string(properties, "X11Variant");
    char *old_layout = xd_intl_first(layouts), *old_variant = xd_intl_first(variants);
    if (ok && layout != nullptr &&
        (strcmp(layout, old_layout) != 0 || g_strcmp0(variant, old_variant) != 0)) {
        /* Other layouts after the first only go with their variants. */
        char *new_layouts = xd_intl_replace_first(layouts, layout);
        char *new_variants = xd_intl_replace_first(variants, variant != nullptr ? variant : "");
        char *model = l1_string(properties, "X11Model"),
             *options = l1_string(properties, "X11Options");
        bool convert = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(properties->convert));
        ok = call(
            properties, "SetX11Keyboard",
            g_variant_new("(ssssbb)", new_layouts, model, new_variants, options, convert, TRUE),
            "The system keyboard could not be changed.");
        g_free(model);
        g_free(options);
        g_free(new_layouts);
        g_free(new_variants);
    }
    g_free(layouts);
    g_free(variants);
    g_free(old_layout);
    g_free(old_variant);
    return ok;
}

static bool apply(props *properties) {
    bool ok = apply_formats(properties) && apply_system(properties);
    if (ok)
        gtk_widget_set_sensitive(properties->apply, FALSE);
    return ok;
}

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    props *properties = data;
    if (properties->busy)
        return; /* the window's close button, while Apply waits */
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply) &&
        !apply(properties))
        return;
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

static void l1_changed([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *variant,
                       [[maybe_unused]] const char *const *inv, gpointer data) {
    props *properties = data;
    /* Only what can't have been edited here; the rest is ours until
     * applied or cancelled. */
    show_console(properties);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.RegionalOptions");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    /* GTK has set this program's locale, customized formats and all;
     * from here on the system's compiled locales are the ones listed and
     * sampled, which LOCPATH would hide. */
    g_unsetenv("LOCPATH");
    static props properties;
    GError *error = nullptr;
    properties.l1 =
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, LOCALE1,
                                      LOCALE1_PATH, LOCALE1, nullptr, &error);
    if (properties.l1 == nullptr) {
        g_printerr("xd-intl: %s\n", error->message);
        g_error_free(error);
    } else {
        /* A proxy is made even when nothing answers; no properties means
         * no localed. */
        char **names = g_dbus_proxy_get_cached_property_names(properties.l1);
        if (names == nullptr) {
            g_printerr("xd-intl: %s isn't available\n", LOCALE1);
            g_clear_object(&properties.l1);
        }
        g_strfreev(names);
    }
    properties.locales = xd_intl_installed_locales(SUPPORTED);

    properties.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties.window), "Regional and Language Options");
    gtk_window_set_icon_name(GTK_WINDOW(properties.window), "preferences-desktop-locale");
    gtk_window_set_resizable(GTK_WINDOW(properties.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "Cancel", GTK_RESPONSE_CANCEL);
    properties.apply =
        gtk_dialog_add_button(GTK_DIALOG(properties.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(properties.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(properties.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), regional_page(&properties),
                             gtk_label_new_with_mnemonic("_Regional Options"));
    /* Input languages are the Keyboard applet's; the tab is only a way
     * there. */
    char *keyboard = find_own("xd-keyboard");
    if (keyboard != nullptr)
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), languages_page(),
                                 gtk_label_new_with_mnemonic("_Languages"));
    g_free(keyboard);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), advanced_page(&properties),
                             gtk_label_new_with_mnemonic("Ad_vanced"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties.window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties.window, GTK_NOTEBOOK(tabs));
    show_samples(&properties);
    if (properties.l1 != nullptr)
        g_signal_connect(properties.l1, "g-properties-changed", G_CALLBACK(l1_changed),
                         &properties);
    g_signal_connect(properties.window, "response", G_CALLBACK(on_response), &properties);
    g_signal_connect(properties.window, "delete-event", G_CALLBACK(on_delete), &properties);
    g_signal_connect(properties.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(properties.window);
    gtk_main();
    if (properties.xkb != nullptr)
        rxkb_context_unref(properties.xkb);
    g_strfreev(properties.locales);
    g_free(properties.user_formats);
    xd_intl_custom_clear(&properties.custom);
    g_clear_object(&properties.l1);
    return EXIT_SUCCESS;
}
