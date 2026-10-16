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
/* Text style for a scheme font role at this theme's scale. */
xd_text_style xd_text(const xd_theme *t, xd_font_role role, xd_rgba color, xd_halign align);

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
    XD_STATE_FLASH = 1u << 6, /* a task button asking for attention */
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
    bool dialog;          /* fixed size: thin frame, no resize edges */
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
/* Taskbar toolbar grip (the ridge you drag a toolbar by), vertical. */
int xd_theme_toolbar_grip_width(const xd_theme *t);
void xd_theme_draw_toolbar_grip(const xd_theme *t, cairo_t *cr, xd_rect r);
/* Flat toolbar button (Quick Launch): only its state is drawn; the caller
 * paints the icon. state: XD_STATE_HOT, _PRESSED. */
void xd_theme_draw_toolbar_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);

/* Notification area well at the right end of the taskbar. */
void xd_theme_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock);

/* ---- menus (device px) ---- */

typedef struct xd_menu_metrics {
    int border;       /* frame thickness on every side */
    int item_h;       /* item with a 16px icon */
    int large_item_h; /* Start menu item with a 32px icon */
    int icon;         /* 16 logical */
    int large_icon;   /* 32 logical */
    int icon_x;       /* icon offset from the item's left edge */
    int text_x;       /* text offset, small items */
    int large_text_x; /* text offset, large items */
    int arrow_w;      /* space reserved for the submenu arrow */
    int separator_h;
    int banner_w;     /* Windows 2000 Start menu side banner */
} xd_menu_metrics;

xd_menu_metrics xd_theme_menu_metrics(const xd_theme *t);
/* Background and border of a whole menu. */
void xd_theme_draw_menu_frame(const xd_theme *t, cairo_t *cr, xd_rect r);
/* One item; the caller draws the icon. state: XD_STATE_HOT, _DISABLED. */
void xd_theme_draw_menu_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                             const char *label, bool submenu, bool large);
void xd_theme_draw_menu_separator(const xd_theme *t, cairo_t *cr, xd_rect r);

/* Window-menu glyphs, drawn in a menu item's icon column. */
typedef enum xd_menu_glyph : uint8_t {
    XD_MENU_GLYPH_NONE,
    XD_MENU_GLYPH_RESTORE,
    XD_MENU_GLYPH_MINIMIZE,
    XD_MENU_GLYPH_MAXIMIZE,
    XD_MENU_GLYPH_CLOSE,
} xd_menu_glyph;

typedef struct xd_menu_item_extras {
    bool bold;          /* the default item */
    bool checked;       /* check mark in the icon column */
    xd_menu_glyph glyph;
    const char *accel;  /* right-aligned shortcut, e.g. "Alt+F4"; may be nullptr */
} xd_menu_item_extras;

/* xd_theme_draw_menu_item with a bold default, check mark, glyph and
 * shortcut text. */
void xd_theme_draw_menu_item_ex(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                const char *label, bool submenu, bool large,
                                const xd_menu_item_extras *x);
/* Width of a bold label (default items). */
int xd_theme_menu_bold_text_width(const xd_theme *t, cairo_t *cr, const char *label);
/* Vertical banner with text reading bottom to top, e.g. "XeonDesktop" in
 * bold followed by "Professional". */
void xd_theme_draw_start_banner(const xd_theme *t, cairo_t *cr, xd_rect r, const char *product,
                                const char *edition);
/* Width of a menu item's label in device px. */
int xd_theme_menu_text_width(const xd_theme *t, cairo_t *cr, const char *label);

/* ---- dialog controls (device px) ---- */

/* Colours for text in fields and lists. */
typedef struct xd_field_colors {
    xd_rgba text, disabled_text, selection, selection_text;
} xd_field_colors;
xd_field_colors xd_theme_field_colors(const xd_theme *t);

/* Sunken frame of a text field or list box. Returns the content rect. */
xd_rect xd_theme_draw_field(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled);
int xd_theme_dropdown_button_width(const xd_theme *t);
/* The arrow button of a combo box. state: XD_STATE_HOT, _PRESSED, _DISABLED. */
void xd_theme_draw_dropdown_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
void xd_theme_draw_list_item(const xd_theme *t, cairo_t *cr, xd_rect r, bool selected,
                             const char *label);
/* Dotted keyboard-focus rectangle. */
void xd_theme_draw_focus_rect(const xd_theme *t, cairo_t *cr, xd_rect r);
/* Static text in a dialog, optionally word-wrapped from the top. */
void xd_theme_draw_label(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text, bool wrap,
                         bool bold, bool disabled);
/* Property-sheet tab. The selected tab is drawn 2px taller and joins the
 * panel below it. state: XD_STATE_CHECKED (selected), _HOT, _FOCUSED. */
void xd_theme_draw_tab(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                       const char *label);
/* The page area under the tabs. */
void xd_theme_draw_tab_panel(const xd_theme *t, cairo_t *cr, xd_rect r);
/* Colour pages are filled with (white-ish gradient on Luna, ButtonFace on
 * Classic). */
xd_rgba xd_theme_tab_page_color(const xd_theme *t);
/* Etched group box with a caption breaking the top edge. */
void xd_theme_draw_groupbox(const xd_theme *t, cairo_t *cr, xd_rect r, const char *label);
int xd_theme_scrollbar_width(const xd_theme *t);
/* Vertical scroll bar: `first`/`page`/`total` in items. */
void xd_theme_draw_vscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, int first, int page,
                              int total);
/* Thumb rectangle of a vertical scroll bar, for hit testing. */
xd_rect xd_theme_vscrollbar_thumb(const xd_theme *t, xd_rect r, int first, int page, int total);

/* Colours a widget toolkit needs to match the active style (used to
 * generate the GTK3 theme). Classic fills the 3D colours from the scheme;
 * the flat styles fill the gradient/border colours. */
typedef struct xd_toolkit_colors {
    bool classic;
    xd_rgba bg, fg, base, text, disabled, selected_bg, selected_fg;
    xd_rgba tooltip_bg, tooltip_fg;
    xd_rgba menu_bg, menu_fg, menu_border, menu_hot, menu_hot_fg, menubar_bg;
    xd_rgba title_bg, title_bg2, title_fg;
    /* Classic 3D edges. */
    xd_rgba face, hilight, light, shadow, dkshadow, frame, scrollbar;
    /* Flat styles (Luna, Whistler). */
    xd_rgba field_border, button_top, button_bottom, button_border, button_hot, button_focus;
    xd_rgba check_border, check_mark, thumb_top, thumb_bottom, track, tab_border, tab_hot;
} xd_toolkit_colors;
void xd_theme_toolkit_colors(const xd_theme *t, xd_toolkit_colors *out);

/* GTK3 CSS for the whole toolkit in this theme (scale is ignored: GTK
 * scales itself). */
void xd_theme_gtk_css(const xd_theme *t, xd_strbuf *out);

/* Images the GTK CSS refers to, by path relative to gtk.css. */
typedef enum xd_gtk_asset {
    XD_GTK_CHECK,
    XD_GTK_CHECK_DISABLED,
    XD_GTK_CHECK_HOT,
    XD_GTK_RADIO,
    XD_GTK_RADIO_DISABLED,
    XD_GTK_RADIO_HOT,
    XD_GTK_ASSET_COUNT,
} xd_gtk_asset;
extern const char *const xd_gtk_assets[XD_GTK_ASSET_COUNT];
/* SVG contents of one of those images in this theme's colours. */
void xd_theme_gtk_asset(const xd_theme *t, xd_gtk_asset which, xd_strbuf *out);

/* Whistler's light-to-dark caption mosaic, stepping from the caption's light
 * blue to its dark blue at x within band (also used by the XP Start menu
 * header). */
void xd_theme_draw_whistler_mosaic(const xd_theme *t, cairo_t *cr, xd_rect band, int x);

/* Product banner across the top of a dialog (Shut Down). */
void xd_theme_draw_dialog_banner(const xd_theme *t, cairo_t *cr, xd_rect r, const char *product,
                                 const char *edition);

#endif
