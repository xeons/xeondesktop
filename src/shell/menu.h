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
typedef struct xd_menu_tree xd_menu_tree;

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
    const char *key; /* into label: that letter, underlined (xd_menu_add_keyed) */
    bool highlight; /* XP's newly installed programs (and their folders) */
    /* Jump lists (Windows 7's look): in the bottom band; a pushpin for
     * the icon (1: lying, 2: pushed in); the pin button shown at the
     * item's right end while hovered, which runs `pin` (with
     * action_data) and leaves the list open. */
    bool band;
    uint8_t pushpin;
    void (*pin)(xd_shell *shell, void *data);
    bool pinned;
    /* Its right-click menu (a jump list item's), built in `tree` from
     * action_data when asked; nullptr: none (a program's comes from `app`). */
    xd_menu_model *(*context)(xd_shell *shell, xd_menu_tree *tree, void *data);
    /* In a right-click menu: runs with just that menu closed, leaving the
     * one it came from open (a jump list's "Pin to this list"). */
    bool keep_open;
    /* Jump list: dragged onto another item with the same `move` (the
     * pinned ones), move(shell, data, the other's data) runs, which shows
     * the list again in its new order with this item where that one was. */
    void (*move)(xd_shell *shell, void *data, void *onto);
} xd_menu_item;

/* The pale orange behind XP's newly installed programs. */
static constexpr uint32_t XD_NEW_PROGRAM_COLOR = 0xFCE3A4;

struct xd_menu_model {
    xd_menu_item *items;
    size_t count, cap;
    bool large;                 /* 32px icons (Start menu top level) */
    bool jumplist;              /* Windows 7's jump list look (Aero's taskbar) */
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
/* As xd_menu_add, with the label written as Windows writes it, "Mi&nimize":
 * the letter after '&' is the item's access key, underlined ("&&" is a
 * plain '&'). */
xd_menu_item *xd_menu_add_keyed(xd_menu_tree *tree, xd_menu_model *model, xd_menu_kind kind,
                                const char *label, const char *icon);
/* A program's right-click menu, after its pinning items: Send To (Desktop
 * (create shortcut), My Documents) and Properties, as XP's and Windows 7's
 * Start menus have. The app belongs to shell->apps. */
void xd_menu_add_program_items(xd_menu_tree *tree, xd_menu_model *model, const xd_app *app);
/* After a program's Open, on a laptop with two graphics cards
 * (switcheroo-control): "Run with dedicated graphics card" (or the
 * integrated one, for a program that starts on the dedicated one) and
 * "Always run with dedicated graphics card". Nothing with one card. */
void xd_menu_add_gpu_items(xd_menu_tree *tree, xd_menu_model *model, const xd_app *app);
/* Whether menus underline their access keys now: always, unless they're
 * hidden until the keyboard is used (Display Properties' Effects), as in
 * Windows; then once a menu was opened or used from the keyboard. */
bool xd_menu_cues(const xd_shell *shell);
/* The keyboard opened or used the menus: they show their underlines. */
void xd_menu_show_cues(xd_shell *shell);

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
    bool hot_pin;      /* jump list: the pointer is on the hot item's pin button */
    bool moving;       /* jump list: the pressed item is being dragged (`move`) */
    int band_y;        /* jump list: the bottom band's top (logical) */
    struct xd_taskbar *bar; /* opened from a taskbar button: its taskbar and the */
    xd_rect anchor;         /* button (surface-local logical), to reposition */
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
/* Shows `model` in place of the menu's (a jump list after a pin): laid
 * out again and moved to fit where it opened. */
void xd_menu_replace_model(xd_menu *menu, const xd_menu_model *model);
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
