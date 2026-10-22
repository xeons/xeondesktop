/* SPDX-License-Identifier: MIT
 * libxdui internals shared by ui.c (connection, input, clipboard) and
 * widgets.c (windows, widgets, popups).
 */
#ifndef XD_UI_INTERNAL_H
#define XD_UI_INTERNAL_H

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "../core/wlist.h"
#include "protocols.h"
#include "shm.h"
#include "textedit.h"
#include "ui.h"

typedef enum xd_widget_kind : uint8_t {
    XD_W_LABEL,
    XD_W_IMAGE,
    XD_W_BANNER,
    XD_W_BUTTON,
    XD_W_EDIT,
    XD_W_COMBO,
    XD_W_TABS,
    XD_W_CHECKBOX,
    XD_W_LISTBOX,
    XD_W_GROUPBOX,
    XD_W_CUSTOM,
    XD_W_SLIDER,
    XD_W_RADIO,
} xd_widget_kind;

struct xd_widget {
    xd_widget_kind kind;
    xd_window *win;
    xd_rect r; /* logical, window-local */
    bool enabled;
    uint8_t flags;
    char *text;    /* label / button text, image icon name, banner product */
    char *text2;   /* banner edition */
    xd_widget_cb cb;
    void *user;

    /* edit, editable combo */
    xd_textedit edit;
    double scroll; /* device px the text is scrolled left by */

    /* combo, list box, tabs */
    bool editable;
    char **items;
    size_t count, cap;
    int selected;
    int first;     /* list box: first visible row */
    int *tab_x;    /* tabs: logical x of each tab, relative to r.x */
    int *tab_w;

    int page;      /* tab page, or -1 = always shown */
    bool hidden;   /* xd_widget_set_visible(false) */
    bool checked;  /* checkbox */
    xd_draw_cb draw;
    xd_pointer_cb pointer; /* custom: mouse input */
    char *tooltip; /* shown after hovering, or nullptr */
    int smin, smax, svalue; /* slider */
};

/* The drop-down list of a combo box, or a panel the program draws
 * (xd_popup_custom: `combo` is nullptr, `anchor` the widget it hangs
 * from). */
typedef struct xd_listpopup {
    xd_window *owner;
    xd_widget *combo;
    xd_widget *anchor;
    xd_draw_cb draw;
    xd_pointer_cb pointer;
    void *user;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    int width, height; /* logical */
    int x, y;          /* relative to the owner window, as configured */
    int rows;          /* visible rows */
    int first;         /* first visible item (scrolling) */
    int hot;
    double scale;
    struct wl_callback *frame; /* pending frame callback: don't draw yet */
    bool configured;
    bool dirty;
    /* The scroll bar, when there are more items than rows. */
    uint8_t sb_pressed;       /* the part held down (sb_part in widgets.c), 0 = none */
    double sb_offset;         /* thumb drag: pointer y - thumb top */
    double pointer_y;         /* logical, while the pointer is over the list */
    xd_ui_source *sb_repeat;  /* auto-repeat of a held arrow or track */
} xd_listpopup;

/* A widget's tooltip: a popup of its window without a grab, which takes no
 * input. */
typedef struct xd_tooltip {
    xd_window *owner;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    struct wl_callback *frame;
    char *text;
    int width, height; /* logical */
    double scale;
    bool configured;
    bool dirty;
} xd_tooltip;

struct xd_window {
    struct wl_list link; /* xd_ui.windows */
    xd_ui *ui;
    bool main; /* closing it ends xd_ui_run (xd_window_set_main) */
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    xd_shm_ring ring;
    int width, height; /* logical */
    double scale;
    struct wl_callback *frame; /* pending frame callback: don't draw yet */
    bool configured;
    bool dirty;
    bool active;

    xd_widget **widgets;
    size_t count, cap;
    xd_widget *focus;
    xd_widget *def;    /* default button */
    xd_widget *cancel; /* cancel button */
    xd_widget *hot;
    xd_widget *pressed;
    bool selecting; /* dragging a text selection in `pressed` */
    bool thumb_drag; /* dragging a list box's scroll thumb, or a slider's */
    double thumb_offset;
    xd_widget *tabs; /* the window's tab control, if any */
    xd_listpopup *popup;
    /* Tooltips: the widget under the pointer that has one, the timer that
     * shows it (or, once shown, hides it again), and the tip itself. */
    xd_widget *tip_widget;
    xd_ui_source *tip_timer;
    xd_tooltip *tip;
};

/* A timer or watched file descriptor. */
struct xd_ui_source {
    struct wl_list link; /* xd_ui.sources */
    int fd;
    bool timer;  /* fd is ours, a timerfd */
    bool repeat;
    bool dead;   /* removed; freed after the current dispatch */
    xd_ui_source_cb cb;
    void *user;
};

/* wlr-output-management state (display.c). */
typedef struct xd_ui_head xd_ui_head;
typedef struct xd_ui_mode {
    struct wl_list link; /* xd_ui_head.modes */
    xd_ui_head *head;
    struct zwlr_output_mode_v1 *mode;
    xd_display_mode info;
} xd_ui_mode;

struct xd_ui_head {
    struct wl_list link; /* xd_ui.heads */
    struct zwlr_output_head_v1 *head;
    xd_display_head info;
    struct wl_list modes;
    struct zwlr_output_mode_v1 *current;
    xd_display_mode *mode_infos; /* snapshot handed out with info */
};

typedef struct xd_ui_output {
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_output;
    uint32_t global;
    xd_output_info info;
} xd_ui_output;

struct xd_ui {
    xd_visual_style style;
    xd_scheme scheme;
    xd_icons *icons;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *wm_base;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct wp_viewporter *viewporter;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape;
    bool cursor_set;  /* shape sent since the last pointer enter */
    bool cursor_text; /* ...and it was the text beam */
    bool cursor_busy; /* ...or the busy arrow */
    bool busy;        /* xd_ui_set_busy: the arrow shows work going on */
    struct wl_data_device_manager *data_device_manager;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    xd_ui_output *outputs;
    xd_output_info *output_infos; /* snapshot handed out by xd_ui_outputs */
    size_t output_count, output_cap;
    struct wl_data_device *data_device;
    struct wl_list sources;

    struct zwlr_output_manager_v1 *output_manager;
    uint32_t output_serial;
    struct wl_list heads;
    xd_display_head *head_infos;
    xd_ui_source_cb display_changed;
    void *display_changed_user;

    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;

    /* Key repeat for the focused text field. */
    int32_t repeat_rate, repeat_delay;
    int repeat_fd;
    uint32_t repeat_key;

    /* desktop.ini, watched: windows follow the appearance as it changes. */
    int settings_fd;

    struct wl_list windows;
    xd_window *pointer_window;
    xd_listpopup *pointer_popup;
    xd_window *keyboard_window;
    double pointer_x, pointer_y;
    uint32_t pointer_serial; /* last enter/button serial (cursor, popup grabs) */
    uint32_t key_serial;     /* last key serial (clipboard ownership) */

    /* Clipboard. */
    struct wl_data_offer *pending_offer;
    bool pending_offer_text;
    struct wl_data_offer *selection;
    bool selection_text;
    struct wl_data_source *source;
    char *copied; /* text we own on the clipboard */

    bool running;
};

/* ui.c */
void xd_ui_set_cursor_text(xd_ui *ui, bool text);
void xd_ui_clipboard_copy(xd_ui *ui, xd_str text);
/* Reads the clipboard as one line of text; caller frees. nullptr if none. */
char *xd_ui_clipboard_paste(xd_ui *ui);

/* display.c */
void xd_display_bind(xd_ui *ui, struct wl_registry *reg, uint32_t name, uint32_t version);
void xd_display_destroy(xd_ui *ui);

/* widgets.c */
void xd_window_render(xd_window *win);
void xd_popup_render(xd_listpopup *popup);
void xd_tooltip_render(xd_tooltip *tip);
xd_window *xd_window_for_surface(xd_ui *ui, struct wl_surface *surface);
xd_listpopup *xd_popup_for_surface(xd_ui *ui, struct wl_surface *surface);
void xd_window_pointer_motion(xd_window *win, double x, double y);
void xd_window_pointer_leave(xd_window *win);
void xd_window_pointer_button(xd_window *win, uint32_t button, bool pressed);
void xd_popup_pointer_motion(xd_listpopup *popup, double x, double y);
void xd_popup_pointer_button(xd_listpopup *popup, uint32_t button, bool pressed);
void xd_popup_pointer_axis(xd_listpopup *popup, double value);
void xd_window_pointer_axis(xd_window *win, double value);
/* A key press (or repeat): keysym, its text, and XD_MOD_* bits. */
void xd_window_key(xd_window *win, xkb_keysym_t sym, const char *utf8, uint32_t mods);

/* Modifier bits for xd_window_key. */
enum { XD_MOD_SHIFT = 1u << 0, XD_MOD_CTRL = 1u << 1, XD_MOD_ALT = 1u << 2 };

#endif
