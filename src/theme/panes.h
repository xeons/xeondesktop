/* SPDX-License-Identifier: MIT
 * Explorer's panes: XP's task pane, and the parts of Vista's and 7's
 * Explorer that the file manager has (the command bar, the address box,
 * the Folders tree's items, the details pane under the view). The active
 * theme package draws them where it styles them (docs/THEMES.md, "Which
 * parts draw what" and "shell/NAME.xml"); otherwise the task pane is drawn
 * from the colour scheme, and the rest is left to the toolkit.
 *
 * Lengths are device pixels at the theme's scale.
 */
#ifndef XD_THEME_PANES_H
#define XD_THEME_PANES_H

#include "theme.h"

/* ---- XP's task pane ---- */

typedef struct xd_taskpane_style {
    xd_insets padding;   /* the pane's, around its groups */
    int group_spacing;   /* between two groups */
    int header_height;   /* a group's title bar */
    xd_insets body[2];   /* inside a group's body: [special] */
    int link_spacing[2]; /* above each task */
    int icon_gap[2];     /* between a task's icon and its text */
    int icon_size;
    int details_spacing; /* above each line of the Details group */
    /* Fonts and colours: [special][hot]. */
    xd_font_spec header_font[2];
    xd_rgba header_color[2][2];
    xd_font_spec link_font[2][2];
    bool link_underline[2][2];
    xd_rgba link_color[2][2];
    xd_font_spec details_font, details_name_font;
    xd_rgba details_color;
} xd_taskpane_style;

void xd_theme_taskpane_style(const xd_theme *theme, xd_taskpane_style *out);
/* The pane's background, all of `rect`. */
void xd_theme_draw_taskpane(const xd_theme *theme, cairo_t *cairo, xd_rect rect);
/* A group's title bar with its title and its Expand or Collapse button
 * (`expanded`: Collapse); `state` XD_STATE_HOT / PRESSED / FOCUSED. */
void xd_theme_draw_taskpane_header(const xd_theme *theme, cairo_t *cairo,
                                   const xd_taskpane_style *taskpane_style, xd_rect rect,
                                   bool special, bool expanded, uint8_t state, const char *title);
/* A group's body, behind its tasks. */
void xd_theme_draw_taskpane_body(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool special);

/* ---- Vista's and 7's Explorer, where a package styles it ---- */

typedef enum xd_explorer_part {
    XD_EXPLORER_COMMAND_BAR,    /* the toolbar: CommandModule's ModuleBackground */
    XD_EXPLORER_COMMAND_BUTTON, /* a toolbar button, hot or pressed: TaskButton */
    XD_EXPLORER_ADDRESS_BOX,    /* the address box: AddressBand's Background */
    XD_EXPLORER_TREE_ITEM,      /* a Folders tree row: Explorer::TreeView's TreeItem */
    XD_EXPLORER_DETAILS_PANE,   /* under the view: PreviewPane's PreviewBackground */
} xd_explorer_part;

/* Draws the part in `state` (XD_STATE_HOT, PRESSED, FOCUSED, CHECKED for a
 * selected tree row, DISABLED); false, drawing nothing, when no package
 * styles it (the caller draws as it does without one). */
bool xd_theme_draw_explorer_part(const xd_theme *theme, cairo_t *cairo, xd_explorer_part part,
                                 uint8_t state, xd_rect rect);
/* The text colour the package gives the part in `state`, if any. */
bool xd_theme_explorer_text_color(const xd_theme *theme, xd_explorer_part part, uint8_t state,
                                  xd_rgba *out);

/* ---- internal: style_pkg.c's drawing from a package (false: it doesn't
 * style the pane), under the functions above ---- */
bool xd_pkgstyle_has_taskpane(const xd_theme *theme);
/* Overwrites what the package gives. */
void xd_pkgstyle_taskpane_style(const xd_theme *theme, xd_taskpane_style *taskpane_style);
bool xd_pkgstyle_draw_taskpane(const xd_theme *theme, cairo_t *cairo, xd_rect rect);
bool xd_pkgstyle_draw_taskpane_header(const xd_theme *theme, cairo_t *cairo,
                                      const xd_taskpane_style *taskpane_style, xd_rect rect,
                                      bool special, bool expanded, uint8_t state,
                                      const char *title);
bool xd_pkgstyle_draw_taskpane_body(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                    bool special);
bool xd_pkgstyle_draw_explorer_part(const xd_theme *theme, cairo_t *cairo, xd_explorer_part part,
                                    uint8_t state, xd_rect rect);
bool xd_pkgstyle_explorer_text_color(const xd_theme *theme, xd_explorer_part part, uint8_t state,
                                     xd_rgba *out);

#endif
