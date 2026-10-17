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
    void (*draw_tray)(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock);
    void (*menu_palette)(const xd_theme *t, struct xd_menu_palette *out);
    void (*toolkit_colors)(const xd_theme *t, xd_toolkit_colors *out);
} xd_style_ops;

extern const xd_style_ops xd_style_classic;
extern const xd_style_ops xd_style_luna;
extern const xd_style_ops xd_style_whistler;

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
