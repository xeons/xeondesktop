/* SPDX-License-Identifier: MIT
 * xd-shell: taskbar (and later Start menu, tray, desktop).
 */
#ifndef XD_SHELL_SHELL_H
#define XD_SHELL_SHELL_H

#include <sys/types.h>
#include <wayland-client.h>

#include "../core/wlist.h"

#include <xkbcommon/xkbcommon.h>

#include "../settings/settings.h"
#include "../theme/theme.h"
#include "../ui/shm.h"
#include "apps.h"
#include "../ui/icons.h"
#include "menu.h"
#include "../ui/protocols.h"

typedef struct xd_shell xd_shell;
typedef struct xd_tooltip xd_tooltip;
typedef struct xd_tray xd_tray;
typedef struct xd_tray_item xd_tray_item;
typedef struct xd_dnd xd_dnd;
typedef struct xd_start7 xd_start7;
typedef struct xd_sleep xd_sleep;
typedef struct xd_primary xd_primary;
typedef struct xd_account xd_account;
typedef struct xd_shellbus xd_shellbus;
typedef struct xd_superbar xd_superbar;

typedef struct xd_early_output {
    struct wl_output *output;
    uint32_t name;
} xd_early_output;

/* A window as reported by the compositor. */
typedef struct xd_toplevel {
    struct wl_list link; /* xd_shell.toplevels, in creation order */
    xd_shell *shell;
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    uint64_t id; /* never reused, unlike the address */
    char *title;
    char *app_id;
    char *icon;     /* its application's Icon key, from app_id; may be nullptr */
    char *app_name; /* its application's name, for a group's button; may be nullptr */
    char *desktop_id; /* its application's desktop file id; may be nullptr */
    uint64_t active_seq; /* when it was last activated (xd_shell.active_seq) */
    bool activated;
    bool minimized;
    bool maximized;
    /* Asked for attention: the button flashes `flashes` more half-cycles
     * (lit on even counts), then stays lit until the window is activated. */
    bool attention;
    int flashes;
    /* Its taskbar button, as last told to the compositor (logical,
     * surface-local), for the minimize animation. */
    struct wl_surface *button_surface;
    int button_x, button_y, button_w, button_h;
} xd_toplevel;

/* Quick Launch shortcuts. */
typedef struct xd_quicklaunch {
    xd_arena arena;
    xd_app *items;
    const char **files; /* items' file names in the folder (arena) */
    size_t count;
    int watch_fd;
} xd_quicklaunch;

/* The desktop background of one output. */
typedef struct xd_desktop {
    xd_shell *shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    int width, height; /* logical */
    double scale;
    bool configured;
    bool closed;
    bool dirty;
    /* The colour and wallpaper at the buffer's size: scaling the wallpaper
     * is the slow part, and icons redraw over it on every drag motion. */
    cairo_surface_t *background;
    double background_scale;
    unsigned background_gen;
} xd_desktop;

/* Hover/press targets on a taskbar: task buttons are >= 0; Quick Launch
 * button i is XD_HIT_QUICK - i. */
enum {
    XD_HIT_NONE = -1,
    XD_HIT_START = -2,
    XD_HIT_TRAY = -3,
    XD_HIT_PAGE_UP = -4, /* task button pages, when they don't all fit */
    XD_HIT_PAGE_DOWN = -5,
    XD_HIT_QUICK_BAND = -6, /* Quick Launch, between or around its buttons */
    XD_HIT_TRAY_CHEVRON = -7, /* shows or folds inactive notification icons */
    XD_HIT_PEEK = -8,         /* Aero: Show desktop, at the far end (hover peeks) */
    XD_HIT_QUICK = -100,     /* Quick Launch button i: XD_HIT_QUICK - i */
    XD_HIT_TRAY_ICON = -1000, /* notification area icon i: XD_HIT_TRAY_ICON - i */
};
static inline bool xd_hit_is_quick(int hit) {
    return hit <= XD_HIT_QUICK && hit > XD_HIT_TRAY_ICON;
}

/* Aero: one of Windows 7's taskbar buttons (superbar.c). */
typedef struct xd_bar_button {
    const char *key;     /* its program: a desktop id, else "app:" app id, else "win:" id */
    xd_toplevel *window; /* a window's own (labelled) button; nullptr: the program's */
    size_t windows;      /* windows it stands for; 0: pinned, not running */
    bool pinned;
} xd_bar_button;

typedef struct xd_taskbar {
    struct wl_list link; /* xd_shell.taskbars */
    xd_shell *shell;
    uint32_t output_global; /* registry name of the wl_output */
    struct wl_output *output;
    xd_desktop *desktop; /* the same output's background */
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    xd_shm_ring ring;

    int width, height; /* logical, from configure */
    double scale;
    bool configured;
    bool dirty;

    /* Layout from the last render, in device px. task_views[i] is the
     * window shown by task_rects[i]. */
    xd_rect start_rect;
    xd_rect peek_rect; /* Aero's Show desktop corner; empty in other styles */
    unsigned int peek_timer; /* GLib source id */
    bool peeking;
    xd_rect tray_rect;
    size_t quick_count;
    xd_rect quick_band; /* the whole Quick Launch toolbar, grips included */
    size_t quick_cap;
    xd_rect *quick_rects;
    size_t task_count;
    size_t task_cap;
    xd_rect *task_rects;
    xd_toplevel **task_views;  /* a group's button shows its first window */
    size_t *task_counts;       /* windows behind each button (>1: a group) */
    xd_toplevel **entry_views; /* layout scratch, task_cap long */
    size_t *entry_counts;
    /* Aero: Windows 7's buttons, one per program; task_views[i] is one of
     * button i's windows (nullptr for a pinned program not running). */
    bool superbar;
    xd_bar_button *bar_buttons; /* task_cap long */
    struct _GStringChunk *bar_keys; /* their keys */
    char *drag_key;             /* the program being dragged along, or nullptr */
    /* Pages of task buttons when they don't fit at their minimum width. */
    size_t page, pages;
    uint64_t page_active_id; /* active window when last laid out */
    double wheel;            /* scrolling not yet turned into a page */
    int64_t clock_click_ms;  /* for double-clicks on the clock */
    /* Quick Launch drops and rearranging: where the insertion mark is
     * (-1: none), and the button being dragged along (-1: none). */
    int drop_insert;
    int quick_drag;
    double press_x, press_y;
    /* Unlocked: dragging the taskbar to another edge, or its inner edge
     * to resize it. */
    enum { XD_BAR_IDLE, XD_BAR_MOVING, XD_BAR_SIZING } grab;
    xd_rect page_up_rect, page_down_rect;
    /* Notification area icons, left of the clock. */
    size_t tray_count, tray_cap;
    xd_rect *tray_rects;
    size_t *tray_index;      /* tray_rects[i] shows xd_tray_get(tray_index[i]) */
    xd_rect tray_chevron;    /* empty if no icon is folded away */
    bool tray_expanded;      /* the chevron was clicked: all icons show */
    unsigned tray_fold_timer; /* folds them again after a while */

    int hot;     /* XD_HIT_* or a task index */
    double pointer_x, pointer_y; /* last pointer position, logical */
    int pressed; /* same, while a button is held */
    bool tooltip_off; /* clicked: no tooltip until the pointer moves on */
    bool tucked;      /* Auto-hide: pushed down to a 2px strip */
} xd_taskbar;

struct xd_shell {
    xd_visual_style style;
    xd_scheme scheme;
    const char *scheme_path; /* --scheme given; passed on to dialogs */
    bool cli_style;          /* --style given; passed on to dialogs */
    xd_settings settings;    /* desktop.ini */
    int settings_fd;
    cairo_surface_t *wallpaper; /* loaded settings.wallpaper, or nullptr */
    unsigned background_gen;    /* bumped when settings change the desktop's background */
    const char *terminal;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct ext_idle_notifier_v1 *idle_notifier;
    struct ext_idle_notification_v1 *saver_note;
    struct ext_idle_notification_v1 *monitor_note;
    struct ext_idle_notification_v1 *standby_note;   /* Power Options' System standby */
    struct ext_idle_notification_v1 *hibernate_note; /* and System hibernates */
    bool on_battery; /* UPower's: the timers are the "Running on batteries" ones */
    xd_sleep *sleep; /* logind (sleep.c), or nullptr */
    xd_account *account; /* full name, Switch User (account.c) */
    struct zwlr_output_power_manager_v1 *power_manager;
    int saver_pidfd; /* the running screen saver, or -1 */
    bool monitors_off;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    struct wl_surface *kb_surface; /* ours with the keyboard focus, if any */
    uint32_t kb_serial;            /* of its keyboard enter */
    struct xdg_wm_base *wm_base;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wp_viewporter *viewporter;
    struct xdg_activation_v1 *activation;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape;

    struct wl_list taskbars;
    struct wl_list toplevels;
    uint64_t next_toplevel_id;
    uint64_t active_seq; /* counts window activations */
    xd_superbar *superbar; /* Windows 7's pinned programs */
    struct _GHashTable *own_commands; /* xd_shell_own_command's results */

    xd_taskbar *pointer_bar; /* taskbar under the pointer */
    xd_menu *pointer_menu;   /* menu under the pointer */

    /* Start menu. */
    xd_icons *icons;
    xd_app_list apps;
    xd_app_list control_apps; /* Control Panel's applets */
    /* Newly installed programs (desktop ids) not opened yet, for XP's
     * highlight; recomputed as the Start menu opens. */
    struct _GHashTable *new_apps;
    /* Programs installed or removed since: reloaded once no menu shows
     * them (menus point into `apps`). */
    struct _GAppInfoMonitor *app_monitor;
    unsigned apps_reload; /* GLib idle source, or 0 */
    bool apps_stale;
    xd_menu_tree menu_tree;
    xd_menu_tree context_tree; /* the open right-click menu's models */
    bool context_menu;         /* the open root menu is a right-click menu */
    struct xd_window_arranger_v1 *arranger; /* Cascade/Tile (nullptr: not offered) */
    int last_arrange;          /* mode of the last Cascade/Tile, -1 if undone */
    xd_menu_model *start_model;
    size_t start_docs_index; /* Documents in the Windows 2000 menu */
    xd_menu_tree docs_tree;  /* its submenu, rebuilt as the menu opens */
    xd_menu *start_menu;    /* open root menu (2000 style), or nullptr */
    xd_start7 *start7;      /* open Windows 7 menu, or nullptr */
    bool pointer_start7;    /* pointer is over start7 */
    bool pointer_flyout;         /* pointer is over the volume flyout */
    bool eat_release; /* the press closed a menu: its release goes nowhere */
    xd_desktop *pointer_desktop; /* a desktop under the pointer */
    struct xd_deskicons *deskicons;
    uint32_t enter_serial;  /* for cursor shapes */
    uint32_t button_serial; /* of the last button press (drags from the Start menu) */
    uint32_t cursor;        /* the shape set since, wp_cursor_shape_device_v1 */
    double flyout_x, flyout_y;
    bool pending_start7;    /* start7 waits for the hover delay */
    xd_taskbar *start_bar;  /* taskbar whose Start button opened it */
    uint32_t menu_serial;   /* input serial the menu grab was made with */
    int menu_timer_fd;      /* hover delay */
    int flash_timer_fd;     /* task buttons flashing for attention */
    /* Taskbar tooltips: the one shown, and the hover delay before one. */
    xd_tooltip *tooltip;
    int tooltip_timer_fd;
    int autohide_timer_fd; /* tucks Auto-hide taskbars away after the pointer leaves */
    /* Key repeat (the compositor's rate and delay; clients repeat keys). */
    int repeat_timer_fd;
    int32_t repeat_rate, repeat_delay; /* per second; ms */
    uint32_t repeat_key;               /* 0: none */
    uint32_t repeat_serial;
    xd_tray *tray;         /* StatusNotifierItem host */
    xd_dnd *dnd;           /* the taskbar as a drop target */
    struct wl_data_device_manager *data_device_manager;
    int64_t tooltip_shown_ms, tooltip_hidden_ms;
    xd_menu *pending_menu;  /* menu waiting for the hover delay */

    char clock[32];
    bool running;
    /* --taskbar-only: no desktop (wallpaper, icons), for another
     * compositor's session. */
    bool taskbar_only;
    /* Not under xd-compositor (XEONDESKTOP_PID isn't the compositor we're
     * connected to). */
    bool standalone;
    xd_shellbus *shellbus; /* org.xeondesktop.Shell (shellbus.c) */

    xd_quicklaunch quicklaunch;
    /* Windows minimized by Show Desktop, restored by the next click. */
    xd_toplevel **desktop_hidden;
    size_t desktop_hidden_n, desktop_hidden_cap;

    xd_primary *primary; /* which monitor is the primary one (primary.c) */

    /* Outputs announced before startup finished binding globals. */
    bool started;
    xd_early_output early_outputs[32];
    size_t early_count;
};

/* taskbar.c */
void xd_taskbar_create(xd_shell *shell, struct wl_output *output, uint32_t global);
void xd_taskbar_destroy(xd_taskbar *bar);
void xd_taskbar_render(xd_taskbar *bar);
void xd_taskbars_mark_dirty(xd_shell *shell);
void xd_taskbars_render_dirty(xd_shell *shell);
void xd_taskbar_pointer_motion(xd_taskbar *bar, double x, double y);
void xd_taskbar_pointer_leave(xd_taskbar *bar);
void xd_taskbar_pointer_button(xd_taskbar *bar, uint32_t button, bool pressed,
                               uint32_t serial);
/* Creates or removes taskbar surfaces ("all displays" or only the first
 * monitor's) and applies [Taskbar]'s layer and Auto-hide. */
void xd_taskbars_update(xd_shell *shell);
/* Auto-hide: slide the taskbar out (pointer, Start menu), and back in a
 * moment after nothing uses it. */
void xd_taskbar_reveal(xd_taskbar *bar);
void xd_taskbars_schedule_tuck(xd_shell *shell);
void xd_taskbars_tuck_timer(xd_shell *shell);
/* Positions a popup opening from a taskbar button (anchor: surface-local
 * logical): up from a bottom taskbar, down from a top one, beside a side
 * one. */
void xd_taskbar_popup_position(const xd_taskbar *bar, xd_rect anchor,
                               struct xdg_positioner *pos);
/* menu.c: opens from a taskbar button (anchor: surface-local logical) the
 * way the taskbar's edge wants: up, down or beside it. */
xd_menu *xd_menu_open_root_bar(xd_shell *shell, const xd_menu_model *model, xd_taskbar *bar,
                               xd_rect anchor, uint32_t serial);
/* The taskbar's thickness in logical px, from [Taskbar]. */
int xd_taskbar_thickness(const xd_shell *shell);
/* Where a drop at (lx, ly) (logical) goes in Quick Launch: before
 * button n; -1 if not over Quick Launch. */
int xd_taskbar_quick_insert_at(const xd_taskbar *bar, double lx, double ly);
xd_taskbar *xd_taskbar_for_surface(xd_shell *shell, struct wl_surface *surface);
/* dnd.c */
void xd_dnd_init(xd_shell *shell, struct wl_data_device_manager *manager);
void xd_dnd_finish(xd_shell *shell);
/* The clipboard's files (URIs), for the desktop's Cut, Copy and Paste. */
void xd_clipboard_set_files(xd_shell *shell, char *const *uris, bool cut, uint32_t serial);
bool xd_clipboard_has_files(xd_shell *shell);
/* `uris` (nullptr-terminated, or nullptr when there are none) is only
 * valid during the call. */
typedef void (*xd_clipboard_got)(xd_shell *shell, char **uris, bool cut, void *user);
void xd_clipboard_get_files(xd_shell *shell, xd_clipboard_got got, void *user);
/* After pasting what was cut: the clipboard is emptied, as in Windows. */
void xd_clipboard_clear_cut(xd_shell *shell);
/* Starts dragging files (the desktop's icons) from `origin`, with `icon`
 * as the drag image; false if it can't. */
bool xd_dnd_start_drag(xd_shell *shell, char *const *uris, struct wl_surface *origin,
                       struct wl_surface *icon, uint32_t serial);
/* Starts dragging a program from the Start menus (its desktop file, which
 * Quick Launch and the desktop take as a shortcut), from the press that
 * shell->button_serial holds. scale: the origin's, for the drag image. */
bool xd_dnd_start_app_drag(xd_shell *shell, const xd_app *app, struct wl_surface *origin,
                           double scale);
/* Shows the previous (delta < 0) or next page of task buttons. */
void xd_taskbar_page(xd_taskbar *bar, int delta);
/* Gives the taskbar keyboard focus; call before opening a menu popup. */
void xd_taskbar_take_keyboard(xd_taskbar *bar);
/* Opens the Start menu above this taskbar's Start button. */
void xd_taskbar_open_start_menu(xd_taskbar *bar, uint32_t serial);
/* Opens this taskbar's Start menu (in the chosen style), or closes it. */
void xd_taskbar_toggle_start(xd_taskbar *bar, uint32_t serial);
/* taskmenu.c: right-click menus (the taskbar's, or a task button's window
 * menu when `hit` is a task index). x, y: logical, taskbar-local. */
void xd_taskbar_context_menu(xd_taskbar *bar, int hit, double x, double y, uint32_t serial);
/* Our own notification area applets (volume, network, battery,
 * Bluetooth). Every callback gets the applet's data. */
typedef struct xd_sysicon_ops {
    const char *(*icon)(void *data); /* icon theme name; nullptr hides the icon */
    char *(*tooltip)(void *data);    /* g_free'd; may be nullptr */
    /* Fills the icon's menu (strings and action data in `tree`). */
    void (*menu)(void *data, xd_menu_tree *tree, xd_menu_model *menu);
    /* Left click; nullptr: the menu. anchor: surface-local logical. */
    void (*activate)(void *data, xd_taskbar *bar, xd_rect anchor, uint32_t serial);
    void (*scroll)(void *data, double delta); /* wheel steps, + is down */
    /* Draws the icon into r (device px); nullptr: the `icon` theme icon. */
    void (*draw)(void *data, const xd_theme *t, cairo_t *cr, xd_rect r);
    void (*destroy)(void *data);
} xd_sysicon_ops;

/* tray.c: the notification area's icons (StatusNotifierItem). */
enum { XD_TRAY_BUTTON_LEFT, XD_TRAY_BUTTON_MIDDLE, XD_TRAY_BUTTON_RIGHT };
void xd_tray_init(xd_shell *shell);
void xd_tray_finish(xd_shell *shell);
/* Icons to show, in order. */
size_t xd_tray_count(const xd_shell *shell);
xd_tray_item *xd_tray_get(const xd_shell *shell, size_t index);
cairo_surface_t *xd_tray_item_icon(xd_shell *shell, xd_tray_item *item, int px);
char *xd_tray_item_tooltip(const xd_tray_item *item); /* g_free */
/* Hide inactive icons: a program's icon that's been quiet a while. */
bool xd_tray_item_inactive(const xd_tray_item *item);
/* Folded behind the chevron: [Notifications]' rule, else inactivity. */
bool xd_tray_item_folded(const xd_shell *shell, const xd_tray_item *item);
/* The id [Notifications] rules use: Id, or our applet's name. */
const char *xd_tray_item_id(const xd_tray_item *item);
/* Draws the item's icon if it draws its own (our applets); else false. */
bool xd_tray_item_draw(xd_tray_item *item, const xd_theme *t, cairo_t *cr, xd_rect r);
void xd_tray_item_scroll(xd_shell *shell, xd_tray_item *item, double delta);
/* Adds one of our applets (after xd_tray_init); `name` keys it. */
void xd_tray_add_sysicon(xd_shell *shell, const char *name, const xd_sysicon_ops *ops,
                         void *data);
/* An applet's icon or state changed: redraw. */
void xd_tray_changed(xd_shell *shell);
/* A click: Activate / SecondaryActivate, or its menu by `anchor`
 * (surface-local logical) on `bar`. */
void xd_tray_item_click(xd_shell *shell, xd_tray_item *item, uint32_t button, xd_taskbar *bar,
                        xd_rect anchor, uint32_t serial);
/* taskmenu.c: a grouped button's menu of its windows. */
void xd_taskbar_group_menu(xd_taskbar *bar, int hit, uint32_t serial);
/* Listens for the compositor's window menu requests. */
void xd_taskmenu_bind(xd_shell *shell);
/* tooltip.c: a tooltip popup of `layer`, next to `anchor` (surface-local,
 * logical), replacing any other. */
void xd_tooltip_show(xd_shell *shell, struct zwlr_layer_surface_v1 *layer, xd_rect anchor,
                     const char *text, double scale);
void xd_tooltip_hide(xd_shell *shell);
void xd_tooltip_render_dirty(xd_shell *shell);
/* Starts (or restarts) the hover delay; xd_taskbar_tooltip_timer runs
 * when it's over. */
void xd_tooltip_arm(xd_shell *shell);
void xd_tooltip_disarm(xd_shell *shell);
int64_t xd_tooltip_now_ms(void);
/* taskbar.c: the hover delay ended: show the hovered item's tooltip. */
void xd_taskbar_tooltip_timer(xd_shell *shell);
/* toplevels.c: a window asked for attention (xd-window-arranger-v1). */
void xd_toplevel_attention(xd_shell *shell, struct zwlr_foreign_toplevel_handle_v1 *handle);
/* toplevels.c: the flash timer fired. */
void xd_toplevels_flash_tick(xd_shell *shell);
/* Forget a window that's going away. */
void xd_taskbars_forget(xd_shell *shell, xd_toplevel *toplevel);
/* Drop hit-test layout after Quick Launch changed (indices shift). */
void xd_taskbars_forget_layout(xd_shell *shell);

/* startmenu.c: Programs, one submenu per folder (also XP's All Programs).
 * highlight_new: newly installed programs and their folders are lit, as
 * XP's All Programs does. */
xd_menu_model *xd_start_programs_model(xd_shell *shell, xd_menu_tree *tree, bool highlight_new);
/* startmenu.c: newly installed programs. A program first seen after the
 * first run (state file start-seen) is new until it's opened. Updates
 * shell->new_apps. */
void xd_start_menu_note_programs(xd_shell *shell);
bool xd_start_menu_is_new(const xd_shell *shell, const char *id);
/* A program was opened: it isn't new any more. */
void xd_start_menu_program_opened(const char *id);
/* Watches for programs being installed or removed (GAppInfoMonitor). */
void xd_start_menu_watch_programs(xd_shell *shell);
void xd_start_menu_unwatch_programs(xd_shell *shell);
/* Reloads the programs, if they changed, once no menu is open (from an
 * idle callback: an item being activated may point into the lists). */
void xd_start_menu_reload_later(xd_shell *shell);
/* startmenu.c: Control Panel's applets, by name (all three Start menus). */
xd_menu_model *xd_start_control_model(xd_shell *shell, xd_menu_tree *tree);

/* start7.c: the Windows XP and Windows 7 style Start menus */
/* xp: the Windows XP layout (user header, cascading All Programs, Log Off
 * and Turn Off Computer) instead of Windows 7's. */
void xd_start7_open(xd_taskbar *bar, uint32_t serial, bool xp);
void xd_start7_close(xd_shell *shell);
bool xd_start7_owns(xd_shell *shell, struct wl_surface *surface);
void xd_start7_render_dirty(xd_shell *shell);
xd_menu *xd_start7_submenu(xd_shell *shell);
void xd_start7_pointer_motion(xd_shell *shell, double x, double y);
void xd_start7_pointer_leave(xd_shell *shell);
void xd_start7_pointer_button(xd_shell *shell, uint32_t button, bool pressed);
void xd_start7_pointer_axis(xd_shell *shell, double value);
bool xd_start7_key(xd_shell *shell, uint32_t keysym, const char *utf8, bool ctrl_or_alt);
void xd_start7_timer_fired(xd_shell *shell);
/* Records a launch for the "frequently used" list, and when it was (Add or
 * Remove Programs' Last Used On). */
void xd_start7_count_launch(const char *app_id);
/* The account's full name changed (account.c). */
void xd_start7_user_changed(xd_shell *shell);
/* Pinned to the Start menu (start-pinned), and whether the list is full. */
bool xd_start7_pinned(const xd_shell *shell, const char *app_id, bool *full);
/* Pin to / Unpin from Start menu. */
void xd_start7_set_pinned(const xd_shell *shell, const char *app_id, bool pin);

/* startmenu.c */
/* Command line for one of our programs (next to xd-shell in a build tree). */
const char *xd_shell_own_command(xd_shell *shell, const char *program, const char *extra);
/* Path of one of our programs (next to xd-shell, else the bare name for
 * PATH). g_free it. */
char *xd_shell_own_program(const char *program);
/* Whether `program` is in PATH. */
bool xd_shell_have_program(const char *program);
/* primary.c: the primary monitor (the one at 0,0) has the first
 * taskbar. bind: zxdg_output_manager_v1; track/forget: each output, with
 * its taskbar. */
void xd_primary_bind(xd_shell *shell, struct wl_registry *reg, uint32_t name, uint32_t version);
void xd_primary_track(xd_shell *shell, struct wl_output *output);
void xd_primary_forget(xd_shell *shell, struct wl_output *output);
void xd_primary_finish(xd_shell *shell);
/* sleep.c: standby and hibernate through logind, and locking the screen
 * before sleeping ([Power] PasswordOnResume). */
void xd_sleep_init(xd_shell *shell);
void xd_sleep_finish(xd_shell *shell);
/* sounds.c: Device Connect and Disconnect, and Power Options' battery
 * alarms (their sound, message and action). */
void xd_shell_sounds_init(void);
void xd_shell_battery_alarm(xd_shell *shell, double percent, bool discharging);
/* Settings changed. */
void xd_sleep_update(xd_shell *shell);
/* Idle long enough: stand by (or hibernate). */
void xd_sleep_now(xd_shell *shell, bool hibernate);
/* A battery alarm's Shut down. */
void xd_power_off_now(xd_shell *shell);
/* account.c: the user's full name (XP Start menu) and Switch User. */
void xd_account_init(xd_shell *shell);
void xd_account_finish(xd_shell *shell);
/* AccountsService's RealName, else GECOS, else the login name. */
const char *xd_account_full_name(xd_shell *shell);
/* Asks AccountsService again (a Start menu is opening). */
void xd_account_refresh(xd_shell *shell);
/* A display manager that can show its greeter is running. */
bool xd_account_can_switch_user(xd_shell *shell);
/* Locks the screen and goes to the display manager's greeter. */
void xd_account_switch_user(xd_shell *shell);
/* sys_*.c: our notification area applets. */
void xd_sys_battery_init(xd_shell *shell);
void xd_sys_network_init(xd_shell *shell);
void xd_sys_bluetooth_init(xd_shell *shell);
/* sys_access.c: StickyKeys', FilterKeys' and MouseKeys' status icons. */
void xd_sys_access_init(xd_shell *shell);
void xd_sys_volume_init(xd_shell *shell);
/* sys_volume.c: the volume flyout's input (surface-local logical). */
bool xd_volume_flyout_owns(xd_shell *shell, struct wl_surface *surface);
void xd_volume_flyout_motion(xd_shell *shell, double x, double y);
void xd_volume_flyout_button(xd_shell *shell, uint32_t button, bool pressed, double x, double y);
void xd_volume_flyout_render_dirty(xd_shell *shell);
/* Closes the flyout (a click elsewhere in the shell, a menu opening). */
void xd_volume_flyout_dismiss(xd_shell *shell);

/* deskicons.c: the desktop's icons, on the first monitor's desktop. */
typedef struct xd_deskicons xd_deskicons;
void xd_deskicons_init(xd_shell *shell);
void xd_deskicons_finish(xd_shell *shell);
void xd_deskicons_draw(xd_shell *shell, xd_desktop *d, cairo_t *cr);
void xd_deskicons_motion(xd_shell *shell, xd_desktop *d, double x, double y);
void xd_deskicons_button(xd_shell *shell, xd_desktop *d, uint32_t button, bool pressed,
                         uint32_t time, uint32_t serial);
/* Redraws them (the taskbar moved, the theme changed). */
void xd_deskicons_relayout(xd_shell *shell);
/* Follows shell->settings' [DesktopIcons] and icon size: which icons
 * show, their pictures, size and arrangement. */
void xd_deskicons_settings_changed(xd_shell *shell);
/* Keys while the desktop has the keyboard; true when used. */
bool xd_deskicons_key(xd_shell *shell, uint32_t sym, const char *utf8, uint32_t serial);
/* Our Cut or Copy was replaced on the clipboard. */
void xd_deskicons_clipboard_lost(xd_shell *shell);
/* A drag over the desktop: what a drop at (x, y) would do, highlighting
 * the folder it would go into; x < 0 ends the highlight. `own`: the
 * desktop's own icons. */
typedef enum { XD_DROP_NONE, XD_DROP_HERE, XD_DROP_FOLDER, XD_DROP_TRASH } xd_drop_kind;
xd_drop_kind xd_deskicons_drag_over(xd_shell *shell, xd_desktop *d, double x, double y, bool own);
/* Files dropped at (x, y); `uris` nullptr for the desktop's own icons. */
void xd_deskicons_drop(xd_shell *shell, xd_desktop *d, char **uris, double x, double y,
                       bool move);
/* Our drag ended (dropped anywhere, or cancelled). */
void xd_deskicons_drag_ended(xd_shell *shell);
/* Opens a file or place as a double-click would: folders (and My
 * Computer, the Recycle Bin) in xd-files, launchers run, documents in the
 * program for their type. */
void xd_shell_open_uri(xd_shell *shell, const char *uri);

/* desktop.c */
xd_desktop *xd_desktop_for_surface(xd_shell *shell, struct wl_surface *surface);
xd_desktop *xd_desktop_create(xd_shell *shell, struct wl_output *output);
void xd_desktop_destroy(xd_desktop *d);
void xd_desktop_render(xd_desktop *d);

/* taskbar.c, for superbar.c: room for n buttons; the minimize animation's
 * target; the page arrows (in page_up_rect and page_down_rect). */
void xd_taskbar_reserve(xd_taskbar *bar, size_t n);
void xd_taskbar_tell_button(xd_taskbar *bar, xd_toplevel *tl, xd_rect r);
void xd_taskbar_draw_page_arrows(xd_taskbar *bar, const xd_theme *t, cairo_t *cr);

/* superbar.c: Windows 7's taskbar buttons (Aero) and jump lists. */
void xd_superbar_init(xd_shell *shell);
void xd_superbar_finish(xd_shell *shell);
/* Lays out and draws the buttons in `band` (device px). */
void xd_superbar_layout(xd_taskbar *bar, const xd_theme *t, cairo_t *cr, xd_rect band, int rows,
                        int row_h);
bool xd_superbar_is_button(const xd_taskbar *bar, int hit);
/* A left or middle click on button `hit`. */
void xd_superbar_click(xd_taskbar *bar, int hit, uint32_t button);
/* Motion with the button held on one: true while it's being dragged. */
bool xd_superbar_drag(xd_taskbar *bar, double x, double y);
/* The button came up: true if it was a drag (which ends). */
bool xd_superbar_drop(xd_taskbar *bar);
char *xd_superbar_tooltip(xd_taskbar *bar, int hit); /* xd_free */
void xd_superbar_jump_list(xd_taskbar *bar, int hit, uint32_t serial);
bool xd_superbar_pinned(xd_shell *shell, const char *desktop_id);
void xd_superbar_set_pinned(xd_shell *shell, const char *desktop_id, bool pin);
/* A program's Recent and Tasks (the Start menu's jump list), and whether
 * it has any. */
xd_menu_model *xd_jumplist_model(xd_shell *shell, xd_menu_tree *tree, const char *desktop_id);
bool xd_jumplist_has_items(xd_shell *shell, const char *desktop_id);

/* taskbar.c, after a theme change */
void xd_taskbar_retheme(xd_taskbar *bar);

/* main.c */
/* The tallest monitor's height (logical), for menus that would be taller. */
int xd_shell_screen_height(const xd_shell *shell);
/* Runs a shell command line detached; returns its pid (0 on failure). */
pid_t xd_shell_launch(xd_shell *shell, const char *command);

/* idle.c: screen saver and monitor power timers; call after settings
 * change. */
void xd_idle_update(xd_shell *shell);
/* Called after the Start menu closed for any reason. */
void xd_shell_start_menu_closed(xd_shell *shell);

/* shellbus.c: org.xeondesktop.Shell, the Start menu for another
 * compositor's key binding. */
void xd_shellbus_init(xd_shell *shell);
void xd_shellbus_finish(xd_shell *shell);
void xd_shell_toggle_start_menu(xd_shell *shell);
void xd_shellbus_keyboard_entered(xd_shell *shell, struct wl_surface *surface, uint32_t serial);
void xd_shellbus_taskbar_gone(xd_shell *shell, xd_taskbar *bar);
/* xd-shell --toggle-start-menu: asks the running shell; an exit status. */
int xd_shellbus_call_toggle(void);

/* session.c */
/* The compositor at the other end of the Wayland connection (0 if that
 * can't be told). */
pid_t xd_session_compositor_pid(struct wl_display *display);
/* Under xd-compositor, or another compositor (sets shell->standalone and
 * fixes up XEONDESKTOP_PID and XEONDESKTOP_NESTED for what we start)? */
void xd_session_detect(xd_shell *shell, pid_t compositor);
/* The Windows Logon sound, once a session. */
void xd_session_logon_sound(pid_t compositor);

/* startmenu.c */
void xd_start_menu_build(xd_shell *shell);
/* The Windows 2000 menu's Documents: recent files, as of now. */
void xd_start_menu_refresh_documents(xd_shell *shell);

/* toplevels.c */
void xd_toplevels_init(xd_shell *shell);
void xd_toplevel_click(xd_toplevel *t);
/* Show Desktop: minimize everything, or restore what it minimized. */
void xd_show_desktop_toggle(xd_shell *shell);

/* quicklaunch.c */
void xd_quicklaunch_init(xd_shell *shell);
void xd_quicklaunch_reload(xd_shell *shell);
bool xd_quicklaunch_watch_changed(xd_shell *shell);
void xd_quicklaunch_finish(xd_shell *shell);
void xd_quicklaunch_activate(xd_shell *shell, size_t index);
/* The Quick Launch folder, and a shortcut's index by file name (or -1). */
void xd_quicklaunch_folder(xd_strbuf *out);
ssize_t xd_quicklaunch_find(const xd_shell *shell, const char *file);
/* Moves shortcut `from` to before position `to` (0..count), renumbering
 * the folder's files; the folder watch reloads the toolbar. */
void xd_quicklaunch_move(xd_shell *shell, size_t from, size_t to);
/* Adds a shortcut to `target` (a .desktop file is linked, anything else
 * gets a launcher that opens it) before position `at`. */
bool xd_quicklaunch_add(xd_shell *shell, size_t at, const char *target);

#endif
