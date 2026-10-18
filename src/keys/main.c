/* SPDX-License-Identifier: MIT
 * xd-keys: Passwords and Keys, GNOME's Seahorse as a Control Panel applet
 * (User Accounts; XP's "Stored User Names and Passwords" is its nearest
 * Windows relative, and User Accounts' "Manage my network passwords"
 * opens it).
 *
 *   Passwords  the Secret Service's keyrings (gnome-keyring, KeePassXC...)
 *              through libsecret: lock and unlock them; add, remove and
 *              change passwords, show and copy them (passwords.c)
 *   SSH Keys   the key pairs in ~/.ssh: make, re-protect, delete (to the
 *              Recycle Bin), copy the public key, and load into or take
 *              out of ssh-agent (sshpage.c, sshkeys.c)
 *   PGP Keys   GnuPG's keyring through gpgme: make, import, export,
 *              delete, change expiry dates (gpgpage.c, gpgkeys.c)
 *
 *   xd-keys [--page N]   the Nth tab, from 0 (Passwords)
 *
 * It's also its own askpass: ssh-keygen and ssh-add, run with
 * XD_KEYS_ASKPASS set, run it for passphrases, which it reads from the
 * pipe they share (keyslib.h).
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "keys.h"
#include "keyslib.h"

GtkWidget *keys_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    return l;
}

GtkWidget *keys_scrolled(GtkWidget *child, int width, int height) {
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sc, width, height);
    gtk_container_add(GTK_CONTAINER(sc), child);
    return sc;
}

GtkTreeViewColumn *keys_text_column(GtkWidget *view, const char *title, int column, int width) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    GtkTreeViewColumn *c =
        gtk_tree_view_column_new_with_attributes(title, r, "text", column, nullptr);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_column_set_sort_column_id(c, column);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
    return c;
}

GtkWidget *keys_button(const char *mnemonic, GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new_with_mnemonic(mnemonic);
    gtk_widget_set_size_request(b, 75, -1);
    if (cb != nullptr)
        g_signal_connect_swapped(b, "clicked", cb, data);
    return b;
}

GtkWidget *keys_button_row(GtkWidget *first, ...) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    va_list ap;
    va_start(ap, first);
    for (GtkWidget *b = first; b != nullptr; b = va_arg(ap, GtkWidget *))
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    va_end(ap);
    return row;
}

GtkWidget *keys_grid(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    return grid;
}

void keys_grid_row(GtkWidget *grid, int row, const char *label, GtkWidget *field) {
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_valign(l, GTK_ALIGN_BASELINE);
    if (field != nullptr) {
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), field);
        gtk_widget_set_hexpand(field, TRUE);
        gtk_grid_attach(GTK_GRID(grid), field, 1, row, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
}

GtkWidget *keys_dialog(GtkWindow *parent, const char *title, const char *ok, GtkWidget **box) {
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), title);
    gtk_window_set_transient_for(GTK_WINDOW(d), parent);
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(d), TRUE);
    gtk_dialog_add_button(GTK_DIALOG(d), ok != nullptr ? ok : "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(*box), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), *box, TRUE, TRUE, 0);
    return d;
}

GtkWidget *keys_password_entry(void) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 28);
    return e;
}

void keys_error(GtkWindow *parent, const char *what, const GError *error) {
    char *text = error != nullptr ? g_strdup_printf("%s\n\n%s", what, error->message)
                                  : g_strdup(what);
    xd_message_box(parent, KEYS_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
}

char *keys_ask_passphrase(GtkWindow *parent, const char *title, const char *text) {
    GtkWidget *box = nullptr;
    GtkWidget *d = keys_dialog(parent, title, nullptr, &box);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-password", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(right), keys_label(text), FALSE, FALSE, 0);
    GtkWidget *grid = keys_grid();
    GtkWidget *entry = keys_password_entry();
    keys_grid_row(grid, 0, "_Passphrase:", entry);
    gtk_box_pack_start(GTK_BOX(right), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), right, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    gtk_widget_grab_focus(entry);
    char *pass = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
        pass = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_widget_destroy(d);
    return pass;
}

bool keys_check_match(GtkWindow *parent, const char *title, GtkWidget *entry, GtkWidget *again) {
    if (strcmp(gtk_entry_get_text(GTK_ENTRY(entry)), gtk_entry_get_text(GTK_ENTRY(again))) == 0)
        return true;
    xd_message_box(parent, title, XD_MB_ERROR, XD_MB_OK, FALSE,
                   "The passwords you typed do not match. Type the same password in both "
                   "boxes.");
    gtk_entry_set_text(GTK_ENTRY(again), "");
    gtk_widget_grab_focus(again);
    return false;
}

void keys_copy(const char *text) {
    GtkClipboard *c = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(c, text, -1);
    gtk_clipboard_store(c);
}

void keys_busy(GtkWidget *w, bool busy) {
    GdkWindow *win = gtk_widget_get_window(gtk_widget_get_toplevel(w));
    if (win == nullptr)
        return;
    GdkCursor *c = busy ? gdk_cursor_new_from_name(gdk_window_get_display(win), "wait")
                        : nullptr;
    gdk_window_set_cursor(win, c);
    if (c != nullptr)
        g_object_unref(c);
    /* Show it before the work starts. */
    gdk_display_flush(gdk_window_get_display(win));
}

static void on_response(GtkDialog *d, [[maybe_unused]] int response,
                        [[maybe_unused]] gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(d));
}

int main(int argc, char **argv) {
    /* Run by ssh-keygen or ssh-add as their askpass: no window. */
    if (g_getenv(XD_KEYS_ASKPASS_ENV) != nullptr)
        return xd_keys_askpass_main();

    int page = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
    }
    xd_gtk_app_preinit("org.xeondesktop.Keys");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();

    static keys_app app;
    app.askpass = g_file_read_link("/proc/self/exe", nullptr);
    if (app.askpass == nullptr)
        app.askpass = g_strdup("xd-keys");
    app.ssh_dir = g_build_filename(g_get_home_dir(), ".ssh", nullptr);

    app.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(app.window), KEYS_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(app.window), "dialog-password");
    gtk_window_set_resizable(GTK_WINDOW(app.window), FALSE);
    /* Everything here takes effect at once, as in XP's Stored User Names
     * and Passwords: Close, no OK/Cancel. */
    gtk_dialog_add_button(GTK_DIALOG(app.window), "Close", GTK_RESPONSE_CLOSE);
    gtk_dialog_set_default_response(GTK_DIALOG(app.window), GTK_RESPONSE_CLOSE);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), passwords_page_new(&app),
                             gtk_label_new_with_mnemonic("Pass_words"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), ssh_page_new(&app),
                             gtk_label_new_with_mnemonic("_SSH Keys"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), gpg_page_new(&app),
                             gtk_label_new_with_mnemonic("_PGP Keys"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(app.window))), tabs, TRUE,
                       TRUE, 0);
    xd_gtk_notebook_ctrl_tab(app.window, GTK_NOTEBOOK(tabs));
    g_signal_connect(app.window, "response", G_CALLBACK(on_response), nullptr);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(app.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs),
                                  XD_CLAMP(page, 0, gtk_notebook_get_n_pages(GTK_NOTEBOOK(tabs)) - 1));
    gtk_main();
    g_free(app.askpass);
    g_free(app.ssh_dir);
    return EXIT_SUCCESS;
}
