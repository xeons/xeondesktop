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
/* Quick Launch's View > Large Icons / Small Icons, Show Text, Show Title. */
static void act_quick_large(xd_shell *shell, [[maybe_unused]] void *d) {
    if (!shell->settings.quick_large)
        toggle_setting(shell, &shell->settings.quick_large);
}
static void act_quick_small(xd_shell *shell, [[maybe_unused]] void *d) {
    if (shell->settings.quick_large)
        toggle_setting(shell, &shell->settings.quick_large);
}
static void act_quick_text(xd_shell *shell, [[maybe_unused]] void *d) {
    toggle_setting(shell, &shell->settings.quick_show_text);
}
static void act_quick_title(xd_shell *shell, [[maybe_unused]] void *d) {
    toggle_setting(shell, &shell->settings.quick_show_title);
}

/* Window menu: the toplevel is looked up again by its id, in case it
 * closed (a new window may have been given the same address). */
static void *toplevel_ref(const xd_toplevel *t) { return (void *)(uintptr_t)t->id; }

static xd_toplevel *find_toplevel(xd_shell *shell, void *d) {
    xd_toplevel *t;
    wl_list_for_each(t, &shell->toplevels, link) {
        if (t->id == (uint64_t)(uintptr_t)d)
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

/* ---- Quick Launch ---------------------------------------------------------- */

/* Items name their shortcut by file (indices shift when the folder
 * changes); the names live in the context tree's arena. */
static void quick_path(const char *file, xd_strbuf *out) {
    xd_quicklaunch_folder(out);
    xd_strbuf_appendf(out, "/%s", file);
}

static void act_quick_open(xd_shell *shell, void *d) {
    ssize_t i = xd_quicklaunch_find(shell, d);
    if (i >= 0)
        xd_quicklaunch_activate(shell, (size_t)i);
}

static void act_quick_delete(xd_shell *shell, void *d) {
    (void)shell;
    xd_strbuf path = {0};
    quick_path(d, &path);
    /* Only the shortcut (or link) in the Quick Launch folder goes; the
     * folder watch updates the toolbar. */
    if (unlink(xd_strbuf_cstr(&path)) != 0)
        fprintf(stderr, "xd-shell: cannot delete %s: %s\n", xd_strbuf_cstr(&path),
                strerror(errno));
    xd_strbuf_free(&path);
}

static void act_quick_properties(xd_shell *shell, void *d) {
    xd_strbuf path = {0}, arg = {0};
    quick_path(d, &path);
    char *quoted = g_shell_quote(xd_strbuf_cstr(&path));
    xd_strbuf_appendf(&arg, "--properties %s", quoted);
    g_free(quoted);
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-files", xd_strbuf_cstr(&arg)));
    xd_strbuf_free(&arg);
    xd_strbuf_free(&path);
}

static void act_quick_folder(xd_shell *shell, [[maybe_unused]] void *d) {
    xd_strbuf dir = {0};
    xd_quicklaunch_folder(&dir);
    char *quoted = g_shell_quote(xd_strbuf_cstr(&dir));
    xd_shell_launch(shell, xd_shell_own_command(shell, "xd-files", quoted));
    g_free(quoted);
    xd_strbuf_free(&dir);
}

/* A shortcut's menu: Open, Delete, Properties, as Explorer's. */
static xd_menu_model *quick_item_menu(xd_shell *shell, xd_menu_tree *tree, size_t index) {
    xd_menu_model *m = xd_menu_model_new(tree);
    char *file = xd_str_arena_cstr(&tree->arena,
                                   xd_str_from_cstr(shell->quicklaunch.files[index]));
    add(tree, m, "Open", act_quick_open, file)->bold = true;
    separator(tree, m);
    add(tree, m, "Delete", act_quick_delete, file);
    separator(tree, m);
    add(tree, m, "Properties", act_quick_properties, file);
    return m;
}

/* ---- grouped buttons ------------------------------------------------------- */

static bool in_group(const xd_toplevel *t, const char *app_id) {
    return t->app_id != nullptr && strcmp(t->app_id, app_id) == 0;
}

static void act_activate(xd_shell *shell, void *d) {
    xd_toplevel *t = find_toplevel(shell, d);
    if (t == nullptr || shell->seat == nullptr)
        return;
    if (t->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
    zwlr_foreign_toplevel_handle_v1_activate(t->handle, shell->seat);
}

static void act_minimize_group(xd_shell *shell, void *d) {
    xd_toplevel *t;
    wl_list_for_each(t, &shell->toplevels, link) {
        if (in_group(t, d))
            zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
    }
}

static void act_close_group(xd_shell *shell, void *d) {
    xd_toplevel *t;
    wl_list_for_each(t, &shell->toplevels, link) {
        if (in_group(t, d))
            zwlr_foreign_toplevel_handle_v1_close(t->handle);
    }
}

/* A group's button: its windows, the active one checked off in bold. */
static xd_menu_model *group_menu(xd_shell *shell, xd_menu_tree *tree, const char *app_id) {
    xd_menu_model *m = xd_menu_model_new(tree);
    xd_toplevel *t;
    wl_list_for_each(t, &shell->toplevels, link) {
        if (!in_group(t, app_id))
            continue;
        const char *label = t->title != nullptr && t->title[0] != '\0' ? t->title : app_id;
        xd_menu_item *it = add(tree, m, label, act_activate, toplevel_ref(t));
        it->icon = t->icon != nullptr ? xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(t->icon))
                                      : nullptr;
        it->fallback_icon = "application-x-executable";
        it->bold = t->activated && !t->minimized;
    }
    return m;
}

static void arrange_group(xd_shell *shell, const char *app_id, uint32_t mode) {
    if (shell->arranger != nullptr &&
        xd_window_arranger_v1_get_version(shell->arranger) >=
            XD_WINDOW_ARRANGER_V1_ARRANGE_APP_SINCE_VERSION)
        xd_window_arranger_v1_arrange_app(shell->arranger, mode, app_id);
}
static void act_cascade_group(xd_shell *shell, void *d) {
    arrange_group(shell, d, XD_WINDOW_ARRANGER_V1_MODE_CASCADE);
}
static void act_tile_h_group(xd_shell *shell, void *d) {
    arrange_group(shell, d, XD_WINDOW_ARRANGER_V1_MODE_TILE_HORIZONTALLY);
}
static void act_tile_v_group(xd_shell *shell, void *d) {
    arrange_group(shell, d, XD_WINDOW_ARRANGER_V1_MODE_TILE_VERTICALLY);
}

/* Right-click on a group, as XP's: the whole group at once. */
static xd_menu_model *group_context_menu(xd_shell *shell, xd_menu_tree *tree,
                                         const char *app_id) {
    xd_menu_model *m = xd_menu_model_new(tree);
    char *key = xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(app_id));
    bool can = shell->arranger != nullptr &&
               xd_window_arranger_v1_get_version(shell->arranger) >=
                   XD_WINDOW_ARRANGER_V1_ARRANGE_APP_SINCE_VERSION;
    add(tree, m, "Cascade", can ? act_cascade_group : nullptr, key);
    add(tree, m, "Tile Horizontally", can ? act_tile_h_group : nullptr, key);
    add(tree, m, "Tile Vertically", can ? act_tile_v_group : nullptr, key);
    separator(tree, m);
    xd_menu_item *it = add(tree, m, "Minimize Group", act_minimize_group, key);
    it->glyph = XD_MENU_GLYPH_MINIMIZE;
    separator(tree, m);
    it = add(tree, m, "Close Group", act_close_group, key);
    it->glyph = XD_MENU_GLYPH_CLOSE;
    it->bold = true;
    return m;
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
    xd_rect r = bar->task_rects[hit];
    xd_rect anchor = {(int)(r.x / bar->scale), (int)(r.y / bar->scale),
                      (int)(r.w / bar->scale), (int)(r.h / bar->scale)};
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
    xd_menu_model *m = xd_menu_model_new(tree);
    bool can_arrange = shell->arranger != nullptr;
    if (quick) {
        /* XP's toolbar menu. */
        xd_menu_item *view = xd_menu_add(tree, m, XD_MENU_SUBMENU, "View", nullptr);
        view->submenu = xd_menu_model_new(tree);
        add(tree, view->submenu, "Large Icons", act_quick_large, nullptr)->checked =
            shell->settings.quick_large;
        add(tree, view->submenu, "Small Icons", act_quick_small, nullptr)->checked =
            !shell->settings.quick_large;
        add(tree, m, "Open Folder", act_quick_folder, nullptr);
        add(tree, m, "Show Text", act_quick_text, nullptr)->checked =
            shell->settings.quick_show_text;
        add(tree, m, "Show Title", act_quick_title, nullptr)->checked =
            shell->settings.quick_show_title;
        add(tree, m, "Close Toolbar", act_quick_launch, nullptr);
        separator(tree, m);
    }
    if (clock) {
        xd_menu_item *it = add(tree, m, "Adjust Date/Time", nullptr, nullptr);
        it->command = xd_shell_own_command(shell, "xd-timedate", nullptr);
        it->disabled = false;
        it->bold = true;
        separator(tree, m);
    }

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
    xd_menu_item *it = add(tree, m, "Restore", restorable ? act_restore : nullptr, toplevel_ref(t));
    it->glyph = XD_MENU_GLYPH_RESTORE;
    it->bold = restorable;
    it->mnemonic = 'r';
    /* Fixed-size windows ignore Size (the compositor knows which they are). */
    bool movable = !restorable && shell->arranger != nullptr;
    /* The system menu's letters: &Move, &Size, Mi&nimize, Ma&ximize. */
    add(tree, m, "Move", movable ? act_move : nullptr, toplevel_ref(t))->mnemonic = 'm';
    add(tree, m, "Size", movable ? act_size : nullptr, toplevel_ref(t))->mnemonic = 's';
    it = add(tree, m, "Minimize", t->minimized ? nullptr : act_minimize, toplevel_ref(t));
    it->glyph = XD_MENU_GLYPH_MINIMIZE;
    it->mnemonic = 'n';
    it = add(tree, m, "Maximize", t->maximized ? nullptr : act_maximize, toplevel_ref(t));
    it->glyph = XD_MENU_GLYPH_MAXIMIZE;
    it->mnemonic = 'x';
    separator(tree, m);
    it = add(tree, m, "Close", act_close, toplevel_ref(t));
    it->glyph = XD_MENU_GLYPH_CLOSE;
    it->accel = "Alt+F4";
    it->mnemonic = 'c';
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
    /* That monitor's taskbar and desktop; without a taskbar there ("Show
     * the taskbar on all displays" off), one shown elsewhere takes the
     * keyboard for the menu. */
    xd_taskbar *here = nullptr, *keys = nullptr, *b;
    wl_list_for_each(b, &shell->taskbars, link) {
        if (b->output == output)
            here = b;
        if (b->configured && (keys == nullptr || b->output == output))
            keys = b;
    }
    xd_toplevel *focused = nullptr, *t;
    wl_list_for_each(t, &shell->toplevels, link) {
        if (t->activated)
            focused = t;
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
    keys->dirty = true;
}

/* The Windows key or Ctrl+Esc. */
static void handle_start_menu(void *data, [[maybe_unused]] struct xd_window_arranger_v1 *a,
                              struct wl_output *output, uint32_t serial) {
    xd_shell *shell = data;
    /* This monitor's taskbar, else (without one there) the first's. */
    xd_taskbar *b, *fallback = nullptr;
    wl_list_for_each(b, &shell->taskbars, link) {
        if (b->output == output && b->configured) {
            xd_taskbar_toggle_start(b, serial);
            return;
        }
        if (fallback == nullptr && b->configured)
            fallback = b;
    }
    if (fallback != nullptr)
        xd_taskbar_toggle_start(fallback, serial);
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

static const struct xd_window_arranger_v1_listener arranger_listener = {
    .start_menu = handle_start_menu,
    .window_menu = handle_window_menu,
    .attention = handle_attention,
    .keyboard_layout = handle_keyboard_layout,
};

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
