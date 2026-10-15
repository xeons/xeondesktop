/* SPDX-License-Identifier: MIT
 * Internal: per-visual-style operation tables. Not part of the public API.
 */
#ifndef XD_THEME_STYLE_H
#define XD_THEME_STYLE_H

#include "theme.h"

typedef struct xd_style_ops {
    const char *name;
    xd_insets (*frame_insets)(const xd_theme *t, const xd_frame *f);
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
} xd_style_ops;

extern const xd_style_ops xd_style_classic;
extern const xd_style_ops xd_style_luna;
extern const xd_style_ops xd_style_whistler;

/* Helpers shared by the styles. */
xd_text_style xd_text(const xd_theme *t, xd_font_role role, xd_rgba color, xd_halign align);

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

/* Win2000 "flag" logo used on the Classic/Whistler start buttons. */
void xd_draw_start_flag(cairo_t *cr, xd_rect r);

#endif
