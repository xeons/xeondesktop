/* SPDX-License-Identifier: MIT
 * The taskbar's right-click menus, as in Windows XP: the taskbar menu
 * (Toolbars, Cascade/Tile, Show the Desktop, Task Manager, Lock the
 * Taskbar, Properties), the window menu on a task button (Restore, Move,
 * Size, Minimize, Maximize, Close), and the Start button's (Open, Explore,
 * Search, Properties, Open/Explore All Users).
 */
#include <errno.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "shell.h"

/* ---- actions --------------------------------------------------------------------- */

static void arrange(xd_shell *shell, uint32_t mode) {
    if (shell->arranger == nullptr)
        return;
    xd_window_arranger_v1_arrange(shell->arranger, mode);
    shell->last_arrange = (int)mode;
}

static void act_cascade(xd_shell *shell, [[maybe_unused]] void *data) {
    arrange(shell, XD_WINDOW_ARRANGER_V1_MODE_CASCADE);
}
static void act_tile_h(xd_shell *shell, [[maybe_unused]] void *data) {
    arrange(shell, XD_WINDOW_ARRANGER_V1_MODE_TILE_HORIZONTALLY);
}
static void act_tile_v(xd_shell *shell, [[maybe_unused]] void *data) {
    arrange(shell, XD_WINDOW_ARRANGER_V1_MODE_TILE_VERTICALLY);
}
static void act_undo(xd_shell *shell, [[maybe_unused]] void *data) {
    if (shell->arranger != nullptr)
        xd_window_arranger_v1_undo(shell->arranger);
    shell->last_arrange = -1;
}
static void act_show_desktop(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_show_desktop_toggle(shell);
}

/* Toggles a taskbar setting and saves it; the settings watch applies it. */
static void toggle_setting(xd_shell *shell, bool *field) {
    *field = !*field;
    if (!xd_settings_save(&shell->settings))
        fprintf(stderr, "xd-shell: cannot save settings\n");
    xd_taskbars_mark_dirty(shell);
}
static void act_lock(xd_shell *shell, [[maybe_unused]] void *data) {
    toggle_setting(shell, &shell->settings.locked);
}
static void act_quick_launch(xd_shell *shell, [[maybe_unused]] void *data) {
    toggle_setting(shell, &shell->settings.quick_launch);
}
/* Quick Launch's View > Large Icons / Small Icons, Show Text, Show Title. */
static void act_quick_large(xd_shell *shell, [[maybe_unused]] void *data) {
    if (!shell->settings.quick_large)
        toggle_setting(shell, &shell->settings.quick_large);
}
static void act_quick_small(xd_shell *shell, [[maybe_unused]] void *data) {
    if (shell->settings.quick_large)
        toggle_setting(shell, &shell->settings.quick_large);
}
static void act_quick_text(xd_shell *shell, [[maybe_unused]] void *data) {
    toggle_setting(shell, &shell->settings.quick_show_text);
}
static void act_quick_title(xd_shell *shell, [[maybe_unused]] void *data) {
    toggle_setting(shell, &shell->settings.quick_show_title);
}

/* Window menu: the toplevel is looked up again by its id, in case it
 * closed (a new window may have been given the same address). */
static void *toplevel_ref(const xd_toplevel *toplevel) { return (void *)(uintptr_t)toplevel->id; }

static xd_toplevel *find_toplevel(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel;
    wl_list_for_each(toplevel, &shell->toplevels, link) {
        if (toplevel->id == (uint64_t)(uintptr_t)toplevel_id)
            return toplevel;
    }
    return nullptr;
}
static void act_restore(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel = find_toplevel(shell, toplevel_id);
    if (toplevel == nullptr)
        return;
    if (toplevel->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(toplevel->handle);
    else if (toplevel->maximized)
        zwlr_foreign_toplevel_handle_v1_unset_maximized(toplevel->handle);
    if (shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, shell->seat);
}
static void act_minimize(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel = find_toplevel(shell, toplevel_id);
    if (toplevel != nullptr)
        zwlr_foreign_toplevel_handle_v1_set_minimized(toplevel->handle);
}
static void act_maximize(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel = find_toplevel(shell, toplevel_id);
    if (toplevel == nullptr)
        return;
    if (toplevel->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(toplevel->handle);
    zwlr_foreign_toplevel_handle_v1_set_maximized(toplevel->handle);
    if (shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, shell->seat);
}
/* Move and Size: bring the window forward, then let the compositor run
 * the keyboard move/size loop on it (requests are handled in order). */
static void act_move(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel = find_toplevel(shell, toplevel_id);
    if (toplevel == nullptr || shell->arranger == nullptr || shell->seat == nullptr)
        return;
    zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, shell->seat);
    xd_window_arranger_v1_begin_move(shell->arranger);
}
static void act_size(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel = find_toplevel(shell, toplevel_id);
    if (toplevel == nullptr || shell->arranger == nullptr || shell->seat == nullptr)
        return;
    zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, shell->seat);
    xd_window_arranger_v1_begin_size(shell->arranger);
}

/* Virtual desktops: the window and the desktop, for the window menu's
 * Move to Another Workspace. */
typedef struct desktop_choice {
    uint64_t toplevel_id;
    uint32_t desktop;
} desktop_choice;

static bool has_desktops(const xd_shell *shell) {
    return shell->arranger != nullptr &&
           xd_window_arranger_v1_get_version(shell->arranger) >=
               XD_WINDOW_ARRANGER_V1_SWITCH_DESKTOP_SINCE_VERSION &&
           shell->desktop_columns * shell->desktop_rows > 1;
}

static void act_move_to_desktop(xd_shell *shell, void *data) {
    const desktop_choice *choice = data;
    xd_toplevel *toplevel = find_toplevel(shell, (void *)(uintptr_t)choice->toplevel_id);
    if (toplevel != nullptr && has_desktops(shell))
        xd_window_arranger_v1_move_window_to_desktop(shell->arranger, toplevel->handle,
                                                     choice->desktop);
}

static void act_toggle_sticky(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel = find_toplevel(shell, toplevel_id);
    if (toplevel != nullptr && has_desktops(shell))
        xd_window_arranger_v1_set_window_sticky(shell->arranger, toplevel->handle,
                                                toplevel->sticky ? 0 : 1);
}

static void act_close(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel = find_toplevel(shell, toplevel_id);
    if (toplevel != nullptr)
        zwlr_foreign_toplevel_handle_v1_close(toplevel->handle);
}

/* ---- menus ------------------------------------------------------------------------- */

/* Labels with their access keys as Windows writes them ("Mi&nimize"). */
static xd_menu_item *add(xd_menu_tree *tree, xd_menu_model *menu_model, const char *label,
                         void (*fn)(xd_shell *, void *), void *data) {
    xd_menu_item *it = xd_menu_add_keyed(tree, menu_model, XD_MENU_ACTION, label, nullptr);
    it->action = fn;
    it->action_data = data;
    it->disabled = fn == nullptr;
    return it;
}

static void separator(xd_menu_tree *tree, xd_menu_model *menu_model) {
    xd_menu_add(tree, menu_model, XD_MENU_SEPARATOR, nullptr, nullptr);
}

/* ---- Quick Launch ---------------------------------------------------------- */

/* Items name their shortcut by file (indices shift when the folder
 * changes); the names live in the context tree's arena. */
static void quick_path(const char *file, xd_strbuf *out) {
    xd_quicklaunch_folder(out);
    xd_strbuf_appendf(out, "/%s", file);
}

static void act_quick_open(xd_shell *shell, void *file) {
    ssize_t i = xd_quicklaunch_find(shell, file);
    if (i >= 0)
        xd_quicklaunch_activate(shell, (size_t)i);
}

static void act_quick_delete(xd_shell *shell, void *file) {
    (void)shell;
    xd_strbuf path = {0};
    quick_path(file, &path);
    /* Only the shortcut (or link) in the Quick Launch folder goes; the
     * folder watch updates the toolbar. */
    if (unlink(xd_strbuf_cstr(&path)) != 0)
        fprintf(stderr, "xd-shell: cannot delete %s: %s\n", xd_strbuf_cstr(&path),
                strerror(errno));
    xd_strbuf_free(&path);
}

static void act_quick_properties(xd_shell *shell, void *file) {
    xd_strbuf path = {0}, arg = {0};
    quick_path(file, &path);
    char *quoted = g_shell_quote(xd_strbuf_cstr(&path));
    xd_strbuf_appendf(&arg, "--properties %s", quoted);
    g_free(quoted);
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-files", xd_strbuf_cstr(&arg)));
    xd_strbuf_free(&arg);
    xd_strbuf_free(&path);
}

static void act_quick_folder(xd_shell *shell, [[maybe_unused]] void *data) {
    xd_strbuf dir = {0};
    xd_quicklaunch_folder(&dir);
    char *quoted = g_shell_quote(xd_strbuf_cstr(&dir));
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-files", quoted));
    g_free(quoted);
    xd_strbuf_free(&dir);
}

/* A shortcut's menu: Open, Delete, Properties, as Explorer's. */
static xd_menu_model *quick_item_menu(xd_shell *shell, xd_menu_tree *tree, size_t index) {
    xd_menu_model *menu_model = xd_menu_model_new(tree);
    char *file = xd_str_arena_cstr(&tree->arena,
                                   xd_str_from_cstr(shell->quicklaunch.files[index]));
    add(tree, menu_model, "&Open", act_quick_open, file)->bold = true;
    separator(tree, menu_model);
    add(tree, menu_model, "&Delete", act_quick_delete, file);
    separator(tree, menu_model);
    add(tree, menu_model, "P&roperties", act_quick_properties, file);
    return menu_model;
}

/* ---- grouped buttons ------------------------------------------------------- */

static bool in_group(const xd_toplevel *toplevel, const char *app_id) {
    return toplevel->app_id != nullptr && strcmp(toplevel->app_id, app_id) == 0;
}

static void act_activate(xd_shell *shell, void *toplevel_id) {
    xd_toplevel *toplevel = find_toplevel(shell, toplevel_id);
    if (toplevel == nullptr || shell->seat == nullptr)
        return;
    if (toplevel->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(toplevel->handle);
    zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, shell->seat);
}

static void act_minimize_group(xd_shell *shell, void *app_id) {
    xd_toplevel *toplevel;
    wl_list_for_each(toplevel, &shell->toplevels, link) {
        if (in_group(toplevel, app_id))
            zwlr_foreign_toplevel_handle_v1_set_minimized(toplevel->handle);
    }
}

static void act_close_group(xd_shell *shell, void *app_id) {
    xd_toplevel *toplevel;
    wl_list_for_each(toplevel, &shell->toplevels, link) {
        if (in_group(toplevel, app_id))
            zwlr_foreign_toplevel_handle_v1_close(toplevel->handle);
    }
}

/* A group's button: its windows, the active one checked off in bold. */
static xd_menu_model *group_menu(xd_shell *shell, xd_menu_tree *tree, const char *app_id) {
    xd_menu_model *menu_model = xd_menu_model_new(tree);
    xd_toplevel *toplevel;
    wl_list_for_each(toplevel, &shell->toplevels, link) {
        if (!in_group(toplevel, app_id))
            continue;
        const char *label =
            toplevel->title != nullptr && toplevel->title[0] != '\0' ? toplevel->title : app_id;
        /* A title, as it is: no access key. */
        xd_menu_item *it = xd_menu_add(tree, menu_model, XD_MENU_ACTION, label, nullptr);
        it->action = act_activate;
        it->action_data = toplevel_ref(toplevel);
        it->icon = toplevel->icon != nullptr
                       ? xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(toplevel->icon))
                       : nullptr;
        it->fallback_icon = "application-x-executable";
        it->bold = toplevel->activated && !toplevel->minimized;
    }
    return menu_model;
}

static void arrange_group(xd_shell *shell, const char *app_id, uint32_t mode) {
    if (shell->arranger != nullptr &&
        xd_window_arranger_v1_get_version(shell->arranger) >=
            XD_WINDOW_ARRANGER_V1_ARRANGE_APP_SINCE_VERSION)
        xd_window_arranger_v1_arrange_app(shell->arranger, mode, app_id);
}
static void act_cascade_group(xd_shell *shell, void *app_id) {
    arrange_group(shell, app_id, XD_WINDOW_ARRANGER_V1_MODE_CASCADE);
}
static void act_tile_h_group(xd_shell *shell, void *app_id) {
    arrange_group(shell, app_id, XD_WINDOW_ARRANGER_V1_MODE_TILE_HORIZONTALLY);
}
static void act_tile_v_group(xd_shell *shell, void *app_id) {
    arrange_group(shell, app_id, XD_WINDOW_ARRANGER_V1_MODE_TILE_VERTICALLY);
}

/* Right-click on a group, as XP's: the whole group at once. */
static xd_menu_model *group_context_menu(xd_shell *shell, xd_menu_tree *tree,
                                         const char *app_id) {
    xd_menu_model *menu_model = xd_menu_model_new(tree);
    char *key = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(app_id));
    bool can = shell->arranger != nullptr &&
               xd_window_arranger_v1_get_version(shell->arranger) >=
                   XD_WINDOW_ARRANGER_V1_ARRANGE_APP_SINCE_VERSION;
    add(tree, menu_model, "&Cascade", can ? act_cascade_group : nullptr, key);
    add(tree, menu_model, "Tile &Horizontally", can ? act_tile_h_group : nullptr, key);
    add(tree, menu_model, "Tile &Vertically", can ? act_tile_v_group : nullptr, key);
    separator(tree, menu_model);
    xd_menu_item *it = add(tree, menu_model, "Mi&nimize Group", act_minimize_group, key);
    it->glyph = XD_MENU_GLYPH_MINIMIZE;
    separator(tree, menu_model);
    it = add(tree, menu_model, "&Close Group", act_close_group, key);
    it->glyph = XD_MENU_GLYPH_CLOSE;
    it->bold = true;
    return menu_model;
}

void xd_taskbar_group_menu(xd_taskbar *bar, int hit, uint32_t serial) {
    xd_shell *shell = bar->shell;
    if (hit < 0 || (size_t)hit >= bar->task_count || bar->task_views[hit]->app_id == nullptr)
        return;
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *model = group_menu(shell, &shell->context_tree, bar->task_views[hit]->app_id);
    /* From the button (device px -> surface-local logical). */
    xd_rect rect = bar->task_rects[hit];
    xd_rect anchor = {(int)(rect.x / bar->scale), (int)(rect.y / bar->scale),
                      (int)(rect.w / bar->scale), (int)(rect.h / bar->scale)};
    xd_taskbar_take_keyboard(bar);
    shell->context_menu = true;
    xd_menu_open_root_bar(shell, model, bar, anchor, serial);
    shell->start_bar = bar;
    bar->dirty = true;
}

/* The taskbar's menu; over the clock, Adjust Date/Time comes first, and
 * over Quick Launch the toolbar's own Open Folder and Close Toolbar. */
static xd_menu_model *taskbar_menu(xd_shell *shell, xd_menu_tree *tree, bool clock,
                                   bool quick) {
    xd_menu_model *menu_model = xd_menu_model_new(tree);
    bool can_arrange = shell->arranger != nullptr;
    if (quick) {
        /* XP's toolbar menu. */
        xd_menu_item *view = xd_menu_add_keyed(tree, menu_model, XD_MENU_SUBMENU, "&View", nullptr);
        view->submenu = xd_menu_model_new(tree);
        add(tree, view->submenu, "&Large Icons", act_quick_large, nullptr)->checked =
            shell->settings.quick_large;
        add(tree, view->submenu, "&Small Icons", act_quick_small, nullptr)->checked =
            !shell->settings.quick_large;
        add(tree, menu_model, "&Open Folder", act_quick_folder, nullptr);
        add(tree, menu_model, "Show Te&xt", act_quick_text, nullptr)->checked =
            shell->settings.quick_show_text;
        add(tree, menu_model, "Show &Title", act_quick_title, nullptr)->checked =
            shell->settings.quick_show_title;
        add(tree, menu_model, "&Close Toolbar", act_quick_launch, nullptr);
        separator(tree, menu_model);
    }
    if (clock) {
        xd_menu_item *it = add(tree, menu_model, "&Adjust Date/Time", nullptr, nullptr);
        it->command = xd_shell_own_command(shell, "xd-timedate", nullptr);
        it->disabled = false;
        it->bold = true;
        separator(tree, menu_model);
    }

    xd_menu_item *toolbars =
        xd_menu_add_keyed(tree, menu_model, XD_MENU_SUBMENU, "&Toolbars", nullptr);
    toolbars->submenu = xd_menu_model_new(tree);
    add(tree, toolbars->submenu, "Address", nullptr, nullptr);
    add(tree, toolbars->submenu, "Links", nullptr, nullptr);
    add(tree, toolbars->submenu, "Desktop", nullptr, nullptr);
    add(tree, toolbars->submenu, "Quick Launch", act_quick_launch, nullptr)->checked =
        shell->settings.quick_launch;
    separator(tree, toolbars->submenu);
    add(tree, toolbars->submenu, "&New Toolbar...", nullptr, nullptr);
    separator(tree, menu_model);

    /* XP's letters: S is both Cascade's and Show the Desktop's. */
    add(tree, menu_model, "Ca&scade Windows", can_arrange ? act_cascade : nullptr, nullptr);
    add(tree, menu_model, "Tile Windows &Horizontally", can_arrange ? act_tile_h : nullptr,
        nullptr);
    add(tree, menu_model, "Tile Windows V&ertically", can_arrange ? act_tile_v : nullptr, nullptr);
    if (shell->last_arrange >= 0) {
        static const char *const undo[] = {"&Undo Cascade", "&Undo Tile", "&Undo Tile"};
        add(tree, menu_model, undo[shell->last_arrange % 3], act_undo, nullptr);
    }
    add(tree, menu_model, "&Show the Desktop", act_show_desktop, nullptr);
    separator(tree, menu_model);
    add(tree, menu_model, "Tas&k Manager", nullptr, nullptr)->command =
        xd_shell_own_command(shell, "xd-taskmgr", nullptr);
    menu_model->items[menu_model->count - 1].disabled = false;
    separator(tree, menu_model);
    add(tree, menu_model, "&Lock the Taskbar", act_lock, nullptr)->checked = shell->settings.locked;
    xd_menu_item *props = add(tree, menu_model, "P&roperties", nullptr, nullptr);
    props->command = xd_shell_own_command(shell, "xd-taskbar", nullptr);
    props->disabled = false;
    return menu_model;
}

static xd_menu_model *window_menu(xd_shell *shell, xd_menu_tree *tree, xd_toplevel *toplevel) {
    xd_menu_model *menu_model = xd_menu_model_new(tree);
    bool restorable = toplevel->minimized || toplevel->maximized;
    /* The system menu's letters. */
    xd_menu_item *it = add(tree, menu_model, "&Restore", restorable ? act_restore : nullptr,
                           toplevel_ref(toplevel));
    it->glyph = XD_MENU_GLYPH_RESTORE;
    it->bold = restorable;
    /* Fixed-size windows ignore Size (the compositor knows which they are). */
    bool movable = !restorable && shell->arranger != nullptr;
    add(tree, menu_model, "&Move", movable ? act_move : nullptr, toplevel_ref(toplevel));
    add(tree, menu_model, "&Size", movable ? act_size : nullptr, toplevel_ref(toplevel));
    it = add(tree, menu_model, "Mi&nimize", toplevel->minimized ? nullptr : act_minimize,
             toplevel_ref(toplevel));
    it->glyph = XD_MENU_GLYPH_MINIMIZE;
    it = add(tree, menu_model, "Ma&ximize", toplevel->maximized ? nullptr : act_maximize,
             toplevel_ref(toplevel));
    it->glyph = XD_MENU_GLYPH_MAXIMIZE;
    /* Virtual desktops have no Windows original: Metacity's names. */
    if (has_desktops(shell)) {
        separator(tree, menu_model);
        add(tree, menu_model, "Always on &Visible Workspace", act_toggle_sticky,
            toplevel_ref(toplevel))
            ->checked = toplevel->sticky;
        xd_menu_item *move =
            xd_menu_add_keyed(tree, menu_model, XD_MENU_SUBMENU, "Move to &Another Workspace",
                              nullptr);
        move->submenu = xd_menu_model_new(tree);
        uint32_t count = shell->desktop_columns * shell->desktop_rows;
        for (uint32_t desktop = 0; desktop < count; desktop++) {
            desktop_choice *choice = XD_ARENA_NEW(&tree->arena, desktop_choice);
            *choice = (desktop_choice){toplevel->id, desktop};
            char label[32];
            snprintf(label, sizeof(label), "Workspace %u", desktop + 1);
            xd_menu_item *item = xd_menu_add(tree, move->submenu, XD_MENU_ACTION, label, nullptr);
            item->action = act_move_to_desktop;
            item->action_data = choice;
            item->disabled = !toplevel->sticky && desktop == toplevel->desktop;
        }
    }
    separator(tree, menu_model);
    it = add(tree, menu_model, "&Close", act_close, toplevel_ref(toplevel));
    it->glyph = XD_MENU_GLYPH_CLOSE;
    it->accel = "Alt+F4";
    it->bold = !restorable;
    return menu_model;
}

/* The Start menu's own folders: the user's application shortcuts, and
 * everyone's. Open and Explore both show them in xd-files, Explore with
 * the Folders bar. */
#define START_USER_DIR "\"${XDG_DATA_HOME:-$HOME/.local/share}/applications\""
#define START_ALL_DIR "/usr/share/applications"
static xd_menu_model *start_button_menu(xd_shell *shell, xd_menu_tree *tree) {
    xd_menu_model *menu_model = xd_menu_model_new(tree);
    xd_menu_item *it = add(tree, menu_model, "&Open", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-files", START_USER_DIR);
    it->disabled = false;
    it->bold = true;
    it = add(tree, menu_model, "&Explore", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-files", "--explore " START_USER_DIR);
    it->disabled = false;
    add(tree, menu_model, "Searc&h...", nullptr, nullptr);
    separator(tree, menu_model);
    it = add(tree, menu_model, "P&roperties", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-taskbar", "--page 1");
    it->disabled = false;
    separator(tree, menu_model);
    it = add(tree, menu_model, "O&pen All Users", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-files", START_ALL_DIR);
    it->disabled = false;
    it = add(tree, menu_model, "E&xplore All Users", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-files", "--explore " START_ALL_DIR);
    it->disabled = false;
    return menu_model;
}

/* Alt+Space asked for the window menu that follows: it underlines its
 * letters at once. */
static bool window_menu_keyboard;

static void handle_window_menu_keyboard([[maybe_unused]] void *data,
                                        [[maybe_unused]] struct xd_window_arranger_v1 *arranger) {
    window_menu_keyboard = true;
}

/* The compositor asks for the focused window's menu (Alt+Space, the caption
 * icon, a right-click on the title bar). x, y: output-local, logical. */
static void handle_window_menu(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                               struct wl_output *output, int32_t x, int32_t y, uint32_t serial) {
    xd_shell *shell = data;
    bool keyboard = window_menu_keyboard;
    window_menu_keyboard = false;
    /* That monitor's taskbar and desktop; without a taskbar there ("Show
     * the taskbar on all displays" off), one shown elsewhere takes the
     * keyboard for the menu. */
    xd_taskbar *here = nullptr, *keys = nullptr, *bar;
    wl_list_for_each(bar, &shell->taskbars, link) {
        if (bar->output == output)
            here = bar;
        if (bar->configured && (keys == nullptr || bar->output == output))
            keys = bar;
    }
    xd_toplevel *focused = nullptr, *toplevel;
    wl_list_for_each(toplevel, &shell->toplevels, link) {
        if (toplevel->activated)
            focused = toplevel;
    }
    if (here == nullptr || keys == nullptr || focused == nullptr || here->desktop == nullptr)
        return;
    struct zwlr_layer_surface_v1 *parent;
    xd_rect anchor = {x, y, 1, 1};
    if (here->configured) {
        /* Popups hang off the taskbar's surface: convert from the output's
         * coordinates to its own, wherever it's docked. */
        if (shell->settings.edge == XD_EDGE_BOTTOM)
            anchor.y -= here->desktop->height - here->height;
        else if (shell->settings.edge == XD_EDGE_RIGHT)
            anchor.x -= here->desktop->width - here->width;
        parent = here->layer;
    } else {
        /* The desktop covers the whole monitor: its coordinates are the
         * output's. */
        parent = here->desktop->layer;
    }
    if (parent == nullptr)
        return;
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *model = window_menu(shell, &shell->context_tree, focused);
    xd_taskbar_take_keyboard(keys);
    shell->context_menu = true;
    xd_menu_open_root_dir(shell, model, parent, anchor, serial, true);
    shell->start_bar = keys;
    shell->menu_cues = keyboard;
    keys->dirty = true;
}

/* The Windows key or Ctrl+Esc. */
static void handle_start_menu(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                              struct wl_output *output, uint32_t serial) {
    xd_shell *shell = data;
    /* This monitor's taskbar, else (without one there) the first's. */
    xd_taskbar *candidate, *here = nullptr, *fallback = nullptr;
    wl_list_for_each(candidate, &shell->taskbars, link) {
        if (here == nullptr && candidate->output == output && candidate->configured)
            here = candidate;
        if (fallback == nullptr && candidate->configured)
            fallback = candidate;
    }
    xd_taskbar *bar = here != nullptr ? here : fallback;
    if (bar == nullptr)
        return;
    xd_taskbar_toggle_start(bar, serial);
    /* Opened from the keyboard: its letters are underlined at once. */
    shell->menu_cues = shell->start_menu != nullptr || shell->start7 != nullptr;
}

static void handle_attention(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                             struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
    xd_toplevel_attention(data, toplevel);
}

static void handle_keyboard_layout(void *data,
                                   [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                                   uint32_t index) {
    xd_langbar_set_active(data, index);
}

static void handle_identifier(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                              struct zwlr_foreign_toplevel_handle_v1 *toplevel,
                              const char *identifier) {
    xd_thumbs_identifier(data, toplevel, identifier);
}

static void handle_power_button(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                                uint32_t button) {
    switch (button) {
    case XD_WINDOW_ARRANGER_V1_POWER_BUTTON_LID_CLOSED:
        xd_buttons_event(data, XD_BUTTON_LID);
        break;
    case XD_WINDOW_ARRANGER_V1_POWER_BUTTON_POWER:
        xd_buttons_event(data, XD_BUTTON_POWER);
        break;
    case XD_WINDOW_ARRANGER_V1_POWER_BUTTON_SLEEP:
        xd_buttons_event(data, XD_BUTTON_SLEEP);
        break;
    default: /* the lid opening does nothing */
        break;
    }
}

static void handle_session_locked(void *data,
                                  [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                                  uint32_t locked) {
    xd_shell *shell = data;
    shell->session_locked = locked != 0;
    xd_sleep_locked_changed(shell);
    xd_account_locked_changed(shell);
}

static void handle_desktops(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                            uint32_t columns, uint32_t rows, uint32_t current) {
    xd_shell *shell = data;
    shell->desktop_columns = XD_MAX(columns, 1u);
    shell->desktop_rows = XD_MAX(rows, 1u);
    shell->desktop_current = current;
    xd_taskbars_mark_dirty(shell);
}

static void handle_window_desktop(void *data,
                                  [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                                  struct zwlr_foreign_toplevel_handle_v1 *handle, uint32_t desktop,
                                  uint32_t sticky) {
    xd_shell *shell = data;
    xd_toplevel *toplevel;
    wl_list_for_each(toplevel, &shell->toplevels, link) {
        if (toplevel->handle == handle) {
            toplevel->desktop = desktop;
            toplevel->sticky = sticky != 0;
        }
    }
}

static const struct xd_window_arranger_v1_listener arranger_listener = {
    .start_menu = handle_start_menu,
    .window_menu = handle_window_menu,
    .attention = handle_attention,
    .keyboard_layout = handle_keyboard_layout,
    .identifier = handle_identifier,
    .window_menu_keyboard = handle_window_menu_keyboard,
    .power_button = handle_power_button,
    .session_locked = handle_session_locked,
    .desktops = handle_desktops,
    .window_desktop = handle_window_desktop,
};

bool xd_shell_hears_lock(const xd_shell *shell) {
    return shell->arranger != nullptr && xd_window_arranger_v1_get_version(shell->arranger) >=
                                             XD_WINDOW_ARRANGER_V1_SESSION_LOCKED_SINCE_VERSION;
}

void xd_taskmenu_bind(xd_shell *shell) {
    xd_window_arranger_v1_add_listener(shell->arranger, &arranger_listener, shell);
}

void xd_taskbar_context_menu(xd_taskbar *bar, int hit, double x, double y, uint32_t serial) {
    xd_shell *shell = bar->shell;
    /* Aero: a program's button has its jump list instead. */
    if (xd_superbar_is_button(bar, hit)) {
        xd_superbar_jump_list(bar, hit, serial);
        return;
    }
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *model;
    if (hit >= 0 && (size_t)hit < bar->task_count && bar->task_counts[hit] > 1)
        model = group_context_menu(shell, &shell->context_tree, bar->task_views[hit]->app_id);
    else if (hit >= 0 && (size_t)hit < bar->task_count)
        model = window_menu(shell, &shell->context_tree, bar->task_views[hit]);
    else if (hit == XD_HIT_START)
        model = start_button_menu(shell, &shell->context_tree);
    else if (xd_hit_is_quick(hit) &&
             (size_t)(XD_HIT_QUICK - hit) < shell->quicklaunch.count)
        model = quick_item_menu(shell, &shell->context_tree, (size_t)(XD_HIT_QUICK - hit));
    else
        model = taskbar_menu(shell, &shell->context_tree, hit == XD_HIT_TRAY,
                             hit == XD_HIT_QUICK_BAND);
    /* Anchored at the pointer; opens upwards. */
    xd_rect anchor = {(int)x, (int)y, 1, 1};
    xd_taskbar_take_keyboard(bar);
    shell->context_menu = true;
    xd_menu_open_root(shell, model, bar->layer, anchor, serial);
    shell->start_bar = bar; /* gives the keyboard back when it closes */
    bar->dirty = true;
}
