/* SPDX-License-Identifier: MIT
 * Popup menus (Start menu and its submenus, later context menus).
 *
 * A menu model is a static description; an open xd_menu is one xdg_popup
 * showing a model. Open menus form a chain root -> child -> child.
 */
#ifndef XD_SHELL_MENU_H
#define XD_SHELL_MENU_H

#include "../core/alloc.h"
#include "../theme/theme.h"
#include "../ui/shm.h"
#include "../ui/protocols.h"
#include "apps.h"

typedef struct xd_shell xd_shell;

typedef enum xd_menu_kind : uint8_t {
    XD_MENU_ACTION,
    XD_MENU_SUBMENU,
    XD_MENU_SEPARATOR,
    XD_MENU_HEADER, /* a section's title (a jump list's "Recent"); not an item */
} xd_menu_kind;

typedef struct xd_menu_model xd_menu_model;

typedef struct xd_menu_item {
    xd_menu_kind kind;
    bool disabled;
    const char *label;
    const char *icon;          /* icon theme name or path; may be nullptr */
    const char *fallback_icon; /* tried if `icon` isn't found */
    xd_menu_model *submenu;    /* XD_MENU_SUBMENU */
    /* XD_MENU_ACTION: shell command line. XD_MENU_SUBMENU: run when the
     * item itself is clicked (Control Panel opens its folder; hovering
     * still cascades). */
    const char *command;
    const xd_app *app;         /* XD_MENU_ACTION: launch this app (instead of command) */
    /* XD_MENU_ACTION: run in the shell instead (after the menus close). */
    void (*action)(xd_shell *shell, void *data);
    void *action_data;
    bool checked;         /* check mark (toggles such as Lock the Taskbar) */
    bool bold;            /* the default item */
    xd_menu_glyph glyph;  /* window-menu glyph in the icon column */
    const char *accel;    /* shortcut text, e.g. "Alt+F4" */
    /* The letter that picks it (lower case), as an underlined letter does
     * in Windows; 0: the label's first letter. */
    uint32_t mnemonic;
    bool highlight; /* XP's newly installed programs (and their folders) */
} xd_menu_item;

/* The pale orange behind XP's newly installed programs. */
static constexpr uint32_t XD_NEW_PROGRAM_COLOR = 0xFCE3A4;

struct xd_menu_model {
    xd_menu_item *items;
    size_t count, cap;
    bool large;                 /* 32px icons (Start menu top level) */
    const char *banner_product; /* Windows 2000 side banner, or nullptr */
    const char *banner_edition;
};

/* Owns every model and string of one menu hierarchy. */
typedef struct xd_menu_tree {
    xd_arena arena;
    xd_menu_model **models;
    size_t count, cap;
} xd_menu_tree;

void xd_menu_tree_init(xd_menu_tree *tree);
void xd_menu_tree_free(xd_menu_tree *tree);
xd_menu_model *xd_menu_model_new(xd_menu_tree *tree);
/* Appends an item; strings are copied into the tree. Returns the item so
 * callers can set submenu/command/disabled. */
xd_menu_item *xd_menu_add(xd_menu_tree *tree, xd_menu_model *model, xd_menu_kind kind,
                          const char *label, const char *icon);

typedef struct xd_menu {
    xd_shell *shell;
    const xd_menu_model *model;
    struct xd_menu *parent;
    struct xd_menu *child;
    int child_index; /* item whose submenu is open, or -1 */

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    xd_shm_ring ring;

    int width, height; /* logical */
    int *item_y;       /* logical top of each item */
    int *item_h;
    int *item_xs;      /* logical left of each item (columns) */
    int item_x, item_w; /* the first column's left; every column's width */
    double scale;
    bool configured;
    bool shown; /* first frame committed: the compositor knows where it is */
    bool dirty;
    int hot; /* highlighted item or -1 */
    int pressed; /* item the left button went down on, or -1 */
    double ptr_x, ptr_y;     /* the pointer (logical): where a drag starts */
    double press_x, press_y; /* ...and where the press was */
    /* A program's right-click menu (Open, Pin to Start menu) over this
     * menu's item child_index: its parent ignores the pointer meanwhile.
     * It owns its model. */
    bool context;
    xd_menu_tree *own_tree;
} xd_menu;

/* Opens `model` as a popup of the taskbar's layer surface, anchored to
 * `anchor` (logical, taskbar-local). serial 0 opens without a grab (used
 * for debugging only). */
xd_menu *xd_menu_open_root(xd_shell *shell, const xd_menu_model *model,
                           struct zwlr_layer_surface_v1 *layer, xd_rect anchor, uint32_t serial);
/* As xd_menu_open_root, growing downwards from the anchor if asked. */
xd_menu *xd_menu_open_root_dir(xd_shell *shell, const xd_menu_model *model,
                               struct zwlr_layer_surface_v1 *layer, xd_rect anchor,
                               uint32_t serial, bool downwards);
void xd_menu_close_all(xd_shell *shell);
/* Opens `model` as a submenu of any popup surface (the Windows 7 Start
 * menu's ▸ items), to the right of `anchor` (parent-local, logical). The
 * caller owns it and closes it with xd_menu_destroy. */
xd_menu *xd_menu_open_attached(xd_shell *shell, const xd_menu_model *model,
                               struct xdg_surface *parent, xd_rect anchor, uint32_t serial);
void xd_menu_destroy(xd_menu *menu);
/* Keyboard navigation in the chain starting at root. */
bool xd_menu_key_chain(xd_menu *root, uint32_t keysym);
/* Opens the submenu of item `index`, closing any other open child. */
xd_menu *xd_menu_open_child(xd_menu *menu, int index);

xd_menu *xd_menu_for_surface(xd_shell *shell, struct wl_surface *surface);
void xd_menus_render_dirty(xd_shell *shell);

void xd_menu_pointer_motion(xd_menu *menu, double x, double y);
void xd_menu_pointer_leave(xd_menu *menu);
void xd_menu_pointer_button(xd_menu *menu, uint32_t button, bool pressed);
/* Keyboard navigation of the deepest open menu. Returns true if used. */
bool xd_menu_key(xd_shell *shell, uint32_t keysym);
/* The hover delay elapsed. */
void xd_menu_timer_fired(xd_shell *shell);

#endif
