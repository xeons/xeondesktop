/* SPDX-License-Identifier: MIT
 * xd-effects: Desktop Effects, a Control Panel applet for the compositor's
 * Compiz-style effects, laid out as CompizConfig Settings Manager was:
 *
 *   side bar    a Filter box, the categories, Preferences and Close
 *   main area   every effect under its category's heading, each with its
 *               icon, its name and a check box that turns it on at once
 *   its page    an effect's options, on tabs where it has many, each with
 *               a button putting it back to its default; Back returns
 *
 * Everything comes from the table in src/settings/effects.h: a new effect
 * needs no code here. Changes are saved to effects.ini as they're made
 * and the compositor applies them at once, as CCSM's were. Effects drawn
 * with OpenGL show greyed out, with the reason, when the compositor can't
 * draw them (its support file says so).
 *
 *   xd-effects [--effect ID]   opens that effect's page (an id from the
 *                              table: WobblyWindows)
 */
#include <stdlib.h>
#include <string.h>

#include "../core/alloc.h"
#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "../settings/effects.h"

enum { CONFLICTS_MAX = 16, ALL_CATEGORIES = -1 };

typedef struct panel panel;

/* One option on an effect's page. */
typedef struct option_row {
    panel *app;
    size_t effect, option;
    GtkWidget *editor; /* the check box, spin button, colour button... */
    GtkAdjustment *adjustment;
    GtkWidget *reset;
    /* A choice that needs another effect: why it's another meanwhile. */
    GtkWidget *note;
} option_row;

/* One effect in the grid. */
typedef struct effect_tile {
    panel *app;
    size_t effect;
    GtkWidget *child; /* its GtkFlowBoxChild */
    GtkWidget *check;
} effect_tile;

struct panel {
    xd_effects_settings *settings;
    xd_effects_support support;
    bool have_support; /* the compositor wrote its support file */
    GtkWidget *window;
    GtkWidget *sidebar_stack, *main_stack;
    GtkWidget *filter, *categories;
    int category; /* shown, or ALL_CATEGORIES */
    GtkWidget *sections[XD_EFFECT_CATEGORY_COUNT];
    effect_tile *tiles;
    size_t tile_count;
    /* The effect's page (or Preferences) in front. */
    bool page_open;
    size_t page_effect;
    GtkWidget *page_icon, *page_name, *page_description, *page_use, *page_reason;
    GtkWidget *page_holder; /* the page's notebook goes in here */
    GtkWidget *page_notebook;
    option_row *rows;
    size_t row_count;
    GtkWidget *warn_check;
    bool updating; /* widgets being set from the settings: no handlers */
};

static void panel_show_page(panel *app, size_t effect);
static void panel_refresh_tiles(panel *app);

/* ---- small helpers -------------------------------------------------------------- */

static const char *panel_get_category_icon(xd_effect_category category) {
    switch (category) {
    case XD_EFFECT_CATEGORY_EFFECTS:
        return "preferences-desktop-effects";
    case XD_EFFECT_CATEGORY_WINDOW_MANAGEMENT:
        return "preferences-system-windows";
    case XD_EFFECT_CATEGORY_DESKTOP:
        return "preferences-desktop";
    case XD_EFFECT_CATEGORY_ACCESSIBILITY:
        return "preferences-desktop-accessibility";
    case XD_EFFECT_CATEGORY_UTILITY:
    case XD_EFFECT_CATEGORY_COUNT:
        break;
    }
    return "applications-utilities";
}

static bool panel_is_available(const panel *app, size_t effect) {
    return !xd_effects_get_info(app->settings, effect)->needs_opengl || app->support.opengl;
}

static const char *panel_get_unavailable_reason(const panel *app) {
    if (app->have_support && app->support.reason[0] != '\0')
        return app->support.reason;
    return "XeonDesktop's compositor isn't running here, so effects drawn with OpenGL can't "
           "be turned on.";
}

static void panel_save(panel *app) {
    if (!xd_effects_save(app->settings))
        xd_message_box(GTK_WINDOW(app->window), "Desktop Effects", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "Desktop Effects could not save your settings.");
}

static GtkWidget *panel_new_label(const char *text, float xalign) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), xalign);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    return label;
}

/* A group box, as XP's property sheets have them. */
static GtkWidget *panel_new_group(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    /* An entry (the Filter box) isn't a container: it's inset by its margin. */
    if (GTK_IS_CONTAINER(child))
        gtk_container_set_border_width(GTK_CONTAINER(child), 6);
    else
        g_object_set(child, "margin", 6, nullptr);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

/* Gives `text` an access key not in `used` (a letter starting a word if
 * it can), as Windows dialogs have one on every label. */
static char *panel_add_access_key(const char *text, char used[64]) {
    size_t length = strlen(text);
    size_t pick = length;
    for (int pass = 0; pass < 2 && pick == length; pass++) {
        for (size_t i = 0; i < length; i++) {
            char letter = g_ascii_tolower(text[i]);
            bool word_start = i == 0 || text[i - 1] == ' ';
            if (!g_ascii_isalnum(letter) || (pass == 0 && !word_start) ||
                strchr(used, letter) != nullptr)
                continue;
            pick = i;
            break;
        }
    }
    GString *out = g_string_new(nullptr);
    for (size_t i = 0; i < length; i++) {
        if (i == pick)
            g_string_append_c(out, '_');
        if (text[i] == '_')
            g_string_append_c(out, '_');
        g_string_append_c(out, text[i]);
    }
    if (pick < length && strlen(used) < 63) {
        size_t end = strlen(used);
        used[end] = g_ascii_tolower(text[pick]);
        used[end + 1] = '\0';
    }
    return g_string_free(out, false);
}

/* ---- turning effects on ------------------------------------------------------------ */

/* The clashes of `effect`'s bindings (only `option`'s, unless SIZE_MAX)
 * with the effects that are on, one line each; nullptr if none. */
static char *panel_describe_conflicts(panel *app, size_t effect, size_t option) {
    xd_effect_conflict conflicts[CONFLICTS_MAX];
    size_t found = xd_effects_find_conflicts(app->settings, effect, conflicts, CONFLICTS_MAX);
    GString *text = g_string_new(nullptr);
    const xd_effect_info *info = xd_effects_get_info(app->settings, effect);
    for (size_t i = 0; i < XD_MIN(found, (size_t)CONFLICTS_MAX); i++) {
        if (option != SIZE_MAX && conflicts[i].option != option)
            continue;
        const xd_effect_info *other = xd_effects_get_info(app->settings, conflicts[i].other_effect);
        const xd_effect_option *mine = &info->options[conflicts[i].option];
        const xd_effect_option *theirs = &other->options[conflicts[i].other_option];
        g_autofree char *value = xd_effects_describe_value(
            mine, xd_effects_get_value(app->settings, effect, conflicts[i].option));
        g_string_append_printf(text, "%s and %s both use %s (%s, and %s).\n", info->name,
                               other->name, value, mine->label, theirs->label);
    }
    if (text->len == 0) {
        g_string_free(text, true);
        return nullptr;
    }
    return g_string_free(text, false);
}

/* Turns the effect on or off, warning first of clashes as CCSM did;
 * false if the user would rather not. */
static bool panel_set_enabled(panel *app, size_t effect, bool on) {
    if (on && !panel_is_available(app, effect))
        return false;
    if (on && xd_effects_get_warn_conflicts(app->settings)) {
        g_autofree char *conflicts = panel_describe_conflicts(app, effect, SIZE_MAX);
        if (conflicts != nullptr) {
            const char *name = xd_effects_get_info(app->settings, effect)->name;
            g_autofree char *text =
                g_strdup_printf("%s\nDo you want to turn on %s anyway?", conflicts, name);
            if (xd_message_box(GTK_WINDOW(app->window), "Desktop Effects", XD_MB_WARNING,
                               XD_MB_YES_NO, TRUE, text) != GTK_RESPONSE_YES)
                return false;
        }
    }
    xd_effects_set_enabled(app->settings, effect, on);
    panel_save(app);
    return true;
}

/* The check boxes showing whether effects are on, to match the settings. */
static void panel_refresh_tiles(panel *app) {
    app->updating = true;
    for (size_t i = 0; i < app->tile_count; i++) {
        effect_tile *tile = &app->tiles[i];
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tile->check),
                                     xd_effects_is_enabled(app->settings, tile->effect));
    }
    if (app->page_open && app->page_use != nullptr)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->page_use),
                                     xd_effects_is_enabled(app->settings, app->page_effect));
    app->updating = false;
}

static void panel_on_tile_toggled(GtkToggleButton *check, gpointer data) {
    effect_tile *tile = data;
    panel *app = tile->app;
    if (app->updating)
        return;
    panel_set_enabled(app, tile->effect, gtk_toggle_button_get_active(check));
    panel_refresh_tiles(app);
}

static void panel_on_use_toggled(GtkToggleButton *check, gpointer data) {
    panel *app = data;
    if (app->updating || !app->page_open)
        return;
    panel_set_enabled(app, app->page_effect, gtk_toggle_button_get_active(check));
    panel_refresh_tiles(app);
}

/* ---- option editors ------------------------------------------------------------- */

static void panel_update_reset(option_row *row) {
    gtk_widget_set_sensitive(row->reset,
                             !xd_effects_is_default(row->app->settings, row->effect, row->option));
}

static const xd_effect_option *panel_get_option(const option_row *row) {
    return &xd_effects_get_info(row->app->settings, row->effect)->options[row->option];
}

/* Whether a choice can be used now (its effect is on and drawable). */
static bool panel_is_choice_available(const option_row *row, int choice) {
    panel *app = row->app;
    return xd_effects_is_choice_available(app->settings, row->effect, row->option, choice,
                                          app->support.opengl);
}

/* A choice that needs an effect which is off shows greyed out in the
 * list; chosen anyway (set before, or in the file), the note under it
 * says what's drawn meanwhile. */
static void panel_update_choice_note(option_row *row, int choice) {
    if (row->note == nullptr)
        return;
    panel *app = row->app;
    const xd_effect_option *option = panel_get_option(row);
    const char *needed = xd_effects_get_choice_requirement(option, choice);
    size_t other = 0;
    if (needed == nullptr || panel_is_choice_available(row, choice) ||
        !xd_effects_find(app->settings, needed, &other)) {
        gtk_widget_hide(row->note);
        return;
    }
    const char *fallback = "another effect";
    for (size_t i = 0; option->choice_fallback != nullptr && option->choice_names[i] != nullptr;
         i++) {
        if (strcmp(option->choice_names[i], option->choice_fallback) == 0)
            fallback = option->choice_labels[i];
    }
    const xd_effect_info *info = xd_effects_get_info(app->settings, other);
    g_autofree char *text =
        info->needs_opengl && !app->support.opengl
            ? g_strdup_printf("%s can't be drawn here, so %s is used instead.", info->name,
                              fallback)
            : g_strdup_printf("Turn on %s to use this effect. Until then, %s is used.",
                              info->name, fallback);
    gtk_label_set_text(GTK_LABEL(row->note), text);
    gtk_widget_show(row->note);
}

/* Greys the choices that can't be used now. */
static void panel_set_choice_sensitive([[maybe_unused]] GtkCellLayout *layout,
                                       GtkCellRenderer *cell, GtkTreeModel *model,
                                       GtkTreeIter *iter, gpointer data) {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    int choice = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    g_object_set(cell, "sensitive", panel_is_choice_available(data, choice), nullptr);
}

/* Shows the option's value in its editor. */
static void panel_show_value(option_row *row) {
    panel *app = row->app;
    const xd_effect_option *option = panel_get_option(row);
    const xd_effect_value *value = xd_effects_get_value(app->settings, row->effect, row->option);
    app->updating = true;
    switch (option->type) {
    case XD_EFFECT_OPTION_BOOL:
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->editor), value->boolean);
        break;
    case XD_EFFECT_OPTION_INT:
        gtk_adjustment_set_value(row->adjustment, value->integer);
        break;
    case XD_EFFECT_OPTION_DOUBLE:
        gtk_adjustment_set_value(row->adjustment, value->number);
        break;
    case XD_EFFECT_OPTION_COLOR: {
        GdkRGBA rgba = {
            .red = (double)(value->color >> 24 & 0xff) / 255.0,
            .green = (double)(value->color >> 16 & 0xff) / 255.0,
            .blue = (double)(value->color >> 8 & 0xff) / 255.0,
            .alpha = (double)(value->color & 0xff) / 255.0,
        };
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(row->editor), &rgba);
        break;
    }
    case XD_EFFECT_OPTION_CHOICE:
        gtk_combo_box_set_active(GTK_COMBO_BOX(row->editor), value->choice);
        panel_update_choice_note(row, value->choice);
        break;
    case XD_EFFECT_OPTION_KEY:
    case XD_EFFECT_OPTION_BUTTON:
    case XD_EFFECT_OPTION_EDGE: {
        g_autofree char *text = xd_effects_describe_value(option, value);
        gtk_button_set_label(GTK_BUTTON(row->editor), text);
        break;
    }
    case XD_EFFECT_OPTION_MATCH:
        gtk_entry_set_text(GTK_ENTRY(row->editor), value->text);
        break;
    }
    app->updating = false;
    panel_update_reset(row);
}

/* Stores a value the user chose; a binding that clashes with an effect
 * that's on is asked about first. */
static void panel_store_value(option_row *row, const xd_effect_value *value) {
    panel *app = row->app;
    const xd_effect_option *option = panel_get_option(row);
    char *previous = xd_effects_format_value(
        option, xd_effects_get_value(app->settings, row->effect, row->option));
    if (!xd_effects_set_value(app->settings, row->effect, row->option, value)) {
        g_free(previous);
        panel_show_value(row); /* clamped back to what it was */
        return;
    }
    bool binding = option->type == XD_EFFECT_OPTION_KEY ||
                   option->type == XD_EFFECT_OPTION_BUTTON || option->type == XD_EFFECT_OPTION_EDGE;
    if (binding && xd_effects_is_enabled(app->settings, row->effect) &&
        xd_effects_get_warn_conflicts(app->settings)) {
        g_autofree char *conflicts = panel_describe_conflicts(app, row->effect, row->option);
        if (conflicts != nullptr) {
            g_autofree char *text = g_strdup_printf("%s\nDo you want to use it anyway?", conflicts);
            if (xd_message_box(GTK_WINDOW(app->window), "Desktop Effects", XD_MB_WARNING,
                               XD_MB_YES_NO, TRUE, text) != GTK_RESPONSE_YES) {
                xd_effect_value old = {0};
                if (xd_effects_parse_value(option, previous, &old)) {
                    xd_effects_set_value(app->settings, row->effect, row->option, &old);
                    if (option->type == XD_EFFECT_OPTION_KEY ||
                        option->type == XD_EFFECT_OPTION_BUTTON)
                        g_free(old.text);
                }
            }
        }
    }
    g_free(previous);
    panel_save(app);
    panel_show_value(row);
}

static void panel_on_bool_toggled(GtkToggleButton *check, gpointer data) {
    option_row *row = data;
    if (row->app->updating)
        return;
    xd_effect_value value = {.boolean = gtk_toggle_button_get_active(check)};
    panel_store_value(row, &value);
}

static void panel_on_number_changed(GtkAdjustment *adjustment, gpointer data) {
    option_row *row = data;
    if (row->app->updating)
        return;
    double number = gtk_adjustment_get_value(adjustment);
    xd_effect_value value = {0};
    if (panel_get_option(row)->type == XD_EFFECT_OPTION_INT)
        value.integer = (int)(number >= 0 ? number + 0.5 : number - 0.5);
    else
        value.number = number;
    panel_store_value(row, &value);
}

static void panel_on_color_set(GtkColorButton *button, gpointer data) {
    option_row *row = data;
    if (row->app->updating)
        return;
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    uint32_t channels[4] = {
        (uint32_t)(XD_CLAMP(rgba.red, 0.0, 1.0) * 255.0 + 0.5),
        (uint32_t)(XD_CLAMP(rgba.green, 0.0, 1.0) * 255.0 + 0.5),
        (uint32_t)(XD_CLAMP(rgba.blue, 0.0, 1.0) * 255.0 + 0.5),
        (uint32_t)(XD_CLAMP(rgba.alpha, 0.0, 1.0) * 255.0 + 0.5),
    };
    xd_effect_value value = {
        .color = channels[0] << 24 | channels[1] << 16 | channels[2] << 8 | channels[3]};
    panel_store_value(row, &value);
}

static void panel_on_choice_changed(GtkComboBox *combo, gpointer data) {
    option_row *row = data;
    if (row->app->updating || gtk_combo_box_get_active(combo) < 0)
        return;
    xd_effect_value value = {.choice = gtk_combo_box_get_active(combo)};
    panel_store_value(row, &value);
}

static void panel_apply_match(option_row *row) {
    if (row->app->updating)
        return;
    char *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(row->editor)));
    xd_effect_value value = {.text = text};
    panel_store_value(row, &value);
    g_free(text);
}

static void panel_on_match_activate([[maybe_unused]] GtkEntry *entry, gpointer data) {
    panel_apply_match(data);
}

static gboolean panel_on_match_focus_out([[maybe_unused]] GtkWidget *entry,
                                         [[maybe_unused]] GdkEvent *event, gpointer data) {
    panel_apply_match(data);
    return GDK_EVENT_PROPAGATE;
}

/* ---- binding dialogs -------------------------------------------------------------- */

/* What a binding dialog caught: a new value, "" for none. */
typedef struct binding_capture {
    xd_effect_option_type type;
    char *value;
    /* Not before a key has been let go, or a moment has passed: the
     * Space or Enter that pressed the button opening the dialog may
     * reach it still held. */
    bool armed;
    gint64 shown_at;
} binding_capture;

static constexpr gint64 BINDING_ARM_USEC = 400000;

static guint panel_get_modifiers(guint state) {
    return state & gtk_accelerator_get_default_mod_mask();
}

/* Modifiers in the binding syntax: "<Control><Super>". */
static void panel_append_modifiers(GString *out, guint state) {
    if (state & GDK_CONTROL_MASK)
        g_string_append(out, "<Control>");
    if (state & GDK_MOD1_MASK)
        g_string_append(out, "<Alt>");
    if (state & GDK_SHIFT_MASK)
        g_string_append(out, "<Shift>");
    if (state & (GDK_SUPER_MASK | GDK_MOD4_MASK))
        g_string_append(out, "<Super>");
}

static gboolean panel_on_binding_key(GtkWidget *dialog, GdkEventKey *event, gpointer data) {
    binding_capture *capture = data;
    if (event->is_modifier)
        return GDK_EVENT_STOP;
    if (!capture->armed && g_get_monotonic_time() - capture->shown_at < BINDING_ARM_USEC)
        return GDK_EVENT_STOP;
    capture->armed = true;
    guint modifiers = panel_get_modifiers(event->state);
    if (modifiers == 0 && event->keyval == GDK_KEY_Escape) {
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        return GDK_EVENT_STOP;
    }
    if (capture->type != XD_EFFECT_OPTION_KEY)
        return GDK_EVENT_PROPAGATE;
    const char *key = gdk_keyval_name(gdk_keyval_to_lower(event->keyval));
    if (modifiers == 0 && event->keyval == GDK_KEY_BackSpace) {
        capture->value = g_strdup("");
    } else if (key != nullptr) {
        GString *value = g_string_new(nullptr);
        panel_append_modifiers(value, event->state);
        g_string_append(value, key);
        capture->value = g_string_free(value, false);
    } else {
        return GDK_EVENT_STOP; /* a key without a name: wait for another */
    }
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    return GDK_EVENT_STOP;
}

static gboolean panel_on_binding_release([[maybe_unused]] GtkWidget *dialog,
                                         [[maybe_unused]] GdkEventKey *event, gpointer data) {
    binding_capture *capture = data;
    capture->armed = true;
    return GDK_EVENT_PROPAGATE;
}

static gboolean panel_on_binding_button([[maybe_unused]] GtkWidget *area, GdkEventButton *event,
                                        gpointer data) {
    binding_capture *capture = data;
    if (event->type != GDK_BUTTON_PRESS || event->button < 1 || event->button > 9)
        return GDK_EVENT_PROPAGATE;
    GString *value = g_string_new(nullptr);
    panel_append_modifiers(value, event->state);
    g_string_append_printf(value, "Button%u", event->button);
    capture->value = g_string_free(value, false);
    GtkWidget *dialog = gtk_widget_get_toplevel(area);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    return GDK_EVENT_STOP;
}

static void panel_edit_binding(option_row *row) {
    panel *app = row->app;
    const xd_effect_option *option = panel_get_option(row);
    bool keys = option->type == XD_EFFECT_OPTION_KEY;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        option->label, GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
        "_None", GTK_RESPONSE_REJECT, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 12);
    g_autofree char *prompt =
        keys ? g_strdup_printf("Press the keys to use for \"%s\", holding Ctrl, Alt, Shift or "
                               "the Windows key with them if you like.",
                               option->label)
             : g_strdup_printf("Click in the box with the mouse button to use for \"%s\", "
                               "holding Ctrl, Alt, Shift or the Windows key if you like.",
                               option->label);
    GtkWidget *label = panel_new_label(prompt, 0);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 44);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);
    binding_capture capture = {.type = option->type, .shown_at = g_get_monotonic_time()};
    g_signal_connect(dialog, "key-release-event", G_CALLBACK(panel_on_binding_release), &capture);
    if (keys) {
        g_signal_connect(dialog, "key-press-event", G_CALLBACK(panel_on_binding_key), &capture);
    } else {
        GtkWidget *area = gtk_event_box_new();
        gtk_widget_set_size_request(area, 240, 80);
        gtk_container_add(GTK_CONTAINER(area), gtk_label_new("Click here"));
        gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(area, "button-press-event", G_CALLBACK(panel_on_binding_button),
                         &capture);
        gtk_box_pack_start(GTK_BOX(content), panel_new_group(nullptr, area), TRUE, TRUE, 0);
        g_signal_connect(dialog, "key-press-event", G_CALLBACK(panel_on_binding_key), &capture);
    }
    gtk_widget_show_all(dialog);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response == GTK_RESPONSE_REJECT)
        capture.value = g_strdup("");
    if (capture.value == nullptr)
        return;
    xd_effect_value value = {.text = capture.value};
    panel_store_value(row, &value);
    g_free(capture.value);
}

/* The screen's edges and corners, as the 3 x 3 squares of a screen. */
static void panel_edit_edges(option_row *row) {
    panel *app = row->app;
    const xd_effect_option *option = panel_get_option(row);
    static const struct {
        const char *label;
        xd_screen_edge edge;
        int column, row;
    } places[XD_SCREEN_EDGES] = {
        {"Top left", XD_SCREEN_EDGE_TOP_LEFT, 0, 0},
        {"Top", XD_SCREEN_EDGE_TOP, 1, 0},
        {"Top right", XD_SCREEN_EDGE_TOP_RIGHT, 2, 0},
        {"Left", XD_SCREEN_EDGE_LEFT, 0, 1},
        {"Right", XD_SCREEN_EDGE_RIGHT, 2, 1},
        {"Bottom left", XD_SCREEN_EDGE_BOTTOM_LEFT, 0, 2},
        {"Bottom", XD_SCREEN_EDGE_BOTTOM, 1, 2},
        {"Bottom right", XD_SCREEN_EDGE_BOTTOM_RIGHT, 2, 2},
    };
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        option->label, GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
        "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 12);
    gtk_box_pack_start(GTK_BOX(content),
                       panel_new_label("Choose the edges and corners of the screen to use:", 0),
                       FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    uint32_t edges = xd_effects_get_value(app->settings, row->effect, row->option)->edges;
    GtkWidget *toggles[XD_SCREEN_EDGES];
    for (size_t i = 0; i < XD_SCREEN_EDGES; i++) {
        toggles[i] = gtk_toggle_button_new_with_label(places[i].label);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggles[i]), (edges & places[i].edge) != 0);
        gtk_widget_set_size_request(toggles[i], 96, 32);
        gtk_grid_attach(GTK_GRID(grid), toggles[i], places[i].column, places[i].row, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Screen"), 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    uint32_t chosen = 0;
    for (size_t i = 0; i < XD_SCREEN_EDGES; i++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggles[i])))
            chosen |= places[i].edge;
    }
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_OK)
        return;
    xd_effect_value value = {.edges = chosen};
    panel_store_value(row, &value);
}

static void panel_on_binding_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    option_row *row = data;
    if (panel_get_option(row)->type == XD_EFFECT_OPTION_EDGE)
        panel_edit_edges(row);
    else
        panel_edit_binding(row);
}

static void panel_on_reset_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    option_row *row = data;
    xd_effects_reset_value(row->app->settings, row->effect, row->option);
    panel_save(row->app);
    panel_show_value(row);
}

/* ---- an effect's page ----------------------------------------------------------------- */

/* The editor for one option, in row->editor (and what to give the label's
 * access key to). */
static GtkWidget *panel_new_editor(option_row *row, const char *mnemonic_label) {
    const xd_effect_option *option = panel_get_option(row);
    switch (option->type) {
    case XD_EFFECT_OPTION_BOOL:
        row->editor = gtk_check_button_new_with_mnemonic(mnemonic_label);
        g_signal_connect(row->editor, "toggled", G_CALLBACK(panel_on_bool_toggled), row);
        return row->editor;
    case XD_EFFECT_OPTION_INT:
    case XD_EFFECT_OPTION_DOUBLE: {
        double step = option->step > 0 ? option->step : 1.0;
        row->adjustment = gtk_adjustment_new(option->min, option->min, option->max, step,
                                             step * 10.0, 0.0);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, row->adjustment);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_widget_set_hexpand(scale, TRUE);
        gtk_widget_set_size_request(scale, 160, -1);
        guint digits = option->type == XD_EFFECT_OPTION_DOUBLE ? (guint)XD_MAX(option->digits, 0)
                                                               : 0;
        row->editor = gtk_spin_button_new(row->adjustment, step, digits);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(row->editor), TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(row->editor), 6);
        gtk_box_pack_start(GTK_BOX(box), scale, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), row->editor, FALSE, FALSE, 0);
        g_signal_connect(row->adjustment, "value-changed", G_CALLBACK(panel_on_number_changed),
                         row);
        return box;
    }
    case XD_EFFECT_OPTION_COLOR:
        row->editor = gtk_color_button_new();
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(row->editor), TRUE);
        gtk_widget_set_halign(row->editor, GTK_ALIGN_START);
        g_signal_connect(row->editor, "color-set", G_CALLBACK(panel_on_color_set), row);
        return row->editor;
    case XD_EFFECT_OPTION_CHOICE:
        row->editor = gtk_combo_box_text_new();
        for (size_t i = 0; option->choice_labels != nullptr && option->choice_labels[i] != nullptr;
             i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(row->editor),
                                           option->choice_labels[i]);
        gtk_widget_set_halign(row->editor, GTK_ALIGN_START);
        g_signal_connect(row->editor, "changed", G_CALLBACK(panel_on_choice_changed), row);
        if (option->choice_requires != nullptr) {
            GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(row->editor));
            for (GList *cell = cells; cell != nullptr; cell = cell->next)
                gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(row->editor), cell->data,
                                                   panel_set_choice_sensitive, row, nullptr);
            g_list_free(cells);
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            row->note = panel_new_label("", 0);
            gtk_widget_set_no_show_all(row->note, TRUE);
            gtk_box_pack_start(GTK_BOX(box), row->editor, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), row->note, FALSE, FALSE, 0);
            return box;
        }
        return row->editor;
    case XD_EFFECT_OPTION_KEY:
    case XD_EFFECT_OPTION_BUTTON:
    case XD_EFFECT_OPTION_EDGE:
        row->editor = gtk_button_new_with_label("");
        gtk_widget_set_size_request(row->editor, 160, -1);
        gtk_widget_set_halign(row->editor, GTK_ALIGN_START);
        g_signal_connect(row->editor, "clicked", G_CALLBACK(panel_on_binding_clicked), row);
        return row->editor;
    case XD_EFFECT_OPTION_MATCH:
        row->editor = gtk_entry_new();
        gtk_widget_set_hexpand(row->editor, TRUE);
        g_signal_connect(row->editor, "activate", G_CALLBACK(panel_on_match_activate), row);
        g_signal_connect(row->editor, "focus-out-event", G_CALLBACK(panel_on_match_focus_out),
                         row);
        return row->editor;
    }
    return gtk_label_new("");
}

/* The options of one tab (`group`), in a grid: label, editor, reset. */
static GtkWidget *panel_build_tab(panel *app, size_t effect, const char *group) {
    const xd_effect_info *info = xd_effects_get_info(app->settings, effect);
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    /* Back and "Use this effect" have theirs. */
    char used[64] = "bu";
    int line = 0;
    for (size_t option = 0; option < info->option_count; option++) {
        const xd_effect_option *opt = &info->options[option];
        if (strcmp(xd_effects_get_option_group(opt), group) != 0)
            continue;
        option_row *row = &app->rows[app->row_count++];
        *row = (option_row){.app = app, .effect = effect, .option = option};
        g_autofree char *text = opt->type == XD_EFFECT_OPTION_BOOL
                                    ? g_strdup(opt->label)
                                    : g_strdup_printf("%s:", opt->label);
        g_autofree char *mnemonic = panel_add_access_key(text, used);
        GtkWidget *editor = panel_new_editor(row, mnemonic);
        if (opt->type == XD_EFFECT_OPTION_BOOL) {
            gtk_grid_attach(GTK_GRID(grid), editor, 0, line, 2, 1);
        } else {
            GtkWidget *label = gtk_label_new_with_mnemonic(mnemonic);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_mnemonic_widget(GTK_LABEL(label), row->editor);
            gtk_grid_attach(GTK_GRID(grid), label, 0, line, 1, 1);
            gtk_widget_set_hexpand(editor, TRUE);
            gtk_grid_attach(GTK_GRID(grid), editor, 1, line, 1, 1);
        }
        if (opt->description != nullptr)
            gtk_widget_set_tooltip_text(row->editor, opt->description);
        row->reset = gtk_button_new_from_icon_name("edit-undo", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(row->reset, "Reset to the default");
        gtk_widget_set_valign(row->reset, GTK_ALIGN_CENTER);
        g_signal_connect(row->reset, "clicked", G_CALLBACK(panel_on_reset_clicked), row);
        gtk_grid_attach(GTK_GRID(grid), row->reset, 2, line, 1, 1);
        panel_show_value(row);
        line++;
    }
    GtkWidget *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), grid);
    return scrolled;
}

/* The page's options go when it closes (their rows with them). */
static void panel_clear_page(panel *app) {
    if (app->page_notebook != nullptr)
        gtk_widget_destroy(app->page_notebook);
    app->page_notebook = nullptr;
    XD_FREE(&app->rows);
    app->row_count = 0;
}

static void panel_show_grid(panel *app) {
    panel_clear_page(app);
    app->page_open = false;
    gtk_stack_set_visible_child_name(GTK_STACK(app->sidebar_stack), "main");
    gtk_stack_set_visible_child_name(GTK_STACK(app->main_stack), "grid");
    gtk_widget_grab_focus(app->filter);
}

static void panel_show_page(panel *app, size_t effect) {
    panel_clear_page(app);
    const xd_effect_info *info = xd_effects_get_info(app->settings, effect);
    app->page_open = true;
    app->page_effect = effect;
    gtk_image_set_from_icon_name(GTK_IMAGE(app->page_icon), info->icon, GTK_ICON_SIZE_DIALOG);
    g_autofree char *name = g_markup_printf_escaped("<b>%s</b>", info->name);
    gtk_label_set_markup(GTK_LABEL(app->page_name), name);
    gtk_label_set_text(GTK_LABEL(app->page_description), info->description);
    bool available = panel_is_available(app, effect);
    gtk_widget_set_sensitive(app->page_use, available);
    /* General Options is always in use: nothing to turn on. */
    gtk_widget_set_visible(app->page_use, !info->always_on);
    gtk_label_set_text(GTK_LABEL(app->page_reason),
                       available ? "" : panel_get_unavailable_reason(app));
    gtk_widget_set_visible(app->page_reason, !available);

    /* A tab per group of options, in the order they come; none when there's
     * only one. */
    app->rows = XD_NEW_ARRAY(option_row, XD_MAX(info->option_count, 1));
    app->page_notebook = gtk_notebook_new();
    const char *groups[64];
    size_t group_count = 0;
    for (size_t option = 0; option < info->option_count; option++) {
        const char *group = xd_effects_get_option_group(&info->options[option]);
        bool seen = false;
        for (size_t i = 0; i < group_count; i++)
            seen = seen || strcmp(groups[i], group) == 0;
        if (!seen && group_count < XD_ARRAY_LEN(groups))
            groups[group_count++] = group;
    }
    for (size_t i = 0; i < group_count; i++) {
        GtkWidget *tab = panel_build_tab(app, effect, groups[i]);
        gtk_notebook_append_page(GTK_NOTEBOOK(app->page_notebook), tab,
                                 gtk_label_new(groups[i]));
    }
    if (group_count == 0)
        gtk_notebook_append_page(GTK_NOTEBOOK(app->page_notebook),
                                 panel_new_label("This effect has no options.", 0.5f),
                                 gtk_label_new("General"));
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(app->page_notebook), group_count > 1);
    gtk_box_pack_start(GTK_BOX(app->page_holder), app->page_notebook, TRUE, TRUE, 0);
    gtk_widget_show_all(app->page_notebook);
    panel_refresh_tiles(app);
    gtk_stack_set_visible_child_name(GTK_STACK(app->sidebar_stack), "page");
    gtk_stack_set_visible_child_name(GTK_STACK(app->main_stack), "page");
    gtk_widget_grab_focus(available && !info->always_on ? app->page_use : app->page_notebook);
}

static void panel_show_preferences(panel *app) {
    panel_clear_page(app);
    app->page_open = false;
    gtk_image_set_from_icon_name(GTK_IMAGE(app->page_icon), "preferences-desktop-effects",
                                 GTK_ICON_SIZE_DIALOG);
    gtk_label_set_markup(GTK_LABEL(app->page_name), "<b>Preferences</b>");
    gtk_label_set_text(GTK_LABEL(app->page_description), "How Desktop Effects itself works.");
    gtk_widget_set_visible(app->page_use, false);
    gtk_widget_set_visible(app->page_reason, false);
    app->updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->warn_check),
                                 xd_effects_get_warn_conflicts(app->settings));
    app->updating = false;
    gtk_stack_set_visible_child_name(GTK_STACK(app->sidebar_stack), "page");
    gtk_stack_set_visible_child_name(GTK_STACK(app->main_stack), "preferences");
    gtk_widget_grab_focus(app->warn_check);
}

/* ---- the grid --------------------------------------------------------------------- */

static bool panel_contains(const char *text, const char *needle) {
    if (text == nullptr)
        return false;
    g_autofree char *folded = g_utf8_casefold(text, -1);
    return strstr(folded, needle) != nullptr;
}

/* Whether the filter finds the effect: its name, description or options,
 * as CCSM's filter did. */
static bool panel_matches_filter(const panel *app, size_t effect, const char *needle) {
    if (needle[0] == '\0')
        return true;
    const xd_effect_info *info = xd_effects_get_info(app->settings, effect);
    if (panel_contains(info->name, needle) || panel_contains(info->description, needle))
        return true;
    for (size_t option = 0; option < info->option_count; option++) {
        if (panel_contains(info->options[option].label, needle))
            return true;
    }
    return false;
}

/* Shows the effects of the chosen category the filter finds, and the
 * headings of categories with any; how many are shown. */
static size_t panel_apply_filter(panel *app, size_t *only) {
    g_autofree char *needle =
        g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(app->filter)), -1);
    g_strstrip(needle);
    bool any[XD_EFFECT_CATEGORY_COUNT] = {0};
    size_t shown = 0;
    for (size_t i = 0; i < app->tile_count; i++) {
        effect_tile *tile = &app->tiles[i];
        xd_effect_category category = xd_effects_get_info(app->settings, tile->effect)->category;
        bool visible = (app->category == ALL_CATEGORIES || app->category == (int)category) &&
                       panel_matches_filter(app, tile->effect, needle);
        gtk_widget_set_visible(tile->child, visible);
        if (visible) {
            any[category] = true;
            if (only != nullptr)
                *only = tile->effect;
            shown++;
        }
    }
    for (size_t category = 0; category < XD_EFFECT_CATEGORY_COUNT; category++) {
        if (app->sections[category] != nullptr)
            gtk_widget_set_visible(app->sections[category], any[category]);
    }
    return shown;
}

static void panel_on_filter_changed([[maybe_unused]] GtkSearchEntry *entry, gpointer data) {
    panel_apply_filter(data, nullptr);
}

/* Enter in the filter opens the one effect it finds. */
static void panel_on_filter_activate([[maybe_unused]] GtkEntry *entry, gpointer data) {
    panel *app = data;
    size_t only = 0;
    if (panel_apply_filter(app, &only) == 1)
        panel_show_page(app, only);
}

static void panel_on_category_selected([[maybe_unused]] GtkListBox *list, GtkListBoxRow *row,
                                       gpointer data) {
    panel *app = data;
    app->category = row != nullptr ? gtk_list_box_row_get_index(row) - 1 : ALL_CATEGORIES;
    panel_apply_filter(app, nullptr);
}

static void panel_on_tile_clicked(GtkButton *button, gpointer data) {
    panel *app = data;
    size_t effect = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(button), "effect"));
    panel_show_page(app, effect);
}

static GtkWidget *panel_build_tile(panel *app, effect_tile *tile) {
    const xd_effect_info *info = xd_effects_get_info(app->settings, tile->effect);
    bool available = panel_is_available(app, tile->effect);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    tile->check = gtk_check_button_new();
    gtk_widget_set_valign(tile->check, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(tile->check, available);
    g_autofree char *check_name = g_strdup_printf("Use %s", info->name);
    atk_object_set_name(gtk_widget_get_accessible(tile->check), check_name);
    g_signal_connect(tile->check, "toggled", G_CALLBACK(panel_on_tile_toggled), tile);
    /* Its place kept, so the names line up, but nothing to tick. */
    gtk_widget_set_opacity(tile->check, info->always_on ? 0.0 : 1.0);
    gtk_widget_set_sensitive(tile->check, available && !info->always_on);
    gtk_widget_set_can_focus(tile->check, !info->always_on);
    GtkWidget *button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    g_object_set_data(G_OBJECT(button), "effect", GSIZE_TO_POINTER(tile->effect));
    g_signal_connect(button, "clicked", G_CALLBACK(panel_on_tile_clicked), app);
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(inner), gtk_image_new_from_icon_name(info->icon, GTK_ICON_SIZE_DND),
                       FALSE, FALSE, 0);
    GtkWidget *name = panel_new_label(info->name, 0);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 18);
    gtk_widget_set_sensitive(name, available);
    gtk_box_pack_start(GTK_BOX(inner), name, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(button), inner);
    gtk_box_pack_start(GTK_BOX(box), tile->check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), button, TRUE, TRUE, 0);
    g_autofree char *tooltip =
        available ? g_strdup(info->description)
                  : g_strdup_printf("%s\n\n%s", info->description,
                                    panel_get_unavailable_reason(app));
    gtk_widget_set_tooltip_text(box, tooltip);
    gtk_widget_set_size_request(box, 200, -1);
    return box;
}

/* The effects' indices, by name (a few dozen at most). */
static size_t *panel_sort_by_name(const xd_effects_settings *settings, size_t count) {
    size_t *order = XD_NEW_ARRAY(size_t, XD_MAX(count, 1));
    for (size_t i = 0; i < count; i++) {
        size_t j = i;
        const char *name = xd_effects_get_info(settings, i)->name;
        while (j > 0 &&
               g_utf8_collate(xd_effects_get_info(settings, order[j - 1])->name, name) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    return order;
}

static GtkWidget *panel_build_grid(panel *app) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    size_t count = xd_effects_get_count(app->settings);
    bool any_opengl = false;
    for (size_t effect = 0; effect < count; effect++)
        any_opengl = any_opengl || xd_effects_get_info(app->settings, effect)->needs_opengl;
    if (any_opengl && !app->support.opengl) {
        GtkWidget *note = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(note),
                           gtk_image_new_from_icon_name("dialog-information", GTK_ICON_SIZE_DND),
                           FALSE, FALSE, 0);
        GtkWidget *label = panel_new_label(panel_get_unavailable_reason(app), 0);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 70);
        gtk_box_pack_start(GTK_BOX(note), label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
    }
    size_t *order = panel_sort_by_name(app->settings, count);
    app->tiles = XD_NEW_ARRAY(effect_tile, XD_MAX(count, 1));
    for (size_t category = 0; category < XD_EFFECT_CATEGORY_COUNT; category++) {
        GtkWidget *flow = nullptr;
        for (size_t i = 0; i < count; i++) {
            size_t effect = order[i];
            if (xd_effects_get_info(app->settings, effect)->category != category)
                continue;
            if (flow == nullptr) {
                GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
                GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                gtk_box_pack_start(
                    GTK_BOX(heading),
                    gtk_image_new_from_icon_name(
                        panel_get_category_icon((xd_effect_category)category),
                        GTK_ICON_SIZE_LARGE_TOOLBAR),
                    FALSE, FALSE, 0);
                g_autofree char *markup = g_markup_printf_escaped(
                    "<b>%s</b>", xd_effects_get_category_name((xd_effect_category)category));
                GtkWidget *title = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(title), markup);
                gtk_box_pack_start(GTK_BOX(heading), title, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(section), heading, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(section),
                                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                                   0);
                flow = gtk_flow_box_new();
                gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
                gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
                gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
                gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 6);
                gtk_box_pack_start(GTK_BOX(section), flow, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(box), section, FALSE, FALSE, 0);
                app->sections[category] = section;
            }
            effect_tile *tile = &app->tiles[app->tile_count++];
            *tile = (effect_tile){.app = app, .effect = effect};
            gtk_flow_box_insert(GTK_FLOW_BOX(flow), panel_build_tile(app, tile), -1);
            tile->child = gtk_widget_get_parent(gtk_widget_get_parent(tile->check));
        }
    }
    xd_free(order);
    GtkWidget *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), box);
    return scrolled;
}

/* ---- Preferences ------------------------------------------------------------------- */

static void panel_on_warn_toggled(GtkToggleButton *check, gpointer data) {
    panel *app = data;
    if (app->updating)
        return;
    xd_effects_set_warn_conflicts(app->settings, gtk_toggle_button_get_active(check));
    panel_save(app);
}

static void panel_on_reset_all_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    panel *app = data;
    if (xd_message_box(GTK_WINDOW(app->window), "Desktop Effects", XD_MB_QUESTION, XD_MB_YES_NO,
                       TRUE,
                       "Do you want to turn off every effect and set all their options back to "
                       "their defaults?") != GTK_RESPONSE_YES)
        return;
    xd_effects_reset_all(app->settings);
    panel_save(app);
    panel_refresh_tiles(app);
}

static GtkWidget *panel_build_preferences(panel *app) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *conflicts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    app->warn_check = gtk_check_button_new_with_mnemonic(
        "_Warn me when two effects use the same key, mouse button or screen edge");
    g_signal_connect(app->warn_check, "toggled", G_CALLBACK(panel_on_warn_toggled), app);
    gtk_box_pack_start(GTK_BOX(conflicts), app->warn_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), panel_new_group("Conflicts", conflicts), FALSE, FALSE, 0);
    GtkWidget *defaults = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(
        GTK_BOX(defaults),
        panel_new_label("Turn every effect off and set all their options back to their defaults.",
                        0),
        FALSE, FALSE, 0);
    GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset All...");
    gtk_widget_set_halign(reset, GTK_ALIGN_START);
    g_signal_connect(reset, "clicked", G_CALLBACK(panel_on_reset_all_clicked), app);
    gtk_box_pack_start(GTK_BOX(defaults), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), panel_new_group("Defaults", defaults), FALSE, FALSE, 0);
    return box;
}

/* ---- the side bar ----------------------------------------------------------------- */

static void panel_on_preferences_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    panel_show_preferences(data);
}

static void panel_on_back_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    panel_show_grid(data);
}

static void panel_on_close_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    panel *app = data;
    gtk_widget_destroy(app->window);
}

static GtkWidget *panel_build_main_sidebar(panel *app) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    app->filter = gtk_search_entry_new();
    g_signal_connect(app->filter, "search-changed", G_CALLBACK(panel_on_filter_changed), app);
    g_signal_connect(app->filter, "activate", G_CALLBACK(panel_on_filter_activate), app);
    gtk_box_pack_start(GTK_BOX(box), panel_new_group("Filter", app->filter), FALSE, FALSE, 0);

    app->categories = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->categories), GTK_SELECTION_BROWSE);
    for (int category = ALL_CATEGORIES; category < XD_EFFECT_CATEGORY_COUNT; category++) {
        const char *name = category == ALL_CATEGORIES
                               ? "All"
                               : xd_effects_get_category_name((xd_effect_category)category);
        GtkWidget *label = gtk_label_new(name);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_margin_start(label, 4);
        gtk_widget_set_margin_top(label, 2);
        gtk_widget_set_margin_bottom(label, 2);
        gtk_list_box_insert(GTK_LIST_BOX(app->categories), label, -1);
    }
    gtk_list_box_select_row(GTK_LIST_BOX(app->categories),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(app->categories), 0));
    g_signal_connect(app->categories, "row-selected", G_CALLBACK(panel_on_category_selected), app);
    gtk_box_pack_start(GTK_BOX(box), panel_new_group("Category", app->categories), FALSE, FALSE,
                       0);

    GtkWidget *close = gtk_button_new_with_mnemonic("_Close");
    g_signal_connect(close, "clicked", G_CALLBACK(panel_on_close_clicked), app);
    gtk_box_pack_end(GTK_BOX(box), close, FALSE, FALSE, 0);
    GtkWidget *preferences = gtk_button_new_with_mnemonic("_Preferences");
    gtk_button_set_image(GTK_BUTTON(preferences), gtk_image_new_from_icon_name(
                                                      "preferences-system", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(preferences), TRUE);
    g_signal_connect(preferences, "clicked", G_CALLBACK(panel_on_preferences_clicked), app);
    gtk_box_pack_end(GTK_BOX(box), preferences, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *panel_build_page_sidebar(panel *app) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    app->page_icon = gtk_image_new();
    gtk_box_pack_start(GTK_BOX(box), app->page_icon, FALSE, FALSE, 0);
    app->page_name = panel_new_label("", 0.5f);
    gtk_label_set_justify(GTK_LABEL(app->page_name), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), app->page_name, FALSE, FALSE, 0);
    app->page_description = panel_new_label("", 0);
    gtk_label_set_max_width_chars(GTK_LABEL(app->page_description), 24);
    gtk_box_pack_start(GTK_BOX(box), app->page_description, FALSE, FALSE, 0);
    app->page_use = gtk_check_button_new_with_mnemonic("_Use this effect");
    g_signal_connect(app->page_use, "toggled", G_CALLBACK(panel_on_use_toggled), app);
    gtk_box_pack_start(GTK_BOX(box), app->page_use, FALSE, FALSE, 0);
    app->page_reason = panel_new_label("", 0);
    gtk_label_set_max_width_chars(GTK_LABEL(app->page_reason), 24);
    gtk_widget_set_no_show_all(app->page_reason, TRUE);
    gtk_box_pack_start(GTK_BOX(box), app->page_reason, FALSE, FALSE, 0);
    GtkWidget *back = gtk_button_new_with_mnemonic("_Back");
    gtk_button_set_image(GTK_BUTTON(back),
                         gtk_image_new_from_icon_name("go-previous", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(back), TRUE);
    g_signal_connect(back, "clicked", G_CALLBACK(panel_on_back_clicked), app);
    gtk_box_pack_end(GTK_BOX(box), back, FALSE, FALSE, 0);
    return box;
}

/* Esc goes back from a page, and closes the window from the grid; on a
 * page, Ctrl+Tab and Ctrl+Shift+Tab switch its tabs, as in Windows'
 * property sheets. */
static gboolean panel_on_key(GtkWidget *window, GdkEventKey *event, gpointer data) {
    panel *app = data;
    guint modifiers = panel_get_modifiers(event->state);
    bool tab = event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_ISO_Left_Tab;
    if (tab && (modifiers & GDK_CONTROL_MASK) && app->page_notebook != nullptr) {
        GtkNotebook *notebook = GTK_NOTEBOOK(app->page_notebook);
        int count = gtk_notebook_get_n_pages(notebook);
        int step = (modifiers & GDK_SHIFT_MASK) ? count - 1 : 1;
        gtk_notebook_set_current_page(notebook,
                                      (gtk_notebook_get_current_page(notebook) + step) % count);
        return GDK_EVENT_STOP;
    }
    if (event->keyval != GDK_KEY_Escape || modifiers != 0)
        return GDK_EVENT_PROPAGATE;
    const char *shown = gtk_stack_get_visible_child_name(GTK_STACK(app->main_stack));
    if (shown != nullptr && strcmp(shown, "grid") != 0)
        panel_show_grid(app);
    else
        gtk_widget_destroy(window);
    return GDK_EVENT_STOP;
}

static void panel_build_window(panel *app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Desktop Effects");
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "preferences-desktop-effects");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 800, 560);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(panel_on_key), app);

    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    app->sidebar_stack = gtk_stack_new();
    gtk_widget_set_size_request(app->sidebar_stack, 210, -1);
    gtk_stack_add_named(GTK_STACK(app->sidebar_stack), panel_build_main_sidebar(app), "main");
    gtk_stack_add_named(GTK_STACK(app->sidebar_stack), panel_build_page_sidebar(app), "page");
    gtk_box_pack_start(GTK_BOX(layout), app->sidebar_stack, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE,
                       0);

    app->main_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(app->main_stack), panel_build_grid(app), "grid");
    app->page_holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(app->page_holder), 8);
    gtk_stack_add_named(GTK_STACK(app->main_stack), app->page_holder, "page");
    gtk_stack_add_named(GTK_STACK(app->main_stack), panel_build_preferences(app), "preferences");
    gtk_box_pack_start(GTK_BOX(layout), app->main_stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), layout);
}

static void panel_print_usage(FILE *out) {
    fprintf(out, "usage: xd-effects [--effect ID]\n");
}

int main(int argc, char **argv) {
    const char *open_effect = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--effect") == 0 && i + 1 < argc) {
            open_effect = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            panel_print_usage(stdout);
            return EXIT_SUCCESS;
        } else {
            panel_print_usage(stderr);
            return EXIT_FAILURE;
        }
    }
    xd_gtk_app_preinit("org.xeondesktop.DesktopEffects");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();

    static panel app;
    app.category = ALL_CATEGORIES;
    app.settings = xd_effects_create_settings();
    xd_effects_load(app.settings);
    app.have_support = xd_effects_read_support(g_getenv("WAYLAND_DISPLAY"), &app.support);
    panel_build_window(&app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(app.window);
    gtk_widget_hide(app.page_reason);
    panel_refresh_tiles(&app);
    size_t effect = 0;
    if (open_effect != nullptr && xd_effects_find(app.settings, open_effect, &effect))
        panel_show_page(&app, effect);
    else if (open_effect != nullptr)
        fprintf(stderr, "xd-effects: no effect %s\n", open_effect);
    else
        gtk_widget_grab_focus(app.filter);
    gtk_main();
    /* The window took the page's notebook with it; only its rows are left. */
    app.page_notebook = nullptr;
    panel_clear_page(&app);
    xd_free(app.tiles);
    xd_effects_destroy_settings(app.settings);
    return EXIT_SUCCESS;
}
