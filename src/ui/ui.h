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

xd_widget *xd_checkbox(xd_window *win, xd_rect r, const char *label, bool checked,
                       xd_widget_cb on_change, void *user);
bool xd_checkbox_checked(const xd_widget *w);
void xd_checkbox_set(xd_widget *w, bool checked);

/* Single-selection list with a scroll bar. */
xd_widget *xd_listbox(xd_window *win, xd_rect r, xd_widget_cb on_change, void *user);
xd_widget *xd_groupbox(xd_window *win, xd_rect r, const char *label);

/* An area the caller draws (previews). r is in device pixels. */
typedef void (*xd_draw_cb)(xd_widget *w, cairo_t *cr, xd_rect r, const xd_theme *t, void *user);
xd_widget *xd_custom(xd_window *win, xd_rect r, xd_draw_cb draw, void *user);
void xd_widget_redraw(xd_widget *w);

/* Items of a combo or list box; selection of a combo, list box or tabs. */
void xd_widget_add_item(xd_widget *w, const char *item);
int xd_widget_selected(const xd_widget *w);
void xd_widget_select(xd_widget *w, int index);
size_t xd_widget_item_count(const xd_widget *w);

/* Text of a label, edit or combo (for a drop-down list, the selected
 * item). Never nullptr. */
const char *xd_widget_text(xd_widget *w);
void xd_widget_set_text(xd_widget *w, const char *text);
void xd_widget_set_enabled(xd_widget *w, bool enabled);
xd_window *xd_widget_window(xd_widget *w);

#endif
