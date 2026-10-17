/* SPDX-License-Identifier: MIT
 * xd-access: Windows XP's Accessibility Options. Writes [Accessibility]
 * in desktop.ini (and the High Contrast look into [Appearance], the
 * cursor options into [Keyboard] and [Mouse]); the compositor applies it.
 *
 *   Keyboard  StickyKeys and FilterKeys' BounceKeys, done by the
 *             compositor (input.c)
 *   Display   High Contrast, the caret's blink rate, the pointer size
 *   Mouse     MouseKeys
 *
 * Greyed out, as not done yet: FilterKeys' SlowKeys (the compositor would
 * have to hold keys back), ToggleKeys (it plays tones, and there's no
 * sound for that yet), MouseKeys (moving the pointer from the keypad),
 * and XP's Sound and General tabs aren't here at all.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"

typedef struct props {
    GtkWidget *window, *apply;
    xd_settings orig; /* desktop.ini as we found it */
    xd_settings now;  /* what the widgets show */
    bool updating;
    /* Keyboard */
    GtkWidget *sticky, *sticky_lock, *filter, *bounce_ms;
    /* Display */
    GtkWidget *contrast, *contrast_scheme, *blink, *caret, *pointer_size;
    bool caret_on;
    guint caret_timer;
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

/* An option that does nothing yet: shown, greyed, saying so. */
static GtkWidget *unavailable(const char *check, const char *text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *l = wrapped(text);
    GtkWidget *c = gtk_check_button_new_with_mnemonic(check);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
    GtkWidget *note = wrapped("Not available yet.");
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(c, FALSE);
    gtk_widget_set_sensitive(note, FALSE);
    return box;
}

/* ---- Keyboard ------------------------------------------------------------------------ */

static void show_keyboard_sensitivity(props *p) {
    gtk_widget_set_sensitive(p->sticky_lock, p->now.access.sticky_keys);
    gtk_widget_set_sensitive(p->bounce_ms, p->now.access.bounce_keys);
}

static void on_keyboard_option([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    xd_access_settings *a = &p->now.access;
    a->sticky_keys = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->sticky));
    a->sticky_lock = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->sticky_lock));
    a->bounce_keys = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->filter));
    a->bounce_ms = (int)lround(gtk_spin_button_get_value(GTK_SPIN_BUTTON(p->bounce_ms)) * 1000);
    show_keyboard_sensitivity(p);
    changed(p);
}

static GtkWidget *keyboard_page(props *p) {
    const xd_access_settings *a = &p->now.access;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *sticky = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(sticky),
                       wrapped("Use StickyKeys if you want to use SHIFT, CTRL, ALT, or the "
                               "Windows logo key by pressing one key at a time."),
                       FALSE, FALSE, 0);
    p->sticky = gtk_check_button_new_with_mnemonic("Use _StickyKeys");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->sticky), a->sticky_keys);
    gtk_box_pack_start(GTK_BOX(sticky), p->sticky, FALSE, FALSE, 0);
    p->sticky_lock = gtk_check_button_new_with_mnemonic("Press modifier key _twice to lock");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->sticky_lock), a->sticky_lock);
    gtk_widget_set_margin_start(p->sticky_lock, 20);
    gtk_box_pack_start(GTK_BOX(sticky), p->sticky_lock, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("StickyKeys", sticky), FALSE, FALSE, 0);

    GtkWidget *filter = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(filter),
                       wrapped("Use FilterKeys if you want Windows to ignore brief or repeated "
                               "keystrokes, or slow the repeat rate."),
                       FALSE, FALSE, 0);
    p->filter = gtk_check_button_new_with_mnemonic("Use _FilterKeys: ignore repeated keystrokes "
                                                   "(BounceKeys)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->filter), a->bounce_keys);
    gtk_box_pack_start(GTK_BOX(filter), p->filter, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 20);
    GtkWidget *label = gtk_label_new_with_mnemonic("Ignore a key pressed again _within:");
    p->bounce_ms = gtk_spin_button_new_with_range(0.1, 2.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(p->bounce_ms), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->bounce_ms), a->bounce_ms / 1000.0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->bounce_ms);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->bounce_ms, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("seconds"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filter), row, FALSE, FALSE, 0);
    /* SlowKeys would hold every key back until it's been down a while. */
    GtkWidget *slow = gtk_check_button_new_with_mnemonic(
        "Ignore quick keystrokes and slow down the repeat rate (SlowKeys) - not available yet");
    gtk_widget_set_sensitive(slow, FALSE);
    gtk_box_pack_start(GTK_BOX(filter), slow, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("FilterKeys", filter), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page),
                       frame("ToggleKeys",
                             unavailable("Use T_oggleKeys",
                                         "Use ToggleKeys if you want to hear tones when "
                                         "pressing CAPS LOCK, NUM LOCK, and SCROLL LOCK.")),
                       FALSE, FALSE, 0);

    g_signal_connect(p->sticky, "toggled", G_CALLBACK(on_keyboard_option), p);
    g_signal_connect(p->sticky_lock, "toggled", G_CALLBACK(on_keyboard_option), p);
    g_signal_connect(p->filter, "toggled", G_CALLBACK(on_keyboard_option), p);
    g_signal_connect(p->bounce_ms, "value-changed", G_CALLBACK(on_keyboard_option), p);
    show_keyboard_sensitivity(p);
    return page;
}

/* ---- Display ------------------------------------------------------------------------- */

static void on_contrast([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    gtk_widget_set_sensitive(p->contrast_scheme,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->contrast)));
    changed(p);
}

/* Blink rate notches, as in Keyboard Properties: 0 is no blinking, 1..10
 * slow (1200 ms) to fast. */
static int blink_ms(int notch) { return notch == 0 ? 0 : 1300 - notch * 100; }

static gboolean draw_caret(GtkWidget *w, cairo_t *cr, gpointer data) {
    props *p = data;
    if (!p->caret_on)
        return FALSE;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    gdk_cairo_set_source_rgba(cr, &fg);
    cairo_rectangle(cr, 2, 2, 1, gtk_widget_get_allocated_height(w) - 4);
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
    int ms = p->now.keyboard.cursor_blink_ms;
    if (ms > 0)
        p->caret_timer = g_timeout_add((guint)ms, blink_caret, p);
}

static void on_blink(GtkRange *r, gpointer data) {
    props *p = data;
    p->now.keyboard.cursor_blink_ms = blink_ms(range_int(r));
    restart_caret(p);
    changed(p);
}

static void on_pointer_size(GtkComboBox *c, gpointer data) {
    props *p = data;
    const char *id = gtk_combo_box_get_active_id(c);
    if (id != nullptr)
        p->now.mouse.cursor_size = atoi(id);
    changed(p);
}

/* The installed schemes marked HighContrast. */
static void fill_contrast_schemes(props *p) {
    xd_scheme_list list;
    xd_scheme_list_load(&list);
    for (size_t i = 0; i < list.count; i++) {
        xd_settings probe;
        xd_settings_defaults(&probe);
        g_strlcpy(probe.scheme, list.stems[i], sizeof(probe.scheme));
        xd_scheme scheme;
        xd_settings_scheme(&probe, &scheme);
        if (scheme.high_contrast)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->contrast_scheme), list.stems[i],
                                      list.names[i]);
    }
    xd_scheme_list_free(&list);
    if (!p->now.access.high_contrast ||
        !gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->contrast_scheme), p->now.scheme))
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->contrast_scheme), 0);
}

static GtkWidget *display_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *contrast = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(contrast),
                       wrapped("Use this option if you want Windows to use colors and fonts "
                               "designed for easy reading."),
                       FALSE, FALSE, 0);
    p->contrast = gtk_check_button_new_with_mnemonic("Use _High Contrast");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->contrast), p->now.access.high_contrast);
    gtk_box_pack_start(GTK_BOX(contrast), p->contrast, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 20);
    GtkWidget *label = gtk_label_new_with_mnemonic("Hi_gh Contrast scheme:");
    p->contrast_scheme = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->contrast_scheme);
    fill_contrast_schemes(p);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->contrast_scheme, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(contrast), row, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(p->contrast_scheme, p->now.access.high_contrast);
    gtk_box_pack_start(GTK_BOX(page), frame("High Contrast", contrast), FALSE, FALSE, 0);

    GtkWidget *cursor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *blink_label = gtk_label_new_with_mnemonic("_Blink Rate:");
    gtk_label_set_xalign(GTK_LABEL(blink_label), 0);
    gtk_box_pack_start(GTK_BOX(cursor), blink_label, FALSE, FALSE, 0);
    GtkWidget *blink_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->blink = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 10, 1);
    for (int v = 0; v <= 10; v++)
        gtk_scale_add_mark(GTK_SCALE(p->blink), v, GTK_POS_BOTTOM, nullptr);
    gtk_scale_set_draw_value(GTK_SCALE(p->blink), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(p->blink), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(blink_label), p->blink);
    int b = p->now.keyboard.cursor_blink_ms;
    gtk_range_set_value(GTK_RANGE(p->blink),
                        b == 0 ? 0 : XD_CLAMP((int)lround((1300 - b) / 100.0), 1, 10));
    gtk_widget_set_size_request(p->blink, 180, -1);
    gtk_box_pack_start(GTK_BOX(blink_row), gtk_label_new("None"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(blink_row), p->blink, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(blink_row), gtk_label_new("Fast"), FALSE, FALSE, 0);
    p->caret = gtk_drawing_area_new();
    gtk_widget_set_size_request(p->caret, 8, 24);
    g_signal_connect(p->caret, "draw", G_CALLBACK(draw_caret), p);
    gtk_box_pack_start(GTK_BOX(blink_row), p->caret, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(cursor), blink_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Cursor Options", cursor), FALSE, FALSE, 0);

    GtkWidget *pointer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *size_label = gtk_label_new_with_mnemonic("Mouse _pointer size:");
    p->pointer_size = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(size_label), p->pointer_size);
    static const struct {
        int size;
        const char *name;
    } sizes[] = {{24, "Normal"}, {32, "Large"}, {48, "Extra Large"}};
    char id[16];
    for (size_t i = 0; i < G_N_ELEMENTS(sizes); i++) {
        g_snprintf(id, sizeof(id), "%d", sizes[i].size);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->pointer_size), id, sizes[i].name);
    }
    g_snprintf(id, sizeof(id), "%d", p->now.mouse.cursor_size);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->pointer_size), id)) {
        char text[32];
        g_snprintf(text, sizeof(text), "%d pixels", p->now.mouse.cursor_size);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->pointer_size), id, text);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->pointer_size), id);
    }
    gtk_box_pack_start(GTK_BOX(pointer), size_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pointer), p->pointer_size, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Mouse pointer", pointer), FALSE, FALSE, 0);

    g_signal_connect(p->contrast, "toggled", G_CALLBACK(on_contrast), p);
    g_signal_connect(p->contrast_scheme, "changed", G_CALLBACK(on_contrast), p);
    g_signal_connect(p->blink, "value-changed", G_CALLBACK(on_blink), p);
    g_signal_connect(p->pointer_size, "changed", G_CALLBACK(on_pointer_size), p);
    restart_caret(p);
    return page;
}

/* ---- Mouse ----------------------------------------------------------------------------- */

static GtkWidget *mouse_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    /* MouseKeys needs the compositor to move the pointer from the keypad. */
    gtk_box_pack_start(GTK_BOX(page),
                       frame("MouseKeys", unavailable("Use _MouseKeys",
                                                      "Use MouseKeys if you want to control "
                                                      "the pointer with the numeric keypad.")),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- applying ----------------------------------------------------------------------------- */

static bool apply(props *p) {
    /* The file as it is now, with what was changed here: the Mouse and
     * Keyboard applets write some of the same keys. */
    xd_settings s;
    xd_settings_load(&s);
    xd_access_settings *a = &s.access;
    a->sticky_keys = p->now.access.sticky_keys;
    a->sticky_lock = p->now.access.sticky_lock;
    a->bounce_keys = p->now.access.bounce_keys;
    a->bounce_ms = p->now.access.bounce_ms;
    bool contrast = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->contrast));
    const char *scheme = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->contrast_scheme));
    if (contrast && scheme == nullptr) {
        xd_message_box(GTK_WINDOW(p->window), "Accessibility Options", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "No High Contrast color scheme is installed.");
        return false;
    }
    if (contrast != a->high_contrast || (contrast && strcmp(scheme, s.scheme) != 0))
        xd_settings_set_high_contrast(&s, contrast, contrast ? scheme : "");
    if (p->now.keyboard.cursor_blink_ms != p->orig.keyboard.cursor_blink_ms)
        s.keyboard.cursor_blink_ms = p->now.keyboard.cursor_blink_ms;
    if (p->now.mouse.cursor_size != p->orig.mouse.cursor_size)
        s.mouse.cursor_size = p->now.mouse.cursor_size;
    if (!xd_settings_save(&s)) {
        xd_message_box(GTK_WINDOW(p->window), "Accessibility Options", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "The settings could not be saved.");
        return false;
    }
    p->orig = s;
    p->now.access = s.access;
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
    xd_gtk_app_preinit("org.xeondesktop.Accessibility");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static props p;
    xd_settings_load(&p.orig);
    p.now = p.orig;

    p.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.window), "Accessibility Options");
    gtk_window_set_icon_name(GTK_WINDOW(p.window), "preferences-desktop-accessibility");
    gtk_window_set_resizable(GTK_WINDOW(p.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.window), "Cancel", GTK_RESPONSE_CANCEL);
    p.apply = gtk_dialog_add_button(GTK_DIALOG(p.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(p.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    p.updating = true;
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), keyboard_page(&p),
                             gtk_label_new_with_mnemonic("_Keyboard"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), display_page(&p),
                             gtk_label_new_with_mnemonic("_Display"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), mouse_page(),
                             gtk_label_new_with_mnemonic("_Mouse"));
    p.updating = false;
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));

    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_main();
    if (p.caret_timer != 0)
        g_source_remove(p.caret_timer);
    return EXIT_SUCCESS;
}
