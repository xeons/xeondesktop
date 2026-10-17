/* SPDX-License-Identifier: MIT
 * xd-control: Windows' `control`. Without arguments it opens the Control
 * Panel folder (control:/// in xd-files); `xd-control NAME` opens one
 * applet by its short name, as `control desk.cpl` does: "desk" (or
 * "desk.cpl", "xd-desk", "Display") opens Display Properties.
 */
#include <gio/gdesktopappinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../files/control.h"

/* xd-files next to us (a build tree), else from PATH. */
static char *files_program(void) {
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = self != nullptr ? g_path_get_dirname(self) : nullptr;
    char *local = dir != nullptr ? g_build_filename(dir, "xd-files", nullptr) : nullptr;
    g_free(self);
    g_free(dir);
    if (local != nullptr && g_file_test(local, G_FILE_TEST_IS_EXECUTABLE))
        return local;
    g_free(local);
    return g_strdup("xd-files");
}

int main(int argc, char **argv) {
    /* The shell passes its own --style/--scheme overrides to the programs
     * it starts; the folder and applets read desktop.ini themselves. */
    char *style = nullptr;
    char *scheme = nullptr;
    GOptionEntry entries[] = {
        {"style", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &style, nullptr, nullptr},
        {"scheme", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_FILENAME, &scheme, nullptr, nullptr},
        G_OPTION_ENTRY_NULL,
    };
    GOptionContext *opts = g_option_context_new("[APPLET] - open Control Panel or an applet");
    g_option_context_set_summary(opts, "APPLET is a short name such as desk, sysdm, timedate or "
                                       "taskbar (desk.cpl works too).");
    g_option_context_add_main_entries(opts, entries, nullptr);
    GError *err = nullptr;
    gboolean parsed = g_option_context_parse(opts, &argc, &argv, &err);
    g_option_context_free(opts);
    g_free(style);
    g_free(scheme);
    if (!parsed || argc > 2) {
        fprintf(stderr, "xd-control: %s\n", err != nullptr ? err->message : "too many arguments");
        g_clear_error(&err);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        GDesktopAppInfo *info = xd_control_find(argv[1]);
        if (info == nullptr) {
            fprintf(stderr, "xd-control: no Control Panel applet called '%s'\n", argv[1]);
            return EXIT_FAILURE;
        }
        gboolean ok = g_app_info_launch(G_APP_INFO(info), nullptr, nullptr, &err);
        if (!ok) {
            fprintf(stderr, "xd-control: cannot open %s: %s\n",
                    g_app_info_get_name(G_APP_INFO(info)), err->message);
            g_error_free(err);
        }
        g_object_unref(info);
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    char *files = files_program();
    char uri[] = "control:///";
    char *child[] = {files, uri, nullptr};
    execvp(files, child);
    fprintf(stderr, "xd-control: cannot run %s\n", files);
    g_free(files);
    return EXIT_FAILURE;
}
