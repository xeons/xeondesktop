/* SPDX-License-Identifier: MIT
 * The Windows 2000 Start menu layout.
 */
#include <stdlib.h>

#include "apps.h"
#include "shell.h"

/* Command line that runs an app, through a terminal if it asks for one. */
static const char *app_command(xd_shell *shell, const xd_app *app) {
    if (!app->terminal)
        return app->exec;
    xd_strbuf cmd = {0};
    xd_strbuf_appendf(&cmd, "%s -e %s", shell->terminal, app->exec);
    const char *s = xd_str_arena_cstr(&shell->menu_tree.arena, xd_strbuf_view(&cmd));
    xd_strbuf_free(&cmd);
    return s;
}

/* Programs: one submenu per folder, apps sorted by name inside. */
static xd_menu_model *build_programs(xd_shell *shell) {
    xd_menu_tree *tree = &shell->menu_tree;
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
            it->command = app_command(shell, app);
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
    programs->submenu = build_programs(shell);

    xd_menu_item *docs =
        xd_menu_add(tree, root, XD_MENU_SUBMENU, "Documents", "folder-documents");
    docs->fallback_icon = "folder";
    docs->submenu = xd_menu_model_new(tree);
    xd_menu_item *mydocs =
        xd_menu_add(tree, docs->submenu, XD_MENU_ACTION, "My Documents", "folder-documents");
    mydocs->command = "xdg-open \"${XDG_DOCUMENTS_DIR:-$HOME/Documents}\"";
    xd_menu_item *home = xd_menu_add(tree, docs->submenu, XD_MENU_ACTION, "Home Folder",
                                     "user-home");
    home->command = "xdg-open \"$HOME\"";

    /* Not implemented yet: shown, but greyed out like unavailable items in
     * Windows. They light up as Control Panel and the dialogs land. */
    xd_menu_item *settings =
        xd_menu_add(tree, root, XD_MENU_SUBMENU, "Settings", "preferences-system");
    settings->submenu = xd_menu_model_new(tree);
    add_disabled(tree, settings->submenu, XD_MENU_ACTION, "Control Panel",
                 "preferences-system");
    add_disabled(tree, settings->submenu, XD_MENU_ACTION, "Network and Dial-up Connections",
                 "network-workgroup");
    add_disabled(tree, settings->submenu, XD_MENU_ACTION, "Printers", "printer");
    add_disabled(tree, settings->submenu, XD_MENU_ACTION, "Taskbar & Start Menu...",
                 "preferences-desktop");

    xd_menu_item *search = xd_menu_add(tree, root, XD_MENU_SUBMENU, "Search", "system-search");
    search->submenu = xd_menu_model_new(tree);
    add_disabled(tree, search->submenu, XD_MENU_ACTION, "For Files or Folders...",
                 "system-search");

    add_disabled(tree, root, XD_MENU_ACTION, "Help", "help-browser");
    add_disabled(tree, root, XD_MENU_ACTION, "Run...", "system-run");
    xd_menu_add(tree, root, XD_MENU_SEPARATOR, nullptr, nullptr);

    const char *user = getenv("USER");
    xd_strbuf logoff = {0};
    xd_strbuf_appendf(&logoff, "Log Off %s...", user != nullptr ? user : "");
    add_disabled(tree, root, XD_MENU_ACTION, xd_strbuf_cstr(&logoff), "system-log-out");
    xd_strbuf_free(&logoff);
    add_disabled(tree, root, XD_MENU_ACTION, "Shut Down...", "system-shutdown");

    shell->start_model = root;
}
