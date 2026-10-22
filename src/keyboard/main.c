/* SPDX-License-Identifier: MIT
 * xd-keyboard: Windows XP's Keyboard Properties, with the input languages
 * XP kept in Regional and Language Options. Writes [Keyboard] in
 * desktop.ini, which the compositor applies to every keyboard (input.c).
 *
 *   Speed            repeat delay and rate (with XP's test box), cursor
 *                    blink rate
 *   Input Languages  up to four XKB layouts and variants, the keys that
 *                    switch between them, other XKB options
 *   Text Services    the input method framework (Fcitx 5, IBus) the
 *                    compositor starts, the Compose key, emoji
 *   Hardware         the keyboard model, the keyboards udev knows
 *
 * The lists of layouts, variants, options and models come from
 * xkeyboard-config through libxkbregistry.
 *
 *   xd-keyboard [--page N]   the Nth tab shown, from 0 (Speed)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbregistry.h>

#include "../core/common.h"
#include "../cpl/inputdevs.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/inputmethod.h"
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
    /* Text Services */
    GtkWidget *im_combo, *im_settings, *compose, *emoji_note, *emoji;
    bool im_touched; /* the framework was chosen: its autostart entries follow */
    /* Hardware */
    GtkWidget *model;
} props;

static void changed(props *properties) {
    if (!properties->updating)
        gtk_widget_set_sensitive(properties->apply, TRUE);
}

/* A slider's notch. */
static int range_int(GtkRange *range) {
    double value = gtk_range_get_value(range);
    return (int)lround(value);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *frame_widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame_widget), child);
    return frame_widget;
}

static GtkWidget *wrapped(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
    return label;
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
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    for (double value = lo; value <= hi; value += tick)
        gtk_scale_add_mark(GTK_SCALE(scale), value, GTK_POS_BOTTOM, nullptr);
    gtk_range_set_round_digits(GTK_RANGE(scale), 0);
    return scale;
}

/* ---- Speed ------------------------------------------------------------------------- */

/* XP's four repeat delays, long to short. */
static int delay_ms(int notch) { return 1000 - notch * 250; }

/* Blink rate notches: 0 is no blinking, 1..10 slow (1200 ms) to fast. */
static int blink_ms(int notch) { return notch == 0 ? 0 : 1300 - notch * 100; }

static void stop_repeat(props *properties) {
    if (properties->repeat_timer != 0)
        g_source_remove(properties->repeat_timer);
    properties->repeat_timer = 0;
    properties->held_key = 0;
    g_clear_pointer(&properties->held_event, gdk_event_free);
}

static gboolean repeat_key(gpointer data) {
    props *properties = data;
    properties->replaying = true;
    gtk_widget_event(properties->test, properties->held_event);
    properties->replaying = false;
    return G_SOURCE_CONTINUE;
}

static gboolean start_repeat(gpointer data) {
    props *properties = data;
    repeat_key(properties);
    properties->repeat_timer =
        properties->kb.repeat_rate > 0
            ? g_timeout_add((guint)(1000 / properties->kb.repeat_rate), repeat_key, properties)
            : 0;
    return G_SOURCE_REMOVE;
}

/* The test box repeats a held key itself, at the delay and rate the
 * sliders show (before they're applied), ignoring the repeats the
 * compositor's settings make. */
static gboolean on_test_press([[maybe_unused]] GtkWidget *widget, GdkEventKey *key_event,
                              gpointer data) {
    props *properties = data;
    if (properties->replaying || key_event->is_modifier)
        return FALSE;
    if (properties->held_key == key_event->hardware_keycode)
        return TRUE;
    stop_repeat(properties);
    properties->held_key = key_event->hardware_keycode;
    properties->held_event = gdk_event_copy((GdkEvent *)key_event);
    properties->repeat_timer =
        g_timeout_add((guint)properties->kb.repeat_delay, start_repeat, properties);
    return FALSE;
}

static gboolean on_test_release([[maybe_unused]] GtkWidget *widget, GdkEventKey *key_event,
                                gpointer data) {
    props *properties = data;
    if (properties->held_key == key_event->hardware_keycode)
        stop_repeat(properties);
    return FALSE;
}

static gboolean on_test_focus_out([[maybe_unused]] GtkWidget *widget,
                                  [[maybe_unused]] GdkEventFocus *focus_event, gpointer data) {
    stop_repeat(data);
    return FALSE;
}

static void on_delay(GtkRange *range, gpointer data) {
    props *properties = data;
    properties->kb.repeat_delay = delay_ms(range_int(range));
    changed(properties);
}

static void on_rate(GtkRange *range, gpointer data) {
    props *properties = data;
    properties->kb.repeat_rate = range_int(range);
    changed(properties);
}

static gboolean draw_caret(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    props *properties = data;
    if (!properties->caret_on)
        return FALSE;
    GtkStyleContext *sc = gtk_widget_get_style_context(widget);
    GdkRGBA fg;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    gdk_cairo_set_source_rgba(cairo, &fg);
    int height = gtk_widget_get_allocated_height(widget);
    cairo_rectangle(cairo, 2, 2, 1, height - 4);
    cairo_fill(cairo);
    return FALSE;
}

static gboolean blink_caret(gpointer data) {
    props *properties = data;
    properties->caret_on = !properties->caret_on;
    gtk_widget_queue_draw(properties->caret);
    return G_SOURCE_CONTINUE;
}

static void restart_caret(props *properties) {
    if (properties->caret_timer != 0)
        g_source_remove(properties->caret_timer);
    properties->caret_timer = 0;
    properties->caret_on = true;
    gtk_widget_queue_draw(properties->caret);
    if (properties->kb.cursor_blink_ms > 0)
        properties->caret_timer =
            g_timeout_add((guint)properties->kb.cursor_blink_ms, blink_caret, properties);
}

static void on_blink(GtkRange *range, gpointer data) {
    props *properties = data;
    properties->kb.cursor_blink_ms = blink_ms(range_int(range));
    restart_caret(properties);
    changed(properties);
}

static GtkWidget *speed_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *repeat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    properties->delay = ticked_scale(0, 3, 1, 1);
    int notch = (int)lround((1000 - properties->kb.repeat_delay) / 250.0);
    gtk_range_set_value(GTK_RANGE(properties->delay), XD_CLAMP(notch, 0, 3));
    g_signal_connect(properties->delay, "value-changed", G_CALLBACK(on_delay), properties);
    gtk_box_pack_start(GTK_BOX(repeat),
                       slider_row("Repeat _delay:", "Long", properties->delay, "Short"), FALSE,
                       FALSE, 0);
    properties->rate = ticked_scale(2, 40, 1, 2);
    gtk_range_set_value(GTK_RANGE(properties->rate), XD_CLAMP(properties->kb.repeat_rate, 2, 40));
    g_signal_connect(properties->rate, "value-changed", G_CALLBACK(on_rate), properties);
    gtk_box_pack_start(GTK_BOX(repeat),
                       slider_row("Repeat _rate:", "Slow", properties->rate, "Fast"), FALSE, FALSE,
                       0);
    GtkWidget *test_label =
        gtk_label_new_with_mnemonic("Click here and hold down a key to _test repeat rate:");
    gtk_label_set_xalign(GTK_LABEL(test_label), 0);
    gtk_box_pack_start(GTK_BOX(repeat), test_label, FALSE, FALSE, 0);
    properties->test = gtk_entry_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(test_label), properties->test);
    g_signal_connect(properties->test, "key-press-event", G_CALLBACK(on_test_press), properties);
    g_signal_connect(properties->test, "key-release-event", G_CALLBACK(on_test_release),
                     properties);
    g_signal_connect(properties->test, "focus-out-event", G_CALLBACK(on_test_focus_out),
                     properties);
    gtk_box_pack_start(GTK_BOX(repeat), properties->test, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Character repeat", repeat), FALSE, FALSE, 0);

    GtkWidget *blink = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    properties->caret = gtk_drawing_area_new();
    gtk_widget_set_size_request(properties->caret, 8, 24);
    gtk_widget_set_valign(properties->caret, GTK_ALIGN_CENTER);
    g_signal_connect(properties->caret, "draw", G_CALLBACK(draw_caret), properties);
    gtk_box_pack_start(GTK_BOX(blink), properties->caret, FALSE, FALSE, 0);
    properties->blink = ticked_scale(0, 10, 1, 1);
    int blink_ms = properties->kb.cursor_blink_ms;
    gtk_range_set_value(GTK_RANGE(properties->blink),
                        blink_ms == 0 ? 0
                                      : XD_CLAMP((int)lround((1300 - blink_ms) / 100.0), 1, 10));
    g_signal_connect(properties->blink, "value-changed", G_CALLBACK(on_blink), properties);
    gtk_box_pack_start(GTK_BOX(blink),
                       slider_row("Cursor _blink rate:", "None", properties->blink, "Fast"), TRUE,
                       TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Cursor blink rate", blink), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       wrapped("The blink rate applies to XeonDesktop's own programs."), FALSE,
                       FALSE, 0);
    restart_caret(properties);
    return page;
}

/* ---- Input Languages -------------------------------------------------------------------- */

static struct rxkb_option_group *first_group(props *properties) {
    return properties->xkb != nullptr ? rxkb_option_group_first(properties->xkb) : nullptr;
}

/* xkeyboard-config's name for a layout and variant, else the codes. */
static char *layout_description(props *properties, const char *name, const char *variant) {
    for (struct rxkb_layout *xkb_layout =
             properties->xkb != nullptr ? rxkb_layout_first(properties->xkb) : nullptr;
         xkb_layout != nullptr; xkb_layout = rxkb_layout_next(xkb_layout)) {
        const char *layout_variant = rxkb_layout_get_variant(xkb_layout);
        if (strcmp(rxkb_layout_get_name(xkb_layout), name) == 0 &&
            g_strcmp0(layout_variant != nullptr ? layout_variant : "", variant) == 0)
            return g_strdup(rxkb_layout_get_description(xkb_layout));
    }
    return variant[0] != '\0' ? g_strdup_printf("%s (%s)", name, variant) : g_strdup(name);
}

static void add_layout_row(props *properties, const char *name, const char *variant) {
    char *desc = layout_description(properties, name, variant);
    gtk_list_store_insert_with_values(properties->layouts, nullptr, -1, LAYOUT_DESC, desc,
                                      LAYOUT_NAME, name, LAYOUT_VARIANT, variant, -1);
    g_free(desc);
}

static int layout_count(props *properties) {
    return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(properties->layouts), nullptr);
}

static void update_layout_buttons(props *properties) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->layout_view));
    GtkTreeIter it;
    bool selected = gtk_tree_selection_get_selected(sel, nullptr, &it);
    int index = -1;
    if (selected) {
        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(properties->layouts), &it);
        index = gtk_tree_path_get_indices(path)[0];
        gtk_tree_path_free(path);
    }
    int count = layout_count(properties);
    gtk_widget_set_sensitive(properties->add, count < XD_KBD_LAYOUTS_MAX);
    gtk_widget_set_sensitive(properties->remove, selected && count > 1);
    gtk_widget_set_sensitive(properties->up, selected && index > 0);
    gtk_widget_set_sensitive(properties->down, selected && index < count - 1);
}

static void on_layout_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    update_layout_buttons(data);
}

/* The Add dialog's variant list for the layout picked. */
static void fill_variants(props *properties, GtkWidget *variants, const char *layout) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(variants));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(variants), "", "(Default)");
    GPtrArray *found = g_ptr_array_new();
    for (struct rxkb_layout *xkb_layout =
             properties->xkb != nullptr ? rxkb_layout_first(properties->xkb) : nullptr;
         xkb_layout != nullptr; xkb_layout = rxkb_layout_next(xkb_layout)) {
        if (layout != nullptr && rxkb_layout_get_variant(xkb_layout) != nullptr &&
            strcmp(rxkb_layout_get_name(xkb_layout), layout) == 0)
            g_ptr_array_add(found, xkb_layout);
    }
    for (guint i = 0; i < found->len; i++) {
        struct rxkb_layout *xkb_layout = g_ptr_array_index(found, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(variants), rxkb_layout_get_variant(xkb_layout),
                                  rxkb_layout_get_description(xkb_layout));
    }
    g_ptr_array_free(found, TRUE);
    gtk_combo_box_set_active(GTK_COMBO_BOX(variants), 0);
}

static void on_add_layout_changed(GtkComboBox *combo, gpointer variants) {
    props *properties = g_object_get_data(G_OBJECT(combo), "props");
    fill_variants(properties, variants, gtk_combo_box_get_active_id(combo));
}

static int by_description(gconstpointer left, gconstpointer right) {
    struct rxkb_layout *left_layout = *(struct rxkb_layout *const *)left;
    struct rxkb_layout *right_layout = *(struct rxkb_layout *const *)right;
    return g_utf8_collate(rxkb_layout_get_description(left_layout),
                          rxkb_layout_get_description(right_layout));
}

static void on_add([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Add Input Layout", GTK_WINDOW(properties->window),
                                                 GTK_DIALOG_MODAL, "OK", GTK_RESPONSE_OK, "Cancel",
                                                 GTK_RESPONSE_CANCEL, nullptr);
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
    for (struct rxkb_layout *xkb_layout =
             properties->xkb != nullptr ? rxkb_layout_first(properties->xkb) : nullptr;
         xkb_layout != nullptr; xkb_layout = rxkb_layout_next(xkb_layout)) {
        if (rxkb_layout_get_variant(xkb_layout) == nullptr)
            g_ptr_array_add(all, xkb_layout);
    }
    g_ptr_array_sort(all, by_description);
    for (guint i = 0; i < all->len; i++) {
        struct rxkb_layout *xkb_layout = g_ptr_array_index(all, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(layouts), rxkb_layout_get_name(xkb_layout),
                                  rxkb_layout_get_description(xkb_layout));
    }
    g_ptr_array_free(all, TRUE);
    g_object_set_data(G_OBJECT(layouts), "props", properties);
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
        if (name != nullptr && layout_count(properties) < XD_KBD_LAYOUTS_MAX) {
            add_layout_row(properties, name, variant != nullptr ? variant : "");
            changed(properties);
        }
    }
    gtk_widget_destroy(dlg);
    update_layout_buttons(properties);
}

static void on_remove([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->layout_view));
    GtkTreeIter it;
    if (layout_count(properties) > 1 && gtk_tree_selection_get_selected(sel, nullptr, &it)) {
        gtk_list_store_remove(properties->layouts, &it);
        changed(properties);
    }
    update_layout_buttons(properties);
}

static void move_layout(props *properties, bool up) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->layout_view));
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, nullptr, &it))
        return;
    GtkTreeIter other = it;
    bool ok = up ? gtk_tree_model_iter_previous(GTK_TREE_MODEL(properties->layouts), &other)
                 : gtk_tree_model_iter_next(GTK_TREE_MODEL(properties->layouts), &other);
    if (ok) {
        gtk_list_store_swap(properties->layouts, &it, &other);
        changed(properties);
    }
    update_layout_buttons(properties);
}

static void on_up([[maybe_unused]] GtkButton *button, gpointer data) { move_layout(data, true); }

static void on_down([[maybe_unused]] GtkButton *button, gpointer data) { move_layout(data, false); }

static void on_switch_keys(GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    const char *id = gtk_combo_box_get_active_id(combo);
    char *opts = xd_kbd_options_clear_group(properties->kb.options, "grp");
    char *with = xd_kbd_option_set(opts, id != nullptr ? id : "", true);
    g_strlcpy(properties->kb.options, with, sizeof(properties->kb.options));
    g_free(with);
    g_free(opts);
    changed(properties);
}

static void show_options_count(props *properties) {
    /* Everything but the switching keys, which have their own list. */
    char *rest = xd_kbd_options_clear_group(properties->kb.options, "grp");
    guint count = 0;
    gchar **list = g_strsplit(rest, ",", -1);
    for (guint i = 0; list[i] != nullptr; i++)
        count += list[i][0] != '\0';
    g_strfreev(list);
    g_free(rest);
    char *text = count == 0   ? g_strdup("No other key options are set.")
                 : count == 1 ? g_strdup("1 other key option is set.")
                              : g_strdup_printf("%u other key options are set.", count);
    gtk_label_set_text(GTK_LABEL(properties->options_label), text);
    g_free(text);
}

static void on_option_toggled([[maybe_unused]] GtkCellRendererToggle *renderer, char *path,
                              gpointer store) {
    GtkTreeModel *model = store;
    GtkTreeIter it, parent, sib;
    if (!gtk_tree_model_get_iter_from_string(model, &it, path))
        return;
    gboolean active, multi;
    gtk_tree_model_get(model, &it, OPT_ACTIVE, &active, OPT_GROUP_MULTI, &multi, -1);
    /* A group that allows one option at a time works like radio buttons
     * that can all be off. */
    if (!active && !multi && gtk_tree_model_iter_parent(model, &parent, &it) &&
        gtk_tree_model_iter_children(model, &sib, &parent)) {
        do {
            gtk_tree_store_set(store, &sib, OPT_ACTIVE, FALSE, -1);
        } while (gtk_tree_model_iter_next(model, &sib));
    }
    gtk_tree_store_set(store, &it, OPT_ACTIVE, !active, -1);
}

static void show_compose(props *properties);

static void on_options([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    GtkTreeStore *store = gtk_tree_store_new(OPT_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    gchar **current = g_strsplit(properties->kb.options, ",", -1);
    for (struct rxkb_option_group *option_group = first_group(properties); option_group != nullptr;
         option_group = rxkb_option_group_next(option_group)) {
        if (strcmp(rxkb_option_group_get_name(option_group), "grp") == 0)
            continue;
        GtkTreeIter gi;
        bool multi = rxkb_option_group_allows_multiple(option_group);
        gtk_tree_store_insert_with_values(store, &gi, nullptr, -1, OPT_TEXT,
                                          rxkb_option_group_get_description(option_group),
                                          OPT_IS_OPTION, FALSE, -1);
        for (struct rxkb_option *xkb_option = rxkb_option_first(option_group);
             xkb_option != nullptr; xkb_option = rxkb_option_next(xkb_option)) {
            const char *name = rxkb_option_get_name(xkb_option);
            gtk_tree_store_insert_with_values(
                store, nullptr, &gi, -1, OPT_TEXT, rxkb_option_get_description(xkb_option),
                OPT_NAME, name, OPT_ACTIVE, g_strv_contains((const char *const *)current, name),
                OPT_IS_OPTION, TRUE, OPT_GROUP_MULTI, multi, -1);
        }
    }
    g_strfreev(current);

    GtkWidget *dlg =
        gtk_dialog_new_with_buttons("Key Options", GTK_WINDOW(properties->window), GTK_DIALOG_MODAL,
                                    "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
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
        char *opts = g_strdup(properties->kb.options);
        GtkTreeModel *model = GTK_TREE_MODEL(store);
        GtkTreeIter gi, oi;
        for (bool has_group = gtk_tree_model_get_iter_first(model, &gi); has_group;
             has_group = gtk_tree_model_iter_next(model, &gi)) {
            for (bool has_option = gtk_tree_model_iter_children(model, &oi, &gi); has_option;
                 has_option = gtk_tree_model_iter_next(model, &oi)) {
                char *name;
                gboolean on;
                gtk_tree_model_get(model, &oi, OPT_NAME, &name, OPT_ACTIVE, &on, -1);
                char *next = xd_kbd_option_set(opts, name, on);
                g_free(opts);
                opts = next;
                g_free(name);
            }
        }
        if (strlen(opts) < sizeof(properties->kb.options)) {
            if (strcmp(opts, properties->kb.options) != 0)
                changed(properties);
            g_strlcpy(properties->kb.options, opts, sizeof(properties->kb.options));
        } else {
            xd_message_box(GTK_WINDOW(properties->window), "Keyboard Properties", XD_MB_WARNING,
                           XD_MB_OK, FALSE,
                           "Too many key options are selected; the change wasn't kept.");
        }
        g_free(opts);
        show_options_count(properties);
        show_compose(properties);
    }
    gtk_widget_destroy(dlg);
    g_object_unref(store);
}

static GtkWidget *button(const char *label, GCallback cb, props *properties) {
    GtkWidget *widget = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_size_request(widget, 90, -1);
    g_signal_connect(widget, "clicked", cb, properties);
    return widget;
}

static GtkWidget *languages_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *installed = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(installed),
                       wrapped("Keyboard layouts you can switch between. The first one is "
                               "used when you start."),
                       FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    properties->layouts =
        gtk_list_store_new(LAYOUT_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    xd_kbd_layout list[XD_KBD_LAYOUTS_MAX];
    size_t count = xd_kbd_layouts_parse(properties->kb.layout, properties->kb.variant, list);
    if (count == 0) {
        /* No layout set: the compositor uses XKB_DEFAULT_LAYOUT, else us. */
        const char *def = g_getenv("XKB_DEFAULT_LAYOUT");
        count = xd_kbd_layouts_parse(def != nullptr && def[0] != '\0' ? def : "us",
                                     g_getenv("XKB_DEFAULT_VARIANT"), list);
    }
    for (size_t i = 0; i < count; i++)
        add_layout_row(properties, list[i].name, list[i].variant);
    properties->layout_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(properties->layouts));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(properties->layout_view), FALSE);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(properties->layout_view),
        gtk_tree_view_column_new_with_attributes("Layout", gtk_cell_renderer_text_new(), "text",
                                                 LAYOUT_DESC, nullptr));
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, 260, 110);
    gtk_container_add(GTK_CONTAINER(scroll), properties->layout_view);
    gtk_box_pack_start(GTK_BOX(row), scroll, TRUE, TRUE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    properties->add = button("A_dd...", G_CALLBACK(on_add), properties);
    properties->remove = button("_Remove", G_CALLBACK(on_remove), properties);
    properties->up = button("Move _Up", G_CALLBACK(on_up), properties);
    properties->down = button("Move Do_wn", G_CALLBACK(on_down), properties);
    gtk_box_pack_start(GTK_BOX(buttons), properties->add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), properties->remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), properties->up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), properties->down, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(installed), row, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Installed layouts", installed), TRUE, TRUE, 0);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->layout_view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_layout_selected), properties);

    GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *switch_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *switch_label = gtk_label_new_with_mnemonic("_Switch layouts with:");
    properties->switch_keys = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(switch_label), properties->switch_keys);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->switch_keys), "", "(None)");
    char *grp = xd_kbd_option_in_group(properties->kb.options, "grp");
    bool grp_listed = grp == nullptr;
    for (struct rxkb_option_group *option_group = first_group(properties); option_group != nullptr;
         option_group = rxkb_option_group_next(option_group)) {
        if (strcmp(rxkb_option_group_get_name(option_group), "grp") != 0)
            continue;
        for (struct rxkb_option *xkb_option = rxkb_option_first(option_group);
             xkb_option != nullptr; xkb_option = rxkb_option_next(xkb_option)) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->switch_keys),
                                      rxkb_option_get_name(xkb_option),
                                      rxkb_option_get_description(xkb_option));
            grp_listed = grp_listed || strcmp(rxkb_option_get_name(xkb_option), grp) == 0;
        }
    }
    if (!grp_listed)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->switch_keys), grp, grp);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->switch_keys), grp != nullptr ? grp : "");
    g_free(grp);
    gtk_widget_set_size_request(properties->switch_keys, 260, -1);
    g_signal_connect(properties->switch_keys, "changed", G_CALLBACK(on_switch_keys), properties);
    gtk_box_pack_start(GTK_BOX(switch_row), switch_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(switch_row), properties->switch_keys, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(keys), switch_row, FALSE, FALSE, 0);
    GtkWidget *opt_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    properties->options_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(properties->options_label), 0);
    gtk_box_pack_start(GTK_BOX(opt_row), properties->options_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(opt_row),
                       button("Key _Options...", G_CALLBACK(on_options), properties), FALSE, FALSE,
                       0);
    gtk_box_pack_start(GTK_BOX(keys), opt_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Key settings", keys), FALSE, FALSE, 0);
    show_options_count(properties);
    if (properties->xkb == nullptr) {
        gtk_widget_set_sensitive(properties->add, FALSE);
        gtk_box_pack_start(GTK_BOX(page),
                           wrapped("The list of keyboard layouts (xkeyboard-config) could not "
                                   "be read."),
                           FALSE, FALSE, 0);
    }
    update_layout_buttons(properties);
    return page;
}

/* ---- Text Services ------------------------------------------------------------ */

/* Runs a program of the input method's; tells the user if it can't. */
static void run_tool(props *properties, const char *const *argv) {
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        xd_message_box(GTK_WINDOW(properties->window), "Keyboard Properties", XD_MB_ERROR, XD_MB_OK,
                       FALSE, error->message);
        g_error_free(error);
    }
}

static xd_im_framework chosen_im(props *properties) {
    return xd_im_from_name(properties->kb.input_method);
}

static bool on_path(const char *program) {
    char *path = program != nullptr ? g_find_program_in_path(program) : nullptr;
    bool found = path != nullptr;
    g_free(path);
    return found;
}

/* The buttons and the emoji note follow the framework chosen. */
static void show_im(props *properties) {
    xd_im_framework fw = chosen_im(properties);
    gtk_widget_set_sensitive(properties->im_settings, on_path(xd_im_config_tool(fw)));
    const char *hint = xd_im_emoji_hint(fw);
    gtk_label_set_text(GTK_LABEL(properties->emoji_note),
                       hint != nullptr ? hint
                                       : "With an input method, you can type emoji and other "
                                         "characters by name.");
    /* IBus has an emoji picker of its own, as a program. */
    gtk_widget_set_visible(properties->emoji, fw == XD_IM_IBUS && xd_im_installed(fw));
}

static void on_im(GtkComboBox *combo, gpointer data) {
    props *properties = data;
    const char *id = gtk_combo_box_get_active_id(combo);
    g_strlcpy(properties->kb.input_method, id != nullptr ? id : "",
              sizeof(properties->kb.input_method));
    properties->im_touched = true;
    show_im(properties);
    changed(properties);
}

static void on_im_settings([[maybe_unused]] GtkButton *button, gpointer data) {
    props *properties = data;
    const char *argv[] = {xd_im_config_tool(chosen_im(properties)), nullptr};
    if (argv[0] != nullptr)
        run_tool(properties, argv);
}

static void on_emoji([[maybe_unused]] GtkButton *button, gpointer data) {
    const char *argv[] = {"ibus", "emoji", nullptr};
    run_tool(data, argv);
}

/* The Compose key choice shows the first compose: option set. */
static void show_compose(props *properties) {
    char *compose = xd_kbd_option_in_group(properties->kb.options, "compose");
    bool was = properties->updating;
    properties->updating = true;
    if (compose != nullptr &&
        !gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->compose), compose)) {
        /* Not one xkeyboard-config lists: keep it. */
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->compose), compose, compose);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->compose), compose);
    } else if (compose == nullptr) {
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->compose), "");
    }
    properties->updating = was;
    g_free(compose);
}

static void on_compose(GtkComboBox *combo, gpointer data) {
    props *properties = data;
    if (properties->updating)
        return;
    const char *id = gtk_combo_box_get_active_id(combo);
    char *opts = xd_kbd_options_clear_group(properties->kb.options, "compose");
    char *with = id != nullptr && id[0] != '\0' ? xd_kbd_option_set(opts, id, true) : g_strdup(opts);
    if (strlen(with) < sizeof(properties->kb.options))
        g_strlcpy(properties->kb.options, with, sizeof(properties->kb.options));
    g_free(opts);
    g_free(with);
    show_options_count(properties);
    changed(properties);
}

static GtkWidget *labelled_row(const char *mnemonic, GtkWidget *widget) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);
    gtk_widget_set_size_request(label, 110, -1);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), widget, TRUE, TRUE, 0);
    return row;
}

static GtkWidget *text_services_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    /* The input method framework: the installed ones, and the chosen one
     * even if it has gone. */
    GtkWidget *im = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(im),
                       wrapped("An input method lets you type languages that have more "
                               "characters than the keyboard has keys, such as Chinese, "
                               "Japanese and Korean. It starts when you log on."),
                       FALSE, FALSE, 0);
    properties->im_combo = gtk_combo_box_text_new();
    xd_im_framework current = chosen_im(properties);
    bool any = false;
    for (int i = 0; i < XD_IM_COUNT; i++) {
        xd_im_framework fw = (xd_im_framework)i;
        bool installed = xd_im_installed(fw);
        any = any || (installed && fw != XD_IM_NONE);
        if (installed || fw == current) {
            char *text = installed ? g_strdup(xd_im_display_name(fw))
                                   : g_strdup_printf("%s (not installed)", xd_im_display_name(fw));
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->im_combo), xd_im_name(fw),
                                      text);
            g_free(text);
        }
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->im_combo), xd_im_name(current));
    g_signal_connect(properties->im_combo, "changed", G_CALLBACK(on_im), properties);
    GtkWidget *im_row = labelled_row("Input _method:", properties->im_combo);
    properties->im_settings = button("_Properties...", G_CALLBACK(on_im_settings), properties);
    gtk_box_pack_start(GTK_BOX(im_row), properties->im_settings, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(im), im_row, FALSE, FALSE, 0);
    if (!any)
        gtk_box_pack_start(GTK_BOX(im),
                           wrapped("No input method is installed. Install Fcitx 5 or IBus to "
                                   "choose one here."),
                           FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Input method", im), FALSE, FALSE, 0);

    /* The Compose key: an XKB option, kept with the others. */
    GtkWidget *compose = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(compose),
                       wrapped("Press the Compose key and then two keys to type a character "
                               "the keyboard doesn't have: Compose, ' and e types é."),
                       FALSE, FALSE, 0);
    properties->compose = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->compose), "", "(None)");
    for (struct rxkb_option_group *option_group = first_group(properties); option_group != nullptr;
         option_group = rxkb_option_group_next(option_group)) {
        for (struct rxkb_option *xkb_option = rxkb_option_first(option_group);
             xkb_option != nullptr; xkb_option = rxkb_option_next(xkb_option)) {
            if (g_str_has_prefix(rxkb_option_get_name(xkb_option), "compose:"))
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->compose),
                                          rxkb_option_get_name(xkb_option),
                                          rxkb_option_get_description(xkb_option));
        }
    }
    show_compose(properties);
    g_signal_connect(properties->compose, "changed", G_CALLBACK(on_compose), properties);
    gtk_box_pack_start(GTK_BOX(compose), labelled_row("_Compose key:", properties->compose), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Compose key", compose), FALSE, FALSE, 0);

    /* Emoji: each framework's own way. */
    GtkWidget *emoji = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    properties->emoji_note = wrapped("");
    gtk_box_pack_start(GTK_BOX(emoji), properties->emoji_note, TRUE, TRUE, 0);
    properties->emoji = button("_Emoji...", G_CALLBACK(on_emoji), properties);
    gtk_widget_set_valign(properties->emoji, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(properties->emoji, TRUE);
    gtk_box_pack_start(GTK_BOX(emoji), properties->emoji, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Emoji and symbols", emoji), FALSE, FALSE, 0);
    show_im(properties);
    return page;
}

/* ---- Hardware ------------------------------------------------------------------- */

static void on_model(GtkComboBox *combo, gpointer data) {
    props *properties = data;
    const char *id = gtk_combo_box_get_active_id(combo);
    g_strlcpy(properties->kb.model, id != nullptr ? id : "", sizeof(properties->kb.model));
    changed(properties);
}

static int model_order(gconstpointer left, gconstpointer right) {
    struct rxkb_model *left_model = *(struct rxkb_model *const *)left;
    struct rxkb_model *right_model = *(struct rxkb_model *const *)right;
    return g_utf8_collate(rxkb_model_get_description(left_model),
                          rxkb_model_get_description(right_model));
}

static GtkWidget *hardware_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row), 10);
    GtkWidget *label = gtk_label_new_with_mnemonic("Keyboard _model:");
    properties->model = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->model);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->model), "", "(Default)");
    GPtrArray *models = g_ptr_array_new();
    for (struct rxkb_model *xkb_model =
             properties->xkb != nullptr ? rxkb_model_first(properties->xkb) : nullptr;
         xkb_model != nullptr; xkb_model = rxkb_model_next(xkb_model))
        g_ptr_array_add(models, xkb_model);
    g_ptr_array_sort(models, model_order);
    bool listed = properties->kb.model[0] == '\0';
    for (guint i = 0; i < models->len; i++) {
        struct rxkb_model *xkb_model = g_ptr_array_index(models, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->model),
                                  rxkb_model_get_name(xkb_model),
                                  rxkb_model_get_description(xkb_model));
        listed = listed || strcmp(rxkb_model_get_name(xkb_model), properties->kb.model) == 0;
    }
    g_ptr_array_free(models, TRUE);
    if (!listed)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->model), properties->kb.model,
                                  properties->kb.model);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->model), properties->kb.model);
    gtk_widget_set_size_request(properties->model, 280, -1);
    g_signal_connect(properties->model, "changed", G_CALLBACK(on_model), properties);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), properties->model, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), xd_input_devices_page(XD_INPUT_KEYBOARDS), TRUE, TRUE, 0);
    return page;
}

/* ---- applying ---------------------------------------------------------------------- */

static bool apply(props *properties) {
    xd_kbd_layout list[XD_KBD_LAYOUTS_MAX];
    size_t count = 0;
    GtkTreeIter it;
    GtkTreeModel *model = GTK_TREE_MODEL(properties->layouts);
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok && count < XD_KBD_LAYOUTS_MAX;
         ok = gtk_tree_model_iter_next(model, &it)) {
        char *name, *variant;
        gtk_tree_model_get(model, &it, LAYOUT_NAME, &name, LAYOUT_VARIANT, &variant, -1);
        list[count] = (xd_kbd_layout){0};
        g_strlcpy(list[count].name, name, sizeof(list[count].name));
        g_strlcpy(list[count].variant, variant, sizeof(list[count].variant));
        count++;
        g_free(name);
        g_free(variant);
    }
    if (!xd_kbd_layouts_format(list, count, properties->kb.layout, sizeof(properties->kb.layout),
                               properties->kb.variant, sizeof(properties->kb.variant))) {
        xd_message_box(GTK_WINDOW(properties->window), "Keyboard Properties", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "These layouts' names are too long to save. Remove one.");
        return false;
    }
    /* Everything else as the file has it now. */
    xd_settings settings;
    xd_settings_load(&settings);
    settings.keyboard = properties->kb;
    if (!xd_settings_save(&settings)) {
        xd_message_box(GTK_WINDOW(properties->window), "Keyboard Properties", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "The settings could not be saved.");
        return false;
    }
    /* The compositor starts the framework chosen; the frameworks' own
     * autostart entries mustn't start it (or another) again. */
    GError *error = nullptr;
    if (properties->im_touched && !xd_im_autostart_override_session(&error)) {
        xd_message_box(GTK_WINDOW(properties->window), "Keyboard Properties", XD_MB_WARNING,
                       XD_MB_OK, FALSE, error->message);
        g_error_free(error);
    }
    properties->im_touched = false;
    gtk_widget_set_sensitive(properties->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    props *properties = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply) &&
        !apply(properties))
        return;
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

int main(int argc, char **argv) {
    int page = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
    }
    xd_gtk_app_preinit("org.xeondesktop.Keyboard");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props properties;
    xd_settings settings;
    xd_settings_load(&settings);
    properties.kb = settings.keyboard;
    properties.xkb = rxkb_context_new(RXKB_CONTEXT_NO_FLAGS);
    if (properties.xkb != nullptr && !rxkb_context_parse_default_ruleset(properties.xkb)) {
        g_printerr("xd-keyboard: cannot read the XKB rules (xkeyboard-config)\n");
        rxkb_context_unref(properties.xkb);
        properties.xkb = nullptr;
    }

    properties.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties.window), "Keyboard Properties");
    gtk_window_set_icon_name(GTK_WINDOW(properties.window), "input-keyboard");
    gtk_window_set_resizable(GTK_WINDOW(properties.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "Cancel", GTK_RESPONSE_CANCEL);
    properties.apply =
        gtk_dialog_add_button(GTK_DIALOG(properties.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(properties.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    properties.updating = true;
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), speed_page(&properties),
                             gtk_label_new_with_mnemonic("_Speed"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), languages_page(&properties),
                             gtk_label_new_with_mnemonic("_Input Languages"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), text_services_page(&properties),
                             gtk_label_new_with_mnemonic("_Text Services"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), hardware_page(&properties),
                             gtk_label_new_with_mnemonic("_Hardware"));
    properties.updating = false;
    gtk_widget_set_sensitive(properties.apply, FALSE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties.window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties.window, GTK_NOTEBOOK(tabs));

    g_signal_connect(properties.window, "response", G_CALLBACK(on_response), &properties);
    g_signal_connect(properties.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(properties.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), page);
    gtk_main();
    stop_repeat(&properties);
    if (properties.caret_timer != 0)
        g_source_remove(properties.caret_timer);
    g_object_unref(properties.layouts);
    if (properties.xkb != nullptr)
        rxkb_context_unref(properties.xkb);
    return EXIT_SUCCESS;
}
