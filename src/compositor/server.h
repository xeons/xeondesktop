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
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

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
} xd_cursor_mode;

typedef struct xd_server xd_server;

typedef struct xd_output {
    struct wl_list link; /* xd_server.outputs */
    xd_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_rect *background;
    struct wl_list layer_surfaces; /* xd_layer_surface.link */
    struct wlr_box usable;         /* work area: output minus exclusive zones */
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
} xd_output;

/* A layer-shell surface (taskbar, desktop, notifications, ...). */
typedef struct xd_layer_surface {
    xd_node_tag tag; /* must be first */
    struct wl_list link; /* xd_output.layer_surfaces */
    xd_server *server;
    xd_output *output;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene;
    enum zwlr_layer_shell_v1_layer layer;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener new_popup;
    struct wl_listener destroy;
} xd_layer_surface;

typedef struct xd_view {
    xd_node_tag tag; /* must be first */
    struct wl_list link; /* xd_server.views, most recently focused first */
    xd_server *server;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    /* Taskbar entry; exists while mapped. */
    struct wlr_foreign_toplevel_handle_v1 *ftl;
    struct wlr_output *ftl_output;

    struct wlr_scene_tree *tree;    /* at the outer frame origin */
    struct wlr_scene_buffer *frame; /* server-side decoration */
    struct wlr_scene_tree *content; /* the client's surfaces */

    bool mapped;
    bool minimized;
    bool maximized;
    int x, y;               /* outer frame position, layout coordinates */
    struct wlr_box restore; /* outer box before maximizing */

    xd_frame_part hot;
    xd_frame_part pressed;

    /* What the frame buffer was last drawn with, to skip redundant redraws. */
    struct {
        int width, height;
        double scale;
        bool active, maximized;
        xd_frame_part hot, pressed;
        bool title_dirty;
    } drawn;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_minimize;
    struct wl_listener request_fullscreen;
    struct wl_listener set_title;
    struct wl_listener set_app_id;
    struct wl_listener ftl_activate;
    struct wl_listener ftl_minimize;
    struct wl_listener ftl_maximize;
    struct wl_listener ftl_close;
    struct wl_listener decoration_mode;
    struct wl_listener decoration_destroy;
} xd_view;

typedef struct xd_keyboard {
    struct wl_list link; /* xd_server.keyboards */
    xd_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
} xd_keyboard;

typedef struct xd_config {
    xd_visual_style style;
    xd_scheme scheme;
    const char *scheme_path; /* passed on to the shell */
    const char *shell;       /* shell executable; nullptr = don't start one */
    double forced_scale; /* 0 = pick per output from its DPI */
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
    struct wlr_scene_tree *layers[4]; /* indexed by zwlr_layer_shell_v1_layer */

    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    /* Layer surface holding keyboard focus (exclusive interactivity, e.g.
     * while the Start menu is open); windows get it back afterwards. */
    xd_layer_surface *layer_focus;

    struct wlr_foreign_toplevel_manager_v1 *ftl_manager;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
    struct wl_listener layout_change;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_toplevel;
    struct wl_listener new_popup;
    struct wl_list views;
    int cascade;

    struct wlr_xdg_decoration_manager_v1 *xdg_decoration;
    struct wl_listener new_decoration;

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
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;

    /* Pointer grab state. */
    xd_cursor_mode mode;
    xd_view *grab_view;
    xd_frame_part grab_part;
    double grab_x, grab_y;
    struct wlr_box grab_box;
    xd_view *hover_view; /* view whose frame shows a hover state */
    const char *cursor_name;

    /* Double-click detection on captions. */
    uint32_t last_click_msec;
    xd_view *last_click_view;
    xd_frame_part last_click_part;

    /* Alt+Tab: selection while Alt is held. */
    xd_view *cycle_view;

    struct wl_event_source *sigchld;

    /* The shell is restarted if it crashes, like explorer.exe. */
    pid_t shell_pid;
    uint32_t shell_restarts;
    struct timespec shell_restart_window;
    bool terminating;
};

/* output.c */
void xd_output_init(xd_server *server);
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
double xd_output_scale_at(xd_server *server, double lx, double ly);
/* Usable area of the output containing (lx, ly), in layout coordinates. */
struct wlr_box xd_output_work_area_at(xd_server *server, double lx, double ly);

/* input.c */
void xd_input_init(xd_server *server);
void xd_cursor_set(xd_server *server, const char *name);
/* Runs argv[0] (searched in PATH) with argv. Returns the pid or -1. */
pid_t xd_spawn_argv(char *const argv[]);
/* Runs a shell command line. */
void xd_spawn(xd_server *server, const char *command);
void xd_shell_start(xd_server *server);

/* view.c */
void xd_view_init(xd_server *server);
xd_view *xd_view_from_node(struct wlr_scene_node *node);
xd_view *xd_view_focused(xd_server *server);
void xd_view_focus(xd_view *view);
void xd_view_focus_next(xd_server *server);
void xd_view_close(xd_view *view);
void xd_view_set_maximized(xd_view *view, bool maximized);
void xd_view_minimize(xd_view *view);
void xd_view_restore(xd_view *view);
void xd_view_set_position(xd_view *view, int x, int y);
/* Resizes so the outer frame becomes `outer` (layout coordinates). */
void xd_view_set_outer_box(xd_view *view, struct wlr_box outer);
struct wlr_box xd_view_outer_box(const xd_view *view);
xd_insets xd_view_insets(const xd_view *view);
/* Which part of the frame the layout point (lx, ly) is over. */
xd_frame_part xd_view_hit(const xd_view *view, double lx, double ly);
void xd_view_set_hover(xd_view *view, xd_frame_part hot, xd_frame_part pressed);
void xd_view_update_all_frames(xd_server *server);
/* Re-fits maximized windows after a work area change. */
void xd_view_refit_maximized(xd_server *server, xd_output *out);
/* Shows a popup (menu) as a child of parent_tree. */
void xd_popup_attach(struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent_tree);
/* Forget an output that is going away. */
void xd_view_output_gone(xd_server *server, struct wlr_output *wlr_output);

/* frame.c */
void xd_frame_render(xd_view *view, bool force);
double xd_view_scale(const xd_view *view);

#endif
