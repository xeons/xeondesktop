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
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    return l;
}

GtkWidget *xd_wuau_frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

void xd_wuau_error(wuau *w, const char *what, const char *detail) {
    char *text = detail != nullptr && detail[0] != '\0' ? g_strdup_printf("%s\n\n%s", what, detail)
                                                        : g_strdup(what);
    xd_message_box(GTK_WINDOW(w->window), XD_WUAU_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
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

void xd_wuau_ask_restart(wuau *w, const char *why) {
    /* XP's words after updates that need a restart. */
    char *text = g_strdup_printf("%s You must restart your computer for the updates to take "
                                 "effect.\n\nDo you want to restart your computer now?",
                                 why);
    static const char *const labels[] = {"Restart _Now", "Restart _Later"};
    int r = xd_message_box_choose(GTK_WINDOW(w->window), XD_WUAU_TITLE, XD_MB_QUESTION, text,
                                  labels, 2, 0, 1);
    g_free(text);
    if (r != 0)
        return;
    /* Through Shut Down's own path (logind, which asks polkit). */
    char *prog = own_program("xd-shutdown");
    char action[] = "--action", restart[] = "restart";
    char *argv[] = {prog, action, restart, nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                       &error)) {
        xd_wuau_error(w, "The computer could not be restarted.", error->message);
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
    xd_wuau_progress *p = data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(p->bar));
    return G_SOURCE_CONTINUE;
}

static void on_cancel(GtkButton *b, gpointer data) {
    xd_wuau_progress *p = data;
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    gtk_label_set_text(GTK_LABEL(p->status), "Canceling...");
    g_cancellable_cancel(p->cancel);
}

static gboolean no_close([[maybe_unused]] GtkWidget *w, [[maybe_unused]] GdkEvent *e,
                         [[maybe_unused]] gpointer data) {
    return TRUE;
}

xd_wuau_progress *xd_wuau_progress_new(wuau *w, const char *text, GCancellable *cancel) {
    xd_wuau_progress *p = g_new0(xd_wuau_progress, 1);
    p->cancel = g_object_ref(cancel);
    GtkWidget *d = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(d), XD_WUAU_TITLE);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(w->window));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(d), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ON_PARENT);
    g_signal_connect(d, "delete-event", G_CALLBACK(no_close), nullptr);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name("software-update-available", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), xd_wuau_label(text), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
    p->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(p->bar, 340, -1);
    gtk_box_pack_start(GTK_BOX(box), p->bar, FALSE, FALSE, 0);
    p->status = xd_wuau_label("Starting...");
    gtk_label_set_ellipsize(GTK_LABEL(p->status), PANGO_ELLIPSIZE_END);
    gtk_label_set_line_wrap(GTK_LABEL(p->status), FALSE);
    gtk_box_pack_start(GTK_BOX(box), p->status, FALSE, FALSE, 0);
    p->cancel_button = gtk_button_new_with_mnemonic("Cancel");
    gtk_widget_set_size_request(p->cancel_button, 80, -1);
    gtk_widget_set_halign(p->cancel_button, GTK_ALIGN_END);
    g_signal_connect(p->cancel_button, "clicked", G_CALLBACK(on_cancel), p);
    gtk_box_pack_start(GTK_BOX(box), p->cancel_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(d), box);
    p->window = d;
    gtk_widget_show_all(d);
    xd_wuau_progress_set(p, -1, nullptr);
    return p;
}

void xd_wuau_progress_set(xd_wuau_progress *p, double fraction, const char *status) {
    if (status != nullptr && !g_cancellable_is_cancelled(p->cancel))
        gtk_label_set_text(GTK_LABEL(p->status), status);
    if (fraction < -1.5)
        return; /* the status alone */
    if (fraction < 0) {
        if (p->pulse == 0)
            p->pulse = g_timeout_add(100, pulse, p);
        return;
    }
    if (p->pulse != 0) {
        g_source_remove(p->pulse);
        p->pulse = 0;
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->bar), XD_CLAMP(fraction, 0.0, 1.0));
}

void xd_wuau_progress_cancellable(xd_wuau_progress *p, bool can) {
    if (!g_cancellable_is_cancelled(p->cancel))
        gtk_widget_set_sensitive(p->cancel_button, can);
}

void xd_wuau_progress_free(xd_wuau_progress *p) {
    if (p == nullptr)
        return;
    if (p->pulse != 0)
        g_source_remove(p->pulse);
    gtk_widget_destroy(p->window);
    g_object_unref(p->cancel);
    g_free(p);
}

/* ---- what's waiting, for the shell ------------------------------------------------- */

void xd_wuau_save_state(wuau *w, bool checked) {
    char *path = xd_updates_state_path();
    xd_updates_state st;
    xd_updates_state_read(path, &st);
    int important = 0;
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    int software = xd_wuau_software_count(w, &important, ids);
    if (software != st.software)
        st.downloaded = false; /* a different set; the next check downloads it */
    st.software = software;
    if (w->fw_updates >= 0) {
        st.firmware = w->fw_updates;
        xd_wuau_firmware_ids(w, &important, ids);
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

static void changed(wuau *w) {
    if (!w->filling)
        gtk_widget_set_sensitive(w->apply, TRUE);
}

static void sync_schedule(wuau *w) {
    bool on = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->mode[XD_UPDATES_OFF]));
    gtk_widget_set_sensitive(w->schedule, on);
}

static void on_mode(GtkToggleButton *b, gpointer data) {
    wuau *w = data;
    sync_schedule(w);
    if (gtk_toggle_button_get_active(b))
        changed(w);
}

static void on_combo([[maybe_unused]] GtkComboBox *c, gpointer data) { changed(data); }

/* "3:00 AM", as XP's hour list. g_free it. */
static char *hour_text(int hour) {
    return g_strdup_printf("%d:00 %s", hour % 12 == 0 ? 12 : hour % 12, hour < 12 ? "AM" : "PM");
}

/* "Last checked for updates: 10/18/2026 at 3:00 AM" */
static void show_last_checked(wuau *w) {
    char *path = xd_updates_state_path();
    xd_updates_state st;
    xd_updates_state_read(path, &st);
    g_free(path);
    char *text;
    if (st.checked == 0) {
        text = g_strdup("Your computer hasn't checked for updates yet.");
    } else {
        GDateTime *t = g_date_time_new_from_unix_local(st.checked);
        char *date = xd_appwiz_format_date(st.checked);
        char *time = t != nullptr ? g_date_time_format(t, "%-I:%M %p") : g_strdup("");
        text = g_strdup_printf("Last checked for updates: %s at %s.%s%s", date, time,
                               st.error[0] != '\0' ? "\nThe check didn't finish: " : "",
                               st.error);
        g_free(time);
        g_free(date);
        if (t != nullptr)
            g_date_time_unref(t);
    }
    gtk_label_set_text(GTK_LABEL(w->last_checked), text);
    g_free(text);
}

static GtkWidget *options_page(wuau *w, const xd_settings *s) {
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
        w->mode[i] = gtk_radio_button_new_with_mnemonic(group, labels[i]);
        gtk_label_set_line_wrap(GTK_LABEL(gtk_bin_get_child(GTK_BIN(w->mode[i]))), TRUE);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(w->mode[i]));
        gtk_box_pack_start(GTK_BOX(choices), w->mode[i], FALSE, FALSE, 0);
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
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->mode[s->updates.mode]), TRUE);
    for (int i = 0; i < XD_UPDATES_MODE_COUNT; i++)
        g_signal_connect(w->mode[i], "toggled", G_CALLBACK(on_mode), w);

    /* When: XP's "Every day" / "Every Sunday"... and its hours. */
    w->schedule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *check = gtk_label_new_with_mnemonic("_Check for updates:");
    gtk_box_pack_start(GTK_BOX(w->schedule), check, FALSE, FALSE, 0);
    w->day = gtk_combo_box_text_new();
    for (size_t i = 0; i < XD_ARRAY_LEN(day_names); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->day), day_names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->day), s->updates.day);
    gtk_label_set_mnemonic_widget(GTK_LABEL(check), w->day);
    gtk_box_pack_start(GTK_BOX(w->schedule), w->day, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(w->schedule), gtk_label_new("at"), FALSE, FALSE, 0);
    w->hour = gtk_combo_box_text_new();
    for (int h = 0; h < 24; h++) {
        char *t = hour_text(h);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->hour), t);
        g_free(t);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->hour), s->updates.hour);
    gtk_box_pack_start(GTK_BOX(w->schedule), w->hour, FALSE, FALSE, 0);
    g_signal_connect(w->day, "changed", G_CALLBACK(on_combo), w);
    g_signal_connect(w->hour, "changed", G_CALLBACK(on_combo), w);
    gtk_box_pack_start(GTK_BOX(choices), w->schedule, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(page), xd_wuau_frame(nullptr, choices), FALSE, FALSE, 0);
    sync_schedule(w);

    w->last_checked = xd_wuau_label("");
    gtk_box_pack_start(GTK_BOX(page), w->last_checked, FALSE, FALSE, 0);
    show_last_checked(w);
    return page;
}

static bool apply(wuau *w) {
    /* Re-read: another program may have saved since we opened. */
    xd_settings s;
    xd_settings_load(&s);
    for (int i = 0; i < XD_UPDATES_MODE_COUNT; i++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->mode[i])))
            s.updates.mode = (xd_updates_mode)i;
    }
    s.updates.day = XD_MAX(gtk_combo_box_get_active(GTK_COMBO_BOX(w->day)), 0);
    s.updates.hour = XD_MAX(gtk_combo_box_get_active(GTK_COMBO_BOX(w->hour)), 0);
    if (!xd_settings_save(&s)) {
        xd_wuau_error(w, "The settings could not be saved.", nullptr);
        return false;
    }
    gtk_widget_set_sensitive(w->apply, FALSE);
    return true;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    wuau *w = data;
    if (response == GTK_RESPONSE_APPLY) {
        apply(w);
        return;
    }
    /* Not while an install is running: its progress window is modal, but
     * Escape on the dialog would still get here. */
    if (w->busy)
        return;
    if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(w->apply) && !apply(w))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_backend(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    if (!pk_control_get_properties_finish(PK_CONTROL(src), res, &error)) {
        bool gone = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        g_error_free(error);
        if (gone)
            return;
    }
    wuau *w = data;
    g_object_get(w->control, "backend-name", &w->backend, nullptr);
    xd_wuau_software_load(w);
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
    static wuau w;
    w.quit = g_cancellable_new();
    w.fw_updates = -1;
    w.pk = pk_client_new();
    pk_client_set_interactive(w.pk, FALSE);
    w.pk_task = pk_client_new();
    pk_client_set_interactive(w.pk_task, TRUE);
    w.control = pk_control_new();
    xd_settings s;
    xd_settings_load(&s);

    w.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(w.window), XD_WUAU_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(w.window), "software-update-available");
    gtk_window_set_default_size(GTK_WINDOW(w.window), 560, 520);
    gtk_dialog_add_button(GTK_DIALOG(w.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(w.window), "Cancel", GTK_RESPONSE_CANCEL);
    w.apply = gtk_dialog_add_button(GTK_DIALOG(w.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_widget_set_sensitive(w.apply, FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(w.window), GTK_RESPONSE_OK);

    w.filling = true;
    w.notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(w.notebook), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(w.notebook), options_page(&w, &s),
                             gtk_label_new_with_mnemonic("Automatic _Updates"));
    gtk_notebook_append_page(GTK_NOTEBOOK(w.notebook), xd_wuau_software_page(&w),
                             gtk_label_new_with_mnemonic("_Software Updates"));
    gtk_notebook_append_page(GTK_NOTEBOOK(w.notebook), xd_wuau_firmware_page(&w),
                             gtk_label_new_with_mnemonic("_Firmware"));
    gtk_notebook_append_page(GTK_NOTEBOOK(w.notebook), xd_wuau_security_page(&w),
                             gtk_label_new_with_mnemonic("Device Securit_y"));
    w.filling = false;
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(w.window))), w.notebook,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(w.window, GTK_NOTEBOOK(w.notebook));
    g_signal_connect(w.window, "response", G_CALLBACK(on_response), &w);
    g_signal_connect(w.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(w.window);
    gtk_notebook_set_current_page(
        GTK_NOTEBOOK(w.notebook),
        XD_CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(w.notebook)) - 1));

    pk_control_get_properties_async(w.control, w.quit, on_backend, &w);
    xd_wuau_firmware_load(&w);
    gtk_main();

    g_cancellable_cancel(w.quit);
    xd_wuau_software_finish(&w);
    xd_wuau_firmware_finish(&w);
    g_object_unref(w.pk);
    g_object_unref(w.pk_task);
    g_object_unref(w.control);
    g_free(w.backend);
    g_object_unref(w.quit);
    return EXIT_SUCCESS;
}
