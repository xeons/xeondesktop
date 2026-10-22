/* SPDX-License-Identifier: MIT
 * libxdui: windows, widgets and combo drop-down lists.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../core/alloc.h"
#include "ui_internal.h"

enum {
    XD_LIST_ROW_H = 17,
    XD_LIST_MAX_ROWS = 8,
    XD_TEXT_PAD = 2,
    XD_TAB_H = 21,    /* tab strip; the panel starts 2px higher */
    XD_LIST_BORDER = 2,
    XD_SCROLL_W = 16,
};

/* ---- geometry ------------------------------------------------------------------ */

static int dev(double logical, double scale) {
    double rounded = round(logical * scale);
    XD_ASSERT(rounded > -65536.0 && rounded < 65536.0);
    return (int)rounded;
}

/* Logical -> device with shared edges, so neighbours never gap or overlap. */
static xd_rect to_device(xd_rect rect, double scale) {
    int x0 = dev(rect.x, scale);
    int y0 = dev(rect.y, scale);
    return (xd_rect){x0, y0, dev(rect.x + rect.w, scale) - x0, dev(rect.y + rect.h, scale) - y0};
}

static bool contains(xd_rect rect, double x, double y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

static void window_theme(const xd_window *win, xd_theme *theme) {
    xd_theme_init(theme, win->ui->style, &win->ui->scheme, win->scale);
}

/* Content rect inside a field's border (matches xd_theme_draw_field). */
static xd_rect field_inner(const xd_theme *theme, xd_rect rect) {
    int border = theme->style == XD_STYLE_CLASSIC ? 2 * xd_tpx(theme, 1) : xd_tpx(theme, 1);
    return xd_rect_inset(rect, border, border);
}

/* Where a combo's arrow button goes, inside the field border. */
static xd_rect dropdown_rect(const xd_theme *theme, xd_rect inner) {
    int bw = xd_theme_dropdown_button_width(theme);
    return (xd_rect){inner.x + inner.w - bw, inner.y, bw, inner.h};
}

/* Device rect of the editable text area of an edit or editable combo. */
static xd_rect text_area(const xd_theme *theme, const xd_widget *widget) {
    xd_rect inner = field_inner(theme, to_device(widget->r, theme->scale));
    if (widget->kind == XD_W_COMBO)
        inner.w -= xd_theme_dropdown_button_width(theme);
    return inner;
}

static bool is_text_widget(const xd_widget *widget) {
    return widget->kind == XD_W_EDIT || (widget->kind == XD_W_COMBO && widget->editable);
}

/* Hidden when it belongs to a tab page that isn't selected, or was hidden. */
static bool is_visible(const xd_widget *widget) {
    const xd_widget *tabs = widget->win->tabs;
    return !widget->hidden &&
           (widget->page < 0 || tabs == nullptr || tabs->selected == widget->page);
}

static bool is_focusable(const xd_widget *widget) {
    return widget->enabled && is_visible(widget) &&
           (widget->kind == XD_W_BUTTON || widget->kind == XD_W_EDIT ||
            widget->kind == XD_W_COMBO || widget->kind == XD_W_TABS ||
            widget->kind == XD_W_CHECKBOX || widget->kind == XD_W_LISTBOX ||
            widget->kind == XD_W_SLIDER || widget->kind == XD_W_RADIO);
}

/* Drawn differently under the pointer, so hovering it needs a redraw. */
static bool draws_hot(const xd_widget *widget) {
    return widget != nullptr && (widget->kind == XD_W_BUTTON || widget->kind == XD_W_CHECKBOX ||
                                 widget->kind == XD_W_COMBO || widget->kind == XD_W_RADIO);
}

/* ---- radio button groups ---------------------------------------------------------- */

enum { RADIO_GROUP_START = 1u << 0 }; /* xd_widget.flags of a radio button */

static size_t widget_index(const xd_window *win, const xd_widget *widget) {
    for (size_t i = 0; i < win->count; i++) {
        if (win->widgets[i] == widget)
            return i;
    }
    XD_ASSERT(false);
    return 0;
}

/* The radio buttons of w's group: widgets [*first, *end). A group starts at
 * a button made with `group` set, or after any other kind of widget. */
static void radio_group(const xd_widget *widget, size_t *first, size_t *end) {
    const xd_window *win = widget->win;
    size_t i = widget_index(win, widget);
    size_t group_start = i;
    while (group_start > 0 && !(win->widgets[group_start]->flags & RADIO_GROUP_START) &&
           win->widgets[group_start - 1]->kind == XD_W_RADIO)
        group_start--;
    size_t group_end = i + 1;
    while (group_end < win->count && win->widgets[group_end]->kind == XD_W_RADIO &&
           !(win->widgets[group_end]->flags & RADIO_GROUP_START))
        group_end++;
    *first = group_start;
    *end = group_end;
}

/* Tab stops at a group's checked button (or its first, if none is), as in
 * a Windows dialog; the arrow keys reach the others. */
static bool is_tab_stop(const xd_widget *widget) {
    if (!is_focusable(widget))
        return false;
    if (widget->kind != XD_W_RADIO || widget->checked)
        return true;
    size_t first;
    size_t end;
    radio_group(widget, &first, &end);
    const xd_widget *stop = nullptr;
    for (size_t i = first; i < end; i++) {
        const xd_widget *radio = widget->win->widgets[i];
        if (radio->checked && is_focusable(radio))
            return false;
        if (stop == nullptr && is_focusable(radio))
            stop = radio;
    }
    return stop == widget;
}

/* Checks w and unchecks the rest of its group. Returns whether w changed. */
static bool radio_check(xd_widget *widget) {
    size_t first;
    size_t end;
    radio_group(widget, &first, &end);
    bool changed = !widget->checked;
    for (size_t i = first; i < end; i++)
        widget->win->widgets[i]->checked = widget->win->widgets[i] == widget;
    widget->win->dirty = true;
    return changed;
}

/* Takes clicks without taking keyboard focus. */
static bool is_clickable(const xd_widget *widget) {
    return widget->kind == XD_W_CUSTOM && widget->pointer != nullptr && widget->enabled &&
           is_visible(widget);
}

/* ---- slider geometry (logical units) -------------------------------------------- */

enum { SLIDER_THUMB_W = 11, SLIDER_THUMB_H = 21, SLIDER_PAD = 2 };

static int slider_x0(const xd_widget *widget) {
    return widget->r.x + SLIDER_PAD + SLIDER_THUMB_W / 2;
}
static int slider_x1(const xd_widget *widget) {
    return widget->r.x + widget->r.w - SLIDER_PAD - SLIDER_THUMB_W / 2 - 1;
}

static double slider_pos(const xd_widget *widget, int value) {
    int range = XD_MAX(widget->smax - widget->smin, 1);
    return slider_x0(widget) +
           (double)(value - widget->smin) * (slider_x1(widget) - slider_x0(widget)) / range;
}

static int slider_value_at(const xd_widget *widget, double x) {
    int span = XD_MAX(slider_x1(widget) - slider_x0(widget), 1);
    double fraction = XD_CLAMP((x - slider_x0(widget)) / span, 0.0, 1.0);
    return widget->smin + (int)lround(fraction * (widget->smax - widget->smin));
}

static xd_rect slider_thumb(const xd_widget *widget) {
    int cx = (int)lround(slider_pos(widget, widget->svalue));
    return (xd_rect){cx - SLIDER_THUMB_W / 2, widget->r.y + SLIDER_PAD, SLIDER_THUMB_W,
                     SLIDER_THUMB_H};
}

static void draw_slider(const xd_theme *theme, cairo_t *cairo, const xd_widget *widget,
                        bool focused) {
    double scale = theme->scale;
    int px = xd_tpx(theme, 1);

    /* Channel: a groove the thumb rides along. */
    xd_rect thumb_l = slider_thumb(widget);
    int cy = thumb_l.y + 8;
    xd_rect channel = to_device(
        (xd_rect){slider_x0(widget) - 2, cy, slider_x1(widget) - slider_x0(widget) + 5, 4}, scale);

    /* Ticks under the thumb, one per step when they fit. */
    int steps = widget->smax - widget->smin;
    if (steps > 0 && steps <= 40) {
        xd_rgba tick = xd_theme_trackbar_tics_color(theme, !widget->enabled);
        for (int tick_value = widget->smin; tick_value <= widget->smax; tick_value++) {
            double x = slider_pos(widget, tick_value);
            xd_rect tr =
                to_device((xd_rect){(int)lround(x), thumb_l.y + SLIDER_THUMB_H + 2, 1, 3}, scale);
            tr.w = px;
            xd_fill_rect(cairo, tr, tick);
        }
    }

    uint8_t state = widget->enabled ? 0 : XD_STATE_DISABLED;
    if (focused)
        state |= XD_STATE_FOCUSED;
    if (widget->win->hot == widget)
        state |= XD_STATE_HOT;
    if (widget->win->pressed == widget)
        state |= XD_STATE_PRESSED;
    xd_theme_draw_trackbar(theme, cairo, channel, to_device(thumb_l, scale), state);
    if (focused)
        xd_theme_draw_focus_rect(theme, cairo, to_device(widget->r, scale));
}

static void slider_changed(xd_widget *widget, int value) {
    value = XD_CLAMP(value, widget->smin, widget->smax);
    if (value == widget->svalue)
        return;
    widget->svalue = value;
    widget->win->dirty = true;
    if (widget->cb != nullptr)
        widget->cb(widget, widget->user); /* may close the window */
}

/* ---- vertical scroll bars (logical units) ----------------------------------------- */

/* Where a press on a scroll bar lands. */
typedef enum sb_part : uint8_t {
    SB_NONE,
    SB_LINE_UP,
    SB_LINE_DOWN,
    SB_PAGE_UP,
    SB_PAGE_DOWN,
    SB_THUMB,
} sb_part;

static int sb_arrow(xd_rect sb) { return XD_MIN(sb.w, sb.h / 2); }

/* The thumb of scroll bar `sb` showing `page` of `total` rows from `first`,
 * measured as the theme draws it. */
static xd_rect sb_thumb(const xd_ui *ui, xd_rect sb, int first, int page, int total) {
    xd_theme theme;
    xd_theme_init(&theme, ui->style, &ui->scheme, 1.0);
    return xd_theme_vscrollbar_thumb(&theme, sb, first, page, total);
}

static sb_part sb_hit(const xd_ui *ui, xd_rect sb, int first, int page, int total, double y) {
    int arrow = sb_arrow(sb);
    if (y < sb.y + arrow)
        return SB_LINE_UP;
    if (y >= sb.y + sb.h - arrow)
        return SB_LINE_DOWN;
    xd_rect thumb = sb_thumb(ui, sb, first, page, total);
    if (y < thumb.y)
        return SB_PAGE_UP;
    if (y >= thumb.y + thumb.h)
        return SB_PAGE_DOWN;
    return SB_THUMB;
}

/* The first row that puts the thumb's top at `thumb_y` (thumb dragging). */
static int sb_first_at(const xd_ui *ui, xd_rect sb, int first, int page, int total,
                       double thumb_y) {
    xd_rect thumb = sb_thumb(ui, sb, first, page, total);
    int arrow = sb_arrow(sb);
    double track_h = sb.h - 2 * arrow - thumb.h;
    int range = total - page;
    if (track_h <= 0 || range <= 0)
        return first;
    double pos = (thumb_y - (sb.y + arrow)) / track_h;
    return (int)lround(XD_CLAMP(pos, 0.0, 1.0) * range);
}

/* The first row after a click on `part` (not the thumb). */
static int sb_step(sb_part part, int first, int page) {
    switch (part) {
    case SB_LINE_UP:
        return first - 1;
    case SB_LINE_DOWN:
        return first + 1;
    case SB_PAGE_UP:
        return first - page;
    case SB_PAGE_DOWN:
        return first + page;
    case SB_NONE:
    case SB_THUMB:
        break;
    }
    return first;
}

/* ---- list box and tab geometry (logical units) ---------------------------------- */

static int list_rows(const xd_widget *widget) {
    return XD_MAX((widget->r.h - 2 * XD_LIST_BORDER) / XD_LIST_ROW_H, 1);
}

static bool list_has_scrollbar(const xd_widget *widget) {
    return (int)widget->count > list_rows(widget);
}

static xd_rect list_scrollbar(const xd_widget *widget) {
    return (xd_rect){widget->r.x + widget->r.w - XD_LIST_BORDER - XD_SCROLL_W,
                     widget->r.y + XD_LIST_BORDER, XD_SCROLL_W, widget->r.h - 2 * XD_LIST_BORDER};
}

static xd_rect list_row(const xd_widget *widget, int row) {
    int sw = list_has_scrollbar(widget) ? XD_SCROLL_W : 0;
    return (xd_rect){widget->r.x + XD_LIST_BORDER,
                     widget->r.y + XD_LIST_BORDER + row * XD_LIST_ROW_H,
                     widget->r.w - 2 * XD_LIST_BORDER - sw, XD_LIST_ROW_H};
}

static void list_clamp(xd_widget *widget) {
    int max_first = XD_MAX((int)widget->count - list_rows(widget), 0);
    widget->first = XD_CLAMP(widget->first, 0, max_first);
}

static void list_scroll_to(xd_widget *widget, int index) {
    if (index < widget->first)
        widget->first = index;
    if (index >= widget->first + list_rows(widget))
        widget->first = index - list_rows(widget) + 1;
    list_clamp(widget);
}

static xd_rect tab_rect(const xd_widget *widget, size_t i) {
    return (xd_rect){widget->r.x + widget->tab_x[i], widget->r.y, widget->tab_w[i], XD_TAB_H};
}

static xd_rect tab_panel(const xd_widget *widget) {
    return (xd_rect){widget->r.x, widget->r.y + XD_TAB_H - 2, widget->r.w,
                     widget->r.h - XD_TAB_H + 2};
}

static xd_text_style field_text_style(const xd_theme *theme, xd_rgba color) {
    xd_text_style st = {
        .font = &theme->scheme.fonts[XD_FONT_MESSAGE],
        .scale = theme->scale,
        .color = color,
        .align = XD_ALIGN_START,
        .smoothing = theme->scheme.font_smoothing,
    };
    return st;
}

/* ---- text fields ----------------------------------------------------------------- */

static const char *edit_cstr(xd_widget *widget) { return xd_strbuf_cstr(&widget->edit.text); }

static int index_x(PangoLayout *layout, size_t index) {
    PangoRectangle pos;
    pango_layout_index_to_pos(layout, (int)XD_MIN(index, (size_t)INT32_MAX), &pos);
    return pos.x / PANGO_SCALE;
}

static void draw_text_field(xd_window *win, const xd_theme *theme, cairo_t *cairo,
                            xd_widget *widget, xd_rect area) {
    xd_field_colors field_colors = xd_theme_field_colors(theme);
    bool focused = win->focus == widget && win->active;
    int pad = xd_tpx(theme, XD_TEXT_PAD);
    int avail = XD_MAX(area.w - 2 * pad, 1);

    xd_text_style st =
        field_text_style(theme, widget->enabled ? field_colors.text : field_colors.disabled_text);
    PangoLayout *layout = xd_text_layout(cairo, edit_cstr(widget), &st);
    int tw = 0;
    int th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);

    /* Scroll so the caret stays visible, and don't leave blank space on
     * the right when text is deleted. */
    int caret = index_x(layout, widget->edit.cursor);
    if (caret - widget->scroll > avail)
        widget->scroll = caret - avail;
    if (caret - widget->scroll < 0)
        widget->scroll = caret;
    if (tw - widget->scroll < avail)
        widget->scroll = XD_MAX(0, tw - avail);

    double x0 = area.x + pad - widget->scroll;
    double y0 = area.y + (area.h - th) / 2;
    cairo_save(cairo);
    cairo_rectangle(cairo, area.x, area.y, area.w, area.h);
    cairo_clip(cairo);
    xd_set_color(cairo, st.color);
    cairo_move_to(cairo, x0, y0);
    pango_cairo_show_layout(cairo, layout);

    if (focused && xd_textedit_has_selection(&widget->edit)) {
        size_t selection_start;
        size_t selection_end;
        xd_textedit_selection(&widget->edit, &selection_start, &selection_end);
        double sx0 = x0 + index_x(layout, selection_start);
        double sx1 = x0 + index_x(layout, selection_end);
        xd_fill_rect(cairo, (xd_rect){(int)sx0, (int)y0, (int)(sx1 - sx0), th},
                     field_colors.selection);
        cairo_rectangle(cairo, sx0, y0, sx1 - sx0, th);
        cairo_clip(cairo);
        xd_set_color(cairo, field_colors.selection_text);
        cairo_move_to(cairo, x0, y0);
        pango_cairo_show_layout(cairo, layout);
    } else if (focused) {
        xd_fill_rect(cairo, (xd_rect){(int)(x0 + caret), (int)y0, XD_MAX(xd_tpx(theme, 1), 1), th},
                     field_colors.text);
    }
    cairo_restore(cairo);
    g_object_unref(layout);
}

/* Byte position in the text under window-local logical x. */
static size_t text_pos_at(xd_window *win, xd_widget *widget, double lx) {
    xd_theme theme;
    window_theme(win, &theme);
    xd_rect area = text_area(&theme, widget);
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(scratch);
    xd_text_style st = field_text_style(&theme, xd_rgba_hex(0));
    PangoLayout *layout = xd_text_layout(cairo, edit_cstr(widget), &st);

    double x = lx * win->scale - area.x - xd_tpx(&theme, XD_TEXT_PAD) + widget->scroll;
    int index = 0;
    int trailing = 0;
    pango_layout_xy_to_index(layout, (int)(x * PANGO_SCALE), 0, &index, &trailing);
    g_object_unref(layout);
    cairo_destroy(cairo);
    cairo_surface_destroy(scratch);

    xd_textedit tmp = widget->edit; /* only the cursor fields change */
    xd_textedit_set_cursor(&tmp, (size_t)XD_MAX(index, 0), false);
    for (int i = 0; i < trailing; i++)
        xd_textedit_move(&tmp, XD_MOTION_RIGHT, false);
    return tmp.cursor;
}

/* ---- drawing ------------------------------------------------------------------------ */

static void draw_widget(xd_window *win, const xd_theme *theme, cairo_t *cairo, xd_widget *widget) {
    xd_rect rect = to_device(widget->r, theme->scale);
    bool focused = win->focus == widget && win->active;
    switch (widget->kind) {
    case XD_W_LABEL:
        xd_theme_draw_label(theme, cairo, rect, widget->text, widget->flags & XD_LABEL_WRAP,
                            widget->flags & XD_LABEL_BOLD,
                            !widget->enabled || (widget->flags & XD_LABEL_DIM));
        break;
    case XD_W_IMAGE: {
        int px = XD_MIN(rect.w, rect.h);
        cairo_surface_t *icon = xd_icons_get(win->ui->icons, widget->text, px);
        if (icon != nullptr) {
            cairo_set_source_surface(cairo, icon, rect.x + (rect.w - px) / 2,
                                     rect.y + (rect.h - px) / 2);
            cairo_paint(cairo);
        }
        break;
    }
    case XD_W_BANNER:
        xd_theme_draw_dialog_banner(theme, cairo, rect, widget->text,
                                    widget->text2 != nullptr ? widget->text2 : "");
        break;
    case XD_W_BUTTON: {
        uint8_t state = 0;
        /* The focused button is the default while it has focus. */
        bool is_default = win->focus != nullptr && win->focus->kind == XD_W_BUTTON
                              ? win->focus == widget
                              : win->def == widget;
        if (is_default)
            state |= XD_STATE_DEFAULT;
        if (focused)
            state |= XD_STATE_FOCUSED;
        if (win->hot == widget)
            state |= XD_STATE_HOT;
        if (win->pressed == widget && win->hot == widget)
            state |= XD_STATE_PRESSED;
        if (!widget->enabled)
            state |= XD_STATE_DISABLED;
        xd_theme_draw_button(theme, cairo, rect, state, widget->text);
        break;
    }
    case XD_W_EDIT: {
        xd_theme_draw_field(theme, cairo, rect, !widget->enabled);
        draw_text_field(win, theme, cairo, widget, text_area(theme, widget));
        break;
    }
    case XD_W_TABS: {
        xd_theme_draw_tab_panel(theme, cairo, to_device(tab_panel(widget), theme->scale));
        /* Unselected tabs first; the selected one overlaps its neighbours
         * and the panel's top edge. */
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < widget->count; i++) {
                bool sel = (int)i == widget->selected;
                if (sel != (pass == 1))
                    continue;
                xd_rect tr = tab_rect(widget, i);
                if (sel)
                    tr = (xd_rect){tr.x - 2, tr.y, tr.w + 4, tr.h};
                uint8_t state = sel ? XD_STATE_CHECKED : 0;
                if (sel && focused)
                    state |= XD_STATE_FOCUSED;
                xd_theme_draw_tab(theme, cairo, to_device(tr, theme->scale), state,
                                  widget->items[i]);
            }
        }
        break;
    }
    case XD_W_CHECKBOX:
    case XD_W_RADIO: {
        uint8_t state = widget->checked ? XD_STATE_CHECKED : 0;
        if (focused)
            state |= XD_STATE_FOCUSED;
        if (win->hot == widget)
            state |= XD_STATE_HOT;
        if (win->pressed == widget && win->hot == widget)
            state |= XD_STATE_PRESSED;
        if (!widget->enabled)
            state |= XD_STATE_DISABLED;
        if (widget->kind == XD_W_RADIO)
            xd_theme_draw_radio(theme, cairo, rect, state, widget->text);
        else
            xd_theme_draw_checkbox(theme, cairo, rect, state, widget->text);
        break;
    }
    case XD_W_LISTBOX: {
        xd_theme_draw_field(theme, cairo, rect, !widget->enabled);
        int rows = list_rows(widget);
        for (int i = 0; i < rows && widget->first + i < (int)widget->count; i++) {
            int idx = widget->first + i;
            xd_rect row = to_device(list_row(widget, i), theme->scale);
            xd_theme_draw_list_item(theme, cairo, row, idx == widget->selected, widget->items[idx]);
            if (idx == widget->selected && focused)
                xd_theme_draw_focus_rect(theme, cairo, row);
        }
        if (list_has_scrollbar(widget))
            xd_theme_draw_vscrollbar(theme, cairo, to_device(list_scrollbar(widget), theme->scale),
                                     widget->first, rows, (int)widget->count);
        break;
    }
    case XD_W_GROUPBOX:
        xd_theme_draw_groupbox(theme, cairo, rect, widget->text);
        break;
    case XD_W_SLIDER:
        draw_slider(theme, cairo, widget, focused);
        break;
    case XD_W_CUSTOM:
        if (widget->draw != nullptr) {
            cairo_save(cairo);
            cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
            cairo_clip(cairo);
            widget->draw(widget, cairo, rect, theme, widget->user);
            cairo_restore(cairo);
        }
        break;
    case XD_W_COMBO: {
        xd_rect inner = xd_theme_draw_field(theme, cairo, rect, !widget->enabled);
        xd_rect button = dropdown_rect(theme, inner);
        uint8_t bstate = 0;
        if (win->popup != nullptr && win->popup->combo == widget)
            bstate |= XD_STATE_PRESSED;
        if (win->hot == widget)
            bstate |= XD_STATE_HOT;
        if (!widget->enabled)
            bstate |= XD_STATE_DISABLED;
        xd_theme_draw_dropdown_button(theme, cairo, button, bstate);
        if (widget->editable) {
            draw_text_field(win, theme, cairo, widget, text_area(theme, widget));
        } else {
            /* Drop-down list: the selection shows highlighted while focused. */
            xd_rect area = {inner.x, inner.y, inner.w - button.w, inner.h};
            int pad = xd_tpx(theme, 1);
            xd_rect item = xd_rect_inset(area, pad, pad);
            const char *text = xd_widget_text(widget);
            if (focused) {
                xd_theme_draw_list_item(theme, cairo, item, true, text);
                xd_theme_draw_focus_rect(theme, cairo, item);
            } else {
                xd_theme_draw_list_item(theme, cairo, item, false, text);
            }
        }
        break;
    }
    }
}

/* ---- frame pacing ------------------------------------------------------------------ */

/* A surface draws at most once per frame the compositor shows: changes made
 * while a frame is pending (a slider dragged faster than the display) wait
 * for its done event and go out together, instead of queueing buffers the
 * compositor would only drop. */
static void frame_done(void *data, struct wl_callback *cb, [[maybe_unused]] uint32_t time) {
    struct wl_callback **slot = data;
    wl_callback_destroy(cb);
    *slot = nullptr;
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

/* Asks for the next frame's done event; before the commit it belongs to. */
static void request_frame(struct wl_surface *surface, struct wl_callback **slot) {
    *slot = wl_surface_frame(surface);
    wl_callback_add_listener(*slot, &frame_listener, slot);
}

static void cancel_frame(struct wl_callback **slot) {
    if (*slot != nullptr)
        wl_callback_destroy(*slot);
    *slot = nullptr;
}

void xd_window_render(xd_window *win) {
    xd_ui *ui = win->ui;
    if (win->frame != nullptr)
        return; /* stays dirty until the frame is done */
    double scale = win->scale;
    int dw = dev(win->width, scale);
    int dh = dev(win->height, scale);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&win->ring, ui->shm, dw, dh);
    if (buf == nullptr)
        return;

    xd_theme theme;
    window_theme(win, &theme);
    cairo_t *cairo = cairo_create(buf->surface);
    xd_fill_rect(cairo, (xd_rect){0, 0, dw, dh}, xd_theme_dialog_color(&theme));
    for (size_t i = 0; i < win->count; i++) {
        if (is_visible(win->widgets[i]))
            draw_widget(win, &theme, cairo, win->widgets[i]);
    }
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);

    wl_surface_attach(win->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(win->surface, 0, 0, dw, dh);
    if (win->viewport != nullptr)
        wp_viewport_set_destination(win->viewport, win->width, win->height);
    request_frame(win->surface, &win->frame);
    wl_surface_commit(win->surface);
    buf->busy = true;
    win->dirty = false;
}

/* ---- drop-down list popup ------------------------------------------------------- */

static void popup_close(xd_window *win);
static void tip_hide(xd_window *win);

static bool popup_has_scrollbar(const xd_listpopup *popup) {
    return (int)popup->combo->count > popup->rows;
}

/* Inside the 1px border, on the right. */
static xd_rect popup_scrollbar(const xd_listpopup *popup) {
    return (xd_rect){popup->width - 1 - XD_SCROLL_W, 1, XD_SCROLL_W, popup->height - 2};
}

static int popup_row_w(const xd_listpopup *popup) {
    return popup->width - 2 - (popup_has_scrollbar(popup) ? XD_SCROLL_W : 0);
}

static void popup_set_first(xd_listpopup *popup, int first) {
    first = XD_CLAMP(first, 0, XD_MAX((int)popup->combo->count - popup->rows, 0));
    if (first != popup->first) {
        popup->first = first;
        popup->dirty = true;
    }
}

static void popup_scroll_to(xd_listpopup *popup, int index) {
    int first = popup->first;
    if (index < first)
        first = index;
    if (index >= first + popup->rows)
        first = index - popup->rows + 1;
    popup_set_first(popup, first);
}

/* The drop-down list's rows and scroll bar, in its 1px border. */
static void draw_list(const xd_listpopup *popup, const xd_theme *theme, cairo_t *cairo,
                      xd_rect all) {
    double scale = popup->scale;
    xd_fill_rect(cairo, all, xd_sys(theme, XD_COLOR_WINDOW));
    xd_rgba border = theme->style == XD_STYLE_CLASSIC ? xd_sys(theme, XD_COLOR_WINDOW_FRAME)
                                                      : xd_rgba_hex(0x7F9DB9);
    xd_draw_bevel(cairo, all, xd_tpx(theme, 1), border, border);
    for (int i = 0; i < popup->rows; i++) {
        int idx = popup->first + i;
        if (idx >= (int)popup->combo->count)
            break;
        xd_rect row = to_device(
            (xd_rect){1, 1 + i * XD_LIST_ROW_H, popup_row_w(popup), XD_LIST_ROW_H}, scale);
        xd_theme_draw_list_item(theme, cairo, row, idx == popup->hot, popup->combo->items[idx]);
    }
    if (popup_has_scrollbar(popup))
        xd_theme_draw_vscrollbar(theme, cairo, to_device(popup_scrollbar(popup), scale),
                                 popup->first, popup->rows, (int)popup->combo->count);
}

void xd_popup_render(xd_listpopup *popup) {
    xd_ui *ui = popup->owner->ui;
    if (popup->frame != nullptr)
        return;
    double scale = popup->scale;
    int dw = dev(popup->width, scale);
    int dh = dev(popup->height, scale);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&popup->ring, ui->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme theme;
    xd_theme_init(&theme, ui->style, &ui->scheme, scale);
    cairo_t *cairo = cairo_create(buf->surface);

    xd_rect all = {0, 0, dw, dh};
    if (popup->draw != nullptr)
        popup->draw(popup->anchor, cairo, all, &theme, popup->user);
    else
        draw_list(popup, &theme, cairo, all);
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);

    wl_surface_attach(popup->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(popup->surface, 0, 0, dw, dh);
    if (popup->viewport != nullptr)
        wp_viewport_set_destination(popup->viewport, popup->width, popup->height);
    request_frame(popup->surface, &popup->frame);
    wl_surface_commit(popup->surface);
    buf->busy = true;
    popup->dirty = false;
}

static void popup_xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    xd_listpopup *popup = data;
    xdg_surface_ack_configure(xdg, serial);
    popup->configured = true;
    popup->dirty = true;
}

static const struct xdg_surface_listener popup_xdg_listener = {.configure = popup_xdg_configure};

static void popup_configure(void *data, [[maybe_unused]] struct xdg_popup *popup_role, int32_t px,
                            int32_t py, [[maybe_unused]] int32_t width,
                            [[maybe_unused]] int32_t height) {
    xd_listpopup *popup = data;
    popup->x = px;
    popup->y = py;
}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *popup_role) {
    xd_listpopup *popup = data;
    popup_close(popup->owner);
}

static void popup_repositioned([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_popup *popup_role,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static void popup_scale(void *data,
                        [[maybe_unused]] struct wp_fractional_scale_v1 *fractional_scale,
                        uint32_t scale_120) {
    xd_listpopup *popup = data;
    double scale = scale_120 / 120.0;
    if (scale >= 0.5 && scale <= 8.0 && scale != popup->scale) {
        popup->scale = scale;
        popup->dirty = true;
    }
}

static const struct wp_fractional_scale_v1_listener popup_fractional_listener = {
    .preferred_scale = popup_scale,
};

/* Maps a popup of p->width x p->height under p->anchor, with a grab. */
static void popup_map(xd_window *win, xd_listpopup *popup) {
    xd_ui *ui = win->ui;
    xd_widget *anchor = popup->anchor;
    popup->surface = wl_compositor_create_surface(ui->compositor);
    if (ui->fractional_manager != nullptr && ui->viewporter != nullptr) {
        popup->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            ui->fractional_manager, popup->surface);
        wp_fractional_scale_v1_add_listener(popup->fractional, &popup_fractional_listener, popup);
        popup->viewport = wp_viewporter_get_viewport(ui->viewporter, popup->surface);
    }
    popup->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm_base, popup->surface);
    xdg_surface_add_listener(popup->xdg_surface, &popup_xdg_listener, popup);

    struct xdg_positioner *pos = xdg_wm_base_create_positioner(ui->wm_base);
    xdg_positioner_set_size(pos, popup->width, popup->height);
    xdg_positioner_set_anchor_rect(pos, anchor->r.x, anchor->r.y, anchor->r.w, anchor->r.h);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos,
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    popup->popup = xdg_surface_get_popup(popup->xdg_surface, win->xdg_surface, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(popup->popup, &popup_listener, popup);
    uint32_t serial = XD_MAX(ui->pointer_serial, ui->key_serial);
    if (ui->seat != nullptr && serial != 0)
        xdg_popup_grab(popup->popup, ui->seat, serial);
    wl_surface_commit(popup->surface);

    win->popup = popup;
    win->dirty = true;
}

static void popup_open(xd_window *win, xd_widget *combo) {
    if (combo->count == 0 || win->popup != nullptr)
        return;
    tip_hide(win);
    xd_listpopup *popup = XD_NEW(xd_listpopup);
    popup->owner = win;
    popup->combo = combo;
    popup->anchor = combo;
    popup->scale = win->scale;
    popup->rows = (int)XD_MIN(combo->count, (size_t)XD_LIST_MAX_ROWS);
    popup->width = combo->r.w;
    popup->height = popup->rows * XD_LIST_ROW_H + 2;
    popup->hot = XD_MAX(combo->selected, 0);
    popup_scroll_to(popup, popup->hot);
    popup_map(win, popup);
}

void xd_popup_custom(xd_widget *anchor, int width, int height, xd_draw_cb draw,
                     xd_pointer_cb pointer, void *user) {
    xd_window *win = anchor->win;
    if (win->popup != nullptr)
        return;
    tip_hide(win);
    xd_listpopup *popup = XD_NEW(xd_listpopup);
    popup->owner = win;
    popup->anchor = anchor;
    popup->draw = draw;
    popup->pointer = pointer;
    popup->user = user;
    popup->scale = win->scale;
    popup->width = width;
    popup->height = height;
    popup_map(win, popup);
}

void xd_popup_dismiss(xd_window *win) { popup_close(win); }

static void popup_close(xd_window *win) {
    xd_listpopup *popup = win->popup;
    if (popup == nullptr)
        return;
    win->popup = nullptr;
    win->dirty = true;
    if (win->ui->pointer_popup == popup)
        win->ui->pointer_popup = nullptr;
    xd_ui_remove_source(popup->sb_repeat);
    cancel_frame(&popup->frame);
    xdg_popup_destroy(popup->popup);
    xdg_surface_destroy(popup->xdg_surface);
    if (popup->viewport != nullptr)
        wp_viewport_destroy(popup->viewport);
    if (popup->fractional != nullptr)
        wp_fractional_scale_v1_destroy(popup->fractional);
    wl_surface_destroy(popup->surface);
    xd_shm_ring_finish(&popup->ring);
    xd_free(popup);
}

static void combo_changed(xd_widget *widget) {
    widget->win->dirty = true;
    if (widget->cb != nullptr)
        widget->cb(widget, widget->user);
}

/* Picks an item from the open list; may run callbacks that close the
 * window, so callers must not touch `win` afterwards. */
static void popup_choose(xd_window *win, int index) {
    xd_widget *combo = win->popup->combo;
    popup_close(win);
    if (index >= 0 && (size_t)index < combo->count && index != combo->selected) {
        xd_combo_select(combo, index);
        combo_changed(combo);
    }
}

xd_listpopup *xd_popup_for_surface(xd_ui *ui, struct wl_surface *surface) {
    XD_LIST_FOR_EACH(win, &ui->windows, xd_window, link) {
        if (win->popup != nullptr && win->popup->surface == surface)
            return win->popup;
    }
    return nullptr;
}

static int popup_row_at(const xd_listpopup *popup, double x, double y) {
    if (x >= 1 + popup_row_w(popup))
        return -1; /* on the scroll bar */
    double row_index = floor((y - 1) / (double)XD_LIST_ROW_H);
    if (!(row_index >= 0 && row_index < (double)XD_LIST_MAX_ROWS))
        return -1;
    int row = (int)row_index;
    if (row < 0 || row >= popup->rows || popup->first + row >= (int)popup->combo->count)
        return -1;
    return popup->first + row;
}

static sb_part popup_sb_hit(const xd_listpopup *popup, double y) {
    return sb_hit(popup->owner->ui, popup_scrollbar(popup), popup->first, popup->rows,
                  (int)popup->combo->count, y);
}

/* A held arrow or track scrolls on, as Windows' does: the track only while
 * the pointer is still on the same side of the thumb. */
static void popup_sb_repeat(void *user) {
    xd_listpopup *popup = user;
    sb_part part = popup->sb_pressed;
    if (popup->sb_repeat != nullptr && !popup->sb_repeat->repeat)
        popup->sb_repeat = xd_ui_add_timer(popup->owner->ui, 50, true, popup_sb_repeat, popup);
    if (popup_sb_hit(popup, popup->pointer_y) == part)
        popup_set_first(popup, sb_step(part, popup->first, popup->rows));
}

static void popup_sb_release(xd_listpopup *popup) {
    popup->sb_pressed = SB_NONE;
    xd_ui_remove_source(popup->sb_repeat);
    popup->sb_repeat = nullptr;
}

void xd_popup_pointer_motion(xd_listpopup *popup, double x, double y) {
    if (popup->combo == nullptr) {
        popup->dirty = true;
        if (popup->pointer != nullptr)
            popup->pointer(popup->anchor, XD_POINTER_MOTION, x, y, popup->user);
        return;
    }
    popup->pointer_y = y;
    if (popup->sb_pressed == SB_THUMB) {
        popup_set_first(popup,
                        sb_first_at(popup->owner->ui, popup_scrollbar(popup), popup->first,
                                    popup->rows, (int)popup->combo->count, y - popup->sb_offset));
        return;
    }
    if (popup->sb_pressed != SB_NONE)
        return;
    int hot = popup_row_at(popup, x, y);
    if (hot >= 0 && hot != popup->hot) {
        popup->hot = hot;
        popup->dirty = true;
    }
}

void xd_popup_pointer_button(xd_listpopup *popup, uint32_t button, bool pressed) {
    if (button != BTN_LEFT)
        return;
    xd_ui *ui = popup->owner->ui;
    double x = ui->pointer_x;
    double y = ui->pointer_y;
    if (popup->combo == nullptr) {
        popup->dirty = true;
        if (popup->pointer != nullptr) /* may close the popup, or the window */
            popup->pointer(popup->anchor, pressed ? XD_POINTER_PRESS : XD_POINTER_RELEASE, x, y,
                           popup->user);
        return;
    }
    popup->pointer_y = y;
    if (pressed) {
        /* A drag whose release went to another program ends here. */
        popup_sb_release(popup);
        if (!popup_has_scrollbar(popup) || !contains(popup_scrollbar(popup), x, y))
            return;
        sb_part part = popup_sb_hit(popup, y);
        popup->sb_pressed = part;
        if (part == SB_THUMB) {
            xd_rect thumb = sb_thumb(ui, popup_scrollbar(popup), popup->first, popup->rows,
                                     (int)popup->combo->count);
            popup->sb_offset = y - thumb.y;
        } else {
            popup_set_first(popup, sb_step(part, popup->first, popup->rows));
            xd_ui_remove_source(popup->sb_repeat);
            popup->sb_repeat = xd_ui_add_timer(ui, 400, false, popup_sb_repeat, popup);
        }
        return;
    }
    if (popup->sb_pressed != SB_NONE) {
        popup_sb_release(popup); /* the list stays open */
        return;
    }
    int row = popup_row_at(popup, x, y);
    if (row >= 0)
        popup_choose(popup->owner, row);
}

void xd_popup_pointer_axis(xd_listpopup *popup, double value) {
    if (popup->combo == nullptr)
        return;
    popup_set_first(popup, popup->first + (value > 0 ? 1 : -1));
}

/* ---- tooltips ----------------------------------------------------------------------- */

/* As in Windows (TTM_SETDELAYTIME's defaults): shown after half a second's
 * hover, gone after five; 4px of padding, the scheme's InfoWindow colours
 * and a border in InfoText, like the taskbar's. */
enum { TIP_INITIAL_MS = 500, TIP_AUTOPOP_MS = 5000, TIP_PAD_X = 4, TIP_BELOW_POINTER = 20 };

static char *copy_str(const char *text);

static xd_text_style tip_text_style(const xd_theme *theme) {
    return xd_text(theme, XD_FONT_TOOLTIP, xd_sys(theme, XD_COLOR_INFO_TEXT), XD_ALIGN_START);
}

void xd_tooltip_render(xd_tooltip *tip) {
    if (tip->frame != nullptr)
        return;
    xd_ui *ui = tip->owner->ui;
    double scale = tip->scale;
    int dw = dev(tip->width, scale);
    int dh = dev(tip->height, scale);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&tip->ring, ui->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme theme;
    xd_theme_init(&theme, ui->style, &ui->scheme, scale);
    cairo_t *cairo = cairo_create(buf->surface);
    xd_theme_draw_tooltip(&theme, cairo, (xd_rect){0, 0, dw, dh}, tip->text);
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);

    wl_surface_attach(tip->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(tip->surface, 0, 0, dw, dh);
    if (tip->viewport != nullptr)
        wp_viewport_set_destination(tip->viewport, tip->width, tip->height);
    request_frame(tip->surface, &tip->frame);
    wl_surface_commit(tip->surface);
    buf->busy = true;
    tip->dirty = false;
}

static void tip_xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    xd_tooltip *tip = data;
    xdg_surface_ack_configure(xdg, serial);
    tip->configured = true;
    tip->dirty = true;
}

static const struct xdg_surface_listener tip_xdg_listener = {.configure = tip_xdg_configure};

static void tip_popup_done(void *data, [[maybe_unused]] struct xdg_popup *popup_role) {
    tip_hide(((xd_tooltip *)data)->owner);
}

static void tip_popup_configure([[maybe_unused]] void *data,
                                [[maybe_unused]] struct xdg_popup *popup_role,
                                [[maybe_unused]] int32_t px, [[maybe_unused]] int32_t py,
                                [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}

static const struct xdg_popup_listener tip_popup_listener = {
    .configure = tip_popup_configure,
    .popup_done = tip_popup_done,
    .repositioned = popup_repositioned,
};

static void tip_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *fractional_scale,
                      uint32_t scale_120) {
    xd_tooltip *tip = data;
    double scale = scale_120 / 120.0;
    if (scale >= 0.5 && scale <= 8.0 && scale != tip->scale) {
        tip->scale = scale;
        tip->dirty = true;
    }
}

static const struct wp_fractional_scale_v1_listener tip_fractional_listener = {
    .preferred_scale = tip_scale,
};

static void tip_cancel_timer(xd_window *win) {
    xd_ui_remove_source(win->tip_timer);
    win->tip_timer = nullptr;
}

/* Takes the tip down; it shows again only once the pointer rests on
 * another widget. */
static void tip_hide(xd_window *win) {
    tip_cancel_timer(win);
    xd_tooltip *tip = win->tip;
    if (tip == nullptr)
        return;
    win->tip = nullptr;
    cancel_frame(&tip->frame);
    xdg_popup_destroy(tip->popup);
    xdg_surface_destroy(tip->xdg_surface);
    if (tip->viewport != nullptr)
        wp_viewport_destroy(tip->viewport);
    if (tip->fractional != nullptr)
        wp_fractional_scale_v1_destroy(tip->fractional);
    wl_surface_destroy(tip->surface);
    xd_shm_ring_finish(&tip->ring);
    xd_free(tip->text);
    xd_free(tip);
}

static void tip_autopop(void *user) {
    xd_window *win = user;
    win->tip_timer = nullptr; /* a one-shot, already finished */
    tip_hide(win);
}

static void tip_show(void *user) {
    xd_window *win = user;
    win->tip_timer = nullptr;
    xd_ui *ui = win->ui;
    xd_widget *widget = win->tip_widget;
    if (widget == nullptr || widget->tooltip == nullptr || win->popup != nullptr ||
        win->tip != nullptr || !win->active)
        return;
    xd_tooltip *tip = XD_NEW(xd_tooltip);
    tip->owner = win;
    tip->scale = win->scale;
    tip->text = copy_str(widget->tooltip);

    /* Size in logical px, measured at scale 1. */
    xd_theme t1;
    xd_theme_init(&t1, ui->style, &ui->scheme, 1.0);
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(probe);
    xd_text_style st = tip_text_style(&t1);
    tip->width = xd_text_width(cairo, tip->text, &st) + 2 * TIP_PAD_X + 2;
    cairo_destroy(cairo);
    cairo_surface_destroy(probe);
    tip->height = xd_metric_px(&t1, XD_METRIC_MENU_HEIGHT) + 1; /* 19px, as XP's */

    tip->surface = wl_compositor_create_surface(ui->compositor);
    /* The pointer goes through it to the widget underneath. */
    struct wl_region *none = wl_compositor_create_region(ui->compositor);
    wl_surface_set_input_region(tip->surface, none);
    wl_region_destroy(none);
    if (ui->fractional_manager != nullptr && ui->viewporter != nullptr) {
        tip->fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(ui->fractional_manager, tip->surface);
        wp_fractional_scale_v1_add_listener(tip->fractional, &tip_fractional_listener, tip);
        tip->viewport = wp_viewporter_get_viewport(ui->viewporter, tip->surface);
    }
    tip->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm_base, tip->surface);
    xdg_surface_add_listener(tip->xdg_surface, &tip_xdg_listener, tip);

    /* Under the pointer, clear of the arrow, as Windows places it. */
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(ui->wm_base);
    xdg_positioner_set_size(pos, tip->width, tip->height);
    xdg_positioner_set_anchor_rect(pos, (int)ui->pointer_x, (int)ui->pointer_y, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(pos, 0, TIP_BELOW_POINTER);
    xdg_positioner_set_constraint_adjustment(pos,
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    tip->popup = xdg_surface_get_popup(tip->xdg_surface, win->xdg_surface, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(tip->popup, &tip_popup_listener, tip);
    wl_surface_commit(tip->surface);
    win->tip = tip;
    win->tip_timer = xd_ui_add_timer(ui, TIP_AUTOPOP_MS, false, tip_autopop, win);
}

/* The topmost shown widget with a tooltip at window-local (x, y). */
static xd_widget *tip_widget_at(xd_window *win, double x, double y) {
    for (size_t i = win->count; i-- > 0;) {
        xd_widget *widget = win->widgets[i];
        if (widget->tooltip != nullptr && is_visible(widget) && contains(widget->r, x, y))
            return widget;
    }
    return nullptr;
}

/* Pointer motion over the window: arms the tip of the widget it's now on. */
static void tip_track(xd_window *win, double x, double y) {
    xd_widget *widget = tip_widget_at(win, x, y);
    if (widget == win->tip_widget)
        return;
    tip_hide(win);
    win->tip_widget = widget;
    if (widget != nullptr)
        win->tip_timer = xd_ui_add_timer(win->ui, TIP_INITIAL_MS, false, tip_show, win);
}

void xd_widget_set_tooltip(xd_widget *widget, const char *text) {
    XD_FREE(&widget->tooltip);
    if (text != nullptr && text[0] != '\0')
        widget->tooltip = copy_str(text);
    if (widget->win->tip_widget == widget)
        tip_hide(widget->win);
}

/* ---- window lifecycle --------------------------------------------------------- */

static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    xd_window *win = data;
    xdg_surface_ack_configure(xdg, serial);
    win->configured = true;
    win->dirty = true;
}

static const struct xdg_surface_listener xdg_listener = {.configure = xdg_configure};

static void toplevel_configure([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_toplevel *toplevel,
                               [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height,
                               [[maybe_unused]] struct wl_array *states) {
    /* Dialogs keep their size; activation is tracked via keyboard focus. */
}

static void click(xd_widget *widget);

static void toplevel_close(void *data, [[maybe_unused]] struct xdg_toplevel *toplevel) {
    xd_window *win = data;
    if (win->cancel != nullptr && win->cancel->enabled)
        click(win->cancel);
    else
        xd_window_close(win);
}

static void toplevel_bounds([[maybe_unused]] void *data,
                            [[maybe_unused]] struct xdg_toplevel *toplevel,
                            [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}

static void toplevel_caps([[maybe_unused]] void *data,
                          [[maybe_unused]] struct xdg_toplevel *toplevel,
                          [[maybe_unused]] struct wl_array *caps) {}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_caps,
};

static void window_scale(void *data,
                         [[maybe_unused]] struct wp_fractional_scale_v1 *fractional_scale,
                         uint32_t scale_120) {
    xd_window *win = data;
    double scale = scale_120 / 120.0;
    if (scale >= 0.5 && scale <= 8.0 && scale != win->scale) {
        win->scale = scale;
        win->dirty = true;
    }
}

static const struct wp_fractional_scale_v1_listener window_fractional_listener = {
    .preferred_scale = window_scale,
};

xd_window *xd_window_create(xd_ui *ui, const char *title, const char *app_id, int width,
                            int height) {
    XD_ASSERT(width > 0 && height > 0 && width < 8192 && height < 8192);
    xd_window *win = XD_NEW(xd_window);
    win->ui = ui;
    win->width = width;
    win->height = height;
    win->scale = 1.0;
    wl_list_insert(&ui->windows, &win->link);

    win->surface = wl_compositor_create_surface(ui->compositor);
    if (ui->fractional_manager != nullptr && ui->viewporter != nullptr) {
        win->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            ui->fractional_manager, win->surface);
        wp_fractional_scale_v1_add_listener(win->fractional, &window_fractional_listener, win);
        win->viewport = wp_viewporter_get_viewport(ui->viewporter, win->surface);
    }
    win->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm_base, win->surface);
    xdg_surface_add_listener(win->xdg_surface, &xdg_listener, win);
    win->toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, win);
    xdg_toplevel_set_title(win->toplevel, title);
    xdg_toplevel_set_app_id(win->toplevel, app_id);
    /* min == max tells the compositor this is a fixed-size dialog. */
    xdg_toplevel_set_min_size(win->toplevel, width, height);
    xdg_toplevel_set_max_size(win->toplevel, width, height);
    return win;
}

void xd_window_show(xd_window *win) {
    /* Focus the first focusable widget if the caller didn't pick one. */
    for (size_t i = 0; win->focus == nullptr && i < win->count; i++) {
        if (is_tab_stop(win->widgets[i]))
            xd_window_set_focus(win, win->widgets[i]);
    }
    wl_surface_commit(win->surface);
}

static void widget_free(xd_widget *widget) {
    XD_FREE(&widget->tooltip);
    XD_FREE(&widget->text);
    XD_FREE(&widget->text2);
    xd_textedit_free(&widget->edit);
    for (size_t i = 0; i < widget->count; i++)
        xd_free(widget->items[i]);
    xd_free(widget->items);
    xd_free(widget->tab_x);
    xd_free(widget->tab_w);
    xd_free(widget);
}

void xd_window_close(xd_window *win) {
    xd_ui *ui = win->ui;
    tip_hide(win);
    popup_close(win);
    if (ui->pointer_window == win)
        ui->pointer_window = nullptr;
    if (ui->keyboard_window == win)
        ui->keyboard_window = nullptr;
    cancel_frame(&win->frame);
    xdg_toplevel_destroy(win->toplevel);
    xdg_surface_destroy(win->xdg_surface);
    if (win->viewport != nullptr)
        wp_viewport_destroy(win->viewport);
    if (win->fractional != nullptr)
        wp_fractional_scale_v1_destroy(win->fractional);
    wl_surface_destroy(win->surface);
    xd_shm_ring_finish(&win->ring);
    for (size_t i = 0; i < win->count; i++)
        widget_free(win->widgets[i]);
    xd_free(win->widgets);
    wl_list_remove(&win->link);
    xd_free(win);
}

xd_ui *xd_window_ui(xd_window *win) { return win->ui; }
xd_window *xd_widget_window(xd_widget *widget) { return widget->win; }

xd_window *xd_window_for_surface(xd_ui *ui, struct wl_surface *surface) {
    XD_LIST_FOR_EACH(win, &ui->windows, xd_window, link) {
        if (win->surface == surface)
            return win;
    }
    return nullptr;
}

void xd_window_set_default(xd_window *win, xd_widget *button) { win->def = button; }
void xd_window_set_cancel(xd_window *win, xd_widget *button) { win->cancel = button; }

void xd_window_set_focus(xd_window *win, xd_widget *widget) {
    if (widget != nullptr && !is_focusable(widget))
        return;
    if (win->focus != widget && widget != nullptr && is_text_widget(widget))
        xd_textedit_select_all(&widget->edit); /* Windows selects on Tab-in */
    win->focus = widget;
    win->dirty = true;
}

/* ---- widgets ---------------------------------------------------------------------- */

static xd_widget *add(xd_window *win, xd_widget_kind kind, xd_rect rect) {
    xd_widget *widget = XD_NEW(xd_widget);
    widget->kind = kind;
    widget->win = win;
    widget->r = rect;
    widget->enabled = true;
    widget->selected = -1;
    widget->page = -1;
    if (win->count == win->cap) {
        win->cap = win->cap == 0 ? 8 : win->cap * 2;
        win->widgets = xd_realloc_array(win->widgets, win->cap, sizeof(*win->widgets));
    }
    win->widgets[win->count++] = widget;
    win->dirty = true;
    return widget;
}

static char *copy_str(const char *text) { return xd_str_dup_cstr(xd_str_from_cstr(text)); }

xd_widget *xd_label(xd_window *win, xd_rect rect, const char *text, uint8_t flags) {
    xd_widget *widget = add(win, XD_W_LABEL, rect);
    widget->text = copy_str(text);
    widget->flags = flags;
    return widget;
}

xd_widget *xd_image(xd_window *win, xd_rect rect, const char *icon_name) {
    xd_widget *widget = add(win, XD_W_IMAGE, rect);
    widget->text = copy_str(icon_name);
    return widget;
}

xd_widget *xd_banner(xd_window *win, xd_rect rect, const char *product, const char *edition) {
    xd_widget *widget = add(win, XD_W_BANNER, rect);
    widget->text = copy_str(product);
    widget->text2 = copy_str(edition);
    return widget;
}

xd_widget *xd_button(xd_window *win, xd_rect rect, const char *label, xd_widget_cb on_click,
                     void *user) {
    xd_widget *widget = add(win, XD_W_BUTTON, rect);
    widget->text = copy_str(label);
    widget->cb = on_click;
    widget->user = user;
    return widget;
}

xd_widget *xd_edit(xd_window *win, xd_rect rect, const char *text) {
    xd_widget *widget = add(win, XD_W_EDIT, rect);
    widget->edit.max_len = 4096;
    xd_textedit_set(&widget->edit, xd_str_from_cstr(text));
    return widget;
}

void xd_edit_set_on_change(xd_widget *widget, xd_widget_cb on_change, void *user) {
    XD_ASSERT(widget->kind == XD_W_EDIT);
    widget->cb = on_change;
    widget->user = user;
}

xd_widget *xd_combo(xd_window *win, xd_rect rect, bool editable, xd_widget_cb on_change,
                    void *user) {
    xd_widget *widget = add(win, XD_W_COMBO, rect);
    widget->editable = editable;
    widget->cb = on_change;
    widget->user = user;
    widget->edit.max_len = 4096;
    return widget;
}

void xd_combo_add(xd_widget *widget, const char *item) { xd_widget_add_item(widget, item); }

static void xd_widget_add_item_any(xd_widget *widget, const char *item);

void xd_widget_add_item(xd_widget *widget, const char *item) {
    XD_ASSERT(widget->kind == XD_W_COMBO || widget->kind == XD_W_LISTBOX);
    xd_widget_add_item_any(widget, item);
}

static void xd_widget_add_item_any(xd_widget *widget, const char *item) {
    if (widget->count == widget->cap) {
        widget->cap = widget->cap == 0 ? 8 : widget->cap * 2;
        widget->items = xd_realloc_array(widget->items, widget->cap, sizeof(*widget->items));
    }
    widget->items[widget->count++] = copy_str(item);
    widget->win->dirty = true;
}

void xd_widget_clear_items(xd_widget *widget) {
    for (size_t i = 0; i < widget->count; i++)
        xd_free(widget->items[i]);
    widget->count = 0;
    widget->selected = -1;
    widget->first = 0;
    widget->win->dirty = true;
}

int xd_combo_selected(const xd_widget *widget) { return widget->selected; }
int xd_widget_selected(const xd_widget *widget) { return widget->selected; }
size_t xd_widget_item_count(const xd_widget *widget) { return widget->count; }

void xd_combo_select(xd_widget *widget, int index) {
    XD_ASSERT(widget->kind == XD_W_COMBO);
    xd_widget_select(widget, index);
}

void xd_widget_select(xd_widget *widget, int index) {
    XD_ASSERT(widget->kind == XD_W_COMBO || widget->kind == XD_W_LISTBOX ||
              widget->kind == XD_W_TABS);
    if (index < 0 || (size_t)index >= widget->count)
        return;
    widget->selected = index;
    if (widget->kind == XD_W_COMBO && widget->editable)
        xd_textedit_set(&widget->edit, xd_str_from_cstr(widget->items[index]));
    if (widget->kind == XD_W_LISTBOX)
        list_scroll_to(widget, index);
    if (widget->kind == XD_W_TABS && widget->win->focus != nullptr &&
        !is_visible(widget->win->focus))
        widget->win->focus = widget; /* focus was on the page we left */
    widget->win->dirty = true;
}

xd_widget *xd_tabs(xd_window *win, xd_rect rect, const char *const *labels, size_t count,
                   xd_widget_cb on_change, void *user) {
    XD_ASSERT(win->tabs == nullptr); /* one per window */
    xd_widget *widget = add(win, XD_W_TABS, rect);
    widget->cb = on_change;
    widget->user = user;
    widget->tab_x = xd_alloc_array(XD_MAX(count, 1), sizeof(int));
    widget->tab_w = xd_alloc_array(XD_MAX(count, 1), sizeof(int));

    /* Tabs are as wide as their labels, measured at logical scale. */
    xd_theme theme;
    xd_theme_init(&theme, win->ui->style, &win->ui->scheme, 1.0);
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(scratch);
    xd_text_style st = xd_text(&theme, XD_FONT_MESSAGE, xd_rgba_hex(0), XD_ALIGN_START);
    /* 7px each side of a label; less, down to 3, where that wouldn't fit
     * in one row (Display Properties' seven tabs). */
    int text = 0;
    for (size_t i = 0; i < count; i++)
        text += xd_text_width(cairo, labels[i], &st);
    int pad = 14;
    while (pad > 6 && count > 0 && text + pad * (int)count > rect.w - 4)
        pad -= 2;
    int x = 2;
    for (size_t i = 0; i < count; i++) {
        xd_widget_add_item_any(widget, labels[i]);
        widget->tab_x[i] = x;
        widget->tab_w[i] = xd_text_width(cairo, labels[i], &st) + pad;
        x += widget->tab_w[i];
    }
    cairo_destroy(cairo);
    cairo_surface_destroy(scratch);
    widget->selected = count > 0 ? 0 : -1;
    win->tabs = widget;
    return widget;
}

void xd_widget_set_page(xd_widget *widget, int page) {
    widget->page = page;
    widget->win->dirty = true;
}

void xd_widget_set_visible(xd_widget *widget, bool visible) {
    widget->hidden = !visible;
    if (!visible && widget->win->focus == widget)
        widget->win->focus = nullptr;
    widget->win->dirty = true;
}

xd_widget *xd_checkbox(xd_window *win, xd_rect rect, const char *label, bool checked,
                       xd_widget_cb on_change, void *user) {
    xd_widget *widget = add(win, XD_W_CHECKBOX, rect);
    widget->text = copy_str(label);
    widget->checked = checked;
    widget->cb = on_change;
    widget->user = user;
    return widget;
}

bool xd_checkbox_checked(const xd_widget *widget) { return widget->checked; }

void xd_checkbox_set(xd_widget *widget, bool checked) {
    widget->checked = checked;
    widget->win->dirty = true;
}

xd_widget *xd_radio(xd_window *win, xd_rect rect, const char *label, bool group,
                    xd_widget_cb on_change, void *user) {
    xd_widget *widget = add(win, XD_W_RADIO, rect);
    widget->text = copy_str(label);
    widget->flags = group ? RADIO_GROUP_START : 0;
    widget->cb = on_change;
    widget->user = user;
    return widget;
}

void xd_radio_set(xd_widget *widget) {
    XD_ASSERT(widget->kind == XD_W_RADIO);
    radio_check(widget);
}

xd_widget *xd_listbox(xd_window *win, xd_rect rect, xd_widget_cb on_change, void *user) {
    xd_widget *widget = add(win, XD_W_LISTBOX, rect);
    widget->cb = on_change;
    widget->user = user;
    return widget;
}

xd_widget *xd_groupbox(xd_window *win, xd_rect rect, const char *label) {
    xd_widget *widget = add(win, XD_W_GROUPBOX, rect);
    widget->text = copy_str(label);
    return widget;
}

xd_widget *xd_custom(xd_window *win, xd_rect rect, xd_draw_cb draw, void *user) {
    xd_widget *widget = add(win, XD_W_CUSTOM, rect);
    widget->draw = draw;
    widget->user = user;
    return widget;
}

void xd_widget_redraw(xd_widget *widget) { widget->win->dirty = true; }

void xd_custom_set_pointer(xd_widget *widget, xd_pointer_cb cb) { widget->pointer = cb; }

xd_widget *xd_slider(xd_window *win, xd_rect rect, int min, int max, int value,
                     xd_widget_cb on_change, void *user) {
    xd_widget *widget = add(win, XD_W_SLIDER, rect);
    widget->cb = on_change;
    widget->user = user;
    xd_slider_set(widget, min, max, value);
    return widget;
}

int xd_slider_value(const xd_widget *widget) { return widget->svalue; }

void xd_slider_set(xd_widget *widget, int min, int max, int value) {
    widget->smin = min;
    widget->smax = XD_MAX(max, min);
    widget->svalue = XD_CLAMP(value, widget->smin, widget->smax);
    widget->win->dirty = true;
}

const char *xd_widget_text(xd_widget *widget) {
    switch (widget->kind) {
    case XD_W_EDIT:
        return edit_cstr(widget);
    case XD_W_COMBO:
        if (widget->editable)
            return edit_cstr(widget);
        return widget->selected >= 0 ? widget->items[widget->selected] : "";
    case XD_W_LISTBOX:
    case XD_W_TABS:
        return widget->selected >= 0 ? widget->items[widget->selected] : "";
    case XD_W_LABEL:
    case XD_W_IMAGE:
    case XD_W_BANNER:
    case XD_W_BUTTON:
    case XD_W_CHECKBOX:
    case XD_W_GROUPBOX:
    case XD_W_CUSTOM:
    case XD_W_SLIDER:
    case XD_W_RADIO:
        break;
    }
    return widget->text != nullptr ? widget->text : "";
}

void xd_widget_set_text(xd_widget *widget, const char *text) {
    if (is_text_widget(widget)) {
        xd_textedit_set(&widget->edit, xd_str_from_cstr(text));
    } else {
        XD_FREE(&widget->text);
        widget->text = copy_str(text);
    }
    widget->win->dirty = true;
}

void xd_widget_set_enabled(xd_widget *widget, bool enabled) {
    if (widget->enabled == enabled)
        return;
    widget->enabled = enabled;
    if (!enabled && widget->win->focus == widget)
        widget->win->focus = nullptr;
    widget->win->dirty = true;
}

/* ---- input ----------------------------------------------------------------------- */

/* Runs a button's action. The callback may close the window: callers must
 * return without touching the window afterwards. */
static void click(xd_widget *widget) {
    if (widget->enabled && widget->cb != nullptr)
        widget->cb(widget, widget->user);
}

static xd_widget *widget_at(xd_window *win, double x, double y) {
    for (size_t i = win->count; i-- > 0;) {
        xd_widget *widget = win->widgets[i];
        if ((is_focusable(widget) || is_clickable(widget)) && contains(widget->r, x, y))
            return widget;
    }
    return nullptr;
}

static bool over_text(xd_window *win, xd_widget *widget, double x, double y) {
    if (widget == nullptr || !is_text_widget(widget))
        return false;
    xd_theme theme;
    window_theme(win, &theme);
    xd_rect area = text_area(&theme, widget);
    return contains(area, x * win->scale, y * win->scale);
}

/* Selection change from the user: notify. May close the window. */
static void select_changed(xd_widget *widget) {
    widget->win->dirty = true;
    if (widget->cb != nullptr)
        widget->cb(widget, widget->user);
}

static void select_index(xd_widget *widget, int index) {
    if (index < 0 || (size_t)index >= widget->count || index == widget->selected)
        return;
    xd_widget_select(widget, index);
    select_changed(widget);
}

/* Scroll thumb dragging: map the pointer back to a first row. */
static void list_drag_thumb(xd_widget *widget, double y) {
    int first = sb_first_at(widget->win->ui, list_scrollbar(widget), widget->first,
                            list_rows(widget), (int)widget->count, y - widget->win->thumb_offset);
    if (first != widget->first) {
        widget->first = first;
        widget->win->dirty = true;
    }
}

void xd_window_pointer_axis(xd_window *win, double value) {
    xd_widget *widget = widget_at(win, win->ui->pointer_x, win->ui->pointer_y);
    if (widget == nullptr || widget->kind != XD_W_LISTBOX)
        return;
    widget->first += value > 0 ? 3 : -3; /* Windows scrolls three lines a notch */
    list_clamp(widget);
    win->dirty = true;
}

void xd_window_pointer_motion(xd_window *win, double x, double y) {
    /* A drag of the drop-down's scroll thumb goes on past the list: the
     * popup grab hands the pointer to this window once it leaves. */
    xd_listpopup *popup = win->popup;
    if (popup != nullptr && popup->sb_pressed != SB_NONE) {
        xd_popup_pointer_motion(popup, x - popup->x, y - popup->y);
        return;
    }
    xd_widget *pw = win->pressed;
    if (pw != nullptr && pw->kind == XD_W_CUSTOM && pw->pointer != nullptr) {
        pw->pointer(pw, XD_POINTER_MOTION, x - pw->r.x, y - pw->r.y, pw->user);
        return;
    }
    if (win->thumb_drag && pw != nullptr && pw->kind == XD_W_SLIDER) {
        slider_changed(pw, slider_value_at(pw, x - win->thumb_offset));
        return;
    }
    if (win->thumb_drag && pw != nullptr) {
        list_drag_thumb(pw, y);
        return;
    }
    if (win->selecting && win->pressed != nullptr) {
        xd_textedit_set_cursor(&win->pressed->edit, text_pos_at(win, win->pressed, x), true);
        win->dirty = true;
        return;
    }
    xd_widget *hot = widget_at(win, x, y);
    if (hot != win->hot) {
        if (draws_hot(hot) || draws_hot(win->hot))
            win->dirty = true;
        win->hot = hot;
    }
    xd_ui_set_cursor_text(win->ui, over_text(win, hot, x, y));
    tip_track(win, x, y);
}

void xd_window_pointer_leave(xd_window *win) {
    if (draws_hot(win->hot))
        win->dirty = true;
    win->hot = nullptr;
    tip_hide(win);
    win->tip_widget = nullptr;
}

void xd_window_pointer_button(xd_window *win, uint32_t button, bool pressed) {
    if (pressed)
        tip_hide(win); /* and it stays away until the pointer moves on */
    if (win->popup != nullptr && win->popup->sb_pressed != SB_NONE) {
        /* The end of a scroll bar drag that left the list (or, on a press,
         * one whose release went to another program). */
        popup_sb_release(win->popup);
        if (!pressed)
            return;
    }
    /* The compositor ends the drop-down's grab only for clicks in other
     * programs. A click in this window (another tab, a button) closes the
     * list and does nothing else, as in Windows. */
    if (pressed && win->popup != nullptr) {
        popup_close(win);
        win->dirty = true;
        return;
    }
    if (button != BTN_LEFT)
        return;
    double x = win->ui->pointer_x;
    double y = win->ui->pointer_y;
    if (!pressed) {
        xd_widget *target = win->pressed;
        if (target == nullptr)
            return; /* pressed outside any widget: nothing changes */
        bool released_over = win->hot == target;
        win->pressed = nullptr;
        if (target->kind == XD_W_CUSTOM && target->pointer != nullptr) {
            win->dirty = true;
            target->pointer(target, XD_POINTER_RELEASE, x - target->r.x, y - target->r.y,
                            target->user); /* may close the window */
            return;
        }
        win->selecting = false;
        win->thumb_drag = false;
        win->dirty = true;
        if (released_over && target->kind == XD_W_BUTTON) {
            click(target); /* may close the window */
        } else if (released_over && target->kind == XD_W_CHECKBOX) {
            target->checked = !target->checked;
            select_changed(target); /* may close the window */
        } else if (released_over && target->kind == XD_W_RADIO) {
            if (radio_check(target))
                select_changed(target); /* may close the window */
        }
        return;
    }

    xd_widget *widget = widget_at(win, x, y);
    if (widget == nullptr)
        return;
    if (is_clickable(widget)) {
        win->pressed = widget;
        widget->pointer(widget, XD_POINTER_PRESS, x - widget->r.x, y - widget->r.y, widget->user);
        return;
    }
    if (win->focus != widget) {
        win->focus = widget;
        win->dirty = true;
    }
    win->pressed = widget;
    if (widget->kind == XD_W_SLIDER) {
        xd_rect thumb = slider_thumb(widget);
        if (contains(thumb, x, y)) {
            win->thumb_drag = true;
            win->thumb_offset = x - (thumb.x + SLIDER_THUMB_W / 2);
        } else {
            /* A click in the channel pages toward it. */
            int page = XD_MAX((widget->smax - widget->smin) / 5, 1);
            slider_changed(widget, widget->svalue + (x < thumb.x ? -page : page));
        }
        win->dirty = true;
        return;
    }
    if (widget->kind == XD_W_TABS) {
        for (size_t i = 0; i < widget->count; i++) {
            if (contains(tab_rect(widget, i), x, y)) {
                select_index(widget, (int)i); /* may close the window */
                return;
            }
        }
        return;
    }
    if (widget->kind == XD_W_LISTBOX) {
        if (list_has_scrollbar(widget) && contains(list_scrollbar(widget), x, y)) {
            xd_rect sb = list_scrollbar(widget);
            int rows = list_rows(widget);
            int total = (int)widget->count;
            sb_part part = sb_hit(win->ui, sb, widget->first, rows, total, y);
            if (part == SB_THUMB) {
                win->thumb_drag = true;
                win->thumb_offset = y - sb_thumb(win->ui, sb, widget->first, rows, total).y;
            } else {
                widget->first = sb_step(part, widget->first, rows);
            }
            list_clamp(widget);
            win->dirty = true;
            return;
        }
        double top = widget->r.y + (double)XD_LIST_BORDER;
        int row = (int)((y - top) / (double)XD_LIST_ROW_H);
        if (y >= top && row >= 0 && row < list_rows(widget))
            select_index(widget, widget->first + row); /* may close the window */
        return;
    }
    if (widget->kind == XD_W_COMBO && (!widget->editable || !over_text(win, widget, x, y))) {
        if (win->popup != nullptr)
            popup_close(win);
        else
            popup_open(win, widget);
    } else if (is_text_widget(widget)) {
        xd_textedit_set_cursor(&widget->edit, text_pos_at(win, widget, x), false);
        win->selecting = true;
    }
    win->dirty = true;
}

static void focus_step(xd_window *win, int dir) {
    if (win->count == 0)
        return;
    size_t start = 0;
    for (size_t i = 0; i < win->count; i++) {
        if (win->widgets[i] == win->focus)
            start = i;
    }
    size_t count = win->count;
    size_t i = start;
    for (size_t tries = 0; tries < count; tries++) {
        i = dir > 0 ? (i + 1) % count : (i + count - 1) % count;
        if (is_tab_stop(win->widgets[i])) {
            xd_window_set_focus(win, win->widgets[i]);
            return;
        }
    }
}

static void edit_changed(xd_widget *widget) {
    widget->win->dirty = true;
    if (widget->kind == XD_W_COMBO)
        widget->selected = -1; /* typed text no longer matches an item */
    if (widget->cb != nullptr)
        widget->cb(widget, widget->user); /* may close the window */
}

/* Text editing keys; returns true if handled. */
static bool edit_key(xd_widget *widget, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
    xd_textedit *edit = &widget->edit;
    bool shift = mods & XD_MOD_SHIFT;
    bool ctrl = mods & XD_MOD_CTRL;
    xd_ui *ui = widget->win->ui;

    switch (sym) {
    case XKB_KEY_Left:
        xd_textedit_move(edit, ctrl ? XD_MOTION_WORD_LEFT : XD_MOTION_LEFT, shift);
        widget->win->dirty = true;
        return true;
    case XKB_KEY_Right:
        xd_textedit_move(edit, ctrl ? XD_MOTION_WORD_RIGHT : XD_MOTION_RIGHT, shift);
        widget->win->dirty = true;
        return true;
    case XKB_KEY_Home:
        xd_textedit_move(edit, XD_MOTION_HOME, shift);
        widget->win->dirty = true;
        return true;
    case XKB_KEY_End:
        xd_textedit_move(edit, XD_MOTION_END, shift);
        widget->win->dirty = true;
        return true;
    case XKB_KEY_BackSpace:
        xd_textedit_backspace(edit, ctrl);
        edit_changed(widget);
        return true;
    case XKB_KEY_Delete:
        if (shift) { /* Shift+Del = cut */
            xd_ui_clipboard_copy(ui, xd_textedit_selected(edit));
            xd_textedit_delete_selection(edit);
        } else {
            xd_textedit_delete(edit, ctrl);
        }
        edit_changed(widget);
        return true;
    default:
        break;
    }

    if (ctrl) {
        switch (sym) {
        case XKB_KEY_a:
        case XKB_KEY_A:
            xd_textedit_select_all(edit);
            widget->win->dirty = true;
            return true;
        case XKB_KEY_c:
        case XKB_KEY_C:
        case XKB_KEY_Insert:
            xd_ui_clipboard_copy(ui, xd_textedit_selected(edit));
            return true;
        case XKB_KEY_x:
        case XKB_KEY_X:
            xd_ui_clipboard_copy(ui, xd_textedit_selected(edit));
            xd_textedit_delete_selection(edit);
            edit_changed(widget);
            return true;
        case XKB_KEY_v:
        case XKB_KEY_V: {
            char *text = xd_ui_clipboard_paste(ui);
            if (text != nullptr) {
                xd_textedit_insert(edit, xd_str_from_cstr(text));
                xd_free(text);
                edit_changed(widget);
            }
            return true;
        }
        default:
            return false;
        }
    }
    if (sym == XKB_KEY_Insert && shift) { /* Shift+Ins = paste */
        char *text = xd_ui_clipboard_paste(ui);
        if (text != nullptr) {
            xd_textedit_insert(edit, xd_str_from_cstr(text));
            xd_free(text);
            edit_changed(widget);
        }
        return true;
    }
    if (!(mods & XD_MOD_ALT) && utf8 != nullptr && (unsigned char)utf8[0] >= 0x20 &&
        utf8[0] != 0x7F) {
        xd_textedit_insert(edit, xd_str_from_cstr(utf8));
        edit_changed(widget);
        return true;
    }
    return false;
}

static void combo_step(xd_widget *widget, int dir) {
    if (widget->count == 0)
        return;
    int i = widget->selected < 0 ? (dir > 0 ? 0 : (int)widget->count - 1) : widget->selected + dir;
    i = XD_CLAMP(i, 0, (int)widget->count - 1);
    if (i != widget->selected) {
        xd_combo_select(widget, i);
        combo_changed(widget); /* must be last: may close the window */
    }
}

void xd_window_key(xd_window *win, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
    tip_hide(win); /* any key puts a tooltip away */
    xd_widget *focused = win->focus;
    bool alt = mods & XD_MOD_ALT;

    /* An open drop-down list takes navigation keys. */
    xd_listpopup *popup = win->popup;
    if (popup != nullptr && popup->combo == nullptr) {
        /* A panel: Esc puts it away, Tab moves on past it. */
        if (sym != XKB_KEY_Tab && sym != XKB_KEY_ISO_Left_Tab && sym != XKB_KEY_Escape &&
            sym != XKB_KEY_F4)
            return;
        popup_close(win);
        if (sym != XKB_KEY_Tab && sym != XKB_KEY_ISO_Left_Tab)
            return;
        popup = nullptr;
    }
    if (popup != nullptr) {
        switch (sym) {
        case XKB_KEY_Up:
        case XKB_KEY_Down:
            if (alt) {
                popup_close(win);
                return;
            }
            popup->hot = XD_CLAMP(popup->hot + (sym == XKB_KEY_Down ? 1 : -1), 0,
                                  (int)popup->combo->count - 1);
            popup_scroll_to(popup, popup->hot);
            popup->dirty = true;
            return;
        case XKB_KEY_Prior:
        case XKB_KEY_Next: {
            /* As in a Windows list: first to the edge of the visible page,
             * then a page further (keeping one row in view). */
            int last = (int)popup->combo->count - 1;
            int page = XD_MAX(popup->rows - 1, 1);
            int target;
            if (sym == XKB_KEY_Next) {
                int bottom = XD_MIN(popup->first + popup->rows - 1, last);
                target = popup->hot < bottom ? bottom : popup->hot + page;
            } else {
                target = popup->hot > popup->first ? popup->first : popup->hot - page;
            }
            popup->hot = XD_CLAMP(target, 0, last);
            popup_scroll_to(popup, popup->hot);
            popup->dirty = true;
            return;
        }
        case XKB_KEY_Home:
        case XKB_KEY_End:
            popup->hot = sym == XKB_KEY_Home ? 0 : (int)popup->combo->count - 1;
            popup_scroll_to(popup, popup->hot);
            popup->dirty = true;
            return;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            popup_choose(win, popup->hot);
            return;
        case XKB_KEY_Escape:
        case XKB_KEY_F4:
            popup_close(win);
            return;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            popup_close(win);
            break;
        default:
            return;
        }
    }

    /* Ctrl+Tab / Ctrl+PgDn switch property-sheet pages from anywhere. */
    xd_widget *tabs = win->tabs;
    if (tabs != nullptr && tabs->count > 0 && (mods & XD_MOD_CTRL) &&
        (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab || sym == XKB_KEY_Next ||
         sym == XKB_KEY_Prior)) {
        bool back = sym == XKB_KEY_Prior || sym == XKB_KEY_ISO_Left_Tab || (mods & XD_MOD_SHIFT);
        int count = (int)tabs->count;
        select_index(tabs, (tabs->selected + (back ? count - 1 : 1)) % count);
        return;
    }

    switch (sym) {
    case XKB_KEY_Tab:
        focus_step(win, (mods & XD_MOD_SHIFT) ? -1 : 1);
        return;
    case XKB_KEY_ISO_Left_Tab:
        focus_step(win, -1);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (focused != nullptr && focused->kind == XD_W_BUTTON)
            click(focused);
        else if (win->def != nullptr)
            click(win->def);
        return;
    case XKB_KEY_Escape:
        if (win->cancel != nullptr)
            click(win->cancel);
        else
            xd_window_close(win);
        return;
    default:
        break;
    }
    if (focused == nullptr)
        return;

    if (focused->kind == XD_W_BUTTON) {
        if (sym == XKB_KEY_space)
            click(focused);
        return;
    }
    if (focused->kind == XD_W_CHECKBOX) {
        if (sym == XKB_KEY_space) {
            focused->checked = !focused->checked;
            select_changed(focused);
        }
        return;
    }
    if (focused->kind == XD_W_RADIO) {
        int dir = sym == XKB_KEY_Down || sym == XKB_KEY_Right ? 1
                  : sym == XKB_KEY_Up || sym == XKB_KEY_Left  ? -1
                                                              : 0;
        xd_widget *target = sym == XKB_KEY_space ? focused : nullptr;
        if (dir != 0) {
            /* The arrows go round the group, checking as they go. */
            size_t first;
            size_t end;
            radio_group(focused, &first, &end);
            size_t count = end - first;
            size_t i = widget_index(win, focused) - first;
            for (size_t tries = 1; tries < count && target == nullptr; tries++) {
                size_t j = first + (dir > 0 ? (i + tries) % count : (i + count - tries) % count);
                if (is_focusable(win->widgets[j]))
                    target = win->widgets[j];
            }
        }
        if (target != nullptr) {
            xd_window_set_focus(win, target);
            if (radio_check(target))
                select_changed(target); /* may close the window */
        }
        return;
    }
    if (focused->kind == XD_W_SLIDER) {
        int page = XD_MAX((focused->smax - focused->smin) / 5, 1);
        switch (sym) {
        case XKB_KEY_Left:
        case XKB_KEY_Up:
            slider_changed(focused, focused->svalue - 1);
            break;
        case XKB_KEY_Right:
        case XKB_KEY_Down:
            slider_changed(focused, focused->svalue + 1);
            break;
        case XKB_KEY_Prior:
            slider_changed(focused, focused->svalue - page);
            break;
        case XKB_KEY_Next:
            slider_changed(focused, focused->svalue + page);
            break;
        case XKB_KEY_Home:
            slider_changed(focused, focused->smin);
            break;
        case XKB_KEY_End:
            slider_changed(focused, focused->smax);
            break;
        default:
            break;
        }
        return;
    }
    if (focused->kind == XD_W_TABS) {
        if (sym == XKB_KEY_Left || sym == XKB_KEY_Right)
            select_index(focused, XD_CLAMP(focused->selected + (sym == XKB_KEY_Right ? 1 : -1), 0,
                                           (int)focused->count - 1));
        return;
    }
    if (focused->kind == XD_W_LISTBOX) {
        int target = focused->selected;
        int rows = list_rows(focused);
        switch (sym) {
        case XKB_KEY_Up:
            target = XD_MAX(focused->selected - 1, 0);
            break;
        case XKB_KEY_Down:
            target = XD_MIN(focused->selected + 1, (int)focused->count - 1);
            break;
        case XKB_KEY_Prior:
            target = XD_MAX(focused->selected - rows, 0);
            break;
        case XKB_KEY_Next:
            target = XD_MIN(focused->selected + rows, (int)focused->count - 1);
            break;
        case XKB_KEY_Home:
            target = 0;
            break;
        case XKB_KEY_End:
            target = (int)focused->count - 1;
            break;
        default:
            return;
        }
        select_index(focused, target);
        return;
    }
    if (focused->kind == XD_W_COMBO) {
        if ((alt && (sym == XKB_KEY_Down || sym == XKB_KEY_Up)) || sym == XKB_KEY_F4) {
            popup_open(win, focused);
            return;
        }
        if (sym == XKB_KEY_Down || sym == XKB_KEY_Up) {
            combo_step(focused, sym == XKB_KEY_Down ? 1 : -1);
            return;
        }
        if (!focused->editable) {
            if (sym == XKB_KEY_Home)
                combo_step(focused, -(int)focused->count);
            else if (sym == XKB_KEY_End)
                combo_step(focused, (int)focused->count);
            return;
        }
    }
    if (is_text_widget(focused))
        edit_key(focused, sym, utf8, mods);
}
