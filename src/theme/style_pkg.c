/* SPDX-License-Identifier: MIT
 * What a theme package draws in place of its base style: window frames
 * and caption buttons, from uxtheme's Window class, as XP's non-client
 * painting does. See pkg.h.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static int part_w(const xd_theme *t, const char *part, int fallback) {
    int w, h;
    return xd_pkg_part_size(t, WINDOW, part, "Active", &w, &h) ? w : fallback;
}

static int part_h(const xd_theme *t, const char *part, int fallback) {
    int w, h;
    return xd_pkg_part_size(t, WINDOW, part, "Active", &w, &h) ? h : fallback;
}

xd_insets xd_pkgstyle_frame_insets(const xd_theme *t, const xd_frame *f) {
    int top = part_h(t, f->maximized ? "MaxCaption" : "Caption", xd_tpx(t, 30));
    if (f->maximized)
        return (xd_insets){0, top, 0, 0};
    return (xd_insets){part_w(t, "FrameLeft", xd_tpx(t, 4)), top,
                       part_w(t, "FrameRight", xd_tpx(t, 4)),
                       part_h(t, "FrameBottom", xd_tpx(t, 4))};
}

/* A part's Offset, scaled: where it goes from the corner its OffsetType
 * names. */
static bool offset(const xd_theme *t, const char *part, int *x, int *y, bool *right) {
    int n[2];
    const char *v = xd_pkg_prop(t->pkg, WINDOW, part, nullptr, "Offset");
    if (v == nullptr || sscanf(v, "%d %d", &n[0], &n[1]) != 2)
        return false;
    const char *type = xd_pkg_prop(t->pkg, WINDOW, part, nullptr, "OffsetType");
    *right = type != nullptr && g_ascii_strncasecmp(type, "TopRight", 8) == 0;
    *x = (int)lround(n[0] * t->scale);
    *y = (int)lround(n[1] * t->scale);
    return true;
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
    /* Offsets are from the window's own corners; a maximized window's
     * borders are pushed off the screen, as Windows does. */
    int border = f->maximized ? part_w(t, "FrameLeft", xd_tpx(t, 4)) : 0;
    int right = outer.x + outer.w + border, top = outer.y - border;
    int bw = xd_tpx(t, 21), bh = bw;
    int w, h;
    if (xd_pkg_part_size(t, WINDOW, "CloseButton", "1", &w, &h)) {
        bw = w;
        bh = h;
    }
    /* Where Windows puts them: square buttons the caption bar's height
     * less 4, centred in the bar below the frame's top edge, 2 px in from
     * the right border and 2 px apart. A style's Offsets are used only
     * where they make sense (Luna's agree with this; WE's don't). */
    int bar = in.top - xd_tpx(t, 4);
    int cbh = 0;
    if (xd_pkg_sys_int(t->pkg, "CaptionBarHeight", &cbh) && cbh > 0)
        bar = MIN(xd_tpx(t, cbh), in.top);
    int frame_top = f->maximized ? 0 : in.top - bar;
    int size = MAX(bar - xd_tpx(t, 4), xd_tpx(t, 8));
    if (bh > size || bh <= 0) {
        bw = bh = size;
    }
    int cx = -(f->maximized ? 0 : in.right) - xd_tpx(t, 2) - bw - border;
    int cy = frame_top + (bar - bh) / 2 + border;
    int step = bw + xd_tpx(t, 2);
    int ox, oy, mx, my;
    bool o_right, m_right;
    if (offset(t, "CloseButton", &ox, &oy, &o_right) && o_right && ox <= -bw && oy >= 0 &&
        oy + bh <= in.top + border) {
        cx = ox;
        cy = oy;
        if (offset(t, "MaxButton", &mx, &my, &m_right) && m_right && ox - mx >= bw &&
            ox - mx <= 2 * bw)
            step = ox - mx;
    }
    int bx = right + cx, by = MAX(top + cy, outer.y);
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
