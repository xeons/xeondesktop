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
static bool dwm_frame(const xd_theme *t) {
    return t->glass && t->pkg != nullptr && xd_pkg_has(t->pkg, "DWMWindow", "CloseButton");
}

bool xd_pkgstyle_has_frame(const xd_theme *t) {
    return t->pkg != nullptr && xd_pkg_has(t->pkg, WINDOW, "Caption") && !dwm_frame(t);
}

/* A DWMWindow glyph: the one drawn for the nearest DPI at or below the
 * monitor's, at its own size, centred in `r`. */
static void dwm_glyph(const xd_theme *t, cairo_t *cr, const char *glyph, const char *state,
                      xd_rect r) {
    static const int dpis[] = {192, 144, 120, 96};
    double dpi = 96.0 * t->scale;
    for (size_t i = 0; i < XD_ARRAY_LEN(dpis); i++) {
        char part[32];
        g_snprintf(part, sizeof(part), "%s%d", glyph, dpis[i]);
        if ((dpis[i] > dpi + 0.5 && dpis[i] != 96) || !xd_pkg_has(t->pkg, "DWMWindow", part))
            continue;
        /* Drawn for dpis[i]: scaled only by what the monitor adds. */
        xd_theme at = *t;
        at.scale = MAX(dpi / dpis[i], 0.5);
        int w, h;
        if (!xd_pkg_part_size(&at, "DWMWindow", part, state, &w, &h))
            return;
        (void)xd_pkg_draw(&at, cr, "DWMWindow", part, state,
                          (xd_rect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h});
        return;
    }
}

bool xd_pkgstyle_dwm_button(const xd_theme *t, cairo_t *cr, xd_rect r, xd_frame_part part,
                            const xd_frame *f, bool leftmost, bool alone) {
    if (!dwm_frame(t))
        return false;
    bool close = part == XD_PART_CLOSE;
    const char *base = close ? (alone ? "CloseButtonAlone" : "CloseButton")
                             : (leftmost ? "MinButton" : "MaxButton");
    char bg[40];
    g_snprintf(bg, sizeof(bg), "%s%s", base, f->active ? "" : "Inactive");
    if (!xd_pkg_has(t->pkg, "DWMWindow", bg))
        g_strlcpy(bg, base, sizeof(bg));
    bool pressed = f->pressed == part && f->hot == part;
    bool hot = f->hot == part && !pressed;
    const char *state = pressed ? "3" : hot ? "2" : "1";
    if (hot) {
        /* The glow around a hot button, larger than it, centred on it. */
        const char *glow = close ? "CloseGlow" : "ButtonGlow";
        int w, h;
        if (xd_pkg_part_size(t, "DWMWindow", glow, "1", &w, &h))
            (void)xd_pkg_draw(t, cr, "DWMWindow", glow, "1",
                              (xd_rect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h});
    }
    (void)xd_pkg_draw(t, cr, "DWMWindow", bg, state, r);
    const char *glyph = close                      ? "CloseGlyph"
                        : part == XD_PART_HELP     ? "HelpGlyph"
                        : part == XD_PART_MINIMIZE ? "MinGlyph"
                        : f->maximized             ? "RestoreGlyph"
                                                   : "MaxGlyph";
    dwm_glyph(t, cr, glyph, state, r);
    return true;
}

/* ---- Vista's and 7's glass frame: DWMWindow's edges and reflections ---- */

static const char *const DWM = "DWMWindow";

bool xd_pkgstyle_dwm_has_reflections(const xd_theme *t) {
    return dwm_frame(t) && xd_pkg_has(t->pkg, DWM, "ReflectionStreaks");
}

/* A reflection from the window's top corner, as it is, at its Opacity for
 * the state (active, inactive, maximized active, maximized inactive). */
static void reflection(const xd_theme *t, cairo_t *cr, const char *part, const xd_frame *f,
                       xd_rect outer, bool right) {
    const char *image = f->active ? "1" : "2";
    const char *state = f->maximized ? (f->active ? "3" : "4") : image;
    int w, h, opacity = 100;
    if (!xd_pkg_part_size(t, DWM, part, image, &w, &h))
        return;
    (void)xd_pkg_int(t->pkg, DWM, part, state, "Opacity", &opacity);
    if (opacity <= 0)
        return;
    cairo_push_group(cr);
    (void)xd_pkg_draw(t, cr, DWM, part, image,
                      (xd_rect){right ? outer.x + outer.w - w : outer.x, outer.y, w, h});
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, MIN(opacity, 100) / 100.0);
}

bool xd_pkgstyle_dwm_reflections(const xd_theme *t, cairo_t *cr, xd_rect outer,
                                 const xd_frame *f) {
    if (!xd_pkgstyle_dwm_has_reflections(t))
        return false;
    /* The streaks aren't stretched: past their size (802 x 604 in 7)
     * there's plain glass. */
    reflection(t, cr, "ReflectionStreaks", f, outer, false);
    reflection(t, cr, "ReflectionTopLeft", f, outer, false);
    reflection(t, cr, "ReflectionTopRight", f, outer, true);
    return true;
}

static void slice(const xd_theme *t, cairo_t *cr, const char *part, const char *state,
                  xd_rect src, xd_rect dst) {
    (void)xd_pkg_draw_slice(t, cr, DWM, part, state, src, dst);
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
bool xd_pkgstyle_dwm_edges(const xd_theme *t, cairo_t *cr, xd_rect o, xd_rect c,
                           const xd_frame *f) {
    static const char *const pieces[] = {"FrameTop", "FrameBottom", "FrameLeft", "FrameRight"};
    if (!dwm_frame(t) || f->maximized)
        return false;
    for (size_t i = 0; i < XD_ARRAY_LEN(pieces); i++)
        if (!xd_pkg_has(t->pkg, DWM, pieces[i]))
            return false;
    const char *st = f->active ? "1" : "2";
    int u1 = xd_tpx(t, 1), u2 = xd_tpx(t, 2), u3 = xd_tpx(t, 3);
    int u7 = xd_tpx(t, 7), u8 = xd_tpx(t, 8);
    int right = o.x + o.w, bottom = o.y + o.h;
    cairo_save(cr);
    /* Nothing over the client area: a line under it shows only where the
     * client is transparent. */
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, o.x, o.y, o.w, o.h);
    if (c.w > 0 && c.h > 0)
        cairo_rectangle(cr, c.x, c.y, c.w, c.h);
    cairo_clip(cr);
    /* The outer edge: corners, top and bottom, sides. */
    slice(t, cr, "FrameTop", st, (xd_rect){0, 0, 7, 7}, (xd_rect){o.x, o.y, u7, u7});
    slice(t, cr, "FrameTop", st, (xd_rect){10, 0, 1, 7}, (xd_rect){o.x + u7, o.y, o.w - 2 * u7, u7});
    slice(t, cr, "FrameTop", st, (xd_rect){14, 0, 7, 7}, (xd_rect){right - u7, o.y, u7, u7});
    slice(t, cr, "FrameBottom", st, (xd_rect){0, 3, 7, 8}, (xd_rect){o.x, bottom - u8, u7, u8});
    slice(t, cr, "FrameBottom", st, (xd_rect){10, 3, 1, 8},
          (xd_rect){o.x + u7, bottom - u8, o.w - 2 * u7, u8});
    slice(t, cr, "FrameBottom", st, (xd_rect){14, 3, 7, 8}, (xd_rect){right - u7, bottom - u8, u7, u8});
    int side = bottom - u8 - (o.y + u7);
    slice(t, cr, "FrameLeft", st, (xd_rect){0, 0, 3, 3}, (xd_rect){o.x, o.y + u7, u3, side});
    slice(t, cr, "FrameRight", st, (xd_rect){8, 0, 3, 3}, (xd_rect){right - u3, o.y + u7, u3, side});
    /* The client edge: a light line outside a dark one. */
    if (c.w > 0 && c.h > 0) {
        int cr_ = c.x + c.w, cb = c.y + c.h;
        slice(t, cr, "FrameTop", st, (xd_rect){8, 24, 2, 3}, (xd_rect){c.x - u3, c.y - u2, u2, u3});
        slice(t, cr, "FrameTop", st, (xd_rect){10, 24, 1, 3},
              (xd_rect){c.x - u1, c.y - u2, c.w + u2, u3});
        slice(t, cr, "FrameTop", st, (xd_rect){11, 24, 2, 3}, (xd_rect){cr_ + u1, c.y - u2, u2, u3});
        slice(t, cr, "FrameBottom", st, (xd_rect){8, 0, 2, 3}, (xd_rect){c.x - u3, cb - u1, u2, u3});
        slice(t, cr, "FrameBottom", st, (xd_rect){10, 0, 1, 3},
              (xd_rect){c.x - u1, cb - u1, c.w + u2, u3});
        slice(t, cr, "FrameBottom", st, (xd_rect){11, 0, 2, 3}, (xd_rect){cr_ + u1, cb - u1, u2, u3});
        slice(t, cr, "FrameLeft", st, (xd_rect){8, 0, 3, 3},
              (xd_rect){c.x - u2, c.y + u1, u3, c.h - u2});
        slice(t, cr, "FrameRight", st, (xd_rect){0, 0, 3, 3},
              (xd_rect){cr_ - u1, c.y + u1, u3, c.h - u2});
    }
    cairo_restore(cr);
    return true;
}

static int part_h(const xd_theme *t, const char *part, int fallback) {
    int w, h;
    return xd_pkg_part_size(t, WINDOW, part, "Active", &w, &h) ? h : fallback;
}

/* Windows sizes a frame from system metrics, not from the style's images
 * (it stretches them to fit): borders of XP's sizing frame, 4 px, plus
 * Vista's and 7's PaddedBorderWidth; the caption CaptionBarHeight + 1
 * below the top border (30 for Luna, as XP's screenshots measure). A
 * maximized window's borders are off the screen. */
static int frame_border(const xd_theme *t) {
    int padded = 0;
    (void)xd_pkg_sys_int(t->pkg, "PaddedBorderWidth", &padded);
    return xd_tpx(t, 4 + MAX(padded, 0));
}

static int caption_bar(const xd_theme *t) {
    int cbh = 0;
    if (xd_pkg_sys_int(t->pkg, "CaptionBarHeight", &cbh) && cbh > 0)
        return xd_tpx(t, cbh + 1);
    /* No metric: the caption image less the border. */
    return MAX(part_h(t, "Caption", xd_tpx(t, 30)) - frame_border(t), xd_tpx(t, 8));
}

xd_insets xd_pkgstyle_frame_insets(const xd_theme *t, const xd_frame *f) {
    if (f->maximized)
        return (xd_insets){0, caption_bar(t), 0, 0};
    int b = frame_border(t);
    return (xd_insets){b, b + caption_bar(t), b, b};
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

void xd_pkgstyle_frame_layout(const xd_theme *t, xd_rect outer, const xd_frame *f,
                              xd_frame_layout *l) {
    xd_insets in = xd_pkgstyle_frame_insets(t, f);
    l->caption = (xd_rect){outer.x, outer.y, outer.w, in.top};
    /* Where Windows puts them, whatever a style's Offsets say (the WE
     * styles' would put them outside the window): the style's own button
     * size if it fits the bar (WE's 18x20), else square at the bar's
     * height less 4; centred between the top border and the client area,
     * 2 px in from the right border and 2 px apart. */
    int border = f->maximized ? 0 : frame_border(t);
    int bar = in.top - border;
    int size = MAX(bar - xd_tpx(t, 5), xd_tpx(t, 8));
    int bw = size, bh = size;
    int w, h;
    if (xd_pkg_part_size(t, WINDOW, "CloseButton", "1", &w, &h) && h <= bar - xd_tpx(t, 2)) {
        bw = w;
        bh = h;
    }
    int step = bw + xd_tpx(t, 2);
    int right = outer.x + outer.w - border - xd_tpx(t, 2);
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
        if (!(f->buttons & order[i].flag))
            continue;
        xd_layout_add_button(l, (xd_rect){bx, by, bw, bh}, order[i].part);
        leftmost = bx;
        bx -= step;
    }
    int icon = xd_tpx(t, 16);
    int edge = f->maximized ? 0 : in.left;
    l->icon = (xd_rect){outer.x + edge + xd_tpx(t, 3),
                        outer.y + (in.top - icon + (f->maximized ? 0 : xd_tpx(t, 1))) / 2, icon,
                        icon};
    int tx = l->icon.x + icon + xd_tpx(t, 3);
    l->title = (xd_rect){tx, outer.y, leftmost - xd_tpx(t, 2) - tx, in.top};
}

void xd_pkgstyle_draw_frame(const xd_theme *t, cairo_t *cr, xd_rect outer, const xd_frame *f) {
    xd_insets in = xd_pkgstyle_frame_insets(t, f);
    const char *st = f->active ? "Active" : "Inactive";
    (void)xd_pkg_draw(t, cr, WINDOW, f->maximized ? "MaxCaption" : "Caption", st,
                      (xd_rect){outer.x, outer.y, outer.w, in.top});
    if (!f->maximized) {
        int mid = outer.h - in.top - in.bottom;
        (void)xd_pkg_draw(t, cr, WINDOW, "FrameLeft", st,
                          (xd_rect){outer.x, outer.y + in.top, in.left, mid});
        (void)xd_pkg_draw(t, cr, WINDOW, "FrameRight", st,
                          (xd_rect){outer.x + outer.w - in.right, outer.y + in.top, in.right,
                                    mid});
        (void)xd_pkg_draw(t, cr, WINDOW, "FrameBottom", st,
                          (xd_rect){outer.x, outer.y + outer.h - in.bottom, outer.w, in.bottom});
    }
    xd_frame_layout l = {0};
    xd_pkgstyle_frame_layout(t, outer, f, &l);
    for (size_t i = 0; i < l.button_count; i++) {
        const char *part = button_part(l.button_parts[i], f->maximized);
        /* Normal, Hot, Pushed; XP's strips have the inactive window's
         * versions after them. */
        int n = f->pressed == l.button_parts[i] ? 3 : f->hot == l.button_parts[i] ? 2 : 1;
        int count = 1;
        (void)xd_pkg_int(t->pkg, WINDOW, part, nullptr, "ImageCount", &count);
        if (!f->active && count >= 8)
            n += 4;
        char state[8];
        g_snprintf(state, sizeof(state), "%d", n);
        (void)xd_pkg_draw(t, cr, WINDOW, part, state, l.button_rects[i]);
    }
    xd_style_task_icon(t, cr, f->icon, l.icon.x, l.icon.y);
    if (f->title == nullptr || l.title.w <= 0)
        return;
    const char *cap = f->maximized ? "MaxCaption" : "Caption";
    xd_rgba color = xd_sys(t, f->active ? XD_COLOR_TITLE_TEXT : XD_COLOR_INACTIVE_TITLE_TEXT);
    (void)xd_pkg_color(t->pkg, WINDOW, cap, st, "TextColor", &color);
    xd_text_style ts = xd_text(t, XD_FONT_CAPTION, color, XD_ALIGN_START);
    const char *shadow = xd_pkg_prop(t->pkg, WINDOW, cap, st, "TextShadowType");
    xd_rgba sc;
    int so[2] = {1, 1};
    const char *sov = xd_pkg_prop(t->pkg, WINDOW, cap, st, "TextShadowOffset");
    if (sov != nullptr)
        (void)sscanf(sov, "%d %d", &so[0], &so[1]);
    if (shadow != nullptr && g_ascii_strcasecmp(shadow, "Single") == 0 &&
        xd_pkg_color(t->pkg, WINDOW, cap, st, "TextShadowColor", &sc)) {
        xd_text_style ss = xd_text(t, XD_FONT_CAPTION, sc, XD_ALIGN_START);
        xd_rect sr = l.title;
        sr.x += (int)lround(so[0] * t->scale);
        sr.y += (int)lround(so[1] * t->scale);
        xd_draw_text(cr, sr, f->title, &ss);
    }
    xd_draw_text(cr, l.title, f->title, &ts);
}

/* ---- controls -------------------------------------------------------------------- */

static const char *const BUTTON = "Button";

static void state_str(char out[8], int n) { g_snprintf(out, 8, "%d", n); }

/* Normal 1, Hot 2, Pressed 3, Disabled 4: uxtheme's usual order. */
static int basic_state(uint8_t state) {
    if (state & XD_STATE_DISABLED)
        return 4;
    if (state & XD_STATE_PRESSED)
        return 3;
    return (state & XD_STATE_HOT) ? 2 : 1;
}

static xd_rgba text_color(const xd_theme *t, const char *cls, const char *part, int n,
                          xd_rgba fallback) {
    char st[8];
    state_str(st, n);
    xd_rgba c = fallback;
    (void)xd_pkg_color(t->pkg, cls, part, st, "TextColor", &c);
    return c;
}

bool xd_pkgstyle_draw_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                             const char *label) {
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, BUTTON, "PushButton"))
        return false;
    int n = basic_state(state);
    if (n == 1 && (state & XD_STATE_DEFAULT))
        n = 5; /* Defaulted */
    char st[8];
    state_str(st, n);
    (void)xd_pkg_draw(t, cr, BUTTON, "PushButton", st, r);
    xd_rgba fg = text_color(t, BUTTON, "PushButton", n,
                            xd_sys(t, (state & XD_STATE_DISABLED) ? XD_COLOR_GRAY_TEXT
                                                                  : XD_COLOR_BUTTON_TEXT));
    xd_rect c = xd_pkg_content_rect(t, BUTTON, "PushButton", st, r);
    xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, fg, XD_ALIGN_CENTER);
    xd_draw_text(cr, c, label, &ts);
    if (state & XD_STATE_FOCUSED)
        xd_theme_draw_focus_rect(t, cr, xd_rect_inset(c, xd_tpx(t, 1), xd_tpx(t, 1)));
    return true;
}

/* A check box or radio button: the box at its own size where the label's
 * row starts, then the label. */
static bool box_and_label(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                          const char *label, const char *part, int checked_offset) {
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, BUTTON, part))
        return false;
    int n = basic_state(state) + ((state & XD_STATE_CHECKED) ? checked_offset : 0);
    char st[8];
    state_str(st, n);
    int bw = xd_tpx(t, 13), bh = bw;
    (void)xd_pkg_part_size(t, BUTTON, part, st, &bw, &bh);
    (void)xd_pkg_draw(t, cr, BUTTON, part, st,
                      (xd_rect){r.x, r.y + (r.h - bh) / 2, bw, bh});
    bool disabled = state & XD_STATE_DISABLED;
    xd_rgba fg = text_color(t, BUTTON, part, n,
                            xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_WINDOW_TEXT));
    int gap = xd_tpx(t, 5);
    xd_rect text = {r.x + bw + gap, r.y, r.w - bw - gap, r.h};
    xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
    xd_draw_text(cr, text, label, &ts);
    if (state & XD_STATE_FOCUSED) {
        int w = MIN(xd_text_width(cr, label, &ts), text.w);
        xd_theme_draw_focus_rect(t, cr,
                                 (xd_rect){text.x - xd_tpx(t, 1), r.y + xd_tpx(t, 2),
                                           w + xd_tpx(t, 3), r.h - xd_tpx(t, 4)});
    }
    return true;
}

bool xd_pkgstyle_draw_checkbox(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                               const char *label) {
    /* Unchecked 1-4, Checked 5-8 (Mixed 9-12). */
    return box_and_label(t, cr, r, state, label, "CheckBox", 4);
}

bool xd_pkgstyle_draw_radio(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                            const char *label) {
    return box_and_label(t, cr, r, state, label, "RadioButton", 4);
}

bool xd_pkgstyle_tab_page_color(const xd_theme *t, xd_rgba *out) {
    if (t->pkg == nullptr)
        return false;
    return xd_pkg_color(t->pkg, "Tab", "Body", nullptr, "FillColorHint", out) ||
           xd_pkg_color(t->pkg, "Tab", "Pane", nullptr, "FillColorHint", out);
}

bool xd_pkgstyle_draw_groupbox(const xd_theme *t, cairo_t *cr, xd_rect r, const char *label) {
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, BUTTON, "GroupBox"))
        return false;
    int th = xd_tpx(t, 14);
    int top = r.y + th / 2;
    xd_rgba fg = text_color(t, BUTTON, "GroupBox", 1, xd_sys(t, XD_COLOR_BUTTON_TEXT));
    xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
    bool has_label = label != nullptr && label[0] != '\0';
    xd_rect gap = {0};
    if (has_label)
        gap = (xd_rect){r.x + xd_tpx(t, 7), r.y, xd_text_width(cr, label, &ts) + xd_tpx(t, 4), th};
    /* The box is drawn around the label, which has no background of its
     * own: whatever is behind the group shows through, as in Windows. */
    cairo_save(cr);
    if (has_label) {
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_rectangle(cr, gap.x, gap.y, gap.w, gap.h);
        cairo_clip(cr);
    }
    (void)xd_pkg_draw(t, cr, BUTTON, "GroupBox", "1",
                      (xd_rect){r.x, top, r.w, r.h - (top - r.y)});
    cairo_restore(cr);
    if (has_label)
        xd_draw_text(cr, xd_rect_inset4(gap, xd_tpx(t, 2), 0, 0, 0), label, &ts);
    return true;
}

bool xd_pkgstyle_draw_field(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled,
                            xd_rect *inner) {
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, "Edit", "EditText"))
        return false;
    const char *st = disabled ? "Disabled" : "Normal";
    xd_rgba fill = xd_sys(t, disabled ? XD_COLOR_BUTTON_FACE : XD_COLOR_WINDOW);
    (void)xd_pkg_color(t->pkg, "Edit", "EditText", st, "FillColor", &fill);
    xd_fill_rect(cr, r, fill);
    (void)xd_pkg_draw(t, cr, "Edit", "EditText", st, r);
    int border = 1;
    (void)xd_pkg_int(t->pkg, "Edit", "EditText", st, "BorderSize", &border);
    *inner = xd_rect_inset(r, xd_tpx(t, border), xd_tpx(t, border));
    return true;
}

bool xd_pkgstyle_draw_dropdown_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                      uint8_t state) {
    char st[8];
    state_str(st, basic_state(state));
    return xd_pkg_draw(t, cr, "ComboBox", "DropDownButton", st, r);
}

/* A scroll bar along `r`: arrows at its ends, the track either side of the
 * thumb, the thumb and its gripper. */
static bool scrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rect thumb, bool disabled,
                      bool vertical) {
    const char *const SB = "ScrollBar";
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, SB, "ArrowBtn"))
        return false;
    int len = vertical ? r.h : r.w, across = vertical ? r.w : r.h;
    int arrow = MIN(across, len / 2);
    /* [a, b) along the bar, as a rectangle across it. */
#define SPAN(a, b)                                                                             \
    (vertical ? (xd_rect){r.x, (a), r.w, (b) - (a)} : (xd_rect){(a), r.y, (b) - (a), r.h})
    int lo = (vertical ? r.y : r.x), hi = lo + len;
    xd_rect first = SPAN(lo, lo + arrow), last = SPAN(hi - arrow, hi);
    int tlo = vertical ? thumb.y : thumb.x, tlen = vertical ? thumb.h : thumb.w;
    const char *upper = vertical ? "UpperTrackVert" : "UpperTrackHorz";
    const char *lower = vertical ? "LowerTrackVert" : "LowerTrackHorz";
    const char *track_state = disabled ? "4" : "1";
    if (tlen > 0) {
        (void)xd_pkg_draw(t, cr, SB, upper, track_state, SPAN(lo + arrow, tlo));
        (void)xd_pkg_draw(t, cr, SB, lower, track_state, SPAN(tlo + tlen, hi - arrow));
    } else {
        (void)xd_pkg_draw(t, cr, SB, upper, track_state, SPAN(lo + arrow, hi - arrow));
    }
#undef SPAN
    /* ArrowBtn: Up 1-4, Down 5-8, Left 9-12, Right 13-16 (Normal, Hot,
     * Pressed, Disabled). */
    int base = vertical ? 1 : 9;
    char st[8];
    state_str(st, base + (disabled ? 3 : 0));
    (void)xd_pkg_draw(t, cr, SB, "ArrowBtn", st, first);
    state_str(st, base + 4 + (disabled ? 3 : 0));
    (void)xd_pkg_draw(t, cr, SB, "ArrowBtn", st, last);
    if (tlen > 0) {
        (void)xd_pkg_draw(t, cr, SB, vertical ? "ThumbBtnVert" : "ThumbBtnHorz", "1", thumb);
        (void)xd_pkg_draw(t, cr, SB, vertical ? "GripperVert" : "GripperHorz", "1", thumb);
    }
    return true;
}

bool xd_pkgstyle_draw_vscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rect thumb,
                                 bool disabled) {
    return scrollbar(t, cr, r, thumb, disabled, true);
}

bool xd_pkgstyle_draw_hscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rect thumb,
                                 bool disabled) {
    return scrollbar(t, cr, r, thumb, disabled, false);
}

bool xd_pkgstyle_draw_tab(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                          const char *label) {
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, "Tab", "TabItem"))
        return false;
    bool selected = state & XD_STATE_CHECKED;
    /* The selected tab is TopTabItem, taller, joining the page. */
    const char *part = selected && xd_pkg_has(t->pkg, "Tab", "TopTabItem") ? "TopTabItem"
                                                                           : "TabItem";
    int n = selected ? 3 : (state & XD_STATE_DISABLED) ? 4 : (state & XD_STATE_HOT) ? 2 : 1;
    char st[8];
    state_str(st, n);
    xd_rect tab = selected ? r : xd_rect_inset4(r, 0, xd_tpx(t, 2), 0, 0);
    (void)xd_pkg_draw(t, cr, "Tab", part, st, tab);
    xd_rgba fg = text_color(t, "Tab", part, n, xd_sys(t, XD_COLOR_BUTTON_TEXT));
    xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, fg, XD_ALIGN_CENTER);
    xd_rect text = xd_rect_inset4(tab, xd_tpx(t, 4), xd_tpx(t, 1), xd_tpx(t, 4), 0);
    xd_draw_text(cr, text, label, &ts);
    if (state & XD_STATE_FOCUSED) {
        int w = MIN(xd_text_width(cr, label, &ts) + xd_tpx(t, 4), text.w);
        xd_theme_draw_focus_rect(t, cr,
                                 (xd_rect){text.x + (text.w - w) / 2, text.y + xd_tpx(t, 2), w,
                                           text.h - xd_tpx(t, 4)});
    }
    return true;
}

bool xd_pkgstyle_draw_tab_panel(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, "Tab", "Pane"))
        return false;
    (void)xd_pkg_draw(t, cr, "Tab", "Pane", "1", r);
    if (xd_pkg_has(t->pkg, "Tab", "Body"))
        (void)xd_pkg_draw(t, cr, "Tab", "Body", "1",
                          xd_rect_inset(r, xd_tpx(t, 1), xd_tpx(t, 1)));
    return true;
}

/* ---- the rest of the controls ---------------------------------------------------- */

static bool has(const xd_theme *t, const char *cls, const char *part) {
    return t->pkg != nullptr && xd_pkg_has(t->pkg, cls, part);
}

static xd_rgba text_color_st(const xd_theme *t, const char *cls, const char *part,
                             const char *st, xd_rgba fallback) {
    xd_rgba c = fallback;
    (void)xd_pkg_color(t->pkg, cls, part, st, "TextColor", &c);
    return c;
}

/* The background's content rectangle as uxtheme's
 * GetThemeBackgroundContentRect gives it: ContentMargins, or for an image
 * its SizingMargins, or for a border and fill its BorderSize. */
static xd_rect content_or_sizing(const xd_theme *t, const char *cls, const char *part,
                                 const char *st, xd_rect r) {
    xd_insets m;
    if (xd_pkg_margins(t->pkg, cls, part, st, "ContentMargins", &m))
        return xd_pkg_content_rect(t, cls, part, st, r);
    const char *bg = xd_pkg_prop(t->pkg, cls, part, st, "BgType");
    if (bg != nullptr && g_ascii_strcasecmp(bg, "BorderFill") == 0) {
        int b = 1;
        (void)xd_pkg_int(t->pkg, cls, part, st, "BorderSize", &b);
        m = (xd_insets){b, b, b, b};
    } else if (!xd_pkg_margins(t->pkg, cls, part, st, "SizingMargins", &m)) {
        return r;
    }
    int l = xd_tpx(t, m.left), rr = xd_tpx(t, m.right);
    int top = xd_tpx(t, m.top), b = xd_tpx(t, m.bottom);
    xd_rect c = {r.x + l, r.y + top, r.w - l - rr, r.h - top - b};
    return c.w > 0 && c.h > 0 ? c : r;
}

bool xd_pkgstyle_draw_sizebox(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (!has(t, "ScrollBar", "Sizebox"))
        return false;
    if (!xd_pkg_draw(t, cr, "ScrollBar", "SizeboxBkgnd", "1", r))
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    (void)xd_pkg_draw(t, cr, "ScrollBar", "Sizebox", "RightAlign", r);
    return true;
}

bool xd_pkgstyle_draw_progress(const xd_theme *t, cairo_t *cr, xd_rect r, double fraction) {
    const char *const P = "Progress";
    if (!has(t, P, "Bar"))
        return false;
    (void)xd_pkg_draw(t, cr, P, "Bar", "1", r);
    xd_rect c = content_or_sizing(t, P, "Bar", "1", r);
    int done = (int)lround(c.w * CLAMP(fraction, 0.0, 1.0));
    if (done <= 0)
        return true;
    /* Vista and 7 fill continuously; XP draws ProgressChunkSize chunks
     * ProgressSpaceSize apart, as comctl32 does. */
    if (has(t, P, "Fill")) {
        (void)xd_pkg_draw(t, cr, P, "Fill", "Normal", (xd_rect){c.x, c.y, done, c.h});
        return true;
    }
    int chunk = 6, space = 2;
    (void)xd_pkg_int(t->pkg, P, "Chunk", nullptr, "ProgressChunkSize", &chunk);
    (void)xd_pkg_int(t->pkg, P, "Chunk", nullptr, "ProgressSpaceSize", &space);
    chunk = MAX(xd_tpx(t, MAX(chunk, 1)), 1);
    space = xd_tpx(t, MAX(space, 0));
    cairo_save(cr);
    cairo_rectangle(cr, c.x, c.y, c.w, c.h);
    cairo_clip(cr);
    for (int x = c.x; x < c.x + done; x += chunk + space)
        (void)xd_pkg_draw(t, cr, P, "Chunk", "1", (xd_rect){x, c.y, chunk, c.h});
    cairo_restore(cr);
    return true;
}

bool xd_pkgstyle_draw_trackbar(const xd_theme *t, cairo_t *cr, xd_rect channel, xd_rect thumb,
                               uint8_t state) {
    const char *const TB = "TrackBar";
    if (!has(t, TB, "Track"))
        return false;
    (void)xd_pkg_draw(t, cr, TB, "Track", "1", channel);
    /* Normal 1, Hot 2, Pressed 3, Focused 4, Disabled 5. */
    int n = (state & XD_STATE_DISABLED) ? 5
            : (state & XD_STATE_PRESSED) ? 3
            : (state & XD_STATE_HOT)     ? 2
            : (state & XD_STATE_FOCUSED) ? 4
                                         : 1;
    char st[8];
    state_str(st, n);
    /* The thumb points down at the ticks below it. */
    (void)xd_pkg_draw(t, cr, TB, has(t, TB, "ThumbBottom") ? "ThumbBottom" : "Thumb", st, thumb);
    return true;
}

bool xd_pkgstyle_trackbar_tics_color(const xd_theme *t, xd_rgba *out) {
    return t->pkg != nullptr && xd_pkg_color(t->pkg, "TrackBar", "Tics", "1", "Color", out);
}

bool xd_pkgstyle_draw_spin(const xd_theme *t, cairo_t *cr, xd_rect r, bool up, uint8_t state) {
    const char *part = up ? "Up" : "Down";
    if (!has(t, "Spin", part))
        return false;
    char st[8];
    state_str(st, basic_state(state));
    (void)xd_pkg_draw(t, cr, "Spin", part, st, r);
    return true;
}

bool xd_pkgstyle_draw_header_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label) {
    const char *const H = "Header";
    if (!has(t, H, "HeaderItem"))
        return false;
    /* Normal 1, Hot 2, Pressed 3. */
    int n = (state & XD_STATE_PRESSED) ? 3 : (state & XD_STATE_HOT) ? 2 : 1;
    char st[8];
    state_str(st, n);
    (void)xd_pkg_draw(t, cr, H, "HeaderItem", st, r);
    if (label == nullptr)
        return true;
    xd_rgba fg = text_color_st(t, H, "HeaderItem", st, xd_sys(t, XD_COLOR_BUTTON_TEXT));
    xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
    xd_rect c = xd_pkg_content_rect(t, H, "HeaderItem", st, r);
    xd_draw_text(cr, xd_rect_inset4(c, xd_tpx(t, 6), 0, xd_tpx(t, 6), 0), label, &ts);
    return true;
}

bool xd_pkgstyle_draw_tree_glyph(const xd_theme *t, cairo_t *cr, xd_rect r, bool open) {
    if (!has(t, "TreeView", "Glyph"))
        return false;
    (void)xd_pkg_draw(t, cr, "TreeView", "Glyph", open ? "Opened" : "Closed", r);
    return true;
}

bool xd_pkgstyle_draw_statusbar(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text,
                                bool grip) {
    const char *const S = "Status";
    if (!has(t, S, "Pane"))
        return false;
    if (!xd_pkg_draw(t, cr, S, nullptr, nullptr, r))
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    int gw = 0, gh = 0;
    if (grip && !xd_pkg_part_size(t, S, "Gripper", "1", &gw, &gh))
        grip = false;
    xd_rect pane = {r.x, r.y + xd_tpx(t, 2), r.w - (grip ? gw : 0), r.h - xd_tpx(t, 2)};
    (void)xd_pkg_draw(t, cr, S, "Pane", "1", pane);
    if (grip)
        (void)xd_pkg_draw(t, cr, S, "Gripper", "1",
                          (xd_rect){r.x + r.w - gw, r.y + r.h - gh, gw, gh});
    if (text != nullptr) {
        xd_rgba fg = text_color_st(t, S, "Pane", "1", xd_sys(t, XD_COLOR_BUTTON_TEXT));
        xd_text_style ts = xd_text(t, XD_FONT_TOOLTIP, fg, XD_ALIGN_START);
        xd_draw_text(cr, xd_rect_inset4(pane, xd_tpx(t, 4), 0, xd_tpx(t, 4), 0), text, &ts);
    }
    return true;
}

bool xd_pkgstyle_draw_tooltip(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text) {
    const char *const TT = "Tooltip";
    if (!has(t, TT, "Standard"))
        return false;
    /* Its own shape: rounded corners show what's behind. */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_fill(cr);
    cairo_restore(cr);
    (void)xd_pkg_draw(t, cr, TT, "Standard", "Normal", r);
    xd_rgba fg = text_color_st(t, TT, "Standard", "Normal", xd_sys(t, XD_COLOR_INFO_TEXT));
    xd_text_style ts = xd_text(t, XD_FONT_TOOLTIP, fg, XD_ALIGN_START);
    xd_draw_text(cr, xd_rect_inset(r, xd_tpx(t, 4), 0), text, &ts);
    return true;
}

bool xd_pkgstyle_draw_rebar(const xd_theme *t, cairo_t *cr, xd_rect r, bool gripper) {
    const char *const RB = "Rebar";
    if (!has(t, RB, "Band") && !has(t, RB, "Gripper"))
        return false;
    if (!xd_pkg_draw(t, cr, RB, nullptr, nullptr, r))
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_BUTTON_FACE));
    (void)xd_pkg_draw(t, cr, RB, "Band", "1", r);
    if (gripper) {
        int gw = xd_tpx(t, 3), gh;
        (void)xd_pkg_part_size(t, RB, "Gripper", "1", &gw, &gh);
        (void)xd_pkg_draw(t, cr, RB, "Gripper", "1",
                          (xd_rect){r.x + xd_tpx(t, 2), r.y + xd_tpx(t, 2), gw,
                                    r.h - xd_tpx(t, 4)});
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

bool xd_pkgstyle_draw_tool_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label) {
    const char *const TB = "Toolbar";
    if (!has(t, TB, "Button"))
        return false;
    char st[8];
    state_str(st, toolbar_state(state));
    (void)xd_pkg_draw(t, cr, TB, "Button", st, r);
    if (label != nullptr) {
        xd_rgba fg = text_color_st(t, TB, "Button", st,
                                   xd_sys(t, (state & XD_STATE_DISABLED) ? XD_COLOR_GRAY_TEXT
                                                                         : XD_COLOR_BUTTON_TEXT));
        xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, fg, XD_ALIGN_CENTER);
        xd_draw_text(cr, xd_pkg_content_rect(t, TB, "Button", st, r), label, &ts);
    }
    return true;
}

/* The taskbar's toolbars (Quick Launch) and their grips: the TaskBar
 * application's classes, as Explorer opens them. */
bool xd_pkgstyle_draw_taskbar_toolbar_button(const xd_theme *t, cairo_t *cr, xd_rect r,
                                             uint8_t state) {
    if (!has(t, "TaskBar::Toolbar", "Button"))
        return false;
    char st[8];
    state_str(st, toolbar_state(state));
    (void)xd_pkg_draw(t, cr, "TaskBar::Toolbar", "Button", st, r);
    return true;
}

bool xd_pkgstyle_draw_taskbar_grip(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (!has(t, "TaskBar::Rebar", "Gripper"))
        return false;
    (void)xd_pkg_draw(t, cr, "TaskBar::Rebar", "Gripper", "1", r);
    return true;
}

/* ---- Vista and 7 menus (Menu's Popup and Bar parts) ----
 * XP's styles have none of these (its menus are flat, in [SysMetrics]'s
 * colours): they keep the style's drawing. */

static const char *const MENU = "Menu";

static bool menu_themed(const xd_theme *t) { return has(t, MENU, "PopupItem"); }

bool xd_pkgstyle_draw_menu_frame(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (!menu_themed(t))
        return false;
    if (!xd_pkg_draw(t, cr, MENU, "PopupBackground", "1", r))
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_MENU));
    (void)xd_pkg_draw(t, cr, MENU, "PopupBorders", "1", r);
    /* 7's gutter, just left of the items' text. */
    int gw, gh;
    xd_menu_metrics m = xd_theme_menu_metrics(t);
    if (xd_pkg_part_size(t, MENU, "PopupGutter", "1", &gw, &gh))
        (void)xd_pkg_draw(t, cr, MENU, "PopupGutter", "1",
                          (xd_rect){r.x + m.border + m.text_x - xd_tpx(t, 5), r.y + m.border, gw,
                                    r.h - 2 * m.border});
    return true;
}

bool xd_pkgstyle_draw_menu_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                xd_rgba *text) {
    if (!menu_themed(t))
        return false;
    /* Normal 1, Hot 2, Disabled 3, DisabledHot 4. */
    bool hot = state & XD_STATE_HOT, disabled = state & XD_STATE_DISABLED;
    int n = disabled ? (hot ? 4 : 3) : (hot ? 2 : 1);
    char st[8];
    state_str(st, n);
    if (hot)
        (void)xd_pkg_draw(t, cr, MENU, "PopupItem", st, r);
    *text = text_color_st(t, MENU, "PopupItem", st,
                          xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT : XD_COLOR_MENU_TEXT));
    return true;
}

bool xd_pkgstyle_draw_menu_separator(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (!menu_themed(t) || !has(t, MENU, "PopupSeparator"))
        return false;
    int w, h = xd_tpx(t, 3);
    (void)xd_pkg_part_size(t, MENU, "PopupSeparator", "1", &w, &h);
    xd_menu_metrics m = xd_theme_menu_metrics(t);
    /* Right of the gutter, as 7 draws it. */
    int x = r.x + m.text_x - xd_tpx(t, 3);
    (void)xd_pkg_draw(t, cr, MENU, "PopupSeparator", "1",
                      (xd_rect){x, r.y + (r.h - h) / 2, r.x + r.w - x, h});
    return true;
}

bool xd_pkgstyle_draw_menu_submenu(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled) {
    if (!menu_themed(t) || !has(t, MENU, "PopupSubmenu"))
        return false;
    (void)xd_pkg_draw(t, cr, MENU, "PopupSubmenu", disabled ? "2" : "1", r);
    return true;
}

bool xd_pkgstyle_draw_menu_check(const xd_theme *t, cairo_t *cr, xd_rect r, bool disabled) {
    if (!menu_themed(t) || !has(t, MENU, "PopupCheck"))
        return false;
    /* PopupCheckBackground: Disabled 1, Normal 2. PopupCheck:
     * CheckMarkNormal 1, CheckMarkDisabled 2. */
    (void)xd_pkg_draw(t, cr, MENU, "PopupCheckBackground", disabled ? "1" : "2", r);
    (void)xd_pkg_draw(t, cr, MENU, "PopupCheck", disabled ? "2" : "1", r);
    return true;
}

bool xd_pkgstyle_draw_menubar(const xd_theme *t, cairo_t *cr, xd_rect r, bool active) {
    if (!has(t, MENU, "BarBackground"))
        return false;
    (void)xd_pkg_draw(t, cr, MENU, "BarBackground", active ? "Active" : "Inactive", r);
    return true;
}

bool xd_pkgstyle_draw_menubar_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                   const char *label) {
    if (!has(t, MENU, "BarItem"))
        return false;
    /* Normal 1, Hot 2, Pushed 3, Disabled 4, DisabledHot 5,
     * DisabledPushed 6; Normal and Disabled have no background. */
    int n = (state & XD_STATE_PRESSED) ? 3 : (state & XD_STATE_HOT) ? 2 : 1;
    if (state & XD_STATE_DISABLED)
        n += 3;
    char st[8];
    state_str(st, n);
    if (n != 1 && n != 4)
        (void)xd_pkg_draw(t, cr, MENU, "BarItem", st, r);
    xd_rgba fg = text_color_st(t, MENU, "BarItem", st,
                               xd_sys(t, (state & XD_STATE_DISABLED) ? XD_COLOR_GRAY_TEXT
                                                                     : XD_COLOR_MENU_TEXT));
    xd_text_style ts = xd_text(t, XD_FONT_MENU, fg, XD_ALIGN_CENTER);
    xd_draw_text(cr, r, label, &ts);
    return true;
}

/* ==== the taskbar, thumbnails, jump lists and Start menus ==================
}

 * Explorer's classes, as XP's and 7's explorer.exe opens them. Vista's
 * and 7's undocumented parts go by the names data/theme/uxtheme.ini
 * deduces for them (xd-theme-import writes them into the package). */

static bool seven(const xd_theme *t) { return t->style == XD_STYLE_AERO; }

static void basic_str(char out[8], uint8_t state) {
    state_str(out, (state & XD_STATE_PRESSED) ? 3 : (state & XD_STATE_HOT) ? 2 : 1);
}

/* The composited variant of an App::Class ("TaskBand2Composited::
 * TaskBand2"), as Windows picks it with desktop composition on, where it
 * has its own image for the part; else the plain class. A composited
 * class without one would fall back to its base class's image (Button's
 * push button), not the plain App::Class's. */
static const char *pick(const xd_theme *t, const char *plain, const char *comp,
                        const char *part) {
    if (!t->glass || comp == nullptr || !xd_pkg_has_own(t->pkg, comp, part))
        return plain;
    const char *colon = strstr(comp, "::");
    const char *own = xd_pkg_prop(t->pkg, comp, part, nullptr, "ImageFile");
    const char *inherited =
        colon != nullptr ? xd_pkg_prop(t->pkg, colon + 2, part, nullptr, "ImageFile") : nullptr;
    return own != nullptr && own != inherited ? comp : plain;
}

/* Text with the part's TextShadowColor behind it, TextShadowOffset away
 * (XP's "Continuous" shadow drawn as a plain one). */
static void shadowed_text(const xd_theme *t, cairo_t *cr, xd_rect r, const char *text,
                          xd_text_style st, const char *cls, const char *part,
                          const char *state) {
    xd_rgba sc;
    const char *type = xd_pkg_prop(t->pkg, cls, part, state, "TextShadowType");
    if (type != nullptr && g_ascii_strcasecmp(type, "None") != 0 &&
        xd_pkg_color(t->pkg, cls, part, state, "TextShadowColor", &sc)) {
        int so[2] = {1, 1};
        const char *sov = xd_pkg_prop(t->pkg, cls, part, state, "TextShadowOffset");
        if (sov != nullptr)
            (void)sscanf(sov, "%d %d", &so[0], &so[1]);
        xd_text_style ss = st;
        ss.color = sc;
        ss.shadow = false;
        xd_draw_text(cr,
                     (xd_rect){r.x + (int)lround(so[0] * t->scale),
                               r.y + (int)lround(so[1] * t->scale), r.w, r.h},
                     text, &ss);
    }
    xd_draw_text(cr, r, text, &st);
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
static const char *taskbar_class(const xd_theme *t, const char *part) {
    if (!seven(t))
        return "TaskBar";
    if (xd_pkg_has_own(t->pkg, "TaskBar2::TaskBar", part))
        return pick(t, "TaskBar2::TaskBar", "TaskBar2Composited::TaskBar", part);
    return pick(t, "TaskBar", "TaskBarComposited::TaskBar", part);
}

bool xd_pkgstyle_draw_taskbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge) {
    if (t->pkg == nullptr)
        return false;
    char part[32];
    g_snprintf(part, sizeof(part), "Background%s", edge_name(edge));
    const char *cls = taskbar_class(t, part);
    if (!xd_pkg_has_own(t->pkg, cls, part))
        return false;
    if (seven(t)) {
        /* Vista's and 7's images are a sheen over the glass colour (or,
         * without composition, opaque). */
        xd_glass_colors g;
        xd_theme_glass_colors(t, &g);
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        xd_fill_rect(cr, r, g.taskbar);
        cairo_restore(cr);
    }
    (void)xd_pkg_draw(t, cr, cls, part, "1", r);
    return true;
}

void xd_theme_draw_taskbar_sizing_bar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_edge edge) {
    if (t->pkg == nullptr)
        return;
    char part[32];
    g_snprintf(part, sizeof(part), "SizingBar%s", edge_name(edge));
    const char *cls = taskbar_class(t, part);
    int w, h;
    if (!xd_pkg_has_own(t->pkg, cls, part) || !xd_pkg_part_size(t, cls, part, "1", &w, &h))
        return;
    bool side = edge == XD_EDGE_LEFT || edge == XD_EDGE_RIGHT;
    (void)xd_pkg_draw(t, cr, cls, part, "1", xd_style_rim_line(r, edge, 0, side ? w : h));
}

/* A Start button image drawn at its size (TrueSize: the WE styles') is
 * the button's width; 0 for the style's own (Luna stretches its image to
 * fit "start"). */
int xd_pkgstyle_start_button_width(const xd_theme *t) {
    const char *const SB = "Start::Button";
    int w, h;
    if (t->pkg == nullptr || seven(t) || !xd_pkg_has_own(t->pkg, SB, nullptr))
        return 0;
    const char *sizing = xd_pkg_prop(t->pkg, SB, nullptr, nullptr, "SizingType");
    if (sizing == nullptr || g_ascii_strcasecmp(sizing, "TrueSize") != 0 ||
        !xd_pkg_part_size(t, SB, nullptr, "1", &w, &h))
        return 0;
    return w;
}

bool xd_pkgstyle_draw_start_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    if (t->pkg == nullptr)
        return false;
    char st[8];
    basic_str(st, state);
    /* Vista's orb (7's is explorer.exe's own: ours stands in). */
    if (seven(t) && xd_pkg_has_own(t->pkg, "StartMiddle::Button", nullptr))
        return xd_pkg_draw(t, cr, "StartMiddle::Button", nullptr, st, r);
    const char *const SB = "Start::Button";
    if (!xd_pkg_has_own(t->pkg, SB, nullptr))
        return false;
    (void)xd_pkg_draw(t, cr, SB, nullptr, st, r);
    /* XP's flag is explorer.exe's, not the style's: our logo, then
     * "start" in the style's font and colours, where its content area
     * is. Styles whose button image has its own logo (the WE styles) put
     * the content area off the button and the font at 1 point: then
     * neither is drawn. */
    xd_rect c = xd_pkg_content_rect(t, SB, nullptr, st, r);
    if (c.x < r.x || c.x + c.w > r.x + r.w)
        return true;
    int dy = (state & XD_STATE_PRESSED) ? xd_tpx(t, 1) : 0;
    int icon = xd_tpx(t, 20);
    int ix = r.x + xd_tpx(t, 8);
    xd_draw_start_logo(cr, (xd_rect){ix, c.y + (c.h - icon) / 2 + dy, icon, icon});
    xd_font_spec font = {.family = XD_FONT_FAMILY_TREBUCHET, .size_pt = 13, .bold = true,
                         .italic = true};
    (void)xd_pkg_font(t->pkg, SB, nullptr, st, "Font", &font, nullptr);
    if (font.size_pt < 4)
        return true;
    xd_text_style ts = xd_text(
        t, XD_FONT_MENU, text_color_st(t, SB, nullptr, st, xd_rgba_hex(0xFFFFFF)), XD_ALIGN_START);
    ts.font = &font;
    int tx = ix + icon + xd_tpx(t, 5);
    shadowed_text(t, cr, (xd_rect){tx, c.y + dy, r.x + r.w - tx, c.h}, "start", ts, SB, nullptr,
                  st);
    return true;
}

bool xd_pkgstyle_draw_task_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                  const char *label, cairo_surface_t *icon, int count) {
    if (t->pkg == nullptr)
        return false;
    const char *cls = (state & XD_STATE_VERTICAL) ? "TaskBandVert::Toolbar" : "TaskBand::Toolbar";
    if (!xd_pkg_has_own(t->pkg, cls, "Button"))
        cls = "TaskBand::Toolbar";
    if (!xd_pkg_has_own(t->pkg, cls, "Button"))
        return false;
    /* Toolbar buttons' states: the active window's is Checked. */
    char st[8];
    state_str(st, toolbar_state(state));
    bool flash = (state & XD_STATE_FLASH) && !(state & XD_STATE_CHECKED);
    if (!flash || !xd_pkg_draw(t, cr, "TaskBand", "FlashButton", "1", r))
        (void)xd_pkg_draw(t, cr, cls, "Button", st, r);
    int isz = xd_tpx(t, 16);
    int ix = r.x + xd_tpx(t, 10);
    xd_style_task_icon(t, cr, icon, ix, r.y + (r.h - isz) / 2);
    xd_font_spec font = t->scheme.fonts[XD_FONT_MENU];
    (void)xd_pkg_font(t->pkg, cls, "Button", st, "Font", &font, nullptr);
    xd_text_style ts =
        xd_text(t, XD_FONT_MENU, text_color_st(t, cls, "Button", st, xd_rgba_hex(0xFFFFFF)),
                XD_ALIGN_START);
    ts.font = &font;
    int tx = ix + isz + xd_tpx(t, 4);
    int end = r.x + r.w - xd_tpx(t, 6);
    char *text = nullptr;
    xd_font_spec cf = font;
    if (count > 1 && xd_pkg_has(t->pkg, "TaskBand", "GroupCount")) {
        /* The count in GroupCount's font and colour, then the name. */
        char n[16];
        g_snprintf(n, sizeof(n), "%d", count);
        cf.bold = true;
        (void)xd_pkg_font(t->pkg, "TaskBand", "GroupCount", nullptr, "Font", &cf, nullptr);
        xd_text_style cs = ts;
        cs.font = &cf;
        cs.color = text_color_st(t, "TaskBand", "GroupCount", nullptr, ts.color);
        xd_draw_text(cr, (xd_rect){tx, r.y, end - tx, r.h}, n, &cs);
        tx += xd_text_width(cr, n, &cs) + xd_tpx(t, 4);
    } else if (count > 1) {
        text = g_strdup_printf("%d %s", count, label);
        label = text;
    }
    xd_draw_text(cr, (xd_rect){tx, r.y, end - tx, r.h}, label, &ts);
    g_free(text);
    return true;
}

void xd_theme_draw_task_group(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                              int count, const char *label, cairo_surface_t *icon) {
    if (xd_pkgstyle_draw_task_button(t, cr, r, state, label, icon, count))
        return;
    char *text = g_strdup_printf("%d %s", count, label);
    xd_theme_draw_task_button(t, cr, r, state, text, icon);
    g_free(text);
}

/* 7's program buttons: TaskBand2's AppButton, its states Normal 1, Hot 2,
 * Pressed 3 for a window in the background, 4-6 for the active one's,
 * Flashing 7 (deduced); the edges of a group's windows stacked behind it
 * to its right, 1-3 for none, one or two (AppButtonStack, Lit while
 * active or hot). */
static const char *const TB2 = "TaskBand2";

bool xd_pkgstyle_draw_app_button(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                 const xd_app_button *ab) {
    if (t->pkg == nullptr || !xd_pkg_has_own(t->pkg, TB2, "AppButton"))
        return false;
    const char *comp = "TaskBand2Composited::TaskBand2";
    bool active = state & XD_STATE_CHECKED, pressed = state & XD_STATE_PRESSED;
    bool hot = state & XD_STATE_HOT, flash = (state & XD_STATE_FLASH) && !active;
    int stack = CLAMP(ab->windows - 1, 0, 2);
    xd_rect b = xd_rect_inset(r, xd_tpx(t, 1), xd_tpx(t, 1));
    if (ab->windows > 0 || hot || pressed || flash) {
        const char *edges = (active || hot) && xd_pkg_has_own(t->pkg, TB2, "AppButtonStackLit")
                                ? "AppButtonStackLit"
                                : "AppButtonStack";
        if (stack > 0 && xd_pkg_has_own(t->pkg, TB2, edges)) {
            b.w -= xd_tpx(t, stack == 1 ? 5 : 8);
            char es[8];
            state_str(es, stack + 1);
            (void)xd_pkg_draw(t, cr, pick(t, TB2, comp, edges), edges, es,
                              (xd_rect){b.x + b.w, b.y, xd_tpx(t, 8), b.h});
        }
        char st[8];
        state_str(st, flash    ? 7
                      : active ? (pressed ? 6 : hot ? 5 : 4)
                               : (pressed ? 3 : hot ? 2 : 1));
        (void)xd_pkg_draw(t, cr, pick(t, TB2, comp, "AppButton"), "AppButton", st, b);
        if (hot && !flash && ab->glow_x >= 0) {
            /* The icon's colour glowing up from under the pointer. */
            cairo_save(cr);
            cairo_rectangle(cr, b.x, b.y, b.w, b.h);
            cairo_clip(cr);
            double gx = CLAMP(ab->glow_x, b.x, b.x + b.w), gy = b.y + b.h;
            cairo_pattern_t *g = cairo_pattern_create_radial(gx, gy, 0, gx, gy, b.w * 0.85);
            cairo_pattern_add_color_stop_rgba(g, 0, ab->tint.r, ab->tint.g, ab->tint.b,
                                              pressed ? 0.45 : 0.65);
            cairo_pattern_add_color_stop_rgba(g, 1, ab->tint.r, ab->tint.g, ab->tint.b, 0);
            cairo_set_source(cr, g);
            cairo_paint(cr);
            cairo_pattern_destroy(g);
            cairo_restore(cr);
        }
    }
    if (ab->progress != XD_PROGRESS_NONE)
        xd_theme_draw_taskbar_progress(t, cr, b, ab->progress, ab->fraction);
    if (ab->icon == nullptr)
        return true;
    double sx = 1, sy = 1;
    cairo_surface_get_device_scale(ab->icon, &sx, &sy);
    int iw = (int)(cairo_image_surface_get_width(ab->icon) / sx);
    int ih = (int)(cairo_image_surface_get_height(ab->icon) / sy);
    int ix = ab->label != nullptr ? b.x + xd_tpx(t, 6) : b.x + (b.w - iw) / 2;
    cairo_set_source_surface(cr, ab->icon, ix, b.y + (b.h - ih) / 2);
    cairo_paint(cr);
    if (ab->label != nullptr) {
        xd_font_spec font = t->scheme.fonts[XD_FONT_MENU];
        (void)xd_pkg_font(t->pkg, TB2, nullptr, nullptr, "Font", &font, nullptr);
        xd_text_style ts =
            xd_text(t, XD_FONT_MENU, text_color_st(t, TB2, nullptr, nullptr, xd_rgba_hex(0xFFFFFF)),
                    XD_ALIGN_START);
        ts.font = &font;
        ts.shadow = true;
        ts.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
        int tx = ix + iw + xd_tpx(t, 6);
        xd_draw_text(cr, (xd_rect){tx, b.y, b.x + b.w - tx - xd_tpx(t, 4), b.h}, ab->label, &ts);
    }
    return true;
}

void xd_theme_draw_taskbar_progress(const xd_theme *t, cairo_t *cr, xd_rect r,
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
    if (kind == XD_PROGRESS_NONE || (size_t)kind >= G_N_ELEMENTS(kinds) || r.w <= 0)
        return;
    fraction = CLAMP(fraction, 0.0, 1.0);
    xd_rect f = {r.x, r.y, (int)lround(r.w * fraction), r.h};
    if (kind == XD_PROGRESS_INDETERMINATE) {
        /* A third of the button wide, sweeping in from the left. */
        int pw = MAX(r.w / 3, 1);
        f = (xd_rect){r.x - pw + (int)lround((r.w + pw) * fraction), r.y, pw, r.h};
    }
    if (f.w <= 0)
        return;
    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    if (t->pkg == nullptr || !xd_pkg_has_own(t->pkg, TB2, kinds[kind].part) ||
        !xd_pkg_draw(t, cr, TB2, kinds[kind].part, "1", f)) {
        xd_rgba c = xd_rgba_hex(kinds[kind].rgb);
        xd_fill_gradient(cr, f, xd_rgba_alpha(c, 0.7), xd_rgba_alpha(c, 0.45), false);
    }
    cairo_restore(cr);
}

/* ---- the notification area ---- */

bool xd_pkgstyle_clock_color(const xd_theme *t, xd_rgba *out) {
    return t->pkg != nullptr && xd_pkg_has_own(t->pkg, "TrayNotify::Clock", nullptr) &&
           xd_pkg_color(t->pkg, "TrayNotify::Clock", nullptr, nullptr, "TextColor", out);
}

bool xd_pkgstyle_draw_tray(const xd_theme *t, cairo_t *cr, xd_rect r, const char *clock,
                           xd_edge edge) {
    bool vert = edge == XD_EDGE_LEFT || edge == XD_EDGE_RIGHT;
    const char *plain = vert ? "TrayNotifyVert::TrayNotify" : "TrayNotifyHoriz::TrayNotify";
    if (t->pkg == nullptr || !xd_pkg_has_own(t->pkg, plain, "Background"))
        return false;
    const char *comp =
        vert ? "TrayNotifyVertComposited::TrayNotify" : "TrayNotifyHorizComposited::TrayNotify";
    (void)xd_pkg_draw(t, cr, pick(t, plain, comp, "Background"), "Background", "1", r);
    if (clock == nullptr || clock[0] == '\0')
        return true;
    xd_font_spec font = t->scheme.fonts[XD_FONT_MENU];
    (void)xd_pkg_font(t->pkg, "TrayNotify::Clock", nullptr, nullptr, "Font", &font, nullptr);
    xd_text_style ts = xd_text(t, XD_FONT_MENU, xd_theme_clock_color(t), XD_ALIGN_END);
    ts.font = &font;
    ts.lines = true; /* a tall taskbar's day and date under the time */
    ts.shadow = seven(t);
    ts.shadow_color = xd_rgba_alpha(xd_rgba_hex(0x000000), 0.8);
    int ml, mr;
    xd_theme_tray_margins(t, &ml, &mr);
    xd_draw_text(cr, xd_rect_inset4(r, ml, 0, mr, 0), clock, &ts);
    return true;
}

bool xd_theme_draw_tray_chevron(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                bool open, bool vertical) {
    if (t->pkg == nullptr)
        return false;
    char plain[48], comp[64];
    g_snprintf(plain, sizeof(plain), "TrayNotify%s%s::Button", vertical ? "Vert" : "Horiz",
               open ? "Open" : "");
    g_snprintf(comp, sizeof(comp), "TrayNotify%s%sComposited::Button",
               vertical ? "Vert" : "Horiz", open ? "Open" : "");
    if (!xd_pkg_has_own(t->pkg, plain, nullptr))
        return false;
    char st[8];
    basic_str(st, state);
    (void)xd_pkg_draw(t, cr, pick(t, plain, comp, nullptr), nullptr, st, r);
    return true;
}

bool xd_theme_draw_show_desktop(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                bool vertical) {
    const char *cls = vertical ? "VerticalShowDesktop::Button" : "ShowDesktop::Button";
    if (t->pkg == nullptr || !xd_pkg_has_own(t->pkg, cls, nullptr))
        return false;
    char st[8];
    basic_str(st, state);
    (void)xd_pkg_draw(t, cr, cls, nullptr, st, r);
    return true;
}

/* ---- Alt+Tab: AltTab (deduced names) ---- */

static const char *const ALTTAB = "AltTab";

bool xd_pkgstyle_has_switcher(const xd_theme *t) {
    return t->pkg != nullptr && xd_pkg_has_own(t->pkg, ALTTAB, "SelectedItem");
}

void xd_pkgstyle_draw_switcher_box(const xd_theme *t, cairo_t *cr, xd_rect r) {
    /* The class's own image: 7's box without glass (with it, 7 shows live
     * thumbnails on the glass; we show icons, as without). */
    if (!xd_pkg_draw(t, cr, ALTTAB, nullptr, nullptr, r))
        xd_fill_rect(cr, r, xd_rgba_from_rgb(t->scheme.colors[XD_COLOR_BUTTON_FACE]));
}

void xd_pkgstyle_draw_switcher_item(const xd_theme *t, cairo_t *cr, xd_rect r, bool selected,
                                    bool hot) {
    const char *part = selected && hot ? "HotSelectedItem"
                       : selected      ? "SelectedItem"
                       : hot           ? "HotItem"
                                       : nullptr;
    if (part != nullptr)
        (void)xd_pkg_draw(t, cr, ALTTAB, part, nullptr, r);
}

void xd_pkgstyle_draw_switcher_title(const xd_theme *t, cairo_t *cr, xd_rect r,
                                     const char *title) {
    if (title == nullptr || *title == '\0' || r.w <= 0)
        return;
    xd_rgba fg = xd_rgba_hex(0x000000);
    (void)xd_pkg_color(t->pkg, ALTTAB, "Title", nullptr, "TextColor", &fg);
    xd_text_style ts = xd_text(t, XD_FONT_CAPTION, fg, XD_ALIGN_START);
    xd_font_spec font;
    if (xd_pkg_font(t->pkg, ALTTAB, "Title", nullptr, "Font", &font, nullptr))
        ts.font = &font;
    ts.ellipsize = true;
    /* On glass, 7 sets the title on a white glow (TextGlowSize): TextGlow's
     * image round the text. */
    int glow = 0;
    if (xd_pkg_int(t->pkg, ALTTAB, "Title", nullptr, "TextGlowSize", &glow) && glow > 0 &&
        xd_pkg_has(t->pkg, "TextGlow", "Glow")) {
        PangoLayout *l = xd_text_layout(cr, title, &ts);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(l, &tw, &th);
        g_object_unref(l);
        int g = xd_tpx(t, glow) / 2;
        tw = MIN(tw, r.w);
        (void)xd_pkg_draw(t, cr, "TextGlow", "Glow", nullptr,
                          (xd_rect){r.x - g, r.y + (r.h - th) / 2 - g, tw + 2 * g, th + 2 * g});
    }
    xd_draw_text(cr, r, title, &ts);
}

/* ---- 7's thumbnails: TaskbandExtendedUI (deduced names) ---- */

static const char *const TEU = "TaskbandExtendedUI";

bool xd_pkgstyle_draw_thumb_panel(const xd_theme *t, cairo_t *cr, xd_rect r) {
    return t->pkg != nullptr && xd_pkg_has_own(t->pkg, TEU, "ThumbnailBackground") &&
           xd_pkg_draw(t, cr, TEU, "ThumbnailBackground", "1", r);
}

bool xd_pkgstyle_draw_thumb_tile(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    if (t->pkg == nullptr || !xd_pkg_has_own(t->pkg, TEU, "ThumbnailTile"))
        return false;
    /* Hot 1, Pressed 2 (deduced from the frames' look). */
    if (state & (XD_STATE_HOT | XD_STATE_PRESSED))
        (void)xd_pkg_draw(t, cr, TEU, "ThumbnailTile", (state & XD_STATE_PRESSED) ? "2" : "1",
                          r);
    return true;
}

bool xd_pkgstyle_thumb_title_color(const xd_theme *t, xd_rgba *out) {
    return t->pkg != nullptr && xd_pkg_has_own(t->pkg, TEU, "ThumbnailTitle") &&
           xd_pkg_color(t->pkg, TEU, "ThumbnailTitle", nullptr, "TextColor", out);
}

bool xd_pkgstyle_draw_thumb_close(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    if (t->pkg == nullptr || !xd_pkg_has_own(t->pkg, TEU, "ThumbnailClose"))
        return false;
    char st[8];
    basic_str(st, state);
    return xd_pkg_draw(t, cr, TEU, "ThumbnailClose", st, r);
}

/* ---- jump lists: our JumpList class, else 7's Start menu items ---- */

static const char *const JL = "JumpList";

bool xd_pkgstyle_draw_jumplist_frame(const xd_theme *t, cairo_t *cr, xd_rect r, int band_y) {
    if (t->pkg == nullptr || !xd_pkg_has_own(t->pkg, JL, "Background"))
        return false;
    (void)xd_pkg_draw(t, cr, JL, "Background", "1", r);
    if (band_y < r.y + r.h)
        (void)xd_pkg_draw(t, cr, JL, "Band", "1", (xd_rect){r.x, band_y, r.w, r.y + r.h - band_y});
    return true;
}

bool xd_pkgstyle_jumplist_header_color(const xd_theme *t, xd_rgba *out) {
    return t->pkg != nullptr && xd_pkg_has_own(t->pkg, JL, "Header") &&
           xd_pkg_color(t->pkg, JL, "Header", nullptr, "TextColor", out);
}

/* A hot jump list item or pin button: JumpList's, else what 7's Start menu
 * highlights its programs with (its jump lists are the same control). */
static bool jump_hot(const xd_theme *t, cairo_t *cr, const char *part, xd_rect r,
                     bool pressed) {
    if (xd_pkg_has_own(t->pkg, JL, part))
        return xd_pkg_draw(t, cr, JL, part, pressed ? "3" : "2", r);
    return xd_theme_draw_start_part(t, cr, XD_START_PROG_ITEM, r,
                                    pressed ? XD_STATE_PRESSED : XD_STATE_HOT);
}

bool xd_pkgstyle_draw_jumplist_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                    xd_rgba *text) {
    if (t->pkg == nullptr)
        return false;
    int w, h;
    bool own = xd_pkg_has_own(t->pkg, JL, "Item");
    if (!own && !xd_theme_start_part_size(t, XD_START_PROG_ITEM, XD_STATE_HOT, &w, &h))
        return false;
    bool disabled = state & XD_STATE_DISABLED;
    bool hot = (state & XD_STATE_HOT) && !disabled;
    if (hot)
        (void)jump_hot(t, cr, "Item", r, false);
    /* Item: Normal 1, Hot 2, Disabled 3. */
    const char *st = disabled ? "3" : hot ? "2" : "1";
    if (!own || !xd_pkg_color(t->pkg, JL, "Item", st, "TextColor", text))
        *text = xd_rgba_hex(disabled ? 0x6D6D6D : 0x1E1E1E);
    return true;
}

bool xd_pkgstyle_draw_jumplist_pin(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state) {
    return t->pkg != nullptr && jump_hot(t, cr, "PinButton", r, state & XD_STATE_PRESSED);
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
};

/* The class drawing a part: 7's StartPanelPriv, then StartPanel (XP's
 * and Vista's); their composited variants with the glass. nullptr when
 * the package has neither. */
static const char *start_class(const xd_theme *t, xd_start_part part) {
    if (t->pkg == nullptr || (unsigned)part >= XD_START_PART_COUNT)
        return nullptr;
    const char *name = start_names[part];
    if (seven(t) && xd_pkg_has_own(t->pkg, "StartPanelPriv", name))
        return pick(t, "StartPanelPriv", "StartPanelComposited::StartPanelPriv", name);
    if (xd_pkg_has_own(t->pkg, "StartPanel", name))
        return pick(t, "StartPanel", "StartPanelComposited::StartPanel", name);
    return nullptr;
}

bool xd_theme_draw_start_part(const xd_theme *t, cairo_t *cr, xd_start_part part, xd_rect r,
                              uint8_t state) {
    const char *cls = start_class(t, part);
    if (cls == nullptr)
        return false;
    char st[8];
    basic_str(st, state);
    return xd_pkg_draw(t, cr, cls, start_names[part], st, r);
}

bool xd_theme_start_part_size(const xd_theme *t, xd_start_part part, uint8_t state, int *w,
                              int *h) {
    const char *cls = start_class(t, part);
    char st[8];
    basic_str(st, state);
    return cls != nullptr && xd_pkg_part_size(t, cls, start_names[part], st, w, h);
}

bool xd_theme_draw_start_logoff_icon(const xd_theme *t, cairo_t *cr, xd_rect r, bool power,
                                     uint8_t state) {
    const char *cls = start_class(t, XD_START_LOGOFF_BUTTONS);
    char st[8];
    basic_str(st, state);
    int w, h;
    if (cls == nullptr || !xd_pkg_part_size(t, cls, "LogoffButtons", st, &w, &h) || h <= 0)
        return false;
    int squares = MAX(w / h, 1);
    int i = MAX(power ? squares - 1 : squares - 2, 0);
    xd_rect icon = {r.x + (r.w - h) / 2, r.y + (r.h - h) / 2, h, h};
    cairo_save(cr);
    cairo_rectangle(cr, icon.x, icon.y, icon.w, icon.h);
    cairo_clip(cr);
    bool drew =
        xd_pkg_draw(t, cr, cls, "LogoffButtons", st, (xd_rect){icon.x - i * h, icon.y, w, h});
    cairo_restore(cr);
    return drew;
}

bool xd_theme_start_part_color(const xd_theme *t, xd_start_part part, uint8_t state,
                               xd_start_color which, xd_rgba *out) {
    static const char *const props[] = {
        [XD_START_TEXT] = "TextColor",
        [XD_START_SUBTEXT] = "CaptionText",
        [XD_START_SHADOW] = "TextShadowColor",
    };
    const char *cls = start_class(t, part);
    if (cls == nullptr || (unsigned)which >= G_N_ELEMENTS(props))
        return false;
    char st[8];
    basic_str(st, state);
    return xd_pkg_color(t->pkg, cls, start_names[part], st, props[which], out);
}

bool xd_theme_start_part_font(const xd_theme *t, xd_start_part part, xd_font_spec *out) {
    const char *cls = start_class(t, part);
    return cls != nullptr && xd_pkg_font(t->pkg, cls, start_names[part], nullptr, "Font", out, nullptr);
}

bool xd_theme_start_heading_color(const xd_theme *t, xd_rgba *out) {
    return t->pkg != nullptr && xd_pkg_has_own(t->pkg, "StartMenu::ListView", "GroupHeader") &&
           xd_pkg_color(t->pkg, "StartMenu::ListView", "GroupHeader", nullptr,
                        "Heading1TextColor", out);
}

bool xd_theme_draw_new_program(const xd_theme *t, cairo_t *cr, xd_rect r) {
    return t->pkg != nullptr && xd_pkg_has_own(t->pkg, "StartMenu::MenuBand", "NewAppButton") &&
           xd_pkg_draw(t, cr, "StartMenu::MenuBand", "NewAppButton", "1", r);
}

/* ---- the Start menus' cascades ----
 * XP's and 7's: StartMenu::Toolbar's background and Button, ::MenuBand's
 * separator ("Seperator", as XP spells it). The Windows 2000 Start menu:
 * our ClassicStartMenu (Background, Banner, Item, Separator,
 * SubmenuArrow; docs/THEMES.md). */

static const char *const SMT = "StartMenu::Toolbar";
static const char *const CSM = "ClassicStartMenu";

static bool start_look(const xd_theme *t) {
    return t->pkg != nullptr && t->menu_look == XD_MENU_LOOK_START;
}

static bool classic_look(const xd_theme *t) {
    return t->pkg != nullptr && t->menu_look == XD_MENU_LOOK_CLASSIC_START;
}

bool xd_pkgstyle_draw_start_menu_frame(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (start_look(t) && xd_pkg_has_own(t->pkg, SMT, nullptr) &&
        xd_pkg_prop(t->pkg, SMT, nullptr, nullptr, "ImageFile") != nullptr) {
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_MENU));
        return xd_pkg_draw(t, cr, SMT, nullptr, "1", r);
    }
    if (classic_look(t) && xd_pkg_has_own(t->pkg, CSM, "Background")) {
        xd_fill_rect(cr, r, xd_sys(t, XD_COLOR_MENU));
        return xd_pkg_draw(t, cr, CSM, "Background", "1", r);
    }
    return false;
}

bool xd_pkgstyle_draw_start_menu_item(const xd_theme *t, cairo_t *cr, xd_rect r, uint8_t state,
                                      xd_rgba *text) {
    bool hot = state & XD_STATE_HOT, disabled = state & XD_STATE_DISABLED;
    if (start_look(t) && xd_pkg_has_own(t->pkg, SMT, "Button")) {
        /* Toolbar buttons: Hot 2; the text Toolbar(Hot)'s. */
        if (hot)
            (void)xd_pkg_draw(t, cr, SMT, "Button", "2", r);
        *text = disabled ? xd_sys(t, XD_COLOR_GRAY_TEXT)
                         : text_color_st(t, SMT, nullptr, hot ? "2" : "1",
                                         xd_sys(t, hot ? XD_COLOR_HILIGHT_TEXT
                                                       : XD_COLOR_MENU_TEXT));
        return true;
    }
    if (classic_look(t) && xd_pkg_has_own(t->pkg, CSM, "Item")) {
        /* Normal 1, Hot 2, Disabled 3, DisabledHot 4, as Menu's PopupItem. */
        char st[8];
        state_str(st, disabled ? (hot ? 4 : 3) : (hot ? 2 : 1));
        (void)xd_pkg_draw(t, cr, CSM, "Item", st, r);
        *text = text_color_st(t, CSM, "Item", st,
                              xd_sys(t, disabled ? XD_COLOR_GRAY_TEXT
                                        : hot    ? XD_COLOR_HILIGHT_TEXT
                                                 : XD_COLOR_MENU_TEXT));
        return true;
    }
    return false;
}

bool xd_pkgstyle_draw_start_menu_separator(const xd_theme *t, cairo_t *cr, xd_rect r) {
    if (start_look(t)) {
        static const char *const parts[] = {"Seperator", "Separator"};
        for (size_t i = 0; i < G_N_ELEMENTS(parts); i++)
            if (xd_pkg_has_own(t->pkg, "StartMenu::MenuBand", parts[i]))
                return xd_pkg_draw(t, cr, "StartMenu::MenuBand", parts[i], "1", r);
    }
    return classic_look(t) && xd_pkg_has_own(t->pkg, CSM, "Separator") &&
           xd_pkg_draw(t, cr, CSM, "Separator", "1", r);
}

bool xd_pkgstyle_draw_start_menu_submenu(const xd_theme *t, cairo_t *cr, xd_rect r,
                                         uint8_t state) {
    if (!classic_look(t) || !xd_pkg_has_own(t->pkg, CSM, "SubmenuArrow"))
        return false;
    /* Normal 1, Hot 2, Disabled 3. */
    const char *st = (state & XD_STATE_DISABLED) ? "3" : (state & XD_STATE_HOT) ? "2" : "1";
    return xd_pkg_draw(t, cr, CSM, "SubmenuArrow", st, r);
}

bool xd_pkgstyle_draw_start_banner(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rgba *text,
                                   xd_font_spec *font) {
    if (!classic_look(t) || !xd_pkg_has_own(t->pkg, CSM, "Banner"))
        return false;
    (void)xd_pkg_draw(t, cr, CSM, "Banner", "1", r);
    (void)xd_pkg_color(t->pkg, CSM, "Banner", nullptr, "TextColor", text);
    (void)xd_pkg_font(t->pkg, CSM, "Banner", nullptr, "Font", font, nullptr);
    return true;
}

/* ---- Explorer's panes ------------------------------------------------------------ *
 * XP's task pane from ExplorerBar (with what an import of shellstyle.dll
 * adds: shell/NAME.xml), and Vista's and 7's command bar, address box,
 * tree items and details pane. See panes.h; panes.c draws without a
 * package. */

static const char *const EXPLORERBAR = "ExplorerBar";

bool xd_pkgstyle_has_taskpane(const xd_theme *t) {
    return t->pkg != nullptr && xd_pkg_has(t->pkg, EXPLORERBAR, "NormalGroupHead");
}

static int scaled_px(const xd_theme *t, int v) { return (int)lround(v * t->scale); }

static xd_insets scaled_insets(const xd_theme *t, xd_insets m) {
    return (xd_insets){scaled_px(t, m.left), scaled_px(t, m.top), scaled_px(t, m.right),
                       scaled_px(t, m.bottom)};
}

/* A BorderFill as DirectUI draws its boxes: a fill or a gradient, and a
 * border whose sides may differ (BorderThickness, our own: uxtheme's
 * BorderSize is the same all round). Anything else, as uxtheme draws it. */
static bool draw_box(const xd_theme *t, cairo_t *cr, const char *cls, const char *part,
                     const char *state, xd_rect r) {
    const xd_pkg *p = t->pkg;
    const char *bg = xd_pkg_prop(p, cls, part, state, "BgType");
    if (bg == nullptr || g_ascii_strcasecmp(bg, "BorderFill") != 0)
        return xd_pkg_draw(t, cr, cls, part, state, r);
    xd_rgba fill, g1, g2;
    const char *type = xd_pkg_prop(p, cls, part, state, "FillType");
    bool gradient = type != nullptr && g_ascii_strcasecmp(type, "Solid") != 0 &&
                    xd_pkg_color(p, cls, part, state, "GradientColor1", &g1) &&
                    xd_pkg_color(p, cls, part, state, "GradientColor2", &g2);
    if (gradient)
        xd_fill_gradient(cr, r, g1, g2, g_ascii_strcasecmp(type, "HorzGradient") == 0);
    else if (xd_pkg_color(p, cls, part, state, "FillColor", &fill))
        xd_fill_rect(cr, r, fill);
    xd_rgba bc;
    if (!xd_pkg_color(p, cls, part, state, "BorderColor", &bc))
        return true;
    xd_insets b;
    int size = 0;
    if (xd_pkg_margins(p, cls, part, state, "BorderThickness", &b)) {
        b = scaled_insets(t, b);
    } else if (xd_pkg_int(p, cls, part, state, "BorderSize", &size) && size > 0) {
        int px = xd_tpx(t, size);
        b = (xd_insets){px, px, px, px};
    } else {
        return true;
    }
    if (b.left > 0)
        xd_fill_rect(cr, (xd_rect){r.x, r.y, b.left, r.h}, bc);
    if (b.right > 0)
        xd_fill_rect(cr, (xd_rect){r.x + r.w - b.right, r.y, b.right, r.h}, bc);
    if (b.top > 0)
        xd_fill_rect(cr, (xd_rect){r.x, r.y, r.w, b.top}, bc);
    if (b.bottom > 0)
        xd_fill_rect(cr, (xd_rect){r.x, r.y + r.h - b.bottom, r.w, b.bottom}, bc);
    return true;
}

static void group_part(char *out, size_t cap, bool special, const char *what) {
    g_snprintf(out, cap, "%sGroup%s", special ? "Special" : "Normal", what);
}

static int font_px(const xd_theme *t, const xd_font_spec *f) {
    /* A line of text: the font's em at 96 DPI and its leading. */
    double px = ceil(f->size_pt * 96.0 / 72.0 * 1.2 * t->scale);
    return (int)px;
}

void xd_pkgstyle_taskpane_style(const xd_theme *t, xd_taskpane_style *s) {
    const xd_pkg *p = t->pkg;
    if (!xd_pkgstyle_has_taskpane(t))
        return;
    xd_insets m;
    int n;
    if (xd_pkg_margins(p, EXPLORERBAR, nullptr, nullptr, "ContentMargins", &m))
        s->padding = scaled_insets(t, m);
    if (xd_pkg_int(p, EXPLORERBAR, nullptr, nullptr, "GroupSpacing", &n))
        s->group_spacing = scaled_px(t, n);
    for (int sp = 0; sp < 2; sp++) {
        char head[40], body[40];
        group_part(head, sizeof(head), sp, "Head");
        group_part(body, sizeof(body), sp, "Background");
        if (xd_pkg_margins(p, EXPLORERBAR, body, nullptr, "ContentMargins", &m))
            s->body[sp] = scaled_insets(t, m);
        if (xd_pkg_int(p, EXPLORERBAR, body, nullptr, "LinkSpacing", &n))
            s->link_spacing[sp] = scaled_px(t, n);
        if (xd_pkg_int(p, EXPLORERBAR, body, nullptr, "IconSpacing", &n))
            s->icon_gap[sp] = scaled_px(t, n);
        (void)xd_pkg_font(p, EXPLORERBAR, head, nullptr, "Font", &s->header_font[sp], nullptr);
        for (int hot = 0; hot < 2; hot++) {
            const char *st = hot ? "Hot" : nullptr;
            (void)xd_pkg_color(p, EXPLORERBAR, head, st, "TextColor", &s->header_color[sp][hot]);
            (void)xd_pkg_color(p, EXPLORERBAR, body, st, "TextColor", &s->link_color[sp][hot]);
            (void)xd_pkg_font(p, EXPLORERBAR, body, st, "Font", &s->link_font[sp][hot],
                              &s->link_underline[sp][hot]);
        }
    }
    /* The title bar: as tall as its image, its arrow or its title with
     * its margins, whichever is taller. */
    int h = 0, w, ih;
    if (xd_pkg_part_size(t, EXPLORERBAR, "NormalGroupHead", nullptr, &w, &ih))
        h = ih;
    xd_insets hm = {0};
    if (xd_pkg_margins(p, EXPLORERBAR, "NormalGroupHead", nullptr, "ContentMargins", &hm))
        hm = scaled_insets(t, hm);
    int vm = hm.top + hm.bottom;
    if (xd_pkg_part_size(t, EXPLORERBAR, "NormalGroupExpand", "1", &w, &ih))
        h = MAX(h, ih + vm);
    h = MAX(h, font_px(t, &s->header_font[0]) + vm);
    if (h > 0)
        s->header_height = h;
    (void)xd_pkg_font(p, "TaskPane", "Details", nullptr, "Font", &s->details_font, nullptr);
    (void)xd_pkg_font(p, "TaskPane", "DetailsName", nullptr, "Font", &s->details_name_font,
                      nullptr);
    (void)xd_pkg_color(p, "TaskPane", "Details", nullptr, "TextColor", &s->details_color);
    if (xd_pkg_int(p, "TaskPane", "Details", nullptr, "LinkSpacing", &n))
        s->details_spacing = scaled_px(t, n);
}

bool xd_pkgstyle_draw_taskpane(const xd_theme *t, cairo_t *cr, xd_rect r) {
    return xd_pkgstyle_has_taskpane(t) && draw_box(t, cr, EXPLORERBAR, nullptr, nullptr, r);
}

bool xd_pkgstyle_draw_taskpane_header(const xd_theme *t, cairo_t *cr, const xd_taskpane_style *s,
                                      xd_rect r, bool special, bool expanded, uint8_t state,
                                      const char *title) {
    if (!xd_pkgstyle_has_taskpane(t))
        return false;
    char head[40], arrow[40];
    group_part(head, sizeof(head), special, "Head");
    group_part(arrow, sizeof(arrow), special, expanded ? "Collapse" : "Expand");
    (void)draw_box(t, cr, EXPLORERBAR, head, nullptr, r);
    xd_rect c = xd_pkg_content_rect(t, EXPLORERBAR, head, nullptr, r);
    bool hot = state & (XD_STATE_HOT | XD_STATE_PRESSED);
    const char *st = (state & XD_STATE_PRESSED) ? "3" : hot ? "2" : "1";
    int aw = 0, ah = 0;
    if (xd_pkg_part_size(t, EXPLORERBAR, arrow, st, &aw, &ah)) {
        /* Right-aligned in the content, or where an Offset from the top
         * right corner puts it (7's): its left edge or its right edge. */
        xd_rect a = {c.x + c.w - aw, c.y + (c.h - ah) / 2, aw, ah};
        int off[2];
        const char *v = xd_pkg_prop(t->pkg, EXPLORERBAR, arrow, st, "Offset");
        if (v != nullptr && sscanf(v, "%d %d", &off[0], &off[1]) == 2) {
            int ox = scaled_px(t, off[0]);
            a.x = r.x + r.w + ox - (ox > -aw ? aw : 0);
            a.y = r.y + scaled_px(t, off[1]);
        }
        (void)xd_pkg_draw(t, cr, EXPLORERBAR, arrow, st, a);
        c.w = a.x - c.x - xd_tpx(t, 2);
    }
    if (title != nullptr && c.w > 0) {
        xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, s->header_color[special][hot],
                                   XD_ALIGN_START);
        ts.font = &s->header_font[special];
        xd_draw_text(cr, c, title, &ts);
    }
    if (state & XD_STATE_FOCUSED)
        xd_theme_draw_focus_rect(t, cr, xd_rect_inset(c, 0, xd_tpx(t, 2)));
    return true;
}

bool xd_pkgstyle_draw_taskpane_body(const xd_theme *t, cairo_t *cr, xd_rect r, bool special) {
    if (!xd_pkgstyle_has_taskpane(t))
        return false;
    char body[40];
    group_part(body, sizeof(body), special, "Background");
    (void)draw_box(t, cr, EXPLORERBAR, body, nullptr, r);
    return true;
}

/* Vista's and 7's parts: the class, the part (named in uxtheme.ini's
 * [Deduced ...] sections) and the state's number. */
static bool explorer_part(const xd_theme *t, xd_explorer_part part, uint8_t state,
                          const char **cls, const char **name, char st[8]) {
    int n = 1;
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
        n = (state & XD_STATE_DISABLED)  ? 6
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
        n = (state & XD_STATE_DISABLED)  ? 3
            : (state & XD_STATE_FOCUSED) ? 4
            : (state & XD_STATE_HOT)     ? 2
                                         : 1;
        break;
    case XD_EXPLORER_TREE_ITEM:
        /* Normal, Hot, Selected, Disabled, SelectedNotFocus, HotSelected. */
        *cls = "Explorer::TreeView";
        *name = "TreeItem";
        n = (state & XD_STATE_DISABLED) ? 4
            : (state & XD_STATE_CHECKED)
                ? ((state & XD_STATE_HOT) ? 6 : (state & XD_STATE_FOCUSED) ? 3 : 5)
            : (state & XD_STATE_HOT) ? 2
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
    g_snprintf(st, 8, "%d", n);
    return t->pkg != nullptr && xd_pkg_has(t->pkg, *cls, *name);
}

bool xd_pkgstyle_draw_explorer_part(const xd_theme *t, cairo_t *cr, xd_explorer_part part,
                                    uint8_t state, xd_rect r) {
    const char *cls, *name;
    char st[8];
    if (!explorer_part(t, part, state, &cls, &name, st))
        return false;
    if (part == XD_EXPLORER_COMMAND_BAR) {
        /* 7's bar: a gradient under a translucent sheen (Vista's sheen is
         * the whole bar). */
        xd_rgba g[3];
        if (xd_pkg_color(t->pkg, cls, "ModuleGradient", nullptr, "GradientColor1", &g[0]) &&
            xd_pkg_color(t->pkg, cls, "ModuleGradient", nullptr, "GradientColor2", &g[1]) &&
            xd_pkg_color(t->pkg, cls, "ModuleGradient", nullptr, "GradientColor3", &g[2])) {
            static const double stops[] = {0.0, 0.5, 1.0};
            xd_fill_vstops(cr, r, stops, g, 3);
        }
    }
    /* A button or tree row at rest (or a button disabled: 7 has no image
     * for it, only a text colour) shows what's behind it. */
    if ((part == XD_EXPLORER_COMMAND_BUTTON || part == XD_EXPLORER_TREE_ITEM) &&
        (strcmp(st, "1") == 0 || (part == XD_EXPLORER_COMMAND_BUTTON && strcmp(st, "6") == 0)))
        return true;
    (void)xd_pkg_draw(t, cr, cls, name, st, r);
    return true;
}

bool xd_pkgstyle_explorer_text_color(const xd_theme *t, xd_explorer_part part, uint8_t state,
                                     xd_rgba *out) {
    const char *cls, *name;
    char st[8];
    if (!explorer_part(t, part, state, &cls, &name, st))
        return false;
    if (part == XD_EXPLORER_COMMAND_BAR)
        return xd_pkg_color(t->pkg, cls, "TaskButton", "1", "TextColor", out);
    if (part == XD_EXPLORER_DETAILS_PANE)
        return xd_pkg_color(t->pkg, cls, "MetadataLabel", nullptr, "TextColor", out);
    return xd_pkg_color(t->pkg, cls, name, st, "TextColor", out);
}
