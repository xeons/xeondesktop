/* SPDX-License-Identifier: MIT
 * xd-intl: Windows XP's Regional and Language Options.
 *
 *   Regional Options  the user's standards and formats (LC_NUMERIC,
 *                     LC_TIME, ...; see intl.h), with samples;
 *                     Customize... changes them (customize.c), compiled
 *                     into a locale of the user's own with localedef
 *   Languages         Details... opens the Keyboard applet's input
 *                     languages (only if xd-keyboard is installed)
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

static void changed(props *p) {
    if (!p->updating)
        gtk_widget_set_sensitive(p->apply, TRUE);
}

static char *formats_path(void) {
    return g_build_filename(g_get_user_config_dir(), "environment.d",
                            "60-xeondesktop-locale.conf", nullptr);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *text(const char *s) {
    GtkWidget *l = gtk_label_new(s);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
}

/* A combo of locales by their names, with ids; `current` is selected,
 * and added if it isn't installed (so it's still shown as it is). */
static int compare_display(gconstpointer a, gconstpointer b, gpointer names) {
    return g_utf8_collate(g_hash_table_lookup(names, *(const char *const *)a),
                          g_hash_table_lookup(names, *(const char *const *)b));
}

static void fill_locales(props *p, GtkWidget *combo, const char *current) {
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, nullptr, g_free);
    GPtrArray *list = g_ptr_array_new();
    for (char **l = p->locales; *l != nullptr; l++) {
        g_ptr_array_add(list, *l);
        g_hash_table_insert(names, *l, xd_intl_display_name(*l));
    }
    if (current != nullptr && !g_strv_contains((const char *const *)p->locales, current)) {
        g_ptr_array_add(list, (gpointer)(uintptr_t)current);
        g_hash_table_insert(names, (gpointer)(uintptr_t)current, xd_intl_display_name(current));
    }
    g_ptr_array_sort_with_data(list, compare_display, names);
    p->updating = true;
    for (guint i = 0; i < list->len; i++) {
        const char *id = g_ptr_array_index(list, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, g_hash_table_lookup(names, id));
    }
    if (current != nullptr)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), current);
    p->updating = false;
    g_ptr_array_free(list, TRUE);
    g_hash_table_unref(names);
}

/* ---- Regional Options ---------------------------------------------------------- */

static char *custom_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "formats.ini", nullptr);
}

/* Samples are read-only text boxes, as in Windows. */
static void entry_text(GtkWidget *w, const char *s) { gtk_entry_set_text(GTK_ENTRY(w), s); }

static void show_samples(props *p) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->formats));
    xd_intl_samples s = {0};
    time_t now = time(nullptr);
    xd_intl_custom e;
    bool ok;
    if (id != nullptr && !xd_intl_custom_empty(&p->custom) &&
        xd_intl_custom_effective(id, &p->custom, &e)) {
        /* Customized: as Customize shows them. */
        s.number = xd_intl_number_sample(&e, false);
        s.currency = xd_intl_currency_sample(&e, false);
        s.time = xd_intl_time_sample(id, &e, now);
        s.short_date = xd_intl_date_sample(id, &e, now, false);
        s.long_date = xd_intl_date_sample(id, &e, now, true);
        xd_intl_custom_clear(&e);
        ok = true;
    } else {
        ok = id != nullptr && xd_intl_samples_get(id, now, &s);
    }
    entry_text(p->number, ok ? s.number : "");
    entry_text(p->currency, ok ? s.currency : "");
    entry_text(p->time, ok ? s.time : "");
    entry_text(p->short_date, ok ? s.short_date : "");
    entry_text(p->long_date, ok ? s.long_date : "");
    if (ok)
        xd_intl_samples_free(&s);
}

static void on_formats([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    /* Other formats start without the customization, as in XP. */
    if (!xd_intl_custom_empty(&p->custom)) {
        xd_intl_custom_clear(&p->custom);
        p->custom_changed = true;
    }
    show_samples(p);
    changed(p);
}

static void customized(void *data) {
    props *p = data;
    p->custom_changed = true;
    show_samples(p);
    changed(p);
}

static void on_customize([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->formats));
    if (id != nullptr)
        xd_intl_customize(GTK_WINDOW(p->window), id, &p->custom, customized, p);
}

static GtkWidget *sample_row(GtkGrid *grid, int row, const char *label) {
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    GtkWidget *value = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(value), FALSE);
    gtk_widget_set_can_focus(value, FALSE);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(grid, l, 0, row, 1, 1);
    gtk_grid_attach(grid, value, 1, row, 1, 1);
    return value;
}

static GtkWidget *regional_page(props *p) {
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
    p->formats = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(choose), p->formats, TRUE, TRUE, 0);
    p->customize = gtk_button_new_with_mnemonic("C_ustomize...");
    g_signal_connect(p->customize, "clicked", G_CALLBACK(on_customize), p);
    gtk_box_pack_start(GTK_BOX(choose), p->customize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), choose, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    p->number = sample_row(GTK_GRID(grid), 0, "Number:");
    p->currency = sample_row(GTK_GRID(grid), 1, "Currency:");
    p->time = sample_row(GTK_GRID(grid), 2, "Time:");
    p->short_date = sample_row(GTK_GRID(grid), 3, "Short date:");
    p->long_date = sample_row(GTK_GRID(grid), 4, "Long date:");
    gtk_box_pack_start(GTK_BOX(box), frame("Samples", grid), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Standards and formats", box), FALSE, FALSE, 0);

    /* Filled in on Apply; empty, it keeps its line so the window doesn't
     * grow. */
    p->note = text("");
    gtk_box_pack_start(GTK_BOX(page), p->note, FALSE, FALSE, 0);

    /* The formats as set: ours, else what this session got. */
    char *path = formats_path();
    char *set = xd_intl_read_formats(path);
    g_free(path);
    if (set == nullptr)
        set = g_strdup(setlocale(LC_TIME, nullptr));
    p->user_formats = xd_intl_custom_base(set);
    g_free(set);
    /* The customization, if it's of these formats. */
    path = custom_path();
    char *of = nullptr;
    xd_intl_custom_load(path, &of, &p->custom);
    g_free(path);
    if (g_strcmp0(of, p->user_formats) != 0)
        xd_intl_custom_clear(&p->custom);
    g_free(of);
    fill_locales(p, p->formats, p->user_formats);
    g_signal_connect(p->formats, "changed", G_CALLBACK(on_formats), p);
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

static void on_details(GtkButton *b, [[maybe_unused]] gpointer data) {
    char *exe = find_own("xd-keyboard");
    const char *argv[] = {exe, nullptr};
    GError *error = nullptr;
    if (exe == nullptr || !g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr,
                                         G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error)) {
        xd_message_box(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(b))),
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

static GVariant *l1_prop(props *p, const char *name) {
    return p->l1 != nullptr ? g_dbus_proxy_get_cached_property(p->l1, name) : nullptr;
}

static char *l1_string(props *p, const char *name) {
    GVariant *v = l1_prop(p, name);
    char *s = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                  ? g_variant_dup_string(v, nullptr)
                  : g_strdup("");
    if (v != nullptr)
        g_variant_unref(v);
    return s;
}

static char **l1_locale(props *p) {
    GVariant *v = l1_prop(p, "Locale");
    char **l = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)
                   ? g_variant_dup_strv(v, nullptr)
                   : g_new0(char *, 1);
    if (v != nullptr)
        g_variant_unref(v);
    return l;
}

static int compare_text(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b,
                        [[maybe_unused]] gpointer data) {
    char *x = nullptr, *y = nullptr, *xid = nullptr, *yid = nullptr;
    gtk_tree_model_get(m, a, 0, &x, 1, &xid, -1);
    gtk_tree_model_get(m, b, 0, &y, 1, &yid, -1);
    /* The empty id, "(Standard)", goes first. */
    bool xs = xid == nullptr || xid[0] == '\0', ys = yid == nullptr || yid[0] == '\0';
    int r = xs != ys ? (xs ? -1 : 1) : g_utf8_collate(x != nullptr ? x : "", y != nullptr ? y : "");
    g_free(x);
    g_free(y);
    g_free(xid);
    g_free(yid);
    return r;
}

/* Combo boxes sorted by their text (GtkComboBoxText's columns: text, id). */
static GtkWidget *sorted_combo(void) {
    GtkWidget *c = gtk_combo_box_text_new();
    GtkTreeSortable *s = GTK_TREE_SORTABLE(gtk_combo_box_get_model(GTK_COMBO_BOX(c)));
    gtk_tree_sortable_set_sort_func(s, 0, compare_text, nullptr, nullptr);
    gtk_tree_sortable_set_sort_column_id(s, 0, GTK_SORT_ASCENDING);
    return c;
}

/* The variants of the chosen layout; "" is the layout itself. */
static void fill_variants(props *p, const char *selected) {
    const char *layout = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->layout));
    p->updating = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->variant));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->variant), "", "(Standard)");
    bool found = selected == nullptr || selected[0] == '\0';
    for (struct rxkb_layout *l = p->xkb != nullptr ? rxkb_layout_first(p->xkb) : nullptr;
         l != nullptr && layout != nullptr; l = rxkb_layout_next(l)) {
        const char *v = rxkb_layout_get_variant(l);
        if (v == nullptr || strcmp(rxkb_layout_get_name(l), layout) != 0)
            continue;
        const char *desc = rxkb_layout_get_description(l);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->variant), v, desc != nullptr ? desc : v);
        found = found || strcmp(v, selected) == 0;
    }
    if (!found) /* not one xkeyboard-config knows; keep it */
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->variant), selected, selected);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->variant), selected != nullptr ? selected : "");
    p->updating = false;
}

static void on_layout([[maybe_unused]] GtkComboBox *c, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    fill_variants(p, "");
    changed(p);
}

static void on_changed_widget([[maybe_unused]] GtkWidget *w, gpointer data) { changed(data); }

static void show_console(props *p) {
    char *keymap = l1_string(p, "VConsoleKeymap");
    char *s = g_strdup_printf("Text console keymap: %s",
                              keymap[0] != '\0' ? keymap : "(the kernel's default)");
    gtk_label_set_text(GTK_LABEL(p->console), s);
    g_free(s);
    g_free(keymap);
}

static void show_keyboard(props *p) {
    char *layouts = l1_string(p, "X11Layout"), *variants = l1_string(p, "X11Variant");
    char *layout = xd_intl_first(layouts), *variant = xd_intl_first(variants);
    p->updating = true;
    if (layout[0] != '\0' && !gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->layout), layout)) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->layout), layout, layout);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->layout), layout);
    }
    p->updating = false;
    fill_variants(p, variant);
    show_console(p);
    g_free(layouts);
    g_free(variants);
    g_free(layout);
    g_free(variant);
}

static GtkWidget *advanced_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(box),
                       text("The system locale is the language and formats of the logon "
                            "screen, system services, and programs that don't choose their "
                            "own. Select the one that matches the language of your programs:"),
                       FALSE, FALSE, 0);
    p->system_locale = gtk_combo_box_text_new();
    char **locale = l1_locale(p);
    fill_locales(p, p->system_locale, xd_intl_lang((const char *const *)locale));
    g_strfreev(locale);
    g_signal_connect(p->system_locale, "changed", G_CALLBACK(on_changed_widget), p);
    gtk_box_pack_start(GTK_BOX(box), p->system_locale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("System locale", box), FALSE, FALSE, 0);

    GtkWidget *kb = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(kb), 8);
    gtk_grid_set_column_spacing(GTK_GRID(kb), 12);
    GtkWidget *what = text("The keyboard layout of the logon screen and of new user accounts:");
    gtk_grid_attach(GTK_GRID(kb), what, 0, 0, 2, 1);
    p->layout = sorted_combo();
    p->variant = sorted_combo();
    gtk_widget_set_hexpand(p->layout, TRUE);
    p->xkb = rxkb_context_new(RXKB_CONTEXT_NO_FLAGS);
    if (p->xkb != nullptr && !rxkb_context_parse_default_ruleset(p->xkb)) {
        rxkb_context_unref(p->xkb);
        p->xkb = nullptr;
    }
    for (struct rxkb_layout *l = p->xkb != nullptr ? rxkb_layout_first(p->xkb) : nullptr;
         l != nullptr; l = rxkb_layout_next(l)) {
        if (rxkb_layout_get_variant(l) != nullptr)
            continue;
        const char *desc = rxkb_layout_get_description(l);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->layout), rxkb_layout_get_name(l),
                                  desc != nullptr ? desc : rxkb_layout_get_name(l));
    }
    GtkWidget *ll = gtk_label_new_with_mnemonic("_Layout:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(ll), p->layout);
    gtk_label_set_xalign(GTK_LABEL(ll), 0);
    GtkWidget *vl = gtk_label_new_with_mnemonic("_Variant:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(vl), p->variant);
    gtk_label_set_xalign(GTK_LABEL(vl), 0);
    gtk_grid_attach(GTK_GRID(kb), ll, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(kb), p->layout, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(kb), vl, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(kb), p->variant, 1, 2, 1, 1);
    p->convert = gtk_check_button_new_with_mnemonic(
        "Use the _closest keymap on text consoles too");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->convert), TRUE);
    gtk_grid_attach(GTK_GRID(kb), p->convert, 0, 3, 2, 1);
    p->console = text("");
    gtk_grid_attach(GTK_GRID(kb), p->console, 0, 4, 2, 1);
    gtk_box_pack_start(GTK_BOX(page), frame("System keyboard", kb), FALSE, FALSE, 0);
    show_keyboard(p);
    g_signal_connect(p->layout, "changed", G_CALLBACK(on_layout), p);
    g_signal_connect(p->variant, "changed", G_CALLBACK(on_changed_widget), p);

    if (p->l1 == nullptr) {
        gtk_widget_set_sensitive(p->system_locale, FALSE);
        gtk_widget_set_sensitive(kb, FALSE);
        gtk_box_pack_start(GTK_BOX(page),
                           text("The system settings can't be changed: systemd-localed isn't "
                                "available."),
                           FALSE, FALSE, 0);
    }
    return page;
}

/* ---- applying -------------------------------------------------------------------- */

static void error_box(props *p, const char *what, const char *detail) {
    char *s = g_strdup_printf("%s\n\n%s", what, detail);
    xd_message_box(GTK_WINDOW(p->window), "Regional and Language Options", XD_MB_ERROR,
                   XD_MB_OK, FALSE, s);
    g_free(s);
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
    pending_call *c = data;
    c->result = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &c->error);
    g_main_loop_quit(c->loop);
}

static bool call(props *p, const char *method, GVariant *args, const char *what) {
    pending_call c = {.loop = g_main_loop_new(nullptr, FALSE)};
    p->busy = true;
    gtk_widget_set_sensitive(p->window, FALSE);
    g_dbus_proxy_call(p->l1, method, args, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, nullptr, call_done,
                      &c);
    g_main_loop_run(c.loop);
    g_main_loop_unref(c.loop);
    gtk_widget_set_sensitive(p->window, TRUE);
    p->busy = false;
    if (c.result == nullptr) {
        g_dbus_error_strip_remote_error(c.error);
        error_box(p, what, c.error->message);
        g_error_free(c.error);
        return false;
    }
    g_variant_unref(c.result);
    return true;
}

/* Nor may the window be destroyed under Apply. */
static gboolean on_delete([[maybe_unused]] GtkWidget *w, [[maybe_unused]] GdkEvent *e,
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
    GFileEnumerator *e = g_file_enumerate_children(
        dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    GFileInfo *info;
    while (e != nullptr && (info = g_file_enumerator_next_file(e, nullptr, nullptr)) != nullptr) {
        GFile *child = g_file_get_child(dir, g_file_info_get_name(info));
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
            remove_tree(child);
        else
            g_file_delete(child, nullptr, nullptr);
        g_object_unref(child);
        g_object_unref(info);
    }
    g_clear_object(&e);
    g_file_delete(dir, nullptr, nullptr);
}

static void remove_path(const char *path) {
    GFile *f = g_file_new_for_path(path);
    if (g_file_query_exists(f, nullptr))
        remove_tree(f);
    g_object_unref(f);
}

/* localedef, waited for the way localed is (call()). */
typedef struct pending_run {
    GMainLoop *loop;
    bool ok;
    char *why;
} pending_run;

static void run_done(GObject *src, GAsyncResult *res, gpointer data) {
    pending_run *r = data;
    GSubprocess *sp = G_SUBPROCESS(src);
    GError *error = nullptr;
    char *err = nullptr;
    if (g_subprocess_communicate_utf8_finish(sp, res, nullptr, &err, &error)) {
        /* 1 is warnings only: the locale is written. */
        r->ok = g_subprocess_get_if_exited(sp) && g_subprocess_get_exit_status(sp) <= 1;
        r->why = err;
    } else {
        r->why = g_strdup(error->message);
        g_error_free(error);
    }
    g_main_loop_quit(r->loop);
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
    pending_run r = {.loop = g_main_loop_new(nullptr, FALSE)};
    g_subprocess_communicate_utf8_async(sp, nullptr, nullptr, run_done, &r);
    g_main_loop_run(r.loop);
    g_main_loop_unref(r.loop);
    g_object_unref(sp);
    char *check = g_build_filename(out, "LC_NUMERIC", nullptr);
    r.ok = r.ok && g_file_test(check, G_FILE_TEST_IS_REGULAR);
    g_free(check);
    if (r.ok)
        g_free(r.why);
    else
        *why = r.why != nullptr && r.why[0] != '\0' ? r.why : g_strdup("localedef failed.");
    return r.ok;
}

/* Built in, or not a name we'd find: nothing to compile. */
static bool needs_compiling(const char *name) {
    return name != nullptr && name[0] != '\0' && strcmp(name, "C") != 0 &&
           strcmp(name, "POSIX") != 0 && g_ascii_strncasecmp(name, "C.", 2) != 0 &&
           !g_str_has_suffix(name, "@xeondesktop") && xd_intl_valid_name(name);
}

/* The customized formats as a compiled locale in `dir`, and the
 * session's language beside it (LOCPATH hides the system's). */
static bool compile_custom(props *p, const char *id, const char *dir, char **why) {
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
    char *text = ok ? xd_intl_custom_source(id, &p->custom) : nullptr;
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

static bool apply_formats(props *p) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->formats));
    if (id == nullptr || (g_strcmp0(id, p->user_formats) == 0 && !p->custom_changed))
        return true;
    char *path = formats_path();
    char *cpath = custom_path();
    GError *error = nullptr;
    char *why = nullptr;
    bool ok;
    if (xd_intl_custom_empty(&p->custom)) {
        /* The compiled locales stay: programs of this session use them
         * until you log off. */
        ok = xd_intl_write_formats(path, id, &error) &&
             xd_intl_custom_save(cpath, nullptr, &p->custom, &error);
    } else {
        char *dir = locale_dir();
        p->busy = true;
        gtk_widget_set_sensitive(p->window, FALSE);
        GdkWindow *win = gtk_widget_get_window(p->window);
        GdkCursor *wait = win != nullptr ? gdk_cursor_new_from_name(gdk_window_get_display(win),
                                                                    "wait")
                                         : nullptr;
        if (win != nullptr)
            gdk_window_set_cursor(win, wait);
        ok = compile_custom(p, id, dir, &why);
        if (win != nullptr)
            gdk_window_set_cursor(win, nullptr);
        g_clear_object(&wait);
        gtk_widget_set_sensitive(p->window, TRUE);
        p->busy = false;
        if (ok) {
            char *name = xd_intl_custom_name(id);
            ok = xd_intl_write_formats_full(path, name, dir, &error) &&
                 xd_intl_custom_save(cpath, id, &p->custom, &error);
            g_free(name);
        }
        g_free(dir);
    }
    g_free(path);
    g_free(cpath);
    if (!ok) {
        error_box(p, "Your formats could not be saved.",
                  why != nullptr ? why : error != nullptr ? error->message : "");
        g_clear_error(&error);
        g_free(why);
        return false;
    }
    g_free(p->user_formats);
    p->user_formats = g_strdup(id);
    p->custom_changed = false;
    gtk_label_set_text(GTK_LABEL(p->note),
                       "Programs use the new formats after you log off and log on again.");
    return true;
}

static bool apply_system(props *p) {
    if (p->l1 == nullptr)
        return true;
    bool ok = true;
    const char *lang = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->system_locale));
    char **locale = l1_locale(p);
    if (lang != nullptr && g_strcmp0(lang, xd_intl_lang((const char *const *)locale)) != 0) {
        char **set = xd_intl_replace_lang((const char *const *)locale, lang);
        ok = call(p, "SetLocale", g_variant_new("(^asb)", set, TRUE),
                  "The system locale could not be changed.");
        g_strfreev(set);
    }
    g_strfreev(locale);

    const char *layout = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->layout));
    const char *variant = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->variant));
    char *layouts = l1_string(p, "X11Layout"), *variants = l1_string(p, "X11Variant");
    char *old_layout = xd_intl_first(layouts), *old_variant = xd_intl_first(variants);
    if (ok && layout != nullptr &&
        (strcmp(layout, old_layout) != 0 || g_strcmp0(variant, old_variant) != 0)) {
        /* Other layouts after the first only go with their variants. */
        char *new_layouts = xd_intl_replace_first(layouts, layout);
        char *new_variants = xd_intl_replace_first(variants, variant != nullptr ? variant : "");
        char *model = l1_string(p, "X11Model"), *options = l1_string(p, "X11Options");
        bool convert = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->convert));
        ok = call(p, "SetX11Keyboard",
                  g_variant_new("(ssssbb)", new_layouts, model, new_variants, options, convert,
                                TRUE),
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

static bool apply(props *p) {
    bool ok = apply_formats(p) && apply_system(p);
    if (ok)
        gtk_widget_set_sensitive(p->apply, FALSE);
    return ok;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (p->busy)
        return; /* the window's close button, while Apply waits */
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !apply(p))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void l1_changed([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *c,
                       [[maybe_unused]] const char *const *inv, gpointer data) {
    props *p = data;
    /* Only what can't have been edited here; the rest is ours until
     * applied or cancelled. */
    show_console(p);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.RegionalOptions");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    /* GTK has set this program's locale, customized formats and all;
     * from here on the system's compiled locales are the ones listed and
     * sampled, which LOCPATH would hide. */
    g_unsetenv("LOCPATH");
    static props p;
    GError *error = nullptr;
    p.l1 = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                         LOCALE1, LOCALE1_PATH, LOCALE1, nullptr, &error);
    if (p.l1 == nullptr) {
        g_printerr("xd-intl: %s\n", error->message);
        g_error_free(error);
    } else {
        /* A proxy is made even when nothing answers; no properties means
         * no localed. */
        char **names = g_dbus_proxy_get_cached_property_names(p.l1);
        if (names == nullptr) {
            g_printerr("xd-intl: %s isn't available\n", LOCALE1);
            g_clear_object(&p.l1);
        }
        g_strfreev(names);
    }
    p.locales = xd_intl_installed_locales(SUPPORTED);

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), "Regional and Language Options");
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "preferences-desktop-locale");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), regional_page(&p),
                             gtk_label_new_with_mnemonic("_Regional Options"));
    /* Input languages are the Keyboard applet's; the tab is only a way
     * there. */
    char *keyboard = find_own("xd-keyboard");
    if (keyboard != nullptr)
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), languages_page(),
                                 gtk_label_new_with_mnemonic("_Languages"));
    g_free(keyboard);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), advanced_page(&p),
                             gtk_label_new_with_mnemonic("Ad_vanced"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));
    show_samples(&p);
    if (p.l1 != nullptr)
        g_signal_connect(p.l1, "g-properties-changed", G_CALLBACK(l1_changed), &p);
    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "delete-event", G_CALLBACK(on_delete), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_main();
    if (p.xkb != nullptr)
        rxkb_context_unref(p.xkb);
    g_strfreev(p.locales);
    g_free(p.user_formats);
    xd_intl_custom_clear(&p.custom);
    g_clear_object(&p.l1);
    return EXIT_SUCCESS;
}
