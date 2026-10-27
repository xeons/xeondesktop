/* SPDX-License-Identifier: MIT
 * The Windows 2000 Start menu layout.
 */
#include <stdlib.h>
#include <unistd.h>

#include "apps.h"
#include <gio/gio.h>
#include <glib.h>

#include "../settings/state.h"
#include "recent.h"
#include "shell.h"

/* Appends s to a shell command line as one single-quoted word. */
static void append_quoted(xd_strbuf *cmd, const char *word) {
    xd_strbuf_append_char(cmd, '\'');
    for (; *word != '\0'; word++) {
        if (*word == '\'')
            xd_strbuf_append(cmd, XD_STR("'\\''"));
        else
            xd_strbuf_append_char(cmd, *word);
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
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    bool local = false;
    if (length > 0) {
        self[length] = '\0';
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
    char *command = g_hash_table_lookup(shell->own_commands, text);
    if (command == nullptr) {
        command = g_strdup(text);
        g_hash_table_add(shell->own_commands, command);
    }
    xd_strbuf_free(&cmd);
    return command;
}

/* Programs: one submenu per folder, apps sorted by name inside. */
xd_menu_model *xd_start_programs_model(xd_shell *shell, xd_menu_tree *tree, bool highlight_new) {
    xd_menu_model *programs = xd_menu_model_new(tree);
    const xd_app_list *apps = &shell->apps;

    for (size_t i = 0; i < apps->count;) {
        const char *folder = apps->items[i].folder;
        xd_menu_item *folder_item =
            xd_menu_add(tree, programs, XD_MENU_SUBMENU, folder, apps->items[i].folder_icon);
        folder_item->fallback_icon = "folder";
        folder_item->submenu = xd_menu_model_new(tree);
        for (; i < apps->count && strcmp(apps->items[i].folder, folder) == 0; i++) {
            const xd_app *app = &apps->items[i];
            xd_menu_item *it =
                xd_menu_add(tree, folder_item->submenu, XD_MENU_ACTION, app->name, app->icon);
            it->app = app;
            /* A new program, and the folder on the way to it, are lit. */
            it->highlight = highlight_new && xd_start_menu_is_new(shell, app->id);
            folder_item->highlight = folder_item->highlight || it->highlight;
        }
    }
    if (programs->count == 0)
        xd_menu_add(tree, programs, XD_MENU_ACTION, "(Empty)", nullptr)->disabled = true;
    return programs;
}

/* ---- newly installed programs ------------------------------------------------- */

/* start-seen: "seconds<TAB>id" for every program, when it was first seen,
 * or 0 for one that was there at the first run or has been opened since.
 * A program is new while its time isn't 0: XP highlights a program from
 * its installation until it's first used. */
static constexpr size_t SEEN_MAX = 8192;

size_t xd_start_menu_note_programs(xd_shell *shell) {
    char **lines = nullptr;
    size_t count = 0;
    xd_state_read_lines("start-seen", &lines, &count, SEEN_MAX);
    bool first_run = count == 0;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal); /* id -> line */
    for (size_t i = 0; i < count; i++) {
        char *tab = strchr(lines[i], '\t');
        if (tab != nullptr)
            g_hash_table_insert(seen, tab + 1, lines[i]);
    }
    if (shell->new_apps == nullptr)
        shell->new_apps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    g_hash_table_remove_all(shell->new_apps);

    /* Only the programs installed now: one removed and installed again is
     * new again. */
    const xd_app_list *apps = &shell->apps;
    char **out = xd_alloc_array(XD_MAX(apps->count, 1), sizeof(char *));
    size_t line_count = 0;
    bool changed = apps->count != count;
    size_t unseen = 0;
    long long now = (long long)(g_get_real_time() / G_USEC_PER_SEC);
    for (size_t i = 0; i < apps->count && line_count < SEEN_MAX; i++) {
        const char *id = apps->items[i].id;
        const char *line = g_hash_table_lookup(seen, id);
        long long when = line != nullptr ? strtoll(line, nullptr, 10) : first_run ? 0 : now;
        changed = changed || line == nullptr;
        if (line == nullptr && !first_run)
            unseen++;
        if (when > 0)
            g_hash_table_add(shell->new_apps, g_strdup(id));
        xd_strbuf entry = {0};
        xd_strbuf_appendf(&entry, "%lld\t%s", when, id);
        out[line_count++] = xd_str_dup_cstr(xd_strbuf_view(&entry));
        xd_strbuf_free(&entry);
    }
    if (changed && !xd_state_write_lines("start-seen", out, line_count))
        fprintf(stderr, "xd-shell: cannot save the programs seen\n");
    xd_free_lines(out, line_count);
    g_hash_table_unref(seen);
    xd_free_lines(lines, count);
    return unseen;
}

bool xd_start_menu_is_new(const xd_shell *shell, const char *id) {
    return shell->new_apps != nullptr && id != nullptr &&
           g_hash_table_contains(shell->new_apps, id);
}

void xd_start_menu_program_opened(const char *id) {
    char **lines = nullptr;
    size_t count = 0;
    xd_state_read_lines("start-seen", &lines, &count, SEEN_MAX);
    bool changed = false;
    for (size_t i = 0; i < count; i++) {
        char *tab = strchr(lines[i], '\t');
        if (tab == nullptr || strcmp(tab + 1, id) != 0 || strtoll(lines[i], nullptr, 10) == 0)
            continue;
        xd_strbuf entry = {0};
        xd_strbuf_appendf(&entry, "0\t%s", id);
        xd_free(lines[i]);
        lines[i] = xd_str_dup_cstr(xd_strbuf_view(&entry));
        xd_strbuf_free(&entry);
        changed = true;
    }
    if (changed && !xd_state_write_lines("start-seen", lines, count))
        fprintf(stderr, "xd-shell: cannot save the programs seen\n");
    xd_free_lines(lines, count);
}

static void reload_if_stale(xd_shell *shell) {
    if (!shell->apps_stale || shell->start_menu != nullptr || shell->start7 != nullptr)
        return;
    shell->apps_stale = false;
    xd_apps_free(&shell->apps);
    xd_apps_load(&shell->apps);
    xd_apps_free(&shell->control_apps);
    xd_apps_load_control_panel(&shell->control_apps);
    /* The Windows 2000 menu's models point into the old lists. */
    xd_menu_tree_free(&shell->menu_tree);
    xd_menu_tree_init(&shell->menu_tree);
    xd_start_menu_build(shell);
    if (xd_start_menu_note_programs(shell) > 0)
        xd_balloon_new_programs(shell);
}

static gboolean reload_idle(gpointer data) {
    xd_shell *shell = data;
    shell->apps_reload = 0;
    reload_if_stale(shell); /* else once the menu closes */
    return G_SOURCE_REMOVE;
}

void xd_start_menu_reload_later(xd_shell *shell) {
    if (shell->apps_stale && shell->apps_reload == 0)
        shell->apps_reload = g_idle_add(reload_idle, shell);
}

/* Reloaded from an idle callback, never at once: a menu item being
 * activated may still point into the lists. */
static void programs_changed([[maybe_unused]] GAppInfoMonitor *monitor, gpointer data) {
    xd_shell *shell = data;
    shell->apps_stale = true;
    xd_start_menu_reload_later(shell);
}

void xd_start_menu_watch_programs(xd_shell *shell) {
    shell->app_monitor = g_app_info_monitor_get();
    g_signal_connect(shell->app_monitor, "changed", G_CALLBACK(programs_changed), shell);
    /* Installed since the last session: XP says so at logon. */
    if (xd_start_menu_note_programs(shell) > 0)
        xd_balloon_new_programs(shell);
}

void xd_start_menu_unwatch_programs(xd_shell *shell) {
    xd_balloon_hide(shell);
    if (shell->app_monitor != nullptr) {
        g_signal_handlers_disconnect_by_data(shell->app_monitor, shell);
        g_object_unref(shell->app_monitor);
        shell->app_monitor = nullptr;
    }
    if (shell->apps_reload != 0) {
        g_source_remove(shell->apps_reload);
        shell->apps_reload = 0;
    }
    if (shell->new_apps != nullptr) {
        g_hash_table_unref(shell->new_apps);
        shell->new_apps = nullptr;
    }
}

xd_menu_model *xd_start_control_model(xd_shell *shell, xd_menu_tree *tree) {
    xd_menu_model *menu_model = xd_menu_model_new(tree);
    const xd_app_list *applets = &shell->control_apps;
    for (size_t i = 0; i < applets->count; i++) {
        const xd_app *app = &applets->items[i];
        xd_menu_item *it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, app->name, app->icon);
        it->fallback_icon = "preferences-system";
        it->app = app;
    }
    if (menu_model->count == 0)
        xd_menu_add(tree, menu_model, XD_MENU_ACTION, "(Empty)", nullptr)->disabled = true;
    return menu_model;
}

static void open_recent(xd_shell *shell, void *data) { xd_shell_open_uri(shell, data); }

/* Documents: My Documents, the Home folder, then the recently used files
 * in name order (Windows 2000 sorts them), or "(Empty)". */
void xd_start_menu_refresh_documents(xd_shell *shell) {
    if (shell->start_model == nullptr || shell->start_docs_index >= shell->start_model->count)
        return;
    xd_menu_tree *tree = &shell->docs_tree;
    xd_menu_tree_free(tree);
    xd_menu_tree_init(tree);
    xd_menu_model *menu_model = xd_menu_model_new(tree);
    xd_menu_item *mydocs =
        xd_menu_add(tree, menu_model, XD_MENU_ACTION, "My Documents", "folder-documents");
    mydocs->command = xd_shell_own_command(
        shell, "xd-files", "\"$(xdg-user-dir DOCUMENTS 2>/dev/null || echo \"$HOME/Documents\")\"");
    xd_menu_item *home = xd_menu_add(tree, menu_model, XD_MENU_ACTION, "Home Folder", "user-home");
    home->command = xd_shell_own_command(shell, "xd-files", "\"$HOME\"");
    xd_menu_add(tree, menu_model, XD_MENU_SEPARATOR, nullptr, nullptr);

    char **paths = nullptr;
    size_t count = 0;
    xd_recent_load(&paths, &count, 15);
    GPtrArray *names = g_ptr_array_new();
    for (size_t i = 0; i < count; i++)
        g_ptr_array_add(names, paths[i]);
    g_ptr_array_sort_values(names, (GCompareFunc)g_utf8_collate);
    for (guint i = 0; i < names->len; i++) {
        const char *path = g_ptr_array_index(names, i);
        char *base = g_path_get_basename(path);
        char *label = g_filename_display_name(base);
        char *ctype = g_content_type_guess(path, nullptr, 0, nullptr);
        char *icon = ctype != nullptr ? g_content_type_get_generic_icon_name(ctype) : nullptr;
        GIcon *gicon = ctype != nullptr ? g_content_type_get_icon(ctype) : nullptr;
        const char *specific = gicon != nullptr && G_IS_THEMED_ICON(gicon)
                                   ? g_themed_icon_get_names(G_THEMED_ICON(gicon))[0]
                                   : nullptr;
        xd_menu_item *it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, label,
                                       specific != nullptr ? specific : icon);
        it->fallback_icon = icon != nullptr ? xd_str_arena_cstr(&tree->arena,
                                                                xd_str_from_cstr(icon))
                                            : "text-x-generic";
        char *uri = g_filename_to_uri(path, nullptr, nullptr);
        it->action = open_recent;
        it->action_data =
            uri != nullptr ? xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(uri)) : nullptr;
        if (uri == nullptr)
            it->disabled = true;
        g_free(uri);
        g_clear_object(&gicon);
        g_free(icon);
        g_free(ctype);
        g_free(label);
        g_free(base);
    }
    if (names->len == 0)
        xd_menu_add(tree, menu_model, XD_MENU_ACTION, "(Empty)", nullptr)->disabled = true;
    g_ptr_array_unref(names);
    xd_free_lines(paths, count);
    shell->start_model->items[shell->start_docs_index].submenu = menu_model;
    xd_menu_model_set_look(menu_model, XD_MENU_LOOK_CLASSIC_START);
}

void xd_start_menu_build(xd_shell *shell) {
    xd_menu_tree *tree = &shell->menu_tree;
    xd_menu_model *root = xd_menu_model_new(tree);
    root->large = true;
    root->banner_product = "XeonDesktop";
    root->banner_edition = "Professional";

    xd_menu_item *programs =
        xd_menu_add_keyed(tree, root, XD_MENU_SUBMENU, "&Programs", "applications-other");
    programs->fallback_icon = "folder";
    programs->submenu = xd_start_programs_model(shell, tree, false);

    shell->start_docs_index = root->count;
    xd_menu_item *docs =
        xd_menu_add_keyed(tree, root, XD_MENU_SUBMENU, "&Documents", "folder-documents");
    docs->fallback_icon = "folder";
    docs->submenu = nullptr; /* filled each time the menu opens */

    xd_menu_item *settings =
        xd_menu_add_keyed(tree, root, XD_MENU_SUBMENU, "&Settings", "preferences-system");
    settings->submenu = xd_menu_model_new(tree);
    /* "Expand Control Panel" style: a cascading list of applets; clicking
     * Control Panel itself opens the folder. */
    xd_menu_item *control = xd_menu_add_keyed(tree, settings->submenu, XD_MENU_SUBMENU,
                                              "&Control Panel", "preferences-system");
    control->submenu = xd_start_control_model(shell, tree);
    control->command = xd_shell_own_command(shell, "xd-control", nullptr);
    xd_menu_item *network = xd_menu_add_keyed(tree, settings->submenu, XD_MENU_ACTION,
                                              "&Network and Dial-up Connections",
                                              "network-workgroup");
    network->command = xd_shell_own_command(shell, "xd-netconn", nullptr);
    xd_menu_item *printers =
        xd_menu_add_keyed(tree, settings->submenu, XD_MENU_ACTION, "&Printers", "printer");
    printers->command = xd_shell_own_command(shell, "xd-printers", nullptr);
    xd_menu_item *taskbar = xd_menu_add_keyed(tree, settings->submenu, XD_MENU_ACTION,
                                              "&Taskbar && Start Menu...", "preferences-desktop");
    taskbar->command = xd_shell_own_command(shell, "xd-taskbar", nullptr);

    xd_menu_item *search =
        xd_menu_add_keyed(tree, root, XD_MENU_SUBMENU, "Sear&ch", "system-search");
    search->submenu = xd_menu_model_new(tree);
    xd_menu_item *files = xd_menu_add_keyed(tree, search->submenu, XD_MENU_ACTION,
                                            "For &Files or Folders...", "system-search");
    files->command = xd_shell_own_command(shell, "xd-files", "--search");

    xd_menu_item *help = xd_menu_add_keyed(tree, root, XD_MENU_ACTION, "&Help", "help-browser");
    help->command = xd_shell_own_command(shell, "xd-help", nullptr);
    xd_menu_item *run = xd_menu_add_keyed(tree, root, XD_MENU_ACTION, "&Run...", "system-run");
    run->command = xd_shell_own_command(shell, "xd-run", nullptr);
    xd_menu_add(tree, root, XD_MENU_SEPARATOR, nullptr, nullptr);

    const char *user = getenv("USER");
    xd_strbuf logoff = {0};
    xd_strbuf_append(&logoff, XD_STR("&Log Off "));
    for (const char *character = user != nullptr ? user : ""; *character != '\0'; character++) {
        if (*character == '&')
            xd_strbuf_append_char(&logoff, '&'); /* the name's own, as it is */
        xd_strbuf_append_char(&logoff, *character);
    }
    xd_strbuf_append(&logoff, XD_STR("..."));
    xd_menu_item *logoff_item =
        xd_menu_add_keyed(tree, root, XD_MENU_ACTION, xd_strbuf_cstr(&logoff), "system-log-out");
    logoff_item->command = xd_shell_own_command(shell, "xd-shutdown", "--logoff");
    xd_strbuf_free(&logoff);
    /* Sh&ut Down: Windows' Win, U. */
    xd_menu_item *shutdown =
        xd_menu_add_keyed(tree, root, XD_MENU_ACTION, "Sh&ut Down...", "system-shutdown");
    shutdown->command = xd_shell_own_command(shell, "xd-shutdown", nullptr);

    xd_menu_model_set_look(root, XD_MENU_LOOK_CLASSIC_START);
    shell->start_model = root;
}
