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
#include "scale.h"

typedef enum xd_visual_style {
    XD_STYLE_CLASSIC,
    XD_STYLE_LUNA,
    XD_STYLE_WHISTLER,
    XD_STYLE_AERO, /* Windows 7: glass frames */
    XD_STYLE_COUNT
} xd_visual_style;

const char *xd_visual_style_name(xd_visual_style s);
[[nodiscard]] bool xd_visual_style_from_name(xd_str name, xd_visual_style *out);

typedef struct xd_theme {
    xd_visual_style style;
    xd_scheme scheme;
    double scale; /* device pixels per logical pixel, e.g. 1.0, 1.25, 2.0 */
    /* The compositor blurs behind translucent glass (xd-compositor-fx on a
     * GPU, which sets XD_GLASS=1 for the session). Without it Aero is drawn
     * opaque, as Windows 7 Basic is without desktop composition. */
    bool glass;
    /* A theme package (pkg.h) drawing what it styles, over the style;
     * xd_theme_init() attaches the active one. Its images are scaled with
     * `image_filter`, whole-number scales exactly when `integer_scaling`. */
    const struct xd_pkg *pkg;
    xd_image_filter image_filter;
    bool integer_scaling;
    /* Whose menus the menu functions draw (xd_theme_init: ordinary ones). */
    enum xd_menu_look : uint8_t {
        XD_MENU_LOOK_NORMAL,
        XD_MENU_LOOK_START,         /* the XP and 7 Start menus' cascades */
        XD_MENU_LOOK_CLASSIC_START, /* the Windows 2000 Start menu and its cascades */
    } menu_look;
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
    XD_STATE_VERTICAL = 1u << 7, /* a task button on a side taskbar */
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
    /* The window's icon, 16 logical px in device px; nullptr while there's
     * none, for the style's generic one. */
    cairo_surface_t *icon;
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
/* A radio button and its label, lined up with check boxes. state:
 * XD_STATE_CHECKED, _HOT, _PRESSED, _FOCUSED, _DISABLED. */
void xd_theme_draw_radio(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                         const char *label);

/* The screen edge a taskbar sits on. */
typedef enum xd_edge : uint8_t { XD_EDGE_BOTTOM, XD_EDGE_TOP, XD_EDGE_LEFT, XD_EDGE_RIGHT } xd_edge;

/* One row's height (also a side taskbar's start button height). */
int xd_theme_taskbar_height(const xd_theme *t);
/* Whether task buttons fill the taskbar's row: a theme package's do (their
 * images carry their own margins, transparent in Luna's), the built-in
 * styles draw theirs inset. */
bool xd_theme_task_buttons_fill_row(const xd_theme *t);
/* The background of a taskbar on `edge`, of any size: its light rim
 * faces the desktop. */
void xd_theme_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge);
int xd_theme_start_button_width(const xd_theme *t, cairo_t *cr);
void xd_theme_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
/* icon: the window's 16px (logical) icon, or nullptr for a generic one. */
void xd_theme_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label, cairo_surface_t *icon);
/* Windows 7's taskbar button for a program (Aero), pinned or running.
 * Other styles draw an ordinary task button. */
typedef struct xd_app_button {
    cairo_surface_t *icon; /* at the size to show it; nullptr: none */
    const char *label;     /* nullptr: the icon alone, centred */
    int windows;           /* 0: pinned, not running; 2 and more: stacked edges */
    double glow_x;         /* hovered: the pointer's x (device px); else < 0 */
    xd_rgba tint;          /* the icon's main colour, for the hover glow */
    /* A program's progress (ITaskbarList3's), filling the button. */
    enum xd_taskbar_progress : uint8_t {
        XD_PROGRESS_NONE,
        XD_PROGRESS_NORMAL,        /* green */
        XD_PROGRESS_PAUSED,        /* yellow */
        XD_PROGRESS_ERROR,         /* red */
        XD_PROGRESS_INDETERMINATE, /* a green pulse sweeping across */
    } progress;
    double fraction; /* how far, 0..1; indeterminate: where the pulse is */
} xd_app_button;
/* The progress fill over a program's button `r`: from the theme package
 * (TaskBand2's progress bars), else Windows 7's colours. */
void xd_theme_draw_taskbar_progress(const xd_theme *t, cairo_t *cr, xd_rect r,
                                    enum xd_taskbar_progress kind, double fraction);
void xd_theme_draw_app_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                              const xd_app_button *b);
/* Alt+Tab's window (device px): `count` windows' icons (32 logical px,
 * nullptr for the generic one) in rows of up to seven, the `selected` one
 * framed, and its title: in a sunken box below them, as Windows 2000 and
 * XP draw it in every style, or above them from a Vista or 7 package's
 * AltTab class. Its size for `count` windows: */
void xd_theme_switcher_size(const xd_theme *t, size_t count, int *w, int *h);
void xd_theme_draw_switcher(const xd_theme *t, cairo_t *cr, xd_rect r,
                            cairo_surface_t *const *icons, size_t count, size_t selected,
                            const char *title);
/* Windows 7's taskbar thumbnails (Aero's taskbar only; device px): the
 * glass panel above a button, one window's tile in it (lit when
 * XD_STATE_HOT, deeper when _PRESSED), its title, and the red close
 * button in a hovered tile's corner (state: _HOT, _PRESSED). */
void xd_theme_draw_thumb_panel(const xd_theme *t, cairo_t *cr, xd_rect r);
void xd_theme_draw_thumb_tile(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
void xd_theme_draw_thumb_title(const xd_theme *t, cairo_t *cr, xd_rect r, const char *title);
void xd_theme_draw_thumb_close(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);
/* Windows 7's jump lists (Aero's taskbar; device px): a white panel with
 * the bottom band from band_y down (its program, Pin and Close items),
 * grey section headers, items (the caller draws the icon; state:
 * XD_STATE_HOT, _DISABLED), and the pin button at a hovered item's right
 * end (state: _HOT, _PRESSED; pinned: the item is pinned). */
void xd_theme_draw_jumplist_frame(const xd_theme *t, cairo_t *cr, xd_rect r, int band_y);
void xd_theme_draw_jumplist_header(const xd_theme *t, cairo_t *cr, xd_rect r, const char *label);
void xd_theme_draw_jumplist_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                 const char *label, int text_x);
void xd_theme_draw_jumplist_pin(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                bool pinned);
/* The pushpin alone (a band item's icon): upright when pinned. */
void xd_theme_draw_pushpin(const xd_theme *t, cairo_t *cr, xd_rect r, bool pinned);
/* Taskbar toolbar grip (the ridge you drag a toolbar by), vertical. */
int xd_theme_toolbar_grip_width(const xd_theme *t);
void xd_theme_draw_toolbar_grip(const xd_theme *t, cairo_t *cr, xd_rect r);
/* Flat toolbar button (Quick Launch): only its state is drawn; the caller
 * paints the icon. state: XD_STATE_HOT, _PRESSED. */
void xd_theme_draw_toolbar_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state);

/* Notification area well at the right end of the taskbar: the clock is
 * drawn right-aligned inside the margins; tray icons go to its left. */
void xd_theme_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock);

/* ---- the taskbar from theme packages (style_pkg.c; device px) ----
 * Each is drawn from the package where it styles the part, else as the
 * style draws it (or, where they return false, as the caller does). */

/* The well of a taskbar on `edge` (xd_theme_draw_tray: a bottom one's). */
void xd_theme_draw_tray_on(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock,
                           xd_edge edge);
/* The clock's colour, where the caller draws it (a side taskbar's). */
xd_rgba xd_theme_clock_color(const xd_theme *t);
/* An unlocked taskbar's sizing bar along `r`'s edge facing the desktop:
 * XP's packages have one; the built-in styles draw none. */
void xd_theme_draw_taskbar_sizing_bar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge);
/* XP's button for `count` windows of one program, grouped: the count
 * apart from the name where the package styles it, else "3 Firefox". */
void xd_theme_draw_task_group(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                              int count, const char *label, cairo_surface_t *icon);
/* The chevron folding the tray's quiet icons (open: they show). state:
 * _HOT, _PRESSED. False: the package has none, the caller draws its own. */
bool xd_theme_draw_tray_chevron(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                bool open, bool vertical);
/* Windows 7's Show desktop button at the end of the taskbar; false as
 * above. */
bool xd_theme_draw_show_desktop(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                bool vertical);
/* A newly installed program's highlight in a Start menu (XP's
 * NewAppButton); false: the caller fills its own colour. */
bool xd_theme_draw_new_program(const xd_theme *t, cairo_t *cr, xd_rect r);

/* ---- the XP and 7 Start menus from theme packages (style_pkg.c) ----
 * XP's StartPanel parts, and 7's StartPanelPriv (StartPanel where it has
 * none), by what they draw. */
typedef enum xd_start_part : uint8_t {
    XD_START_USER_PANE,           /* XP's header: the user's picture and name */
    XD_START_USER_PICTURE,        /* the frame around the picture */
    XD_START_PROG_LIST,           /* the program list (left) */
    XD_START_PROG_SEPARATOR,
    XD_START_PLACES_LIST,         /* the places (right) */
    XD_START_PLACES_SEPARATOR,
    XD_START_MORE_PROGRAMS,       /* "All Programs" */
    XD_START_MORE_PROGRAMS_ARROW, /* its arrow (_HOT) */
    XD_START_MORE_PROGRAMS_BACK,  /* 7's "Back" arrow (_HOT) */
    XD_START_LOGOFF,              /* XP's footer */
    XD_START_LOGOFF_BUTTONS,      /* XP: Log Off's icon, then Turn Off's (_HOT) */
    XD_START_PROG_ITEM,           /* 7: a program's highlight (_HOT, _PRESSED) */
    XD_START_PROG_ITEM_LEFT,      /* 7: ...a program with a jump list: its name */
    XD_START_PROG_ITEM_SPLIT,     /* 7: ...and its ▸ */
    XD_START_PLACES_ITEM,         /* 7: a place's highlight (_HOT, _PRESSED) */
    XD_START_SHUTDOWN,            /* 7: Shut down (_HOT, _PRESSED) */
    XD_START_SHUTDOWN_ARROW,      /* 7: its ▸ */
    XD_START_PART_COUNT,
} xd_start_part;
/* Draws the part into r: false when the package doesn't style it. */
bool xd_theme_draw_start_part(const xd_theme *t, cairo_t *cr, xd_start_part part, xd_rect r,
                              uint8_t state);
/* XP's footer icons: LogoffButtons is a strip of square icons, Log Off's
 * key next to last and Turn Off's power button last. */
bool xd_theme_draw_start_logoff_icon(const xd_theme *t, cairo_t *cr, xd_rect r, bool power,
                                     uint8_t state);
/* The part's image size at the theme's scale (true-size parts). */
bool xd_theme_start_part_size(const xd_theme *t, xd_start_part part, uint8_t state, int *w,
                              int *h);
/* A part's colours (TextColor, CaptionText: a pinned program's second
 * line, TextShadowColor) and font, from the package. */
typedef enum xd_start_color : uint8_t {
    XD_START_TEXT,
    XD_START_SUBTEXT,
    XD_START_SHADOW,
} xd_start_color;
bool xd_theme_start_part_color(const xd_theme *t, xd_start_part part, uint8_t state,
                               xd_start_color which, xd_rgba *out);
bool xd_theme_start_part_font(const xd_theme *t, xd_start_part part, xd_font_spec *out);
/* Search results' group headings (7's StartMenu::ListView). */
bool xd_theme_start_heading_color(const xd_theme *t, xd_rgba *out);

/* Windows 7's glass colours, in Window Color and Appearance's order, as
 * 0xAARRGGBB (the alpha is the swatch's colour intensity). */
typedef struct xd_glass_swatch {
    const char *name;
    uint32_t argb;
} xd_glass_swatch;
enum { XD_GLASS_SWATCH_COUNT = 16 };
extern const xd_glass_swatch xd_glass_swatches[XD_GLASS_SWATCH_COUNT];

/* Aero's glass in the scheme's glass colour: translucent where the
 * compositor blurs behind it and transparency is on, else opaque. */
typedef struct xd_glass_colors {
    bool translucent;
    xd_rgba frame, frame_inactive; /* window frames */
    xd_rgba taskbar;
    xd_rgba menu, menu_frame; /* the Windows 7 Start menu's right column, its frame */
} xd_glass_colors;
void xd_theme_glass_colors(const xd_theme *t, xd_glass_colors *out);

/* Windows XP style's Start menu colours (0xRRGGBB) in the theme's colour
 * variant: header and footer gradients, frame, the program list (left),
 * the places column (right), the hot item, and the accent line. */
typedef struct xd_luna_start_colors {
    uint32_t head_a, head_b, foot_a, foot_b, frame;
    uint32_t left_bg, left_text, left_sub, left_sep;
    uint32_t right_bg, right_text, right_dim, right_sep, column_edge;
    uint32_t hot;
    bool accent; /* XP's orange line under the header */
} xd_luna_start_colors;
void xd_theme_luna_start_colors(const xd_theme *t, xd_luna_start_colors *out);
/* Space (device px) between the well's edges and its contents. */
void xd_theme_tray_margins(const xd_theme *t, int *left, int *right);

/* The notification area's own icons (trayicons.c), drawn into r. */
typedef enum xd_tray_icon : uint8_t {
    XD_TRAY_ICON_VOLUME,    /* level: 0-3 sound waves */
    XD_TRAY_ICON_WIRED,
    XD_TRAY_ICON_WIRELESS,  /* level: 0-5 signal bars */
    XD_TRAY_ICON_BATTERY,   /* level: percent */
    XD_TRAY_ICON_BLUETOOTH,
} xd_tray_icon;
enum {
    XD_TRAY_ICON_OFF = 1u << 0,      /* muted, disconnected, Bluetooth off */
    XD_TRAY_ICON_CHARGING = 1u << 1,
};
void xd_theme_draw_tray_icon(const xd_theme *t, cairo_t *cr, xd_rect r, xd_tray_icon icon,
                             int level, uint8_t flags);

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
    const char *underline; /* into the label: its access key, underlined; may be nullptr */
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

/* ---- more controls (controls.c; device px) ----
 * Each is drawn from the theme package where it styles the control, else
 * as the style draws it. */

/* Horizontal scroll bar, and its thumb: the vertical one along x. */
void xd_theme_draw_hscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, int first, int page,
                              int total);
xd_rect xd_theme_hscrollbar_thumb(const xd_theme *t, xd_rect r, int first, int page, int total);
/* The size box where two scroll bars meet: a resize grip in its corner. */
void xd_theme_draw_sizebox(const xd_theme *t, cairo_t *cr, xd_rect r);
/* Progress bar, `fraction` (0..1) done. */
void xd_theme_draw_progress(const xd_theme *t, cairo_t *cr, xd_rect r, double fraction);
/* Slider (trackbar): the channel, and the thumb pointing down at its
 * ticks. state: XD_STATE_HOT, _PRESSED, _FOCUSED, _DISABLED. */
void xd_theme_draw_trackbar(const xd_theme *t, cairo_t *cr, xd_rect channel, xd_rect thumb,
                            uint8_t state);
xd_rgba xd_theme_trackbar_tics_color(const xd_theme *t, bool disabled);
/* One button of a spin (up-down) control. state: _HOT, _PRESSED, _DISABLED. */
void xd_theme_draw_spin_button(const xd_theme *t, cairo_t *cr, xd_rect r, bool up,
                               uint8_t state);
/* A list view's column header. state: _HOT, _PRESSED. */
void xd_theme_draw_header_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label);
/* A tree view's expand box: a plus when closed, a minus when open. */
void xd_theme_draw_tree_glyph(const xd_theme *t, cairo_t *cr, xd_rect r, bool open);
/* A status bar with one pane of `text` (nullptr: none) and, with `grip`,
 * the size grip at its right end. */
void xd_theme_draw_statusbar(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text,
                             bool grip);
/* A tooltip filling r, its text 4 logical px in from the sides. */
void xd_theme_draw_tooltip(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text);
/* A program's toolbar band (rebar), with the gripper at its left end. */
void xd_theme_draw_rebar(const xd_theme *t, cairo_t *cr, xd_rect r, bool gripper);
/* A button on a program's toolbar; the caller draws its icon. state: _HOT,
 * _PRESSED, _CHECKED, _DISABLED. */
void xd_theme_draw_tool_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label);
/* A menu bar, and one of its items. state: _HOT, _PRESSED, _DISABLED. */
void xd_theme_draw_menubar(const xd_theme *t, cairo_t *cr, xd_rect r, bool active);
void xd_theme_draw_menubar_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                const char *label);

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
    XD_GTK_MENU_ARROW, /* a submenu's arrow */
    XD_GTK_MENU_ARROW_HOT,
    XD_GTK_MENU_ARROW_DISABLED,
    XD_GTK_SPIN_UP, /* spin buttons' arrows */
    XD_GTK_SPIN_UP_DISABLED,
    XD_GTK_SPIN_DOWN,
    XD_GTK_SPIN_DOWN_DISABLED,
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
