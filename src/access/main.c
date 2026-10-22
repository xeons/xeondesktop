/* SPDX-License-Identifier: MIT
 * xd-access: Windows XP's Accessibility Options. Writes [Accessibility]
 * in desktop.ini (and the High Contrast look into [Appearance], the
 * cursor options into [Keyboard] and [Mouse]); the compositor applies it
 * (input.c, access.c).
 *
 *   Keyboard  StickyKeys, FilterKeys (BounceKeys or SlowKeys), ToggleKeys,
 *             each with XP's Settings dialog and keyboard shortcut; the
 *             dialogs' "Show ... status on screen" puts an icon in the
 *             notification area while the feature is on (the shell's)
 *   Sound     SoundSentry (the compositor flashes the caption, window or
 *             desktop when it plays a sound) and ShowSounds (stored for
 *             programs; none read it yet)
 *   Display   High Contrast, the caret's blink rate, the pointer size
 *   Mouse     MouseKeys
 *   General   turning the features off after so long idle (the
 *             compositor's), the shortcuts' warning and sounds
 *
 * xd-access --shortcut stickykeys|filterkeys|togglekeys|mousekeys is what
 * the compositor runs when a shortcut has turned a feature on: XP's box
 * asking whether to keep it (Cancel turns it off again, Settings opens its
 * settings).
 *
 * xd-access [--page N] opens the Nth tab (0 Keyboard ... 4 General).
 *
 * Not here: the General tab's SerialKey devices and administrative
 * options.
 */
#include <math.h>
#include <stddef.h>
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
    GtkWidget *sticky, *filter, *toggle;
    /* Sound */
    GtkWidget *sentry, *sentry_effect, *show_sounds;
    /* Display */
    GtkWidget *contrast, *contrast_scheme, *blink, *caret, *pointer_size;
    bool caret_on;
    guint caret_timer;
    /* Mouse */
    GtkWidget *mouse_keys;
    /* General */
    GtkWidget *reset, *reset_minutes, *warn, *sound_on_off;
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
    GtkWidget *widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(widget), child);
    return widget;
}

static GtkWidget *wrapped(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 50);
    return widget;
}

static GtkWidget *check(GtkWidget *box, const char *label, bool on) {
    GtkWidget *widget = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), on);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    return widget;
}

static bool checked(GtkWidget *widget) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
}

/* ---- the Settings dialogs ------------------------------------------------------- */

/* A modal OK/Cancel dialog around `body`, shown. */
static GtkWidget *settings_dialog(GtkWindow *parent, const char *title, GtkWidget *body) {
    GtkWidget *widget = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(widget), title);
    gtk_window_set_modal(GTK_WINDOW(widget), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(widget), parent);
    gtk_dialog_add_button(GTK_DIALOG(widget), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(widget), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(widget), GTK_RESPONSE_OK);
    gtk_container_set_border_width(GTK_CONTAINER(body), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(widget))), body, TRUE, TRUE,
                       0);
    gtk_widget_show_all(widget);
    return widget;
}

static bool run_ok(GtkWidget *dialog) {
    return gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
}

/* "Keyboard shortcut": what it is, and Use shortcut. */
static GtkWidget *shortcut_frame(GtkWidget *page, const char *shortcut, bool on) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    char *text = g_strdup_printf("The shortcut for %s", shortcut);
    gtk_box_pack_start(GTK_BOX(box), wrapped(text), FALSE, FALSE, 0);
    g_free(text);
    GtkWidget *use = check(box, "_Use shortcut", on);
    gtk_box_pack_start(GTK_BOX(page), frame("Keyboard shortcut", box), FALSE, FALSE, 0);
    return use;
}

static bool sticky_settings(GtkWindow *parent, xd_access_settings *access_settings) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *shortcut = shortcut_frame(page, "StickyKeys is:\nPress the SHIFT key five times.",
                                         access_settings->sticky_shortcut);
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *lock =
        check(options, "_Press modifier key twice to lock", access_settings->sticky_lock);
    GtkWidget *two = check(options, "_Turn StickyKeys off if two keys are pressed at once",
                           access_settings->sticky_two_keys_off);
    gtk_box_pack_start(GTK_BOX(page), frame("Options", options), FALSE, FALSE, 0);
    GtkWidget *notify = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *sound =
        check(notify, "_Make sounds when modifier key is pressed", access_settings->sticky_sound);
    GtkWidget *indicator =
        check(notify, "_Show StickyKeys status on screen", access_settings->sticky_indicator);
    gtk_box_pack_start(GTK_BOX(page), frame("Notification", notify), FALSE, FALSE, 0);
    GtkWidget *widget = settings_dialog(parent, "Settings for StickyKeys", page);
    bool ok = run_ok(widget);
    if (ok) {
        access_settings->sticky_shortcut = checked(shortcut);
        access_settings->sticky_lock = checked(lock);
        access_settings->sticky_two_keys_off = checked(two);
        access_settings->sticky_sound = checked(sound);
        access_settings->sticky_indicator = checked(indicator);
    }
    gtk_widget_destroy(widget);
    return ok;
}

/* A combo of times in seconds ("0.50 seconds"), ids in ms; the current
 * value is added if it isn't one of them. */
static GtkWidget *seconds_combo(const int *ms, size_t count, int current) {
    GtkWidget *widget = gtk_combo_box_text_new();
    char id[16], text[32];
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        g_snprintf(id, sizeof(id), "%d", ms[i]);
        g_snprintf(text, sizeof(text), "%.2f seconds", ms[i] / 1000.0);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), id, text);
        found = found || ms[i] == current;
    }
    g_snprintf(id, sizeof(id), "%d", current);
    if (!found) {
        g_snprintf(text, sizeof(text), "%.2f seconds", current / 1000.0);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), id, text);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id);
    return widget;
}

static int combo_ms(GtkWidget *widget, int fallback) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget));
    return id != nullptr ? atoi(id) : fallback;
}

static GtkWidget *labelled(GtkWidget *box, const char *label, GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *caption = gtk_label_new_with_mnemonic(label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(caption), control);
    gtk_box_pack_start(GTK_BOX(row), caption, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), control, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}

/* BounceKeys' Advanced Settings. */
static void bounce_settings(GtkWindow *parent, xd_access_settings *access_settings) {
    static const int times[] = {500, 700, 1000, 1500, 2000};
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *combo = seconds_combo(times, G_N_ELEMENTS(times), access_settings->bounce_ms);
    labelled(box, "_Ignore keystrokes repeated faster than:", combo);
    gtk_box_pack_start(GTK_BOX(page), frame("BounceKeys", box), FALSE, FALSE, 0);
    GtkWidget *widget = settings_dialog(parent, "Advanced Settings for FilterKeys", page);
    if (run_ok(widget))
        access_settings->bounce_ms = combo_ms(combo, access_settings->bounce_ms);
    gtk_widget_destroy(widget);
}

typedef struct repeat_widgets {
    GtkWidget *slow_down, *delay, *rate;
} repeat_widgets;

static void on_repeat_kind([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    repeat_widgets *widgets = data;
    bool on = checked(widgets->slow_down);
    gtk_widget_set_sensitive(widgets->delay, on);
    gtk_widget_set_sensitive(widgets->rate, on);
}

/* SlowKeys' Advanced Settings: key repeat, and how long a key is held. */
static void slow_settings(GtkWindow *parent, xd_access_settings *access_settings) {
    static const int repeat_times[] = {300, 500, 700, 1000, 1500, 2000};
    static const int slow_times[] = {0, 300, 500, 700, 1000, 1400, 2000};
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *repeat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *none = gtk_radio_button_new_with_mnemonic(nullptr, "_No keyboard repeat");
    repeat_widgets widgets = {0};
    widgets.slow_down = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(none), "_Slow down keyboard repeat rates");
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(access_settings->filter_no_repeat ? none : widgets.slow_down), TRUE);
    gtk_box_pack_start(GTK_BOX(repeat), none, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(repeat), widgets.slow_down, FALSE, FALSE, 0);
    widgets.delay = seconds_combo(repeat_times, G_N_ELEMENTS(repeat_times),
                                  access_settings->filter_repeat_delay);
    widgets.rate =
        seconds_combo(repeat_times, G_N_ELEMENTS(repeat_times), access_settings->filter_repeat_ms);
    gtk_widget_set_margin_start(labelled(repeat, "Repeat _delay:", widgets.delay), 20);
    gtk_widget_set_margin_start(labelled(repeat, "Repeat _rate:", widgets.rate), 20);
    g_signal_connect(widgets.slow_down, "toggled", G_CALLBACK(on_repeat_kind), &widgets);
    on_repeat_kind(nullptr, &widgets);
    gtk_box_pack_start(GTK_BOX(page), frame("Repeat keys", repeat), FALSE, FALSE, 0);
    GtkWidget *slow = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *held = seconds_combo(slow_times, G_N_ELEMENTS(slow_times), access_settings->slow_ms);
    labelled(slow, "_Keys must be held down for:", held);
    gtk_box_pack_start(GTK_BOX(page), frame("SlowKeys", slow), FALSE, FALSE, 0);
    GtkWidget *widget = settings_dialog(parent, "Advanced Settings for FilterKeys", page);
    if (run_ok(widget)) {
        access_settings->filter_no_repeat = checked(none);
        access_settings->filter_repeat_delay =
            combo_ms(widgets.delay, access_settings->filter_repeat_delay);
        access_settings->filter_repeat_ms =
            combo_ms(widgets.rate, access_settings->filter_repeat_ms);
        access_settings->slow_ms = combo_ms(held, access_settings->slow_ms);
    }
    gtk_widget_destroy(widget);
}

typedef struct filter_dialog {
    GtkWidget *window;
    xd_access_settings *a; /* the dialog's copy */
    GtkWidget *bounce, *bounce_settings, *slow_settings;
} filter_dialog;

static void on_filter_kind([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    filter_dialog *filter_keys = data;
    bool bounce = checked(filter_keys->bounce);
    gtk_widget_set_sensitive(filter_keys->bounce_settings, bounce);
    gtk_widget_set_sensitive(filter_keys->slow_settings, !bounce);
}

static void on_bounce_settings(GtkWidget *widget, gpointer data) {
    filter_dialog *filter_keys = data;
    bounce_settings(GTK_WINDOW(gtk_widget_get_toplevel(widget)), filter_keys->a);
}

static void on_slow_settings(GtkWidget *widget, gpointer data) {
    filter_dialog *filter_keys = data;
    slow_settings(GTK_WINDOW(gtk_widget_get_toplevel(widget)), filter_keys->a);
}

static GtkWidget *option_row(GtkWidget *box, GtkWidget *option, GtkWidget **settings) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row), option, TRUE, TRUE, 0);
    *settings = gtk_button_new_with_mnemonic("Se_ttings");
    gtk_widget_set_valign(*settings, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(row), *settings, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}

static bool filter_settings(GtkWindow *parent, xd_access_settings *access_settings) {
    xd_access_settings copy = *access_settings;
    filter_dialog filter_keys = {.a = &copy};
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *shortcut =
        shortcut_frame(page, "FilterKeys is:\nHold down right SHIFT for eight seconds.",
                       access_settings->filter_shortcut);
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    filter_keys.bounce = gtk_radio_button_new_with_mnemonic(nullptr, "Ignore _repeated keystrokes");
    GtkWidget *slow = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(filter_keys.bounce),
        "Ignore _quick keystrokes and slow down the repeat rate");
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(access_settings->slow_keys ? slow : filter_keys.bounce), TRUE);
    option_row(options, filter_keys.bounce, &filter_keys.bounce_settings);
    option_row(options, slow, &filter_keys.slow_settings);
    g_signal_connect(filter_keys.bounce, "toggled", G_CALLBACK(on_filter_kind), &filter_keys);
    g_signal_connect(filter_keys.bounce_settings, "clicked", G_CALLBACK(on_bounce_settings),
                     &filter_keys);
    g_signal_connect(filter_keys.slow_settings, "clicked", G_CALLBACK(on_slow_settings),
                     &filter_keys);
    on_filter_kind(nullptr, &filter_keys);
    gtk_box_pack_start(GTK_BOX(page), frame("Filter options", options), FALSE, FALSE, 0);
    GtkWidget *notify = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *beep =
        check(notify, "_Beep when keys pressed or accepted", access_settings->filter_beep);
    GtkWidget *indicator =
        check(notify, "_Show FilterKeys status on screen", access_settings->filter_indicator);
    gtk_box_pack_start(GTK_BOX(page), frame("Notification", notify), FALSE, FALSE, 0);
    GtkWidget *widget = settings_dialog(parent, "Settings for FilterKeys", page);
    bool ok = run_ok(widget);
    if (ok) {
        copy.filter_shortcut = checked(shortcut);
        copy.slow_keys = !checked(filter_keys.bounce);
        copy.filter_beep = checked(beep);
        copy.filter_indicator = checked(indicator);
        *access_settings = copy;
    }
    gtk_widget_destroy(widget);
    return ok;
}

static bool toggle_settings(GtkWindow *parent, xd_access_settings *access_settings) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *shortcut =
        shortcut_frame(page, "ToggleKeys is:\nHold down NUM LOCK for five seconds.",
                       access_settings->toggle_shortcut);
    GtkWidget *widget = settings_dialog(parent, "Settings for ToggleKeys", page);
    bool ok = run_ok(widget);
    if (ok)
        access_settings->toggle_shortcut = checked(shortcut);
    gtk_widget_destroy(widget);
    return ok;
}

static GtkWidget *slider_row(GtkWidget *box, const char *label, const char *left,
                             GtkWidget *scale, const char *right) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *widget = gtk_label_new_with_mnemonic(label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), scale);
    gtk_label_set_width_chars(GTK_LABEL(widget), 13);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_size_request(scale, 160, -1);
    gtk_box_pack_start(GTK_BOX(row), widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(left), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(right), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}

static bool mouse_keys_settings(GtkWindow *parent, xd_access_settings *access_settings) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *shortcut = shortcut_frame(page, "MouseKeys is:\nLeft ALT + left SHIFT + NUM LOCK",
                                         access_settings->mouse_keys_shortcut);
    GtkWidget *speed = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *top = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, XD_MOUSE_KEYS_SPEED_MIN,
                                              XD_MOUSE_KEYS_SPEED_MAX, 10);
    gtk_range_set_value(GTK_RANGE(top), access_settings->mouse_keys_speed);
    slider_row(speed, "_Top speed:", "Low", top, "High");
    /* Fast on the right: the shortest time to top speed. */
    GtkWidget *accel = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                XD_MOUSE_KEYS_ACCEL_MIN, XD_MOUSE_KEYS_ACCEL_MAX,
                                                500);
    gtk_range_set_inverted(GTK_RANGE(accel), TRUE);
    gtk_range_set_value(GTK_RANGE(accel), access_settings->mouse_keys_accel_ms);
    slider_row(speed, "_Acceleration:", "Slow", accel, "Fast");
    GtkWidget *mods = check(speed, "_Hold down Ctrl to speed up and Shift to slow down",
                            access_settings->mouse_keys_modifiers);
    gtk_box_pack_start(GTK_BOX(page), frame("Pointer speed", speed), FALSE, FALSE, 0);
    GtkWidget *num = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(num), gtk_label_new("Use MouseKeys when NumLock is:"), FALSE,
                       FALSE, 0);
    GtkWidget *on = gtk_radio_button_new_with_mnemonic(nullptr, "O_n");
    GtkWidget *off = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(on), "O_ff");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(access_settings->mouse_keys_numlock ? on : off),
                                 TRUE);
    gtk_box_pack_start(GTK_BOX(num), on, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(num), off, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), num, FALSE, FALSE, 0);
    GtkWidget *indicator =
        check(page, "_Show MouseKey status on screen", access_settings->mouse_keys_indicator);
    GtkWidget *widget = settings_dialog(parent, "Settings for MouseKeys", page);
    bool ok = run_ok(widget);
    if (ok) {
        access_settings->mouse_keys_shortcut = checked(shortcut);
        access_settings->mouse_keys_speed = range_int(GTK_RANGE(top));
        access_settings->mouse_keys_accel_ms = range_int(GTK_RANGE(accel));
        access_settings->mouse_keys_modifiers = checked(mods);
        access_settings->mouse_keys_numlock = checked(on);
        access_settings->mouse_keys_indicator = checked(indicator);
    }
    gtk_widget_destroy(widget);
    return ok;
}

/* ---- Keyboard ------------------------------------------------------------------------ */

static void on_keyboard_option([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    xd_access_settings *access_settings = &properties->now.access;
    access_settings->sticky_keys = checked(properties->sticky);
    access_settings->filter_keys = checked(properties->filter);
    access_settings->toggle_keys = checked(properties->toggle);
    changed(properties);
}

typedef bool (*settings_fn)(GtkWindow *parent, xd_access_settings *access_settings);

static void on_settings_button(GtkWidget *widget, gpointer data) {
    props *properties = g_object_get_data(G_OBJECT(widget), "props");
    settings_fn fn = (settings_fn)(uintptr_t)data;
    if (fn(GTK_WINDOW(properties->window), &properties->now.access))
        changed(properties);
}

/* A feature's frame: what it's for, Use ..., and Settings. */
static GtkWidget *feature_frame(props *properties, GtkWidget *page, const char *title,
                                const char *text, const char *use, bool on, settings_fn settings) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), wrapped(text), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *use_check = gtk_check_button_new_with_mnemonic(use);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(use_check), on);
    gtk_box_pack_start(GTK_BOX(row), use_check, TRUE, TRUE, 0);
    GtkWidget *settings_button = gtk_button_new_with_mnemonic("_Settings");
    gtk_widget_set_size_request(settings_button, 75, -1);
    g_object_set_data(G_OBJECT(settings_button), "props", properties);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(on_settings_button),
                     (gpointer)(uintptr_t)settings);
    gtk_box_pack_end(GTK_BOX(row), settings_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame(title, box), FALSE, FALSE, 0);
    return use_check;
}

static GtkWidget *keyboard_page(props *properties) {
    const xd_access_settings *access_settings = &properties->now.access;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    /* Each frame's Settings button has its own mnemonic in XP; GTK picks
     * the next one on a repeated key. */
    properties->sticky =
        feature_frame(properties, page, "StickyKeys",
                      "Use StickyKeys if you want to use SHIFT, CTRL, ALT, or Windows "
                      "logo key by pressing one key at a time.",
                      "Use _StickyKeys", access_settings->sticky_keys, sticky_settings);
    properties->filter =
        feature_frame(properties, page, "FilterKeys",
                      "Use FilterKeys if you want Windows to ignore brief or repeated "
                      "keystrokes, or slow the repeat rate.",
                      "Use _FilterKeys", access_settings->filter_keys, filter_settings);
    properties->toggle =
        feature_frame(properties, page, "ToggleKeys",
                      "Use ToggleKeys if you want to hear tones when pressing CAPS "
                      "LOCK, NUM LOCK, and SCROLL LOCK.",
                      "Use T_oggleKeys", access_settings->toggle_keys, toggle_settings);
    g_signal_connect(properties->sticky, "toggled", G_CALLBACK(on_keyboard_option), properties);
    g_signal_connect(properties->filter, "toggled", G_CALLBACK(on_keyboard_option), properties);
    g_signal_connect(properties->toggle, "toggled", G_CALLBACK(on_keyboard_option), properties);
    return page;
}

/* ---- Sound --------------------------------------------------------------------------- */

static void on_sound_option([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    xd_access_settings *access_settings = &properties->now.access;
    access_settings->sound_sentry = checked(properties->sentry);
    access_settings->show_sounds = checked(properties->show_sounds);
    /* The list is in xd_sound_sentry's order. */
    int effect = gtk_combo_box_get_active(GTK_COMBO_BOX(properties->sentry_effect));
    if (effect >= 0 && effect < XD_SENTRY_COUNT)
        access_settings->sound_sentry_effect = (xd_sound_sentry)effect;
    gtk_widget_set_sensitive(properties->sentry_effect, access_settings->sound_sentry);
    changed(properties);
}

static GtkWidget *sound_page(props *properties) {
    const xd_access_settings *access_settings = &properties->now.access;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *sentry = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(sentry),
                       wrapped("Use SoundSentry if you want Windows to generate visual warnings "
                               "when your system makes a sound."),
                       FALSE, FALSE, 0);
    properties->sentry = check(sentry, "Use _SoundSentry", access_settings->sound_sentry);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Choose the visual warning:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(sentry), label, FALSE, FALSE, 0);
    properties->sentry_effect = gtk_combo_box_text_new();
    static const char *const effects[XD_SENTRY_COUNT] = {
        [XD_SENTRY_NONE] = "[None]",
        [XD_SENTRY_CAPTION] = "Flash active caption bar",
        [XD_SENTRY_WINDOW] = "Flash active window",
        [XD_SENTRY_DESKTOP] = "Flash desktop",
    };
    char id[8];
    for (int i = 0; i < XD_SENTRY_COUNT; i++) {
        g_snprintf(id, sizeof(id), "%d", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->sentry_effect), id, effects[i]);
    }
    g_snprintf(id, sizeof(id), "%d", (int)access_settings->sound_sentry_effect);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->sentry_effect), id);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->sentry_effect);
    gtk_widget_set_sensitive(properties->sentry_effect, access_settings->sound_sentry);
    gtk_box_pack_start(GTK_BOX(sentry), properties->sentry_effect, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("SoundSentry", sentry), FALSE, FALSE, 0);

    GtkWidget *show = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(show),
                       wrapped("Use ShowSounds to tell your programs to display captions for the "
                               "speech and sounds they make."),
                       FALSE, FALSE, 0);
    properties->show_sounds = check(show, "Use Show_Sounds", access_settings->show_sounds);
    gtk_box_pack_start(GTK_BOX(page), frame("ShowSounds", show), FALSE, FALSE, 0);

    g_signal_connect(properties->sentry, "toggled", G_CALLBACK(on_sound_option), properties);
    g_signal_connect(properties->sentry_effect, "changed", G_CALLBACK(on_sound_option), properties);
    g_signal_connect(properties->show_sounds, "toggled", G_CALLBACK(on_sound_option), properties);
    return page;
}

/* ---- Display ------------------------------------------------------------------------- */

static void on_contrast([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    gtk_widget_set_sensitive(properties->contrast_scheme, checked(properties->contrast));
    changed(properties);
}

/* Blink rate notches, as in Keyboard Properties: 0 is no blinking, 1..10
 * slow (1200 ms) to fast. */
static int blink_ms(int notch) { return notch == 0 ? 0 : 1300 - notch * 100; }

static gboolean draw_caret(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    props *properties = data;
    if (!properties->caret_on)
        return FALSE;
    GtkStyleContext *sc = gtk_widget_get_style_context(widget);
    GdkRGBA fg;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    gdk_cairo_set_source_rgba(cairo, &fg);
    cairo_rectangle(cairo, 2, 2, 1, gtk_widget_get_allocated_height(widget) - 4);
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
    int ms = properties->now.keyboard.cursor_blink_ms;
    if (ms > 0)
        properties->caret_timer = g_timeout_add((guint)ms, blink_caret, properties);
}

static void on_blink(GtkRange *range, gpointer data) {
    props *properties = data;
    properties->now.keyboard.cursor_blink_ms = blink_ms(range_int(range));
    restart_caret(properties);
    changed(properties);
}

static void on_pointer_size(GtkComboBox *combo, gpointer data) {
    props *properties = data;
    const char *id = gtk_combo_box_get_active_id(combo);
    if (id != nullptr)
        properties->now.mouse.cursor_size = atoi(id);
    changed(properties);
}

/* The installed schemes marked HighContrast. */
static void fill_contrast_schemes(props *properties) {
    xd_scheme_list list;
    xd_scheme_list_load(&list);
    for (size_t i = 0; i < list.count; i++) {
        xd_settings probe;
        xd_settings_defaults(&probe);
        g_strlcpy(probe.scheme, list.stems[i], sizeof(probe.scheme));
        xd_scheme scheme;
        xd_settings_scheme(&probe, &scheme);
        if (scheme.high_contrast)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->contrast_scheme),
                                      list.stems[i], list.names[i]);
    }
    xd_scheme_list_free(&list);
    if (!properties->now.access.high_contrast ||
        !gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->contrast_scheme),
                                     properties->now.scheme))
        gtk_combo_box_set_active(GTK_COMBO_BOX(properties->contrast_scheme), 0);
}

static GtkWidget *display_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *contrast = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(contrast),
                       wrapped("Use this option if you want Windows to use colors and fonts "
                               "designed for easy reading."),
                       FALSE, FALSE, 0);
    properties->contrast =
        check(contrast, "Use _High Contrast", properties->now.access.high_contrast);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 20);
    GtkWidget *label = gtk_label_new_with_mnemonic("Hi_gh Contrast scheme:");
    properties->contrast_scheme = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), properties->contrast_scheme);
    fill_contrast_schemes(properties);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), properties->contrast_scheme, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(contrast), row, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(properties->contrast_scheme, properties->now.access.high_contrast);
    gtk_box_pack_start(GTK_BOX(page), frame("High Contrast", contrast), FALSE, FALSE, 0);

    GtkWidget *cursor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *blink_label = gtk_label_new_with_mnemonic("_Blink Rate:");
    gtk_label_set_xalign(GTK_LABEL(blink_label), 0);
    gtk_box_pack_start(GTK_BOX(cursor), blink_label, FALSE, FALSE, 0);
    GtkWidget *blink_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    properties->blink = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 10, 1);
    for (int mark = 0; mark <= 10; mark++)
        gtk_scale_add_mark(GTK_SCALE(properties->blink), mark, GTK_POS_BOTTOM, nullptr);
    gtk_scale_set_draw_value(GTK_SCALE(properties->blink), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(properties->blink), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(blink_label), properties->blink);
    int blink_ms = properties->now.keyboard.cursor_blink_ms;
    gtk_range_set_value(GTK_RANGE(properties->blink),
                        blink_ms == 0 ? 0
                                      : XD_CLAMP((int)lround((1300 - blink_ms) / 100.0), 1, 10));
    gtk_widget_set_size_request(properties->blink, 180, -1);
    gtk_box_pack_start(GTK_BOX(blink_row), gtk_label_new("None"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(blink_row), properties->blink, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(blink_row), gtk_label_new("Fast"), FALSE, FALSE, 0);
    properties->caret = gtk_drawing_area_new();
    gtk_widget_set_size_request(properties->caret, 8, 24);
    g_signal_connect(properties->caret, "draw", G_CALLBACK(draw_caret), properties);
    gtk_box_pack_start(GTK_BOX(blink_row), properties->caret, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(cursor), blink_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Cursor Options", cursor), FALSE, FALSE, 0);

    GtkWidget *pointer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *size_label = gtk_label_new_with_mnemonic("Mouse _pointer size:");
    properties->pointer_size = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(size_label), properties->pointer_size);
    static const struct {
        int size;
        const char *name;
    } sizes[] = {{24, "Normal"}, {32, "Large"}, {48, "Extra Large"}};
    char id[16];
    for (size_t i = 0; i < G_N_ELEMENTS(sizes); i++) {
        g_snprintf(id, sizeof(id), "%d", sizes[i].size);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->pointer_size), id, sizes[i].name);
    }
    g_snprintf(id, sizeof(id), "%d", properties->now.mouse.cursor_size);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->pointer_size), id)) {
        char text[32];
        g_snprintf(text, sizeof(text), "%d pixels", properties->now.mouse.cursor_size);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->pointer_size), id, text);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->pointer_size), id);
    }
    gtk_box_pack_start(GTK_BOX(pointer), size_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pointer), properties->pointer_size, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Mouse pointer", pointer), FALSE, FALSE, 0);

    g_signal_connect(properties->contrast, "toggled", G_CALLBACK(on_contrast), properties);
    g_signal_connect(properties->contrast_scheme, "changed", G_CALLBACK(on_contrast), properties);
    g_signal_connect(properties->blink, "value-changed", G_CALLBACK(on_blink), properties);
    g_signal_connect(properties->pointer_size, "changed", G_CALLBACK(on_pointer_size), properties);
    restart_caret(properties);
    return page;
}

/* ---- Mouse and General ----------------------------------------------------------- */

static void on_mouse_keys([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    properties->now.access.mouse_keys = checked(properties->mouse_keys);
    changed(properties);
}

static GtkWidget *mouse_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    properties->mouse_keys =
        feature_frame(properties, page, "MouseKeys",
                      "Use MouseKeys if you want to control the pointer with the "
                      "numeric keypad on your keyboard.",
                      "Use _MouseKeys", properties->now.access.mouse_keys, mouse_keys_settings);
    g_signal_connect(properties->mouse_keys, "toggled", G_CALLBACK(on_mouse_keys), properties);
    return page;
}

static void on_general([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props *properties = data;
    properties->now.access.auto_reset = checked(properties->reset);
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->reset_minutes));
    if (id != nullptr)
        properties->now.access.auto_reset_minutes = atoi(id);
    gtk_widget_set_sensitive(properties->reset_minutes, properties->now.access.auto_reset);
    properties->now.access.warn = checked(properties->warn);
    properties->now.access.sound_on_off = checked(properties->sound_on_off);
    changed(properties);
}

static GtkWidget *general_page(props *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    const xd_access_settings *access_settings = &properties->now.access;
    GtkWidget *reset = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    properties->reset = check(
        reset, "_Turn off accessibility features after idle for:", access_settings->auto_reset);
    properties->reset_minutes = gtk_combo_box_text_new();
    char id[8], text[32];
    for (int option_minutes = XD_AUTO_RESET_MIN; option_minutes <= XD_AUTO_RESET_MAX;
         option_minutes += 5) {
        g_snprintf(id, sizeof(id), "%d", option_minutes);
        g_snprintf(text, sizeof(text), "%d minutes", option_minutes);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->reset_minutes), id, text);
    }
    g_snprintf(id, sizeof(id), "%d", access_settings->auto_reset_minutes);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->reset_minutes), id)) {
        g_snprintf(text, sizeof(text), "%d minutes", access_settings->auto_reset_minutes);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(properties->reset_minutes), id, text);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(properties->reset_minutes), id);
    }
    gtk_widget_set_margin_start(properties->reset_minutes, 20);
    gtk_widget_set_halign(properties->reset_minutes, GTK_ALIGN_START);
    gtk_widget_set_sensitive(properties->reset_minutes, access_settings->auto_reset);
    gtk_box_pack_start(GTK_BOX(reset), properties->reset_minutes, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Automatic reset", reset), FALSE, FALSE, 0);
    GtkWidget *notify = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    properties->warn = check(notify, "Give _warning message when turning a feature on",
                             properties->now.access.warn);
    properties->sound_on_off = check(notify, "Make a sound when turning a feature on or o_ff",
                                     properties->now.access.sound_on_off);
    gtk_box_pack_start(GTK_BOX(page), frame("Notification", notify), FALSE, FALSE, 0);
    g_signal_connect(properties->reset, "toggled", G_CALLBACK(on_general), properties);
    g_signal_connect(properties->reset_minutes, "changed", G_CALLBACK(on_general), properties);
    g_signal_connect(properties->warn, "toggled", G_CALLBACK(on_general), properties);
    g_signal_connect(properties->sound_on_off, "toggled", G_CALLBACK(on_general), properties);
    return page;
}

/* ---- applying ----------------------------------------------------------------------------- */

/* Our [Accessibility] over `settings`'s, but its High Contrast (set apart). */
static void merge_access(xd_settings *settings, const xd_access_settings *now) {
    xd_access_settings keep = settings->access;
    settings->access = *now;
    settings->access.high_contrast = keep.high_contrast;
    settings->access.saved_style = keep.saved_style;
    memcpy(settings->access.saved_scheme, keep.saved_scheme, sizeof(keep.saved_scheme));
}

static bool apply(props *properties) {
    /* The file as it is now, with what was changed here: the Mouse and
     * Keyboard applets write some of the same keys. */
    xd_settings settings;
    xd_settings_load(&settings);
    merge_access(&settings, &properties->now.access);
    bool contrast = checked(properties->contrast);
    const char *scheme = gtk_combo_box_get_active_id(GTK_COMBO_BOX(properties->contrast_scheme));
    if (contrast && scheme == nullptr) {
        xd_message_box(GTK_WINDOW(properties->window), "Accessibility Options", XD_MB_ERROR,
                       XD_MB_OK, FALSE, "No High Contrast color scheme is installed.");
        return false;
    }
    if (contrast != settings.access.high_contrast ||
        (contrast && strcmp(scheme, settings.scheme) != 0))
        xd_settings_set_high_contrast(&settings, contrast, contrast ? scheme : "");
    if (properties->now.keyboard.cursor_blink_ms != properties->orig.keyboard.cursor_blink_ms)
        settings.keyboard.cursor_blink_ms = properties->now.keyboard.cursor_blink_ms;
    if (properties->now.mouse.cursor_size != properties->orig.mouse.cursor_size)
        settings.mouse.cursor_size = properties->now.mouse.cursor_size;
    if (!xd_settings_save(&settings)) {
        xd_message_box(GTK_WINDOW(properties->window), "Accessibility Options", XD_MB_ERROR,
                       XD_MB_OK, FALSE, "The settings could not be saved.");
        return false;
    }
    properties->orig = settings;
    properties->now.access = settings.access;
    gtk_widget_set_sensitive(properties->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *dialog, int response, gpointer data) {
    props *properties = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(properties);
        return;
    }
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(properties->apply) &&
        !apply(properties))
        return;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* ---- xd-access --shortcut ---------------------------------------------------------- */

static const struct {
    const char *arg, *title, *text;
    size_t flag; /* offsetof the feature's switch */
    settings_fn settings;
} shortcuts[] = {
    {"stickykeys", "StickyKeys",
     "StickyKeys lets you use the SHIFT, CTRL, ALT, or Windows Logo keys by pressing one key at "
     "a time. The keyboard shortcut to turn on StickyKeys is to press the SHIFT key 5 times.",
     offsetof(xd_access_settings, sticky_keys), sticky_settings},
    {"filterkeys", "FilterKeys",
     "FilterKeys lets you ignore brief or repeated keystrokes, or slow the repeat rate. The "
     "keyboard shortcut to turn on FilterKeys is to hold down the right SHIFT key for 8 seconds.",
     offsetof(xd_access_settings, filter_keys), filter_settings},
    {"togglekeys", "ToggleKeys",
     "ToggleKeys lets you hear tones when pressing CAPS LOCK, NUM LOCK, and SCROLL LOCK. The "
     "keyboard shortcut to turn on ToggleKeys is to hold down the NUM LOCK key for 5 seconds.",
     offsetof(xd_access_settings, toggle_keys), toggle_settings},
    {"mousekeys", "MouseKeys",
     "MouseKeys lets you use the numeric keypad to move the mouse pointer. The keyboard shortcut "
     "to turn on MouseKeys is LEFT ALT+LEFT SHIFT+NUM LOCK.",
     offsetof(xd_access_settings, mouse_keys), mouse_keys_settings},
};

enum { CONFIRM_OK, CONFIRM_CANCEL, CONFIRM_SETTINGS };

/* XP's box after a shortcut turned a feature on (the compositor already
 * has): OK keeps it, Cancel turns it off, Settings opens its settings. */
static int confirm_shortcut(const char *arg) {
    size_t i = 0;
    while (i < G_N_ELEMENTS(shortcuts) && strcmp(shortcuts[i].arg, arg) != 0)
        i++;
    if (i == G_N_ELEMENTS(shortcuts)) {
        fprintf(stderr, "xd-access: unknown feature '%s'\n", arg);
        return EXIT_FAILURE;
    }
    GtkWidget *widget = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(widget), shortcuts[i].title);
    gtk_window_set_icon_name(GTK_WINDOW(widget), "preferences-desktop-accessibility");
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(widget), "OK", CONFIRM_OK);
    gtk_dialog_add_button(GTK_DIALOG(widget), "Cancel", CONFIRM_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(widget), "_Settings", CONFIRM_SETTINGS);
    gtk_dialog_set_default_response(GTK_DIALOG(widget), CONFIRM_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *image =
        gtk_image_new_from_icon_name("preferences-desktop-accessibility", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
    gtk_widget_set_valign(image, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    char *text = g_strdup_printf("%s\n\nTo keep %s on, click OK.\nTo cancel %s, click Cancel.\n"
                                 "To deactivate the key combination for %s, click Settings.",
                                 shortcuts[i].text, shortcuts[i].title, shortcuts[i].title,
                                 shortcuts[i].title);
    GtkWidget *label = wrapped(text);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    g_free(text);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(widget))), box, TRUE, TRUE,
                       0);
    gtk_widget_show_all(widget);
    int response = gtk_dialog_run(GTK_DIALOG(widget));
    gtk_widget_hide(widget);
    xd_settings settings;
    xd_settings_load(&settings);
    bool save = false;
    if (response == CONFIRM_CANCEL || response == GTK_RESPONSE_DELETE_EVENT) {
        *(bool *)((char *)&settings.access + shortcuts[i].flag) = false;
        save = true;
    } else if (response == CONFIRM_SETTINGS) {
        save = shortcuts[i].settings(GTK_WINDOW(widget), &settings.access);
    }
    gtk_widget_destroy(widget);
    if (save && !xd_settings_save(&settings)) {
        xd_message_box(nullptr, shortcuts[i].title, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The settings could not be saved.");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.Accessibility");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    if (argc == 3 && strcmp(argv[1], "--shortcut") == 0)
        return confirm_shortcut(argv[2]);
    int page = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--page") == 0)
            page = atoi(argv[++i]);
    }
    static props properties;
    xd_settings_load(&properties.orig);
    properties.now = properties.orig;

    properties.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(properties.window), "Accessibility Options");
    gtk_window_set_icon_name(GTK_WINDOW(properties.window), "preferences-desktop-accessibility");
    gtk_window_set_resizable(GTK_WINDOW(properties.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties.window), "Cancel", GTK_RESPONSE_CANCEL);
    properties.apply =
        gtk_dialog_add_button(GTK_DIALOG(properties.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(properties.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    properties.updating = true;
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), keyboard_page(&properties),
                             gtk_label_new_with_mnemonic("_Keyboard"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), sound_page(&properties),
                             gtk_label_new_with_mnemonic("_Sound"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), display_page(&properties),
                             gtk_label_new_with_mnemonic("_Display"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), mouse_page(&properties),
                             gtk_label_new_with_mnemonic("_Mouse"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(&properties),
                             gtk_label_new_with_mnemonic("_General"));
    properties.updating = false;
    gtk_widget_set_sensitive(properties.apply, FALSE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties.window))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties.window, GTK_NOTEBOOK(tabs));

    g_signal_connect(properties.window, "response", G_CALLBACK(on_response), &properties);
    g_signal_connect(properties.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(properties.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs),
                                  XD_CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(tabs)) - 1));
    gtk_main();
    if (properties.caret_timer != 0)
        g_source_remove(properties.caret_timer);
    return EXIT_SUCCESS;
}
