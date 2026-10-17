/* SPDX-License-Identifier: MIT */
#include <cups/cups.h>
#include <gtk/gtk.h>
#include <string.h>

#include "auth.h"

static constexpr int MAX_TRIES = 3;

static int tries;
/* libcups reads the password after the callback returns, so it lives
 * here until the next operation (which wipes it). */
static char password[256];

static GtkWindow *active_window(void) {
    GList *all = gtk_window_list_toplevels();
    GtkWindow *found = nullptr;
    for (GList *l = all; l != nullptr && found == nullptr; l = l->next) {
        if (gtk_window_is_active(l->data))
            found = l->data;
    }
    g_list_free(all);
    return found;
}

static GtkWidget *grid_label(const char *mnemonic, GtkWidget *target) {
    GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
    return l;
}

static const char *ask([[maybe_unused]] const char *prompt, http_t *http,
                       [[maybe_unused]] const char *method,
                       [[maybe_unused]] const char *resource, [[maybe_unused]] void *data) {
    explicit_bzero(password, sizeof(password));
    if (++tries > MAX_TRIES)
        return nullptr;
    char host[256] = "";
    if (http != nullptr)
        httpGetHostname(http, host, (int)sizeof(host));
    /* A local socket's "host" is its path: this computer. */
    const char *server = host[0] == '\0' || host[0] == '/' ? "localhost" : host;

    GtkWidget *dlg = gtk_dialog_new();
    char *title = g_strdup_printf("Connect to %s", server);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    g_free(title);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWindow *parent = active_window();
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-password", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    char *text = g_strdup_printf(
        tries > 1 ? "The user name or password is incorrect. Type them again to connect to %s."
                  : "Connecting to %s. Changing printers needs the user name and password of "
                    "a printer administrator.",
        server);
    GtkWidget *msg = gtk_label_new(text);
    g_free(text);
    gtk_label_set_line_wrap(GTK_LABEL(msg), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(msg), 44);
    gtk_label_set_xalign(GTK_LABEL(msg), 0);
    gtk_box_pack_start(GTK_BOX(top), msg, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    GtkWidget *user = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(user), cupsUser());
    gtk_entry_set_activates_default(GTK_ENTRY(user), TRUE);
    gtk_widget_set_hexpand(user, TRUE);
    GtkWidget *pass = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(pass), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(pass), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(pass), TRUE);
    gtk_grid_attach(GTK_GRID(grid), grid_label("_User name:", user), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), user, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("_Password:", pass), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pass, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    gtk_widget_grab_focus(pass);

    const char *result = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(user));
        if (name[0] != '\0')
            cupsSetUser(name);
        g_strlcpy(password, gtk_entry_get_text(GTK_ENTRY(pass)), sizeof(password));
        result = password;
    }
    /* Not left in the entry's buffer either. */
    gtk_entry_set_text(GTK_ENTRY(pass), "");
    gtk_widget_destroy(dlg);
    if (result == nullptr)
        tries = MAX_TRIES;
    return result;
}

void xd_cups_auth_init(void) { cupsSetPasswordCB2(ask, nullptr); }

void xd_cups_auth_reset(void) {
    tries = 0;
    explicit_bzero(password, sizeof(password));
}
