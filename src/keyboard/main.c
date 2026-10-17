/* SPDX-License-Identifier: MIT
 * xd-keyboard: Windows XP's Keyboard Properties, with the input languages
 * XP kept in Regional and Language Options. Writes [Keyboard] in
 * desktop.ini, which the compositor applies to every keyboard (input.c).
 *
 *   Speed            repeat delay and rate (with XP's test box), cursor
 *                    blink rate
 *   Input Languages  up to four XKB layouts and variants, the keys that
 *                    switch between them, other XKB options
 *   Hardware         the keyboard model, the keyboards udev knows
 *
 * The lists of layouts, variants, options and models come from
 * xkeyboard-config through libxkbregistry.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbregistry.h>

#include "../core/common.h"
#include "../cpl/inputdevs.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "layouts.h"

enum { LAYOUT_DESC, LAYOUT_NAME, LAYOUT_VARIANT, LAYOUT_COLS };
enum { OPT_TEXT, OPT_NAME, OPT_ACTIVE, OPT_IS_OPTION, OPT_GROUP_MULTI, OPT_COLS };

typedef struct props {
    GtkWidget *window, *apply;
    xd_keyboard_settings kb; /* what the widgets show */
    bool updating;
    struct rxkb_context *xkb; /* nullptr if xkeyboard-config couldn't be read */
    /* Speed */
    GtkWidget *delay, *rate, *test, *blink, *caret;
    guint held_key;            /* the test box's key being held */
    GdkEvent *held_event;      /* ... to type again */
    guint repeat_timer;        /* the test box's own repeat */
    bool replaying;            /* the test box gets a key we send it */
    bool caret_on;
    guint caret_timer;
    /* Input Languages */
    GtkListStore *layouts;
    GtkWidget *layout_view, *add, *remove, *up, *down, *switch_keys, *options_label;
    /* Hardware */
    GtkWidget *model;
} props;

static void changed(props *p) {
    if (!p->updating)
        gtk_widget_set_sensitive(p->apply, TRUE);
}

/* A slider's notch. */
static int range_int(GtkRange *r) {
    double v = gtk_range_get_value(r);
    return (int)lround(v);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *wrapped(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
    return l;
}

/* "Title:  Left [-----|-----] Right" */
static GtkWidget *slider_row(const char *title, const char *left, GtkWidget *scale,
                             const char *right) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *label = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), scale);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_size_request(scale, 180, -1);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(left), FALSE, FALSE, 12);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(right), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *ticked_scale(double lo, double hi, double step, double tick) {
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    for (double v = lo; v <= hi; v += tick)
        gtk_scale_add_mark(GTK_SCALE(s), v, GTK_POS_BOTTOM, nullptr);
    gtk_range_set_round_digits(GTK_RANGE(s), 0);
    return s;
}

/* ---- Speed ------------------------------------------------------------------------- */

/* XP's four repeat delays, long to short. */
static int delay_ms(int notch) { return 1000 - notch * 250; }

/* Blink rate notches: 0 is no blinking, 1..10 slow (1200 ms) to fast. */
static int blink_ms(int notch) { return notch == 0 ? 0 : 1300 - notch * 100; }

static void stop_repeat(props *p) {
    if (p->repeat_timer != 0)
        g_source_remove(p->repeat_timer);
    p->repeat_timer = 0;
    p->held_key = 0;
    g_clear_pointer(&p->held_event, gdk_event_free);
}

static gboolean repeat_key(gpointer data) {
    props *p = data;
    p->replaying = true;
    gtk_widget_event(p->test, p->held_event);
    p->replaying = false;
    return G_SOURCE_CONTINUE;
}

static gboolean start_repeat(gpointer data) {
    props *p = data;
    repeat_key(p);
    p->repeat_timer = p->kb.repeat_rate > 0
                          ? g_timeout_add((guint)(1000 / p->kb.repeat_rate), repeat_key, p)
                          : 0;
    return G_SOURCE_REMOVE;
}

/* The test box repeats a held key itself, at the delay and rate the
 * sliders show (before they're applied), ignoring the repeats the
 * compositor's settings make. */
static gboolean on_test_press([[maybe_unused]] GtkWidget *w, GdkEventKey *e, gpointer data) {
    props *p = data;
    if (p->replaying || e->is_modifier)
        return FALSE;
    if (p->held_key == e->hardware_keycode)
        return TRUE;
    stop_repeat(p);
    p->held_key = e->hardware_keycode;
    p->held_event = gdk_event_copy((GdkEvent *)e);
    p->repeat_timer = g_timeout_add((guint)p->kb.repeat_delay, start_repeat, p);
    return FALSE;
}

static gboolean on_test_release([[maybe_unused]] GtkWidget *w, GdkEventKey *e, gpointer data) {
    props *p = data;
    if (p->held_key == e->hardware_keycode)
        stop_repeat(p);
    return FALSE;
}

static gboolean on_test_focus_out([[maybe_unused]] GtkWidget *w,
                                  [[maybe_unused]] GdkEventFocus *e, gpointer data) {
    stop_repeat(data);
    return FALSE;
}

static void on_delay(GtkRange *r, gpointer data) {
    props *p = data;
    p->kb.repeat_delay = delay_ms(range_int(r));
    changed(p);
}

static void on_rate(GtkRange *r, gpointer data) {
    props *p = data;
    p->kb.repeat_rate = range_int(r);
    changed(p);
}

static gboolean draw_caret(GtkWidget *w, cairo_t *cr, gpointer data) {
    props *p = data;
    if (!p->caret_on)
        return FALSE;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    gdk_cairo_set_source_rgba(cr, &fg);
    int h = gtk_widget_get_allocated_height(w);
    cairo_rectangle(cr, 2, 2, 1, h - 4);
    cairo_fill(cr);
    return FALSE;
}

static gboolean blink_caret(gpointer data) {
    props *p = data;
    p->caret_on = !p->caret_on;
    gtk_widget_queue_draw(p->caret);
    return G_SOURCE_CONTINUE;
}

static void restart_caret(props *p) {
    if (p->caret_timer != 0)
        g_source_remove(p->caret_timer);
    p->caret_timer = 0;
    p->caret_on = true;
    gtk_widget_queue_draw(p->caret);
    if (p->kb.cursor_blink_ms > 0)
        p->caret_timer = g_timeout_add((guint)p->kb.cursor_blink_ms, blink_caret, p);
}

static void on_blink(GtkRange *r, gpointer data) {
    props *p = data;
    p->kb.cursor_blink_ms = blink_ms(range_int(r));
    restart_caret(p);
    changed(p);
}

static GtkWidget *speed_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *repeat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    p->delay = ticked_scale(0, 3, 1, 1);
    int notch = (int)lround((1000 - p->kb.repeat_delay) / 250.0);
    gtk_range_set_value(GTK_RANGE(p->delay), XD_CLAMP(notch, 0, 3));
    g_signal_connect(p->delay, "value-changed", G_CALLBACK(on_delay), p);
    gtk_box_pack_start(GTK_BOX(repeat), slider_row("Repeat _delay:", "Long", p->delay, "Short"),
                       FALSE, FALSE, 0);
    p->rate = ticked_scale(2, 40, 1, 2);
    gtk_range_set_value(GTK_RANGE(p->rate), XD_CLAMP(p->kb.repeat_rate, 2, 40));
    g_signal_connect(p->rate, "value-changed", G_CALLBACK(on_rate), p);
    gtk_box_pack_start(GTK_BOX(repeat), slider_row("Repeat _rate:", "Slow", p->rate, "Fast"),
                       FALSE, FALSE, 0);
    GtkWidget *test_label =
        gtk_label_new_with_mnemonic("Click here and hold down a key to _test repeat rate:");
    gtk_label_set_xalign(GTK_LABEL(test_label), 0);
    gtk_box_pack_start(GTK_BOX(repeat), test_label, FALSE, FALSE, 0);
    p->test = gtk_entry_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(test_label), p->test);
    g_signal_connect(p->test, "key-press-event", G_CALLBACK(on_test_press), p);
    g_signal_connect(p->test, "key-release-event", G_CALLBACK(on_test_release), p);
    g_signal_connect(p->test, "focus-out-event", G_CALLBACK(on_test_focus_out), p);
    gtk_box_pack_start(GTK_BOX(repeat), p->test, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Character repeat", repeat), FALSE, FALSE, 0);

    GtkWidget *blink = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    p->caret = gtk_drawing_area_new();
    gtk_widget_set_size_request(p->caret, 8, 24);
    gtk_widget_set_valign(p->caret, GTK_ALIGN_CENTER);
    g_signal_connect(p->caret, "draw", G_CALLBACK(draw_caret), p);
    gtk_box_pack_start(GTK_BOX(blink), p->caret, FALSE, FALSE, 0);
    p->blink = ticked_scale(0, 10, 1, 1);
    int b = p->kb.cursor_blink_ms;
    gtk_range_set_value(GTK_RANGE(p->blink),
                        b == 0 ? 0 : XD_CLAMP((int)lround((1300 - b) / 100.0), 1, 10));
    g_signal_connect(p->blink, "value-changed", G_CALLBACK(on_blink), p);
    gtk_box_pack_start(GTK_BOX(blink), slider_row("Cursor _blink rate:", "None", p->blink, "Fast"),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Cursor blink rate", blink), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       wrapped("The blink rate applies to XeonDesktop's own programs."), FALSE,
                       FALSE, 0);
    restart_caret(p);
    return page;
}

/* ---- Input Languages -------------------------------------------------------------------- */

static struct rxkb_option_group *first_group(props *p) {
    return p->xkb != nullptr ? rxkb_option_group_first(p->xkb) : nullptr;
}

/* xkeyboard-config's name for a layout and variant, else the codes. */
static char *layout_description(props *p, const char *name, const char *variant) {
    for (struct rxkb_layout *l = p->xkb != nullptr ? rxkb_layout_first(p->xkb) : nullptr;
         l != nullptr; l = rxkb_layout_next(l)) {
        const char *v = rxkb_layout_get_variant(l);
        if (strcmp(rxkb_layout_get_name(l), name) == 0 &&
            g_strcmp0(v != nullptr ? v : "", variant) == 0)
            return g_strdup(rxkb_layout_get_description(l));
    }
    return variant[0] != '\0' ? g_strdup_printf("%s (%s)", name, variant) : g_strdup(name);
}

static void add_layout_row(props *p, const char *name, const char *variant) {
    char *desc = layout_description(p, name, variant);
    gtk_list_store_insert_with_values(p->layouts, nullptr, -1, LAYOUT_DESC, desc, LAYOUT_NAME,
                                      name, LAYOUT_VARIANT, variant, -1);
    g_free(desc);
}

static int layout_count(props *p) {
    return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(p->layouts), nullptr);
}

static void update_layout_buttons(props *p) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->layout_view));
    GtkTreeIter it;
    bool selected = gtk_tree_selection_get_selected(sel, nullptr, &it);
    int index = -1;
    if (selected) {
        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(p->layouts), &it);
        index = gtk_tree_path_get_indices(path)[0];
        gtk_tree_path_free(path);
    }
    int n = layout_count(p);
    gtk_widget_set_sensitive(p->add, n < XD_KBD_LAYOUTS_MAX);
    gtk_widget_set_sensitive(p->remove, selected && n > 1);
    gtk_widget_set_sensitive(p->up, selected && index > 0);
    gtk_widget_set_sensitive(p->down, selected && index < n - 1);
}

static void on_layout_selected([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    update_layout_buttons(data);
}

/* The Add dialog's variant list for the layout picked. */
static void fill_variants(props *p, GtkWidget *variants, const char *layout) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(variants));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(variants), "", "(Default)");
    GPtrArray *found = g_ptr_array_new();
    for (struct rxkb_layout *l = p->xkb != nullptr ? rxkb_layout_first(p->xkb) : nullptr;
         l != nullptr; l = rxkb_layout_next(l)) {
        if (layout != nullptr && rxkb_layout_get_variant(l) != nullptr &&
            strcmp(rxkb_layout_get_name(l), layout) == 0)
            g_ptr_array_add(found, l);
    }
    for (guint i = 0; i < found->len; i++) {
        struct rxkb_layout *l = g_ptr_array_index(found, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(variants), rxkb_layout_get_variant(l),
                                  rxkb_layout_get_description(l));
    }
    g_ptr_array_free(found, TRUE);
    gtk_combo_box_set_active(GTK_COMBO_BOX(variants), 0);
}

static void on_add_layout_changed(GtkComboBox *c, gpointer variants) {
    props *p = g_object_get_data(G_OBJECT(c), "props");
    fill_variants(p, variants, gtk_combo_box_get_active_id(c));
}

static int by_description(gconstpointer a, gconstpointer b) {
    struct rxkb_layout *x = *(struct rxkb_layout *const *)a;
    struct rxkb_layout *y = *(struct rxkb_layout *const *)b;
    return g_utf8_collate(rxkb_layout_get_description(x), rxkb_layout_get_description(y));
}

static void on_add([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Add Input Layout", GTK_WINDOW(p->window),
                                                 GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK,
                                                 "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    GtkWidget *layouts = gtk_combo_box_text_new();
    GtkWidget *variants = gtk_combo_box_text_new();
    gtk_widget_set_size_request(layouts, 280, -1);
    GPtrArray *all = g_ptr_array_new();
    for (struct rxkb_layout *l = p->xkb != nullptr ? rxkb_layout_first(p->xkb) : nullptr;
         l != nullptr; l = rxkb_layout_next(l)) {
        if (rxkb_layout_get_variant(l) == nullptr)
            g_ptr_array_add(all, l);
    }
    g_ptr_array_sort(all, by_description);
    for (guint i = 0; i < all->len; i++) {
        struct rxkb_layout *l = g_ptr_array_index(all, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(layouts), rxkb_layout_get_name(l),
                                  rxkb_layout_get_description(l));
    }
    g_ptr_array_free(all, TRUE);
    g_object_set_data(G_OBJECT(layouts), "props", p);
    g_signal_connect(layouts, "changed", G_CALLBACK(on_add_layout_changed), variants);
    gtk_combo_box_set_active(GTK_COMBO_BOX(layouts), 0);

    GtkWidget *l1 = gtk_label_new_with_mnemonic("_Keyboard layout:");
    gtk_label_set_xalign(GTK_LABEL(l1), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l1), layouts);
    GtkWidget *l2 = gtk_label_new_with_mnemonic("_Variant:");
    gtk_label_set_xalign(GTK_LABEL(l2), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l2), variants);
    gtk_grid_attach(GTK_GRID(grid), l1, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), layouts, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l2, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), variants, 1, 1, 1, 1);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), grid);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *name = gtk_combo_box_get_active_id(GTK_COMBO_BOX(layouts));
        const char *variant = gtk_combo_box_get_active_id(GTK_COMBO_BOX(variants));
        if (name != nullptr && layout_count(p) < XD_KBD_LAYOUTS_MAX) {
            add_layout_row(p, name, variant != nullptr ? variant : "");
            changed(p);
        }
    }
    gtk_widget_destroy(dlg);
    update_layout_buttons(p);
}

static void on_remove([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->layout_view));
    GtkTreeIter it;
    if (layout_count(p) > 1 && gtk_tree_selection_get_selected(sel, nullptr, &it)) {
        gtk_list_store_remove(p->layouts, &it);
        changed(p);
    }
    update_layout_buttons(p);
}

static void move_layout(props *p, bool up) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->layout_view));
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, nullptr, &it))
        return;
    GtkTreeIter other = it;
    bool ok = up ? gtk_tree_model_iter_previous(GTK_TREE_MODEL(p->layouts), &other)
                 : gtk_tree_model_iter_next(GTK_TREE_MODEL(p->layouts), &other);
    if (ok) {
        gtk_list_store_swap(p->layouts, &it, &other);
        changed(p);
    }
    update_layout_buttons(p);
}

static void on_up([[maybe_unused]] GtkButton *b, gpointer data) { move_layout(data, true); }

static void on_down([[maybe_unused]] GtkButton *b, gpointer data) { move_layout(data, false); }

static void on_switch_keys(GtkComboBox *c, gpointer data) {
    props *p = data;
    if (p->updating)
        return;
    const char *id = gtk_combo_box_get_active_id(c);
    char *opts = xd_kbd_options_clear_group(p->kb.options, "grp");
    char *with = xd_kbd_option_set(opts, id != nullptr ? id : "", true);
    g_strlcpy(p->kb.options, with, sizeof(p->kb.options));
    g_free(with);
    g_free(opts);
    changed(p);
}

static void show_options_count(props *p) {
    /* Everything but the switching keys, which have their own list. */
    char *rest = xd_kbd_options_clear_group(p->kb.options, "grp");
    guint n = 0;
    gchar **list = g_strsplit(rest, ",", -1);
    for (guint i = 0; list[i] != nullptr; i++)
        n += list[i][0] != '\0';
    g_strfreev(list);
    g_free(rest);
    char *text = n == 0   ? g_strdup("No other key options are set.")
                 : n == 1 ? g_strdup("1 other key option is set.")
                          : g_strdup_printf("%u other key options are set.", n);
    gtk_label_set_text(GTK_LABEL(p->options_label), text);
    g_free(text);
}

static void on_option_toggled([[maybe_unused]] GtkCellRendererToggle *r, char *path,
                              gpointer store) {
    GtkTreeModel *m = store;
    GtkTreeIter it, parent, sib;
    if (!gtk_tree_model_get_iter_from_string(m, &it, path))
        return;
    gboolean active, multi;
    gtk_tree_model_get(m, &it, OPT_ACTIVE, &active, OPT_GROUP_MULTI, &multi, -1);
    /* A group that allows one option at a time works like radio buttons
     * that can all be off. */
    if (!active && !multi && gtk_tree_model_iter_parent(m, &parent, &it) &&
        gtk_tree_model_iter_children(m, &sib, &parent)) {
        do {
            gtk_tree_store_set(store, &sib, OPT_ACTIVE, FALSE, -1);
        } while (gtk_tree_model_iter_next(m, &sib));
    }
    gtk_tree_store_set(store, &it, OPT_ACTIVE, !active, -1);
}

static void on_options([[maybe_unused]] GtkButton *b, gpointer data) {
    props *p = data;
    GtkTreeStore *store = gtk_tree_store_new(OPT_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    gchar **current = g_strsplit(p->kb.options, ",", -1);
    for (struct rxkb_option_group *g = first_group(p); g != nullptr;
         g = rxkb_option_group_next(g)) {
        if (strcmp(rxkb_option_group_get_name(g), "grp") == 0)
            continue;
        GtkTreeIter gi;
        bool multi = rxkb_option_group_allows_multiple(g);
        gtk_tree_store_insert_with_values(store, &gi, nullptr, -1, OPT_TEXT,
                                          rxkb_option_group_get_description(g), OPT_IS_OPTION,
                                          FALSE, -1);
        for (struct rxkb_option *o = rxkb_option_first(g); o != nullptr; o = rxkb_option_next(o)) {
            const char *name = rxkb_option_get_name(o);
            gtk_tree_store_insert_with_values(
                store, nullptr, &gi, -1, OPT_TEXT, rxkb_option_get_description(o), OPT_NAME, name,
                OPT_ACTIVE, g_strv_contains((const char *const *)current, name), OPT_IS_OPTION,
                TRUE, OPT_GROUP_MULTI, multi, -1);
        }
    }
    g_strfreev(current);

    GtkWidget *dlg = gtk_dialog_new_with_buttons("Key Options", GTK_WINDOW(p->window),
                                                 GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK,
                                                 "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 520, 440);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_option_toggled), store);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col, toggle, FALSE);
    gtk_tree_view_column_add_attribute(col, toggle, "active", OPT_ACTIVE);
    gtk_tree_view_column_add_attribute(col, toggle, "visible", OPT_IS_OPTION);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", OPT_TEXT);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_pack_start(GTK_BOX(box), wrapped("Options from xkeyboard-config for every layout:"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box, TRUE, TRUE,
                       0);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        /* Options the list doesn't have (written by hand) stay. */
        char *opts = g_strdup(p->kb.options);
        GtkTreeModel *m = GTK_TREE_MODEL(store);
        GtkTreeIter gi, oi;
        for (bool g = gtk_tree_model_get_iter_first(m, &gi); g;
             g = gtk_tree_model_iter_next(m, &gi)) {
            for (bool o = gtk_tree_model_iter_children(m, &oi, &gi); o;
                 o = gtk_tree_model_iter_next(m, &oi)) {
                char *name;
                gboolean on;
                gtk_tree_model_get(m, &oi, OPT_NAME, &name, OPT_ACTIVE, &on, -1);
                char *next = xd_kbd_option_set(opts, name, on);
                g_free(opts);
                opts = next;
                g_free(name);
            }
        }
        if (strlen(opts) < sizeof(p->kb.options)) {
            if (strcmp(opts, p->kb.options) != 0)
                changed(p);
            g_strlcpy(p->kb.options, opts, sizeof(p->kb.options));
        } else {
            xd_message_box(GTK_WINDOW(p->window), "Keyboard Properties", XD_MB_WARNING, XD_MB_OK,
                           FALSE, "Too many key options are selected; the change wasn't kept.");
        }
        g_free(opts);
        show_options_count(p);
    }
    gtk_widget_destroy(dlg);
    g_object_unref(store);
}

static GtkWidget *button(const char *label, GCallback cb, props *p) {
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_size_request(b, 90, -1);
    g_signal_connect(b, "clicked", cb, p);
    return b;
}

static GtkWidget *languages_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *installed = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(installed),
                       wrapped("Keyboard layouts you can switch between. The first one is "
                               "used when you start."),
                       FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    p->layouts = gtk_list_store_new(LAYOUT_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    xd_kbd_layout list[XD_KBD_LAYOUTS_MAX];
    size_t n = xd_kbd_layouts_parse(p->kb.layout, p->kb.variant, list);
    if (n == 0) {
        /* No layout set: the compositor uses XKB_DEFAULT_LAYOUT, else us. */
        const char *def = g_getenv("XKB_DEFAULT_LAYOUT");
        n = xd_kbd_layouts_parse(def != nullptr && def[0] != '\0' ? def : "us",
                                 g_getenv("XKB_DEFAULT_VARIANT"), list);
    }
    for (size_t i = 0; i < n; i++)
        add_layout_row(p, list[i].name, list[i].variant);
    p->layout_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->layouts));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->layout_view), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->layout_view),
                                gtk_tree_view_column_new_with_attributes(
                                    "Layout", gtk_cell_renderer_text_new(), "text", LAYOUT_DESC,
                                    nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, 260, 110);
    gtk_container_add(GTK_CONTAINER(scroll), p->layout_view);
    gtk_box_pack_start(GTK_BOX(row), scroll, TRUE, TRUE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    p->add = button("A_dd...", G_CALLBACK(on_add), p);
    p->remove = button("_Remove", G_CALLBACK(on_remove), p);
    p->up = button("Move _Up", G_CALLBACK(on_up), p);
    p->down = button("Move Do_wn", G_CALLBACK(on_down), p);
    gtk_box_pack_start(GTK_BOX(buttons), p->add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), p->remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), p->up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), p->down, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(installed), row, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Installed layouts", installed), TRUE, TRUE, 0);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->layout_view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_layout_selected), p);

    GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *switch_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *switch_label = gtk_label_new_with_mnemonic("_Switch layouts with:");
    p->switch_keys = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(switch_label), p->switch_keys);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->switch_keys), "", "(None)");
    char *grp = xd_kbd_option_in_group(p->kb.options, "grp");
    bool grp_listed = grp == nullptr;
    for (struct rxkb_option_group *g = first_group(p); g != nullptr;
         g = rxkb_option_group_next(g)) {
        if (strcmp(rxkb_option_group_get_name(g), "grp") != 0)
            continue;
        for (struct rxkb_option *o = rxkb_option_first(g); o != nullptr; o = rxkb_option_next(o)) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->switch_keys), rxkb_option_get_name(o),
                                      rxkb_option_get_description(o));
            grp_listed = grp_listed || strcmp(rxkb_option_get_name(o), grp) == 0;
        }
    }
    if (!grp_listed)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->switch_keys), grp, grp);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->switch_keys), grp != nullptr ? grp : "");
    g_free(grp);
    gtk_widget_set_size_request(p->switch_keys, 260, -1);
    g_signal_connect(p->switch_keys, "changed", G_CALLBACK(on_switch_keys), p);
    gtk_box_pack_start(GTK_BOX(switch_row), switch_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(switch_row), p->switch_keys, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(keys), switch_row, FALSE, FALSE, 0);
    GtkWidget *opt_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    p->options_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->options_label), 0);
    gtk_box_pack_start(GTK_BOX(opt_row), p->options_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(opt_row), button("Key _Options...", G_CALLBACK(on_options), p),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(keys), opt_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Key settings", keys), FALSE, FALSE, 0);
    show_options_count(p);
    if (p->xkb == nullptr) {
        gtk_widget_set_sensitive(p->add, FALSE);
        gtk_box_pack_start(GTK_BOX(page),
                           wrapped("The list of keyboard layouts (xkeyboard-config) could not "
                                   "be read."),
                           FALSE, FALSE, 0);
    }
    update_layout_buttons(p);
    return page;
}

/* ---- Hardware ------------------------------------------------------------------- */

static void on_model(GtkComboBox *c, gpointer data) {
    props *p = data;
    const char *id = gtk_combo_box_get_active_id(c);
    g_strlcpy(p->kb.model, id != nullptr ? id : "", sizeof(p->kb.model));
    changed(p);
}

static int model_order(gconstpointer a, gconstpointer b) {
    struct rxkb_model *x = *(struct rxkb_model *const *)a;
    struct rxkb_model *y = *(struct rxkb_model *const *)b;
    return g_utf8_collate(rxkb_model_get_description(x), rxkb_model_get_description(y));
}

static GtkWidget *hardware_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row), 10);
    GtkWidget *label = gtk_label_new_with_mnemonic("Keyboard _model:");
    p->model = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->model);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->model), "", "(Default)");
    GPtrArray *models = g_ptr_array_new();
    for (struct rxkb_model *m = p->xkb != nullptr ? rxkb_model_first(p->xkb) : nullptr;
         m != nullptr; m = rxkb_model_next(m))
        g_ptr_array_add(models, m);
    g_ptr_array_sort(models, model_order);
    bool listed = p->kb.model[0] == '\0';
    for (guint i = 0; i < models->len; i++) {
        struct rxkb_model *m = g_ptr_array_index(models, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->model), rxkb_model_get_name(m),
                                  rxkb_model_get_description(m));
        listed = listed || strcmp(rxkb_model_get_name(m), p->kb.model) == 0;
    }
    g_ptr_array_free(models, TRUE);
    if (!listed)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->model), p->kb.model, p->kb.model);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->model), p->kb.model);
    gtk_widget_set_size_request(p->model, 280, -1);
    g_signal_connect(p->model, "changed", G_CALLBACK(on_model), p);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->model, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), xd_input_devices_page(XD_INPUT_KEYBOARDS), TRUE, TRUE, 0);
    return page;
}

/* ---- applying ---------------------------------------------------------------------- */

static bool apply(props *p) {
    xd_kbd_layout list[XD_KBD_LAYOUTS_MAX];
    size_t n = 0;
    GtkTreeIter it;
    GtkTreeModel *m = GTK_TREE_MODEL(p->layouts);
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok && n < XD_KBD_LAYOUTS_MAX;
         ok = gtk_tree_model_iter_next(m, &it)) {
        char *name, *variant;
        gtk_tree_model_get(m, &it, LAYOUT_NAME, &name, LAYOUT_VARIANT, &variant, -1);
        list[n] = (xd_kbd_layout){0};
        g_strlcpy(list[n].name, name, sizeof(list[n].name));
        g_strlcpy(list[n].variant, variant, sizeof(list[n].variant));
        n++;
        g_free(name);
        g_free(variant);
    }
    if (!xd_kbd_layouts_format(list, n, p->kb.layout, sizeof(p->kb.layout), p->kb.variant,
                               sizeof(p->kb.variant))) {
        xd_message_box(GTK_WINDOW(p->window), "Keyboard Properties", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "These layouts' names are too long to save. Remove one.");
        return false;
    }
    /* Everything else as the file has it now. */
    xd_settings s;
    xd_settings_load(&s);
    s.keyboard = p->kb;
    if (!xd_settings_save(&s)) {
        xd_message_box(GTK_WINDOW(p->window), "Keyboard Properties", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "The settings could not be saved.");
        return false;
    }
    gtk_widget_set_sensitive(p->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    props *p = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(p);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(p->apply) && !apply(p))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Keyboard");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props p;
    xd_settings s;
    xd_settings_load(&s);
    p.kb = s.keyboard;
    p.xkb = rxkb_context_new(RXKB_CONTEXT_NO_FLAGS);
    if (p.xkb != nullptr && !rxkb_context_parse_default_ruleset(p.xkb)) {
        g_printerr("xd-keyboard: cannot read the XKB rules (xkeyboard-config)\n");
        rxkb_context_unref(p.xkb);
        p.xkb = nullptr;
    }

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), "Keyboard Properties");
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "input-keyboard");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    p.updating = true;
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), speed_page(&p),
                             gtk_label_new_with_mnemonic("_Speed"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), languages_page(&p),
                             gtk_label_new_with_mnemonic("_Input Languages"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), hardware_page(&p),
                             gtk_label_new_with_mnemonic("_Hardware"));
    p.updating = false;
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));

    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_main();
    stop_repeat(&p);
    if (p.caret_timer != 0)
        g_source_remove(p.caret_timer);
    g_object_unref(p.layouts);
    if (p.xkb != nullptr)
        rxkb_context_unref(p.xkb);
    return EXIT_SUCCESS;
}
