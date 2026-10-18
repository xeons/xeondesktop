/* SPDX-License-Identifier: MIT
 * libxdui: a small Win32-style widget layer for shell dialogs.
 *
 * Windows are fixed-size dialogs laid out in logical units (1 = 1px at
 * 96 DPI), like a Windows .rc dialog template. Everything is drawn by
 * libxdtheme at each output's fractional scale. Dialog conventions follow
 * Windows: Tab moves focus, Enter presses the default button, Esc or the
 * close button presses the cancel button.
 *
 * Larger apps use GTK3 instead (see docs/ARCHITECTURE.md).
 */
#ifndef XD_UI_UI_H
#define XD_UI_UI_H

#include "../theme/theme.h"
#include "icons.h"

typedef struct xd_ui xd_ui;
typedef struct xd_window xd_window;
typedef struct xd_widget xd_widget;
typedef void (*xd_widget_cb)(xd_widget *widget, void *user);

/* Connects to the compositor. Reads --style and --scheme (as passed by the
 * shell) and ignores other arguments, which belong to the caller. Returns
 * nullptr after printing an error. */
[[nodiscard]] xd_ui *xd_ui_connect(int argc, char **argv);
void xd_ui_run(xd_ui *ui);
void xd_ui_quit(xd_ui *ui);
void xd_ui_destroy(xd_ui *ui);
xd_icons *xd_ui_icons(xd_ui *ui);
/* The visual style windows are drawn in (--style, else the settings'). */
xd_visual_style xd_ui_style(const xd_ui *ui);

/* Monitors, as the compositor reports them. */
typedef struct xd_output_info {
    char name[64];         /* connector, e.g. "DP-1" */
    char description[128]; /* make and model */
    int x, y;              /* logical position in the desktop */
    int logical_w, logical_h;
    int mode_w, mode_h;    /* pixels */
    int refresh_mhz;
    int phys_w_mm, phys_h_mm;
} xd_output_info;

/* Valid until the next event dispatch. */
size_t xd_ui_outputs(xd_ui *ui, const xd_output_info **out);

/* ---- timers and file descriptors ------------------------------------------- */

/* Run from xd_ui_run's loop. Removing a source from its own callback (or
 * any other) is fine. */
typedef struct xd_ui_source xd_ui_source;
typedef void (*xd_ui_source_cb)(void *user);
xd_ui_source *xd_ui_add_timer(xd_ui *ui, int interval_ms, bool repeat, xd_ui_source_cb cb,
                              void *user);
/* Calls cb when fd is readable or hung up. The caller keeps owning fd. */
xd_ui_source *xd_ui_add_fd(xd_ui *ui, int fd, xd_ui_source_cb cb, void *user);
void xd_ui_remove_source(xd_ui_source *src);

/* ---- display configuration (wlr-output-management) ------------------------- */

typedef struct xd_display_mode {
    int width, height; /* pixels */
    int refresh_mhz;
    bool preferred;
} xd_display_mode;

typedef enum xd_display_transform : uint8_t {
    XD_TRANSFORM_NORMAL,
    XD_TRANSFORM_90,
    XD_TRANSFORM_180,
    XD_TRANSFORM_270,
} xd_display_transform;

/* One monitor as the compositor can configure it. */
typedef struct xd_display_head {
    char name[64];         /* connector, e.g. "DP-1" */
    char description[128]; /* make and model */
    int phys_w_mm, phys_h_mm;
    bool enabled;
    int mode;              /* index into modes, -1 if custom or off */
    int x, y;              /* logical position */
    double scale;
    xd_display_transform transform;
    const xd_display_mode *modes;
    size_t mode_count;
} xd_display_head;

/* What to apply to one head (matched by name). */
typedef struct xd_display_setting {
    char name[64];
    bool enabled;
    int mode; /* index into the head's modes */
    int x, y;
    double scale;
    xd_display_transform transform;
} xd_display_setting;

/* False if the compositor can't change display settings. The heads are
 * valid until the next event dispatch. */
bool xd_ui_display_heads(xd_ui *ui, const xd_display_head **heads, size_t *count);
/* Runs when the compositor reports a new set of heads (after an apply,
 * hotplug, or another program's change). */
void xd_ui_on_display_change(xd_ui *ui, xd_ui_source_cb cb, void *user);
/* Applies the settings; done(ok) runs once the compositor answers. */
typedef void (*xd_display_done_cb)(bool ok, void *user);
void xd_ui_display_apply(xd_ui *ui, const xd_display_setting *settings, size_t count,
                         xd_display_done_cb done, void *user);
/* Re-themes every open window, e.g. after the user applied new settings. */
void xd_ui_set_theme(xd_ui *ui, xd_visual_style style, const xd_scheme *scheme);

/* A fixed-size dialog window; the compositor gives it a dialog frame. */
xd_window *xd_window_create(xd_ui *ui, const char *title, const char *app_id, int width,
                            int height);
void xd_window_show(xd_window *win);
/* Destroys the window; the app quits when its last window closes. */
void xd_window_close(xd_window *win);
xd_ui *xd_window_ui(xd_window *win);
/* Pressed by Enter (unless another button has focus). */
void xd_window_set_default(xd_window *win, xd_widget *button);
/* Pressed by Esc and the caption's close button. */
void xd_window_set_cancel(xd_window *win, xd_widget *button);
void xd_window_set_focus(xd_window *win, xd_widget *widget);

enum { XD_LABEL_WRAP = 1u << 0, XD_LABEL_BOLD = 1u << 1, XD_LABEL_DIM = 1u << 2 };

xd_widget *xd_label(xd_window *win, xd_rect r, const char *text, uint8_t flags);
xd_widget *xd_image(xd_window *win, xd_rect r, const char *icon_name);
xd_widget *xd_banner(xd_window *win, xd_rect r, const char *product, const char *edition);
xd_widget *xd_button(xd_window *win, xd_rect r, const char *label, xd_widget_cb on_click,
                     void *user);
xd_widget *xd_edit(xd_window *win, xd_rect r, const char *text);
/* Runs on_change whenever an edit's text changes (typing, pasting;
 * xd_widget_set_text doesn't). */
void xd_edit_set_on_change(xd_widget *edit, xd_widget_cb on_change, void *user);
/* Editable combos have a text field (Run's "Open:" box); others are drop-
 * down lists (Shut Down's choice). on_change runs when the selection or
 * text changes. */
xd_widget *xd_combo(xd_window *win, xd_rect r, bool editable, xd_widget_cb on_change,
                    void *user);
void xd_combo_add(xd_widget *combo, const char *item);
int xd_combo_selected(const xd_widget *combo);
void xd_combo_select(xd_widget *combo, int index);

/* Property-sheet tabs. Widgets join a page with xd_widget_set_page and are
 * only shown (and focusable) while it's selected. One tab control per
 * window. */
xd_widget *xd_tabs(xd_window *win, xd_rect r, const char *const *labels, size_t count,
                   xd_widget_cb on_change, void *user);
void xd_widget_set_page(xd_widget *w, int page);
/* Shows or hides a widget (Windows 7's expanders: Show color mixer). */
void xd_widget_set_visible(xd_widget *w, bool visible);

xd_widget *xd_checkbox(xd_window *win, xd_rect r, const char *label, bool checked,
                       xd_widget_cb on_change, void *user);
bool xd_checkbox_checked(const xd_widget *w);
void xd_checkbox_set(xd_widget *w, bool checked);

/* Radio buttons. A group starts at a button made with `group` set (Windows'
 * WS_GROUP) and runs through the radio buttons added right after it;
 * checking one unchecks the others. Tab stops only at the group's checked
 * button; the arrow keys move round the group and check as they go.
 * on_change runs for the button that became checked; xd_checkbox_checked
 * reads a radio button too. */
xd_widget *xd_radio(xd_window *win, xd_rect r, const char *label, bool group,
                    xd_widget_cb on_change, void *user);
/* Checks w and unchecks the rest of its group (on_change doesn't run). */
void xd_radio_set(xd_widget *w);

/* Single-selection list with a scroll bar. */
xd_widget *xd_listbox(xd_window *win, xd_rect r, xd_widget_cb on_change, void *user);
xd_widget *xd_groupbox(xd_window *win, xd_rect r, const char *label);

/* An area the caller draws (previews). r is in device pixels. */
typedef void (*xd_draw_cb)(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t, void *user);
xd_widget *xd_custom(xd_window *win, xd_rect r, xd_draw_cb draw, void *user);
void xd_widget_redraw(xd_widget *w);
/* Mouse input for a custom area: a left-button press, then motion until
 * the release. x and y are logical, relative to the area. */
typedef enum xd_pointer_event : uint8_t {
    XD_POINTER_PRESS,
    XD_POINTER_MOTION,
    XD_POINTER_RELEASE,
} xd_pointer_event;
typedef void (*xd_pointer_cb)(xd_widget *w, xd_pointer_event ev, double x, double y, void *user);
void xd_custom_set_pointer(xd_widget *w, xd_pointer_cb cb);
/* A drop-down panel the program draws (Windows' colour picker), hanging
 * from `anchor` as a combo box's list does: `draw` paints it (r in device
 * px, from 0,0) and `pointer` gets the pointer over it (logical, relative
 * to it; motion comes with no button held too, for hover). Like the list,
 * it takes the keyboard (Esc closes it) and closes on a click anywhere
 * else. Callbacks get `anchor` as their widget. */
void xd_popup_custom(xd_widget *anchor, int width, int height, xd_draw_cb draw,
                     xd_pointer_cb pointer, void *user);
/* Closes the window's drop-down list or panel, if one is open. */
void xd_popup_dismiss(xd_window *win);

/* A trackbar (Windows' slider) from min to max with a tick per step.
 * on_change runs as the thumb moves. */
xd_widget *xd_slider(xd_window *win, xd_rect r, int min, int max, int value,
                     xd_widget_cb on_change, void *user);
int xd_slider_value(const xd_widget *w);
void xd_slider_set(xd_widget *w, int min, int max, int value);

/* Items of a combo or list box; selection of a combo, list box or tabs. */
void xd_widget_add_item(xd_widget *w, const char *item);
void xd_widget_clear_items(xd_widget *w);
int xd_widget_selected(const xd_widget *w);
void xd_widget_select(xd_widget *w, int index);
size_t xd_widget_item_count(const xd_widget *w);

/* Text of a label, edit or combo (for a drop-down list, the selected
 * item). Never nullptr. */
const char *xd_widget_text(xd_widget *w);
void xd_widget_set_text(xd_widget *w, const char *text);
void xd_widget_set_enabled(xd_widget *w, bool enabled);
/* A tooltip, shown when the pointer rests on the widget for half a second
 * (in the active window), as Windows' tooltip controls do; nullptr or ""
 * removes it. Any widget can have one, labels and images too. */
void xd_widget_set_tooltip(xd_widget *w, const char *text);
xd_window *xd_widget_window(xd_widget *w);

#endif
