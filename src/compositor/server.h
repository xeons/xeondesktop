/* SPDX-License-Identifier: MIT
 * xd-compositor: shared server state.
 */
#ifndef XD_COMPOSITOR_SERVER_H
#define XD_COMPOSITOR_SERVER_H

#include <sys/types.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#ifdef XD_SCENEFX
/* xd-compositor-fx: SceneFX's scene graph (blur, shadows) in place of
 * wlroots'; the API is a superset. */
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <scenefx/types/wlr_scene.h>
#else
#include <wlr/types/wlr_scene.h>
#endif
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/xwayland/xwayland.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "../settings/settings.h"
#include "../theme/theme.h"
#include "wl.h"

/* Tag stored in wlr_scene_node.data so a node under the cursor can be
 * traced back to what owns it. */
typedef enum xd_node_kind : uint8_t {
    XD_NODE_VIEW = 1,
    XD_NODE_LAYER = 2,
} xd_node_kind;

typedef struct xd_node_tag {
    xd_node_kind kind;
} xd_node_tag;

/* Smallest client area a window may be resized to, in layout units. */
enum { XD_MIN_CLIENT_W = 112, XD_MIN_CLIENT_H = 27 };

typedef enum xd_cursor_mode : uint8_t {
    XD_CURSOR_PASSTHROUGH,
    XD_CURSOR_MOVE,
    XD_CURSOR_RESIZE,
    XD_CURSOR_BUTTON, /* holding a caption button */
    XD_CURSOR_KEY_MOVE, /* window menu Move: arrows or the mouse, Enter/Esc */
    XD_CURSOR_KEY_SIZE, /* window menu Size: an arrow picks the edge first */
} xd_cursor_mode;

typedef struct xd_server xd_server;
typedef struct xd_access xd_access;
typedef struct xd_ime xd_ime;

typedef struct xd_output {
    struct wl_list link; /* xd_server.outputs */
    xd_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_rect *background;
    struct wl_list layer_surfaces; /* xd_layer_surface.link */
    struct wlr_box usable;         /* work area: output minus exclusive zones */
    /* Where monitors.ini or the last configuration put it, relative to the
     * other monitors (the layout is shifted to start at 0,0). */
    bool has_position;
    int want_x, want_y;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
} xd_output;

/* A layer-shell surface (taskbar, desktop, notifications, ...). */
/* Aero Snap: where a window is snapped (or would be). */
typedef enum xd_snap : uint8_t {
    XD_SNAP_NONE,
    XD_SNAP_MAXIMIZE,
    XD_SNAP_LEFT,
    XD_SNAP_RIGHT,
    XD_SNAP_TALL, /* Win+Shift+Up: the work area's full height */
} xd_snap;

typedef struct xd_layer_surface {
    xd_node_tag tag; /* must be first */
    struct wl_list link; /* xd_output.layer_surfaces */
    xd_server *server;
    xd_output *output;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene;
    enum zwlr_layer_shell_v1_layer layer;
#ifdef XD_SCENEFX
    struct wlr_scene_blur *blur; /* Aero: behind the shell's glass taskbar */
#endif
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener new_popup;
    struct wl_listener destroy;
} xd_layer_surface;

typedef enum xd_view_kind : uint8_t { XD_VIEW_XDG, XD_VIEW_X11 } xd_view_kind;

typedef struct xd_view {
    xd_node_tag tag; /* must be first */
    struct wl_list link; /* xd_server.views, most recently focused first */
    xd_server *server;
    xd_view_kind kind;
    struct wlr_xdg_toplevel *toplevel;      /* XD_VIEW_XDG */
    struct wlr_xwayland_surface *xsurface;  /* XD_VIEW_X11 */
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    /* Taskbar entry; exists while mapped. */
    struct wlr_foreign_toplevel_handle_v1 *ftl;
    struct wlr_output *ftl_output;

    struct wlr_scene_tree *tree;    /* at the outer frame origin */
    struct wlr_scene_buffer *frame; /* server-side decoration */
#ifdef XD_SCENEFX
    /* Aero: the blur behind its glass (masked by the frame) and its shadow. */
    struct wlr_scene_blur *blur;
    struct wlr_scene_shadow *shadow;
#endif
    struct wlr_scene_tree *content; /* the client's surfaces */
    /* capture.c: its ext-foreign-toplevel-list handle (while it has a
     * taskbar button) and capture source (once the shell asked). */
    struct wlr_ext_foreign_toplevel_handle_v1 *ext_toplevel;
    struct xd_capture_source *capture;

    bool mapped;
    bool minimized;
    /* Didn't answer a ping (xdg-shell): "(Not Responding)" after its
     * title, as Windows shows, until it answers again. */
    bool on_top; /* Always On Top: in layer_on_top */
    xd_snap snapped;            /* Aero Snap: half the screen */
    struct wlr_box snap_restore; /* its size before */
    bool hung;
    bool sentry_dim; /* SoundSentry flashing its caption: drawn inactive */
    bool ping_out; /* a ping to its program hasn't timed out (yet) */
    char *hung_title;
    /* Asked to close while responding: if it stops responding before
     * handling that, Windows' End Program box follows. CLOSE_SENT until a
     * ping goes out after the request, CLOSE_PINGED until it's answered. */
    enum : uint8_t { XD_CLOSE_NONE, XD_CLOSE_SENT, XD_CLOSE_PINGED } close_wait;
    bool maximized;
    bool dialog; /* fixed size (min == max): dialog frame, no resizing */
    bool csd;    /* the client draws its own decorations: no frame from us */
    int ghost_dx, ghost_dy; /* how far our frame moved it while it hangs */
    bool fullscreen;
    struct wlr_box fullscreen_restore; /* outer box before fullscreen */
    /* Top/left edge resizing: the edges that stay put, applied when the
     * client commits its new size so the window doesn't jitter. It ends
     * once the client has the size asked for (anchor_w x anchor_h), or
     * when the window is moved. */
    uint32_t anchor_edges;
    int anchor_right, anchor_bottom;
    int anchor_w, anchor_h;
    /* Where the last Cascade/Tile found the window, for Undo. */
    bool undo_valid, undo_maximized;
    struct wlr_box undo_box;
    int x, y;               /* outer frame position, layout coordinates */
    struct wlr_box restore; /* outer box before maximizing */

    xd_frame_part hot;
    xd_frame_part pressed;

    /* What the frame buffer was last drawn with, to skip redundant redraws. */
    struct {
        int width, height;
        double scale;
        bool active, maximized, dialog;
        xd_frame_part hot, pressed;
        bool title_dirty;
        cairo_surface_t *icon; /* the cache's, so compared by address */
    } drawn;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_minimize;
    struct wl_listener request_window_menu;
    struct wl_listener request_fullscreen;
    struct wl_listener set_parent;
    /* X11 windows only. */
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener request_configure;
    struct wl_listener request_activate;
    struct wl_listener request_close;
    struct wl_listener set_decorations;
    struct wl_listener set_hints;
    struct wl_listener content_destroy; /* wlroots frees it with the surface */
    struct wl_listener set_title;
    struct wl_listener ping_timeout;
    bool pings; /* X11: lists _NET_WM_PING in WM_PROTOCOLS */
    struct wl_listener set_app_id;
    struct wl_listener ftl_activate;
    struct wl_listener ftl_minimize;
    struct wl_listener ftl_maximize;
    struct wl_listener ftl_close;
    struct wl_listener ftl_set_rectangle;
    /* Its taskbar button (layout coordinates), from the shell. */
    struct wlr_box button_box;
    bool has_button_box;
    struct wl_listener decoration_mode;
    struct wl_listener decoration_destroy;
} xd_view;

typedef struct xd_keyboard {
    struct wl_list link; /* xd_server.keyboards */
    xd_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener keymap;
    struct wl_listener key;
    struct wl_listener destroy;
    bool physical; /* gets [Keyboard]'s keymap (virtual keyboards bring their own) */
    bool resend_layout; /* a new keymap: the focused client needs the layout again */
    /* StickyKeys ([Accessibility]), as xkb modifier masks. */
    uint32_t sticky_alone;   /* a modifier is down with no other key yet */
    uint32_t sticky_latched; /* applies to the next key */
    uint32_t sticky_locked;  /* pressed twice: stays on */
    /* BounceKeys: the last key released, and a press being ignored. */
    uint32_t bounce_keycode, bounce_msec, bounced_keycode;
} xd_keyboard;

/* A physical pointer (mouse, touchpad), configured from [Mouse]. */
typedef struct xd_pointer {
    struct wl_list link; /* xd_server.pointers */
    struct wlr_input_device *device;
    struct wl_listener destroy;
} xd_pointer;

typedef struct xd_config {
    xd_visual_style style;
    xd_scheme scheme;
    const char *scheme_path; /* --scheme given: passed on to the shell */
    bool cli_style;          /* --style given: passed on to the shell */
    const char *shell;       /* shell executable; nullptr = don't start one */
    const char *taskmgr;     /* Task Manager executable (Ctrl+Shift+Esc) */
    const char *security;    /* Windows Security (Ctrl+Alt+Delete, Classic style) */
    const char *locker;      /* Lock Computer (Win+L; restarted if a locker dies) */
    const char *polkit;      /* the session's polkit authentication agent */
    const char *endtask;     /* "End Program" for windows that don't respond */
    const char *accessibility; /* xd-access, asking to keep a feature a shortcut turned on */
    double forced_scale; /* 0 = pick per output from its DPI */
    bool virtual_input;  /* --allow-virtual-input: for automated UI tests */
    bool always_frames;  /* [Windows] AlwaysDrawFrames */
    bool animate_windows; /* [Effects] AnimateWindows */
    bool menu_transitions;              /* [Effects] MenuTransitions */
    xd_menu_transition menu_transition; /* ...and MenuTransition */
    bool full_window_drag;              /* [Effects] FullWindowDrag; off: outlines */
    xd_keyboard_settings keyboard; /* [Keyboard] */
    xd_mouse_settings mouse;       /* [Mouse] */
    xd_access_settings access;     /* [Accessibility] */
    const char *terminal;
    const char *startup;
} xd_config;

struct xd_server {
    xd_config config;
    xd_theme logical_theme; /* scale 1: frame geometry in layout units */

    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    /* Bottom to top: desktop colour, layer-shell background and bottom,
     * windows, layer-shell top and overlay. */
    struct wlr_scene_tree *layer_background;
    struct wlr_scene_tree *layer_views;
    struct wlr_scene_tree *layer_on_top; /* Always On Top windows, below the taskbar */
    struct wlr_scene_tree *layer_fullscreen; /* the focused fullscreen window */
    struct wlr_scene_tree *layers[4]; /* indexed by zwlr_layer_shell_v1_layer */

    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    /* Layer surface holding keyboard focus (exclusive interactivity, e.g.
     * while the Start menu is open); windows get it back afterwards. */
    xd_layer_surface *layer_focus;
    /* A layer surface that takes the keyboard when clicked (on-demand:
     * the desktop). While it has it, no window is active, as when
     * Windows' desktop is clicked. */
    xd_layer_surface *desktop_focus;

    struct wlr_foreign_toplevel_manager_v1 *ftl_manager;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
    struct wl_listener layout_change;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_event_source *ping_timer; /* asks windows if they're responding */
    struct wl_listener new_toplevel;
    struct wl_listener new_popup;
    struct wl_list views;
    int cascade;
    struct xd_appicons *appicons; /* title bar icons */

    struct wlr_xdg_decoration_manager_v1 *xdg_decoration;
    struct wlr_server_decoration_manager *kde_decoration; /* used by GTK 3 */
    struct wl_listener new_kde_decoration;
    struct wl_listener new_decoration;

    /* protocols.c */
    struct wlr_output_manager_v1 *output_manager;
    struct wl_listener output_apply;
    struct wl_listener output_test;
    struct wl_listener output_power;
    struct wlr_idle_notifier_v1 *idle_notifier;
    int idle_inhibitors;
    struct wl_listener new_idle_inhibitor;
    struct wl_listener request_activate;
    struct wl_listener new_activation_token;

    /* xwayland.c */
    struct wlr_compositor *compositor;
    struct wlr_xwayland *xwayland;
    xcb_atom_t net_wm_ping; /* 0 until Xwayland is ready */
    struct wl_listener xwayland_ready;
    struct wl_listener xwayland_new_surface;
    struct wlr_scene_tree *layer_x11_popups; /* X11 menus and tooltips */

    /* lock.c */
    struct wlr_scene_tree *layer_lock; /* above everything */
    bool locked;
    struct xd_lock *lock;             /* the locker client, while it runs */
    struct wlr_scene_rect *lock_cover;
    struct wlr_surface *lock_focus;   /* locker surface with the keyboard */
    struct wl_list lock_surfaces;     /* xd_lock_surface.link: one per output */
    struct wl_listener new_lock;
    int locker_restarts;              /* recent automatic restarts */
    struct timespec locker_restart_window;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *xcursor;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener request_cursor_shape;

    struct wlr_seat *seat;
    struct wl_list keyboards;
    /* The active keyboard layout (XKB group), the same on every keyboard
     * for the whole session; the shell's language indicator shows it. */
    uint32_t kb_group;
    uint32_t kb_layouts;   /* in [Keyboard]'s keymap; 0: not counted yet */
    bool kb_group_syncing; /* setting it on the keyboards: their events are ours */
    struct wl_list pointers; /* xd_pointer */
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    /* Drag and drop: the icon under the pointer, above everything. */
    struct wl_listener request_start_drag;
    struct wl_listener start_drag;
    struct wl_listener drag_destroy;
    struct wlr_scene_tree *layer_drag;
    struct wl_list zooms; /* animate.c: minimize/maximize animations in flight */
    bool peeking;         /* Aero Peek */
    xd_view *peek_view;   /* Aero Peek at this window alone (a thumbnail hovered) */
    /* Windows in the stacking order before it came forward (bottom
     * first), to put back; nullptr once a window was raised for real. */
    xd_view **peek_order;
    size_t peek_order_n;
    /* A virtual machine's GPU drawing with 3D (vmwgfx and the like): its
     * cursor plane takes our cursor and shows nothing, so the cursor is
     * drawn in software. */
    bool software_cursors;
    struct wlr_scene_tree *drag_icon;
    struct wl_listener new_virtual_keyboard;
    struct wl_listener new_virtual_pointer;
    /* Always there, for the input method (ime.c decides who sees it). */
    struct wlr_virtual_keyboard_manager_v1 *virtual_keyboards;

    /* Pointer grab state. */
    xd_cursor_mode mode;
    xd_view *grab_view;
    xd_frame_part grab_part;
    double grab_x, grab_y;
    struct wlr_box grab_box;
    /* snap.c: Aero Snap and Shake during a move (Aero style). */
    double grab_press_x, grab_press_y;
    bool snap_unmaximize; /* a maximized window waits to be dragged */
    xd_snap snap_target;
    struct wlr_scene_rect *snap_preview;
    double shake_anchor;
    int shake_dir, shake_swings;
    int64_t shake_start;
    xd_view **shaken; /* minimized by a shake, to bring back */
    size_t shaken_n;
    struct wlr_box key_box;  /* keyboard move/size: the box being set */
    uint32_t key_edges;      /* keyboard size: WLR_EDGE_* chosen so far */
    bool ignore_release;     /* the click that ended keyboard move/size */
    bool super_alone;        /* the Windows key is down with nothing else yet */
    xd_view *hover_view; /* view whose frame shows a hover state */
    const char *cursor_name;

    /* Double-click detection on captions. */
    uint32_t last_click_msec;
    xd_view *last_click_view;
    xd_frame_part last_click_part;

    /* Alt+Tab: selection while Alt is held. */
    xd_view *cycle_view;

    struct wl_event_source *sigchld;
    struct wl_event_source *settings_watch;
    int settings_fd;

    /* The shell is restarted if it crashes, like explorer.exe. */
    pid_t shell_pid;
    struct wl_global *arranger; /* xd-window-arranger-v1, shell only */
    struct wl_resource *arranger_resource; /* the shell's binding, for events */
    struct arranger_client **arranger_clients; /* every binding (shell, Task Manager) */
    size_t arranger_clients_n, arranger_clients_cap;
    /* capture.c: thumbnails for the shell's taskbar, shell only. */
    struct wlr_ext_foreign_toplevel_list_v1 *ext_toplevels;
    struct wl_global *capture_globals[3];
    struct wl_listener capture_request;
    struct wl_listener capture_sources_destroy;
    uint32_t shell_restarts;
    struct timespec shell_restart_window;
    /* The polkit agent too: without it, administrator actions just fail. */
    pid_t polkit_pid;
    uint32_t polkit_restarts;
    struct timespec polkit_restart_window;
    bool terminating;
    xd_access *access; /* access.c's state */
    xd_ime *ime;       /* ime.c's */
    struct xd_outline *outline; /* outline.c's, while one shows */
};

/* output.c */
void xd_output_init(xd_server *server);
void xd_output_update_backgrounds(xd_server *server);
/* Lays out layer surfaces and recomputes the work area. */
void xd_output_arrange(xd_output *out);
xd_output *xd_output_from_wlr(xd_server *server, struct wlr_output *wlr_output);
xd_output *xd_output_at(xd_server *server, double lx, double ly);

/* layers.c */
void xd_layers_init(xd_server *server);
/* Gives keyboard focus to the focused layer surface or else the focused
 * window. */
void xd_keyboard_refocus(xd_server *server);
/* The layer surface owning a scene node, if any. */
xd_layer_surface *xd_layer_from_node(struct wlr_scene_node *node);
/* A click on an on-demand layer surface: it gets the keyboard and the
 * front window is deactivated. */
void xd_layer_click_focus(xd_layer_surface *ls);
double xd_output_scale_at(xd_server *server, double lx, double ly);
/* Usable area of the output containing (lx, ly), in layout coordinates. */
struct wlr_box xd_output_work_area_at(xd_server *server, double lx, double ly);

/* input.c */
void xd_input_init(xd_server *server);
/* Applies config.keyboard (layout, repeat) to every physical keyboard. */
void xd_input_apply_keyboard(xd_server *server);
/* Applies config.mouse's buttons, speed and acceleration to every
 * libinput pointer. */
void xd_input_apply_mouse(xd_server *server);
/* Loads config.mouse's cursor theme and size, for our cursor and (by the
 * environment) for programs started from now on. */
void xd_input_apply_cursor_theme(xd_server *server);
/* config.access changed: drops StickyKeys' latched and locked modifiers. */
void xd_input_apply_access(xd_server *server);
/* Makes layout `group` the active one on every keyboard (the language
 * indicator); ignored past the keymap's layouts. */
void xd_input_set_keyboard_layout(xd_server *server, uint32_t group);
/* Lets clients inject keyboard/pointer input. Testing only. */
void xd_input_enable_virtual(xd_server *server);
void xd_cursor_set(xd_server *server, const char *name);
/* Ends a move, size or caption-button grab where it is, without acting on
 * it (the screen locks). */
void xd_cursor_cancel_grab(xd_server *server);
/* Runs argv[0] (searched in PATH) with argv. Returns the pid or -1. */
pid_t xd_spawn_argv(char *const argv[]);
/* Runs a shell command line. */
void xd_spawn(xd_server *server, const char *command);
void xd_shell_start(xd_server *server);
/* Starts config.polkit, the session's polkit authentication agent. */
void xd_polkit_start(xd_server *server);

/* view.c */
void xd_view_init(xd_server *server);
xd_view *xd_view_from_node(struct wlr_scene_node *node);
xd_view *xd_view_focused(xd_server *server);
void xd_view_focus(xd_view *view);
/* The front window drawn and told inactive (the desktop has the focus). */
void xd_view_deactivate_front(xd_server *server);
void xd_view_focus_next(xd_server *server);
void xd_view_close(xd_view *view);
void xd_view_set_maximized(xd_view *view, bool maximized);
void xd_view_minimize(xd_view *view);
void xd_view_restore(xd_view *view);
void xd_view_set_position(xd_view *view, int x, int y);
/* Resizes so the outer frame becomes `outer` (layout coordinates). */
void xd_view_set_outer_box(xd_view *view, struct wlr_box outer);
/* Resizes keeping the edges opposite `moving` (WLR_EDGE_*) where they are
 * once the client has drawn its new size. */
void xd_view_resize_anchored(xd_view *view, struct wlr_box outer, uint32_t moving);
void xd_view_set_fullscreen(xd_view *view, bool fullscreen);
struct wlr_box xd_view_outer_box(const xd_view *view);
/* Whether we draw its frame (also around a hung client-decorated one). */
bool xd_view_framed(const xd_view *view);
xd_insets xd_view_insets(const xd_view *view);
/* Kind-independent access (xdg-shell or X11). */
struct wlr_surface *xd_view_surface(const xd_view *view);
const char *xd_view_title(const xd_view *view);
const char *xd_view_app_id(const xd_view *view);
void xd_view_set_resizing(xd_view *view, bool resizing);
/* Shared lifecycle, called by both shells. */
void xd_view_on_map(xd_view *view);
void xd_view_on_unmap(xd_view *view);
void xd_view_on_commit(xd_view *view);
void xd_view_on_destroy(xd_view *view);
/* Tells an X11 window where its client area is (it wants absolute
 * coordinates). */
void xd_view_x11_sync(xd_view *view);
/* Re-reads whether the client asked to draw its own decorations. */
void xd_view_update_decorations(xd_view *view);

/* arrange.c: Cascade/Tile for the taskbar menu (shell-only protocol). */
void xd_arrange_init(xd_server *server);

/* animate.c: XP's minimize/maximize animation. A bar flies from `from` to
 * `to` (layout coordinates); `reveal`, if given, is hidden until it
 * arrives (a window being restored). */
void xd_animate_zoom(xd_server *server, struct wlr_box from, struct wlr_box to,
                     xd_view *reveal);
void xd_animate_forget_view(xd_server *server, xd_view *view);
void xd_animate_finish(xd_server *server);

/* transition.c: the shell's menus and tooltips fade or scroll in. */
void xd_transition_watch_popup(xd_server *server, struct wlr_xdg_popup *popup);

/* outline.c: "Show window contents while dragging" off. Moving and sizing
 * show the window's outline at `box` (layout coordinates) until hidden;
 * the window itself moves on release. */
bool xd_outline_dragging(const xd_server *server);
void xd_outline_show(xd_server *server, struct wlr_box box);
void xd_outline_hide(xd_server *server);

/* snap.c: Windows 7's Snap and Shake (Aero style only). begin returns
 * whether the window may be moved (a maximized one, only in Aero). */
bool xd_snap_begin(xd_server *server, xd_view *view);
void xd_snap_motion(xd_server *server, xd_view *view);
void xd_snap_end(xd_server *server, xd_view *view);
/* The move ended without a drop (window gone, screen locked): the
 * preview goes, nothing snaps. */
void xd_snap_cancel(xd_server *server);
void xd_snap_key(xd_view *view, xd_snap side);
/* Win+Shift+Up: stretched to the work area's height, keeping its width. */
void xd_snap_tall(xd_view *view);
/* Win+Shift+Left/Right: to the next monitor that way (round the end). */
void xd_snap_to_monitor(xd_view *view, int dir);
void xd_snap_forget_view(xd_server *server, xd_view *view);
void xd_snap_finish(xd_server *server);
/* A window is going: Task Manager's marks for arrange_marked drop it. */
void xd_arrange_forget_view(xd_server *server, xd_view *view);
/* Always On Top: the window (and its dialogs) above ordinary windows. */
void xd_view_set_on_top(xd_view *view, bool on_top);
/* Aero Peek: windows fade to their outline, and back. */
void xd_view_peek(xd_server *server, bool on);
/* Aero Peek at one window (a taskbar thumbnail): the others fade, it
 * comes in front of them; nullptr ends it. */
void xd_view_peek_window(xd_server *server, xd_view *view);
/* Not Responding: a ping timed out (view.c pings; X11 windows that
 * support _NET_WM_PING too). The title the frame and taskbar show. */
void xd_view_ping_timed_out(xd_view *view);
void xd_view_title_changed(xd_view *view);

/* protocols.c: output management/power, idle, activation. */
void xd_protocols_init(xd_server *server);
void xd_output_config_changed(xd_server *server);
/* Remembers every output's settings in monitors.ini. */
void xd_output_save_config(xd_server *server);
/* Places outputs with a wanted position, keeping the desktop at 0,0. */
void xd_output_place_all(xd_server *server);
void xd_output_key(const struct wlr_output *o, char *out, size_t cap);
void xd_idle_activity(xd_server *server);

/* access.c: SlowKeys, ToggleKeys, MouseKeys, ClickLock, the
 * accessibility shortcuts; the system bell. */
struct wlr_keyboard_key_event;
void xd_access_init(xd_server *server);
void xd_access_finish(xd_server *server);
/* Sees each key event first (after BounceKeys); true if it's used up. */
bool xd_access_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *e);
/* Sees each physical button event first; true if ClickLock keeps it. */
bool xd_access_button(xd_server *server, uint32_t button, bool pressed, uint32_t time_msec);
/* config.access or config.mouse changed. */
void xd_access_apply(xd_server *server);
void xd_access_keyboard_gone(xd_keyboard *kb);
/* Input from the user (xd_idle_activity): the automatic reset's clock. */
void xd_access_activity(xd_server *server);
/* input.c, for access.c: a key event past the accessibility filters, a
 * button event as if from a mouse, and a pointer move. */
void xd_input_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *e);
void xd_input_button(xd_server *server, uint32_t time_msec, uint32_t button,
                     enum wl_pointer_button_state state);
void xd_input_move_pointer(xd_server *server, double dx, double dy, uint32_t time_msec);

/* ime.c: input methods (text-input-v3 relayed to input-method-v2). */
void xd_ime_init(xd_server *server);
void xd_ime_finish(xd_server *server);
/* config.keyboard.input_method: the environment programs get (true if it
 * changed) and the framework running. */
bool xd_ime_apply(xd_server *server);
/* The input method's protocols (and virtual keyboards, unless
 * --allow-virtual-input) are offered only where xd_ime_client_allowed. */
bool xd_ime_global_private(const xd_server *server, const struct wl_global *global);
bool xd_ime_client_allowed(const xd_server *server, const struct wl_client *client);
/* The input method's own virtual keyboard: keys it passes back. */
bool xd_ime_own_keyboard(const xd_keyboard *kb);
/* A key or the modifiers for the input method's keyboard grab: true if
 * it took them. */
bool xd_ime_key(xd_keyboard *kb, const struct wlr_keyboard_key_event *e);
bool xd_ime_modifiers(xd_keyboard *kb);
/* The screen locked or unlocked: candidate windows hide or come back. */
void xd_ime_lock_changed(xd_server *server);
/* SIGCHLD: true if `pid` was the input method we started. */
bool xd_ime_child_exited(xd_server *server, pid_t pid, int status);

/* lock.c: ext-session-lock. */
void xd_lock_init(xd_server *server);
void xd_lock_layout_changed(xd_server *server);

/* xwayland.c: X11 applications. */
void xd_xwayland_init(xd_server *server);
void xd_xwayland_finish(xd_server *server);
/* input.c: the window menu's Move and Size, driven by the keyboard (and the
 * mouse) until Enter, a click, or Esc to put the window back. */
void xd_view_begin_keyboard_move(xd_view *view);
void xd_view_begin_keyboard_size(xd_view *view);
/* arrange.c: asks the shell to show the window menu at layout (lx, ly);
 * keyboard: Alt+Space asked (the menu then underlines its access keys). */
void xd_window_menu_show(xd_view *view, double lx, double ly, bool keyboard);
/* arrange.c: asks the shell to open (or close) the Start menu. */
void xd_start_menu_toggle(xd_server *server);
/* arrange.c: tells the shell the active keyboard layout (kb_group). */
void xd_arrange_keyboard_layout(xd_server *server);
/* arrange.c: tells the shell of the lid or the power or sleep button
 * (xd-window-arranger-v1's power_button, whose values these are), for
 * Power Options' action. Returns whether a shell heard it. */
typedef enum xd_power_event : uint8_t {
    XD_POWER_EVENT_LID_CLOSED,
    XD_POWER_EVENT_LID_OPENED,
    XD_POWER_EVENT_POWER,
    XD_POWER_EVENT_SLEEP,
} xd_power_event;
bool xd_arrange_power_button(xd_server *server, xd_power_event event);
/* input.c: the lid closed or opened (its switch, or a UI test's). */
void xd_input_lid(xd_server *server, bool closed);
/* arrange.c: asks the shell to flash the window's taskbar button. */
void xd_view_attention(xd_view *view);
/* Where the window menu opens from the keyboard or the caption icon:
 * below the icon (layout coordinates). */
void xd_view_menu_anchor(const xd_view *view, double *lx, double *ly);
/* Frame description for drawing and hit testing. */
xd_frame xd_view_frame(const xd_view *view, bool active);
/* Which part of the frame the layout point (lx, ly) is over. */
xd_frame_part xd_view_hit(const xd_view *view, double lx, double ly);
void xd_view_set_hover(xd_view *view, xd_frame_part hot, xd_frame_part pressed);
void xd_view_update_all_frames(xd_server *server);
/* Re-lays out and redraws a window's frame after a theme change. */
void xd_view_retheme(xd_view *view);
/* Re-fits maximized windows after a work area change. */
void xd_view_refit_maximized(xd_server *server, xd_output *out);
/* Brings windows left outside every output (one was unplugged, turned off
 * or moved) onto the nearest output. */
void xd_view_rescue_offscreen(xd_server *server);
/* Shows a popup (menu) as a child of parent_tree. */
/* glass_server: the shell's popups, blurred behind in the Aero style
 * (xd-compositor-fx); nullptr for windows' popups. */
void xd_popup_attach(struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent_tree,
                     xd_server *glass_server);
/* Forget an output that is going away. */
void xd_view_output_gone(xd_server *server, struct wlr_output *wlr_output);

/* capture.c: window pictures for the shell's taskbar thumbnails. */
void xd_capture_init(xd_server *server);
/* Whether `global` is one of capture's, offered to the shell alone. */
bool xd_capture_global_private(const xd_server *server, const struct wl_global *global);
/* The window got or lost its taskbar button, or its title or app id
 * changed, or it committed. */
void xd_capture_view_shown(xd_view *view);
void xd_capture_view_hidden(xd_view *view);
void xd_capture_view_changed(xd_view *view);
void xd_capture_view_committed(xd_view *view);

/* frame.c */
void xd_frame_render(xd_view *view, bool force);
/* An application's icon has loaded: redraws its windows' frames. */
void xd_frame_icon_ready(void *server, const char *app_id);
double xd_view_scale(const xd_view *view);
/* A buffer the compositor draws into with cairo (*surface, owned by the
 * buffer), or nullptr. */
struct wlr_buffer *xd_cairo_buffer_new(int width, int height, cairo_surface_t **surface);

#endif
