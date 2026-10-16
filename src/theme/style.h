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
    void (*draw_taskbar)(const xd_theme *t, cairo_t *cr, xd_rect r);
    int (*start_button_width)(const xd_theme *t, cairo_t *cr);
    void (*draw_start_button)(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
    void (*draw_task_button)(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                             const char *label);
    void (*draw_tray)(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock);
    void (*menu_palette)(const xd_theme *t, struct xd_menu_palette *out);
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

/* Win2000 "flag" logo used on the Classic/Whistler start buttons. */
void xd_draw_start_flag(cairo_t *cr, xd_rect r);

#endif
