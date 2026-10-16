/* SPDX-License-Identifier: MIT
 * The taskbar's right-click menus, as in Windows XP: the taskbar menu
 * (Toolbars, Cascade/Tile, Show the Desktop, Task Manager, Lock the
 * Taskbar, Properties), the window menu on a task button (Restore, Move,
 * Size, Minimize, Maximize, Close), and the Start button's (Open, Explore,
 * Search, Properties, Open/Explore All Users).
 */
#include "../core/alloc.h"
#include "shell.h"

/* ---- actions --------------------------------------------------------------------- */

static void arrange(xd_shell *shell, uint32_t mode) {
    if (shell->arranger == nullptr)
        return;
    xd_window_arranger_v1_arrange(shell->arranger, mode);
    shell->last_arrange = (int)mode;
}

static void act_cascade(xd_shell *shell, [[maybe_unused]] void *d) {
    arrange(shell, XD_WINDOW_ARRANGER_V1_MODE_CASCADE);
}
static void act_tile_h(xd_shell *shell, [[maybe_unused]] void *d) {
    arrange(shell, XD_WINDOW_ARRANGER_V1_MODE_TILE_HORIZONTALLY);
}
static void act_tile_v(xd_shell *shell, [[maybe_unused]] void *d) {
    arrange(shell, XD_WINDOW_ARRANGER_V1_MODE_TILE_VERTICALLY);
}
static void act_undo(xd_shell *shell, [[maybe_unused]] void *d) {
    if (shell->arranger != nullptr)
        xd_window_arranger_v1_undo(shell->arranger);
    shell->last_arrange = -1;
}
static void act_show_desktop(xd_shell *shell, [[maybe_unused]] void *d) {
    xd_show_desktop_toggle(shell);
}

/* Toggles a taskbar setting and saves it; the settings watch applies it. */
static void toggle_setting(xd_shell *shell, bool *field) {
    *field = !*field;
    if (!xd_settings_save(&shell->settings))
        fprintf(stderr, "xd-shell: cannot save settings\n");
    xd_taskbars_mark_dirty(shell);
}
static void act_lock(xd_shell *shell, [[maybe_unused]] void *d) {
    toggle_setting(shell, &shell->settings.locked);
}
static void act_quick_launch(xd_shell *shell, [[maybe_unused]] void *d) {
    toggle_setting(shell, &shell->settings.quick_launch);
}

/* Window menu: the toplevel is looked up again, in case it closed. */
static xd_toplevel *find_toplevel(xd_shell *shell, void *d) {
    xd_toplevel *t;
    wl_list_for_each(t, &shell->toplevels, link) {
        if (t == d)
            return t;
    }
    return nullptr;
}
static void act_restore(xd_shell *shell, void *d) {
    xd_toplevel *t = find_toplevel(shell, d);
    if (t == nullptr)
        return;
    if (t->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
    else if (t->maximized)
        zwlr_foreign_toplevel_handle_v1_unset_maximized(t->handle);
    if (shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(t->handle, shell->seat);
}
static void act_minimize(xd_shell *shell, void *d) {
    xd_toplevel *t = find_toplevel(shell, d);
    if (t != nullptr)
        zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
}
static void act_maximize(xd_shell *shell, void *d) {
    xd_toplevel *t = find_toplevel(shell, d);
    if (t == nullptr)
        return;
    if (t->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
    zwlr_foreign_toplevel_handle_v1_set_maximized(t->handle);
    if (shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(t->handle, shell->seat);
}
/* Move and Size: bring the window forward, then let the compositor run
 * the keyboard move/size loop on it (requests are handled in order). */
static void act_move(xd_shell *shell, void *d) {
    xd_toplevel *t = find_toplevel(shell, d);
    if (t == nullptr || shell->arranger == nullptr || shell->seat == nullptr)
        return;
    zwlr_foreign_toplevel_handle_v1_activate(t->handle, shell->seat);
    xd_window_arranger_v1_begin_move(shell->arranger);
}
static void act_size(xd_shell *shell, void *d) {
    xd_toplevel *t = find_toplevel(shell, d);
    if (t == nullptr || shell->arranger == nullptr || shell->seat == nullptr)
        return;
    zwlr_foreign_toplevel_handle_v1_activate(t->handle, shell->seat);
    xd_window_arranger_v1_begin_size(shell->arranger);
}

static void act_close(xd_shell *shell, void *d) {
    xd_toplevel *t = find_toplevel(shell, d);
    if (t != nullptr)
        zwlr_foreign_toplevel_handle_v1_close(t->handle);
}

/* ---- menus ------------------------------------------------------------------------- */

static xd_menu_item *add(xd_menu_tree *tree, xd_menu_model *m, const char *label,
                         void (*fn)(xd_shell *, void *), void *data) {
    xd_menu_item *it = xd_menu_add(tree, m, XD_MENU_ACTION, label, nullptr);
    it->action = fn;
    it->action_data = data;
    it->disabled = fn == nullptr;
    return it;
}

static void separator(xd_menu_tree *tree, xd_menu_model *m) {
    xd_menu_add(tree, m, XD_MENU_SEPARATOR, nullptr, nullptr);
}

static xd_menu_model *taskbar_menu(xd_shell *shell, xd_menu_tree *tree) {
    xd_menu_model *m = xd_menu_model_new(tree);
    bool can_arrange = shell->arranger != nullptr;

    xd_menu_item *toolbars = xd_menu_add(tree, m, XD_MENU_SUBMENU, "Toolbars", nullptr);
    toolbars->submenu = xd_menu_model_new(tree);
    add(tree, toolbars->submenu, "Address", nullptr, nullptr);
    add(tree, toolbars->submenu, "Links", nullptr, nullptr);
    add(tree, toolbars->submenu, "Desktop", nullptr, nullptr);
    add(tree, toolbars->submenu, "Quick Launch", act_quick_launch, nullptr)->checked =
        shell->settings.quick_launch;
    separator(tree, toolbars->submenu);
    add(tree, toolbars->submenu, "New Toolbar...", nullptr, nullptr);
    separator(tree, m);

    add(tree, m, "Cascade Windows", can_arrange ? act_cascade : nullptr, nullptr);
    add(tree, m, "Tile Windows Horizontally", can_arrange ? act_tile_h : nullptr, nullptr);
    add(tree, m, "Tile Windows Vertically", can_arrange ? act_tile_v : nullptr, nullptr);
    if (shell->last_arrange >= 0) {
        static const char *const undo[] = {"Undo Cascade", "Undo Tile", "Undo Tile"};
        add(tree, m, undo[shell->last_arrange % 3], act_undo, nullptr);
    }
    add(tree, m, "Show the Desktop", act_show_desktop, nullptr);
    separator(tree, m);
    add(tree, m, "Task Manager", nullptr, nullptr)->command =
        xd_shell_own_command(shell, "xd-taskmgr", nullptr);
    m->items[m->count - 1].disabled = false;
    separator(tree, m);
    add(tree, m, "Lock the Taskbar", act_lock, nullptr)->checked = shell->settings.locked;
    xd_menu_item *props = add(tree, m, "Properties", nullptr, nullptr);
    props->command = xd_shell_own_command(shell, "xd-taskbar", nullptr);
    props->disabled = false;
    return m;
}

static xd_menu_model *window_menu(xd_shell *shell, xd_menu_tree *tree, xd_toplevel *t) {
    xd_menu_model *m = xd_menu_model_new(tree);
    bool restorable = t->minimized || t->maximized;
    xd_menu_item *it = add(tree, m, "Restore", restorable ? act_restore : nullptr, t);
    it->glyph = XD_MENU_GLYPH_RESTORE;
    it->bold = restorable;
    /* Fixed-size windows ignore Size (the compositor knows which they are). */
    bool movable = !restorable && shell->arranger != nullptr;
    add(tree, m, "Move", movable ? act_move : nullptr, t);
    add(tree, m, "Size", movable ? act_size : nullptr, t);
    it = add(tree, m, "Minimize", t->minimized ? nullptr : act_minimize, t);
    it->glyph = XD_MENU_GLYPH_MINIMIZE;
    it = add(tree, m, "Maximize", t->maximized ? nullptr : act_maximize, t);
    it->glyph = XD_MENU_GLYPH_MAXIMIZE;
    separator(tree, m);
    it = add(tree, m, "Close", act_close, t);
    it->glyph = XD_MENU_GLYPH_CLOSE;
    it->accel = "Alt+F4";
    it->bold = !restorable;
    return m;
}

/* The Start menu's own folders: the user's application shortcuts, and
 * everyone's. Open and Explore both show them in xd-files, which always
 * has its Folders pane available. */
static xd_menu_model *start_button_menu(xd_shell *shell, xd_menu_tree *tree) {
    xd_menu_model *m = xd_menu_model_new(tree);
    static const char user_dir[] =
        "\"${XDG_DATA_HOME:-$HOME/.local/share}/applications\"";
    static const char all_dir[] = "/usr/share/applications";
    xd_menu_item *it = add(tree, m, "Open", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-files", user_dir);
    it->disabled = false;
    it->bold = true;
    it = add(tree, m, "Explore", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-files", user_dir);
    it->disabled = false;
    add(tree, m, "Search...", nullptr, nullptr);
    separator(tree, m);
    it = add(tree, m, "Properties", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-taskbar", "--page 1");
    it->disabled = false;
    separator(tree, m);
    it = add(tree, m, "Open All Users", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-files", all_dir);
    it->disabled = false;
    it = add(tree, m, "Explore All Users", nullptr, nullptr);
    it->command = xd_shell_own_command(shell, "xd-files", all_dir);
    it->disabled = false;
    return m;
}

/* The compositor asks for the focused window's menu (Alt+Space, the caption
 * icon, a right-click on the title bar). x, y: output-local, logical. */
static void handle_window_menu(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *a,
                               struct wl_output *output, int32_t x, int32_t y, uint32_t serial) {
    xd_shell *shell = data;
    xd_taskbar *bar = nullptr, *b;
    wl_list_for_each(b, &shell->taskbars, link) {
        if (b->output == output && b->configured)
            bar = b;
    }
    xd_toplevel *focused = nullptr, *t;
    wl_list_for_each(t, &shell->toplevels, link) {
        if (t->activated)
            focused = t;
    }
    if (bar == nullptr || focused == nullptr || bar->desktop == nullptr)
        return;
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *model = window_menu(shell, &shell->context_tree, focused);
    /* Popups hang off the taskbar's surface, which sits at the bottom of
     * the output: convert to its coordinates. */
    int bar_top = bar->desktop->height - bar->height;
    xd_rect anchor = {x, y - bar_top, 1, 1};
    xd_taskbar_take_keyboard(bar);
    shell->context_menu = true;
    xd_menu_open_root_dir(shell, model, bar->layer, anchor, serial, true);
    shell->start_bar = bar;
    bar->dirty = true;
}

/* The Windows key or Ctrl+Esc. */
static void handle_start_menu(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *a,
                              struct wl_output *output, uint32_t serial) {
    xd_shell *shell = data;
    xd_taskbar *b;
    wl_list_for_each(b, &shell->taskbars, link) {
        if (b->output == output && b->configured) {
            xd_taskbar_toggle_start(b, serial);
            return;
        }
    }
}

static void handle_attention(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *arranger,
                             struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
    xd_toplevel_attention(data, toplevel);
}

static const struct xd_window_arranger_v1_listener arranger_listener = {
    .start_menu = handle_start_menu,
    .window_menu = handle_window_menu,
    .attention = handle_attention,
};

void xd_taskmenu_bind(xd_shell *shell) {
    xd_window_arranger_v1_add_listener(shell->arranger, &arranger_listener, shell);
}

void xd_taskbar_context_menu(xd_taskbar *bar, int hit, double x, double y, uint32_t serial) {
    xd_shell *shell = bar->shell;
    xd_menu_close_all(shell);
    xd_menu_tree_free(&shell->context_tree);
    xd_menu_tree_init(&shell->context_tree);
    xd_menu_model *model;
    if (hit >= 0 && (size_t)hit < bar->task_count)
        model = window_menu(shell, &shell->context_tree, bar->task_views[hit]);
    else if (hit == XD_HIT_START)
        model = start_button_menu(shell, &shell->context_tree);
    else
        model = taskbar_menu(shell, &shell->context_tree);
    /* Anchored at the pointer; opens upwards. */
    xd_rect anchor = {(int)x, (int)y, 1, 1};
    xd_taskbar_take_keyboard(bar);
    shell->context_menu = true;
    xd_menu_open_root(shell, model, bar->layer, anchor, serial);
    shell->start_bar = bar; /* gives the keyboard back when it closes */
    bar->dirty = true;
}
