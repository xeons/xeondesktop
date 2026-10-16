/* SPDX-License-Identifier: MIT
 * Theme engine public API.
 *
 * A theme is a visual style (Classic, Luna, Whistler) plus a color scheme and
 * a scale factor. Classic draws everything from the scheme's system colors
 * the way Windows 2000 does, so classic color schemes apply directly. The
 * Luna and Whistler styles have their own artwork, which is drawn
 * procedurally, and use the scheme only for text and client-area colors.
 */
#ifndef XD_THEME_THEME_H
#define XD_THEME_THEME_H

#include "draw.h"

typedef enum xd_visual_style {
    XD_STYLE_CLASSIC,
    XD_STYLE_LUNA,
    XD_STYLE_WHISTLER,
    XD_STYLE_COUNT
} xd_visual_style;

const char *xd_visual_style_name(xd_visual_style s);
[[nodiscard]] bool xd_visual_style_from_name(xd_str name, xd_visual_style *out);

typedef struct xd_theme {
    xd_visual_style style;
    xd_scheme scheme;
    double scale; /* device pixels per logical pixel, e.g. 1.0, 1.25, 2.0 */
} xd_theme;

void xd_theme_init(xd_theme *theme, xd_visual_style style, const xd_scheme *scheme,
                   double scale);

/* Scheme colour as RGBA. */
xd_rgba xd_sys(const xd_theme *t, xd_syscolor c);
/* Logical (96-DPI) length -> device pixels at this theme's scale. */
int xd_tpx(const xd_theme *t, double logical);
int xd_metric_px(const xd_theme *t, xd_metric m);

typedef enum xd_caption_buttons : uint8_t {
    XD_CAPBTN_CLOSE = 1u << 0,
    XD_CAPBTN_MAXIMIZE = 1u << 1,
    XD_CAPBTN_MINIMIZE = 1u << 2,
    XD_CAPBTN_HELP = 1u << 3,
} xd_caption_buttons;

typedef enum xd_widget_state : uint8_t {
    XD_STATE_NORMAL = 0,
    XD_STATE_HOT = 1u << 0,
    XD_STATE_PRESSED = 1u << 1,
    XD_STATE_DISABLED = 1u << 2,
    XD_STATE_FOCUSED = 1u << 3,
    XD_STATE_DEFAULT = 1u << 4,
    XD_STATE_CHECKED = 1u << 5,
} xd_widget_state;

/* Parts of a window frame, as returned by hit testing (WM_NCHITTEST). */
typedef enum xd_frame_part : uint8_t {
    XD_PART_NONE,
    XD_PART_CLIENT,
    XD_PART_CAPTION,
    XD_PART_SYSMENU, /* the caption icon */
    XD_PART_CLOSE,
    XD_PART_MAXIMIZE,
    XD_PART_MINIMIZE,
    XD_PART_HELP,
    XD_PART_TOP,
    XD_PART_BOTTOM,
    XD_PART_LEFT,
    XD_PART_RIGHT,
    XD_PART_TOP_LEFT,
    XD_PART_TOP_RIGHT,
    XD_PART_BOTTOM_LEFT,
    XD_PART_BOTTOM_RIGHT,
} xd_frame_part;

bool xd_frame_part_is_button(xd_frame_part p);
bool xd_frame_part_is_border(xd_frame_part p);

typedef struct xd_frame {
    const char *title;
    bool active;
    bool maximized;
    uint8_t buttons;      /* xd_caption_buttons */
    xd_frame_part hot;     /* caption button under the pointer, or NONE */
    xd_frame_part pressed; /* caption button held down, or NONE */
} xd_frame;

typedef struct xd_insets {
    int left, top, right, bottom;
} xd_insets;

#define XD_FRAME_MAX_BUTTONS 4

/* Where each piece of the frame goes, in device px. */
typedef struct xd_frame_layout {
    xd_rect caption;
    xd_rect icon;
    xd_rect title;
    size_t button_count;
    xd_rect button_rects[XD_FRAME_MAX_BUTTONS];
    xd_frame_part button_parts[XD_FRAME_MAX_BUTTONS];
} xd_frame_layout;

/* Space taken by the non-client frame (borders + caption), in device px. */
xd_insets xd_theme_frame_insets(const xd_theme *t, const xd_frame *f);
void xd_theme_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                           xd_frame_layout *out);
/* Which part of a frame drawn at `outer` the device-px point (x, y) is on. */
xd_frame_part xd_theme_frame_hit(const xd_theme *t, xd_rect outer, const xd_frame *f, int x,
                                 int y);
void xd_theme_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f);
/* Background colour for dialog client areas. */
xd_rgba xd_theme_dialog_color(const xd_theme *t);

void xd_theme_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                          const char *label);
void xd_theme_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                            const char *label);

int xd_theme_taskbar_height(const xd_theme *t);
void xd_theme_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r);
int xd_theme_start_button_width(const xd_theme *t, cairo_t *cr);
void xd_theme_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
void xd_theme_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label);
/* Notification area well at the right end of the taskbar. */
void xd_theme_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock);

#endif
