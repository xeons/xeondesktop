/* SPDX-License-Identifier: MIT
 * xd-choose-file: a GTK file chooser for the libxdui dialogs, which have
 * none of their own. Prints the chosen path on stdout; exits 1 if the user
 * cancels.
 *
 *   xd-choose-file [--title TEXT] [--images] [--folder DIR]
 */
#include <stdio.h>
#include <string.h>

#include "gtkapp.h"

int main(int argc, char **argv) {
    const char *title = "Browse";
    const char *folder = nullptr;
    bool images = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--title") == 0 && i + 1 < argc)
            title = argv[++i];
        else if (strcmp(argv[i], "--folder") == 0 && i + 1 < argc)
            folder = argv[++i];
        else if (strcmp(argv[i], "--images") == 0)
            images = true;
    }
    xd_gtk_app_preinit();
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();

    GtkWidget *dialog = gtk_file_chooser_dialog_new(title, nullptr, GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL, "_Open",
                                                    GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    if (images) {
        GtkFileFilter *pictures = gtk_file_filter_new();
        gtk_file_filter_set_name(pictures, "Pictures (*.bmp;*.jpg;*.png;*.webp)");
        gtk_file_filter_add_pixbuf_formats(pictures);
        gtk_file_chooser_add_filter(chooser, pictures);
        GtkFileFilter *all = gtk_file_filter_new();
        gtk_file_filter_set_name(all, "All Files (*.*)");
        gtk_file_filter_add_pattern(all, "*");
        gtk_file_chooser_add_filter(chooser, all);
    }
    if (folder != nullptr)
        gtk_file_chooser_set_current_folder(chooser, folder);

    int status = 1;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(chooser);
        if (path != nullptr) {
            printf("%s\n", path);
            fflush(stdout);
            g_free(path);
            status = 0;
        }
    }
    gtk_widget_destroy(dialog);
    return status;
}
