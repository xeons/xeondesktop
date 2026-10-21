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

bool xd_pkgstyle_has_frame(const xd_theme *t) {
    return t->pkg != nullptr && xd_pkg_has(t->pkg, WINDOW, "Caption");
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
    int cx, cy, mx, my;
    bool cr, mr;
    if (!offset(t, "CloseButton", &cx, &cy, &cr) || !cr) {
        cx = -bw - xd_tpx(t, 4);
        cy = xd_tpx(t, 5);
    }
    int step = bw + xd_tpx(t, 3);
    if (offset(t, "MaxButton", &mx, &my, &mr) && mr && cx - mx > 0)
        step = cx - mx;
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
    (void)xd_pkg_draw(t, cr, BUTTON, "GroupBox", "1",
                      (xd_rect){r.x, top, r.w, r.h - (top - r.y)});
    if (label != nullptr && label[0] != '\0') {
        xd_rgba fg = text_color(t, BUTTON, "GroupBox", 1, xd_sys(t, XD_COLOR_BUTTON_TEXT));
        xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, fg, XD_ALIGN_START);
        int tw = xd_text_width(cr, label, &ts);
        xd_rect gap = {r.x + xd_tpx(t, 7), r.y, tw + xd_tpx(t, 4), th};
        xd_fill_rect(cr, gap, xd_theme_tab_page_color(t));
        xd_draw_text(cr, xd_rect_inset4(gap, xd_tpx(t, 2), 0, 0, 0), label, &ts);
    }
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

bool xd_pkgstyle_draw_vscrollbar(const xd_theme *t, cairo_t *cr, xd_rect r, xd_rect thumb,
                                 bool disabled) {
    const char *const SB = "ScrollBar";
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, SB, "ArrowBtn"))
        return false;
    int arrow = MIN(r.w, r.h / 2);
    xd_rect up = {r.x, r.y, r.w, arrow};
    xd_rect down = {r.x, r.y + r.h - arrow, r.w, arrow};
    const char *track_state = disabled ? "4" : "1";
    if (thumb.h > 0) {
        (void)xd_pkg_draw(t, cr, SB, "UpperTrackVert", track_state,
                          (xd_rect){r.x, up.y + up.h, r.w, thumb.y - (up.y + up.h)});
        (void)xd_pkg_draw(t, cr, SB, "LowerTrackVert", track_state,
                          (xd_rect){r.x, thumb.y + thumb.h, r.w, down.y - (thumb.y + thumb.h)});
    } else {
        (void)xd_pkg_draw(t, cr, SB, "UpperTrackVert", track_state,
                          (xd_rect){r.x, up.y + up.h, r.w, down.y - (up.y + up.h)});
    }
    /* ArrowBtn: Up 1-4, Down 5-8 (Normal, Hot, Pressed, Disabled). */
    (void)xd_pkg_draw(t, cr, SB, "ArrowBtn", disabled ? "4" : "1", up);
    (void)xd_pkg_draw(t, cr, SB, "ArrowBtn", disabled ? "8" : "5", down);
    if (thumb.h > 0) {
        (void)xd_pkg_draw(t, cr, SB, "ThumbBtnVert", "1", thumb);
        (void)xd_pkg_draw(t, cr, SB, "GripperVert", "1", thumb);
    }
    return true;
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
