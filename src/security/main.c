/* SPDX-License-Identifier: MIT
 * xd-security: Windows 2000's Windows Security dialog, opened by
 * Ctrl+Alt+Delete in the Classic style: who is logged on and since when,
 * and Lock Computer, Log Off, Shut Down, Change Password and Task Manager.
 *
 * Change Password runs passwd(1) for the user (see passwd.h).
 */
#include <langinfo.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/settings.h"
#include "passwd.h"

#define PRODUCT "XeonDesktop"
#define EDITION "Professional"

/* ---- helpers --------------------------------------------------------------------- */

/* Runs one of our programs: next to this one (a build tree), else PATH. */
static void run_own(const char *program, const char *arg) {
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = g_path_get_dirname(self != nullptr ? self : program);
    g_free(self);
    char *local = g_build_filename(dir, program, nullptr);
    const char *exe = g_file_test(local, G_FILE_TEST_IS_EXECUTABLE) ? local : program;
    const char *argv[] = {exe, arg, nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, nullptr, nullptr,
                       nullptr, &error)) {
        g_printerr("xd-security: cannot run %s: %s\n", program, error->message);
        g_error_free(error);
    }
    g_free(local);
    g_free(dir);
}

/* COMPUTER, as Windows shows the NetBIOS name: the host name's first
 * label in capitals. */
static char *computer_name(void) {
    char *host = g_ascii_strup(g_get_host_name(), -1);
    char *dot = strchr(host, '.');
    if (dot != nullptr)
        *dot = '\0';
    return host;
}

/* "COMPUTER\user (Full Name)". */
static char *logon_name(void) {
    char *computer = computer_name();
    const char *user = g_get_user_name();
    const char *real = g_get_real_name();
    char *name = real != nullptr && real[0] != '\0' && strcmp(real, "Unknown") != 0 &&
                         strcmp(real, user) != 0
                     ? g_strdup_printf("%s\\%s (%s)", computer, user, real)
                     : g_strdup_printf("%s\\%s", computer, user);
    g_free(computer);
    return name;
}

/* When the session began: the compositor's start time. */
static GDateTime *logon_time(void) {
    const char *env = getenv("XEONDESKTOP_PID");
    gint64 pid = env != nullptr ? g_ascii_strtoll(env, nullptr, 10) : 0;
    char *path = g_strdup_printf("/proc/%" G_GINT64_FORMAT "/stat", pid);
    char *stat = nullptr;
    char *sys = nullptr;
    GDateTime *date_time = nullptr;
    if (pid > 1 && g_file_get_contents(path, &stat, nullptr, nullptr) &&
        g_file_get_contents("/proc/stat", &sys, nullptr, nullptr)) {
        /* Field 22 (starttime, in clock ticks after boot) counts from the
         * ")" that ends the command name. */
        char *name_end = strrchr(stat, ')');
        char **fields = name_end != nullptr ? g_strsplit(name_end + 2, " ", 0) : nullptr;
        char *btime = strstr(sys, "\nbtime ");
        if (fields != nullptr && g_strv_length(fields) > 19 && btime != nullptr) {
            gint64 ticks = g_ascii_strtoll(fields[19], nullptr, 10);
            gint64 boot = g_ascii_strtoll(btime + 7, nullptr, 10);
            long hz = sysconf(_SC_CLK_TCK);
            if (hz > 0)
                date_time = g_date_time_new_from_unix_local(boot + ticks / hz);
        }
        g_strfreev(fields);
    }
    g_free(sys);
    g_free(stat);
    g_free(path);
    return date_time != nullptr ? date_time : g_date_time_new_now_local();
}

static GtkWidget *label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    return widget;
}

/* ---- the banner -------------------------------------------------------------------- */

static gboolean draw_banner(GtkWidget *widget, cairo_t *cairo, [[maybe_unused]] gpointer data) {
    xd_settings settings;
    xd_settings_load(&settings);
    xd_scheme scheme;
    xd_settings_scheme(&settings, &scheme);
    xd_theme theme;
    xd_theme_init(&theme, settings.style, &scheme, 1.0);
    xd_rect rect = {0, 0, gtk_widget_get_allocated_width(widget),
                    gtk_widget_get_allocated_height(widget)};
    xd_theme_draw_dialog_banner(&theme, cairo, rect, PRODUCT, EDITION);
    return FALSE;
}

static GtkWidget *banner(void) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, -1, 72);
    g_signal_connect(area, "draw", G_CALLBACK(draw_banner), nullptr);
    return area;
}

/* A dialog with the banner on top and `content` under it. */
static GtkWidget *banner_dialog(GtkWindow *parent, const char *title, GtkWidget **content) {
    GtkWidget *widget = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(widget), title);
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(widget), TRUE);
    if (parent != nullptr) {
        gtk_window_set_transient_for(GTK_WINDOW(widget), parent);
        gtk_window_set_modal(GTK_WINDOW(widget), TRUE);
    }
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(widget));
    gtk_box_set_spacing(GTK_BOX(area), 0);
    gtk_box_pack_start(GTK_BOX(area), banner(), FALSE, FALSE, 0);
    *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(*content), 12);
    gtk_box_pack_start(GTK_BOX(area), *content, TRUE, TRUE, 0);
    return widget;
}

/* ---- Change Password ------------------------------------------------------------- */

typedef struct change {
    GtkWidget *dialog;
    GtkWidget *old_entry, *new_entry, *confirm_entry;
    xd_passwd *passwd;
    guint watch;
} change;

static void change_busy(change *password_change, bool busy) {
    gtk_widget_set_sensitive(
        gtk_dialog_get_widget_for_response(GTK_DIALOG(password_change->dialog), GTK_RESPONSE_OK),
        !busy);
    gtk_widget_set_sensitive(password_change->old_entry, !busy);
    gtk_widget_set_sensitive(password_change->new_entry, !busy);
    gtk_widget_set_sensitive(password_change->confirm_entry, !busy);
    GdkWindow *gw = gtk_widget_get_window(password_change->dialog);
    if (gw != nullptr) {
        GdkCursor *cursor =
            busy ? gdk_cursor_new_from_name(gdk_window_get_display(gw), "progress") : nullptr;
        gdk_window_set_cursor(gw, cursor);
        if (cursor != nullptr)
            g_object_unref(cursor);
    }
}

static void clear_entry(GtkWidget *widget) { gtk_entry_set_text(GTK_ENTRY(widget), ""); }

static gboolean on_passwd_output([[maybe_unused]] GIOChannel *ch,
                                 [[maybe_unused]] GIOCondition cond, gpointer data) {
    change *password_change = data;
    xd_passwd_result passwd_result = xd_passwd_step(password_change->passwd);
    if (passwd_result == XD_PASSWD_RUNNING)
        return G_SOURCE_CONTINUE;
    password_change->watch = 0;
    char *message = g_strdup(xd_passwd_message(password_change->passwd));
    xd_passwd_free(password_change->passwd);
    password_change->passwd = nullptr;
    change_busy(password_change, false);
    GtkWindow *toplevel = GTK_WINDOW(password_change->dialog);
    switch (passwd_result) {
    case XD_PASSWD_OK:
        xd_message_box(toplevel, "Change Password", XD_MB_INFO, XD_MB_OK, FALSE,
                       "Your password has been changed.");
        gtk_widget_destroy(password_change->dialog);
        break;
    case XD_PASSWD_WRONG_OLD:
        xd_message_box(toplevel, "Change Password", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The old password is incorrect. Letters in passwords must be typed "
                       "using the correct case.");
        clear_entry(password_change->old_entry);
        gtk_widget_grab_focus(password_change->old_entry);
        break;
    case XD_PASSWD_REJECTED: {
        char *text = g_strdup_printf(
            "Unable to update the password. The value provided for the new password does "
            "not meet the length, complexity, or history requirements of this computer."
            "%s%s",
            message[0] != '\0' ? "\n\n" : "", message);
        xd_message_box(toplevel, "Change Password", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
        clear_entry(password_change->new_entry);
        clear_entry(password_change->confirm_entry);
        gtk_widget_grab_focus(password_change->new_entry);
        break;
    }
    case XD_PASSWD_FAILED:
    case XD_PASSWD_RUNNING: {
        char *text = g_strdup_printf("Unable to change the password.%s%s",
                                     message[0] != '\0' ? "\n\n" : "", message);
        xd_message_box(toplevel, "Change Password", XD_MB_ERROR, XD_MB_OK, FALSE, text);
        g_free(text);
        break;
    }
    }
    g_free(message);
    return G_SOURCE_REMOVE;
}

static void on_change_response(GtkDialog *dialog_widget, int response, gpointer data) {
    change *password_change = data;
    if (response != GTK_RESPONSE_OK) {
        gtk_widget_destroy(GTK_WIDGET(dialog_widget));
        return;
    }
    const char *old = gtk_entry_get_text(GTK_ENTRY(password_change->old_entry));
    const char *new = gtk_entry_get_text(GTK_ENTRY(password_change->new_entry));
    const char *confirm = gtk_entry_get_text(GTK_ENTRY(password_change->confirm_entry));
    if (strcmp(new, confirm) != 0) {
        xd_message_box(GTK_WINDOW(dialog_widget), "Change Password", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The passwords you typed do not match. Type the new password in both "
                       "text boxes.");
        clear_entry(password_change->new_entry);
        clear_entry(password_change->confirm_entry);
        gtk_widget_grab_focus(password_change->new_entry);
        return;
    }
    GError *error = nullptr;
    password_change->passwd = xd_passwd_start(nullptr, old, new, &error);
    if (password_change->passwd == nullptr) {
        xd_message_box(GTK_WINDOW(dialog_widget), "Change Password", XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
        return;
    }
    GIOChannel *ch = g_io_channel_unix_new(xd_passwd_fd(password_change->passwd));
    password_change->watch = g_io_add_watch(ch, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                            on_passwd_output, password_change);
    g_io_channel_unref(ch);
    change_busy(password_change, true);
}

static void on_change_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    change *password_change = data;
    if (password_change->watch != 0)
        g_source_remove(password_change->watch);
    xd_passwd_free(password_change->passwd);
    g_free(password_change);
}

static GtkWidget *password_entry(void) {
    GtkWidget *widget = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(widget), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(widget), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(widget), TRUE);
    gtk_widget_set_hexpand(widget, TRUE);
    return widget;
}

static void attach_row(GtkGrid *grid, int row, const char *mnemonic, GtkWidget *field) {
    GtkWidget *widget = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), field);
    gtk_grid_attach(grid, widget, 0, row, 1, 1);
    gtk_grid_attach(grid, field, 1, row, 1, 1);
}

static GtkWidget *open_change_password(GtkWindow *parent) {
    change *password_change = g_new0(change, 1);
    GtkWidget *content;
    password_change->dialog = banner_dialog(parent, "Change Password", &content);
    gtk_dialog_add_button(GTK_DIALOG(password_change->dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(password_change->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(password_change->dialog), GTK_RESPONSE_OK);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_size_request(grid, 330, -1);
    GtkWidget *user = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(user), g_get_user_name());
    gtk_widget_set_sensitive(user, FALSE);
    attach_row(GTK_GRID(grid), 0, "_User name:", user);
    GtkWidget *logon = gtk_combo_box_text_new();
    char *computer = computer_name();
    char *item = g_strdup_printf("%s (this computer)", computer);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(logon), item);
    gtk_combo_box_set_active(GTK_COMBO_BOX(logon), 0);
    gtk_widget_set_sensitive(logon, FALSE);
    g_free(item);
    g_free(computer);
    attach_row(GTK_GRID(grid), 1, "_Log on to:", logon);
    password_change->old_entry = password_entry();
    attach_row(GTK_GRID(grid), 2, "_Old Password:", password_change->old_entry);
    password_change->new_entry = password_entry();
    attach_row(GTK_GRID(grid), 3, "_New Password:", password_change->new_entry);
    password_change->confirm_entry = password_entry();
    attach_row(GTK_GRID(grid), 4, "Con_firm New Password:", password_change->confirm_entry);
    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

    g_signal_connect(password_change->dialog, "response", G_CALLBACK(on_change_response),
                     password_change);
    g_signal_connect(password_change->dialog, "destroy", G_CALLBACK(on_change_destroy),
                     password_change);
    gtk_widget_show_all(password_change->dialog);
    gtk_widget_grab_focus(password_change->old_entry);
    return password_change->dialog;
}

/* ---- Windows Security -------------------------------------------------------------- */

typedef enum choice {
    CHOICE_LOCK,
    CHOICE_LOGOFF,
    CHOICE_SHUTDOWN,
    CHOICE_PASSWORD,
    CHOICE_TASKMGR,
    CHOICE_CANCEL,
} choice;

static void on_choice(GtkButton *button, gpointer data) {
    choice selected = (choice)GPOINTER_TO_INT(data);
    GtkWindow *toplevel = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    switch (selected) {
    case CHOICE_LOCK:
        run_own("xd-lock", nullptr);
        break;
    case CHOICE_LOGOFF:
        run_own("xd-shutdown", "--logoff");
        break;
    case CHOICE_SHUTDOWN:
        run_own("xd-shutdown", nullptr);
        break;
    case CHOICE_PASSWORD:
        open_change_password(toplevel); /* Windows Security stays open under it */
        return;
    case CHOICE_TASKMGR:
        run_own("xd-taskmgr", nullptr);
        break;
    case CHOICE_CANCEL:
        break;
    }
    gtk_widget_destroy(GTK_WIDGET(toplevel));
}

static GtkWidget *choice_button(const char *mnemonic, choice selected) {
    GtkWidget *widget = gtk_button_new_with_mnemonic(mnemonic);
    gtk_widget_set_size_request(widget, 116, 23);
    g_signal_connect(widget, "clicked", G_CALLBACK(on_choice), GINT_TO_POINTER(selected));
    return widget;
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *key_event, [[maybe_unused]] gpointer data) {
    if (key_event->keyval == GDK_KEY_Escape) {
        gtk_widget_destroy(widget);
        return TRUE;
    }
    return FALSE;
}

static void open_security(GtkApplication *app) {
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), PRODUCT " Security");
    gtk_window_set_icon_name(GTK_WINDOW(win), "system-lock-screen");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), nullptr);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), outer);
    gtk_box_pack_start(GTK_BOX(outer), banner(), FALSE, FALSE, 0);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(outer), box, TRUE, TRUE, 0);

    GtkWidget *frame = gtk_frame_new("Logon Information");
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(info), 10);
    char *who = logon_name();
    char *line1 = g_strdup_printf("You are logged on as %s.", who);
    GDateTime *when = logon_time();
    /* Windows leaves out the seconds: "10/16/2026 5:26 PM". */
    const char *am = nl_langinfo(AM_STR);
    char *date = g_date_time_format(when, am != nullptr && am[0] != '\0' ? "%x %-l:%M %p"
                                                                        : "%x %H:%M");
    char *line2 = g_strdup_printf("Logon Date: %s", date);
    gtk_box_pack_start(GTK_BOX(info), label(line1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info), label(line2), FALSE, FALSE, 0);
    g_free(line2);
    g_free(date);
    g_date_time_unref(when);
    g_free(line1);
    g_free(who);
    gtk_container_add(GTK_CONTAINER(frame), info);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(box),
        label("Use the Task Manager to close an application that is not responding."), FALSE,
        FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    GtkWidget *lock = choice_button("_Lock Computer", CHOICE_LOCK);
    gtk_grid_attach(GTK_GRID(grid), lock, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), choice_button("Log _Off...", CHOICE_LOGOFF), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), choice_button("_Shut Down...", CHOICE_SHUTDOWN), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), choice_button("_Change Password...", CHOICE_PASSWORD), 0, 1,
                    1, 1);
    gtk_grid_attach(GTK_GRID(grid), choice_button("_Task Manager", CHOICE_TASKMGR), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), choice_button("Cancel", CHOICE_CANCEL), 2, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 4);

    gtk_widget_show_all(win);
    gtk_widget_grab_focus(lock);
}

static void on_activate(GtkApplication *app, [[maybe_unused]] gpointer data) {
    GList *windows = gtk_application_get_windows(app);
    if (windows != nullptr) {
        gtk_window_present(windows->data);
        return;
    }
    open_security(app);
}

static void on_startup([[maybe_unused]] GApplication *app, [[maybe_unused]] gpointer data) {
    xd_gtk_app_follow_theme();
}

int main(int argc, char **argv) {
    bool change_only = argc > 1 && strcmp(argv[1], "--change-password") == 0;
    xd_gtk_app_preinit("org.xeondesktop.Security");
    if (change_only) {
        /* Change Password on its own (Windows 7's Ctrl+Alt+Delete screen,
         * or a Control Panel link). */
        gtk_init(&argc, &argv);
        xd_gtk_app_follow_theme();
        g_signal_connect(open_change_password(nullptr), "destroy", G_CALLBACK(gtk_main_quit),
                         nullptr);
        gtk_main();
        return EXIT_SUCCESS;
    }
    GtkApplication *app = gtk_application_new("org.xeondesktop.Security", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int rc = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return rc;
}
