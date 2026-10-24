/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "style.h"

static const xd_style_ops *const styles[XD_STYLE_COUNT] = {
    [XD_STYLE_CLASSIC] = &xd_style_classic,
    [XD_STYLE_LUNA] = &xd_style_luna,
    [XD_STYLE_WHISTLER] = &xd_style_whistler,
    [XD_STYLE_AERO] = &xd_style_aero,
};

static const xd_style_ops *ops(const xd_theme *theme) {
    XD_ASSERT((unsigned)theme->style < XD_STYLE_COUNT);
    return styles[theme->style];
}

const char *xd_visual_style_name(xd_visual_style visual_style) {
    XD_ASSERT((unsigned)visual_style < XD_STYLE_COUNT);
    return styles[visual_style]->name;
}

bool xd_visual_style_from_name(xd_str name, xd_visual_style *out) {
    for (size_t i = 0; i < XD_STYLE_COUNT; i++) {
        if (xd_str_eq_nocase(name, xd_str_from_cstr(styles[i]->name))) {
            *out = (xd_visual_style)i;
            return true;
        }
    }
    return false;
}

void xd_theme_init(xd_theme *theme, xd_visual_style style, const xd_scheme *scheme, double scale) {
    XD_ASSERT((unsigned)style < XD_STYLE_COUNT);
    XD_ASSERT(scale >= 0.5 && scale <= 8.0);
    theme->style = style;
    theme->scheme = *scheme;
    theme->scale = scale;
    const char *glass = getenv("XD_GLASS");
    theme->glass = glass != nullptr && strcmp(glass, "1") == 0;
    theme->menu_look = XD_MENU_LOOK_NORMAL;
    xd_theme_attach_package(theme);
}

xd_insets xd_theme_frame_insets(const xd_theme *theme, const xd_frame *frame) {
    if (xd_pkgstyle_has_frame(theme))
        return xd_pkgstyle_frame_insets(theme, frame);
    return ops(theme)->frame_insets(theme, frame);
}
void xd_theme_frame_layout(const xd_theme *theme, xd_rect outer, const xd_frame *frame,
                           xd_frame_layout *out) {
    *out = (xd_frame_layout){0};
    if (xd_pkgstyle_has_frame(theme))
        xd_pkgstyle_frame_layout(theme, outer, frame, out);
    else
        ops(theme)->frame_layout(theme, outer, frame, out);
}
void xd_theme_draw_frame(const xd_theme *theme, cairo_t *cairo, xd_rect outer,
                         const xd_frame *frame) {
    if (xd_pkgstyle_has_frame(theme))
        xd_pkgstyle_draw_frame(theme, cairo, outer, frame);
    else
        ops(theme)->draw_frame(theme, cairo, outer, frame);
}

bool xd_frame_part_is_button(xd_frame_part part) {
    return part == XD_PART_CLOSE || part == XD_PART_MAXIMIZE || part == XD_PART_MINIMIZE ||
           part == XD_PART_HELP;
}

bool xd_frame_part_is_border(xd_frame_part part) { return part >= XD_PART_TOP; }

static bool rect_contains(xd_rect rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x - rect.x < rect.w && y - rect.y < rect.h;
}

xd_frame_part xd_theme_frame_hit(const xd_theme *theme, xd_rect outer, const xd_frame *frame, int x,
                                 int y) {
    if (!rect_contains(outer, x, y))
        return XD_PART_NONE;

    xd_frame_layout frame_layout;
    xd_theme_frame_layout(theme, outer, frame, &frame_layout);
    for (size_t i = 0; i < frame_layout.button_count; i++) {
        if (rect_contains(frame_layout.button_rects[i], x, y))
            return frame_layout.button_parts[i];
    }

    xd_insets in = xd_theme_frame_insets(theme, frame);
    if (!frame->maximized && !frame->dialog) {
        /* Edges are at least 4 logical px wide even with thin borders, and
         * the corner zones extend along each edge like Windows' do. */
        int edge = XD_MAX(in.left, xd_tpx(theme, 4));
        int corner = xd_tpx(theme, 16);
        bool left = x < outer.x + edge;
        bool right = x >= outer.x + outer.w - edge;
        bool top = y < outer.y + edge;
        bool bottom = y >= outer.y + outer.h - edge;
        bool near_left = x < outer.x + corner;
        bool near_right = x >= outer.x + outer.w - corner;
        bool near_top = y < outer.y + corner;
        bool near_bottom = y >= outer.y + outer.h - corner;

        if ((top && near_left) || (left && near_top))
            return XD_PART_TOP_LEFT;
        if ((top && near_right) || (right && near_top))
            return XD_PART_TOP_RIGHT;
        if ((bottom && near_left) || (left && near_bottom))
            return XD_PART_BOTTOM_LEFT;
        if ((bottom && near_right) || (right && near_bottom))
            return XD_PART_BOTTOM_RIGHT;
        if (top)
            return XD_PART_TOP;
        if (bottom)
            return XD_PART_BOTTOM;
        if (left)
            return XD_PART_LEFT;
        if (right)
            return XD_PART_RIGHT;
    }
    if (rect_contains(frame_layout.icon, x, y))
        return XD_PART_SYSMENU;
    if (y < outer.y + in.top)
        return XD_PART_CAPTION;
    return XD_PART_CLIENT;
}

void xd_layout_add_button(xd_frame_layout *frame_layout, xd_rect rect, xd_frame_part part) {
    XD_ASSERT(frame_layout->button_count < XD_FRAME_MAX_BUTTONS);
    frame_layout->button_rects[frame_layout->button_count] = rect;
    frame_layout->button_parts[frame_layout->button_count] = part;
    frame_layout->button_count++;
}

xd_glyph xd_glyph_for_part(xd_frame_part part, bool maximized) {
    switch (part) {
    case XD_PART_CLOSE:
        return XD_GLYPH_CLOSE;
    case XD_PART_MAXIMIZE:
        return maximized ? XD_GLYPH_RESTORE : XD_GLYPH_MAXIMIZE;
    case XD_PART_MINIMIZE:
        return XD_GLYPH_MINIMIZE;
    case XD_PART_HELP:
        return XD_GLYPH_HELP;
    case XD_PART_NONE:
    case XD_PART_CLIENT:
    case XD_PART_CAPTION:
    case XD_PART_SYSMENU:
    case XD_PART_TOP:
    case XD_PART_BOTTOM:
    case XD_PART_LEFT:
    case XD_PART_RIGHT:
    case XD_PART_TOP_LEFT:
    case XD_PART_TOP_RIGHT:
    case XD_PART_BOTTOM_LEFT:
    case XD_PART_BOTTOM_RIGHT:
        break;
    }
    XD_PANIC("frame part has no glyph");
}
xd_rgba xd_theme_dialog_color(const xd_theme *theme) { return ops(theme)->dialog_color(theme); }
void xd_theme_draw_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                          const char *label) {
    if (xd_pkgstyle_draw_button(theme, cairo, rect, state, label))
        return;
    ops(theme)->draw_button(theme, cairo, rect, state, label);
}
void xd_theme_draw_checkbox(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                            const char *label) {
    if (xd_pkgstyle_draw_checkbox(theme, cairo, rect, state, label))
        return;
    ops(theme)->draw_checkbox(theme, cairo, rect, state, label);
}
int xd_theme_taskbar_height(const xd_theme *theme, bool large_icons) {
    /* Aero's own is Windows 7's with large icons; small ones are XP's
     * height, as 7's "Use small icons" makes it. */
    if (!large_icons && theme->style == XD_STYLE_AERO)
        return xd_tpx(theme, 30);
    int height = xd_pkgstyle_taskbar_height(theme);
    if (height <= 0)
        height = ops(theme)->taskbar_height(theme);
    return large_icons ? XD_MAX(height, xd_tpx(theme, 40)) : height;
}
bool xd_theme_task_buttons_fill_row(const xd_theme *theme) {
    return xd_pkgstyle_taskbar_height(theme) > 0;
}
void xd_theme_draw_taskbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_edge edge) {
    if (xd_pkgstyle_draw_taskbar(theme, cairo, rect, edge))
        return;
    ops(theme)->draw_taskbar(theme, cairo, rect, edge);
}

xd_rect xd_style_rim_line(xd_rect rect, xd_edge edge, int i, int px) {
    switch (edge) {
    case XD_EDGE_TOP:
        return (xd_rect){rect.x, rect.y + rect.h - i - px, rect.w, px};
    case XD_EDGE_LEFT:
        return (xd_rect){rect.x + rect.w - i - px, rect.y, px, rect.h};
    case XD_EDGE_RIGHT:
        return (xd_rect){rect.x + i, rect.y, px, rect.h};
    case XD_EDGE_BOTTOM:
        break;
    }
    return (xd_rect){rect.x, rect.y + i, rect.w, px};
}
int xd_theme_start_button_width(const xd_theme *theme, cairo_t *cairo) {
    int width = xd_pkgstyle_start_button_width(theme);
    return width > 0 ? width : ops(theme)->start_button_width(theme, cairo);
}
void xd_theme_draw_start_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                uint8_t state) {
    if (xd_pkgstyle_draw_start_button(theme, cairo, rect, state))
        return;
    ops(theme)->draw_start_button(theme, cairo, rect, state);
}
void xd_theme_draw_task_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                               const char *label, cairo_surface_t *icon) {
    if (xd_pkgstyle_draw_task_button(theme, cairo, rect, state, label, icon, 1))
        return;
    ops(theme)->draw_task_button(theme, cairo, rect, state, label, icon);
}

/* The unified taskbar in the styles without 7's buttons, from their own:
 * a pinned program that isn't running is a flat toolbar button, as Quick
 * Launch's; a running one is a task button, with its other windows'
 * buttons' edges stacked behind it, or XP's "3 Firefox" when labelled;
 * progress is a strip along its foot. */
static void draw_app_button_tasks(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state, const xd_app_button *app_button) {
    if (app_button->windows == 0) {
        xd_theme_draw_toolbar_button(theme, cairo, rect, state & (XD_STATE_HOT | XD_STATE_PRESSED));
        if (app_button->icon != nullptr) {
            int size = xd_style_task_icon_size(theme, app_button->icon);
            int offset = (state & XD_STATE_PRESSED) ? xd_tpx(theme, 1) : 0;
            xd_style_task_icon(theme, cairo, app_button->icon,
                               rect.x + (rect.w - size) / 2 + offset,
                               rect.y + (rect.h - size) / 2 + offset);
        }
        return;
    }
    xd_rect front = rect;
    if (app_button->label != nullptr && app_button->windows > 1) {
        xd_theme_draw_task_group(theme, cairo, front, state, app_button->windows, app_button->label,
                                 app_button->icon);
    } else {
        int stack = XD_CLAMP(app_button->windows - 1, 0, 2);
        int step = xd_tpx(theme, 3);
        front.w -= stack * step;
        /* The farthest first, each showing only past the one in front. */
        for (int k = stack; k >= 1; k--) {
            xd_rect behind = {front.x + k * step, front.y, front.w, front.h};
            int from = front.x + front.w + (k - 1) * step;
            cairo_save(cairo);
            cairo_rectangle(cairo, from, behind.y, behind.x + behind.w - from, behind.h);
            cairo_clip(cairo);
            xd_theme_draw_task_button(theme, cairo, behind,
                                      (uint8_t)(state & ~(XD_STATE_HOT | XD_STATE_PRESSED)),
                                      nullptr, nullptr);
            cairo_restore(cairo);
        }
        xd_theme_draw_task_button(theme, cairo, front, state, app_button->label, app_button->icon);
    }
    if (app_button->progress != XD_PROGRESS_NONE) {
        int inset = xd_tpx(theme, 4), strip = xd_tpx(theme, 3);
        xd_theme_draw_taskbar_progress(theme, cairo,
                                       (xd_rect){front.x + inset, front.y + front.h - inset - strip,
                                                 front.w - 2 * inset, strip},
                                       app_button->progress, app_button->fraction);
    }
}

void xd_theme_draw_app_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                              const xd_app_button *app_button) {
    if (xd_pkgstyle_draw_app_button(theme, cairo, rect, state, app_button))
        return;
    if (ops(theme)->draw_app_button != nullptr)
        ops(theme)->draw_app_button(theme, cairo, rect, state, app_button);
    else
        draw_app_button_tasks(theme, cairo, rect, state, app_button);
}

int xd_style_task_icon_size(const xd_theme *theme, cairo_surface_t *icon) {
    if (icon == nullptr || cairo_surface_get_type(icon) != CAIRO_SURFACE_TYPE_IMAGE)
        return xd_tpx(theme, 16);
    double sx = 1, sy = 1;
    cairo_surface_get_device_scale(icon, &sx, &sy);
    return (int)(cairo_image_surface_get_height(icon) / sy);
}

void xd_style_task_icon(const xd_theme *theme, cairo_t *cairo, cairo_surface_t *icon, int x,
                        int y) {
    int size = xd_style_task_icon_size(theme, icon);
    if (icon != nullptr) {
        cairo_save(cairo);
        cairo_rectangle(cairo, x, y, size, size);
        cairo_clip(cairo);
        cairo_set_source_surface(cairo, icon, x, y);
        cairo_paint(cairo);
        cairo_restore(cairo);
        return;
    }
    /* A generic window: white with a title bar. */
    xd_fill_rect(cairo, (xd_rect){x, y, size, size}, xd_rgba_hex(0xFFFFFF));
    xd_fill_rect(cairo, (xd_rect){x, y, size, xd_tpx(theme, 4)}, xd_rgba_hex(0x0A246A));
    int pixel = xd_tpx(theme, 1);
    xd_draw_bevel(cairo, (xd_rect){x, y, size, size}, pixel, xd_rgba_hex(0x404040),
                  xd_rgba_hex(0x404040));
}
void xd_theme_draw_tray(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *clock) {
    xd_theme_draw_tray_on(theme, cairo, rect, clock, XD_EDGE_BOTTOM);
}

void xd_theme_draw_tray_on(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *clock,
                           xd_edge edge) {
    if (xd_pkgstyle_draw_tray(theme, cairo, rect, clock, edge))
        return;
    ops(theme)->draw_tray(theme, cairo, rect, clock);
}

xd_rgba xd_theme_clock_color(const xd_theme *theme) {
    xd_rgba color;
    if (xd_pkgstyle_clock_color(theme, &color))
        return color;
    return theme->style == XD_STYLE_CLASSIC ? xd_sys(theme, XD_COLOR_BUTTON_TEXT)
                                            : xd_rgba_hex(0xFFFFFF);
}
void xd_theme_tray_margins(const xd_theme *theme, int *left, int *right) {
    /* What each style's draw_tray leaves around the clock. */
    switch (theme->style) {
    case XD_STYLE_LUNA:
        *left = xd_tpx(theme, 10);
        *right = xd_tpx(theme, 13);
        return;
    case XD_STYLE_WHISTLER:
        *left = *right = xd_tpx(theme, 10);
        return;
    case XD_STYLE_AERO:
        *left = *right = xd_tpx(theme, 8);
        return;
    case XD_STYLE_CLASSIC:
    case XD_STYLE_COUNT:
        break;
    }
    *left = *right = xd_tpx(theme, 1) + xd_tpx(theme, 6);
}

/* ---- menus ----------------------------------------------------------------- */

xd_menu_metrics xd_theme_menu_metrics(const xd_theme *theme) {
    xd_menu_palette menu_palette;
    ops(theme)->menu_palette(theme, &menu_palette);
    return (xd_menu_metrics){
        .border = xd_tpx(theme, 3),
        .item_h = xd_tpx(theme, 22),
        .large_item_h = xd_tpx(theme, 36),
        .icon = xd_tpx(theme, 16),
        .large_icon = xd_tpx(theme, 32),
        .icon_x = xd_tpx(theme, menu_palette.flat ? 4 : 3),
        .text_x = xd_tpx(theme, 28),
        .large_text_x = xd_tpx(theme, 44),
        .arrow_w = xd_tpx(theme, 20),
        .separator_h = xd_tpx(theme, 9),
        .banner_w = xd_tpx(theme, 21),
    };
}

void xd_theme_menu_palette(const xd_theme *theme, xd_menu_palette *out) {
    ops(theme)->menu_palette(theme, out);
}

void xd_theme_draw_menu_frame(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (xd_pkgstyle_draw_start_menu_frame(theme, cairo, rect) ||
        xd_pkgstyle_draw_menu_frame(theme, cairo, rect))
        return;
    xd_menu_palette menu_palette;
    ops(theme)->menu_palette(theme, &menu_palette);
    xd_fill_rect(cairo, rect, menu_palette.background);
    int px = xd_tpx(theme, 1);
    if (menu_palette.flat) {
        xd_draw_bevel(cairo, rect, px, menu_palette.border, menu_palette.border);
        return;
    }
    /* EDGE_RAISED, as Windows 2000 draws menus. */
    rect = xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
                         xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
    xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                  xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
}

static void draw_submenu_arrow(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_rgba color) {
    /* Solid right-pointing triangle, 4 logical px wide, 7 tall. */
    double width = xd_tpx(theme, 4);
    double height = xd_tpx(theme, 7);
    double x = rect.x + rect.w - xd_tpx(theme, 12);
    double y = rect.y + (rect.h - height) / 2;
    xd_set_color(cairo, color);
    cairo_move_to(cairo, x, y);
    cairo_line_to(cairo, x + width, y + height / 2);
    cairo_line_to(cairo, x, y + height);
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

void xd_theme_draw_menu_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                             const char *label, bool submenu, bool large) {
    xd_theme_draw_menu_item_ex(theme, cairo, rect, state, label, submenu, large, nullptr);
}

void xd_theme_draw_menu_item_ex(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                const char *label, bool submenu, bool large,
                                const xd_menu_item_extras *extras) {
    xd_menu_palette menu_palette;
    ops(theme)->menu_palette(theme, &menu_palette);
    xd_menu_metrics menu_metrics = xd_theme_menu_metrics(theme);
    bool disabled = state & XD_STATE_DISABLED;
    bool hot = state & XD_STATE_HOT;

    xd_rgba text = menu_palette.text;
    bool themed = xd_pkgstyle_draw_start_menu_item(theme, cairo, rect, state, &text) ||
                  xd_pkgstyle_draw_menu_item(theme, cairo, rect, state, &text);
    if (themed) {
        /* The package's background and text colour. */
    } else if (hot) {
        xd_fill_rect(cairo, rect, menu_palette.hilight);
        if (menu_palette.hilight_border.a > 0)
            xd_draw_bevel(cairo, rect, xd_tpx(theme, 1), menu_palette.hilight_border,
                          menu_palette.hilight_border);
        text = disabled ? menu_palette.disabled_text : menu_palette.hilight_text;
    } else if (disabled) {
        text = menu_palette.disabled_text;
    }

    int tx = rect.x + (large ? menu_metrics.large_text_x : menu_metrics.text_x);
    xd_rect tr = {tx, rect.y, rect.x + rect.w - tx - menu_metrics.arrow_w, rect.h};
    if (disabled && !hot && !themed && menu_palette.disabled_emboss.a > 0) {
        int px = xd_tpx(theme, 1);
        xd_text_style emboss =
            xd_text(theme, XD_FONT_MENU, menu_palette.disabled_emboss, XD_ALIGN_START);
        emboss.underline = extras != nullptr ? extras->underline : nullptr;
        xd_draw_text(cairo, (xd_rect){tr.x + px, tr.y + px, tr.w, tr.h}, label, &emboss);
    }
    xd_font_spec bold = theme->scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style st = xd_text(theme, XD_FONT_MENU, text, XD_ALIGN_START);
    if (extras != nullptr && extras->bold)
        st.font = &bold;
    if (extras != nullptr)
        st.underline = extras->underline;
    if (extras != nullptr && extras->accel != nullptr) {
        xd_text_style ac = xd_text(theme, XD_FONT_MENU, text, XD_ALIGN_END);
        xd_draw_text(cairo, tr, extras->accel, &ac);
        tr.w -= xd_text_width(cairo, extras->accel, &ac) + xd_tpx(theme, 12);
    }
    xd_draw_text(cairo, tr, label, &st);
    xd_rect arrow = {rect.x + rect.w - menu_metrics.arrow_w, rect.y, menu_metrics.arrow_w, rect.h};
    if (submenu && !xd_pkgstyle_draw_start_menu_submenu(theme, cairo, arrow, state) &&
        !xd_pkgstyle_draw_menu_submenu(theme, cairo, arrow, disabled))
        draw_submenu_arrow(theme, cairo, rect, text);
    if (extras != nullptr && (extras->checked || extras->glyph != XD_MENU_GLYPH_NONE)) {
        int box = xd_tpx(theme, 16);
        xd_rect ic = {rect.x + menu_metrics.icon_x, rect.y + (rect.h - box) / 2, box, box};
        static const xd_glyph map[] = {
            [XD_MENU_GLYPH_RESTORE] = XD_GLYPH_RESTORE,
            [XD_MENU_GLYPH_MINIMIZE] = XD_GLYPH_MINIMIZE,
            [XD_MENU_GLYPH_MAXIMIZE] = XD_GLYPH_MAXIMIZE,
            [XD_MENU_GLYPH_CLOSE] = XD_GLYPH_CLOSE,
        };
        if (extras->checked && xd_pkgstyle_draw_menu_check(theme, cairo, ic, disabled))
            return;
        xd_glyph glyph = extras->checked ? XD_GLYPH_CHECK : map[extras->glyph];
        /* Window glyphs are drawn small, as in a window menu. */
        xd_rect gr = extras->checked ? ic : xd_rect_inset(ic, xd_tpx(theme, 3), xd_tpx(theme, 3));
        xd_draw_glyph(theme, cairo, gr, glyph, text, 1.0);
    }
}

int xd_theme_menu_bold_text_width(const xd_theme *theme, cairo_t *cairo, const char *label) {
    xd_font_spec bold = theme->scheme.fonts[XD_FONT_MENU];
    bold.bold = true;
    xd_text_style st = xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_START);
    st.font = &bold;
    return xd_text_width(cairo, label, &st);
}

void xd_theme_draw_menu_separator(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (xd_pkgstyle_draw_start_menu_separator(theme, cairo, rect) ||
        xd_pkgstyle_draw_menu_separator(theme, cairo, rect))
        return;
    xd_menu_palette menu_palette;
    ops(theme)->menu_palette(theme, &menu_palette);
    int px = xd_tpx(theme, 1);
    int inset = menu_palette.flat ? xd_tpx(theme, 2) : xd_tpx(theme, 1);
    int y = rect.y + (rect.h - (menu_palette.separator_light.a > 0 ? 2 * px : px)) / 2;
    xd_fill_rect(cairo, (xd_rect){rect.x + inset, y, rect.w - 2 * inset, px},
                 menu_palette.separator_dark);
    if (menu_palette.separator_light.a > 0)
        xd_fill_rect(cairo, (xd_rect){rect.x + inset, y + px, rect.w - 2 * inset, px},
                     menu_palette.separator_light);
}

void xd_theme_draw_start_banner(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                const char *product, const char *edition) {
    xd_menu_palette menu_palette;
    ops(theme)->menu_palette(theme, &menu_palette);
    xd_font_spec bold = {.size_pt = 14, .bold = true};
    snprintf(bold.family, sizeof(bold.family), "%s", XD_FONT_FAMILY_TAHOMA);
    if (!xd_pkgstyle_draw_start_banner(theme, cairo, rect, &menu_palette.banner_text, &bold))
        xd_fill_gradient(cairo, rect, menu_palette.banner_top, menu_palette.banner_bottom, false);

    /* Rotate so text runs bottom to top, starting a few px above the
     * banner's foot. */
    xd_font_spec light = bold;
    light.bold = false;
    xd_text_style st = {.font = &bold,
                        .scale = theme->scale,
                        .color = menu_palette.banner_text,
                        .smoothing = theme->scheme.font_smoothing};

    cairo_save(cairo);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_clip(cairo);
    cairo_translate(cairo, rect.x, rect.y + rect.h);
    cairo_rotate(cairo, -XD_PI / 2);
    int x = xd_tpx(theme, 6);
    int w1 = xd_text_width(cairo, product, &st);
    xd_draw_text(cairo, (xd_rect){x, 0, rect.h - x, rect.w}, product, &st);
    st.font = &light;
    xd_draw_text(cairo, (xd_rect){x + w1 + xd_tpx(theme, 4), 0, rect.h - x - w1, rect.w}, edition,
                 &st);
    cairo_restore(cairo);
}

int xd_theme_menu_text_width(const xd_theme *theme, cairo_t *cairo, const char *label) {
    xd_text_style st = xd_text(theme, XD_FONT_MENU, xd_rgba_hex(0), XD_ALIGN_START);
    return xd_text_width(cairo, label, &st);
}

void xd_theme_toolkit_colors(const xd_theme *theme, xd_toolkit_colors *out) {
    *out = (xd_toolkit_colors){0};
    ops(theme)->toolkit_colors(theme, out);
}

/* ---- taskbar toolbars ---------------------------------------------------------- */

int xd_theme_toolbar_grip_width(const xd_theme *theme) {
    return theme->style == XD_STYLE_CLASSIC ? xd_tpx(theme, 6) : xd_tpx(theme, 8);
}

void xd_theme_draw_toolbar_grip(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (xd_pkgstyle_draw_taskbar_grip(theme, cairo, rect))
        return;
    int px = xd_tpx(theme, 1);
    if (theme->style == XD_STYLE_CLASSIC) {
        /* Windows 2000: a thin raised bar, inset from top and bottom. */
        xd_rect bar = {rect.x + xd_tpx(theme, 2), rect.y + xd_tpx(theme, 2), xd_tpx(theme, 3),
                       rect.h - xd_tpx(theme, 4)};
        xd_draw_bevel(cairo, bar, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
        return;
    }
    /* XP: a column of dots with highlights. */
    int dot = XD_MAX(px * 2, 2);
    int x = rect.x + (rect.w - dot) / 2;
    for (int y = rect.y + xd_tpx(theme, 5); y + dot <= rect.y + rect.h - xd_tpx(theme, 4);
         y += dot * 2) {
        xd_fill_rect(cairo, (xd_rect){x + px, y + px, dot, dot},
                     xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.6));
        xd_fill_rect(cairo, (xd_rect){x, y, dot, dot}, xd_rgba_alpha(xd_rgba_hex(0x000000), 0.45));
    }
}

void xd_theme_draw_toolbar_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state) {
    if (xd_pkgstyle_draw_taskbar_toolbar_button(theme, cairo, rect, state))
        return;
    bool hot = state & XD_STATE_HOT;
    bool pressed = state & XD_STATE_PRESSED;
    if (!hot && !pressed)
        return;
    int px = xd_tpx(theme, 1);
    if (theme->style == XD_STYLE_CLASSIC) {
        /* Windows 2000 flat buttons: a 1px edge appears on hover. */
        if (pressed)
            xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                          xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
        else
            xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                          xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
        return;
    }
    double rad = xd_tpx(theme, 2);
    xd_path_rounded(cairo, rect.x, rect.y, rect.w, rect.h, rad, rad, rad, rad);
    xd_set_color(cairo, pressed ? xd_rgba_alpha(xd_rgba_hex(0x000000), 0.25)
                                : xd_rgba_alpha(xd_rgba_hex(0xFFFFFF), 0.3));
    cairo_fill_preserve(cairo);
    xd_set_color(cairo, xd_rgba_alpha(xd_rgba_hex(pressed ? 0x000000 : 0xFFFFFF), 0.5));
    cairo_set_line_width(cairo, px);
    cairo_stroke(cairo);
}

/* ---- dialog controls -------------------------------------------------------- */

static bool classic(const xd_theme *theme) { return theme->style == XD_STYLE_CLASSIC; }
static bool watercolor(const xd_theme *theme) { return theme->style == XD_STYLE_WHISTLER; }
/* Luna and Aero draw their controls from measured artwork (luna.c,
 * style_aero.c). */
static bool luna(const xd_theme *theme) { return theme->style == XD_STYLE_LUNA; }
static bool aero(const xd_theme *theme) { return theme->style == XD_STYLE_AERO; }

/* XP's flat control border colour per style. */
static xd_rgba flat_border(const xd_theme *theme) {
    return xd_rgba_hex(watercolor(theme) ? 0x646464 : 0x7F9DB9);
}

/* Whistler's flat face, tab and page colours. */
static const uint32_t WC_FACE = 0xEBEBE4, WC_TAB = 0xDBDBCE, WC_PAGE = 0xF3F3EF,
                      WC_THUMB_BORDER = 0xC1C1BB;

xd_field_colors xd_theme_field_colors(const xd_theme *theme) {
    return (xd_field_colors){
        .text = xd_sys(theme, XD_COLOR_WINDOW_TEXT),
        .disabled_text = xd_sys(theme, XD_COLOR_GRAY_TEXT),
        .selection = watercolor(theme) ? xd_rgba_hex(0x316AC5) : xd_sys(theme, XD_COLOR_HILIGHT),
        .selection_text =
            watercolor(theme) ? xd_rgba_hex(0xFFFFFF) : xd_sys(theme, XD_COLOR_HILIGHT_TEXT),
    };
}

xd_rect xd_theme_draw_field(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool disabled) {
    return xd_theme_draw_field_state(theme, cairo, rect,
                                     disabled ? XD_STATE_DISABLED : XD_STATE_NORMAL);
}

xd_rect xd_theme_draw_field_state(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state) {
    bool disabled = state & XD_STATE_DISABLED;
    xd_rect inner;
    if (xd_pkgstyle_draw_field(theme, cairo, rect, disabled, &inner))
        return inner;
    int px = xd_tpx(theme, 1);
    xd_rgba fill = disabled ? xd_sys(theme, XD_COLOR_BUTTON_FACE) : xd_sys(theme, XD_COLOR_WINDOW);
    if (aero(theme))
        return xd_aero_draw_field(theme, cairo, rect, state);
    if (luna(theme)) {
        xd_rgba border, off;
        xd_luna_field_colors(theme, &border, &off);
        xd_fill_rect(cairo, rect, disabled ? off : xd_sys(theme, XD_COLOR_WINDOW));
        return xd_draw_bevel(cairo, rect, px, border, border);
    }
    if (!classic(theme)) {
        xd_fill_rect(cairo, rect, disabled ? xd_rgba_hex(WC_FACE) : xd_sys(theme, XD_COLOR_WINDOW));
        return xd_draw_bevel(cairo, rect, px, flat_border(theme), flat_border(theme));
    }
    /* EDGE_SUNKEN */
    rect = xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                         xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
    rect = xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW),
                         xd_sys(theme, XD_COLOR_BUTTON_LIGHT));
    xd_fill_rect(cairo, rect, fill);
    return rect;
}

int xd_theme_dropdown_button_width(const xd_theme *theme) {
    return classic(theme) ? xd_tpx(theme, 16) : xd_tpx(theme, 17);
}

void xd_theme_draw_dropdown_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                   uint8_t state) {
    if (xd_pkgstyle_draw_dropdown_button(theme, cairo, rect, state))
        return;
    if (luna(theme)) {
        xd_luna_draw_dropdown_button(theme, cairo, rect, state);
        return;
    }
    if (aero(theme)) {
        xd_aero_draw_dropdown_button(theme, cairo, rect, state);
        return;
    }
    bool pressed = state & XD_STATE_PRESSED;
    bool disabled = state & XD_STATE_DISABLED;
    int px = xd_tpx(theme, 1);
    xd_rgba arrow = xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT);

    if (watercolor(theme)) {
        /* Just a black arrow on the face, inside the field's border. */
        xd_fill_rect(cairo, rect, xd_rgba_hex(pressed ? 0xD8DAD4 : WC_FACE));
        arrow = xd_rgba_hex(disabled ? 0xA0A0A0 : 0x000000);
    } else if (classic(theme)) {
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
        if (pressed) {
            xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_SHADOW),
                          xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
            rect = (xd_rect){rect.x + px, rect.y + px, rect.w, rect.h};
        } else {
            xd_rect in = xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
                                       xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
            xd_draw_bevel(cairo, in, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                          xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
        }
    } else {
        /* XP's pale blue rounded arrow button. */
        uint32_t top = pressed ? 0x6E8DED : (state & XD_STATE_HOT) ? 0xDDECFE : 0xC1D3FB;
        uint32_t bottom = pressed ? 0x8FADF5 : (state & XD_STATE_HOT) ? 0xB6CDFB : 0xA6C0F4;
        if (disabled) {
            top = 0xF0F0F0;
            bottom = 0xE0E0E0;
        }
        double rad = xd_tpx(theme, 2);
        xd_rect button = xd_rect_inset(rect, px, px);
        xd_path_rounded(cairo, button.x, button.y, button.w, button.h, rad, rad, rad, rad);
        cairo_save(cairo);
        cairo_clip(cairo);
        xd_fill_gradient(cairo, button, xd_rgba_hex(top), xd_rgba_hex(bottom), false);
        cairo_restore(cairo);
        arrow = xd_rgba_hex(disabled ? 0xC9C7BA : 0x4D6185);
    }

    /* Down-pointing triangle, 7 logical px wide. */
    double width = xd_tpx(theme, 7);
    double height = xd_tpx(theme, 4);
    double x = rect.x + (rect.w - width) / 2;
    double y = rect.y + (rect.h - height) / 2;
    xd_set_color(cairo, arrow);
    cairo_move_to(cairo, x, y);
    cairo_line_to(cairo, x + width, y);
    cairo_line_to(cairo, x + width / 2, y + height);
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

void xd_theme_draw_list_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool selected,
                             const char *label) {
    xd_field_colors field_colors = xd_theme_field_colors(theme);
    if (selected)
        xd_fill_rect(cairo, rect, field_colors.selection);
    xd_text_style st =
        xd_text(theme, XD_FONT_MESSAGE, selected ? field_colors.selection_text : field_colors.text,
                XD_ALIGN_START);
    xd_draw_text(cairo, xd_rect_inset4(rect, xd_tpx(theme, 2), 0, xd_tpx(theme, 2), 0), label, &st);
}

void xd_theme_draw_focus_rect(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    double pixel = xd_tpx(theme, 1);
    double dash[] = {pixel, pixel};
    cairo_save(cairo);
    xd_set_color(cairo, xd_sys(theme, XD_COLOR_BUTTON_TEXT));
    cairo_set_line_width(cairo, pixel);
    cairo_set_dash(cairo, dash, 2, 0);
    cairo_rectangle(cairo, rect.x + pixel / 2, rect.y + pixel / 2, rect.w - pixel, rect.h - pixel);
    cairo_stroke(cairo);
    cairo_restore(cairo);
}

/* Classic's 12x12 radio button, as DrawFrameControl draws it from Marlett's
 * pieces: an outer ring of Shadow over Hilight, an inner one of DkShadow
 * over Light (top-left over bottom-right), the well, and a 4x4 dot. */
static const char *const radio_rows[12] = {
    "....SSSS....", "..SSDDDDSS..", ".SDDWWWWLLH.", ".SDWWWWWWLH.",
    "SDWWWWWWWWLH", "SDWWWWWWWWLH", "SDWWWWWWWWLH", "SDWWWWWWWWLH",
    ".SDWWWWWWLH.", ".SLLWWWWLLH.", "..HHLLLLHH..", "....HHHH....",
};
static const char *const radio_dot[4] = {".##.", "####", "####", ".##."};

static void classic_radio(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state) {
    bool disabled = state & XD_STATE_DISABLED;
    xd_rgba well = xd_sys(theme, (disabled || (state & XD_STATE_PRESSED)) ? XD_COLOR_BUTTON_FACE
                                                                          : XD_COLOR_WINDOW);
    xd_rgba dot = xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT);
    double k = rect.w / 12.0;
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            xd_rgba color;
            switch (radio_rows[y][x]) {
            case 'S':
                color = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
                break;
            case 'D':
                color = xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW);
                break;
            case 'H':
                color = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT);
                break;
            case 'L':
                color = xd_sys(theme, XD_COLOR_BUTTON_LIGHT);
                break;
            case 'W':
                color = well;
                if ((state & XD_STATE_CHECKED) && x >= 4 && x < 8 && y >= 4 && y < 8 &&
                    radio_dot[y - 4][x - 4] == '#')
                    color = dot;
                break;
            default:
                continue;
            }
            /* Each design pixel covers whole device pixels, edges shared. */
            int x0 = rect.x + (int)lround(x * k);
            int y0 = rect.y + (int)lround(y * k);
            int x1 = rect.x + (int)lround((x + 1) * k);
            int y1 = rect.y + (int)lround((y + 1) * k);
            xd_fill_rect(cairo, (xd_rect){x0, y0, x1 - x0, y1 - y0}, color);
        }
    }
}

/* The flat styles' 13x13 round button: the check box's border and shading
 * (XP's light-grey to white), its hover ring, and a dot in the check mark's
 * colour. */
static void flat_radio(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state) {
    if (luna(theme)) {
        xd_luna_draw_radio(theme, cairo, rect, state);
        return;
    }
    if (aero(theme)) {
        xd_aero_draw_radio(theme, cairo, rect, state);
        return;
    }
    xd_toolkit_colors toolkit_colors;
    xd_theme_toolkit_colors(theme, &toolkit_colors);
    bool disabled = state & XD_STATE_DISABLED;
    double px = xd_tpx(theme, 1);
    double cx = rect.x + rect.w / 2.0;
    double cy = rect.y + rect.h / 2.0;
    double radius = rect.w / 2.0;
    cairo_save(cairo);
    cairo_new_path(cairo);
    cairo_arc(cairo, cx, cy, radius - px / 2, 0, 2 * XD_PI);
    if (disabled) {
        xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
        cairo_fill_preserve(cairo);
    } else {
        xd_rgba top = xd_rgba_hex((state & XD_STATE_PRESSED) ? 0xB0B0A7 : 0xDCDCD7);
        cairo_pattern_t *pattern =
            cairo_pattern_create_linear(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h);
        cairo_pattern_add_color_stop_rgba(pattern, 0, top.r, top.g, top.b, 1);
        cairo_pattern_add_color_stop_rgba(pattern, 1, 1, 1, 1, 1);
        cairo_set_source(cairo, pattern);
        cairo_fill_preserve(cairo);
        cairo_pattern_destroy(pattern);
    }
    xd_set_color(cairo, disabled ? toolkit_colors.disabled : toolkit_colors.check_border);
    cairo_set_line_width(cairo, px);
    cairo_stroke(cairo);
    if (!disabled && (state & XD_STATE_HOT)) {
        cairo_arc(cairo, cx, cy, radius - 2 * px, 0, 2 * XD_PI);
        xd_set_color(cairo, toolkit_colors.button_hot);
        cairo_set_line_width(cairo, 2 * px);
        cairo_stroke(cairo);
    }
    if (state & XD_STATE_CHECKED) {
        cairo_arc(cairo, cx, cy, 2.5 * px, 0, 2 * XD_PI);
        xd_set_color(cairo, disabled ? toolkit_colors.disabled : toolkit_colors.check_mark);
        cairo_fill(cairo);
    }
    cairo_restore(cairo);
}

void xd_theme_draw_radio(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                         const char *label) {
    if (xd_pkgstyle_draw_radio(theme, cairo, rect, state, label))
        return;
    /* The button sits where a check box's box does, so their labels line up. */
    int slot = luna(theme) ? xd_luna_box_size(theme) : xd_tpx(theme, 13);
    bool disabled = state & XD_STATE_DISABLED;
    if (classic(theme)) {
        int box = xd_tpx(theme, 12);
        classic_radio(theme, cairo, (xd_rect){rect.x, rect.y + (rect.h - box) / 2, box, box},
                      state);
    } else {
        flat_radio(theme, cairo, (xd_rect){rect.x, rect.y + (rect.h - slot) / 2, slot, slot},
                   state);
    }
    xd_rgba fg = xd_sys(theme, XD_COLOR_WINDOW_TEXT);
    if (disabled) {
        xd_toolkit_colors toolkit_colors;
        xd_theme_toolkit_colors(theme, &toolkit_colors);
        fg = classic(theme) ? xd_sys(theme, XD_COLOR_GRAY_TEXT) : toolkit_colors.disabled;
    }
    int gap = xd_tpx(theme, 5);
    xd_rect text = {rect.x + slot + gap, rect.y, rect.w - slot - gap, rect.h};
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
    xd_draw_text(cairo, text, label, &st);
    if (state & XD_STATE_FOCUSED) {
        int width = XD_MIN(xd_text_width(cairo, label, &st), text.w);
        xd_theme_draw_focus_rect(theme, cairo,
                                 (xd_rect){text.x - xd_tpx(theme, 1), rect.y + xd_tpx(theme, 2),
                                           width + xd_tpx(theme, 3), rect.h - xd_tpx(theme, 4)});
    }
}

void xd_theme_draw_label(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *text,
                         bool wrap, bool bold, bool disabled) {
    xd_font_spec font = theme->scheme.fonts[XD_FONT_MESSAGE];
    font.bold = bold;
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE,
                               xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    st.font = &font;
    st.wrap = wrap;
    st.ellipsize = !wrap;
    xd_draw_text(cairo, rect, text, &st);
}

void xd_theme_draw_tab(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                       const char *label) {
    if (xd_pkgstyle_draw_tab(theme, cairo, rect, state, label))
        return;
    int px = xd_tpx(theme, 1);
    bool selected = state & XD_STATE_CHECKED;
    xd_rect tab = selected ? rect : xd_rect_inset4(rect, 0, xd_tpx(theme, 2), 0, 0);
    if (classic(theme)) {
        /* Raised edge on top/left/right, open at the bottom. */
        xd_fill_rect(cairo, tab, xd_sys(theme, XD_COLOR_BUTTON_FACE));
        xd_fill_rect(cairo, (xd_rect){tab.x + 2 * px, tab.y, tab.w - 4 * px, px},
                     xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
        xd_fill_rect(cairo, (xd_rect){tab.x + px, tab.y + px, px, px},
                     xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
        xd_fill_rect(cairo, (xd_rect){tab.x, tab.y + 2 * px, px, tab.h - 2 * px},
                     xd_sys(theme, XD_COLOR_BUTTON_HILIGHT));
        xd_fill_rect(cairo, (xd_rect){tab.x + tab.w - 2 * px, tab.y + px, px, tab.h - px},
                     xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
        xd_fill_rect(cairo, (xd_rect){tab.x + tab.w - px, tab.y + 2 * px, px, tab.h - 2 * px},
                     xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
    } else if (watercolor(theme)) {
        /* Flat: darker unselected tabs split by short white lines; the
         * selected one is lighter with a white edge, joining the page. */
        xd_fill_rect(cairo, tab, xd_rgba_hex(selected ? WC_PAGE : WC_TAB));
        if (selected) {
            xd_rgba white = xd_rgba_hex(0xFFFFFF);
            xd_fill_rect(cairo, (xd_rect){tab.x, tab.y, tab.w, px}, white);
            xd_fill_rect(cairo, (xd_rect){tab.x, tab.y, px, tab.h}, white);
            xd_fill_rect(cairo, (xd_rect){tab.x + tab.w - px, tab.y, px, tab.h}, white);
        } else {
            xd_fill_rect(cairo,
                         (xd_rect){tab.x + tab.w - px, tab.y + xd_tpx(theme, 4), px,
                                   tab.h - xd_tpx(theme, 8)},
                         xd_rgba_hex(0xFFFFFF));
        }
    } else if (luna(theme)) {
        xd_luna_draw_tab(theme, cairo, tab, state);
    } else if (aero(theme)) {
        xd_aero_draw_tab(theme, cairo, tab, state);
    } else {
        /* XP: white-to-beige tab, orange top line when hot or selected. */
        xd_rgba border = xd_rgba_hex(0x919B9C);
        xd_fill_gradient(cairo, tab, xd_rgba_hex(0xFFFFFF),
                         selected ? xd_rgba_hex(0xFCFCFE) : xd_rgba_hex(0xECEBE6), false);
        xd_fill_rect(cairo, (xd_rect){tab.x, tab.y, tab.w, px}, border);
        xd_fill_rect(cairo, (xd_rect){tab.x, tab.y, px, tab.h}, border);
        xd_fill_rect(cairo, (xd_rect){tab.x + tab.w - px, tab.y, px, tab.h}, border);
        if (selected || (state & XD_STATE_HOT)) {
            xd_fill_rect(cairo, (xd_rect){tab.x, tab.y, tab.w, 2 * px}, xd_rgba_hex(0xFFC83C));
            xd_fill_rect(cairo, (xd_rect){tab.x, tab.y + 2 * px, tab.w, px}, xd_rgba_hex(0xE6A000));
        }
    }
    xd_text_style st =
        xd_text(theme, XD_FONT_MESSAGE, xd_sys(theme, XD_COLOR_BUTTON_TEXT), XD_ALIGN_CENTER);
    xd_rect text = xd_rect_inset4(tab, xd_tpx(theme, 4), xd_tpx(theme, 1), xd_tpx(theme, 4), 0);
    xd_draw_text(cairo, text, label, &st);
    if (state & XD_STATE_FOCUSED) {
        int width = XD_MIN(xd_text_width(cairo, label, &st) + xd_tpx(theme, 4), text.w);
        xd_theme_draw_focus_rect(theme, cairo,
                                 (xd_rect){text.x + (text.w - width) / 2, text.y + xd_tpx(theme, 2),
                                           width, text.h - xd_tpx(theme, 4)});
    }
}

xd_rgba xd_theme_tab_page_color(const xd_theme *theme) {
    xd_rgba pkg_color;
    if (xd_pkgstyle_tab_page_color(theme, &pkg_color))
        return pkg_color;
    if (watercolor(theme))
        return xd_rgba_hex(WC_PAGE);
    if (aero(theme))
        return xd_rgba_hex(0xFFFFFF);
    return classic(theme) ? xd_sys(theme, XD_COLOR_BUTTON_FACE) : xd_rgba_hex(0xFCFCFE);
}

void xd_theme_draw_tab_panel(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (xd_pkgstyle_draw_tab_panel(theme, cairo, rect))
        return;
    int px = xd_tpx(theme, 1);
    if (classic(theme)) {
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
        xd_rect in = xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                                   xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
        xd_draw_bevel(cairo, in, px, xd_sys(theme, XD_COLOR_BUTTON_FACE),
                      xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
        return;
    }
    if (watercolor(theme)) {
        xd_fill_rect(cairo, rect, xd_rgba_hex(WC_PAGE));
        xd_draw_bevel(cairo, rect, px, xd_rgba_hex(0xFFFFFF), xd_rgba_hex(WC_THUMB_BORDER));
        return;
    }
    if (luna(theme)) {
        xd_luna_draw_tab_panel(theme, cairo, rect);
        return;
    }
    if (aero(theme)) {
        xd_aero_draw_tab_panel(theme, cairo, rect);
        return;
    }
    xd_rgba border = xd_rgba_hex(0x919B9C);
    xd_fill_gradient(cairo, rect, xd_rgba_hex(0xFCFCFE), xd_rgba_hex(0xF4F3EE), false);
    xd_draw_bevel(cairo, rect, px, border, border);
}

void xd_theme_draw_groupbox(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                            const char *label) {
    if (xd_pkgstyle_draw_groupbox(theme, cairo, rect, label))
        return;
    int px = xd_tpx(theme, 1);
    xd_text_style st =
        xd_text(theme, XD_FONT_MESSAGE,
                classic(theme) || watercolor(theme) ? xd_sys(theme, XD_COLOR_BUTTON_TEXT)
                : luna(theme)                       ? xd_luna_groupbox_text(theme)
                : aero(theme)                       ? xd_rgba_hex(0x000000)
                                                    : xd_rgba_hex(0x0046D5),
                XD_ALIGN_START);
    int th = xd_tpx(theme, 14);
    int top = rect.y + th / 2;
    xd_rect box = {rect.x, top, rect.w, rect.h - (top - rect.y)};
    if (classic(theme)) {
        /* Etched: shadow line with a highlight line inside it. */
        xd_rgba sh = xd_sys(theme, XD_COLOR_BUTTON_SHADOW);
        xd_rgba hi = xd_sys(theme, XD_COLOR_BUTTON_HILIGHT);
        xd_draw_bevel(cairo, (xd_rect){box.x + px, box.y + px, box.w - px, box.h - px}, px, hi, hi);
        xd_draw_bevel(cairo, (xd_rect){box.x, box.y, box.w - px, box.h - px}, px, sh, sh);
    } else if (watercolor(theme)) {
        xd_rgba border = xd_rgba_hex(WC_THUMB_BORDER);
        xd_draw_bevel(cairo, box, px, border, border);
    } else if (luna(theme) || aero(theme)) {
        /* Around the label, which has no background of its own: what's
         * behind the group shows through, as in XP and 7. */
        cairo_save(cairo);
        if (label != nullptr && label[0] != '\0') {
            int tw = xd_text_width(cairo, label, &st);
            cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_EVEN_ODD);
            cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
            cairo_rectangle(cairo, rect.x + xd_tpx(theme, 7), rect.y, tw + xd_tpx(theme, 4), th);
            cairo_clip(cairo);
        }
        if (luna(theme))
            xd_luna_draw_groupbox_frame(theme, cairo, box);
        else
            xd_aero_draw_groupbox_frame(theme, cairo, box);
        cairo_restore(cairo);
        if (label != nullptr && label[0] != '\0')
            xd_draw_text(
                cairo, (xd_rect){rect.x + xd_tpx(theme, 9), rect.y, rect.w - xd_tpx(theme, 9), th},
                label, &st);
        return;
    } else {
        xd_rgba border = xd_rgba_hex(0xD0D0BF);
        double rad = xd_tpx(theme, 3);
        xd_path_rounded(cairo, box.x + px / 2.0, box.y + px / 2.0, box.w - px, box.h - px, rad, rad,
                        rad, rad);
        xd_set_color(cairo, border);
        cairo_set_line_width(cairo, px);
        cairo_stroke(cairo);
    }
    if (label != nullptr && label[0] != '\0') {
        int tw = xd_text_width(cairo, label, &st);
        xd_rect gap = {rect.x + xd_tpx(theme, 7), rect.y, tw + xd_tpx(theme, 4), th};
        xd_fill_rect(cairo, gap,
                     classic(theme) ? xd_sys(theme, XD_COLOR_BUTTON_FACE)
                                    : xd_theme_tab_page_color(theme));
        xd_draw_text(cairo, xd_rect_inset4(gap, xd_tpx(theme, 2), 0, 0, 0), label, &st);
    }
}

int xd_theme_scrollbar_width(const xd_theme *theme) {
    return xd_metric_px(theme, XD_METRIC_SCROLL_WIDTH);
}

xd_rect xd_theme_vscrollbar_thumb(const xd_theme *theme, xd_rect rect, int first, int page,
                                  int total) {
    int arrow = XD_MIN(rect.w, rect.h / 2);
    xd_rect track = {rect.x, rect.y + arrow, rect.w, rect.h - 2 * arrow};
    if (total <= page || total <= 0 || track.h <= 0)
        return (xd_rect){track.x, track.y, track.w, 0};
    int min_thumb = xd_tpx(theme, 8);
    int th = XD_MAX((int)((long)track.h * page / total), min_thumb);
    th = XD_MIN(th, track.h);
    int range = total - page;
    int y = track.y + (int)((long)(track.h - th) * XD_CLAMP(first, 0, range) / range);
    return (xd_rect){track.x, y, track.w, th};
}

static void scroll_arrow(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool up,
                         bool disabled) {
    double width = xd_tpx(theme, 7);
    double height = xd_tpx(theme, 4);
    double x = rect.x + (rect.w - width) / 2;
    double y = rect.y + (rect.h - height) / 2;
    xd_set_color(cairo, classic(theme)
                            ? xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT)
                        : watercolor(theme) ? xd_rgba_hex(disabled ? 0xA0A0A0 : 0x000000)
                                            : xd_rgba_hex(disabled ? 0xC9C7BA : 0x4D6185));
    if (up) {
        cairo_move_to(cairo, x + width / 2, y);
        cairo_line_to(cairo, x + width, y + height);
        cairo_line_to(cairo, x, y + height);
    } else {
        cairo_move_to(cairo, x, y);
        cairo_line_to(cairo, x + width, y);
        cairo_line_to(cairo, x + width / 2, y + height);
    }
    cairo_close_path(cairo);
    cairo_fill(cairo);
}

/* Watercolor's arrows have no button; only the thumb is drawn: white with
 * a two-pixel grey border. */
static void scroll_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool thumb) {
    int px = xd_tpx(theme, 1);
    if (watercolor(theme)) {
        if (thumb) {
            xd_fill_rect(cairo, rect, xd_rgba_hex(0xFFFFFF));
            xd_draw_bevel(cairo, xd_rect_inset4(rect, px, 0, px, 0), 2 * px,
                          xd_rgba_hex(WC_THUMB_BORDER), xd_rgba_hex(WC_THUMB_BORDER));
        }
        return;
    }
    if (classic(theme)) {
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
        xd_rect in = xd_draw_bevel(cairo, rect, px, xd_sys(theme, XD_COLOR_BUTTON_LIGHT),
                                   xd_sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
        xd_draw_bevel(cairo, in, px, xd_sys(theme, XD_COLOR_BUTTON_HILIGHT),
                      xd_sys(theme, XD_COLOR_BUTTON_SHADOW));
    } else {
        double rad = xd_tpx(theme, 2);
        xd_rect button = xd_rect_inset(rect, px, px);
        xd_path_rounded(cairo, button.x, button.y, button.w, button.h, rad, rad, rad, rad);
        cairo_save(cairo);
        cairo_clip(cairo);
        xd_fill_gradient(cairo, button, xd_rgba_hex(0xC8D6FB), xd_rgba_hex(0xB0C8F8), true);
        cairo_restore(cairo);
        xd_path_rounded(cairo, button.x + px / 2.0, button.y + px / 2.0, button.w - px,
                        button.h - px, rad, rad, rad, rad);
        xd_set_color(cairo, xd_rgba_hex(0xFFFFFF));
        cairo_set_line_width(cairo, px);
        cairo_stroke(cairo);
    }
}

void xd_theme_draw_vscrollbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, int first,
                              int page, int total) {
    int arrow = XD_MIN(rect.w, rect.h / 2);
    bool disabled = total <= page;
    if (xd_pkgstyle_draw_vscrollbar(theme, cairo, rect,
                                    xd_theme_vscrollbar_thumb(theme, rect, first, page, total),
                                    disabled))
        return;
    if (luna(theme)) {
        xd_luna_draw_vscrollbar(theme, cairo, rect,
                                xd_theme_vscrollbar_thumb(theme, rect, first, page, total),
                                disabled);
        return;
    }
    if (aero(theme)) {
        xd_aero_draw_vscrollbar(theme, cairo, rect,
                                xd_theme_vscrollbar_thumb(theme, rect, first, page, total),
                                disabled);
        return;
    }
    /* Track: Classic dithers ButtonHilight over Scrollbar; XP is flat. */
    if (classic(theme)) {
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_SCROLLBAR));
        xd_fill_rect(cairo, rect, xd_rgba_alpha(xd_sys(theme, XD_COLOR_BUTTON_HILIGHT), 0.5));
    } else {
        xd_fill_rect(cairo, rect, xd_rgba_hex(watercolor(theme) ? WC_FACE : 0xF3F1EC));
    }
    xd_rect up = {rect.x, rect.y, rect.w, arrow};
    xd_rect down = {rect.x, rect.y + rect.h - arrow, rect.w, arrow};
    scroll_button(theme, cairo, up, false);
    scroll_arrow(theme, cairo, up, true, disabled);
    scroll_button(theme, cairo, down, false);
    scroll_arrow(theme, cairo, down, false, disabled);
    xd_rect thumb = xd_theme_vscrollbar_thumb(theme, rect, first, page, total);
    if (thumb.h > 0)
        scroll_button(theme, cairo, thumb, true);
}

void xd_theme_draw_dialog_banner(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                 const char *product, const char *edition) {
    xd_menu_palette menu_palette;
    ops(theme)->menu_palette(theme, &menu_palette);
    xd_fill_gradient(cairo, rect, menu_palette.banner_bottom, menu_palette.banner_top, true);

    int flag = XD_MIN(xd_tpx(theme, 48), rect.h - xd_tpx(theme, 10));
    int fx = rect.x + xd_tpx(theme, 16);
    xd_draw_start_logo(cairo, (xd_rect){fx, rect.y + (rect.h - flag) / 2, flag, flag});

    xd_font_spec big = {.size_pt = 18, .bold = true};
    snprintf(big.family, sizeof(big.family), "%s", XD_FONT_FAMILY_TAHOMA);
    xd_font_spec small = big;
    small.size_pt = 10;
    small.bold = false;
    xd_text_style st = {.font = &big,
                        .scale = theme->scale,
                        .color = menu_palette.banner_text,
                        .smoothing = theme->scheme.font_smoothing};
    int tx = fx + flag + xd_tpx(theme, 12);
    int line = xd_tpx(theme, 28);
    int top = rect.y + (rect.h - line - xd_tpx(theme, 16)) / 2;
    xd_draw_text(cairo, (xd_rect){tx, top, rect.x + rect.w - tx, line}, product, &st);
    st.font = &small;
    xd_draw_text(
        cairo,
        (xd_rect){tx + xd_tpx(theme, 2), top + line, rect.x + rect.w - tx, xd_tpx(theme, 16)},
        edition, &st);
}

/* ---- shared helpers ---------------------------------------------------- */

xd_rgba xd_sys(const xd_theme *theme, xd_syscolor color_index) {
    XD_ASSERT((unsigned)color_index < XD_COLOR_COUNT);
    return xd_rgba_from_rgb(theme->scheme.colors[color_index]);
}

int xd_tpx(const xd_theme *theme, double logical) { return xd_px(theme->scale, logical); }

int xd_metric_px(const xd_theme *theme, xd_metric metric) {
    XD_ASSERT((unsigned)metric < XD_METRIC_COUNT);
    return xd_px(theme->scale, theme->scheme.metrics[metric]);
}

xd_text_style xd_text(const xd_theme *theme, xd_font_role role, xd_rgba color, xd_halign align) {
    XD_ASSERT((unsigned)role < XD_FONT_COUNT);
    return (xd_text_style){
        .font = &theme->scheme.fonts[role],
        .scale = theme->scale,
        .color = color,
        .align = align,
        .ellipsize = true,
        .smoothing = theme->scheme.font_smoothing,
    };
}

void xd_draw_glyph(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_glyph glyph,
                   xd_rgba color, double weight) {
    /* Glyphs are designed on a 9x9 logical grid centred in r, so they scale
     * with DPI exactly like their strokes do. */
    int size = XD_MIN(xd_tpx(theme, 9), XD_MIN(rect.w, rect.h));
    double x = rect.x + (rect.w - size) / 2;
    double y = rect.y + (rect.h - size) / 2;
    double extent = size;
    double lw = fmax(1.0, round(weight * theme->scale));
    double thick = fmax(1.0, round(weight * 1.6 * theme->scale));

    cairo_save(cairo);
    xd_set_color(cairo, color);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_SQUARE);
    switch (glyph) {
    case XD_GLYPH_CLOSE: {
        double inset = round(extent * 0.1);
        cairo_set_line_width(cairo, thick);
        cairo_set_line_cap(cairo, CAIRO_LINE_CAP_BUTT);
        cairo_move_to(cairo, x + inset, y + inset);
        cairo_line_to(cairo, x + extent - inset, y + extent - inset);
        cairo_move_to(cairo, x + extent - inset, y + inset);
        cairo_line_to(cairo, x + inset, y + extent - inset);
        cairo_stroke(cairo);
        break;
    }
    case XD_GLYPH_MAXIMIZE: {
        double top = round(lw * 2);
        cairo_rectangle(cairo, x, y, extent, top);
        cairo_fill(cairo);
        cairo_set_line_width(cairo, lw);
        cairo_rectangle(cairo, x + lw / 2, y + lw / 2, extent - lw, extent - lw);
        cairo_stroke(cairo);
        break;
    }
    case XD_GLYPH_RESTORE: {
        double window_size = round(extent * 0.65);
        double off = extent - window_size;
        double top = round(lw * 2);
        cairo_set_line_width(cairo, lw);
        /* Front window (bottom-left). */
        cairo_rectangle(cairo, x, y + off, window_size, top);
        cairo_fill(cairo);
        cairo_rectangle(cairo, x + lw / 2, y + off + lw / 2, window_size - lw, window_size - lw);
        cairo_stroke(cairo);
        /* Back window (top-right), clipped so it stays behind the front. */
        cairo_save(cairo);
        cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(cairo, x - 1, y - 1, extent + 2, extent + 2);
        cairo_rectangle(cairo, x, y + off, window_size, window_size);
        cairo_clip(cairo);
        cairo_rectangle(cairo, x + off, y, window_size, top);
        cairo_fill(cairo);
        cairo_rectangle(cairo, x + off + lw / 2, y + lw / 2, window_size - lw, window_size - lw);
        cairo_stroke(cairo);
        cairo_restore(cairo);
        break;
    }
    case XD_GLYPH_MINIMIZE: {
        double line_height = round(lw * 2);
        cairo_rectangle(cairo, x + round(extent * 0.1), y + extent - line_height,
                        round(extent * 0.65), line_height);
        cairo_fill(cairo);
        break;
    }
    case XD_GLYPH_HELP: {
        cairo_set_line_width(cairo, thick);
        cairo_arc(cairo, x + extent / 2, y + extent * 0.3, extent * 0.25, XD_PI, 2.2 * XD_PI);
        cairo_line_to(cairo, x + extent / 2, y + extent * 0.68);
        cairo_stroke(cairo);
        cairo_rectangle(cairo, x + extent / 2 - lw, y + extent - 2 * lw, 2 * lw, 2 * lw);
        cairo_fill(cairo);
        break;
    }
    case XD_GLYPH_CHECK: {
        cairo_set_line_width(cairo, thick);
        cairo_set_line_cap(cairo, CAIRO_LINE_CAP_BUTT);
        cairo_move_to(cairo, x + extent * 0.12, y + extent * 0.5);
        cairo_line_to(cairo, x + extent * 0.4, y + extent * 0.78);
        cairo_line_to(cairo, x + extent * 0.9, y + extent * 0.2);
        cairo_stroke(cairo);
        break;
    }
    }
    cairo_restore(cairo);
}

static void ellipse(cairo_t *cairo, double cx, double cy, double rx, double ry) {
    cairo_save(cairo);
    cairo_translate(cairo, cx, cy);
    cairo_scale(cairo, rx, ry);
    cairo_arc(cairo, 0, 0, 1, 0, 2 * XD_PI);
    cairo_restore(cairo);
}

void xd_draw_start_logo(cairo_t *cairo, xd_rect rect) {
    /* Tux, drawn from simple shapes so it stays readable at 16px. Shapes
     * are laid out on a unit square and scaled to fit r. */
    double size = XD_MIN(rect.w, rect.h);
    if (size <= 0)
        return;
    cairo_save(cairo);
    cairo_translate(cairo, rect.x + (rect.w - size) / 2, rect.y + (rect.h - size) / 2);
    cairo_scale(cairo, size, size);

    const xd_rgba black = xd_rgba_hex(0x1A1A1A);
    const xd_rgba white = xd_rgba_hex(0xFFFFFF);
    const xd_rgba orange = xd_rgba_hex(0xF2B01E);
    const xd_rgba orange_dark = xd_rgba_hex(0xC88400);

    /* Feet behind the body. */
    xd_set_color(cairo, orange);
    ellipse(cairo, 0.33, 0.92, 0.15, 0.065);
    ellipse(cairo, 0.67, 0.92, 0.15, 0.065);
    cairo_fill(cairo);

    /* Body and head. */
    xd_set_color(cairo, black);
    ellipse(cairo, 0.5, 0.6, 0.32, 0.34);
    cairo_fill(cairo);
    ellipse(cairo, 0.5, 0.27, 0.2, 0.22);
    cairo_fill(cairo);
    /* Flippers. */
    ellipse(cairo, 0.21, 0.62, 0.08, 0.2);
    ellipse(cairo, 0.79, 0.62, 0.08, 0.2);
    cairo_fill(cairo);

    /* White belly and face. */
    xd_set_color(cairo, white);
    ellipse(cairo, 0.5, 0.65, 0.21, 0.26);
    cairo_fill(cairo);
    ellipse(cairo, 0.5, 0.31, 0.13, 0.11);
    cairo_fill(cairo);

    /* Eyes. */
    ellipse(cairo, 0.43, 0.22, 0.055, 0.07);
    ellipse(cairo, 0.57, 0.22, 0.055, 0.07);
    cairo_fill(cairo);
    xd_set_color(cairo, black);
    ellipse(cairo, 0.445, 0.235, 0.028, 0.035);
    ellipse(cairo, 0.555, 0.235, 0.028, 0.035);
    cairo_fill(cairo);

    /* Beak: two halves so it reads as a beak, not a blob. */
    xd_set_color(cairo, orange);
    ellipse(cairo, 0.5, 0.315, 0.11, 0.05);
    cairo_fill(cairo);
    xd_set_color(cairo, orange_dark);
    ellipse(cairo, 0.5, 0.345, 0.075, 0.03);
    cairo_fill(cairo);
    cairo_restore(cairo);
}
