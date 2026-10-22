/* SPDX-License-Identifier: MIT
 * xd-wuaucpl: Windows XP's Automatic Updates (wuaucpl.cpl), with the
 * updates themselves, which XP left to the Windows Update web site:
 *
 *   Automatic Updates  XP SP2's choices, less "Automatic": download
 *                      updates and let me choose when to install them,
 *                      notify me only, or turn it off; and when to check
 *                      (every day or every Sunday... at an hour). Saved
 *                      in desktop.ini [Updates]; the shell checks on that
 *                      schedule and shows XP's notification area icon and
 *                      balloon. Nothing is installed without a click.
 *   Software Updates   PackageKit's and Flatpak's updates (user and system
 *                      installations), as XP's Custom Install listed them:
 *                      check the ones to install, then Install
 *   Firmware           fwupd's devices, their firmware versions and LVFS
 *                      updates, installed with fwupd (which asks polkit)
 *   Device Security    fwupd's Host Security ID and what it's made of
 *
 *   xd-wuaucpl [--page N]   the Nth tab, from 0 (Automatic Updates)
 *   xd-wuaucpl --check      no window: refresh, count what's waiting and,
 *                           in Download mode and off metered connections,
 *                           download it; for the shell's schedule
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../appwiz/programs.h"
#include "../gtkapp/gtkapp.h"
#include "wuaucpl.h"

static const char *const day_names[] = {"Every day",       "Every Sunday",   "Every Monday",
                                        "Every Tuesday",   "Every Wednesday", "Every Thursday",
                                        "Every Friday",    "Every Saturday"};

GtkWidget *xd_wuau_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    return label;
}

GtkWidget *xd_wuau_frame(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

void xd_wuau_error(wuau *applet, const char *what, const char *detail) {
    char *text = detail != nullptr && detail[0] != '\0' ? g_strdup_printf("%s\n\n%s", what, detail)
                                                        : g_strdup(what);
    xd_message_box(GTK_WINDOW(applet->window), XD_WUAU_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

/* One of our programs: next to us (a build tree), else from PATH. */
static char *own_program(const char *name) {
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = self != nullptr ? g_path_get_dirname(self) : nullptr;
    char *local = dir != nullptr ? g_build_filename(dir, name, nullptr) : nullptr;
    g_free(self);
    g_free(dir);
    if (local != nullptr && g_file_test(local, G_FILE_TEST_IS_EXECUTABLE))
        return local;
    g_free(local);
    return g_strdup(name);
}

void xd_wuau_ask_restart(wuau *applet, const char *why) {
    /* XP's words after updates that need a restart. */
    char *text = g_strdup_printf("%s You must restart your computer for the updates to take "
                                 "effect.\n\nDo you want to restart your computer now?",
                                 why);
    static const char *const labels[] = {"Restart _Now", "Restart _Later"};
    int button_index = xd_message_box_choose(GTK_WINDOW(applet->window), XD_WUAU_TITLE,
                                             XD_MB_QUESTION, text, labels, 2, 0, 1);
    g_free(text);
    if (button_index != 0)
        return;
    /* Through Shut Down's own path (logind, which asks polkit). */
    char *prog = own_program("xd-shutdown");
    char action[] = "--action", restart[] = "restart";
    char *argv[] = {prog, action, restart, nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &error)) {
        xd_wuau_error(applet, "The computer could not be restarted.", error->message);
        g_error_free(error);
    }
    g_free(prog);
}

/* ---- the progress window ------------------------------------------------------------ */

struct xd_wuau_progress {
    GtkWidget *window, *bar, *status, *cancel_button;
    GCancellable *cancel;
    guint pulse;
};

static gboolean pulse(gpointer data) {
    xd_wuau_progress *progress = data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress->bar));
    return G_SOURCE_CONTINUE;
}

static void on_cancel(GtkButton *button, gpointer data) {
    xd_wuau_progress *progress = data;
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    gtk_label_set_text(GTK_LABEL(progress->status), "Canceling...");
    g_cancellable_cancel(progress->cancel);
}

static gboolean no_close([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] GdkEvent *event_data,
                         [[maybe_unused]] gpointer data) {
    return TRUE;
}

xd_wuau_progress *xd_wuau_progress_new(wuau *applet, const char *text, GCancellable *cancel) {
    xd_wuau_progress *progress = g_new0(xd_wuau_progress, 1);
    progress->cancel = g_object_ref(cancel);
    GtkWidget *dialog_widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dialog_widget), XD_WUAU_TITLE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog_widget), GTK_WINDOW(applet->window));
    gtk_window_set_modal(GTK_WINDOW(dialog_widget), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog_widget), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(dialog_widget), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_position(GTK_WINDOW(dialog_widget), GTK_WIN_POS_CENTER_ON_PARENT);
    g_signal_connect(dialog_widget, "delete-event", G_CALLBACK(no_close), nullptr);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name("software-update-available", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), xd_wuau_label(text), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
    progress->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(progress->bar, 340, -1);
    gtk_box_pack_start(GTK_BOX(box), progress->bar, FALSE, FALSE, 0);
    progress->status = xd_wuau_label("Starting...");
    gtk_label_set_ellipsize(GTK_LABEL(progress->status), PANGO_ELLIPSIZE_END);
    gtk_label_set_line_wrap(GTK_LABEL(progress->status), FALSE);
    gtk_box_pack_start(GTK_BOX(box), progress->status, FALSE, FALSE, 0);
    progress->cancel_button = gtk_button_new_with_mnemonic("Cancel");
    gtk_widget_set_size_request(progress->cancel_button, 80, -1);
    gtk_widget_set_halign(progress->cancel_button, GTK_ALIGN_END);
    g_signal_connect(progress->cancel_button, "clicked", G_CALLBACK(on_cancel), progress);
    gtk_box_pack_start(GTK_BOX(box), progress->cancel_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(dialog_widget), box);
    progress->window = dialog_widget;
    gtk_widget_show_all(dialog_widget);
    xd_wuau_progress_set(progress, -1, nullptr);
    return progress;
}

void xd_wuau_progress_set(xd_wuau_progress *progress, double fraction, const char *status) {
    if (status != nullptr && !g_cancellable_is_cancelled(progress->cancel))
        gtk_label_set_text(GTK_LABEL(progress->status), status);
    if (fraction < -1.5)
        return; /* the status alone */
    if (fraction < 0) {
        if (progress->pulse == 0)
            progress->pulse = g_timeout_add(100, pulse, progress);
        return;
    }
    if (progress->pulse != 0) {
        g_source_remove(progress->pulse);
        progress->pulse = 0;
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress->bar), XD_CLAMP(fraction, 0.0, 1.0));
}

void xd_wuau_progress_cancellable(xd_wuau_progress *progress, bool can) {
    if (!g_cancellable_is_cancelled(progress->cancel))
        gtk_widget_set_sensitive(progress->cancel_button, can);
}

void xd_wuau_progress_free(xd_wuau_progress *progress) {
    if (progress == nullptr)
        return;
    if (progress->pulse != 0)
        g_source_remove(progress->pulse);
    gtk_widget_destroy(progress->window);
    g_object_unref(progress->cancel);
    g_free(progress);
}

/* ---- what's waiting, for the shell ------------------------------------------------- */

void xd_wuau_save_state(wuau *applet, bool checked) {
    char *path = xd_updates_state_path();
    xd_updates_state st;
    xd_updates_state_read(path, &st);
    int important = 0;
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    int software = xd_wuau_software_count(applet, &important, ids);
    if (software != st.software)
        st.downloaded = false; /* a different set; the next check downloads it */
    st.software = software;
    if (applet->fw_updates >= 0) {
        st.firmware = applet->fw_updates;
        xd_wuau_firmware_ids(applet, &important, ids);
    }
    st.important = important;
    xd_updates_signature((const char *const *)ids->pdata, ids->len, st.signature);
    if (checked) {
        st.checked = g_get_real_time() / G_USEC_PER_SEC;
        st.error[0] = '\0';
    }
    if (!xd_updates_state_write(path, &st))
        g_printerr("xd-wuaucpl: cannot write %s\n", path);
    g_ptr_array_free(ids, TRUE);
    g_free(path);
}

/* ---- Automatic Updates ----------------------------------------------------------------- */

static void changed(wuau *applet) {
    if (!applet->filling)
        gtk_widget_set_sensitive(applet->apply, TRUE);
}

static void sync_schedule(wuau *applet) {
    bool on = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(applet->mode[XD_UPDATES_OFF]));
    gtk_widget_set_sensitive(applet->schedule, on);
}

static void on_mode(GtkToggleButton *toggle, gpointer data) {
    wuau *applet = data;
    sync_schedule(applet);
    if (gtk_toggle_button_get_active(toggle))
        changed(applet);
}

static void on_combo([[maybe_unused]] GtkComboBox *combo, gpointer data) { changed(data); }

/* "3:00 AM", as XP's hour list. g_free it. */
static char *hour_text(int hour) {
    return g_strdup_printf("%d:00 %s", hour % 12 == 0 ? 12 : hour % 12, hour < 12 ? "AM" : "PM");
}

/* "Last checked for updates: 10/18/2026 at 3:00 AM" */
static void show_last_checked(wuau *applet) {
    char *path = xd_updates_state_path();
    xd_updates_state st;
    xd_updates_state_read(path, &st);
    g_free(path);
    char *text;
    if (st.checked == 0) {
        text = g_strdup("Your computer hasn't checked for updates yet.");
    } else {
        GDateTime *date_time = g_date_time_new_from_unix_local(st.checked);
        char *date = xd_appwiz_format_date(st.checked);
        char *time =
            date_time != nullptr ? g_date_time_format(date_time, "%-I:%M %p") : g_strdup("");
        text = g_strdup_printf("Last checked for updates: %s at %s.%s%s", date, time,
                               st.error[0] != '\0' ? "\nThe check didn't finish: " : "",
                               st.error);
        g_free(time);
        g_free(date);
        if (date_time != nullptr)
            g_date_time_unref(date_time);
    }
    gtk_label_set_text(GTK_LABEL(applet->last_checked), text);
    g_free(text);
}

static GtkWidget *options_page(wuau *applet, const xd_settings *settings) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("software-update-available", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(head), img, FALSE, FALSE, 0);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title = xd_wuau_label(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Help protect your PC</b>");
    gtk_box_pack_start(GTK_BOX(texts), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts),
                       xd_wuau_label("Your computer can regularly check for updates to its "
                                     "programs, apps and device firmware, and tell you when "
                                     "they're ready. Nothing is installed until you choose to "
                                     "install it."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), texts, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);

    static const char *const labels[XD_UPDATES_MODE_COUNT] = {
        [XD_UPDATES_DOWNLOAD] = ("_Download updates for me, but let me choose when to install "
                                 "them."),
        [XD_UPDATES_NOTIFY] = "_Notify me but don't automatically download or install them.",
        [XD_UPDATES_OFF] = "_Turn off Automatic Updates.",
    };
    GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GSList *group = nullptr;
    for (int i = 0; i < XD_UPDATES_MODE_COUNT; i++) {
        applet->mode[i] = gtk_radio_button_new_with_mnemonic(group, labels[i]);
        gtk_label_set_line_wrap(GTK_LABEL(gtk_bin_get_child(GTK_BIN(applet->mode[i]))), TRUE);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(applet->mode[i]));
        gtk_box_pack_start(GTK_BOX(choices), applet->mode[i], FALSE, FALSE, 0);
        GtkWidget *note = nullptr;
        if (i == XD_UPDATES_DOWNLOAD)
            note = xd_wuau_label("Updates aren't downloaded while the computer is on a metered "
                                 "connection.");
        else if (i == XD_UPDATES_OFF)
            /* XP SP2's warning under the same choice. */
            note = xd_wuau_label("Your computer will be more vulnerable unless you install "
                                 "updates regularly.");
        if (note != nullptr) {
            gtk_widget_set_margin_start(note, 24);
            gtk_box_pack_start(GTK_BOX(choices), note, FALSE, FALSE, 0);
        }
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(applet->mode[settings->updates.mode]), TRUE);
    for (int i = 0; i < XD_UPDATES_MODE_COUNT; i++)
        g_signal_connect(applet->mode[i], "toggled", G_CALLBACK(on_mode), applet);

    /* When: XP's "Every day" / "Every Sunday"... and its hours. */
    applet->schedule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *check = gtk_label_new_with_mnemonic("_Check for updates:");
    gtk_box_pack_start(GTK_BOX(applet->schedule), check, FALSE, FALSE, 0);
    applet->day = gtk_combo_box_text_new();
    for (size_t i = 0; i < XD_ARRAY_LEN(day_names); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(applet->day), day_names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(applet->day), settings->updates.day);
    gtk_label_set_mnemonic_widget(GTK_LABEL(check), applet->day);
    gtk_box_pack_start(GTK_BOX(applet->schedule), applet->day, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(applet->schedule), gtk_label_new("at"), FALSE, FALSE, 0);
    applet->hour = gtk_combo_box_text_new();
    for (int hour = 0; hour < 24; hour++) {
        char *hour_label = hour_text(hour);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(applet->hour), hour_label);
        g_free(hour_label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(applet->hour), settings->updates.hour);
    gtk_box_pack_start(GTK_BOX(applet->schedule), applet->hour, FALSE, FALSE, 0);
    g_signal_connect(applet->day, "changed", G_CALLBACK(on_combo), applet);
    g_signal_connect(applet->hour, "changed", G_CALLBACK(on_combo), applet);
    gtk_box_pack_start(GTK_BOX(choices), applet->schedule, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(page), xd_wuau_frame(nullptr, choices), FALSE, FALSE, 0);
    sync_schedule(applet);

    applet->last_checked = xd_wuau_label("");
    gtk_box_pack_start(GTK_BOX(page), applet->last_checked, FALSE, FALSE, 0);
    show_last_checked(applet);
    return page;
}

static bool apply(wuau *applet) {
    /* Re-read: another program may have saved since we opened. */
    xd_settings settings;
    xd_settings_load(&settings);
    for (int i = 0; i < XD_UPDATES_MODE_COUNT; i++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(applet->mode[i])))
            settings.updates.mode = (xd_updates_mode)i;
    }
    settings.updates.day = XD_MAX(gtk_combo_box_get_active(GTK_COMBO_BOX(applet->day)), 0);
    settings.updates.hour = XD_MAX(gtk_combo_box_get_active(GTK_COMBO_BOX(applet->hour)), 0);
    if (!xd_settings_save(&settings)) {
        xd_wuau_error(applet, "The settings could not be saved.", nullptr);
        return false;
    }
    gtk_widget_set_sensitive(applet->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    wuau *applet = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(applet);
        return;
    }
    /* Not while an install is running: its progress window is modal, but
     * Escape on the dialog would still get here. */
    if (applet->busy)
        return;
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(applet->apply) && !apply(applet))
        return;
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

static void on_backend(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    if (!pk_control_get_properties_finish(PK_CONTROL(src), res, &error)) {
        bool gone = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        g_error_free(error);
        if (gone)
            return;
    }
    wuau *applet = data;
    g_object_get(applet->control, "backend-name", &applet->backend, nullptr);
    xd_wuau_software_load(applet);
}

int main(int argc, char **argv) {
    int page = 0;
    bool check = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)
            check = true;
        else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
        /* The shell's --style and --scheme overrides: desktop.ini is read
         * here anyway. */
        else if ((strcmp(argv[i], "--style") == 0 || strcmp(argv[i], "--scheme") == 0) &&
                 i + 1 < argc)
            i++;
        else
            fprintf(stderr, "xd-wuaucpl: ignoring %s\n", argv[i]);
    }
    if (check)
        return xd_wuau_check();
    xd_gtk_app_preinit("org.xeondesktop.AutomaticUpdates");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static wuau applet;
    applet.quit = g_cancellable_new();
    applet.fw_updates = -1;
    applet.pk = pk_client_new();
    pk_client_set_interactive(applet.pk, FALSE);
    applet.pk_task = pk_client_new();
    pk_client_set_interactive(applet.pk_task, TRUE);
    applet.control = pk_control_new();
    xd_settings settings;
    xd_settings_load(&settings);

    applet.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(applet.window), XD_WUAU_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(applet.window), "software-update-available");
    gtk_window_set_default_size(GTK_WINDOW(applet.window), 560, 520);
    gtk_dialog_add_button(GTK_DIALOG(applet.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(applet.window), "Cancel", GTK_RESPONSE_CANCEL);
    applet.apply = gtk_dialog_add_button(GTK_DIALOG(applet.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(applet.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(applet.window), GTK_RESPONSE_OK);

    applet.filling = true;
    applet.notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(applet.notebook), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(applet.notebook), options_page(&applet, &settings),
                             gtk_label_new_with_mnemonic("Automatic _Updates"));
    gtk_notebook_append_page(GTK_NOTEBOOK(applet.notebook), xd_wuau_software_page(&applet),
                             gtk_label_new_with_mnemonic("_Software Updates"));
    gtk_notebook_append_page(GTK_NOTEBOOK(applet.notebook), xd_wuau_firmware_page(&applet),
                             gtk_label_new_with_mnemonic("_Firmware"));
    gtk_notebook_append_page(GTK_NOTEBOOK(applet.notebook), xd_wuau_security_page(&applet),
                             gtk_label_new_with_mnemonic("Device Securit_y"));
    applet.filling = false;
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(applet.window))),
                       applet.notebook, TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(applet.window, GTK_NOTEBOOK(applet.notebook));
    g_signal_connect(applet.window, "response", G_CALLBACK(on_response), &applet);
    g_signal_connect(applet.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(applet.window);
    gtk_notebook_set_current_page(
        GTK_NOTEBOOK(applet.notebook),
        XD_CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(applet.notebook)) - 1));

    pk_control_get_properties_async(applet.control, applet.quit, on_backend, &applet);
    xd_wuau_firmware_load(&applet);
    gtk_main();

    g_cancellable_cancel(applet.quit);
    xd_wuau_software_finish(&applet);
    xd_wuau_firmware_finish(&applet);
    g_object_unref(applet.pk);
    g_object_unref(applet.pk_task);
    g_object_unref(applet.control);
    g_free(applet.backend);
    g_object_unref(applet.quit);
    return EXIT_SUCCESS;
}
