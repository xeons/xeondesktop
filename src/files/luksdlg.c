/* SPDX-License-Identifier: MIT
 * Passphrase boxes for encrypted volumes. See luksdlg.h.
 */
#include "luksdlg.h"

#include "../gtkapp/gtkapp.h"
#include "luks.h"

static GtkWidget *wrapped(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 44);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    return label;
}

static GtkWidget *secret_entry(void) {
    GtkWidget *text_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(text_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(text_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(text_entry), TRUE);
    gtk_widget_set_hexpand(text_entry, TRUE);
    return text_entry;
}

static GtkWidget *row_label(const char *text, GtkWidget *entry) {
    GtkWidget *label = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
    return label;
}

/* An OK/Cancel box with the key icon left of `text`; the rows go in the
 * returned grid. */
static GtkWidget *box_new(GtkWindow *parent, const char *title, const char *text,
                          GtkWidget **grid) {
    GtkWidget *dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "dialog-password");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
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
    gtk_box_pack_start(GTK_BOX(top), wrapped(text), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
    *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(*grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(*grid), 12);
    gtk_box_pack_start(GTK_BOX(box), *grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    return dlg;
}

/* The entry's text, then the entry emptied, so the passphrase isn't
 * left in its buffer. */
static char *take_text(GtkWidget *entry) {
    char *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    return text;
}

char *xd_luks_ask_passphrase(GtkWindow *parent, const char *name, gboolean retry,
                             gboolean *remember) {
    char *title = g_strdup_printf("Unlock %s", name);
    char *text = g_strdup_printf(retry ? "The passphrase is incorrect. Type it again to unlock "
                                         "%s."
                                       : "%s is encrypted. Type its passphrase to unlock it.",
                                 name);
    GtkWidget *grid = nullptr;
    GtkWidget *dlg = box_new(parent, title, text, &grid);
    g_free(title);
    g_free(text);
    GtkWidget *pass = secret_entry();
    gtk_grid_attach(GTK_GRID(grid), row_label("_Passphrase:", pass), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pass, 1, 0, 1, 1);
    GtkWidget *keep = nullptr;
    if (remember != nullptr) {
        /* As XP's Connect to box offers for a password. */
        keep = gtk_check_button_new_with_mnemonic("_Remember my passphrase");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(keep), *remember);
        gtk_widget_set_tooltip_text(keep, "The passphrase is kept in your keyring, and the "
                                          "volume is unlocked with it from then on without "
                                          "asking.");
        gtk_grid_attach(GTK_GRID(grid), keep, 1, 1, 1, 1);
    }
    gtk_widget_show_all(dlg);
    gtk_widget_grab_focus(pass);
    char *out = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        out = take_text(pass);
        if (keep != nullptr)
            *remember = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(keep));
    } else {
        gtk_entry_set_text(GTK_ENTRY(pass), "");
    }
    gtk_widget_destroy(dlg);
    return out;
}

gboolean xd_luks_ask_change(GtkWindow *parent, const char *name, char **old,
                            char **new_passphrase) {
    char *title = g_strdup_printf("Change Passphrase for %s", name);
    char *text = g_strdup_printf("Type the current passphrase for %s, then the new one twice. "
                                 "Keep the new one somewhere safe: without it, nobody can read "
                                 "what's on the volume.",
                                 name);
    GtkWidget *grid = nullptr;
    GtkWidget *dlg = box_new(parent, title, text, &grid);
    g_free(text);
    GtkWidget *entries[3];
    static const char *const labels[] = {"_Old passphrase:", "_New passphrase:",
                                         "_Confirm new passphrase:"};
    for (int i = 0; i < 3; i++) {
        entries[i] = secret_entry();
        gtk_grid_attach(GTK_GRID(grid), row_label(labels[i], entries[i]), 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), entries[i], 1, i, 1, 1);
    }
    gtk_widget_show_all(dlg);
    gtk_widget_grab_focus(entries[0]);
    gboolean ok = FALSE;
    while (!ok && gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *why = xd_luks_passphrase_problem(gtk_entry_get_text(GTK_ENTRY(entries[1])),
                                                     gtk_entry_get_text(GTK_ENTRY(entries[2])));
        if (why != nullptr) {
            xd_message_box(GTK_WINDOW(dlg), title, XD_MB_ERROR, XD_MB_OK, FALSE, why);
            /* As XP's Change Password: the new ones again. */
            gtk_entry_set_text(GTK_ENTRY(entries[1]), "");
            gtk_entry_set_text(GTK_ENTRY(entries[2]), "");
            gtk_widget_grab_focus(entries[1]);
            continue;
        }
        *old = take_text(entries[0]);
        *new_passphrase = take_text(entries[1]);
        ok = TRUE;
    }
    for (int i = 0; i < 3; i++)
        gtk_entry_set_text(GTK_ENTRY(entries[i]), "");
    g_free(title);
    gtk_widget_destroy(dlg);
    return ok;
}
