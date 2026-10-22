/* SPDX-License-Identifier: MIT
 * xd-snip: the Snipping Tool, as Windows 7's (with Windows 10's Delay).
 * A small toolbar window: New (its arrow chooses Free-form, Rectangular,
 * Window or Full-screen Snip), Delay, Cancel and Options. A snip opens in
 * the Mark-up window (editor.c).
 *
 *   xd-snip                 the toolbar window
 *   xd-snip --new[=KIND]    a snip at once (Win+Shift+S); KIND is
 *                           free-form, rectangle, window or full-screen
 *   xd-snip --copy-screen   the screen to the clipboard (Print Screen)
 *   xd-snip --copy-window   the active window to the clipboard
 *                           (Alt+Print Screen)
 *   xd-snip --copy-stdin    a PNG from standard input to the clipboard
 *   xd-snip --record        record the active window (Win+Alt+R)
 *   xd-snip --record=area   record an area chosen first (Win+Shift+R)
 *
 * Either --record, while a recording runs, stops it instead.
 * The --copy ones have no window: they stay to serve the clipboard until
 * another program takes it.
 */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../gtkapp/gtkapp.h"
#include "recording.h"
#include "snip.h"

/* How long the windows take to go before the screen is captured. */
static constexpr guint SETTLE_MS = 250;
static constexpr int DELAY_MAX = 5;

/* ---- options ---------------------------------------------------------------------------------- */

static char *options_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "snip.ini", nullptr);
}

static void load_options(snip_app *app) {
    xd_snip_options_init(&app->options);
    g_autofree char *path = options_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (g_key_file_load_from_file(file, path, G_KEY_FILE_KEEP_COMMENTS, nullptr))
        xd_snip_options_load_data(&app->options, file);
}

void snip_app_save_options(snip_app *app) {
    g_autofree char *path = options_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_load_from_file(file, path, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    xd_snip_options_save_data(&app->options, file);
    g_autofree char *folder = g_path_get_dirname(path);
    g_mkdir_with_parents(folder, 0700);
    GError *error = nullptr;
    if (!g_key_file_save_to_file(file, path, &error)) {
        g_warning("cannot save %s: %s", path, error->message);
        g_error_free(error);
    }
}

static GtkWidget *options_check(GtkWidget *box, const char *label, bool active) {
    GtkWidget *check = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), active);
    gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);
    return check;
}

static GtkWidget *options_group(GtkWidget *parent, const char *title) {
    GtkWidget *frame = gtk_frame_new(title);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(frame), box);
    gtk_box_pack_start(GTK_BOX(parent), frame, FALSE, FALSE, 0);
    return box;
}

static void show_instructions(snip_app *app) {
    gtk_widget_set_visible(app->instructions, !app->options.hide_instructions);
    if (app->options.hide_instructions)
        gtk_window_resize(GTK_WINDOW(app->window), 1, 1);
}

void snip_app_show_options(snip_app *app, GtkWindow *parent) {
    xd_snip_options *options = &app->options;
    GtkDialogFlags flags = (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Snipping Tool Options", parent, flags, "OK",
                                                    GTK_RESPONSE_OK, "Cancel",
                                                    GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), content, TRUE,
                       TRUE, 0);

    GtkWidget *application = options_group(content, "Application");
    GtkWidget *hide = options_check(application, "_Hide instruction text",
                                    options->hide_instructions);
    GtkWidget *copy = options_check(application, "Always _copy snips to the Clipboard",
                                    options->always_copy);
    GtkWidget *prompt = options_check(application, "_Prompt to save snips before exiting",
                                      options->prompt_to_save);
    GtkWidget *overlay = options_check(
        application, "Show screen _overlay when Snipping Tool is active", options->show_overlay);

    GtkWidget *selections = options_group(content, "Selections");
    GtkWidget *ink_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ink_label = gtk_label_new_with_mnemonic("_Ink color:");
    GdkRGBA ink = {options->ink[0], options->ink[1], options->ink[2], 1};
    GtkWidget *ink_button = gtk_color_button_new_with_rgba(&ink);
    gtk_label_set_mnemonic_widget(GTK_LABEL(ink_label), ink_button);
    gtk_box_pack_start(GTK_BOX(ink_row), ink_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ink_row), ink_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selections), ink_row, FALSE, FALSE, 0);
    GtkWidget *selection_ink = options_check(
        selections, "_Show selection ink after snips are captured", options->show_selection_ink);

    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        options->hide_instructions = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide));
        options->always_copy = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(copy));
        options->prompt_to_save = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(prompt));
        options->show_overlay = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(overlay));
        options->show_selection_ink =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(selection_ink));
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(ink_button), &ink);
        options->ink[0] = ink.red;
        options->ink[1] = ink.green;
        options->ink[2] = ink.blue;
        snip_app_save_options(app);
        show_instructions(app);
    }
    gtk_widget_destroy(dialog);
}

/* ---- snipping ------------------------------------------------------------------------------- */

static void finish_snipping(snip_app *app) {
    app->snipping = false;
    app->choosing_area = false;
    gtk_widget_set_sensitive(app->cancel, FALSE);
}

/* The toolbar window again, or (started for one snip or recording) quit. */
static void come_back(snip_app *app) {
    if (app->editor != nullptr)
        snip_editor_present(app->editor);
    else if (app->quit_on_cancel)
        gtk_main_quit();
    else
        gtk_window_present(GTK_WINDOW(app->window));
}

static void show_error(snip_app *app, const char *text) {
    GtkWindow *parent = gtk_widget_get_visible(app->window) ? GTK_WINDOW(app->window) : nullptr;
    xd_message_box(parent, SNIP_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
}

static gboolean capture(gpointer data) {
    snip_app *app = data;
    app->delay_source = 0;
    GError *error = nullptr;
    xd_snip_screen *screen = snip_capture_screen(gdk_display_get_default(), &error);
    if (screen == nullptr) {
        finish_snipping(app);
        gtk_widget_show(app->window);
        xd_message_box(GTK_WINDOW(app->window), SNIP_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
        if (app->quit_on_cancel)
            gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    snip_overlay_start(app, screen, app->choosing_area ? XD_SNIP_RECTANGLE : app->options.mode);
    return G_SOURCE_REMOVE;
}

/* The delay is over: our windows go, then the screen is captured. */
static gboolean delay_over(gpointer data) {
    snip_app *app = data;
    gtk_widget_hide(app->window);
    if (app->editor != nullptr)
        snip_editor_hide(app->editor);
    app->delay_source = g_timeout_add(SETTLE_MS, capture, app);
    return G_SOURCE_REMOVE;
}

static void sync_modes(snip_app *app) {
    app->syncing = true;
    GtkWidget *item = app->mode_items[app->options.mode];
    if (item != nullptr)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
    app->syncing = false;
}

void snip_app_new_snip(snip_app *app, xd_snip_mode mode) {
    if (app->snipping)
        return;
    if (mode != app->options.mode) {
        app->options.mode = mode;
        snip_app_save_options(app);
        sync_modes(app);
    }
    app->snipping = true;
    gtk_widget_set_sensitive(app->cancel, TRUE);
    if (app->options.delay_seconds > 0)
        app->delay_source =
            g_timeout_add_seconds((guint)app->options.delay_seconds, delay_over, app);
    else
        delay_over(app);
}

void snip_app_snip_taken(snip_app *app, cairo_surface_t *snip, double scale) {
    finish_snipping(app);
    if (snip == nullptr) {
        come_back(app);
        return;
    }
    if (app->editor == nullptr)
        app->editor = snip_editor_new(app);
    snip_editor_show(app->editor, snip, scale);
}

/* ---- recording ------------------------------------------------------------------------------ */

static gboolean free_recording(gpointer data) {
    snip_recording_free(data);
    return G_SOURCE_REMOVE;
}

static gboolean notice_gone(gpointer data) {
    snip_app *app = data;
    if (app->quit_on_cancel && app->recording == nullptr && !app->snipping)
        gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void recording_done(snip_recording *recording, const char *path, const char *error,
                           void *data) {
    snip_app *app = data;
    app->recording = nullptr;
    g_idle_add(free_recording, recording);
    if (error != nullptr) {
        show_error(app, error);
        come_back(app);
        return;
    }
    /* Started by a shortcut, it waits for the notification's buttons. */
    snip_recording_notify_saved(path, notice_gone, app);
    if (!app->quit_on_cancel)
        come_back(app);
}

static void start_recording(snip_app *app, xd_snip_box selection) {
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_monitor_at_point(
        display, (int)(selection.x + selection.width / 2), (int)(selection.y + selection.height / 2));
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    xd_snip_box area;
    GError *error = nullptr;
    if (!xd_record_fit(selection,
                       (xd_snip_box){geometry.x, geometry.y, geometry.width, geometry.height},
                       &area)) {
        come_back(app);
        return;
    }
    app->recording = snip_recording_start(app, monitor, area, app->options.record_sound,
                                          recording_done, app, &error);
    if (app->recording == nullptr) {
        show_error(app, error->message);
        g_error_free(error);
        come_back(app);
    }
}

void snip_app_area_chosen(snip_app *app, const xd_snip_box *area) {
    finish_snipping(app);
    if (area == nullptr)
        come_back(app);
    else
        start_recording(app, *area);
}

/* Record: an area chosen over the frozen screen (or a monitor clicked);
 * while recording, stops it. */
static void record_area(snip_app *app) {
    if (app->recording != nullptr) {
        snip_recording_stop(app->recording);
        return;
    }
    if (app->snipping)
        return;
    GError *error = nullptr;
    if (!snip_recording_possible(app->options.record_sound, &error)) {
        show_error(app, error->message);
        g_error_free(error);
        come_back(app);
        return;
    }
    app->snipping = true;
    app->choosing_area = true;
    gtk_widget_set_sensitive(app->cancel, TRUE);
    delay_over(app);
}

/* The Game Bar's Win+Alt+R: the active window, or the monitor at the
 * desktop's top left when none is. */
static void record_window(snip_app *app) {
    GError *error = nullptr;
    xd_snip_screen *screen = snip_capture_screen(gdk_display_get_default(), &error);
    if (screen == nullptr) {
        show_error(app, error->message);
        g_error_free(error);
        gtk_main_quit();
        return;
    }
    xd_snip_box box = {screen->origin_x, screen->origin_y, 1, 1};
    int active = xd_snip_window_active(screen->windows, screen->window_count);
    if (active >= 0) {
        const xd_snip_window *window = &screen->windows[active];
        box = (xd_snip_box){window->x, window->y, window->width, window->height};
    } else {
        GdkMonitor *monitor = gdk_display_get_monitor_at_point(gdk_display_get_default(),
                                                               (int)box.x, (int)box.y);
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);
        box = (xd_snip_box){geometry.x, geometry.y, geometry.width, geometry.height};
    }
    xd_snip_screen_free(screen);
    if (!snip_recording_possible(app->options.record_sound, &error)) {
        show_error(app, error->message);
        g_error_free(error);
        gtk_main_quit();
        return;
    }
    start_recording(app, box);
}

/* --record and --record=area, once the main loop runs (quitting needs
 * it). */
static gboolean record_from_command_line(gpointer data) {
    snip_app *app = data;
    if (app->choosing_area) {
        app->choosing_area = false;
        record_area(app);
    } else {
        record_window(app);
    }
    return G_SOURCE_REMOVE;
}

static void on_record(gpointer data) {
    record_area(data);
}

static void on_sound(GtkCheckMenuItem *item, gpointer data) {
    snip_app *app = data;
    if (!gtk_check_menu_item_get_active(item))
        return;
    gpointer sound = g_object_get_data(G_OBJECT(item), "sound");
    app->options.record_sound = (xd_record_sound)GPOINTER_TO_INT(sound);
    snip_app_save_options(app);
}

static void on_cancel(gpointer data) {
    snip_app *app = data;
    if (app->delay_source != 0)
        g_source_remove(app->delay_source);
    app->delay_source = 0;
    finish_snipping(app);
}

static void on_new(gpointer data) {
    snip_app *app = data;
    snip_app_new_snip(app, app->options.mode);
}

static void on_mode(GtkMenuItem *item, gpointer data) {
    snip_app *app = data;
    if (app->syncing)
        return;
    gpointer mode = g_object_get_data(G_OBJECT(item), "mode");
    snip_app_new_snip(app, (xd_snip_mode)GPOINTER_TO_INT(mode));
}

static void on_delay(GtkCheckMenuItem *item, gpointer data) {
    snip_app *app = data;
    if (!gtk_check_menu_item_get_active(item))
        return;
    gpointer delay = g_object_get_data(G_OBJECT(item), "delay");
    app->options.delay_seconds = GPOINTER_TO_INT(delay);
    snip_app_save_options(app);
}

static void on_options(gpointer data) {
    snip_app *app = data;
    snip_app_show_options(app, GTK_WINDOW(app->window));
}

/* ---- the toolbar window -------------------------------------------------------------------- */

static GtkWidget *build_mode_menu(snip_app *app) {
    static const char *const labels[] = {
        [XD_SNIP_FREE_FORM] = "_Free-form Snip",
        [XD_SNIP_RECTANGLE] = "_Rectangular Snip",
        [XD_SNIP_WINDOW] = "_Window Snip",
        [XD_SNIP_FULL_SCREEN] = "F_ull-screen Snip",
    };
    GtkWidget *menu = gtk_menu_new();
    GSList *group = nullptr;
    for (size_t i = 0; i < G_N_ELEMENTS(labels); i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(group, labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "mode", GINT_TO_POINTER((int)i));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        app->mode_items[i] = item;
    }
    sync_modes(app);
    /* Connected after, so marking the saved kind doesn't start a snip. */
    for (size_t i = 0; i < G_N_ELEMENTS(labels); i++)
        g_signal_connect(app->mode_items[i], "activate", G_CALLBACK(on_mode), app);
    gtk_widget_show_all(menu);
    return menu;
}

static GtkWidget *build_delay_menu(snip_app *app) {
    GtkWidget *menu = gtk_menu_new();
    GSList *group = nullptr;
    for (int seconds = 0; seconds <= DELAY_MAX; seconds++) {
        g_autofree char *label = seconds == 0   ? g_strdup("_No Delay")
                                 : seconds == 1 ? g_strdup("_1 second")
                                                : g_strdup_printf("_%d seconds", seconds);
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(group, label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "delay", GINT_TO_POINTER(seconds));
        if (seconds == app->options.delay_seconds)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        g_signal_connect(item, "toggled", G_CALLBACK(on_delay), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    return menu;
}

static GtkWidget *build_sound_menu(snip_app *app) {
    static const char *const labels[] = {
        [XD_RECORD_SILENT] = "_No Sound",
        [XD_RECORD_MICROPHONE] = "_Microphone",
        [XD_RECORD_COMPUTER] = "_Computer Sound",
        [XD_RECORD_MICROPHONE_AND_COMPUTER] = "Microphone _and Computer Sound",
    };
    GtkWidget *menu = gtk_menu_new();
    GSList *group = nullptr;
    for (size_t i = 0; i < G_N_ELEMENTS(labels); i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_mnemonic(group, labels[i]);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "sound", GINT_TO_POINTER((int)i));
        if (i == app->options.record_sound)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        g_signal_connect(item, "toggled", G_CALLBACK(on_sound), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    return menu;
}

static GtkToolItem *add_button(GtkWidget *toolbar, const char *icon, const char *label,
                               GCallback callback, snip_app *app) {
    GtkToolItem *item = gtk_tool_button_new(nullptr, label);
    if (icon != nullptr)
        gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(item), icon);
    gtk_tool_button_set_use_underline(GTK_TOOL_BUTTON(item), TRUE);
    gtk_tool_item_set_is_important(item, TRUE);
    g_signal_connect_swapped(item, "clicked", callback, app);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
    return item;
}

static void build_window(snip_app *app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), SNIP_TITLE);
    gtk_window_set_resizable(GTK_WINDOW(app->window), FALSE);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_BOTH_HORIZ);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_toolbar_set_show_arrow(GTK_TOOLBAR(toolbar), FALSE);
    GtkToolItem *new_snip = gtk_menu_tool_button_new(nullptr, "_New");
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(new_snip), "edit-cut");
    gtk_tool_button_set_use_underline(GTK_TOOL_BUTTON(new_snip), TRUE);
    gtk_tool_item_set_is_important(new_snip, TRUE);
    gtk_tool_item_set_tooltip_text(new_snip, "New Snip");
    gtk_menu_tool_button_set_menu(GTK_MENU_TOOL_BUTTON(new_snip), build_mode_menu(app));
    g_signal_connect_swapped(new_snip, "clicked", G_CALLBACK(on_new), app);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), new_snip, -1);

    GtkToolItem *record = gtk_menu_tool_button_new(nullptr, "_Record");
    gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(record), "media-record");
    gtk_tool_button_set_use_underline(GTK_TOOL_BUTTON(record), TRUE);
    gtk_tool_item_set_is_important(record, TRUE);
    gtk_tool_item_set_tooltip_text(record, "Record the screen (Win+Shift+R); the arrow chooses "
                                           "the sound");
    gtk_menu_tool_button_set_menu(GTK_MENU_TOOL_BUTTON(record), build_sound_menu(app));
    g_signal_connect_swapped(record, "clicked", G_CALLBACK(on_record), app);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), record, -1);

    GtkToolItem *delay_item = gtk_tool_item_new();
    GtkWidget *delay = gtk_menu_button_new();
    gtk_button_set_relief(GTK_BUTTON(delay), GTK_RELIEF_NONE);
    GtkWidget *delay_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *delay_label = gtk_label_new_with_mnemonic("_Delay");
    gtk_label_set_mnemonic_widget(GTK_LABEL(delay_label), delay);
    gtk_box_pack_start(GTK_BOX(delay_box), delay_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(delay_box),
                       gtk_image_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_BUTTON),
                       FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(delay), delay_box);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(delay), build_delay_menu(app));
    gtk_container_add(GTK_CONTAINER(delay_item), delay);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), delay_item, -1);

    app->cancel = GTK_WIDGET(add_button(toolbar, nullptr, "_Cancel", G_CALLBACK(on_cancel), app));
    gtk_widget_set_sensitive(app->cancel, FALSE);
    add_button(toolbar, nullptr, "_Options", G_CALLBACK(on_options), app);

    app->instructions = gtk_label_new(
        "Click New to snip part of the screen, or click the arrow next to it to choose the "
        "kind of snip: free-form, rectangular, a window or the whole screen.");
    gtk_label_set_line_wrap(GTK_LABEL(app->instructions), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(app->instructions), 44);
    gtk_label_set_xalign(GTK_LABEL(app->instructions), 0);
    gtk_widget_set_margin_start(app->instructions, 12);
    gtk_widget_set_margin_end(app->instructions, 12);
    gtk_widget_set_margin_top(app->instructions, 10);
    gtk_widget_set_margin_bottom(app->instructions, 12);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), app->instructions, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), box);
    gtk_widget_show_all(box);
    show_instructions(app);
}

/* ---- without a window: the clipboard ---------------------------------------------------------- */

static int serve(GBytes *png) {
    GError *error = nullptr;
    if (!snip_clipboard_serve(gdk_display_get_default(), png, &error)) {
        g_printerr("xd-snip: %s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }
    gtk_main();
    return EXIT_SUCCESS;
}

/* Print Screen and Alt+Print Screen. */
static int copy_screen(bool window_only) {
    GError *error = nullptr;
    xd_snip_screen *screen = snip_capture_screen(gdk_display_get_default(), &error);
    if (screen == nullptr) {
        g_printerr("xd-snip: %s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }
    xd_snip_box box = {screen->origin_x, screen->origin_y,
                       cairo_image_surface_get_width(screen->image) / screen->scale,
                       cairo_image_surface_get_height(screen->image) / screen->scale};
    int active = window_only ? xd_snip_window_active(screen->windows, screen->window_count) : -1;
    if (active >= 0) {
        const xd_snip_window *window = &screen->windows[active];
        box = (xd_snip_box){window->x, window->y, window->width, window->height};
    }
    cairo_surface_t *snip = xd_snip_screen_cut(screen, box, nullptr, 0, nullptr);
    xd_snip_screen_free(screen);
    if (snip == nullptr)
        return EXIT_FAILURE;
    g_autoptr(GBytes) png = snip_png_bytes(snip);
    cairo_surface_destroy(snip);
    return serve(png);
}

static int copy_input(void) {
    g_autoptr(GByteArray) data = g_byte_array_new();
    unsigned char chunk[65536];
    for (;;) {
        ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count == 0)
            break;
        if (count < 0)
            return EXIT_FAILURE;
        g_byte_array_append(data, chunk, (guint)count);
    }
    if (data->len == 0)
        return EXIT_FAILURE;
    g_autoptr(GBytes) png = g_byte_array_free_to_bytes(g_steal_pointer(&data));
    return serve(png);
}

int main(int argc, char **argv) {
    /* A reader that goes away mid-paste mustn't take us with it. */
    signal(SIGPIPE, SIG_IGN);
    const char *action = nullptr;
    const char *new_mode = nullptr;
    bool snip_now = false;
    const char *record = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--copy-screen") == 0 || strcmp(argv[i], "--copy-window") == 0 ||
            strcmp(argv[i], "--copy-stdin") == 0) {
            action = argv[i];
        } else if (strcmp(argv[i], "--new") == 0) {
            snip_now = true;
        } else if (g_str_has_prefix(argv[i], "--new=")) {
            snip_now = true;
            new_mode = argv[i] + strlen("--new=");
        } else if (strcmp(argv[i], "--record") == 0 || strcmp(argv[i], "--record=area") == 0) {
            record = argv[i];
        }
    }
    /* The shortcut again: the recording running stops. */
    if (record != nullptr && snip_recording_stop_running())
        return EXIT_SUCCESS;
    xd_gtk_app_preinit(SNIP_APP_ID);
    gtk_init(&argc, &argv);

    if (action != nullptr && strcmp(action, "--copy-stdin") == 0)
        return copy_input();
    if (action != nullptr)
        return copy_screen(strcmp(action, "--copy-window") == 0);

    xd_gtk_app_follow_theme();
    gtk_window_set_default_icon_name("applets-screenshooter");
    static snip_app app;
    load_options(&app);
    if (new_mode != nullptr && !xd_snip_mode_parse(new_mode, &app.options.mode)) {
        g_printerr("xd-snip: unknown kind of snip: %s\n", new_mode);
        return EXIT_FAILURE;
    }
    build_window(&app);
    if (record != nullptr) {
        app.quit_on_cancel = true;
        /* Marks which; record_area sets it again. */
        app.choosing_area = strcmp(record, "--record=area") == 0;
        g_idle_add(record_from_command_line, &app);
    } else if (snip_now) {
        app.quit_on_cancel = true;
        /* --new's delay is none: the key was the moment. */
        int delay = app.options.delay_seconds;
        app.options.delay_seconds = 0;
        snip_app_new_snip(&app, app.options.mode);
        app.options.delay_seconds = delay;
    } else {
        gtk_widget_show(app.window);
    }
    gtk_main();
    return EXIT_SUCCESS;
}
