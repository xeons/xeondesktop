/* SPDX-License-Identifier: MIT
 * xd-wifi-key: XP's Wireless Network Connection prompt for a secured
 * network's key, for the shell's network icon. Prints the key on stdout;
 * exits 1 if the user cancels.
 *
 *   xd-wifi-key SSID
 */
#include <stdio.h>
#include <string.h>

#include "gtkapp.h"

typedef struct prompt {
    GtkWidget *dialog, *key, *confirm, *connect;
} prompt;

/* WPA keys are 8-63 characters (or 64 hex digits); WEP keys 5 or 13. */
static bool plausible(const char *k) {
    size_t n = strlen(k);
    return n == 5 || n == 13 || (n >= 8 && n <= 64);
}

static void on_changed([[maybe_unused]] GtkEditable *e, gpointer data) {
    prompt *p = data;
    const char *a = gtk_entry_get_text(GTK_ENTRY(p->key));
    const char *b = gtk_entry_get_text(GTK_ENTRY(p->confirm));
    gtk_widget_set_sensitive(p->connect, plausible(a) && strcmp(a, b) == 0);
}

static GtkWidget *key_entry(prompt *p) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_widget_set_hexpand(e, TRUE);
    g_signal_connect(e, "changed", G_CALLBACK(on_changed), p);
    return e;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: xd-wifi-key SSID\n");
        return 2;
    }
    const char *ssid = argv[1];
    xd_gtk_app_preinit("org.xeondesktop.WifiKey");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();

    prompt p = {0};
    p.dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(p.dialog), "Wireless Network Connection");
    gtk_window_set_icon_name(GTK_WINDOW(p.dialog), "network-wireless");
    gtk_window_set_resizable(GTK_WINDOW(p.dialog), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(p.dialog), TRUE);
    p.connect = gtk_dialog_add_button(GTK_DIALOG(p.dialog), "C_onnect", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(p.dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(p.dialog), GTK_RESPONSE_OK);
    gtk_widget_set_sensitive(p.connect, FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p.dialog))), box, TRUE,
                       TRUE, 0);
    char *text = g_strdup_printf(
        "The network '%s' requires a network key (also called a WEP key or WPA key). A "
        "network key helps prevent unknown intruders from connecting to this network.",
        ssid);
    GtkWidget *intro = gtk_label_new(text);
    g_free(text);
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 52);
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);
    GtkWidget *hint = gtk_label_new("Type the key, and then click Connect.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    p.key = key_entry(&p);
    p.confirm = key_entry(&p);
    GtkWidget *l1 = gtk_label_new_with_mnemonic("Network _key:");
    GtkWidget *l2 = gtk_label_new_with_mnemonic("Con_firm network key:");
    gtk_label_set_xalign(GTK_LABEL(l1), 0);
    gtk_label_set_xalign(GTK_LABEL(l2), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l1), p.key);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l2), p.confirm);
    gtk_grid_attach(GTK_GRID(grid), l1, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p.key, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l2, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p.confirm, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    gtk_widget_show_all(p.dialog);
    gtk_widget_grab_focus(p.key);
    int response = gtk_dialog_run(GTK_DIALOG(p.dialog));
    if (response != GTK_RESPONSE_OK)
        return 1;
    printf("%s\n", gtk_entry_get_text(GTK_ENTRY(p.key)));
    /* Now: a sanitizer's leak report at exit ends us without flushing. */
    fflush(stdout);
    return 0;
}
