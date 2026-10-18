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

static GtkWidget *check(GtkWidget *box, const char *label, bool on) {
    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), on);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
    return c;
}

static bool checked(GtkWidget *c) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c)); }

/* ---- the Settings dialogs ------------------------------------------------------- */

/* A modal OK/Cancel dialog around `body`, shown. */
static GtkWidget *settings_dialog(GtkWindow *parent, const char *title, GtkWidget *body) {
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), title);
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(d), parent);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_container_set_border_width(GTK_CONTAINER(body), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), body, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    return d;
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

static bool sticky_settings(GtkWindow *parent, xd_access_settings *a) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *shortcut =
        shortcut_frame(page, "StickyKeys is:\nPress the SHIFT key five times.", a->sticky_shortcut);
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *lock = check(options, "_Press modifier key twice to lock", a->sticky_lock);
    GtkWidget *two = check(options, "_Turn StickyKeys off if two keys are pressed at once",
                           a->sticky_two_keys_off);
    gtk_box_pack_start(GTK_BOX(page), frame("Options", options), FALSE, FALSE, 0);
    GtkWidget *notify = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *sound = check(notify, "_Make sounds when modifier key is pressed", a->sticky_sound);
    GtkWidget *indicator = check(notify, "_Show StickyKeys status on screen", a->sticky_indicator);
    gtk_box_pack_start(GTK_BOX(page), frame("Notification", notify), FALSE, FALSE, 0);
    GtkWidget *d = settings_dialog(parent, "Settings for StickyKeys", page);
    bool ok = run_ok(d);
    if (ok) {
        a->sticky_shortcut = checked(shortcut);
        a->sticky_lock = checked(lock);
        a->sticky_two_keys_off = checked(two);
        a->sticky_sound = checked(sound);
        a->sticky_indicator = checked(indicator);
    }
    gtk_widget_destroy(d);
    return ok;
}

/* A combo of times in seconds ("0.50 seconds"), ids in ms; the current
 * value is added if it isn't one of them. */
static GtkWidget *seconds_combo(const int *ms, size_t n, int current) {
    GtkWidget *c = gtk_combo_box_text_new();
    char id[16], text[32];
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        g_snprintf(id, sizeof(id), "%d", ms[i]);
        g_snprintf(text, sizeof(text), "%.2f seconds", ms[i] / 1000.0);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), id, text);
        found = found || ms[i] == current;
    }
    g_snprintf(id, sizeof(id), "%d", current);
    if (!found) {
        g_snprintf(text, sizeof(text), "%.2f seconds", current / 1000.0);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), id, text);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), id);
    return c;
}

static int combo_ms(GtkWidget *c, int fallback) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(c));
    return id != nullptr ? atoi(id) : fallback;
}

static GtkWidget *labelled(GtkWidget *box, const char *label, GtkWidget *w) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}

/* BounceKeys' Advanced Settings. */
static void bounce_settings(GtkWindow *parent, xd_access_settings *a) {
    static const int times[] = {500, 700, 1000, 1500, 2000};
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *combo = seconds_combo(times, G_N_ELEMENTS(times), a->bounce_ms);
    labelled(box, "_Ignore keystrokes repeated faster than:", combo);
    gtk_box_pack_start(GTK_BOX(page), frame("BounceKeys", box), FALSE, FALSE, 0);
    GtkWidget *d = settings_dialog(parent, "Advanced Settings for FilterKeys", page);
    if (run_ok(d))
        a->bounce_ms = combo_ms(combo, a->bounce_ms);
    gtk_widget_destroy(d);
}

typedef struct repeat_widgets {
    GtkWidget *slow_down, *delay, *rate;
} repeat_widgets;

static void on_repeat_kind([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    repeat_widgets *w = data;
    bool on = checked(w->slow_down);
    gtk_widget_set_sensitive(w->delay, on);
    gtk_widget_set_sensitive(w->rate, on);
}

/* SlowKeys' Advanced Settings: key repeat, and how long a key is held. */
static void slow_settings(GtkWindow *parent, xd_access_settings *a) {
    static const int repeat_times[] = {300, 500, 700, 1000, 1500, 2000};
    static const int slow_times[] = {0, 300, 500, 700, 1000, 1400, 2000};
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *repeat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *none = gtk_radio_button_new_with_mnemonic(nullptr, "_No keyboard repeat");
    repeat_widgets w = {0};
    w.slow_down = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(none), "_Slow down keyboard repeat rates");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->filter_no_repeat ? none : w.slow_down),
                                 TRUE);
    gtk_box_pack_start(GTK_BOX(repeat), none, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(repeat), w.slow_down, FALSE, FALSE, 0);
    w.delay = seconds_combo(repeat_times, G_N_ELEMENTS(repeat_times), a->filter_repeat_delay);
    w.rate = seconds_combo(repeat_times, G_N_ELEMENTS(repeat_times), a->filter_repeat_ms);
    gtk_widget_set_margin_start(labelled(repeat, "Repeat _delay:", w.delay), 20);
    gtk_widget_set_margin_start(labelled(repeat, "Repeat _rate:", w.rate), 20);
    g_signal_connect(w.slow_down, "toggled", G_CALLBACK(on_repeat_kind), &w);
    on_repeat_kind(nullptr, &w);
    gtk_box_pack_start(GTK_BOX(page), frame("Repeat keys", repeat), FALSE, FALSE, 0);
    GtkWidget *slow = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *held = seconds_combo(slow_times, G_N_ELEMENTS(slow_times), a->slow_ms);
    labelled(slow, "_Keys must be held down for:", held);
    gtk_box_pack_start(GTK_BOX(page), frame("SlowKeys", slow), FALSE, FALSE, 0);
    GtkWidget *d = settings_dialog(parent, "Advanced Settings for FilterKeys", page);
    if (run_ok(d)) {
        a->filter_no_repeat = checked(none);
        a->filter_repeat_delay = combo_ms(w.delay, a->filter_repeat_delay);
        a->filter_repeat_ms = combo_ms(w.rate, a->filter_repeat_ms);
        a->slow_ms = combo_ms(held, a->slow_ms);
    }
    gtk_widget_destroy(d);
}

typedef struct filter_dialog {
    GtkWidget *window;
    xd_access_settings *a; /* the dialog's copy */
    GtkWidget *bounce, *bounce_settings, *slow_settings;
} filter_dialog;

static void on_filter_kind([[maybe_unused]] GtkToggleButton *b, gpointer data) {
    filter_dialog *f = data;
    bool bounce = checked(f->bounce);
    gtk_widget_set_sensitive(f->bounce_settings, bounce);
    gtk_widget_set_sensitive(f->slow_settings, !bounce);
}

static void on_bounce_settings(GtkWidget *b, gpointer data) {
    filter_dialog *f = data;
    bounce_settings(GTK_WINDOW(gtk_widget_get_toplevel(b)), f->a);
}

static void on_slow_settings(GtkWidget *b, gpointer data) {
    filter_dialog *f = data;
    slow_settings(GTK_WINDOW(gtk_widget_get_toplevel(b)), f->a);
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

static bool filter_settings(GtkWindow *parent, xd_access_settings *a) {
    xd_access_settings copy = *a;
    filter_dialog f = {.a = &copy};
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *shortcut = shortcut_frame(
        page, "FilterKeys is:\nHold down right SHIFT for eight seconds.", a->filter_shortcut);
    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    f.bounce = gtk_radio_button_new_with_mnemonic(nullptr, "Ignore _repeated keystrokes");
    GtkWidget *slow = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(f.bounce), "Ignore _quick keystrokes and slow down the repeat rate");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->slow_keys ? slow : f.bounce), TRUE);
    option_row(options, f.bounce, &f.bounce_settings);
    option_row(options, slow, &f.slow_settings);
    g_signal_connect(f.bounce, "toggled", G_CALLBACK(on_filter_kind), &f);
    g_signal_connect(f.bounce_settings, "clicked", G_CALLBACK(on_bounce_settings), &f);
    g_signal_connect(f.slow_settings, "clicked", G_CALLBACK(on_slow_settings), &f);
    on_filter_kind(nullptr, &f);
    gtk_box_pack_start(GTK_BOX(page), frame("Filter options", options), FALSE, FALSE, 0);
    GtkWidget *notify = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *beep = check(notify, "_Beep when keys pressed or accepted", a->filter_beep);
    GtkWidget *indicator = check(notify, "_Show FilterKeys status on screen", a->filter_indicator);
    gtk_box_pack_start(GTK_BOX(page), frame("Notification", notify), FALSE, FALSE, 0);
    GtkWidget *d = settings_dialog(parent, "Settings for FilterKeys", page);
    bool ok = run_ok(d);
    if (ok) {
        copy.filter_shortcut = checked(shortcut);
        copy.slow_keys = !checked(f.bounce);
        copy.filter_beep = checked(beep);
        copy.filter_indicator = checked(indicator);
        *a = copy;
    }
    gtk_widget_destroy(d);
    return ok;
}

static bool toggle_settings(GtkWindow *parent, xd_access_settings *a) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *shortcut = shortcut_frame(
        page, "ToggleKeys is:\nHold down NUM LOCK for five seconds.", a->toggle_shortcut);
    GtkWidget *d = settings_dialog(parent, "Settings for ToggleKeys", page);
    bool ok = run_ok(d);
    if (ok)
        a->toggle_shortcut = checked(shortcut);
    gtk_widget_destroy(d);
    return ok;
}

static GtkWidget *slider_row(GtkWidget *box, const char *label, const char *left,
                             GtkWidget *scale, const char *right) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), scale);
    gtk_label_set_width_chars(GTK_LABEL(l), 13);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_size_request(scale, 160, -1);
    gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(left), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(right), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}

static bool mouse_keys_settings(GtkWindow *parent, xd_access_settings *a) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *shortcut = shortcut_frame(
        page, "MouseKeys is:\nLeft ALT + left SHIFT + NUM LOCK", a->mouse_keys_shortcut);
    GtkWidget *speed = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *top = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, XD_MOUSE_KEYS_SPEED_MIN,
                                              XD_MOUSE_KEYS_SPEED_MAX, 10);
    gtk_range_set_value(GTK_RANGE(top), a->mouse_keys_speed);
    slider_row(speed, "_Top speed:", "Low", top, "High");
    /* Fast on the right: the shortest time to top speed. */
    GtkWidget *accel = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                XD_MOUSE_KEYS_ACCEL_MIN, XD_MOUSE_KEYS_ACCEL_MAX,
                                                500);
    gtk_range_set_inverted(GTK_RANGE(accel), TRUE);
    gtk_range_set_value(GTK_RANGE(accel), a->mouse_keys_accel_ms);
    slider_row(speed, "_Acceleration:", "Slow", accel, "Fast");
    GtkWidget *mods = check(speed, "_Hold down Ctrl to speed up and Shift to slow down",
                            a->mouse_keys_modifiers);
    gtk_box_pack_start(GTK_BOX(page), frame("Pointer speed", speed), FALSE, FALSE, 0);
    GtkWidget *num = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(num), gtk_label_new("Use MouseKeys when NumLock is:"), FALSE,
                       FALSE, 0);
    GtkWidget *on = gtk_radio_button_new_with_mnemonic(nullptr, "O_n");
    GtkWidget *off = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(on), "O_ff");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->mouse_keys_numlock ? on : off), TRUE);
    gtk_box_pack_start(GTK_BOX(num), on, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(num), off, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), num, FALSE, FALSE, 0);
    GtkWidget *indicator = check(page, "_Show MouseKey status on screen", a->mouse_keys_indicator);
    GtkWidget *d = settings_dialog(parent, "Settings for MouseKeys", page);
    bool ok = run_ok(d);
    if (ok) {
        a->mouse_keys_shortcut = checked(shortcut);
        a->mouse_keys_speed = range_int(GTK_RANGE(top));
        a->mouse_keys_accel_ms = range_int(GTK_RANGE(accel));
        a->mouse_keys_modifiers = checked(mods);
        a->mouse_keys_numlock = checked(on);
        a->mouse_keys_indicator = checked(indicator);
    }
    gtk_widget_destroy(d);
    return ok;
}

/* ---- Keyboard ------------------------------------------------------------------------ */

static void on_keyboard_option([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    xd_access_settings *a = &p->now.access;
    a->sticky_keys = checked(p->sticky);
    a->filter_keys = checked(p->filter);
    a->toggle_keys = checked(p->toggle);
    changed(p);
}

typedef bool (*settings_fn)(GtkWindow *parent, xd_access_settings *a);

static void on_settings_button(GtkWidget *b, gpointer data) {
    props *p = g_object_get_data(G_OBJECT(b), "props");
    settings_fn fn = (settings_fn)(uintptr_t)data;
    if (fn(GTK_WINDOW(p->window), &p->now.access))
        changed(p);
}

/* A feature's frame: what it's for, Use ..., and Settings. */
static GtkWidget *feature_frame(props *p, GtkWidget *page, const char *title, const char *text,
                                const char *use, bool on, settings_fn settings) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), wrapped(text), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *c = gtk_check_button_new_with_mnemonic(use);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), on);
    gtk_box_pack_start(GTK_BOX(row), c, TRUE, TRUE, 0);
    GtkWidget *b = gtk_button_new_with_mnemonic("_Settings");
    gtk_widget_set_size_request(b, 75, -1);
    g_object_set_data(G_OBJECT(b), "props", p);
    g_signal_connect(b, "clicked", G_CALLBACK(on_settings_button), (gpointer)(uintptr_t)settings);
    gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame(title, box), FALSE, FALSE, 0);
    return c;
}

static GtkWidget *keyboard_page(props *p) {
    const xd_access_settings *a = &p->now.access;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    /* Each frame's Settings button has its own mnemonic in XP; GTK picks
     * the next one on a repeated key. */
    p->sticky = feature_frame(p, page, "StickyKeys",
                              "Use StickyKeys if you want to use SHIFT, CTRL, ALT, or Windows "
                              "logo key by pressing one key at a time.",
                              "Use _StickyKeys", a->sticky_keys, sticky_settings);
    p->filter = feature_frame(p, page, "FilterKeys",
                              "Use FilterKeys if you want Windows to ignore brief or repeated "
                              "keystrokes, or slow the repeat rate.",
                              "Use _FilterKeys", a->filter_keys, filter_settings);
    p->toggle = feature_frame(p, page, "ToggleKeys",
                              "Use ToggleKeys if you want to hear tones when pressing CAPS "
                              "LOCK, NUM LOCK, and SCROLL LOCK.",
                              "Use T_oggleKeys", a->toggle_keys, toggle_settings);
    g_signal_connect(p->sticky, "toggled", G_CALLBACK(on_keyboard_option), p);
    g_signal_connect(p->filter, "toggled", G_CALLBACK(on_keyboard_option), p);
    g_signal_connect(p->toggle, "toggled", G_CALLBACK(on_keyboard_option), p);
    return page;
}

/* ---- Sound --------------------------------------------------------------------------- */

static void on_sound_option([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    xd_access_settings *a = &p->now.access;
    a->sound_sentry = checked(p->sentry);
    a->show_sounds = checked(p->show_sounds);
    /* The list is in xd_sound_sentry's order. */
    int effect = gtk_combo_box_get_active(GTK_COMBO_BOX(p->sentry_effect));
    if (effect >= 0 && effect < XD_SENTRY_COUNT)
        a->sound_sentry_effect = (xd_sound_sentry)effect;
    gtk_widget_set_sensitive(p->sentry_effect, a->sound_sentry);
    changed(p);
}

static GtkWidget *sound_page(props *p) {
    const xd_access_settings *a = &p->now.access;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *sentry = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(sentry),
                       wrapped("Use SoundSentry if you want Windows to generate visual warnings "
                               "when your system makes a sound."),
                       FALSE, FALSE, 0);
    p->sentry = check(sentry, "Use _SoundSentry", a->sound_sentry);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Choose the visual warning:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(sentry), label, FALSE, FALSE, 0);
    p->sentry_effect = gtk_combo_box_text_new();
    static const char *const effects[XD_SENTRY_COUNT] = {
        [XD_SENTRY_NONE] = "[None]",
        [XD_SENTRY_CAPTION] = "Flash active caption bar",
        [XD_SENTRY_WINDOW] = "Flash active window",
        [XD_SENTRY_DESKTOP] = "Flash desktop",
    };
    char id[8];
    for (int i = 0; i < XD_SENTRY_COUNT; i++) {
        g_snprintf(id, sizeof(id), "%d", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->sentry_effect), id, effects[i]);
    }
    g_snprintf(id, sizeof(id), "%d", (int)a->sound_sentry_effect);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->sentry_effect), id);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->sentry_effect);
    gtk_widget_set_sensitive(p->sentry_effect, a->sound_sentry);
    gtk_box_pack_start(GTK_BOX(sentry), p->sentry_effect, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("SoundSentry", sentry), FALSE, FALSE, 0);

    GtkWidget *show = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(show),
                       wrapped("Use ShowSounds to tell your programs to display captions for the "
                               "speech and sounds they make."),
                       FALSE, FALSE, 0);
    p->show_sounds = check(show, "Use Show_Sounds", a->show_sounds);
    gtk_box_pack_start(GTK_BOX(page), frame("ShowSounds", show), FALSE, FALSE, 0);

    g_signal_connect(p->sentry, "toggled", G_CALLBACK(on_sound_option), p);
    g_signal_connect(p->sentry_effect, "changed", G_CALLBACK(on_sound_option), p);
    g_signal_connect(p->show_sounds, "toggled", G_CALLBACK(on_sound_option), p);
    return page;
}

/* ---- Display ------------------------------------------------------------------------- */

static void on_contrast([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    gtk_widget_set_sensitive(p->contrast_scheme, checked(p->contrast));
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
    p->contrast = check(contrast, "Use _High Contrast", p->now.access.high_contrast);
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

/* ---- Mouse and General ----------------------------------------------------------- */

static void on_mouse_keys([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    p->now.access.mouse_keys = checked(p->mouse_keys);
    changed(p);
}

static GtkWidget *mouse_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    p->mouse_keys = feature_frame(p, page, "MouseKeys",
                                  "Use MouseKeys if you want to control the pointer with the "
                                  "numeric keypad on your keyboard.",
                                  "Use _MouseKeys", p->now.access.mouse_keys, mouse_keys_settings);
    g_signal_connect(p->mouse_keys, "toggled", G_CALLBACK(on_mouse_keys), p);
    return page;
}

static void on_general([[maybe_unused]] GtkWidget *w, gpointer data) {
    props *p = data;
    p->now.access.auto_reset = checked(p->reset);
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->reset_minutes));
    if (id != nullptr)
        p->now.access.auto_reset_minutes = atoi(id);
    gtk_widget_set_sensitive(p->reset_minutes, p->now.access.auto_reset);
    p->now.access.warn = checked(p->warn);
    p->now.access.sound_on_off = checked(p->sound_on_off);
    changed(p);
}

static GtkWidget *general_page(props *p) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    const xd_access_settings *a = &p->now.access;
    GtkWidget *reset = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    p->reset = check(reset, "_Turn off accessibility features after idle for:", a->auto_reset);
    p->reset_minutes = gtk_combo_box_text_new();
    char id[8], text[32];
    for (int m = XD_AUTO_RESET_MIN; m <= XD_AUTO_RESET_MAX; m += 5) {
        g_snprintf(id, sizeof(id), "%d", m);
        g_snprintf(text, sizeof(text), "%d minutes", m);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->reset_minutes), id, text);
    }
    g_snprintf(id, sizeof(id), "%d", a->auto_reset_minutes);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->reset_minutes), id)) {
        g_snprintf(text, sizeof(text), "%d minutes", a->auto_reset_minutes);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->reset_minutes), id, text);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->reset_minutes), id);
    }
    gtk_widget_set_margin_start(p->reset_minutes, 20);
    gtk_widget_set_halign(p->reset_minutes, GTK_ALIGN_START);
    gtk_widget_set_sensitive(p->reset_minutes, a->auto_reset);
    gtk_box_pack_start(GTK_BOX(reset), p->reset_minutes, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Automatic reset", reset), FALSE, FALSE, 0);
    GtkWidget *notify = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    p->warn = check(notify, "Give _warning message when turning a feature on", p->now.access.warn);
    p->sound_on_off = check(notify, "Make a sound when turning a feature on or o_ff",
                            p->now.access.sound_on_off);
    gtk_box_pack_start(GTK_BOX(page), frame("Notification", notify), FALSE, FALSE, 0);
    g_signal_connect(p->reset, "toggled", G_CALLBACK(on_general), p);
    g_signal_connect(p->reset_minutes, "changed", G_CALLBACK(on_general), p);
    g_signal_connect(p->warn, "toggled", G_CALLBACK(on_general), p);
    g_signal_connect(p->sound_on_off, "toggled", G_CALLBACK(on_general), p);
    return page;
}

/* ---- applying ----------------------------------------------------------------------------- */

/* Our [Accessibility] over `s`'s, but its High Contrast (set apart). */
static void merge_access(xd_settings *s, const xd_access_settings *now) {
    xd_access_settings keep = s->access;
    s->access = *now;
    s->access.high_contrast = keep.high_contrast;
    s->access.saved_style = keep.saved_style;
    memcpy(s->access.saved_scheme, keep.saved_scheme, sizeof(keep.saved_scheme));
}

static bool apply(props *p) {
    /* The file as it is now, with what was changed here: the Mouse and
     * Keyboard applets write some of the same keys. */
    xd_settings s;
    xd_settings_load(&s);
    merge_access(&s, &p->now.access);
    bool contrast = checked(p->contrast);
    const char *scheme = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->contrast_scheme));
    if (contrast && scheme == nullptr) {
        xd_message_box(GTK_WINDOW(p->window), "Accessibility Options", XD_MB_ERROR, XD_MB_OK,
                       FALSE, "No High Contrast color scheme is installed.");
        return false;
    }
    if (contrast != s.access.high_contrast || (contrast && strcmp(scheme, s.scheme) != 0))
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
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), shortcuts[i].title);
    gtk_window_set_icon_name(GTK_WINDOW(d), "preferences-desktop-accessibility");
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", CONFIRM_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", CONFIRM_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(d), "_Settings", CONFIRM_SETTINGS);
    gtk_dialog_set_default_response(GTK_DIALOG(d), CONFIRM_OK);
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
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_hide(d);
    xd_settings s;
    xd_settings_load(&s);
    bool save = false;
    if (r == CONFIRM_CANCEL || r == GTK_RESPONSE_DELETE_EVENT) {
        *(bool *)((char *)&s.access + shortcuts[i].flag) = false;
        save = true;
    } else if (r == CONFIRM_SETTINGS) {
        save = shortcuts[i].settings(GTK_WINDOW(d), &s.access);
    }
    gtk_widget_destroy(d);
    if (save && !xd_settings_save(&s)) {
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
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), sound_page(&p),
                             gtk_label_new_with_mnemonic("_Sound"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), display_page(&p),
                             gtk_label_new_with_mnemonic("_Display"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), mouse_page(&p),
                             gtk_label_new_with_mnemonic("_Mouse"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(&p),
                             gtk_label_new_with_mnemonic("_General"));
    p.updating = false;
    gtk_widget_set_sensitive(p.apply, FALSE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(p.window, GTK_NOTEBOOK(tabs));

    g_signal_connect(p.window, "response", G_CALLBACK(on_response), &p);
    g_signal_connect(p.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(p.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs),
                                  XD_CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(tabs)) - 1));
    gtk_main();
    if (p.caret_timer != 0)
        g_source_remove(p.caret_timer);
    return EXIT_SUCCESS;
}
