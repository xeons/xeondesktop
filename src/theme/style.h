/* SPDX-License-Identifier: MIT
 * Internal: per-visual-style operation tables. Not part of the public API.
 */
#ifndef XD_THEME_STYLE_H
#define XD_THEME_STYLE_H

#include "theme.h"

/* Colours and shape of menus, supplied by each style. */
typedef struct xd_menu_palette {
    bool flat;            /* XP flat menus: 1px border, no 3D edge */
    xd_rgba background;
    xd_rgba border;       /* flat menus only */
    xd_rgba text;
    xd_rgba hilight;
    xd_rgba hilight_text;
    xd_rgba hilight_border; /* drawn around flat highlights if alpha > 0 */
    xd_rgba disabled_text;
    xd_rgba disabled_emboss; /* classic etched disabled text if alpha > 0 */
    xd_rgba separator_dark;
    xd_rgba separator_light; /* alpha 0 for a single-line separator */
    xd_rgba banner_top;
    xd_rgba banner_bottom;
    xd_rgba banner_text;
} xd_menu_palette;

typedef struct xd_style_ops {
    const char *name;
    xd_insets (*frame_insets)(const xd_theme *t, const xd_frame *f);
    void (*frame_layout)(const xd_theme *t, xd_rect outer, const xd_frame *f,
                         xd_frame_layout *out);
    void (*draw_frame)(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f);
    xd_rgba (*dialog_color)(const xd_theme *t);
    void (*draw_button)(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                        const char *label);
    void (*draw_checkbox)(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                          const char *label);
    int (*taskbar_height)(const xd_theme *t);
    void (*draw_taskbar)(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge);
    int (*start_button_width)(const xd_theme *t, cairo_t *cr);
    void (*draw_start_button)(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
    void (*draw_task_button)(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                             const char *label, cairo_surface_t *icon);
    /* Optional: the others draw a task button. */
    void (*draw_app_button)(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                            const xd_app_button *b);
    void (*draw_tray)(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock);
    void (*menu_palette)(const xd_theme *t, struct xd_menu_palette *out);
    void (*toolkit_colors)(const xd_theme *t, xd_toolkit_colors *out);
} xd_style_ops;

extern const xd_style_ops xd_style_classic;
extern const xd_style_ops xd_style_luna;
extern const xd_style_ops xd_style_whistler;
extern const xd_style_ops xd_style_aero;

/* pkg.c: sets t's package and image scaling to the active ones (and, with
 * a package, its base style and its [SysMetrics] over the scheme). */
void xd_theme_attach_package(xd_theme *t);

/* style_pkg.c: a package's window frames (uxtheme's Window class), used
 * when it has a caption. */
bool xd_pkgstyle_has_frame(const xd_theme *t);
/* Aero's caption button from a Vista/7 package's DWMWindow parts, when it
 * has them and the glass is on: false to draw Aero's own. */
bool xd_pkgstyle_dwm_button(const xd_theme *t, cairo_t *cr, xd_rect r, xd_frame_part part,
                            const xd_frame *f, bool leftmost, bool alone);
xd_insets xd_pkgstyle_frame_insets(const xd_theme *t, const xd_frame *f);
void xd_pkgstyle_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                              xd_frame_layout *l);
void xd_pkgstyle_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f);
/* The controls a package styles (Button, Edit, ComboBox, ScrollBar, Tab):
 * true when drawn, false to draw them as the style does. */
bool xd_pkgstyle_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                             const char *label);
bool xd_pkgstyle_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label);
bool xd_pkgstyle_draw_radio(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                            const char *label);
bool xd_pkgstyle_tab_page_color(const xd_theme *t, xd_rgba *out);
bool xd_pkgstyle_draw_groupbox(const xd_theme *t, cairo_t *cr, xd_rect r, const char *label);
bool xd_pkgstyle_draw_field(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled,
                            xd_rect *inner);
bool xd_pkgstyle_draw_dropdown_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                      uint8_t state);
bool xd_pkgstyle_draw_vscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rect thumb,
                                 bool disabled);
bool xd_pkgstyle_draw_tab(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                          const char *label);
bool xd_pkgstyle_draw_tab_panel(const xd_theme *t, cairo_t *cr, xd_rect r);
/* The rest (ScrollBar, Progress, TrackBar, Spin, Header, TreeView, Status,
 * Tooltip, Rebar, Toolbar, TaskBar::Toolbar and ::Rebar, and Vista's and
 * 7's Menu): true when drawn, as above. */
bool xd_pkgstyle_draw_hscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rect thumb,
                                 bool disabled);
bool xd_pkgstyle_draw_sizebox(const xd_theme *t, cairo_t *cr, xd_rect r);
bool xd_pkgstyle_draw_progress(const xd_theme *t, cairo_t *cr, xd_rect r, double fraction);
bool xd_pkgstyle_draw_trackbar(const xd_theme *t, cairo_t *cr, xd_rect channel, xd_rect thumb,
                               uint8_t state);
bool xd_pkgstyle_trackbar_tics_color(const xd_theme *t, xd_rgba *out);
bool xd_pkgstyle_draw_spin(const xd_theme *t, cairo_t *cr, xd_rect r, bool up, uint8_t state);
bool xd_pkgstyle_draw_header_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label);
bool xd_pkgstyle_draw_tree_glyph(const xd_theme *t, cairo_t *cr, xd_rect r, bool open);
bool xd_pkgstyle_draw_statusbar(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text,
                                bool grip);
bool xd_pkgstyle_draw_tooltip(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text);
bool xd_pkgstyle_draw_rebar(const xd_theme *t, cairo_t *cr, xd_rect r, bool gripper);
bool xd_pkgstyle_draw_tool_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label);
bool xd_pkgstyle_draw_taskbar_toolbar_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                             uint8_t state);
bool xd_pkgstyle_draw_taskbar_grip(const xd_theme *t, cairo_t *cr, xd_rect r);
bool xd_pkgstyle_draw_menu_frame(const xd_theme *t, cairo_t *cr, xd_rect r);
/* An item's background; `text` gets its text colour. */
bool xd_pkgstyle_draw_menu_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                xd_rgba *text);
bool xd_pkgstyle_draw_menu_separator(const xd_theme *t, cairo_t *cr, xd_rect r);
bool xd_pkgstyle_draw_menu_submenu(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled);
bool xd_pkgstyle_draw_menu_check(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled);
bool xd_pkgstyle_draw_menubar(const xd_theme *t, cairo_t *cr, xd_rect r, bool active);
bool xd_pkgstyle_draw_menubar_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                   const char *label);

/* The taskbar, thumbnails, jump lists and Start menus a package styles
 * (style_pkg.c's own section): true when drawn, false to draw them as
 * the style does. */
bool xd_pkgstyle_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge);
bool xd_pkgstyle_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
/* count > 1: a group's button. */
bool xd_pkgstyle_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label, cairo_surface_t *icon, int count);
bool xd_pkgstyle_draw_app_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                 const xd_app_button *b);
bool xd_pkgstyle_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock,
                           xd_edge edge);
bool xd_pkgstyle_clock_color(const xd_theme *t, xd_rgba *out);
bool xd_pkgstyle_draw_thumb_panel(const xd_theme *t, cairo_t *cr, xd_rect r);
bool xd_pkgstyle_draw_thumb_tile(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
bool xd_pkgstyle_thumb_title_color(const xd_theme *t, xd_rgba *out);
bool xd_pkgstyle_draw_thumb_close(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
bool xd_pkgstyle_draw_jumplist_frame(const xd_theme *t, cairo_t *cr, xd_rect r, int band_y);
bool xd_pkgstyle_jumplist_header_color(const xd_theme *t, xd_rgba *out);
/* An item's background when hot; `text` gets its colour. */
bool xd_pkgstyle_draw_jumplist_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                    xd_rgba *text);
bool xd_pkgstyle_draw_jumplist_pin(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
/* Menus of xd_theme.menu_look's Start menus: StartMenu::Toolbar and
 * ::MenuBand (XP, 7), our ClassicStartMenu (Windows 2000). */
bool xd_pkgstyle_draw_start_menu_frame(const xd_theme *t, cairo_t *cr, xd_rect r);
bool xd_pkgstyle_draw_start_menu_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                      xd_rgba *text);
bool xd_pkgstyle_draw_start_menu_separator(const xd_theme *t, cairo_t *cr, xd_rect r);
bool xd_pkgstyle_draw_start_menu_submenu(const xd_theme *t, cairo_t *cr, xd_rect r,
                                         uint8_t state);
/* The banner's background; its text's colour and (bold) font. */
bool xd_pkgstyle_draw_start_banner(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rgba *text,
                                   xd_font_spec *font);

/* theme.c: the style's menu colours. */
void xd_theme_menu_palette(const xd_theme *t, xd_menu_palette *out);

/* Helpers shared by the styles. */

/* Classic glyphs (Marlett equivalents) drawn as pixel shapes, so the
 * Luna/Whistler styles can reuse them on their own button art. */
typedef enum xd_glyph {
    XD_GLYPH_CLOSE,
    XD_GLYPH_MAXIMIZE,
    XD_GLYPH_RESTORE,
    XD_GLYPH_MINIMIZE,
    XD_GLYPH_HELP,
    XD_GLYPH_CHECK,
} xd_glyph;
void xd_draw_glyph(const xd_theme *t, cairo_t *cr, xd_rect r, xd_glyph g, xd_rgba color,
                   double weight);

void xd_layout_add_button(xd_frame_layout *l, xd_rect r, xd_frame_part part);
xd_glyph xd_glyph_for_part(xd_frame_part part, bool maximized);

/* luna.c: one of Blue's colours (0xRRGGBB) in the theme's Luna colour
 * variant, recoloured as the frame is (unchanged for Blue). */
uint32_t xd_luna_variant_rgb(const xd_theme *t, bool active, uint32_t rgb);
/* luna.c: Luna's frame, caption buttons and taskbar, from measured colours. */
xd_insets xd_luna_frame_insets(const xd_theme *t, const xd_frame *f);
void xd_luna_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                          xd_frame_layout *l);
void xd_luna_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f);
int xd_luna_taskbar_height(const xd_theme *t);
void xd_luna_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge);
/* A line `i` px in from a taskbar's desktop-facing rim, `px` thick. */
xd_rect xd_style_rim_line(xd_rect r, xd_edge edge, int i, int px);
int xd_luna_start_button_width(const xd_theme *t, cairo_t *cr);
void xd_luna_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
void xd_luna_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                              const char *label, cairo_surface_t *icon);
/* Paints a task button's window icon (or the generic one) at x, y. */
void xd_style_task_icon(const xd_theme *t, cairo_t *cr, cairo_surface_t *icon, int x, int y);
void xd_luna_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock);
void xd_luna_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                         const char *label);
void xd_luna_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                           const char *label);


#endif
