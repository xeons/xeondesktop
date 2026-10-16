/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "../settings/gtktheme.h"
#include "../theme/draw.h"
#include "gtkapp.h"

void xd_gtk_app_preinit(void) {
    /* Before GTK loads fonts, so the theme's Tahoma resolves. */
    xd_fonts_init();
    /* GTK_THEME pins the theme for the life of the process; our own apps
     * set it through GtkSettings instead so they can reload it. */
    g_unsetenv("GTK_THEME");
}

static void reload_theme(void) {
    GtkSettings *s = gtk_settings_get_default();
    if (s == nullptr)
        return;
    /* GTK re-reads a theme's CSS when the theme name changes. */
    g_object_set(s, "gtk-theme-name", "Adwaita", nullptr);
    g_object_set(s, "gtk-theme-name", XD_GTK_THEME_NAME, nullptr);
}

static void on_css_changed([[maybe_unused]] GFileMonitor *m, [[maybe_unused]] GFile *f,
                           [[maybe_unused]] GFile *other, GFileMonitorEvent ev,
                           [[maybe_unused]] gpointer data) {
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_CREATED ||
        ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN)
        reload_theme();
}

static gboolean theme_installed(const char *name) {
    GtkIconTheme *t = gtk_icon_theme_get_default();
    gchar **paths = nullptr;
    gint n = 0;
    gtk_icon_theme_get_search_path(t, &paths, &n);
    gboolean found = FALSE;
    for (gint i = 0; i < n && !found; i++) {
        char *index = g_build_filename(paths[i], name, "index.theme", nullptr);
        found = g_file_test(index, G_FILE_TEST_IS_REGULAR);
        g_free(index);
    }
    g_strfreev(paths);
    return found;
}

void xd_gtk_app_follow_theme(void) {
    GtkSettings *s = gtk_settings_get_default();
    if (s == nullptr)
        return;
    xd_strbuf path = {0};
    xd_gtk_theme_css_path(&path);
    if (g_file_test(xd_strbuf_cstr(&path), G_FILE_TEST_IS_REGULAR))
        g_object_set(s, "gtk-theme-name", XD_GTK_THEME_NAME, nullptr);

    /* Same icon theme as the shell: XD_ICON_THEME, else breeze, else
     * GTK's default. */
    const char *icons = getenv("XD_ICON_THEME");
    if (icons == nullptr || !theme_installed(icons))
        icons = theme_installed("breeze") ? "breeze" : nullptr;
    if (icons != nullptr)
        g_object_set(s, "gtk-icon-theme-name", icons, nullptr);

    /* Watch the directory: the theme is replaced by rename. The monitor
     * lives as long as the process. */
    GFile *css = g_file_new_for_path(xd_strbuf_cstr(&path));
    GFileMonitor *mon = g_file_monitor_file(css, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (mon != nullptr)
        g_signal_connect(mon, "changed", G_CALLBACK(on_css_changed), nullptr);
    g_object_unref(css);
    xd_strbuf_free(&path);
}

static void on_mb_button(GtkButton *b, gpointer dialog) {
    gtk_dialog_response(GTK_DIALOG(dialog),
                        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "response")));
}

static GtkWidget *mb_button(GtkWidget *dialog, GtkWidget *row, const char *label, int response) {
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_size_request(b, 75, -1);
    gtk_widget_set_can_default(b, TRUE);
    g_object_set_data(G_OBJECT(b), "response", GINT_TO_POINTER(response));
    g_signal_connect(b, "clicked", G_CALLBACK(on_mb_button), dialog);
    gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    return b;
}

int xd_message_box(GtkWindow *parent, const char *title, xd_mb_icon icon, xd_mb_buttons buttons,
                   gboolean default_no, const char *text) {
    static const char *const icons[] = {
        [XD_MB_ERROR] = "dialog-error",
        [XD_MB_WARNING] = "dialog-warning",
        [XD_MB_INFO] = "dialog-information",
        [XD_MB_QUESTION] = "dialog-question",
    };
    GtkWidget *dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *msg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *image = gtk_image_new_from_icon_name(icons[icon], GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
    gtk_widget_set_valign(image, GTK_ALIGN_START);
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(msg), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(msg), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), msg, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    GtkWidget *focus;
    if (buttons == XD_MB_YES_NO) {
        GtkWidget *yes = mb_button(dlg, row, "_Yes", GTK_RESPONSE_YES);
        GtkWidget *no = mb_button(dlg, row, "_No", GTK_RESPONSE_NO);
        focus = default_no ? no : yes;
    } else {
        focus = mb_button(dlg, row, "OK", GTK_RESPONSE_OK);
    }
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    gtk_widget_grab_default(focus);
    gtk_widget_grab_focus(focus);

    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (buttons == XD_MB_YES_NO)
        return r == GTK_RESPONSE_YES ? GTK_RESPONSE_YES : GTK_RESPONSE_NO;
    return GTK_RESPONSE_OK;
}

static gboolean on_ctrl_tab([[maybe_unused]] GtkWidget *w, GdkEventKey *ev, gpointer notebook) {
    if (!(ev->state & GDK_CONTROL_MASK))
        return FALSE;
    gboolean back = ev->keyval == GDK_KEY_ISO_Left_Tab ||
                    (ev->keyval == GDK_KEY_Tab && (ev->state & GDK_SHIFT_MASK));
    if (ev->keyval != GDK_KEY_Tab && ev->keyval != GDK_KEY_ISO_Left_Tab)
        return FALSE;
    GtkNotebook *nb = notebook;
    int n = gtk_notebook_get_n_pages(nb);
    if (n > 0) {
        int cur = gtk_notebook_get_current_page(nb);
        gtk_notebook_set_current_page(nb, (cur + (back ? n - 1 : 1)) % n);
    }
    return TRUE;
}

void xd_gtk_notebook_ctrl_tab(GtkWidget *window, GtkNotebook *notebook) {
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_ctrl_tab), notebook);
}
