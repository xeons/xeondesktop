/* SPDX-License-Identifier: MIT
 * xd-choose-file: a GTK file chooser for the libxdui dialogs, which have
 * none of their own. Prints the chosen path on stdout; exits 1 if the user
 * cancels.
 *
 *   xd-choose-file [--title TEXT] [--images | --programs | --styles] [--folder DIR]
 *
 * $XD_CHOOSE_FILE_ANSWER, for tests: the answer, printed without asking.
 */
#include <stdio.h>
#include <string.h>

#include "gtkapp.h"

static gboolean is_program(const GtkFileFilterInfo *info, [[maybe_unused]] gpointer data) {
    return info->filename != nullptr &&
           (g_file_test(info->filename, G_FILE_TEST_IS_DIR) ||
            g_file_test(info->filename, G_FILE_TEST_IS_EXECUTABLE));
}

int main(int argc, char **argv) {
    const char *title = "Browse";
    const char *folder = nullptr;
    bool images = false, programs = false, styles = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--title") == 0 && i + 1 < argc)
            title = argv[++i];
        else if (strcmp(argv[i], "--folder") == 0 && i + 1 < argc)
            folder = argv[++i];
        else if (strcmp(argv[i], "--images") == 0)
            images = true;
        else if (strcmp(argv[i], "--programs") == 0)
            programs = true;
        else if (strcmp(argv[i], "--styles") == 0)
            styles = true;
    }
    const char *answer = g_getenv("XD_CHOOSE_FILE_ANSWER");
    if (answer != nullptr) {
        printf("%s\n", answer);
        return 0;
    }
    xd_gtk_app_preinit("org.xeondesktop.ChooseFile");
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
    if (programs) {
        /* Run's Browse: programs (anything executable) first, as XP's
         * "Programs" filter, then everything. */
        GtkFileFilter *progs = gtk_file_filter_new();
        gtk_file_filter_set_name(progs, "Programs");
        gtk_file_filter_add_custom(progs, GTK_FILE_FILTER_FILENAME, is_program, nullptr,
                                   nullptr);
        gtk_file_filter_add_mime_type(progs, "application/x-desktop");
        gtk_file_chooser_add_filter(chooser, progs);
        GtkFileFilter *all = gtk_file_filter_new();
        gtk_file_filter_set_name(all, "All Files");
        gtk_file_filter_add_pattern(all, "*");
        gtk_file_chooser_add_filter(chooser, all);
    }
    if (styles) {
        /* Display Properties' visual styles: Windows' (converted when
         * chosen) and our own packages. */
        GtkFileFilter *visual = gtk_file_filter_new();
        gtk_file_filter_set_name(visual, "Visual Styles (*.msstyles;*.theme;*.xdtheme)");
        gtk_file_filter_add_pattern(visual, "*.[Mm][Ss][Ss][Tt][Yy][Ll][Ee][Ss]");
        gtk_file_filter_add_pattern(visual, "*.[Tt][Hh][Ee][Mm][Ee]");
        gtk_file_filter_add_pattern(visual, "*.xdtheme");
        gtk_file_chooser_add_filter(chooser, visual);
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
