/* SPDX-License-Identifier: MIT
 * xd-shell: taskbar (and later Start menu, tray, desktop).
 */
#ifndef XD_SHELL_SHELL_H
#define XD_SHELL_SHELL_H

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

typedef struct xd_early_output {
    struct wl_output *output;
    uint32_t name;
} xd_early_output;

/* A window as reported by the compositor. */
typedef struct xd_toplevel {
    struct wl_list link; /* xd_shell.toplevels, in creation order */
    xd_shell *shell;
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char *title;
    char *app_id;
    bool activated;
    bool minimized;
    bool maximized;
} xd_toplevel;

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
} xd_desktop;

/* Hover/press targets on a taskbar. */
enum { XD_HIT_NONE = -1, XD_HIT_START = -2, XD_HIT_TRAY = -3 };

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
    xd_rect tray_rect;
    size_t task_count;
    size_t task_cap;
    xd_rect *task_rects;
    xd_toplevel **task_views;

    int hot;     /* XD_HIT_* or a task index */
    int pressed; /* same, while a button is held */
} xd_taskbar;

struct xd_shell {
    xd_visual_style style;
    xd_scheme scheme;
    const char *scheme_path; /* --scheme given; passed on to dialogs */
    bool cli_style;          /* --style given; passed on to dialogs */
    xd_settings settings;    /* desktop.ini */
    int settings_fd;
    cairo_surface_t *wallpaper; /* loaded settings.wallpaper, or nullptr */
    const char *terminal;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    struct xdg_wm_base *wm_base;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wp_viewporter *viewporter;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape;

    struct wl_list taskbars;
    struct wl_list toplevels;

    xd_taskbar *pointer_bar; /* taskbar under the pointer */
    xd_menu *pointer_menu;   /* menu under the pointer */

    /* Start menu. */
    xd_icons *icons;
    xd_app_list apps;
    xd_menu_tree menu_tree;
    xd_menu_model *start_model;
    xd_menu *start_menu;    /* open root menu, or nullptr */
    xd_taskbar *start_bar;  /* taskbar whose Start button opened it */
    uint32_t menu_serial;   /* input serial the menu grab was made with */
    int menu_timer_fd;      /* hover delay */
    xd_menu *pending_menu;  /* menu waiting for the hover delay */

    char clock[32];
    bool running;

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
/* Opens the Start menu above this taskbar's Start button. */
void xd_taskbar_open_start_menu(xd_taskbar *bar, uint32_t serial);
/* Forget a window that's going away. */
void xd_taskbars_forget(xd_shell *shell, xd_toplevel *toplevel);

/* desktop.c */
xd_desktop *xd_desktop_create(xd_shell *shell, struct wl_output *output);
void xd_desktop_destroy(xd_desktop *d);
void xd_desktop_render(xd_desktop *d);

/* taskbar.c, after a theme change */
void xd_taskbar_retheme(xd_taskbar *bar);

/* main.c */
void xd_shell_launch(xd_shell *shell, const char *command);
/* Called after the Start menu closed for any reason. */
void xd_shell_start_menu_closed(xd_shell *shell);

/* startmenu.c */
void xd_start_menu_build(xd_shell *shell);

/* toplevels.c */
void xd_toplevels_init(xd_shell *shell);
void xd_toplevel_click(xd_toplevel *t);

#endif
