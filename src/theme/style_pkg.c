/* SPDX-License-Identifier: MIT
 * What a theme package draws in place of its base style: window frames
 * and caption buttons, from uxtheme's Window class, as XP's non-client
 * painting does. See pkg.h.
 */
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#include "panes.h"
#include "pkg.h"
#include "style.h"

static const char *const WINDOW = "Window";

/* Vista's and 7's glass frame (DWMWindow): with the compositor's glass,
 * Aero's frame with the package's caption buttons; without it, the
 * package's Window parts (Windows' Basic look), as Windows falls back. */
static bool dwm_frame(const xd_theme *theme) {
    return theme->glass && theme->pkg != nullptr &&
           xd_pkg_has(theme->pkg, "DWMWindow", "CloseButton");
}

bool xd_pkgstyle_has_frame(const xd_theme *theme) {
    return theme->pkg != nullptr && xd_pkg_has(theme->pkg, WINDOW, "Caption") && !dwm_frame(theme);
}

/* A DWMWindow glyph: the one drawn for the nearest DPI at or below the
 * monitor's, at its own size, centred in `rect`. */
static void dwm_glyph(const xd_theme *theme, cairo_t *cairo, const char *glyph, const char *state,
                      xd_rect rect) {
    static const int dpis[] = {192, 144, 120, 96};
    double dpi = 96.0 * theme->scale;
    for (size_t i = 0; i < XD_ARRAY_LEN(dpis); i++) {
        char part[32];
        g_snprintf(part, sizeof(part), "%s%d", glyph, dpis[i]);
        if ((dpis[i] > dpi + 0.5 && dpis[i] != 96) || !xd_pkg_has(theme->pkg, "DWMWindow", part))
            continue;
        /* Drawn for dpis[i]: scaled only by what the monitor adds. */
        xd_theme at = *theme;
        at.scale = MAX(dpi / dpis[i], 0.5);
        int width, height;
        if (!xd_pkg_part_size(&at, "DWMWindow", part, state, &width, &height))
            return;
        (void)xd_pkg_draw(&at, cairo, "DWMWindow", part, state,
                          (xd_rect){rect.x + (rect.w - width) / 2, rect.y + (rect.h - height) / 2,
                                    width, height});
        return;
    }
}

bool xd_pkgstyle_dwm_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_frame_part part,
                            const xd_frame *frame, bool leftmost, bool alone) {
    if (!dwm_frame(theme))
        return false;
    bool close = part == XD_PART_CLOSE;
    const char *base = close ? (alone ? "CloseButtonAlone" : "CloseButton")
                             : (leftmost ? "MinButton" : "MaxButton");
    char bg[40];
    g_snprintf(bg, sizeof(bg), "%s%s", base, frame->active ? "" : "Inactive");
    if (!xd_pkg_has(theme->pkg, "DWMWindow", bg))
        g_strlcpy(bg, base, sizeof(bg));
    bool pressed = frame->pressed == part && frame->hot == part;
    bool hot = frame->hot == part && !pressed;
    const char *state = pressed ? "3" : hot ? "2" : "1";
    if (hot) {
        /* The glow around a hot button, larger than it, centred on it. */
        const char *glow = close ? "CloseGlow" : "ButtonGlow";
        int width, height;
        if (xd_pkg_part_size(theme, "DWMWindow", glow, "1", &width, &height))
            (void)xd_pkg_draw(theme, cairo, "DWMWindow", glow, "1",
                              (xd_rect){rect.x + (rect.w - width) / 2,
                                        rect.y + (rect.h - height) / 2, width, height});
    }
    (void)xd_pkg_draw(theme, cairo, "DWMWindow", bg, state, rect);
    const char *glyph = close                      ? "CloseGlyph"
                        : part == XD_PART_HELP     ? "HelpGlyph"
                        : part == XD_PART_MINIMIZE ? "MinGlyph"
                        : frame->maximized         ? "RestoreGlyph"
                                                   : "MaxGlyph";
    dwm_glyph(theme, cairo, glyph, state, rect);
    return true;
}

/* ---- Vista's and 7's glass frame: DWMWindow's edges and reflections ---- */

static const char *const DWM = "DWMWindow";

bool xd_pkgstyle_dwm_has_reflections(const xd_theme *theme) {
    return dwm_frame(theme) && xd_pkg_has(theme->pkg, DWM, "ReflectionStreaks");
}

/* A reflection from the window's top corner, as it is, at its Opacity for
 * the state (active, inactive, maximized active, maximized inactive). */
static void reflection(const xd_theme *theme, cairo_t *cairo, const char *part,
                       const xd_frame *frame, xd_rect outer, bool right) {
    const char *image = frame->active ? "1" : "2";
    const char *state = frame->maximized ? (frame->active ? "3" : "4") : image;
    int width, height, opacity = 100;
    if (!xd_pkg_part_size(theme, DWM, part, image, &width, &height))
        return;
    (void)xd_pkg_int(theme->pkg, DWM, part, state, "Opacity", &opacity);
    if (opacity <= 0)
        return;
    cairo_push_group(cairo);
    (void)xd_pkg_draw(
        theme, cairo, DWM, part, image,
        (xd_rect){right ? outer.x + outer.w - width : outer.x, outer.y, width, height});
    cairo_pop_group_to_source(cairo);
    cairo_paint_with_alpha(cairo, MIN(opacity, 100) / 100.0);
}

bool xd_pkgstyle_dwm_reflections(const xd_theme *theme, cairo_t *cairo, xd_rect outer,
                                 const xd_frame *frame) {
    if (!xd_pkgstyle_dwm_has_reflections(theme))
        return false;
    /* The streaks aren't stretched: past their size (802 x 604 in 7)
     * there's plain glass. */
    reflection(theme, cairo, "ReflectionStreaks", frame, outer, false);
    reflection(theme, cairo, "ReflectionTopLeft", frame, outer, false);
    reflection(theme, cairo, "ReflectionTopRight", frame, outer, true);
    return true;
}

static void slice(const xd_theme *theme, cairo_t *cairo, const char *part, const char *state,
                  xd_rect src, xd_rect dst) {
    (void)xd_pkg_draw_slice(theme, cairo, DWM, part, state, src, dst);
}

/* DWM's edges are four pieces (uxtheme.ini's notes): FrameTop, 21 x 27,
 * holds the top corners and edge (rows 0-6, its middle column stretched),
 * and at its bottom centre the client edge's top corners (columns 8-12,
 * rows 24-26: a light line, a dark one, and one under the client area);
 * FrameBottom, 21 x 11, the client edge's bottom (rows 0-2) and the
 * bottom corners (rows 3-10); FrameLeft and FrameRight, 11 x 3, the outer
 * edge (3 columns) and the client edge (3 columns) across a side. How
 * DWM stretches them between isn't documented: the outer edge and the
 * client edge are each laid where their lines show they go, with the
 * glass between, which keeps the client edge's light and dark lines (DWM's
 * own SizingMargins would squeeze the light one out of an 8 px border). */
bool xd_pkgstyle_dwm_edges(const xd_theme *theme, cairo_t *cairo, xd_rect outer, xd_rect client,
                           const xd_frame *frame) {
    static const char *const pieces[] = {"FrameTop", "FrameBottom", "FrameLeft", "FrameRight"};
    if (!dwm_frame(theme) || frame->maximized)
        return false;
    for (size_t i = 0; i < XD_ARRAY_LEN(pieces); i++)
        if (!xd_pkg_has(theme->pkg, DWM, pieces[i]))
            return false;
    const char *st = frame->active ? "1" : "2";
    int u1 = xd_tpx(theme, 1), u2 = xd_tpx(theme, 2), u3 = xd_tpx(theme, 3);
    int u7 = xd_tpx(theme, 7), u8 = xd_tpx(theme, 8);
    int right = outer.x + outer.w, bottom = outer.y + outer.h;
    cairo_save(cairo);
    /* Nothing over the client area: a line under it shows only where the
     * client is transparent. */
    cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cairo, outer.x, outer.y, outer.w, outer.h);
    if (client.w > 0 && client.h > 0)
        cairo_rectangle(cairo, client.x, client.y, client.w, client.h);
    cairo_clip(cairo);
    /* The outer edge: corners, top and bottom, sides. */
    slice(theme, cairo, "FrameTop", st, (xd_rect){0, 0, 7, 7}, (xd_rect){outer.x, outer.y, u7, u7});
    slice(theme, cairo, "FrameTop", st, (xd_rect){10, 0, 1, 7},
          (xd_rect){outer.x + u7, outer.y, outer.w - 2 * u7, u7});
    slice(theme, cairo, "FrameTop", st, (xd_rect){14, 0, 7, 7},
          (xd_rect){right - u7, outer.y, u7, u7});
    slice(theme, cairo, "FrameBottom", st, (xd_rect){0, 3, 7, 8},
          (xd_rect){outer.x, bottom - u8, u7, u8});
    slice(theme, cairo, "FrameBottom", st, (xd_rect){10, 3, 1, 8},
          (xd_rect){outer.x + u7, bottom - u8, outer.w - 2 * u7, u8});
    slice(theme, cairo, "FrameBottom", st, (xd_rect){14, 3, 7, 8},
          (xd_rect){right - u7, bottom - u8, u7, u8});
    int side = bottom - u8 - (outer.y + u7);
    slice(theme, cairo, "FrameLeft", st, (xd_rect){0, 0, 3, 3},
          (xd_rect){outer.x, outer.y + u7, u3, side});
    slice(theme, cairo, "FrameRight", st, (xd_rect){8, 0, 3, 3},
          (xd_rect){right - u3, outer.y + u7, u3, side});
    /* The client edge: a light line outside a dark one. */
    if (client.w > 0 && client.h > 0) {
        int cr_ = client.x + client.w, cb = client.y + client.h;
        slice(theme, cairo, "FrameTop", st, (xd_rect){8, 24, 2, 3},
              (xd_rect){client.x - u3, client.y - u2, u2, u3});
        slice(theme, cairo, "FrameTop", st, (xd_rect){10, 24, 1, 3},
              (xd_rect){client.x - u1, client.y - u2, client.w + u2, u3});
        slice(theme, cairo, "FrameTop", st, (xd_rect){11, 24, 2, 3},
              (xd_rect){cr_ + u1, client.y - u2, u2, u3});
        slice(theme, cairo, "FrameBottom", st, (xd_rect){8, 0, 2, 3},
              (xd_rect){client.x - u3, cb - u1, u2, u3});
        slice(theme, cairo, "FrameBottom", st, (xd_rect){10, 0, 1, 3},
              (xd_rect){client.x - u1, cb - u1, client.w + u2, u3});
        slice(theme, cairo, "FrameBottom", st, (xd_rect){11, 0, 2, 3},
              (xd_rect){cr_ + u1, cb - u1, u2, u3});
        slice(theme, cairo, "FrameLeft", st, (xd_rect){8, 0, 3, 3},
              (xd_rect){client.x - u2, client.y + u1, u3, client.h - u2});
        slice(theme, cairo, "FrameRight", st, (xd_rect){0, 0, 3, 3},
              (xd_rect){cr_ - u1, client.y + u1, u3, client.h - u2});
    }
    cairo_restore(cairo);
    return true;
}

static int part_h(const xd_theme *theme, const char *part, int fallback) {
    int width, height;
    return xd_pkg_part_size(theme, WINDOW, part, "Active", &width, &height) ? height : fallback;
}

/* Windows sizes a frame from system metrics, not from the style's images
 * (it stretches them to fit): borders of XP's sizing frame, 4 px, plus
 * Vista's and 7's PaddedBorderWidth; the caption CaptionBarHeight + 1
 * below the top border (30 for Luna, as XP's screenshots measure). A
 * maximized window's borders are off the screen. */
static int frame_border(const xd_theme *theme) {
    int padded = 0;
    (void)xd_pkg_sys_int(theme->pkg, "PaddedBorderWidth", &padded);
    return xd_tpx(theme, 4 + MAX(padded, 0));
}

static int caption_bar(const xd_theme *theme) {
    int cbh = 0;
    if (xd_pkg_sys_int(theme->pkg, "CaptionBarHeight", &cbh) && cbh > 0)
        return xd_tpx(theme, cbh + 1);
    /* No metric: the caption image less the border. */
    return MAX(part_h(theme, "Caption", xd_tpx(theme, 30)) - frame_border(theme), xd_tpx(theme, 8));
}

xd_insets xd_pkgstyle_frame_insets(const xd_theme *theme, const xd_frame *frame) {
    if (frame->maximized)
        return (xd_insets){0, caption_bar(theme), 0, 0};
    int border = frame_border(theme);
    return (xd_insets){border, border + caption_bar(theme), border, border};
}

static const char *button_part(xd_frame_part part, bool maximized) {
    if (part == XD_PART_CLOSE)
        return "CloseButton";
    if (part == XD_PART_MAXIMIZE)
        return maximized ? "RestoreButton" : "MaxButton";
    if (part == XD_PART_MINIMIZE)
        return "MinButton";
    return "HelpButton";
}

void xd_pkgstyle_frame_layout(const xd_theme *theme, xd_rect outer, const xd_frame *frame,
                              xd_frame_layout *frame_layout) {
    xd_insets in = xd_pkgstyle_frame_insets(theme, frame);
    frame_layout->caption = (xd_rect){outer.x, outer.y, outer.w, in.top};
    /* Where Windows puts them, whatever a style's Offsets say (the WE
     * styles' would put them outside the window): the style's own button
     * size if it fits the bar (WE's 18x20), else square at the bar's
     * height less 4; centred between the top border and the client area,
     * 2 px in from the right border and 2 px apart. */
    int border = frame->maximized ? 0 : frame_border(theme);
    int bar = in.top - border;
    int size = MAX(bar - xd_tpx(theme, 5), xd_tpx(theme, 8));
    int bw = size, bh = size;
    int width, height;
    if (xd_pkg_part_size(theme, WINDOW, "CloseButton", "1", &width, &height) &&
        height <= bar - xd_tpx(theme, 2)) {
        bw = width;
        bh = height;
    }
    int step = bw + xd_tpx(theme, 2);
    int right = outer.x + outer.w - border - xd_tpx(theme, 2);
    int bx = right - bw, by = outer.y + border + (bar - bh) / 2;
    static const struct {
        uint8_t flag;
        xd_frame_part part;
    } order[] = {
        {XD_CAPBTN_CLOSE, XD_PART_CLOSE},
        {XD_CAPBTN_MAXIMIZE, XD_PART_MAXIMIZE},
        {XD_CAPBTN_MINIMIZE, XD_PART_MINIMIZE},
        {XD_CAPBTN_HELP, XD_PART_HELP},
    };
    int leftmost = right;
    for (size_t i = 0; i < XD_ARRAY_LEN(order); i++) {
        if (!(frame->buttons & order[i].flag))
            continue;
        xd_layout_add_button(frame_layout, (xd_rect){bx, by, bw, bh}, order[i].part);
        leftmost = bx;
        bx -= step;
    }
    int icon = xd_tpx(theme, 16);
    int edge = frame->maximized ? 0 : in.left;
    frame_layout->icon = (xd_rect){
        outer.x + edge + xd_tpx(theme, 3),
        outer.y + (in.top - icon + (frame->maximized ? 0 : xd_tpx(theme, 1))) / 2, icon, icon};
    int tx = frame_layout->icon.x + icon + xd_tpx(theme, 3);
    frame_layout->title = (xd_rect){tx, outer.y, leftmost - xd_tpx(theme, 2) - tx, in.top};
}

void xd_pkgstyle_draw_frame(const xd_theme *theme, cairo_t *cairo, xd_rect outer,
                            const xd_frame *frame) {
    xd_insets in = xd_pkgstyle_frame_insets(theme, frame);
    const char *st = frame->active ? "Active" : "Inactive";
    (void)xd_pkg_draw(theme, cairo, WINDOW, frame->maximized ? "MaxCaption" : "Caption", st,
                      (xd_rect){outer.x, outer.y, outer.w, in.top});
    if (!frame->maximized) {
        int mid = outer.h - in.top - in.bottom;
        (void)xd_pkg_draw(theme, cairo, WINDOW, "FrameLeft", st,
                          (xd_rect){outer.x, outer.y + in.top, in.left, mid});
        (void)xd_pkg_draw(theme, cairo, WINDOW, "FrameRight", st,
                          (xd_rect){outer.x + outer.w - in.right, outer.y + in.top, in.right, mid});
        (void)xd_pkg_draw(theme, cairo, WINDOW, "FrameBottom", st,
                          (xd_rect){outer.x, outer.y + outer.h - in.bottom, outer.w, in.bottom});
    }
    xd_frame_layout frame_layout = {0};
    xd_pkgstyle_frame_layout(theme, outer, frame, &frame_layout);
    for (size_t i = 0; i < frame_layout.button_count; i++) {
        const char *part = button_part(frame_layout.button_parts[i], frame->maximized);
        /* Normal, Hot, Pushed; XP's strips have the inactive window's
         * versions after them. */
        int state_index = frame->pressed == frame_layout.button_parts[i] ? 3
                          : frame->hot == frame_layout.button_parts[i]   ? 2
                                                                         : 1;
        int count = 1;
        (void)xd_pkg_int(theme->pkg, WINDOW, part, nullptr, "ImageCount", &count);
        if (!frame->active && count >= 8)
            state_index += 4;
        char state[8];
        g_snprintf(state, sizeof(state), "%d", state_index);
        (void)xd_pkg_draw(theme, cairo, WINDOW, part, state, frame_layout.button_rects[i]);
    }
    xd_style_task_icon(theme, cairo, frame->icon, frame_layout.icon.x, frame_layout.icon.y);
    if (frame->title == nullptr || frame_layout.title.w <= 0)
        return;
    const char *cap = frame->maximized ? "MaxCaption" : "Caption";
    xd_rgba color =
        xd_sys(theme, frame->active ? XD_COLOR_TITLE_TEXT : XD_COLOR_INACTIVE_TITLE_TEXT);
    (void)xd_pkg_color(theme->pkg, WINDOW, cap, st, "TextColor", &color);
    xd_text_style ts = xd_text(theme, XD_FONT_CAPTION, color, XD_ALIGN_START);
    const char *shadow = xd_pkg_prop(theme->pkg, WINDOW, cap, st, "TextShadowType");
    xd_rgba sc;
    int so[2] = {1, 1};
    const char *sov = xd_pkg_prop(theme->pkg, WINDOW, cap, st, "TextShadowOffset");
    if (sov != nullptr)
        (void)sscanf(sov, "%d %d", &so[0], &so[1]);
    if (shadow != nullptr && g_ascii_strcasecmp(shadow, "Single") == 0 &&
        xd_pkg_color(theme->pkg, WINDOW, cap, st, "TextShadowColor", &sc)) {
        xd_text_style ss = xd_text(theme, XD_FONT_CAPTION, sc, XD_ALIGN_START);
        xd_rect sr = frame_layout.title;
        sr.x += (int)lround(so[0] * theme->scale);
        sr.y += (int)lround(so[1] * theme->scale);
        xd_draw_text(cairo, sr, frame->title, &ss);
    }
    xd_draw_text(cairo, frame_layout.title, frame->title, &ts);
}

/* ---- controls -------------------------------------------------------------------- */

static const char *const BUTTON = "Button";

static void state_str(char out[8], int state_index) { g_snprintf(out, 8, "%d", state_index); }

/* Normal 1, Hot 2, Pressed 3, Disabled 4: uxtheme's usual order. */
static int basic_state(uint8_t state) {
    if (state & XD_STATE_DISABLED)
        return 4;
    if (state & XD_STATE_PRESSED)
        return 3;
    return (state & XD_STATE_HOT) ? 2 : 1;
}

static xd_rgba text_color(const xd_theme *theme, const char *cls, const char *part, int state_index,
                          xd_rgba fallback) {
    char st[8];
    state_str(st, state_index);
    xd_rgba color = fallback;
    (void)xd_pkg_color(theme->pkg, cls, part, st, "TextColor", &color);
    return color;
}

bool xd_pkgstyle_draw_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                             const char *label) {
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, BUTTON, "PushButton"))
        return false;
    int state_index = basic_state(state);
    if (state_index == 1 && (state & XD_STATE_DEFAULT))
        state_index = 5; /* Defaulted */
    char st[8];
    state_str(st, state_index);
    (void)xd_pkg_draw(theme, cairo, BUTTON, "PushButton", st, rect);
    xd_rgba fg = text_color(
        theme, BUTTON, "PushButton", state_index,
        xd_sys(theme, (state & XD_STATE_DISABLED) ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT));
    xd_rect content_rect = xd_pkg_content_rect(theme, BUTTON, "PushButton", st, rect);
    xd_text_style ts = xd_text(theme, XD_FONT_MESSAGE, fg, XD_ALIGN_CENTER);
    xd_draw_text(cairo, content_rect, label, &ts);
    if (state & XD_STATE_FOCUSED)
        xd_theme_draw_focus_rect(theme, cairo,
                                 xd_rect_inset(content_rect, xd_tpx(theme, 1), xd_tpx(theme, 1)));
    return true;
}

/* A check box or radio button: the box at its own size where the label's
 * row starts, then the label. */
static bool box_and_label(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                          const char *label, const char *part, int checked_offset) {
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, BUTTON, part))
        return false;
    int state_index = basic_state(state) + ((state & XD_STATE_CHECKED) ? checked_offset : 0);
    char st[8];
    state_str(st, state_index);
    int bw = xd_tpx(theme, 13), bh = bw;
    (void)xd_pkg_part_size(theme, BUTTON, part, st, &bw, &bh);
    (void)xd_pkg_draw(theme, cairo, BUTTON, part, st,
                      (xd_rect){rect.x, rect.y + (rect.h - bh) / 2, bw, bh});
    bool disabled = state & XD_STATE_DISABLED;
    xd_rgba fg = text_color(theme, BUTTON, part, state_index,
                            xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT));
    int gap = xd_tpx(theme, 5);
    xd_rect text = {rect.x + bw + gap, rect.y, rect.w - bw - gap, rect.h};
    xd_text_style ts = xd_text(theme, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
    xd_draw_text(cairo, text, label, &ts);
    if (state & XD_STATE_FOCUSED) {
        int width = MIN(xd_text_width(cairo, label, &ts), text.w);
        xd_theme_draw_focus_rect(theme, cairo,
                                 (xd_rect){text.x - xd_tpx(theme, 1), rect.y + xd_tpx(theme, 2),
                                           width + xd_tpx(theme, 3), rect.h - xd_tpx(theme, 4)});
    }
    return true;
}

bool xd_pkgstyle_draw_checkbox(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                               const char *label) {
    /* Unchecked 1-4, Checked 5-8 (Mixed 9-12). */
    return box_and_label(theme, cairo, rect, state, label, "CheckBox", 4);
}

bool xd_pkgstyle_draw_radio(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                            const char *label) {
    return box_and_label(theme, cairo, rect, state, label, "RadioButton", 4);
}

bool xd_pkgstyle_tab_page_color(const xd_theme *theme, xd_rgba *out) {
    if (theme->pkg == nullptr)
        return false;
    return xd_pkg_color(theme->pkg, "Tab", "Body", nullptr, "FillColorHint", out) ||
           xd_pkg_color(theme->pkg, "Tab", "Pane", nullptr, "FillColorHint", out);
}

bool xd_pkgstyle_draw_groupbox(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                               const char *label) {
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, BUTTON, "GroupBox"))
        return false;
    int th = xd_tpx(theme, 14);
    int top = rect.y + th / 2;
    xd_rgba fg = text_color(theme, BUTTON, "GroupBox", 1, xd_sys(theme, XD_COLOR_BUTTON_TEXT));
    xd_text_style ts = xd_text(theme, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
    bool has_label = label != nullptr && label[0] != '\0';
    xd_rect gap = {0};
    if (has_label)
        gap = (xd_rect){rect.x + xd_tpx(theme, 7), rect.y,
                        xd_text_width(cairo, label, &ts) + xd_tpx(theme, 4), th};
    /* The box is drawn around the label, which has no background of its
     * own: whatever is behind the group shows through, as in Windows. */
    cairo_save(cairo);
    if (has_label) {
        cairo_set_fill_rule(cairo, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
        cairo_rectangle(cairo, gap.x, gap.y, gap.w, gap.h);
        cairo_clip(cairo);
    }
    (void)xd_pkg_draw(theme, cairo, BUTTON, "GroupBox", "1",
                      (xd_rect){rect.x, top, rect.w, rect.h - (top - rect.y)});
    cairo_restore(cairo);
    if (has_label)
        xd_draw_text(cairo, xd_rect_inset4(gap, xd_tpx(theme, 2), 0, 0, 0), label, &ts);
    return true;
}

bool xd_pkgstyle_draw_field(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool disabled,
                            xd_rect *inner) {
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, "Edit", "EditText"))
        return false;
    const char *st = disabled ? "Disabled" : "Normal";
    xd_rgba fill = xd_sys(theme, disabled ? XD_COLOR_BUTTON_FACE : XD_COLOR_WINDOW);
    (void)xd_pkg_color(theme->pkg, "Edit", "EditText", st, "FillColor", &fill);
    xd_fill_rect(cairo, rect, fill);
    (void)xd_pkg_draw(theme, cairo, "Edit", "EditText", st, rect);
    int border = 1;
    (void)xd_pkg_int(theme->pkg, "Edit", "EditText", st, "BorderSize", &border);
    *inner = xd_rect_inset(rect, xd_tpx(theme, border), xd_tpx(theme, border));
    return true;
}

bool xd_pkgstyle_draw_dropdown_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                      uint8_t state) {
    char st[8];
    state_str(st, basic_state(state));
    return xd_pkg_draw(theme, cairo, "ComboBox", "DropDownButton", st, rect);
}

/* A scroll bar along `rect`: arrows at its ends, the track either side of the
 * thumb, the thumb and its gripper. */
static bool scrollbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_rect thumb,
                      bool disabled, bool vertical) {
    const char *const SB = "ScrollBar";
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, SB, "ArrowBtn"))
        return false;
    int len = vertical ? rect.h : rect.w, across = vertical ? rect.w : rect.h;
    int arrow = MIN(across, len / 2);
    /* [start, end) along the bar, as a rectangle across it. */
#define SPAN(start, end)                                            \
    (vertical ? (xd_rect){rect.x, (start), rect.w, (end) - (start)} \
              : (xd_rect){(start), rect.y, (end) - (start), rect.h})
    int lo = (vertical ? rect.y : rect.x), hi = lo + len;
    xd_rect first = SPAN(lo, lo + arrow), last = SPAN(hi - arrow, hi);
    int tlo = vertical ? thumb.y : thumb.x, tlen = vertical ? thumb.h : thumb.w;
    const char *upper = vertical ? "UpperTrackVert" : "UpperTrackHorz";
    const char *lower = vertical ? "LowerTrackVert" : "LowerTrackHorz";
    const char *track_state = disabled ? "4" : "1";
    if (tlen > 0) {
        (void)xd_pkg_draw(theme, cairo, SB, upper, track_state, SPAN(lo + arrow, tlo));
        (void)xd_pkg_draw(theme, cairo, SB, lower, track_state, SPAN(tlo + tlen, hi - arrow));
    } else {
        (void)xd_pkg_draw(theme, cairo, SB, upper, track_state, SPAN(lo + arrow, hi - arrow));
    }
#undef SPAN
    /* ArrowBtn: Up 1-4, Down 5-8, Left 9-12, Right 13-16 (Normal, Hot,
     * Pressed, Disabled). */
    int base = vertical ? 1 : 9;
    char st[8];
    state_str(st, base + (disabled ? 3 : 0));
    (void)xd_pkg_draw(theme, cairo, SB, "ArrowBtn", st, first);
    state_str(st, base + 4 + (disabled ? 3 : 0));
    (void)xd_pkg_draw(theme, cairo, SB, "ArrowBtn", st, last);
    if (tlen > 0) {
        (void)xd_pkg_draw(theme, cairo, SB, vertical ? "ThumbBtnVert" : "ThumbBtnHorz", "1", thumb);
        (void)xd_pkg_draw(theme, cairo, SB, vertical ? "GripperVert" : "GripperHorz", "1", thumb);
    }
    return true;
}

bool xd_pkgstyle_draw_vscrollbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_rect thumb,
                                 bool disabled) {
    return scrollbar(theme, cairo, rect, thumb, disabled, true);
}

bool xd_pkgstyle_draw_hscrollbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_rect thumb,
                                 bool disabled) {
    return scrollbar(theme, cairo, rect, thumb, disabled, false);
}

bool xd_pkgstyle_draw_tab(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                          const char *label) {
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, "Tab", "TabItem"))
        return false;
    bool selected = state & XD_STATE_CHECKED;
    /* The selected tab is TopTabItem, taller, joining the page. */
    const char *part =
        selected && xd_pkg_has(theme->pkg, "Tab", "TopTabItem") ? "TopTabItem" : "TabItem";
    int state_index = selected                      ? 3
                      : (state & XD_STATE_DISABLED) ? 4
                      : (state & XD_STATE_HOT)      ? 2
                                                    : 1;
    char st[8];
    state_str(st, state_index);
    xd_rect tab = selected ? rect : xd_rect_inset4(rect, 0, xd_tpx(theme, 2), 0, 0);
    (void)xd_pkg_draw(theme, cairo, "Tab", part, st, tab);
    xd_rgba fg = text_color(theme, "Tab", part, state_index, xd_sys(theme, XD_COLOR_BUTTON_TEXT));
    xd_text_style ts = xd_text(theme, XD_FONT_MESSAGE, fg, XD_ALIGN_CENTER);
    xd_rect text = xd_rect_inset4(tab, xd_tpx(theme, 4), xd_tpx(theme, 1), xd_tpx(theme, 4), 0);
    xd_draw_text(cairo, text, label, &ts);
    if (state & XD_STATE_FOCUSED) {
        int width = MIN(xd_text_width(cairo, label, &ts) + xd_tpx(theme, 4), text.w);
        xd_theme_draw_focus_rect(theme, cairo,
                                 (xd_rect){text.x + (text.w - width) / 2, text.y + xd_tpx(theme, 2),
                                           width, text.h - xd_tpx(theme, 4)});
    }
    return true;
}

bool xd_pkgstyle_draw_tab_panel(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, "Tab", "Pane"))
        return false;
    (void)xd_pkg_draw(theme, cairo, "Tab", "Pane", "1", rect);
    if (xd_pkg_has(theme->pkg, "Tab", "Body"))
        (void)xd_pkg_draw(theme, cairo, "Tab", "Body", "1",
                          xd_rect_inset(rect, xd_tpx(theme, 1), xd_tpx(theme, 1)));
    return true;
}

/* ---- the rest of the controls ---------------------------------------------------- */

static bool has(const xd_theme *theme, const char *cls, const char *part) {
    return theme->pkg != nullptr && xd_pkg_has(theme->pkg, cls, part);
}

static xd_rgba text_color_st(const xd_theme *theme, const char *cls, const char *part,
                             const char *st, xd_rgba fallback) {
    xd_rgba color = fallback;
    (void)xd_pkg_color(theme->pkg, cls, part, st, "TextColor", &color);
    return color;
}

/* The background's content rectangle as uxtheme's
 * GetThemeBackgroundContentRect gives it: ContentMargins, or for an image
 * its SizingMargins, or for a border and fill its BorderSize. */
static xd_rect content_or_sizing(const xd_theme *theme, const char *cls, const char *part,
                                 const char *st, xd_rect rect) {
    xd_insets insets;
    if (xd_pkg_margins(theme->pkg, cls, part, st, "ContentMargins", &insets))
        return xd_pkg_content_rect(theme, cls, part, st, rect);
    const char *bg = xd_pkg_prop(theme->pkg, cls, part, st, "BgType");
    if (bg != nullptr && g_ascii_strcasecmp(bg, "BorderFill") == 0) {
        int border_size = 1;
        (void)xd_pkg_int(theme->pkg, cls, part, st, "BorderSize", &border_size);
        insets = (xd_insets){border_size, border_size, border_size, border_size};
    } else if (!xd_pkg_margins(theme->pkg, cls, part, st, "SizingMargins", &insets)) {
        return rect;
    }
    int left = xd_tpx(theme, insets.left), rr = xd_tpx(theme, insets.right);
    int top = xd_tpx(theme, insets.top), bottom = xd_tpx(theme, insets.bottom);
    xd_rect content_rect = {rect.x + left, rect.y + top, rect.w - left - rr, rect.h - top - bottom};
    return content_rect.w > 0 && content_rect.h > 0 ? content_rect : rect;
}

bool xd_pkgstyle_draw_sizebox(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (!has(theme, "ScrollBar", "Sizebox"))
        return false;
    if (!xd_pkg_draw(theme, cairo, "ScrollBar", "SizeboxBkgnd", "1", rect))
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    (void)xd_pkg_draw(theme, cairo, "ScrollBar", "Sizebox", "RightAlign", rect);
    return true;
}

bool xd_pkgstyle_draw_progress(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                               double fraction) {
    const char *const PROGRESS_CLASS = "Progress";
    if (!has(theme, PROGRESS_CLASS, "Bar"))
        return false;
    (void)xd_pkg_draw(theme, cairo, PROGRESS_CLASS, "Bar", "1", rect);
    xd_rect content_rect = content_or_sizing(theme, PROGRESS_CLASS, "Bar", "1", rect);
    int done = (int)lround(content_rect.w * CLAMP(fraction, 0.0, 1.0));
    if (done <= 0)
        return true;
    /* Vista and 7 fill continuously; XP draws ProgressChunkSize chunks
     * ProgressSpaceSize apart, as comctl32 does. */
    if (has(theme, PROGRESS_CLASS, "Fill")) {
        (void)xd_pkg_draw(theme, cairo, PROGRESS_CLASS, "Fill", "Normal",
                          (xd_rect){content_rect.x, content_rect.y, done, content_rect.h});
        return true;
    }
    int chunk = 6, space = 2;
    (void)xd_pkg_int(theme->pkg, PROGRESS_CLASS, "Chunk", nullptr, "ProgressChunkSize", &chunk);
    (void)xd_pkg_int(theme->pkg, PROGRESS_CLASS, "Chunk", nullptr, "ProgressSpaceSize", &space);
    chunk = MAX(xd_tpx(theme, MAX(chunk, 1)), 1);
    space = xd_tpx(theme, MAX(space, 0));
    cairo_save(cairo);
    cairo_rectangle(cairo, content_rect.x, content_rect.y, content_rect.w, content_rect.h);
    cairo_clip(cairo);
    for (int x = content_rect.x; x < content_rect.x + done; x += chunk + space)
        (void)xd_pkg_draw(theme, cairo, PROGRESS_CLASS, "Chunk", "1",
                          (xd_rect){x, content_rect.y, chunk, content_rect.h});
    cairo_restore(cairo);
    return true;
}

bool xd_pkgstyle_draw_trackbar(const xd_theme *theme, cairo_t *cairo, xd_rect channel,
                               xd_rect thumb, uint8_t state) {
    const char *const TB = "TrackBar";
    if (!has(theme, TB, "Track"))
        return false;
    (void)xd_pkg_draw(theme, cairo, TB, "Track", "1", channel);
    /* Normal 1, Hot 2, Pressed 3, Focused 4, Disabled 5. */
    int state_index = (state & XD_STATE_DISABLED)  ? 5
                      : (state & XD_STATE_PRESSED) ? 3
                      : (state & XD_STATE_HOT)     ? 2
                      : (state & XD_STATE_FOCUSED) ? 4
                                                   : 1;
    char st[8];
    state_str(st, state_index);
    /* The thumb points down at the ticks below it. */
    (void)xd_pkg_draw(theme, cairo, TB, has(theme, TB, "ThumbBottom") ? "ThumbBottom" : "Thumb", st,
                      thumb);
    return true;
}

bool xd_pkgstyle_trackbar_tics_color(const xd_theme *theme, xd_rgba *out) {
    return theme->pkg != nullptr && xd_pkg_color(theme->pkg, "TrackBar", "Tics", "1", "Color", out);
}

bool xd_pkgstyle_draw_spin(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool up,
                           uint8_t state) {
    const char *part = up ? "Up" : "Down";
    if (!has(theme, "Spin", part))
        return false;
    char st[8];
    state_str(st, basic_state(state));
    (void)xd_pkg_draw(theme, cairo, "Spin", part, st, rect);
    return true;
}

bool xd_pkgstyle_draw_header_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state, const char *label) {
    const char *const HEADER_CLASS = "Header";
    if (!has(theme, HEADER_CLASS, "HeaderItem"))
        return false;
    /* Normal 1, Hot 2, Pressed 3. */
    int state_index = (state & XD_STATE_PRESSED) ? 3 : (state & XD_STATE_HOT) ? 2 : 1;
    char st[8];
    state_str(st, state_index);
    (void)xd_pkg_draw(theme, cairo, HEADER_CLASS, "HeaderItem", st, rect);
    if (label == nullptr)
        return true;
    xd_rgba fg =
        text_color_st(theme, HEADER_CLASS, "HeaderItem", st, xd_sys(theme, XD_COLOR_BUTTON_TEXT));
    xd_text_style ts = xd_text(theme, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
    xd_rect content_rect = xd_pkg_content_rect(theme, HEADER_CLASS, "HeaderItem", st, rect);
    xd_draw_text(cairo, xd_rect_inset4(content_rect, xd_tpx(theme, 6), 0, xd_tpx(theme, 6), 0),
                 label, &ts);
    return true;
}

bool xd_pkgstyle_draw_tree_glyph(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool open) {
    if (!has(theme, "TreeView", "Glyph"))
        return false;
    (void)xd_pkg_draw(theme, cairo, "TreeView", "Glyph", open ? "Opened" : "Closed", rect);
    return true;
}

bool xd_pkgstyle_draw_statusbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                const char *text, bool grip) {
    const char *const STATUS_CLASS = "Status";
    if (!has(theme, STATUS_CLASS, "Pane"))
        return false;
    if (!xd_pkg_draw(theme, cairo, STATUS_CLASS, nullptr, nullptr, rect))
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    int gw = 0, gh = 0;
    if (grip && !xd_pkg_part_size(theme, STATUS_CLASS, "Gripper", "1", &gw, &gh))
        grip = false;
    xd_rect pane = {rect.x, rect.y + xd_tpx(theme, 2), rect.w - (grip ? gw : 0),
                    rect.h - xd_tpx(theme, 2)};
    (void)xd_pkg_draw(theme, cairo, STATUS_CLASS, "Pane", "1", pane);
    if (grip)
        (void)xd_pkg_draw(theme, cairo, STATUS_CLASS, "Gripper", "1",
                          (xd_rect){rect.x + rect.w - gw, rect.y + rect.h - gh, gw, gh});
    if (text != nullptr) {
        xd_rgba fg =
            text_color_st(theme, STATUS_CLASS, "Pane", "1", xd_sys(theme, XD_COLOR_BUTTON_TEXT));
        xd_text_style ts = xd_text(theme, XD_FONT_TOOLTIP, fg, XD_ALIGN_START);
        xd_draw_text(cairo, xd_rect_inset4(pane, xd_tpx(theme, 4), 0, xd_tpx(theme, 4), 0), text,
                     &ts);
    }
    return true;
}

bool xd_pkgstyle_draw_tooltip(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                              const char *text) {
    const char *const TT = "Tooltip";
    if (!has(theme, TT, "Standard"))
        return false;
    /* Its own shape: rounded corners show what's behind. */
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_fill(cairo);
    cairo_restore(cairo);
    (void)xd_pkg_draw(theme, cairo, TT, "Standard", "Normal", rect);
    xd_rgba fg = text_color_st(theme, TT, "Standard", "Normal", xd_sys(theme, XD_COLOR_INFO_TEXT));
    xd_text_style ts = xd_text(theme, XD_FONT_TOOLTIP, fg, XD_ALIGN_START);
    xd_draw_text(cairo, xd_rect_inset(rect, xd_tpx(theme, 4), 0), text, &ts);
    return true;
}

bool xd_pkgstyle_draw_rebar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool gripper) {
    const char *const RB = "Rebar";
    if (!has(theme, RB, "Band") && !has(theme, RB, "Gripper"))
        return false;
    if (!xd_pkg_draw(theme, cairo, RB, nullptr, nullptr, rect))
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_BUTTON_FACE));
    (void)xd_pkg_draw(theme, cairo, RB, "Band", "1", rect);
    if (gripper) {
        int gw = xd_tpx(theme, 3), gh;
        (void)xd_pkg_part_size(theme, RB, "Gripper", "1", &gw, &gh);
        (void)xd_pkg_draw(theme, cairo, RB, "Gripper", "1",
                          (xd_rect){rect.x + xd_tpx(theme, 2), rect.y + xd_tpx(theme, 2), gw,
                                    rect.h - xd_tpx(theme, 4)});
    }
    return true;
}

/* Toolbar buttons: Normal 1, Hot 2, Pressed 3, Disabled 4, Checked 5,
 * HotChecked 6. */
static int toolbar_state(uint8_t state) {
    if (state & XD_STATE_DISABLED)
        return 4;
    if (state & XD_STATE_PRESSED)
        return 3;
    if (state & XD_STATE_CHECKED)
        return (state & XD_STATE_HOT) ? 6 : 5;
    return (state & XD_STATE_HOT) ? 2 : 1;
}

bool xd_pkgstyle_draw_tool_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state, const char *label) {
    const char *const TB = "Toolbar";
    if (!has(theme, TB, "Button"))
        return false;
    char st[8];
    state_str(st, toolbar_state(state));
    (void)xd_pkg_draw(theme, cairo, TB, "Button", st, rect);
    if (label != nullptr) {
        xd_rgba fg = text_color_st(
            theme, TB, "Button", st,
            xd_sys(theme, (state & XD_STATE_DISABLED) ? XD_COLOR_GRAY_TEXT : XD_COLOR_BUTTON_TEXT));
        xd_text_style ts = xd_text(theme, XD_FONT_MESSAGE, fg, XD_ALIGN_CENTER);
        xd_draw_text(cairo, xd_pkg_content_rect(theme, TB, "Button", st, rect), label, &ts);
    }
    return true;
}

/* The taskbar's toolbars (Quick Launch) and their grips: the TaskBar
 * application's classes, as Explorer opens them. */
bool xd_pkgstyle_draw_taskbar_toolbar_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                             uint8_t state) {
    if (!has(theme, "TaskBar::Toolbar", "Button"))
        return false;
    char st[8];
    state_str(st, toolbar_state(state));
    (void)xd_pkg_draw(theme, cairo, "TaskBar::Toolbar", "Button", st, rect);
    return true;
}

bool xd_pkgstyle_draw_taskbar_grip(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (!has(theme, "TaskBar::Rebar", "Gripper"))
        return false;
    (void)xd_pkg_draw(theme, cairo, "TaskBar::Rebar", "Gripper", "1", rect);
    return true;
}

/* ---- Vista and 7 menus (Menu's Popup and Bar parts) ----
 * XP's styles have none of these (its menus are flat, in [SysMetrics]'s
 * colours): they keep the style's drawing. */

static const char *const MENU = "Menu";

static bool menu_themed(const xd_theme *theme) { return has(theme, MENU, "PopupItem"); }

bool xd_pkgstyle_draw_menu_frame(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (!menu_themed(theme))
        return false;
    if (!xd_pkg_draw(theme, cairo, MENU, "PopupBackground", "1", rect))
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_MENU));
    (void)xd_pkg_draw(theme, cairo, MENU, "PopupBorders", "1", rect);
    /* 7's gutter, just left of the items' text. */
    int gw, gh;
    xd_menu_metrics menu_metrics = xd_theme_menu_metrics(theme);
    if (xd_pkg_part_size(theme, MENU, "PopupGutter", "1", &gw, &gh))
        (void)xd_pkg_draw(
            theme, cairo, MENU, "PopupGutter", "1",
            (xd_rect){rect.x + menu_metrics.border + menu_metrics.text_x - xd_tpx(theme, 5),
                      rect.y + menu_metrics.border, gw, rect.h - 2 * menu_metrics.border});
    return true;
}

bool xd_pkgstyle_draw_menu_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                xd_rgba *text) {
    if (!menu_themed(theme))
        return false;
    /* Normal 1, Hot 2, Disabled 3, DisabledHot 4. */
    bool hot = state & XD_STATE_HOT, disabled = state & XD_STATE_DISABLED;
    int state_index = disabled ? (hot ? 4 : 3) : (hot ? 2 : 1);
    char st[8];
    state_str(st, state_index);
    if (hot)
        (void)xd_pkg_draw(theme, cairo, MENU, "PopupItem", st, rect);
    *text = text_color_st(theme, MENU, "PopupItem", st,
                          xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_MENU_TEXT));
    return true;
}

bool xd_pkgstyle_draw_menu_separator(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (!menu_themed(theme) || !has(theme, MENU, "PopupSeparator"))
        return false;
    int width, height = xd_tpx(theme, 3);
    (void)xd_pkg_part_size(theme, MENU, "PopupSeparator", "1", &width, &height);
    xd_menu_metrics menu_metrics = xd_theme_menu_metrics(theme);
    /* Right of the gutter, as 7 draws it. */
    int x = rect.x + menu_metrics.text_x - xd_tpx(theme, 3);
    (void)xd_pkg_draw(theme, cairo, MENU, "PopupSeparator", "1",
                      (xd_rect){x, rect.y + (rect.h - height) / 2, rect.x + rect.w - x, height});
    return true;
}

bool xd_pkgstyle_draw_menu_submenu(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                   bool disabled) {
    if (!menu_themed(theme) || !has(theme, MENU, "PopupSubmenu"))
        return false;
    (void)xd_pkg_draw(theme, cairo, MENU, "PopupSubmenu", disabled ? "2" : "1", rect);
    return true;
}

bool xd_pkgstyle_draw_menu_check(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                 bool disabled) {
    if (!menu_themed(theme) || !has(theme, MENU, "PopupCheck"))
        return false;
    /* PopupCheckBackground: Disabled 1, Normal 2. PopupCheck:
     * CheckMarkNormal 1, CheckMarkDisabled 2. */
    (void)xd_pkg_draw(theme, cairo, MENU, "PopupCheckBackground", disabled ? "1" : "2", rect);
    (void)xd_pkg_draw(theme, cairo, MENU, "PopupCheck", disabled ? "2" : "1", rect);
    return true;
}

bool xd_pkgstyle_draw_menubar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, bool active) {
    if (!has(theme, MENU, "BarBackground"))
        return false;
    (void)xd_pkg_draw(theme, cairo, MENU, "BarBackground", active ? "Active" : "Inactive", rect);
    return true;
}

bool xd_pkgstyle_draw_menubar_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                   uint8_t state, const char *label) {
    if (!has(theme, MENU, "BarItem"))
        return false;
    /* Normal 1, Hot 2, Pushed 3, Disabled 4, DisabledHot 5,
     * DisabledPushed 6; Normal and Disabled have no background. */
    int state_index = (state & XD_STATE_PRESSED) ? 3 : (state & XD_STATE_HOT) ? 2 : 1;
    if (state & XD_STATE_DISABLED)
        state_index += 3;
    char st[8];
    state_str(st, state_index);
    if (state_index != 1 && state_index != 4)
        (void)xd_pkg_draw(theme, cairo, MENU, "BarItem", st, rect);
    xd_rgba fg = text_color_st(
        theme, MENU, "BarItem", st,
        xd_sys(theme, (state & XD_STATE_DISABLED) ? XD_COLOR_GRAY_TEXT : XD_COLOR_MENU_TEXT));
    xd_text_style ts = xd_text(theme, XD_FONT_MENU, fg, XD_ALIGN_CENTER);
    xd_draw_text(cairo, rect, label, &ts);
    return true;
}

/* ==== the taskbar, thumbnails, jump lists and Start menus ==================
}

 * Explorer's classes, as XP's and 7's explorer.exe opens them. Vista's
 * and 7's undocumented parts go by the names data/theme/uxtheme.ini
 * deduces for them (xd-theme-import writes them into the package). */

static bool seven(const xd_theme *theme) { return theme->style == XD_STYLE_AERO; }

static void basic_str(char out[8], uint8_t state) {
    state_str(out, (state & XD_STATE_PRESSED) ? 3 : (state & XD_STATE_HOT) ? 2 : 1);
}

/* The composited variant of an App::Class ("TaskBand2Composited::
 * TaskBand2"), as Windows picks it with desktop composition on, where it
 * has its own image for the part; else the plain class. A composited
 * class without one would fall back to its base class's image (Button's
 * push button), not the plain App::Class's. */
static const char *pick(const xd_theme *theme, const char *plain, const char *comp,
                        const char *part) {
    if (!theme->glass || comp == nullptr || !xd_pkg_has_own(theme->pkg, comp, part))
        return plain;
    const char *colon = strstr(comp, "::");
    const char *own = xd_pkg_prop(theme->pkg, comp, part, nullptr, "ImageFile");
    const char *inherited =
        colon != nullptr ? xd_pkg_prop(theme->pkg, colon + 2, part, nullptr, "ImageFile") : nullptr;
    return own != nullptr && own != inherited ? comp : plain;
}

/* Text with the part's TextShadowColor behind it, TextShadowOffset away
 * (XP's "Continuous" shadow drawn as a plain one). */
static void shadowed_text(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *text,
                          xd_text_style st, const char *cls, const char *part, const char *state) {
    xd_rgba sc;
    const char *type = xd_pkg_prop(theme->pkg, cls, part, state, "TextShadowType");
    if (type != nullptr && g_ascii_strcasecmp(type, "None") != 0 &&
        xd_pkg_color(theme->pkg, cls, part, state, "TextShadowColor", &sc)) {
        int so[2] = {1, 1};
        const char *sov = xd_pkg_prop(theme->pkg, cls, part, state, "TextShadowOffset");
        if (sov != nullptr)
            (void)sscanf(sov, "%d %d", &so[0], &so[1]);
        xd_text_style ss = st;
        ss.color = sc;
        ss.shadow = false;
        xd_draw_text(cairo,
                     (xd_rect){rect.x + (int)lround(so[0] * theme->scale),
                               rect.y + (int)lround(so[1] * theme->scale), rect.w, rect.h},
                     text, &ss);
    }
    xd_draw_text(cairo, rect, text, &st);
}

/* ---- the taskbar ---- */

static const char *edge_name(xd_edge edge) {
    switch (edge) {
    case XD_EDGE_TOP:
        return "Top";
    case XD_EDGE_LEFT:
        return "Left";
    case XD_EDGE_RIGHT:
        return "Right";
    case XD_EDGE_BOTTOM:
        break;
    }
    return "Bottom";
}

/* The taskbar's class: 7's superbar is TaskBar2's (Vista's TaskBar where a
 * package has none), with the glass their composited variants. */
static const char *taskbar_class(const xd_theme *theme, const char *part) {
    if (!seven(theme))
        return "TaskBar";
    if (xd_pkg_has_own(theme->pkg, "TaskBar2::TaskBar", part))
        return pick(theme, "TaskBar2::TaskBar", "TaskBar2Composited::TaskBar", part);
    return pick(theme, "TaskBar", "TaskBarComposited::TaskBar", part);
}

bool xd_pkgstyle_draw_taskbar(const xd_theme *theme, cairo_t *cairo, xd_rect rect, xd_edge edge) {
    if (theme->pkg == nullptr)
        return false;
    char part[32];
    g_snprintf(part, sizeof(part), "Background%s", edge_name(edge));
    const char *cls = taskbar_class(theme, part);
    if (!xd_pkg_has_own(theme->pkg, cls, part))
        return false;
    if (seven(theme)) {
        /* Vista's and 7's images are a sheen over the glass colour (or,
         * without composition, opaque). */
        xd_glass_colors glass_colors;
        xd_theme_glass_colors(theme, &glass_colors);
        cairo_save(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
        xd_fill_rect(cairo, rect, glass_colors.taskbar);
        cairo_restore(cairo);
    }
    (void)xd_pkg_draw(theme, cairo, cls, part, "1", rect);
    return true;
}

void xd_theme_draw_taskbar_sizing_bar(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                      xd_edge edge) {
    if (theme->pkg == nullptr)
        return;
    char part[32];
    g_snprintf(part, sizeof(part), "SizingBar%s", edge_name(edge));
    const char *cls = taskbar_class(theme, part);
    int width, height;
    if (!xd_pkg_has_own(theme->pkg, cls, part) ||
        !xd_pkg_part_size(theme, cls, part, "1", &width, &height))
        return;
    bool side = edge == XD_EDGE_LEFT || edge == XD_EDGE_RIGHT;
    (void)xd_pkg_draw(theme, cairo, cls, part, "1",
                      xd_style_rim_line(rect, edge, 0, side ? width : height));
}

/* An XP style's taskbar height: Windows sizes the task band from its
 * buttons, a 16 px icon within their ContentMargins, plus 6 (Luna's 4 and
 * 4 make XP's 30 px; the WE styles' 2 and 2 their 26). 0 where the
 * package has no task buttons (or is Vista's or 7's: their taskbar is
 * explorer's own size). */
int xd_pkgstyle_taskbar_height(const xd_theme *theme) {
    if (theme->pkg == nullptr || seven(theme) ||
        !xd_pkg_has_own(theme->pkg, "TaskBand::Toolbar", "Button"))
        return 0;
    xd_insets insets;
    if (!xd_pkg_margins(theme->pkg, "TaskBand::Toolbar", "Button", nullptr, "ContentMargins",
                        &insets))
        return 0;
    return xd_tpx(theme, 16 + MAX(insets.top, 0) + MAX(insets.bottom, 0) + 6);
}

/* A Start button image drawn at its size (TrueSize: the WE styles') is
 * the button's width; 0 for the style's own (Luna stretches its image to
 * fit "start"). */
int xd_pkgstyle_start_button_width(const xd_theme *theme) {
    const char *const SB = "Start::Button";
    int width, height;
    if (theme->pkg == nullptr || seven(theme) || !xd_pkg_has_own(theme->pkg, SB, nullptr))
        return 0;
    const char *sizing = xd_pkg_prop(theme->pkg, SB, nullptr, nullptr, "SizingType");
    if (sizing == nullptr || g_ascii_strcasecmp(sizing, "TrueSize") != 0 ||
        !xd_pkg_part_size(theme, SB, nullptr, "1", &width, &height))
        return 0;
    return width;
}

bool xd_pkgstyle_draw_start_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                   uint8_t state) {
    if (theme->pkg == nullptr)
        return false;
    char st[8];
    basic_str(st, state);
    /* Vista's orb (7's is explorer.exe's own: ours stands in). */
    if (seven(theme) && xd_pkg_has_own(theme->pkg, "StartMiddle::Button", nullptr))
        return xd_pkg_draw(theme, cairo, "StartMiddle::Button", nullptr, st, rect);
    const char *const SB = "Start::Button";
    if (!xd_pkg_has_own(theme->pkg, SB, nullptr))
        return false;
    (void)xd_pkg_draw(theme, cairo, SB, nullptr, st, rect);
    /* XP's flag is explorer.exe's, not the style's: our logo, then
     * "start" in the style's font and colours, where its content area
     * is. Styles whose button image has its own logo (the WE styles) put
     * the content area off the button and the font at 1 point: then
     * neither is drawn. */
    xd_rect content_rect = xd_pkg_content_rect(theme, SB, nullptr, st, rect);
    if (content_rect.x < rect.x || content_rect.x + content_rect.w > rect.x + rect.w)
        return true;
    int dy = (state & XD_STATE_PRESSED) ? xd_tpx(theme, 1) : 0;
    int icon = xd_tpx(theme, 20);
    int ix = rect.x + xd_tpx(theme, 8);
    xd_draw_start_logo(
        cairo, (xd_rect){ix, content_rect.y + (content_rect.h - icon) / 2 + dy, icon, icon});
    xd_font_spec font = {.family = XD_FONT_FAMILY_TREBUCHET, .size_pt = 13, .bold = true,
                         .italic = true};
    (void)xd_pkg_font(theme->pkg, SB, nullptr, st, "Font", &font, nullptr);
    if (font.size_pt < 4)
        return true;
    xd_text_style ts =
        xd_text(theme, XD_FONT_MENU, text_color_st(theme, SB, nullptr, st, xd_rgba_hex(0xFFFFFF)),
                XD_ALIGN_START);
    ts.font = &font;
    int tx = ix + icon + xd_tpx(theme, 5);
    shadowed_text(theme, cairo,
                  (xd_rect){tx, content_rect.y + dy, rect.x + rect.w - tx, content_rect.h}, "start",
                  ts, SB, nullptr, st);
    return true;
}

bool xd_pkgstyle_draw_task_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state, const char *label, cairo_surface_t *icon,
                                  int count) {
    if (theme->pkg == nullptr)
        return false;
    const char *cls = (state & XD_STATE_VERTICAL) ? "TaskBandVert::Toolbar" : "TaskBand::Toolbar";
    if (!xd_pkg_has_own(theme->pkg, cls, "Button"))
        cls = "TaskBand::Toolbar";
    if (!xd_pkg_has_own(theme->pkg, cls, "Button"))
        return false;
    /* Toolbar buttons' states: the active window's is Checked. */
    char st[8];
    state_str(st, toolbar_state(state));
    bool flash = (state & XD_STATE_FLASH) && !(state & XD_STATE_CHECKED);
    if (!flash || !xd_pkg_draw(theme, cairo, "TaskBand", "FlashButton", "1", rect))
        (void)xd_pkg_draw(theme, cairo, cls, "Button", st, rect);
    int isz = xd_style_task_icon_size(theme, icon);
    int ix = label != nullptr ? rect.x + xd_tpx(theme, 10) : rect.x + (rect.w - isz) / 2;
    xd_style_task_icon(theme, cairo, icon, ix, rect.y + (rect.h - isz) / 2);
    if (label == nullptr)
        return true;
    xd_font_spec font = theme->scheme.fonts[XD_FONT_MENU];
    (void)xd_pkg_font(theme->pkg, cls, "Button", st, "Font", &font, nullptr);
    xd_text_style ts =
        xd_text(theme, XD_FONT_MENU, text_color_st(theme, cls, "Button", st, xd_rgba_hex(0xFFFFFF)),
                XD_ALIGN_START);
    ts.font = &font;
    int tx = ix + isz + xd_tpx(theme, 4);
    int end = rect.x + rect.w - xd_tpx(theme, 6);
    char *text = nullptr;
    xd_font_spec cf = font;
    if (count > 1 && xd_pkg_has(theme->pkg, "TaskBand", "GroupCount")) {
        /* The count in GroupCount's font and colour, then the name. */
        char count_text[16];
        g_snprintf(count_text, sizeof(count_text), "%d", count);
        cf.bold = true;
        (void)xd_pkg_font(theme->pkg, "TaskBand", "GroupCount", nullptr, "Font", &cf, nullptr);
        xd_text_style cs = ts;
        cs.font = &cf;
        cs.color = text_color_st(theme, "TaskBand", "GroupCount", nullptr, ts.color);
        xd_draw_text(cairo, (xd_rect){tx, rect.y, end - tx, rect.h}, count_text, &cs);
        tx += xd_text_width(cairo, count_text, &cs) + xd_tpx(theme, 4);
    } else if (count > 1) {
        text = g_strdup_printf("%d %s", count, label);
        label = text;
    }
    xd_draw_text(cairo, (xd_rect){tx, rect.y, end - tx, rect.h}, label, &ts);
    g_free(text);
    return true;
}

void xd_theme_draw_task_group(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                              int count, const char *label, cairo_surface_t *icon) {
    if (xd_pkgstyle_draw_task_button(theme, cairo, rect, state, label, icon, count))
        return;
    char *text = g_strdup_printf("%d %s", count, label);
    xd_theme_draw_task_button(theme, cairo, rect, state, text, icon);
    g_free(text);
}

/* 7's program buttons: TaskBand2's AppButton, its states Normal 1, Hot 2,
 * Pressed 3 for a window in the background, 4-6 for the active one's,
 * Flashing 7 (deduced); the edges of a group's windows stacked behind it
 * to its right, 1-3 for none, one or two (AppButtonStack, Lit while
 * active or hot). */
static const char *const TB2 = "TaskBand2";

bool xd_pkgstyle_draw_app_button(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                 const xd_app_button *ab) {
    if (theme->pkg == nullptr || !xd_pkg_has_own(theme->pkg, TB2, "AppButton"))
        return false;
    const char *comp = "TaskBand2Composited::TaskBand2";
    bool active = state & XD_STATE_CHECKED, pressed = state & XD_STATE_PRESSED;
    bool hot = state & XD_STATE_HOT, flash = (state & XD_STATE_FLASH) && !active;
    int stack = CLAMP(ab->windows - 1, 0, 2);
    xd_rect button = xd_rect_inset(rect, xd_tpx(theme, 1), xd_tpx(theme, 1));
    if (ab->windows > 0 || hot || pressed || flash) {
        const char *edges = (active || hot) && xd_pkg_has_own(theme->pkg, TB2, "AppButtonStackLit")
                                ? "AppButtonStackLit"
                                : "AppButtonStack";
        if (stack > 0 && xd_pkg_has_own(theme->pkg, TB2, edges)) {
            button.w -= xd_tpx(theme, stack == 1 ? 5 : 8);
            char es[8];
            state_str(es, stack + 1);
            (void)xd_pkg_draw(theme, cairo, pick(theme, TB2, comp, edges), edges, es,
                              (xd_rect){button.x + button.w, button.y, xd_tpx(theme, 8), button.h});
        }
        char st[8];
        state_str(st, flash    ? 7
                      : active ? (pressed ? 6 : hot ? 5 : 4)
                               : (pressed ? 3 : hot ? 2 : 1));
        (void)xd_pkg_draw(theme, cairo, pick(theme, TB2, comp, "AppButton"), "AppButton", st,
                          button);
        if (hot && !flash && ab->glow_x >= 0) {
            /* The icon's colour glowing up from under the pointer. */
            cairo_save(cairo);
            cairo_rectangle(cairo, button.x, button.y, button.w, button.h);
            cairo_clip(cairo);
            double gx = CLAMP(ab->glow_x, button.x, button.x + button.w), gy = button.y + button.h;
            cairo_pattern_t *pattern =
                cairo_pattern_create_radial(gx, gy, 0, gx, gy, button.w * 0.85);
            cairo_pattern_add_color_stop_rgba(pattern, 0, ab->tint.r, ab->tint.g, ab->tint.b,
                                              pressed ? 0.45 : 0.65);
            cairo_pattern_add_color_stop_rgba(pattern, 1, ab->tint.r, ab->tint.g, ab->tint.b, 0);
            cairo_set_source(cairo, pattern);
            cairo_paint(cairo);
            cairo_pattern_destroy(pattern);
            cairo_restore(cairo);
        }
    }
    if (ab->progress != XD_PROGRESS_NONE)
        xd_theme_draw_taskbar_progress(theme, cairo, button, ab->progress, ab->fraction);
    if (ab->icon == nullptr)
        return true;
    double sx = 1, sy = 1;
    cairo_surface_get_device_scale(ab->icon, &sx, &sy);
    int iw = (int)(cairo_image_surface_get_width(ab->icon) / sx);
    int ih = (int)(cairo_image_surface_get_height(ab->icon) / sy);
    int ix = ab->label != nullptr ? button.x + xd_tpx(theme, 6) : button.x + (button.w - iw) / 2;
    cairo_set_source_surface(cairo, ab->icon, ix, button.y + (button.h - ih) / 2);
    cairo_paint(cairo);
    if (ab->label != nullptr) {
        xd_font_spec font = theme->scheme.fonts[XD_FONT_MENU];
        (void)xd_pkg_font(theme->pkg, TB2, nullptr, nullptr, "Font", &font, nullptr);
        xd_text_style ts = xd_text(
            theme, XD_FONT_MENU, text_color_st(theme, TB2, nullptr, nullptr, xd_rgba_hex(0xFFFFFF)),
            XD_ALIGN_START);
        ts.font = &font;
        ts.shadow = true;
        ts.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
        int tx = ix + iw + xd_tpx(theme, 6);
        xd_draw_text(cairo,
                     (xd_rect){tx, button.y, button.x + button.w - tx - xd_tpx(theme, 4), button.h},
                     ab->label, &ts);
    }
    return true;
}

void xd_theme_draw_taskbar_progress(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                    enum xd_taskbar_progress kind, double fraction) {
    /* TaskBand2's bars (deduced); without them, 7's colours. */
    static const struct {
        const char *part;
        uint32_t rgb;
    } kinds[] = {
        [XD_PROGRESS_NORMAL] = {"ProgressNormal", 0x06B025},
        [XD_PROGRESS_PAUSED] = {"ProgressPaused", 0xFFC800},
        [XD_PROGRESS_ERROR] = {"ProgressError", 0xDA2626},
        [XD_PROGRESS_INDETERMINATE] = {"ProgressPulse", 0x06B025},
    };
    if (kind == XD_PROGRESS_NONE || (size_t)kind >= G_N_ELEMENTS(kinds) || rect.w <= 0)
        return;
    fraction = CLAMP(fraction, 0.0, 1.0);
    xd_rect filled = {rect.x, rect.y, (int)lround(rect.w * fraction), rect.h};
    if (kind == XD_PROGRESS_INDETERMINATE) {
        /* A third of the button wide, sweeping in from the left. */
        int pw = MAX(rect.w / 3, 1);
        filled = (xd_rect){rect.x - pw + (int)lround((rect.w + pw) * fraction), rect.y, pw, rect.h};
    }
    if (filled.w <= 0)
        return;
    cairo_save(cairo);
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_clip(cairo);
    if (theme->pkg == nullptr || !xd_pkg_has_own(theme->pkg, TB2, kinds[kind].part) ||
        !xd_pkg_draw(theme, cairo, TB2, kinds[kind].part, "1", filled)) {
        xd_rgba color = xd_rgba_hex(kinds[kind].rgb);
        xd_fill_gradient(cairo, filled, xd_rgba_alpha(color, 0.7), xd_rgba_alpha(color, 0.45),
                         false);
    }
    cairo_restore(cairo);
}

/* ---- the notification area ---- */

bool xd_pkgstyle_clock_color(const xd_theme *theme, xd_rgba *out) {
    return theme->pkg != nullptr && xd_pkg_has_own(theme->pkg, "TrayNotify::Clock", nullptr) &&
           xd_pkg_color(theme->pkg, "TrayNotify::Clock", nullptr, nullptr, "TextColor", out);
}

bool xd_pkgstyle_draw_tray(const xd_theme *theme, cairo_t *cairo, xd_rect rect, const char *clock,
                           xd_edge edge) {
    bool vert = edge == XD_EDGE_LEFT || edge == XD_EDGE_RIGHT;
    const char *plain = vert ? "TrayNotifyVert::TrayNotify" : "TrayNotifyHoriz::TrayNotify";
    if (theme->pkg == nullptr || !xd_pkg_has_own(theme->pkg, plain, "Background"))
        return false;
    const char *comp =
        vert ? "TrayNotifyVertComposited::TrayNotify" : "TrayNotifyHorizComposited::TrayNotify";
    (void)xd_pkg_draw(theme, cairo, pick(theme, plain, comp, "Background"), "Background", "1",
                      rect);
    if (clock == nullptr || clock[0] == '\0')
        return true;
    xd_font_spec font = theme->scheme.fonts[XD_FONT_MENU];
    (void)xd_pkg_font(theme->pkg, "TrayNotify::Clock", nullptr, nullptr, "Font", &font, nullptr);
    xd_text_style ts = xd_text(theme, XD_FONT_MENU, xd_theme_clock_color(theme), XD_ALIGN_END);
    ts.font = &font;
    ts.lines = true; /* a tall taskbar's day and date under the time */
    ts.shadow = seven(theme);
    ts.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    int ml, mr;
    xd_theme_tray_margins(theme, &ml, &mr);
    xd_draw_text(cairo, xd_rect_inset4(rect, ml, 0, mr, 0), clock, &ts);
    return true;
}

bool xd_theme_draw_tray_chevron(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                bool open, bool vertical) {
    if (theme->pkg == nullptr)
        return false;
    char plain[48], comp[64];
    g_snprintf(plain, sizeof(plain), "TrayNotify%s%s::Button", vertical ? "Vert" : "Horiz",
               open ? "Open" : "");
    g_snprintf(comp, sizeof(comp), "TrayNotify%s%sComposited::Button",
               vertical ? "Vert" : "Horiz", open ? "Open" : "");
    if (!xd_pkg_has_own(theme->pkg, plain, nullptr))
        return false;
    char st[8];
    basic_str(st, state);
    (void)xd_pkg_draw(theme, cairo, pick(theme, plain, comp, nullptr), nullptr, st, rect);
    return true;
}

bool xd_theme_draw_show_desktop(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint8_t state,
                                bool vertical) {
    const char *cls = vertical ? "VerticalShowDesktop::Button" : "ShowDesktop::Button";
    if (theme->pkg == nullptr || !xd_pkg_has_own(theme->pkg, cls, nullptr))
        return false;
    char st[8];
    basic_str(st, state);
    (void)xd_pkg_draw(theme, cairo, cls, nullptr, st, rect);
    return true;
}

/* ---- Alt+Tab: AltTab (deduced names) ---- */

static const char *const ALTTAB = "AltTab";

bool xd_pkgstyle_has_switcher(const xd_theme *theme) {
    return theme->pkg != nullptr && xd_pkg_has_own(theme->pkg, ALTTAB, "SelectedItem");
}

void xd_pkgstyle_draw_switcher_box(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    /* The class's own image: 7's box without glass (with it, 7 shows live
     * thumbnails on the glass; we show icons, as without). */
    if (!xd_pkg_draw(theme, cairo, ALTTAB, nullptr, nullptr, rect))
        xd_fill_rect(cairo, rect, xd_rgba_from_rgb(theme->scheme.colors[XD_COLOR_BUTTON_FACE]));
}

void xd_pkgstyle_draw_switcher_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                    bool selected, bool hot) {
    const char *part = selected && hot ? "HotSelectedItem"
                       : selected      ? "SelectedItem"
                       : hot           ? "HotItem"
                                       : nullptr;
    if (part != nullptr)
        (void)xd_pkg_draw(theme, cairo, ALTTAB, part, nullptr, rect);
}

void xd_pkgstyle_draw_switcher_title(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                     const char *title) {
    if (title == nullptr || *title == '\0' || rect.w <= 0)
        return;
    xd_rgba fg = xd_rgba_hex(0x000000);
    (void)xd_pkg_color(theme->pkg, ALTTAB, "Title", nullptr, "TextColor", &fg);
    xd_text_style ts = xd_text(theme, XD_FONT_CAPTION, fg, XD_ALIGN_START);
    xd_font_spec font;
    if (xd_pkg_font(theme->pkg, ALTTAB, "Title", nullptr, "Font", &font, nullptr))
        ts.font = &font;
    ts.ellipsize = true;
    /* On glass, 7 sets the title on a white glow (TextGlowSize): TextGlow's
     * image round the text. */
    int glow = 0;
    if (xd_pkg_int(theme->pkg, ALTTAB, "Title", nullptr, "TextGlowSize", &glow) && glow > 0 &&
        xd_pkg_has(theme->pkg, "TextGlow", "Glow")) {
        PangoLayout *layout = xd_text_layout(cairo, title, &ts);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(layout, &tw, &th);
        g_object_unref(layout);
        int glow_margin = xd_tpx(theme, glow) / 2;
        tw = MIN(tw, rect.w);
        (void)xd_pkg_draw(theme, cairo, "TextGlow", "Glow", nullptr,
                          (xd_rect){rect.x - glow_margin, rect.y + (rect.h - th) / 2 - glow_margin,
                                    tw + 2 * glow_margin, th + 2 * glow_margin});
    }
    xd_draw_text(cairo, rect, title, &ts);
}

/* ---- 7's thumbnails: TaskbandExtendedUI (deduced names) ---- */

static const char *const TEU = "TaskbandExtendedUI";

bool xd_pkgstyle_draw_thumb_panel(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    return theme->pkg != nullptr && xd_pkg_has_own(theme->pkg, TEU, "ThumbnailBackground") &&
           xd_pkg_draw(theme, cairo, TEU, "ThumbnailBackground", "1", rect);
}

bool xd_pkgstyle_draw_thumb_tile(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                 uint8_t state) {
    if (theme->pkg == nullptr || !xd_pkg_has_own(theme->pkg, TEU, "ThumbnailTile"))
        return false;
    /* Hot 1, Pressed 2 (deduced from the frames' look). */
    if (state & (XD_STATE_HOT | XD_STATE_PRESSED))
        (void)xd_pkg_draw(theme, cairo, TEU, "ThumbnailTile",
                          (state & XD_STATE_PRESSED) ? "2" : "1", rect);
    return true;
}

bool xd_pkgstyle_thumb_title_color(const xd_theme *theme, xd_rgba *out) {
    return theme->pkg != nullptr && xd_pkg_has_own(theme->pkg, TEU, "ThumbnailTitle") &&
           xd_pkg_color(theme->pkg, TEU, "ThumbnailTitle", nullptr, "TextColor", out);
}

bool xd_pkgstyle_draw_thumb_close(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                  uint8_t state) {
    if (theme->pkg == nullptr || !xd_pkg_has_own(theme->pkg, TEU, "ThumbnailClose"))
        return false;
    char st[8];
    basic_str(st, state);
    return xd_pkg_draw(theme, cairo, TEU, "ThumbnailClose", st, rect);
}

/* ---- jump lists: our JumpList class, else 7's Start menu items ---- */

static const char *const JL = "JumpList";

bool xd_pkgstyle_draw_jumplist_frame(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                     int band_y) {
    if (theme->pkg == nullptr || !xd_pkg_has_own(theme->pkg, JL, "Background"))
        return false;
    (void)xd_pkg_draw(theme, cairo, JL, "Background", "1", rect);
    if (band_y < rect.y + rect.h)
        (void)xd_pkg_draw(theme, cairo, JL, "Band", "1",
                          (xd_rect){rect.x, band_y, rect.w, rect.y + rect.h - band_y});
    return true;
}

bool xd_pkgstyle_jumplist_header_color(const xd_theme *theme, xd_rgba *out) {
    return theme->pkg != nullptr && xd_pkg_has_own(theme->pkg, JL, "Header") &&
           xd_pkg_color(theme->pkg, JL, "Header", nullptr, "TextColor", out);
}

/* A hot jump list item or pin button: JumpList's, else what 7's Start menu
 * highlights its programs with (its jump lists are the same control). */
static bool jump_hot(const xd_theme *theme, cairo_t *cairo, const char *part, xd_rect rect,
                     bool pressed) {
    if (xd_pkg_has_own(theme->pkg, JL, part))
        return xd_pkg_draw(theme, cairo, JL, part, pressed ? "3" : "2", rect);
    return xd_theme_draw_start_part(theme, cairo, XD_START_PROG_ITEM, rect,
                                    pressed ? XD_STATE_PRESSED : XD_STATE_HOT);
}

bool xd_pkgstyle_draw_jumplist_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                    uint8_t state, xd_rgba *text) {
    if (theme->pkg == nullptr)
        return false;
    int width, height;
    bool own = xd_pkg_has_own(theme->pkg, JL, "Item");
    if (!own && !xd_theme_start_part_size(theme, XD_START_PROG_ITEM, XD_STATE_HOT, &width, &height))
        return false;
    bool disabled = state & XD_STATE_DISABLED;
    bool hot = (state & XD_STATE_HOT) && !disabled;
    if (hot)
        (void)jump_hot(theme, cairo, "Item", rect, false);
    /* Item: Normal 1, Hot 2, Disabled 3. */
    const char *st = disabled ? "3" : hot ? "2" : "1";
    if (!own || !xd_pkg_color(theme->pkg, JL, "Item", st, "TextColor", text))
        *text = xd_rgba_hex(disabled ? 0x6D6D6D : 0x1E1E1E);
    return true;
}

bool xd_pkgstyle_draw_jumplist_pin(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                   uint8_t state) {
    return theme->pkg != nullptr &&
           jump_hot(theme, cairo, "PinButton", rect, state & XD_STATE_PRESSED);
}

/* ---- the XP and 7 Start menus ---- */

static const char *const start_names[XD_START_PART_COUNT] = {
    [XD_START_USER_PANE] = "UserPane",
    [XD_START_USER_PICTURE] = "UserPicture",
    [XD_START_PROG_LIST] = "ProgList",
    [XD_START_PROG_SEPARATOR] = "ProgListSeparator",
    [XD_START_PLACES_LIST] = "PlacesList",
    [XD_START_PLACES_SEPARATOR] = "PlacesListSeparator",
    [XD_START_MORE_PROGRAMS] = "MorePrograms",
    [XD_START_MORE_PROGRAMS_ARROW] = "MoreProgramsArrow",
    [XD_START_MORE_PROGRAMS_BACK] = "MoreProgramsArrowBack",
    [XD_START_LOGOFF] = "Logoff",
    [XD_START_LOGOFF_BUTTONS] = "LogoffButtons",
    [XD_START_PROG_ITEM] = "ProgListItem",
    [XD_START_PROG_ITEM_LEFT] = "ProgListItemLeft",
    [XD_START_PROG_ITEM_SPLIT] = "ProgListItemSplit",
    [XD_START_PLACES_ITEM] = "PlacesListItem",
    [XD_START_SHUTDOWN] = "ShutdownButton",
    [XD_START_SHUTDOWN_ARROW] = "LogoffSplitButtonDropdown",
    [XD_START_OPEN_BOX] = "OpenBox",
};

/* The class drawing a part: 7's StartPanelPriv, then StartPanel (XP's
 * and Vista's); their composited variants with the glass. nullptr when
 * the package has neither. */
static const char *start_class(const xd_theme *theme, xd_start_part part) {
    if (theme->pkg == nullptr || (unsigned)part >= XD_START_PART_COUNT)
        return nullptr;
    const char *name = start_names[part];
    if (seven(theme) && xd_pkg_has_own(theme->pkg, "StartPanelPriv", name))
        return pick(theme, "StartPanelPriv", "StartPanelComposited::StartPanelPriv", name);
    if (xd_pkg_has_own(theme->pkg, "StartPanel", name))
        return pick(theme, "StartPanel", "StartPanelComposited::StartPanel", name);
    return nullptr;
}

bool xd_theme_draw_start_part(const xd_theme *theme, cairo_t *cairo, xd_start_part part,
                              xd_rect rect, uint8_t state) {
    const char *cls = start_class(theme, part);
    if (cls == nullptr)
        return false;
    char st[8];
    basic_str(st, state);
    return xd_pkg_draw(theme, cairo, cls, start_names[part], st, rect);
}

bool xd_theme_start_part_size(const xd_theme *theme, xd_start_part part, uint8_t state, int *width,
                              int *height) {
    const char *cls = start_class(theme, part);
    char st[8];
    basic_str(st, state);
    return cls != nullptr && xd_pkg_part_size(theme, cls, start_names[part], st, width, height);
}

bool xd_theme_start_part_margins(const xd_theme *theme, xd_start_part part, xd_insets *out) {
    const char *cls = start_class(theme, part);
    if (cls == nullptr)
        return false;
    *out = (xd_insets){0};
    (void)xd_pkg_margins(theme->pkg, cls, start_names[part], nullptr, "ContentMargins", out);
    return true;
}

bool xd_theme_draw_start_logoff_icon(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                     bool power, uint8_t state) {
    const char *cls = start_class(theme, XD_START_LOGOFF_BUTTONS);
    char st[8];
    basic_str(st, state);
    int width, height;
    if (cls == nullptr || !xd_pkg_part_size(theme, cls, "LogoffButtons", st, &width, &height) ||
        height <= 0)
        return false;
    int squares = MAX(width / height, 1);
    int i = MAX(power ? squares - 1 : squares - 2, 0);
    xd_rect icon = {rect.x + (rect.w - height) / 2, rect.y + (rect.h - height) / 2, height, height};
    cairo_save(cairo);
    cairo_rectangle(cairo, icon.x, icon.y, icon.w, icon.h);
    cairo_clip(cairo);
    bool drew = xd_pkg_draw(theme, cairo, cls, "LogoffButtons", st,
                            (xd_rect){icon.x - i * height, icon.y, width, height});
    cairo_restore(cairo);
    return drew;
}

bool xd_theme_start_part_color(const xd_theme *theme, xd_start_part part, uint8_t state,
                               xd_start_color which, xd_rgba *out) {
    static const char *const props[] = {
        [XD_START_TEXT] = "TextColor",
        [XD_START_SUBTEXT] = "CaptionText",
        [XD_START_SHADOW] = "TextShadowColor",
    };
    const char *cls = start_class(theme, part);
    if (cls == nullptr || (unsigned)which >= G_N_ELEMENTS(props))
        return false;
    char st[8];
    basic_str(st, state);
    return xd_pkg_color(theme->pkg, cls, start_names[part], st, props[which], out);
}

bool xd_theme_start_part_font(const xd_theme *theme, xd_start_part part, xd_font_spec *out) {
    const char *cls = start_class(theme, part);
    return cls != nullptr &&
           xd_pkg_font(theme->pkg, cls, start_names[part], nullptr, "Font", out, nullptr);
}

bool xd_theme_start_heading_color(const xd_theme *theme, xd_rgba *out) {
    return theme->pkg != nullptr &&
           xd_pkg_has_own(theme->pkg, "StartMenu::ListView", "GroupHeader") &&
           xd_pkg_color(theme->pkg, "StartMenu::ListView", "GroupHeader", nullptr,
                        "Heading1TextColor", out);
}

bool xd_theme_draw_new_program(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    return theme->pkg != nullptr &&
           xd_pkg_has_own(theme->pkg, "StartMenu::MenuBand", "NewAppButton") &&
           xd_pkg_draw(theme, cairo, "StartMenu::MenuBand", "NewAppButton", "1", rect);
}

/* ---- the Start menus' cascades ----
 * XP's and 7's: StartMenu::Toolbar's background and Button, ::MenuBand's
 * separator ("Seperator", as XP spells it). The Windows 2000 Start menu:
 * our ClassicStartMenu (Background, Banner, Item, Separator,
 * SubmenuArrow; docs/THEMES.md). */

static const char *const SMT = "StartMenu::Toolbar";
static const char *const CSM = "ClassicStartMenu";

static bool start_look(const xd_theme *theme) {
    return theme->pkg != nullptr && theme->menu_look == XD_MENU_LOOK_START;
}

static bool classic_look(const xd_theme *theme) {
    return theme->pkg != nullptr && theme->menu_look == XD_MENU_LOOK_CLASSIC_START;
}

bool xd_pkgstyle_draw_start_menu_frame(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (start_look(theme) && xd_pkg_has_own(theme->pkg, SMT, nullptr) &&
        xd_pkg_prop(theme->pkg, SMT, nullptr, nullptr, "ImageFile") != nullptr) {
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_MENU));
        return xd_pkg_draw(theme, cairo, SMT, nullptr, "1", rect);
    }
    if (classic_look(theme) && xd_pkg_has_own(theme->pkg, CSM, "Background")) {
        xd_fill_rect(cairo, rect, xd_sys(theme, XD_COLOR_MENU));
        return xd_pkg_draw(theme, cairo, CSM, "Background", "1", rect);
    }
    return false;
}

bool xd_pkgstyle_draw_start_menu_item(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                      uint8_t state, xd_rgba *text) {
    bool hot = state & XD_STATE_HOT, disabled = state & XD_STATE_DISABLED;
    if (start_look(theme) && xd_pkg_has_own(theme->pkg, SMT, "Button")) {
        /* Toolbar buttons: Hot 2; the text Toolbar(Hot)'s. */
        if (hot)
            (void)xd_pkg_draw(theme, cairo, SMT, "Button", "2", rect);
        *text =
            disabled
                ? xd_sys(theme, XD_COLOR_GRAY_TEXT)
                : text_color_st(theme, SMT, nullptr, hot ? "2" : "1",
                                xd_sys(theme, hot ? XD_COLOR_HILIGHT_TEXT : XD_COLOR_MENU_TEXT));
        return true;
    }
    if (classic_look(theme) && xd_pkg_has_own(theme->pkg, CSM, "Item")) {
        /* Normal 1, Hot 2, Disabled 3, DisabledHot 4, as Menu's PopupItem. */
        char st[8];
        state_str(st, disabled ? (hot ? 4 : 3) : (hot ? 2 : 1));
        (void)xd_pkg_draw(theme, cairo, CSM, "Item", st, rect);
        *text = text_color_st(theme, CSM, "Item", st,
                              xd_sys(theme, disabled ? XD_COLOR_GRAY_TEXT
                                            : hot    ? XD_COLOR_HILIGHT_TEXT
                                                     : XD_COLOR_MENU_TEXT));
        return true;
    }
    return false;
}

bool xd_pkgstyle_draw_start_menu_separator(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    if (start_look(theme)) {
        static const char *const parts[] = {"Seperator", "Separator"};
        for (size_t i = 0; i < G_N_ELEMENTS(parts); i++)
            if (xd_pkg_has_own(theme->pkg, "StartMenu::MenuBand", parts[i]))
                return xd_pkg_draw(theme, cairo, "StartMenu::MenuBand", parts[i], "1", rect);
    }
    return classic_look(theme) && xd_pkg_has_own(theme->pkg, CSM, "Separator") &&
           xd_pkg_draw(theme, cairo, CSM, "Separator", "1", rect);
}

bool xd_pkgstyle_draw_start_menu_submenu(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                         uint8_t state) {
    if (!classic_look(theme) || !xd_pkg_has_own(theme->pkg, CSM, "SubmenuArrow"))
        return false;
    /* Normal 1, Hot 2, Disabled 3. */
    const char *st = (state & XD_STATE_DISABLED) ? "3" : (state & XD_STATE_HOT) ? "2" : "1";
    return xd_pkg_draw(theme, cairo, CSM, "SubmenuArrow", st, rect);
}

bool xd_pkgstyle_draw_start_banner(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                   xd_rgba *text, xd_font_spec *font) {
    if (!classic_look(theme) || !xd_pkg_has_own(theme->pkg, CSM, "Banner"))
        return false;
    (void)xd_pkg_draw(theme, cairo, CSM, "Banner", "1", rect);
    (void)xd_pkg_color(theme->pkg, CSM, "Banner", nullptr, "TextColor", text);
    (void)xd_pkg_font(theme->pkg, CSM, "Banner", nullptr, "Font", font, nullptr);
    return true;
}

/* ---- Explorer's panes ------------------------------------------------------------ *
 * XP's task pane from ExplorerBar (with what an import of shellstyle.dll
 * adds: shell/NAME.xml), and Vista's and 7's command bar, address box,
 * tree items and details pane. See panes.h; panes.c draws without a
 * package. */

static const char *const EXPLORERBAR = "ExplorerBar";

bool xd_pkgstyle_has_taskpane(const xd_theme *theme) {
    return theme->pkg != nullptr && xd_pkg_has(theme->pkg, EXPLORERBAR, "NormalGroupHead");
}

static int scaled_px(const xd_theme *theme, int value) { return (int)lround(value * theme->scale); }

static xd_insets scaled_insets(const xd_theme *theme, xd_insets insets) {
    return (xd_insets){scaled_px(theme, insets.left), scaled_px(theme, insets.top),
                       scaled_px(theme, insets.right), scaled_px(theme, insets.bottom)};
}

/* A BorderFill as DirectUI draws its boxes: a fill or a gradient, and a
 * border whose sides may differ (BorderThickness, our own: uxtheme's
 * BorderSize is the same all round). Anything else, as uxtheme draws it. */
static bool draw_box(const xd_theme *theme, cairo_t *cairo, const char *cls, const char *part,
                     const char *state, xd_rect rect) {
    const xd_pkg *package = theme->pkg;
    const char *bg = xd_pkg_prop(package, cls, part, state, "BgType");
    if (bg == nullptr || g_ascii_strcasecmp(bg, "BorderFill") != 0)
        return xd_pkg_draw(theme, cairo, cls, part, state, rect);
    xd_rgba fill, g1, g2;
    const char *type = xd_pkg_prop(package, cls, part, state, "FillType");
    bool gradient = type != nullptr && g_ascii_strcasecmp(type, "Solid") != 0 &&
                    xd_pkg_color(package, cls, part, state, "GradientColor1", &g1) &&
                    xd_pkg_color(package, cls, part, state, "GradientColor2", &g2);
    if (gradient)
        xd_fill_gradient(cairo, rect, g1, g2, g_ascii_strcasecmp(type, "HorzGradient") == 0);
    else if (xd_pkg_color(package, cls, part, state, "FillColor", &fill))
        xd_fill_rect(cairo, rect, fill);
    xd_rgba bc;
    if (!xd_pkg_color(package, cls, part, state, "BorderColor", &bc))
        return true;
    xd_insets insets;
    int size = 0;
    if (xd_pkg_margins(package, cls, part, state, "BorderThickness", &insets)) {
        insets = scaled_insets(theme, insets);
    } else if (xd_pkg_int(package, cls, part, state, "BorderSize", &size) && size > 0) {
        int px = xd_tpx(theme, size);
        insets = (xd_insets){px, px, px, px};
    } else {
        return true;
    }
    if (insets.left > 0)
        xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, insets.left, rect.h}, bc);
    if (insets.right > 0)
        xd_fill_rect(cairo, (xd_rect){rect.x + rect.w - insets.right, rect.y, insets.right, rect.h},
                     bc);
    if (insets.top > 0)
        xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, rect.w, insets.top}, bc);
    if (insets.bottom > 0)
        xd_fill_rect(cairo,
                     (xd_rect){rect.x, rect.y + rect.h - insets.bottom, rect.w, insets.bottom}, bc);
    return true;
}

static void group_part(char *out, size_t cap, bool special, const char *what) {
    g_snprintf(out, cap, "%sGroup%s", special ? "Special" : "Normal", what);
}

static int font_px(const xd_theme *theme, const xd_font_spec *font) {
    /* A line of text: the font's em at 96 DPI and its leading. */
    double px = ceil(font->size_pt * 96.0 / 72.0 * 1.2 * theme->scale);
    return (int)px;
}

void xd_pkgstyle_taskpane_style(const xd_theme *theme, xd_taskpane_style *taskpane_style) {
    const xd_pkg *package = theme->pkg;
    if (!xd_pkgstyle_has_taskpane(theme))
        return;
    xd_insets insets;
    int spacing;
    if (xd_pkg_margins(package, EXPLORERBAR, nullptr, nullptr, "ContentMargins", &insets))
        taskpane_style->padding = scaled_insets(theme, insets);
    if (xd_pkg_int(package, EXPLORERBAR, nullptr, nullptr, "GroupSpacing", &spacing))
        taskpane_style->group_spacing = scaled_px(theme, spacing);
    for (int sp = 0; sp < 2; sp++) {
        char head[40], body[40];
        group_part(head, sizeof(head), sp, "Head");
        group_part(body, sizeof(body), sp, "Background");
        if (xd_pkg_margins(package, EXPLORERBAR, body, nullptr, "ContentMargins", &insets))
            taskpane_style->body[sp] = scaled_insets(theme, insets);
        if (xd_pkg_int(package, EXPLORERBAR, body, nullptr, "LinkSpacing", &spacing))
            taskpane_style->link_spacing[sp] = scaled_px(theme, spacing);
        if (xd_pkg_int(package, EXPLORERBAR, body, nullptr, "IconSpacing", &spacing))
            taskpane_style->icon_gap[sp] = scaled_px(theme, spacing);
        (void)xd_pkg_font(package, EXPLORERBAR, head, nullptr, "Font",
                          &taskpane_style->header_font[sp], nullptr);
        for (int hot = 0; hot < 2; hot++) {
            const char *st = hot ? "Hot" : nullptr;
            (void)xd_pkg_color(package, EXPLORERBAR, head, st, "TextColor",
                               &taskpane_style->header_color[sp][hot]);
            (void)xd_pkg_color(package, EXPLORERBAR, body, st, "TextColor",
                               &taskpane_style->link_color[sp][hot]);
            (void)xd_pkg_font(package, EXPLORERBAR, body, st, "Font",
                              &taskpane_style->link_font[sp][hot],
                              &taskpane_style->link_underline[sp][hot]);
        }
    }
    /* The title bar: as tall as its image, its arrow or its title with
     * its margins, whichever is taller. */
    int height = 0, width, ih;
    if (xd_pkg_part_size(theme, EXPLORERBAR, "NormalGroupHead", nullptr, &width, &ih))
        height = ih;
    xd_insets hm = {0};
    if (xd_pkg_margins(package, EXPLORERBAR, "NormalGroupHead", nullptr, "ContentMargins", &hm))
        hm = scaled_insets(theme, hm);
    int vm = hm.top + hm.bottom;
    if (xd_pkg_part_size(theme, EXPLORERBAR, "NormalGroupExpand", "1", &width, &ih))
        height = MAX(height, ih + vm);
    height = MAX(height, font_px(theme, &taskpane_style->header_font[0]) + vm);
    if (height > 0)
        taskpane_style->header_height = height;
    (void)xd_pkg_font(package, "TaskPane", "Details", nullptr, "Font",
                      &taskpane_style->details_font, nullptr);
    (void)xd_pkg_font(package, "TaskPane", "DetailsName", nullptr, "Font",
                      &taskpane_style->details_name_font, nullptr);
    (void)xd_pkg_color(package, "TaskPane", "Details", nullptr, "TextColor",
                       &taskpane_style->details_color);
    if (xd_pkg_int(package, "TaskPane", "Details", nullptr, "LinkSpacing", &spacing))
        taskpane_style->details_spacing = scaled_px(theme, spacing);
}

bool xd_pkgstyle_draw_taskpane(const xd_theme *theme, cairo_t *cairo, xd_rect rect) {
    return xd_pkgstyle_has_taskpane(theme) &&
           draw_box(theme, cairo, EXPLORERBAR, nullptr, nullptr, rect);
}

bool xd_pkgstyle_draw_taskpane_header(const xd_theme *theme, cairo_t *cairo,
                                      const xd_taskpane_style *taskpane_style, xd_rect rect,
                                      bool special, bool expanded, uint8_t state,
                                      const char *title) {
    if (!xd_pkgstyle_has_taskpane(theme))
        return false;
    char head[40], arrow[40];
    group_part(head, sizeof(head), special, "Head");
    group_part(arrow, sizeof(arrow), special, expanded ? "Collapse" : "Expand");
    (void)draw_box(theme, cairo, EXPLORERBAR, head, nullptr, rect);
    xd_rect content_rect = xd_pkg_content_rect(theme, EXPLORERBAR, head, nullptr, rect);
    bool hot = state & (XD_STATE_HOT | XD_STATE_PRESSED);
    const char *st = (state & XD_STATE_PRESSED) ? "3" : hot ? "2" : "1";
    int aw = 0, ah = 0;
    if (xd_pkg_part_size(theme, EXPLORERBAR, arrow, st, &aw, &ah)) {
        /* Right-aligned in the content, or where an Offset from the top
         * right corner puts it (7's): its left edge or its right edge. */
        xd_rect arrow_rect = {content_rect.x + content_rect.w - aw,
                              content_rect.y + (content_rect.h - ah) / 2, aw, ah};
        int off[2];
        const char *offset_text = xd_pkg_prop(theme->pkg, EXPLORERBAR, arrow, st, "Offset");
        if (offset_text != nullptr && sscanf(offset_text, "%d %d", &off[0], &off[1]) == 2) {
            int ox = scaled_px(theme, off[0]);
            arrow_rect.x = rect.x + rect.w + ox - (ox > -aw ? aw : 0);
            arrow_rect.y = rect.y + scaled_px(theme, off[1]);
        }
        (void)xd_pkg_draw(theme, cairo, EXPLORERBAR, arrow, st, arrow_rect);
        content_rect.w = arrow_rect.x - content_rect.x - xd_tpx(theme, 2);
    }
    if (title != nullptr && content_rect.w > 0) {
        xd_text_style ts = xd_text(theme, XD_FONT_MESSAGE,
                                   taskpane_style->header_color[special][hot], XD_ALIGN_START);
        ts.font = &taskpane_style->header_font[special];
        xd_draw_text(cairo, content_rect, title, &ts);
    }
    if (state & XD_STATE_FOCUSED)
        xd_theme_draw_focus_rect(theme, cairo, xd_rect_inset(content_rect, 0, xd_tpx(theme, 2)));
    return true;
}

bool xd_pkgstyle_draw_taskpane_body(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                                    bool special) {
    if (!xd_pkgstyle_has_taskpane(theme))
        return false;
    char body[40];
    group_part(body, sizeof(body), special, "Background");
    (void)draw_box(theme, cairo, EXPLORERBAR, body, nullptr, rect);
    return true;
}

/* Vista's and 7's parts: the class, the part (named in uxtheme.ini's
 * [Deduced ...] sections) and the state's number. */
static bool explorer_part(const xd_theme *theme, xd_explorer_part part, uint8_t state,
                          const char **cls, const char **name, char st[8]) {
    int state_index = 1;
    switch (part) {
    case XD_EXPLORER_COMMAND_BAR:
        *cls = "CommandModule";
        *name = "ModuleBackground";
        break;
    case XD_EXPLORER_COMMAND_BUTTON:
        /* Normal 1, Hot 2, Pressed 3, KeyFocused 4, NearHot 5 (a checked
         * button), Disabled 6 (7's). */
        *cls = "CommandModule";
        *name = "TaskButton";
        state_index = (state & XD_STATE_DISABLED)  ? 6
                      : (state & XD_STATE_PRESSED) ? 3
                      : (state & XD_STATE_HOT)     ? 2
                      : (state & XD_STATE_CHECKED) ? 5
                      : (state & XD_STATE_FOCUSED) ? 4
                                                   : 1;
        break;
    case XD_EXPLORER_ADDRESS_BOX:
        /* Normal, Hot, Disabled, Focused. */
        *cls = "AB::AddressBand";
        *name = "Background";
        state_index = (state & XD_STATE_DISABLED)  ? 3
                      : (state & XD_STATE_FOCUSED) ? 4
                      : (state & XD_STATE_HOT)     ? 2
                                                   : 1;
        break;
    case XD_EXPLORER_TREE_ITEM:
        /* Normal, Hot, Selected, Disabled, SelectedNotFocus, HotSelected. */
        *cls = "Explorer::TreeView";
        *name = "TreeItem";
        state_index = (state & XD_STATE_DISABLED)  ? 4
                      : (state & XD_STATE_CHECKED) ? ((state & XD_STATE_HOT)       ? 6
                                                      : (state & XD_STATE_FOCUSED) ? 3
                                                                                   : 5)
                      : (state & XD_STATE_HOT)     ? 2
                                                   : 1;
        break;
    case XD_EXPLORER_DETAILS_PANE:
        /* Horizontal (under the view), Vertical (beside it). */
        *cls = "PreviewPane";
        *name = "PreviewBackground";
        break;
    default:
        return false;
    }
    g_snprintf(st, 8, "%d", state_index);
    return theme->pkg != nullptr && xd_pkg_has(theme->pkg, *cls, *name);
}

bool xd_pkgstyle_draw_explorer_part(const xd_theme *theme, cairo_t *cairo, xd_explorer_part part,
                                    uint8_t state, xd_rect rect) {
    const char *cls, *name;
    char st[8];
    if (!explorer_part(theme, part, state, &cls, &name, st))
        return false;
    if (part == XD_EXPLORER_COMMAND_BAR) {
        /* 7's bar: a gradient under a translucent sheen (Vista's sheen is
         * the whole bar). */
        xd_rgba color[3];
        if (xd_pkg_color(theme->pkg, cls, "ModuleGradient", nullptr, "GradientColor1", &color[0]) &&
            xd_pkg_color(theme->pkg, cls, "ModuleGradient", nullptr, "GradientColor2", &color[1]) &&
            xd_pkg_color(theme->pkg, cls, "ModuleGradient", nullptr, "GradientColor3", &color[2])) {
            static const double stops[] = {0.0, 0.5, 1.0};
            xd_fill_vstops(cairo, rect, stops, color, 3);
        }
    }
    /* A button or tree row at rest (or a button disabled: 7 has no image
     * for it, only a text colour) shows what's behind it. */
    if ((part == XD_EXPLORER_COMMAND_BUTTON || part == XD_EXPLORER_TREE_ITEM) &&
        (strcmp(st, "1") == 0 || (part == XD_EXPLORER_COMMAND_BUTTON && strcmp(st, "6") == 0)))
        return true;
    (void)xd_pkg_draw(theme, cairo, cls, name, st, rect);
    return true;
}

bool xd_pkgstyle_explorer_text_color(const xd_theme *theme, xd_explorer_part part, uint8_t state,
                                     xd_rgba *out) {
    const char *cls, *name;
    char st[8];
    if (!explorer_part(theme, part, state, &cls, &name, st))
        return false;
    if (part == XD_EXPLORER_COMMAND_BAR)
        return xd_pkg_color(theme->pkg, cls, "TaskButton", "1", "TextColor", out);
    if (part == XD_EXPLORER_DETAILS_PANE)
        return xd_pkg_color(theme->pkg, cls, "MetadataLabel", nullptr, "TextColor", out);
    return xd_pkg_color(theme->pkg, cls, name, st, "TextColor", out);
}
