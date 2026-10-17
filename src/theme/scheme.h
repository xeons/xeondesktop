/* SPDX-License-Identifier: MIT
 * Color schemes and system metrics.
 *
 * Color names match the Windows "Control Panel\Colors" registry values so
 * classic scheme files read the same way. Metrics are stored in logical
 * units (1 unit = 1 pixel at 96 DPI) and scaled at draw time.
 */
#ifndef XD_THEME_SCHEME_H
#define XD_THEME_SCHEME_H

#include "../core/str.h"

typedef struct xd_rgb {
    uint8_t r, g, b;
} xd_rgb;

#define XD_SYSCOLORS(X)                                                        \
    X(SCROLLBAR, "Scrollbar")                                                  \
    X(BACKGROUND, "Background")                                                \
    X(ACTIVE_TITLE, "ActiveTitle")                                             \
    X(INACTIVE_TITLE, "InactiveTitle")                                         \
    X(MENU, "Menu")                                                            \
    X(WINDOW, "Window")                                                        \
    X(WINDOW_FRAME, "WindowFrame")                                             \
    X(MENU_TEXT, "MenuText")                                                   \
    X(WINDOW_TEXT, "WindowText")                                               \
    X(TITLE_TEXT, "TitleText")                                                 \
    X(ACTIVE_BORDER, "ActiveBorder")                                           \
    X(INACTIVE_BORDER, "InactiveBorder")                                       \
    X(APP_WORKSPACE, "AppWorkspace")                                           \
    X(HILIGHT, "Hilight")                                                      \
    X(HILIGHT_TEXT, "HilightText")                                             \
    X(BUTTON_FACE, "ButtonFace")                                               \
    X(BUTTON_SHADOW, "ButtonShadow")                                           \
    X(GRAY_TEXT, "GrayText")                                                   \
    X(BUTTON_TEXT, "ButtonText")                                               \
    X(INACTIVE_TITLE_TEXT, "InactiveTitleText")                                \
    X(BUTTON_HILIGHT, "ButtonHilight")                                         \
    X(BUTTON_DK_SHADOW, "ButtonDkShadow")                                      \
    X(BUTTON_LIGHT, "ButtonLight")                                             \
    X(INFO_TEXT, "InfoText")                                                   \
    X(INFO_WINDOW, "InfoWindow")                                               \
    X(HOT_TRACKING, "HotTrackingColor")                                        \
    X(GRADIENT_ACTIVE_TITLE, "GradientActiveTitle")                            \
    X(GRADIENT_INACTIVE_TITLE, "GradientInactiveTitle")                        \
    X(MENU_HILIGHT, "MenuHilight")                                             \
    X(MENU_BAR, "MenuBar")

typedef enum xd_syscolor {
#define XD_X(id, name) XD_COLOR_##id,
    XD_SYSCOLORS(XD_X)
#undef XD_X
    XD_COLOR_COUNT
} xd_syscolor;

const char *xd_syscolor_name(xd_syscolor c);
[[nodiscard]] bool xd_syscolor_from_name(xd_str name, xd_syscolor *out);

#define XD_METRICS(X)                                                          \
    X(BORDER_WIDTH, "BorderWidth", 1)                                          \
    X(CAPTION_HEIGHT, "CaptionHeight", 18)                                     \
    X(CAPTION_BUTTON_WIDTH, "CaptionButtonWidth", 16)                          \
    X(MENU_HEIGHT, "MenuHeight", 18)                                           \
    X(SCROLL_WIDTH, "ScrollWidth", 16)                                         \
    X(ICON_SPACING, "IconSpacing", 75)                                         \
    X(TASKBAR_HEIGHT, "TaskbarHeight", 28)

typedef enum xd_metric {
#define XD_X(id, name, def) XD_METRIC_##id,
    XD_METRICS(XD_X)
#undef XD_X
    XD_METRIC_COUNT
} xd_metric;

/* Windows' fonts, each followed by the closest freely packaged font
 * (measured against Tahoma at 8pt; compared by eye for the others), so
 * the real font is used if it's installed and nothing else is needed:
 *   Tahoma: DejaVu Sans Condensed, Tahoma's width and shapes (both come
 *     from Verdana-like designs);
 *   Trebuchet MS (Luna captions): Source Sans 3, a narrow humanist sans;
 *   Franklin Gothic (XP's "start"): Roboto Condensed. */
#define XD_FONT_FAMILY_TAHOMA "Tahoma, DejaVu Sans Condensed, DejaVu Sans, sans-serif"
#define XD_FONT_FAMILY_TREBUCHET                                                               \
    "Trebuchet MS, Source Sans 3, DejaVu Sans Condensed, DejaVu Sans, sans-serif"
/* Segoe UI (Windows 7): Open Sans, by the same designer and closest in
 * shape. */
#define XD_FONT_FAMILY_SEGOE "Segoe UI, Open Sans, Noto Sans, DejaVu Sans, sans-serif"
#define XD_FONT_FAMILY_FRANKLIN                                                                \
    "Franklin Gothic Medium, Roboto Condensed, DejaVu Sans Condensed, DejaVu Sans, sans-serif"

typedef struct xd_font_spec {
    char family[128];
    double size_pt;
    bool bold;
    bool italic;
} xd_font_spec;

typedef enum xd_font_role {
    XD_FONT_CAPTION,
    XD_FONT_MENU,
    XD_FONT_MESSAGE,
    XD_FONT_ICON,
    XD_FONT_TOOLTIP,
    XD_FONT_COUNT
} xd_font_role;

/* "Smooth edges of screen fonts". Zero is the default so structs built
 * without it get standard antialiasing. */
typedef enum xd_font_smoothing : uint8_t {
    XD_SMOOTH_STANDARD = 0,
    XD_SMOOTH_NONE,
    XD_SMOOTH_CLEARTYPE, /* subpixel, RGB order */
} xd_font_smoothing;

/* Windows XP style's colour variants ([Scheme] LunaColor=, named as XP's
 * msstyles names them). A scheme with one is listed under Windows XP
 * style rather than Windows Classic; its colours are XP's system colours
 * for that variant, and the Luna artwork is recoloured to match. */
typedef enum xd_luna_color : uint8_t {
    XD_LUNA_NONE = 0, /* a classic scheme */
    XD_LUNA_BLUE,     /* "NormalColor": Default (blue) */
    XD_LUNA_OLIVE,    /* "HomeStead": Olive Green */
    XD_LUNA_SILVER,   /* "Metallic": Silver */
} xd_luna_color;

typedef struct xd_scheme {
    char name[64];
    xd_rgb colors[XD_COLOR_COUNT];
    int metrics[XD_METRIC_COUNT];
    xd_font_spec fonts[XD_FONT_COUNT];
    bool gradient_captions;
    bool flat_menus;
    /* Accessibility mode: no dither patterns or decorative gradients that
     * would put text on mixed backgrounds. */
    bool high_contrast;
    xd_font_smoothing font_smoothing;
    xd_luna_color luna_color;
} xd_scheme;

/* The built-in "Windows Standard" scheme, used as the base for every
 * scheme file so a file only needs to list what it changes. */
void xd_scheme_init_default(xd_scheme *scheme);

/* Loads a scheme file on top of the defaults. On failure *err describes
 * the problem and the scheme is left at defaults. */
[[nodiscard]] bool xd_scheme_parse(xd_str text, xd_scheme *scheme, xd_strbuf *err);
[[nodiscard]] bool xd_scheme_load(const char *path, xd_scheme *scheme, xd_strbuf *err);

#endif
