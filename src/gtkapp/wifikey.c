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
static bool plausible(const char *key) {
    size_t count = strlen(key);
    return count == 5 || count == 13 || (count >= 8 && count <= 64);
}

static void on_changed([[maybe_unused]] GtkEditable *editable, gpointer data) {
    prompt *wifi_prompt = data;
    const char *key = gtk_entry_get_text(GTK_ENTRY(wifi_prompt->key));
    const char *confirmation = gtk_entry_get_text(GTK_ENTRY(wifi_prompt->confirm));
    gtk_widget_set_sensitive(wifi_prompt->connect,
                             plausible(key) && strcmp(key, confirmation) == 0);
}

static GtkWidget *key_entry(prompt *wifi_prompt) {
    GtkWidget *key_field = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(key_field), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(key_field), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(key_field), TRUE);
    gtk_widget_set_hexpand(key_field, TRUE);
    g_signal_connect(key_field, "changed", G_CALLBACK(on_changed), wifi_prompt);
    return key_field;
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

    prompt wifi_prompt = {0};
    wifi_prompt.dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(wifi_prompt.dialog), "Wireless Network Connection");
    gtk_window_set_icon_name(GTK_WINDOW(wifi_prompt.dialog), "network-wireless");
    gtk_window_set_resizable(GTK_WINDOW(wifi_prompt.dialog), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(wifi_prompt.dialog), TRUE);
    wifi_prompt.connect =
        gtk_dialog_add_button(GTK_DIALOG(wifi_prompt.dialog), "C_onnect", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(wifi_prompt.dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(wifi_prompt.dialog), GTK_RESPONSE_OK);
    gtk_widget_set_sensitive(wifi_prompt.connect, FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(wifi_prompt.dialog))), box,
                       TRUE, TRUE, 0);
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
    wifi_prompt.key = key_entry(&wifi_prompt);
    wifi_prompt.confirm = key_entry(&wifi_prompt);
    GtkWidget *l1 = gtk_label_new_with_mnemonic("Network _key:");
    GtkWidget *l2 = gtk_label_new_with_mnemonic("Con_firm network key:");
    gtk_label_set_xalign(GTK_LABEL(l1), 0);
    gtk_label_set_xalign(GTK_LABEL(l2), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l1), wifi_prompt.key);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l2), wifi_prompt.confirm);
    gtk_grid_attach(GTK_GRID(grid), l1, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), wifi_prompt.key, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l2, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), wifi_prompt.confirm, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    gtk_widget_show_all(wifi_prompt.dialog);
    gtk_widget_grab_focus(wifi_prompt.key);
    int response = gtk_dialog_run(GTK_DIALOG(wifi_prompt.dialog));
    if (response != GTK_RESPONSE_OK)
        return 1;
    printf("%s\n", gtk_entry_get_text(GTK_ENTRY(wifi_prompt.key)));
    /* Now: a sanitizer's leak report at exit ends us without flushing. */
    fflush(stdout);
    return 0;
}
