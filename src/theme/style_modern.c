/* SPDX-License-Identifier: MIT
 * Rounded, gradient-based visual style: Luna (XP). Driven by a palette,
 * so another style in this family is just another palette. (Whistler has
 * its own flat renderer in style_whistler.c.) All artwork is drawn procedurally; no
 * bitmaps from any other product are used.
 */
#include "style.h"

typedef struct stops {
    double at[5];
    uint32_t rgb[5];
    size_t n;
} stops;

typedef struct modern_palette {
    const char *name;
    int caption_h;    /* logical px, includes the top frame edge */
    int border;       /* logical px, sides and bottom */
    double corner;    /* logical radius of the top window corners */
    stops caption_active, caption_inactive;
    uint32_t frame_active, frame_inactive, frame_outline;
    uint32_t title_active, title_inactive, title_shadow;
    xd_font_spec title_font;

    uint32_t dialog;
    stops button_face;
    uint32_t button_outline, button_hot, button_focus;

    stops capbtn, capbtn_close;
    uint32_t capbtn_border, capbtn_glyph;

    uint32_t check_border, check_mark;

    int taskbar_h;
    stops taskbar, tray, start, task, task_active;
    uint32_t tray_edge, taskbar_text, start_text;
    xd_font_spec start_font;
    double start_radius; /* rounded right side; 0 for a pill-less button */
    const char *start_label;

    uint32_t menu_bg, menu_border, menu_text, menu_hilight, menu_hilight_text,
        menu_hilight_border, menu_disabled, menu_separator, banner_top, banner_bottom;
} modern_palette;

static const modern_palette *pal(const xd_theme *theme);

/* Olive Green, Silver and Energy Blue: the dialog, menu and selection colours are the
 * variant's system colours, which its scheme file carries; Blue keeps the
 * palette (the same values, as measured). */
static bool variant(const xd_theme *theme) {
    return theme->scheme.luna_color == XD_LUNA_OLIVE ||
           theme->scheme.luna_color == XD_LUNA_SILVER || theme->scheme.luna_color == XD_LUNA_ENERGY;
}

/* A palette colour (Blue's) in the theme's colour variant. */
static xd_rgba vary(const xd_theme *theme, uint32_t rgb) {
    return xd_rgba_hex(xd_luna_variant_rgb(theme, true, rgb));
}

static xd_rgba modern_dialog_color(const xd_theme *theme) {
    return variant(theme) ? xd_sys(theme, XD_COLOR_BUTTON_FACE) : xd_rgba_hex(pal(theme)->dialog);
}

/* ---- palettes ----------------------------------------------------------- */

#define STOPS3(a, oa, b, c) {.at = {0, oa, 1}, .rgb = {a, b, c}, .n = 3}
#define STOPS4(a, oa, b, ob, c, d) {.at = {0, oa, ob, 1}, .rgb = {a, b, c, d}, .n = 4}

static const modern_palette luna = {
    .name = "Luna",
    .caption_h = 30,
    .border = 4,
    .corner = 7,
    .caption_active = STOPS4(0x3D95FF, 0.1, 0x0A64F0, 0.55, 0x0055E5, 0x0047C8),
    .caption_inactive = STOPS4(0xA9C2F0, 0.1, 0x8FAEE8, 0.55, 0x7D9EE0, 0x7393D6),
    .frame_active = 0x0855DD,
    .frame_inactive = 0x7B9BE0,
    .frame_outline = 0x00138C,
    .title_active = 0xFFFFFF,
    .title_inactive = 0xD8E4F8,
    .title_shadow = 0x0A1883,
    .title_font = {.family = XD_FONT_FAMILY_TREBUCHET, .size_pt = 10, .bold = true},

    .dialog = 0xECE9D8,
    .button_face = STOPS3(0xFFFFFF, 0.85, 0xECEBE6, 0xD6D0C5),
    .button_outline = 0x003C74,
    .button_hot = 0xF8B330,
    .button_focus = 0x7DA2CE,

    .capbtn = STOPS3(0x6F9DF9, 0.5, 0x2E6BEB, 0x1F55D6),
    .capbtn_close = STOPS3(0xF0937A, 0.5, 0xE0532A, 0xC63A12),
    .capbtn_border = 0xFFFFFF,
    .capbtn_glyph = 0xFFFFFF,

    .check_border = 0x1C5180,
    .check_mark = 0x21A121,

    .taskbar_h = 30,
    .taskbar = STOPS4(0x3A80F3, 0.08, 0x5EA0F8, 0.2, 0x2E6FE5, 0x1F4FC2),
    .tray = STOPS3(0x1EA4F4, 0.2, 0x0F8DE6, 0x0C6CCE),
    .tray_edge = 0x1042AF,
    .start = STOPS4(0x6BC56B, 0.15, 0x3C9B3C, 0.8, 0x2F8A2F, 0x206F20),
    .task = STOPS3(0x5C97F7, 0.5, 0x3C81F3, 0x3274E8),
    .task_active = STOPS3(0x1A49AF, 0.5, 0x1E52B7, 0x2459C4),
    .taskbar_text = 0xFFFFFF,
    .start_text = 0xFFFFFF,
    .start_font = {.family = XD_FONT_FAMILY_FRANKLIN, .size_pt = 13, .bold = true,
                   .italic = true},
    .start_radius = 12,
    .start_label = "start",
    .menu_bg = 0xFFFFFF,
    .menu_border = 0xACA899,
    .menu_text = 0x000000,
    .menu_hilight = 0x316AC5,
    .menu_hilight_text = 0xFFFFFF,
    .menu_disabled = 0xACA899,
    .menu_separator = 0xACA899,
    .banner_top = 0x0A246A,
    .banner_bottom = 0x3A6EE8,
};

static const modern_palette *pal(const xd_theme *theme) {
    (void)theme;
    return &luna;
}

static void modern_menu_palette(const xd_theme *theme, xd_menu_palette *menu_palette) {
    const modern_palette *modern = pal(theme);
    xd_rgba none = xd_rgba_alpha(xd_rgba_hex(0), 0);
    *menu_palette = (xd_menu_palette){
        .flat = true,
        .background = xd_rgba_hex(modern->menu_bg),
        .border = xd_rgba_hex(modern->menu_border),
        .text = xd_rgba_hex(modern->menu_text),
        .hilight = xd_rgba_hex(modern->menu_hilight),
        .hilight_text = xd_rgba_hex(modern->menu_hilight_text),
        .hilight_border =
            modern->menu_hilight_border != 0 ? xd_rgba_hex(modern->menu_hilight_border) : none,
        .disabled_text = xd_rgba_hex(modern->menu_disabled),
        .disabled_emboss = none,
        .separator_dark = xd_rgba_hex(modern->menu_separator),
        .separator_light = none,
        .banner_top = vary(theme, modern->banner_top),
        .banner_bottom = vary(theme, modern->banner_bottom),
        .banner_text = xd_rgba_hex(0xFFFFFF),
    };
    if (variant(theme)) {
        menu_palette->background = xd_sys(theme, XD_COLOR_MENU);
        menu_palette->border = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
        menu_palette->text = xd_sys(theme, XD_COLOR_MENU_TEXT);
        menu_palette->hilight = xd_sys(theme, XD_COLOR_MENU_HILIGHT);
        menu_palette->hilight_text = xd_sys(theme, XD_COLOR_HILIGHT_TEXT);
        menu_palette->disabled_text = xd_sys(theme, XD_COLOR_GRAY_TEXT);
        menu_palette->separator_dark = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
    }
}

static void modern_toolkit_colors(const xd_theme *theme, xd_toolkit_colors *toolkit_colors) {
    const modern_palette *modern = pal(theme);
    xd_rgba white = xd_rgba_hex(0xFFFFFF);
    *toolkit_colors = (xd_toolkit_colors){
        .bg = xd_rgba_hex(modern->dialog),
        .fg = xd_rgba_hex(0x000000),
        .base = white,
        .text = xd_rgba_hex(0x000000),
        .disabled = xd_rgba_hex(0xACA899),
        .selected_bg = xd_rgba_hex(0x316AC5),
        .selected_fg = white,
        .tooltip_bg = xd_rgba_hex(0xFFFFE1),
        .tooltip_fg = xd_rgba_hex(0x000000),
        .menu_bg = xd_rgba_hex(modern->menu_bg),
        .menu_fg = xd_rgba_hex(modern->menu_text),
        .menu_border = xd_rgba_hex(modern->menu_border),
        .menu_hot = xd_rgba_hex(modern->menu_hilight),
        .menu_hot_fg = xd_rgba_hex(modern->menu_hilight_text),
        .menubar_bg = xd_rgba_hex(modern->dialog),
        .title_bg = vary(theme, modern->caption_active.rgb[1]),
        .title_bg2 = vary(theme, modern->caption_active.rgb[modern->caption_active.n - 1]),
        .title_fg = xd_rgba_hex(modern->title_active),
        .face = xd_rgba_hex(modern->dialog),
        .field_border = xd_rgba_hex(0x7F9DB9),
        .button_top = xd_rgba_hex(modern->button_face.rgb[0]),
        .button_bottom = xd_rgba_hex(modern->button_face.rgb[modern->button_face.n - 1]),
        .button_border = xd_rgba_hex(modern->button_outline),
        .button_hot = xd_rgba_hex(modern->button_hot),
        .button_focus = xd_rgba_hex(modern->button_focus),
        .check_border = xd_rgba_hex(modern->check_border),
        .check_mark = xd_rgba_hex(modern->check_mark),
        .thumb_top = vary(theme, 0xC8D6FB),
        .thumb_bottom = vary(theme, 0xB0C8F8),
        .track = xd_rgba_hex(0xF3F1EC),
        .tab_border = xd_rgba_hex(0x919B9C),
        .tab_hot = xd_rgba_hex(0xFFC83C),
    };
    if (variant(theme)) {
        toolkit_colors->bg = toolkit_colors->face = xd_sys(theme, XD_COLOR_BUTTON_FACE);
        toolkit_colors->disabled = xd_sys(theme, XD_COLOR_GRAY_TEXT);
        toolkit_colors->selected_bg = xd_sys(theme, XD_COLOR_HILIGHT);
        toolkit_colors->selected_fg = xd_sys(theme, XD_COLOR_HILIGHT_TEXT);
        toolkit_colors->menu_bg = xd_sys(theme, XD_COLOR_MENU);
        toolkit_colors->menu_fg = xd_sys(theme, XD_COLOR_MENU_TEXT);
        toolkit_colors->menu_border = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
        toolkit_colors->menu_hot = xd_sys(theme, XD_COLOR_MENU_HILIGHT);
        toolkit_colors->menu_hot_fg = xd_sys(theme, XD_COLOR_HILIGHT_TEXT);
        toolkit_colors->menubar_bg = xd_sys(theme, XD_COLOR_MENU_BAR);
        toolkit_colors->title_fg = xd_sys(theme, XD_COLOR_TITLE_TEXT);
    }
}

#define MODERN_OPS(sym, label)                                                 \
    const xd_style_ops sym = {                                                 \
        .name = label,                                                         \
        .frame_insets = xd_luna_frame_insets,                                  \
        .frame_layout = xd_luna_frame_layout,                                  \
        .draw_frame = xd_luna_draw_frame,                                      \
        .dialog_color = modern_dialog_color,                                   \
        .draw_button = xd_luna_draw_button,                                    \
        .draw_checkbox = xd_luna_draw_checkbox,                                \
        .taskbar_height = xd_luna_taskbar_height,                              \
        .draw_taskbar = xd_luna_draw_taskbar,                                  \
        .start_button_width = xd_luna_start_button_width,                      \
        .draw_start_button = xd_luna_draw_start_button,                        \
        .draw_task_button = xd_luna_draw_task_button,                          \
        .draw_tray = xd_luna_draw_tray,                                        \
        .menu_palette = modern_menu_palette,                                   \
        .toolkit_colors = modern_toolkit_colors,                               \
    }

MODERN_OPS(xd_style_luna, "Luna");
