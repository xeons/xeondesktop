/* SPDX-License-Identifier: MIT
 * The Windows 2000 Start menu layout.
 */
#include <stdlib.h>
#include <unistd.h>

#include "apps.h"
#include <glib.h>

#include "shell.h"

/* Appends s to a shell command line as one single-quoted word. */
static void append_quoted(xd_strbuf *cmd, const char *s) {
    xd_strbuf_append_char(cmd, '\'');
    for (; *s != '\0'; s++) {
        if (*s == '\'')
            xd_strbuf_append(cmd, XD_STR("'\\''"));
        else
            xd_strbuf_append_char(cmd, *s);
    }
    xd_strbuf_append_char(cmd, '\'');
}

char *xd_shell_own_program(const char *program) {
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = self != nullptr ? g_path_get_dirname(self) : nullptr;
    char *local = dir != nullptr ? g_build_filename(dir, program, nullptr) : nullptr;
    g_free(self);
    g_free(dir);
    if (local != nullptr && g_file_test(local, G_FILE_TEST_IS_EXECUTABLE))
        return local;
    g_free(local);
    return g_strdup(program);
}

bool xd_shell_have_program(const char *program) {
    char *path = g_find_program_in_path(program);
    bool found = path != nullptr;
    g_free(path);
    return found;
}

/* Command line for one of our own programs: next to xd-shell if it's
 * there (a build tree), else from PATH. */
const char *xd_shell_own_command(xd_shell *shell, const char *program, const char *extra) {
    xd_strbuf cmd = {0};
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    bool local = false;
    if (n > 0) {
        self[n] = '\0';
        char *slash = strrchr(self, '/');
        if (slash != nullptr) {
            slash[1] = '\0';
            xd_strbuf path = {0};
            xd_strbuf_appendf(&path, "%s%s", self, program);
            local = access(xd_strbuf_cstr(&path), X_OK) == 0;
            if (local)
                append_quoted(&cmd, xd_strbuf_cstr(&path));
            xd_strbuf_free(&path);
        }
    }
    if (!local)
        xd_strbuf_append(&cmd, xd_str_from_cstr(program));
    /* Dialogs read desktop.ini themselves; only pass on our own
     * command-line overrides. */
    if (shell->cli_style) {
        xd_strbuf_append(&cmd, XD_STR(" --style "));
        xd_strbuf_append(&cmd, xd_str_from_cstr(xd_visual_style_name(shell->style)));
    }
    if (shell->scheme_path != nullptr) {
        xd_strbuf_append(&cmd, XD_STR(" --scheme "));
        append_quoted(&cmd, shell->scheme_path);
    }
    if (extra != nullptr) {
        xd_strbuf_append_char(&cmd, ' ');
        xd_strbuf_append(&cmd, xd_str_from_cstr(extra));
    }
    /* Menus ask for these every time they open: keep one copy of each. */
    if (shell->own_commands == nullptr)
        shell->own_commands = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    const char *text = xd_strbuf_cstr(&cmd);
    char *s = g_hash_table_lookup(shell->own_commands, text);
    if (s == nullptr) {
        s = g_strdup(text);
        g_hash_table_add(shell->own_commands, s);
    }
    xd_strbuf_free(&cmd);
    return s;
}

/* Programs: one submenu per folder, apps sorted by name inside. */
xd_menu_model *xd_start_programs_model(xd_shell *shell, xd_menu_tree *tree) {
    xd_menu_model *programs = xd_menu_model_new(tree);
    const xd_app_list *apps = &shell->apps;

    for (size_t i = 0; i < apps->count;) {
        const char *folder = apps->items[i].folder;
        xd_menu_item *f = xd_menu_add(tree, programs, XD_MENU_SUBMENU, folder,
                                      apps->items[i].folder_icon);
        f->fallback_icon = "folder";
        f->submenu = xd_menu_model_new(tree);
        for (; i < apps->count && strcmp(apps->items[i].folder, folder) == 0; i++) {
            const xd_app *app = &apps->items[i];
            xd_menu_item *it =
                xd_menu_add(tree, f->submenu, XD_MENU_ACTION, app->name, app->icon);
            it->app = app;
        }
    }
    if (programs->count == 0)
        xd_menu_add(tree, programs, XD_MENU_ACTION, "(Empty)", nullptr)->disabled = true;
    return programs;
}

static xd_menu_item *add_disabled(xd_menu_tree *tree, xd_menu_model *m, xd_menu_kind kind,
                                  const char *label, const char *icon) {
    xd_menu_item *it = xd_menu_add(tree, m, kind, label, icon);
    it->disabled = true;
    return it;
}

void xd_start_menu_build(xd_shell *shell) {
    xd_menu_tree *tree = &shell->menu_tree;
    xd_menu_model *root = xd_menu_model_new(tree);
    root->large = true;
    root->banner_product = "XeonDesktop";
    root->banner_edition = "Professional";

    xd_menu_item *programs =
        xd_menu_add(tree, root, XD_MENU_SUBMENU, "Programs", "applications-other");
    programs->fallback_icon = "folder";
    programs->submenu = xd_start_programs_model(shell, tree);

    xd_menu_item *docs =
        xd_menu_add(tree, root, XD_MENU_SUBMENU, "Documents", "folder-documents");
    docs->fallback_icon = "folder";
    docs->submenu = xd_menu_model_new(tree);
    xd_menu_item *mydocs =
        xd_menu_add(tree, docs->submenu, XD_MENU_ACTION, "My Documents", "folder-documents");
    mydocs->command = xd_shell_own_command(
        shell, "xd-files", "\"$(xdg-user-dir DOCUMENTS 2>/dev/null || echo \"$HOME/Documents\")\"");
    xd_menu_item *home = xd_menu_add(tree, docs->submenu, XD_MENU_ACTION, "Home Folder",
                                     "user-home");
    home->command = xd_shell_own_command(shell, "xd-files", "\"$HOME\"");

    /* Not implemented yet: shown, but greyed out like unavailable items in
     * Windows. They light up as Control Panel and Help land. */
    xd_menu_item *settings =
        xd_menu_add(tree, root, XD_MENU_SUBMENU, "Settings", "preferences-system");
    settings->submenu = xd_menu_model_new(tree);
    /* "Expand Control Panel" style: a cascading list of applets. */
    xd_menu_item *control =
        xd_menu_add(tree, settings->submenu, XD_MENU_SUBMENU, "Control Panel", "preferences-system");
    control->submenu = xd_menu_model_new(tree);
    xd_menu_item *datetime = xd_menu_add(tree, control->submenu, XD_MENU_ACTION, "Date/Time",
                                         "preferences-system-time");
    datetime->command = xd_shell_own_command(shell, "xd-timedate", nullptr);
    xd_menu_item *display = xd_menu_add(tree, control->submenu, XD_MENU_ACTION, "Display",
                                        "preferences-desktop-display");
    display->fallback_icon = "video-display";
    display->command = xd_shell_own_command(shell, "xd-desk", nullptr);
    xd_menu_item *system = xd_menu_add(tree, control->submenu, XD_MENU_ACTION, "System",
                                       "computer");
    system->command = xd_shell_own_command(shell, "xd-sysdm", nullptr);
    add_disabled(tree, settings->submenu, XD_MENU_ACTION, "Network and Dial-up Connections",
                 "network-workgroup");
    add_disabled(tree, settings->submenu, XD_MENU_ACTION, "Printers", "printer");
    xd_menu_item *taskbar = xd_menu_add(tree, settings->submenu, XD_MENU_ACTION,
                                        "Taskbar & Start Menu...", "preferences-desktop");
    taskbar->command = xd_shell_own_command(shell, "xd-taskbar", nullptr);

    xd_menu_item *search = xd_menu_add(tree, root, XD_MENU_SUBMENU, "Search", "system-search");
    search->submenu = xd_menu_model_new(tree);
    add_disabled(tree, search->submenu, XD_MENU_ACTION, "For Files or Folders...",
                 "system-search");

    add_disabled(tree, root, XD_MENU_ACTION, "Help", "help-browser");
    xd_menu_item *run = xd_menu_add(tree, root, XD_MENU_ACTION, "Run...", "system-run");
    run->command = xd_shell_own_command(shell, "xd-run", nullptr);
    xd_menu_add(tree, root, XD_MENU_SEPARATOR, nullptr, nullptr);

    const char *user = getenv("USER");
    xd_strbuf logoff = {0};
    xd_strbuf_appendf(&logoff, "Log Off %s...", user != nullptr ? user : "");
    xd_menu_item *logoff_item =
        xd_menu_add(tree, root, XD_MENU_ACTION, xd_strbuf_cstr(&logoff), "system-log-out");
    logoff_item->command = xd_shell_own_command(shell, "xd-shutdown", "--logoff");
    xd_strbuf_free(&logoff);
    xd_menu_item *shutdown =
        xd_menu_add(tree, root, XD_MENU_ACTION, "Shut Down...", "system-shutdown");
    shutdown->command = xd_shell_own_command(shell, "xd-shutdown", nullptr);

    shell->start_model = root;
}
