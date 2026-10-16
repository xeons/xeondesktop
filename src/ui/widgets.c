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
    double v = round(logical * scale);
    XD_ASSERT(v > -65536.0 && v < 65536.0);
    return (int)v;
}

/* Logical -> device with shared edges, so neighbours never gap or overlap. */
static xd_rect to_device(xd_rect r, double s) {
    int x0 = dev(r.x, s);
    int y0 = dev(r.y, s);
    return (xd_rect){x0, y0, dev(r.x + r.w, s) - x0, dev(r.y + r.h, s) - y0};
}

static bool contains(xd_rect r, double x, double y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void window_theme(const xd_window *win, xd_theme *t) {
    xd_theme_init(t, win->ui->style, &win->ui->scheme, win->scale);
}

/* Content rect inside a field's border (matches xd_theme_draw_field). */
static xd_rect field_inner(const xd_theme *t, xd_rect r) {
    int b = t->style == XD_STYLE_CLASSIC ? 2 * xd_tpx(t, 1) : xd_tpx(t, 1);
    return xd_rect_inset(r, b, b);
}

/* Where a combo's arrow button goes, inside the field border. */
static xd_rect dropdown_rect(const xd_theme *t, xd_rect inner) {
    int bw = xd_theme_dropdown_button_width(t);
    return (xd_rect){inner.x + inner.w - bw, inner.y, bw, inner.h};
}

/* Device rect of the editable text area of an edit or editable combo. */
static xd_rect text_area(const xd_theme *t, const xd_widget *w) {
    xd_rect inner = field_inner(t, to_device(w->r, t->scale));
    if (w->kind == XD_W_COMBO)
        inner.w -= xd_theme_dropdown_button_width(t);
    return inner;
}

static bool is_text_widget(const xd_widget *w) {
    return w->kind == XD_W_EDIT || (w->kind == XD_W_COMBO && w->editable);
}

/* Hidden when it belongs to a tab page that isn't selected. */
static bool is_visible(const xd_widget *w) {
    const xd_widget *tabs = w->win->tabs;
    return w->page < 0 || tabs == nullptr || tabs->selected == w->page;
}

static bool is_focusable(const xd_widget *w) {
    return w->enabled && is_visible(w) &&
           (w->kind == XD_W_BUTTON || w->kind == XD_W_EDIT || w->kind == XD_W_COMBO ||
            w->kind == XD_W_TABS || w->kind == XD_W_CHECKBOX || w->kind == XD_W_LISTBOX);
}

/* ---- list box and tab geometry (logical units) ---------------------------------- */

static int list_rows(const xd_widget *w) {
    return XD_MAX((w->r.h - 2 * XD_LIST_BORDER) / XD_LIST_ROW_H, 1);
}

static bool list_has_scrollbar(const xd_widget *w) { return (int)w->count > list_rows(w); }

static xd_rect list_scrollbar(const xd_widget *w) {
    return (xd_rect){w->r.x + w->r.w - XD_LIST_BORDER - XD_SCROLL_W, w->r.y + XD_LIST_BORDER,
                     XD_SCROLL_W, w->r.h - 2 * XD_LIST_BORDER};
}

static xd_rect list_row(const xd_widget *w, int row) {
    int sw = list_has_scrollbar(w) ? XD_SCROLL_W : 0;
    return (xd_rect){w->r.x + XD_LIST_BORDER, w->r.y + XD_LIST_BORDER + row * XD_LIST_ROW_H,
                     w->r.w - 2 * XD_LIST_BORDER - sw, XD_LIST_ROW_H};
}

static void list_clamp(xd_widget *w) {
    int max_first = XD_MAX((int)w->count - list_rows(w), 0);
    w->first = XD_CLAMP(w->first, 0, max_first);
}

static void list_scroll_to(xd_widget *w, int index) {
    if (index < w->first)
        w->first = index;
    if (index >= w->first + list_rows(w))
        w->first = index - list_rows(w) + 1;
    list_clamp(w);
}

static xd_rect tab_rect(const xd_widget *w, size_t i) {
    return (xd_rect){w->r.x + w->tab_x[i], w->r.y, w->tab_w[i], XD_TAB_H};
}

static xd_rect tab_panel(const xd_widget *w) {
    return (xd_rect){w->r.x, w->r.y + XD_TAB_H - 2, w->r.w, w->r.h - XD_TAB_H + 2};
}

static xd_text_style field_text_style(const xd_theme *t, xd_rgba color) {
    xd_text_style st = {
        .font = &t->scheme.fonts[XD_FONT_MESSAGE],
        .scale = t->scale,
        .color = color,
        .align = XD_ALIGN_START,
        .smoothing = t->scheme.font_smoothing,
    };
    return st;
}

/* ---- text fields ----------------------------------------------------------------- */

static const char *edit_cstr(xd_widget *w) { return xd_strbuf_cstr(&w->edit.text); }

static int index_x(PangoLayout *layout, size_t index) {
    PangoRectangle pos;
    pango_layout_index_to_pos(layout, (int)XD_MIN(index, (size_t)INT32_MAX), &pos);
    return pos.x / PANGO_SCALE;
}

static void draw_text_field(xd_window *win, const xd_theme *t, cairo_t *cr, xd_widget *w,
                            xd_rect area) {
    xd_field_colors c = xd_theme_field_colors(t);
    bool focused = win->focus == w && win->active;
    int pad = xd_tpx(t, XD_TEXT_PAD);
    int avail = XD_MAX(area.w - 2 * pad, 1);

    xd_text_style st = field_text_style(t, w->enabled ? c.text : c.disabled_text);
    PangoLayout *layout = xd_text_layout(cr, edit_cstr(w), &st);
    int tw = 0;
    int th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);

    /* Scroll so the caret stays visible, and don't leave blank space on
     * the right when text is deleted. */
    int caret = index_x(layout, w->edit.cursor);
    if (caret - w->scroll > avail)
        w->scroll = caret - avail;
    if (caret - w->scroll < 0)
        w->scroll = caret;
    if (tw - w->scroll < avail)
        w->scroll = XD_MAX(0, tw - avail);

    double x0 = area.x + pad - w->scroll;
    double y0 = area.y + (area.h - th) / 2;
    cairo_save(cr);
    cairo_rectangle(cr, area.x, area.y, area.w, area.h);
    cairo_clip(cr);
    xd_set_color(cr, st.color);
    cairo_move_to(cr, x0, y0);
    pango_cairo_show_layout(cr, layout);

    if (focused && xd_textedit_has_selection(&w->edit)) {
        size_t s;
        size_t e;
        xd_textedit_selection(&w->edit, &s, &e);
        double sx0 = x0 + index_x(layout, s);
        double sx1 = x0 + index_x(layout, e);
        xd_fill_rect(cr, (xd_rect){(int)sx0, (int)y0, (int)(sx1 - sx0), th}, c.selection);
        cairo_rectangle(cr, sx0, y0, sx1 - sx0, th);
        cairo_clip(cr);
        xd_set_color(cr, c.selection_text);
        cairo_move_to(cr, x0, y0);
        pango_cairo_show_layout(cr, layout);
    } else if (focused) {
        xd_fill_rect(cr, (xd_rect){(int)(x0 + caret), (int)y0, XD_MAX(xd_tpx(t, 1), 1), th},
                     c.text);
    }
    cairo_restore(cr);
    g_object_unref(layout);
}

/* Byte position in the text under window-local logical x. */
static size_t text_pos_at(xd_window *win, xd_widget *w, double lx) {
    xd_theme t;
    window_theme(win, &t);
    xd_rect area = text_area(&t, w);
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(scratch);
    xd_text_style st = field_text_style(&t, xd_rgba_hex(0));
    PangoLayout *layout = xd_text_layout(cr, edit_cstr(w), &st);

    double x = lx * win->scale - area.x - xd_tpx(&t, XD_TEXT_PAD) + w->scroll;
    int index = 0;
    int trailing = 0;
    pango_layout_xy_to_index(layout, (int)(x * PANGO_SCALE), 0, &index, &trailing);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);

    xd_textedit tmp = w->edit; /* only the cursor fields change */
    xd_textedit_set_cursor(&tmp, (size_t)XD_MAX(index, 0), false);
    for (int i = 0; i < trailing; i++)
        xd_textedit_move(&tmp, XD_MOTION_RIGHT, false);
    return tmp.cursor;
}

/* ---- drawing ------------------------------------------------------------------------ */

static void draw_widget(xd_window *win, const xd_theme *t, cairo_t *cr, xd_widget *w) {
    xd_rect r = to_device(w->r, t->scale);
    bool focused = win->focus == w && win->active;
    switch (w->kind) {
    case XD_W_LABEL:
        xd_theme_draw_label(t, cr, r, w->text, w->flags & XD_LABEL_WRAP, w->flags & XD_LABEL_BOLD,
                            !w->enabled || (w->flags & XD_LABEL_DIM));
        break;
    case XD_W_IMAGE: {
        int px = XD_MIN(r.w, r.h);
        cairo_surface_t *icon = xd_icons_get(win->ui->icons, w->text, px);
        if (icon != nullptr) {
            cairo_set_source_surface(cr, icon, r.x + (r.w - px) / 2, r.y + (r.h - px) / 2);
            cairo_paint(cr);
        }
        break;
    }
    case XD_W_BANNER:
        xd_theme_draw_dialog_banner(t, cr, r, w->text, w->text2 != nullptr ? w->text2 : "");
        break;
    case XD_W_BUTTON: {
        uint8_t state = 0;
        /* The focused button is the default while it has focus. */
        bool is_default = win->focus != nullptr && win->focus->kind == XD_W_BUTTON
                              ? win->focus == w
                              : win->def == w;
        if (is_default)
            state |= XD_STATE_DEFAULT;
        if (focused)
            state |= XD_STATE_FOCUSED;
        if (win->hot == w)
            state |= XD_STATE_HOT;
        if (win->pressed == w && win->hot == w)
            state |= XD_STATE_PRESSED;
        if (!w->enabled)
            state |= XD_STATE_DISABLED;
        xd_theme_draw_button(t, cr, r, state, w->text);
        break;
    }
    case XD_W_EDIT: {
        xd_theme_draw_field(t, cr, r, !w->enabled);
        draw_text_field(win, t, cr, w, text_area(t, w));
        break;
    }
    case XD_W_TABS: {
        xd_theme_draw_tab_panel(t, cr, to_device(tab_panel(w), t->scale));
        /* Unselected tabs first; the selected one overlaps its neighbours
         * and the panel's top edge. */
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < w->count; i++) {
                bool sel = (int)i == w->selected;
                if (sel != (pass == 1))
                    continue;
                xd_rect tr = tab_rect(w, i);
                if (sel)
                    tr = (xd_rect){tr.x - 2, tr.y, tr.w + 4, tr.h};
                uint8_t state = sel ? XD_STATE_CHECKED : 0;
                if (sel && focused)
                    state |= XD_STATE_FOCUSED;
                xd_theme_draw_tab(t, cr, to_device(tr, t->scale), state, w->items[i]);
            }
        }
        break;
    }
    case XD_W_CHECKBOX: {
        uint8_t state = w->checked ? XD_STATE_CHECKED : 0;
        if (focused)
            state |= XD_STATE_FOCUSED;
        if (win->hot == w)
            state |= XD_STATE_HOT;
        if (win->pressed == w && win->hot == w)
            state |= XD_STATE_PRESSED;
        if (!w->enabled)
            state |= XD_STATE_DISABLED;
        xd_theme_draw_checkbox(t, cr, r, state, w->text);
        break;
    }
    case XD_W_LISTBOX: {
        xd_theme_draw_field(t, cr, r, !w->enabled);
        int rows = list_rows(w);
        for (int i = 0; i < rows && w->first + i < (int)w->count; i++) {
            int idx = w->first + i;
            xd_rect row = to_device(list_row(w, i), t->scale);
            xd_theme_draw_list_item(t, cr, row, idx == w->selected, w->items[idx]);
            if (idx == w->selected && focused)
                xd_theme_draw_focus_rect(t, cr, row);
        }
        if (list_has_scrollbar(w))
            xd_theme_draw_vscrollbar(t, cr, to_device(list_scrollbar(w), t->scale), w->first,
                                     rows, (int)w->count);
        break;
    }
    case XD_W_GROUPBOX:
        xd_theme_draw_groupbox(t, cr, r, w->text);
        break;
    case XD_W_CUSTOM:
        if (w->draw != nullptr) {
            cairo_save(cr);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_clip(cr);
            w->draw(w, cr, r, t, w->user);
            cairo_restore(cr);
        }
        break;
    case XD_W_COMBO: {
        xd_rect inner = xd_theme_draw_field(t, cr, r, !w->enabled);
        xd_rect button = dropdown_rect(t, inner);
        uint8_t bstate = 0;
        if (win->popup != nullptr && win->popup->combo == w)
            bstate |= XD_STATE_PRESSED;
        if (win->hot == w)
            bstate |= XD_STATE_HOT;
        if (!w->enabled)
            bstate |= XD_STATE_DISABLED;
        xd_theme_draw_dropdown_button(t, cr, button, bstate);
        if (w->editable) {
            draw_text_field(win, t, cr, w, text_area(t, w));
        } else {
            /* Drop-down list: the selection shows highlighted while focused. */
            xd_rect area = {inner.x, inner.y, inner.w - button.w, inner.h};
            int pad = xd_tpx(t, 1);
            xd_rect item = xd_rect_inset(area, pad, pad);
            const char *text = xd_widget_text(w);
            if (focused) {
                xd_theme_draw_list_item(t, cr, item, true, text);
                xd_theme_draw_focus_rect(t, cr, item);
            } else {
                xd_theme_draw_list_item(t, cr, item, false, text);
            }
        }
        break;
    }
    }
}

void xd_window_render(xd_window *win) {
    xd_ui *ui = win->ui;
    double s = win->scale;
    int dw = dev(win->width, s);
    int dh = dev(win->height, s);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&win->ring, ui->shm, dw, dh);
    if (buf == nullptr)
        return;

    xd_theme t;
    window_theme(win, &t);
    cairo_t *cr = cairo_create(buf->surface);
    xd_fill_rect(cr, (xd_rect){0, 0, dw, dh}, xd_theme_dialog_color(&t));
    for (size_t i = 0; i < win->count; i++) {
        if (is_visible(win->widgets[i]))
            draw_widget(win, &t, cr, win->widgets[i]);
    }
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    wl_surface_attach(win->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(win->surface, 0, 0, dw, dh);
    if (win->viewport != nullptr)
        wp_viewport_set_destination(win->viewport, win->width, win->height);
    wl_surface_commit(win->surface);
    buf->busy = true;
    win->dirty = false;
}

/* ---- drop-down list popup ------------------------------------------------------- */

static void popup_close(xd_window *win);

static void popup_scroll_to(xd_listpopup *p, int index) {
    if (index < p->first)
        p->first = index;
    if (index >= p->first + p->rows)
        p->first = index - p->rows + 1;
    p->first = XD_CLAMP(p->first, 0, XD_MAX((int)p->combo->count - p->rows, 0));
}

void xd_popup_render(xd_listpopup *p) {
    xd_ui *ui = p->owner->ui;
    double s = p->scale;
    int dw = dev(p->width, s);
    int dh = dev(p->height, s);
    xd_shm_buffer *buf = xd_shm_ring_acquire(&p->ring, ui->shm, dw, dh);
    if (buf == nullptr)
        return;
    xd_theme t;
    xd_theme_init(&t, ui->style, &ui->scheme, s);
    cairo_t *cr = cairo_create(buf->surface);

    xd_rect all = {0, 0, dw, dh};
    xd_fill_rect(cr, all, xd_sys(&t, XD_COLOR_WINDOW));
    xd_rgba border = t.style == XD_STYLE_CLASSIC ? xd_sys(&t, XD_COLOR_WINDOW_FRAME)
                                                 : xd_rgba_hex(0x7F9DB9);
    xd_draw_bevel(cr, all, xd_tpx(&t, 1), border, border);
    for (int i = 0; i < p->rows; i++) {
        int idx = p->first + i;
        if (idx >= (int)p->combo->count)
            break;
        xd_rect row = to_device((xd_rect){1, 1 + i * XD_LIST_ROW_H, p->width - 2, XD_LIST_ROW_H}, s);
        xd_theme_draw_list_item(&t, cr, row, idx == p->hot, p->combo->items[idx]);
    }
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    wl_surface_attach(p->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(p->surface, 0, 0, dw, dh);
    if (p->viewport != nullptr)
        wp_viewport_set_destination(p->viewport, p->width, p->height);
    wl_surface_commit(p->surface);
    buf->busy = true;
    p->dirty = false;
}

static void popup_xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    xd_listpopup *p = data;
    xdg_surface_ack_configure(xdg, serial);
    p->configured = true;
    p->dirty = true;
}

static const struct xdg_surface_listener popup_xdg_listener = {.configure = popup_xdg_configure};

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *x,
                            [[maybe_unused]] int32_t px, [[maybe_unused]] int32_t py,
                            [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *x) {
    xd_listpopup *p = data;
    popup_close(p->owner);
}

static void popup_repositioned([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *x,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static void popup_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *f,
                        uint32_t scale_120) {
    xd_listpopup *p = data;
    double s = scale_120 / 120.0;
    if (s >= 0.5 && s <= 8.0 && s != p->scale) {
        p->scale = s;
        p->dirty = true;
    }
}

static const struct wp_fractional_scale_v1_listener popup_fractional_listener = {
    .preferred_scale = popup_scale,
};

static void popup_open(xd_window *win, xd_widget *combo) {
    xd_ui *ui = win->ui;
    if (combo->count == 0 || win->popup != nullptr)
        return;
    xd_listpopup *p = XD_NEW(xd_listpopup);
    p->owner = win;
    p->combo = combo;
    p->scale = win->scale;
    p->rows = (int)XD_MIN(combo->count, (size_t)XD_LIST_MAX_ROWS);
    p->width = combo->r.w;
    p->height = p->rows * XD_LIST_ROW_H + 2;
    p->hot = XD_MAX(combo->selected, 0);
    popup_scroll_to(p, p->hot);

    p->surface = wl_compositor_create_surface(ui->compositor);
    if (ui->fractional_manager != nullptr && ui->viewporter != nullptr) {
        p->fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(ui->fractional_manager, p->surface);
        wp_fractional_scale_v1_add_listener(p->fractional, &popup_fractional_listener, p);
        p->viewport = wp_viewporter_get_viewport(ui->viewporter, p->surface);
    }
    p->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm_base, p->surface);
    xdg_surface_add_listener(p->xdg_surface, &popup_xdg_listener, p);

    struct xdg_positioner *pos = xdg_wm_base_create_positioner(ui->wm_base);
    xdg_positioner_set_size(pos, p->width, p->height);
    xdg_positioner_set_anchor_rect(pos, combo->r.x, combo->r.y, combo->r.w, combo->r.h);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos,
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    p->popup = xdg_surface_get_popup(p->xdg_surface, win->xdg_surface, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(p->popup, &popup_listener, p);
    uint32_t serial = XD_MAX(ui->pointer_serial, ui->key_serial);
    if (ui->seat != nullptr && serial != 0)
        xdg_popup_grab(p->popup, ui->seat, serial);
    wl_surface_commit(p->surface);

    win->popup = p;
    win->dirty = true;
}

static void popup_close(xd_window *win) {
    xd_listpopup *p = win->popup;
    if (p == nullptr)
        return;
    win->popup = nullptr;
    win->dirty = true;
    if (win->ui->pointer_popup == p)
        win->ui->pointer_popup = nullptr;
    xdg_popup_destroy(p->popup);
    xdg_surface_destroy(p->xdg_surface);
    if (p->viewport != nullptr)
        wp_viewport_destroy(p->viewport);
    if (p->fractional != nullptr)
        wp_fractional_scale_v1_destroy(p->fractional);
    wl_surface_destroy(p->surface);
    xd_shm_ring_finish(&p->ring);
    xd_free(p);
}

static void combo_changed(xd_widget *w) {
    w->win->dirty = true;
    if (w->cb != nullptr)
        w->cb(w, w->user);
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

xd_listpopup *xd_popup_for_surface(xd_ui *ui, struct wl_surface *s) {
    XD_LIST_FOR_EACH(win, &ui->windows, xd_window, link) {
        if (win->popup != nullptr && win->popup->surface == s)
            return win->popup;
    }
    return nullptr;
}

static int popup_row_at(const xd_listpopup *p, double y) {
    double f = floor((y - 1) / (double)XD_LIST_ROW_H);
    if (!(f >= 0 && f < (double)XD_LIST_MAX_ROWS))
        return -1;
    int row = (int)f;
    if (row < 0 || row >= p->rows || p->first + row >= (int)p->combo->count)
        return -1;
    return p->first + row;
}

void xd_popup_pointer_motion(xd_listpopup *p, [[maybe_unused]] double x, double y) {
    int hot = popup_row_at(p, y);
    if (hot >= 0 && hot != p->hot) {
        p->hot = hot;
        p->dirty = true;
    }
}

void xd_popup_pointer_button(xd_listpopup *p, uint32_t button, bool pressed) {
    if (button != BTN_LEFT || pressed)
        return;
    int row = popup_row_at(p, p->owner->ui->pointer_y);
    if (row >= 0)
        popup_choose(p->owner, row);
}

void xd_popup_pointer_axis(xd_listpopup *p, double value) {
    int step = value > 0 ? 1 : -1;
    int first = XD_CLAMP(p->first + step, 0, XD_MAX((int)p->combo->count - p->rows, 0));
    if (first != p->first) {
        p->first = first;
        p->dirty = true;
    }
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
                               [[maybe_unused]] struct xdg_toplevel *t,
                               [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h,
                               [[maybe_unused]] struct wl_array *states) {
    /* Dialogs keep their size; activation is tracked via keyboard focus. */
}

static void click(xd_widget *w);

static void toplevel_close(void *data, [[maybe_unused]] struct xdg_toplevel *t) {
    xd_window *win = data;
    if (win->cancel != nullptr && win->cancel->enabled)
        click(win->cancel);
    else
        xd_window_close(win);
}

static void toplevel_bounds([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_toplevel *t,
                            [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}

static void toplevel_caps([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_toplevel *t,
                          [[maybe_unused]] struct wl_array *caps) {}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_caps,
};

static void window_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *f,
                         uint32_t scale_120) {
    xd_window *win = data;
    double s = scale_120 / 120.0;
    if (s >= 0.5 && s <= 8.0 && s != win->scale) {
        win->scale = s;
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
        if (is_focusable(win->widgets[i]))
            xd_window_set_focus(win, win->widgets[i]);
    }
    wl_surface_commit(win->surface);
}

static void widget_free(xd_widget *w) {
    XD_FREE(&w->text);
    XD_FREE(&w->text2);
    xd_textedit_free(&w->edit);
    for (size_t i = 0; i < w->count; i++)
        xd_free(w->items[i]);
    xd_free(w->items);
    xd_free(w->tab_x);
    xd_free(w->tab_w);
    xd_free(w);
}

void xd_window_close(xd_window *win) {
    xd_ui *ui = win->ui;
    popup_close(win);
    if (ui->pointer_window == win)
        ui->pointer_window = nullptr;
    if (ui->keyboard_window == win)
        ui->keyboard_window = nullptr;
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
xd_window *xd_widget_window(xd_widget *w) { return w->win; }

xd_window *xd_window_for_surface(xd_ui *ui, struct wl_surface *s) {
    XD_LIST_FOR_EACH(win, &ui->windows, xd_window, link) {
        if (win->surface == s)
            return win;
    }
    return nullptr;
}

void xd_window_set_default(xd_window *win, xd_widget *button) { win->def = button; }
void xd_window_set_cancel(xd_window *win, xd_widget *button) { win->cancel = button; }

void xd_window_set_focus(xd_window *win, xd_widget *w) {
    if (w != nullptr && !is_focusable(w))
        return;
    if (win->focus != w && w != nullptr && is_text_widget(w))
        xd_textedit_select_all(&w->edit); /* Windows selects on Tab-in */
    win->focus = w;
    win->dirty = true;
}

/* ---- widgets ---------------------------------------------------------------------- */

static xd_widget *add(xd_window *win, xd_widget_kind kind, xd_rect r) {
    xd_widget *w = XD_NEW(xd_widget);
    w->kind = kind;
    w->win = win;
    w->r = r;
    w->enabled = true;
    w->selected = -1;
    w->page = -1;
    if (win->count == win->cap) {
        win->cap = win->cap == 0 ? 8 : win->cap * 2;
        win->widgets = xd_realloc_array(win->widgets, win->cap, sizeof(*win->widgets));
    }
    win->widgets[win->count++] = w;
    win->dirty = true;
    return w;
}

static char *copy_str(const char *s) { return xd_str_dup_cstr(xd_str_from_cstr(s)); }

xd_widget *xd_label(xd_window *win, xd_rect r, const char *text, uint8_t flags) {
    xd_widget *w = add(win, XD_W_LABEL, r);
    w->text = copy_str(text);
    w->flags = flags;
    return w;
}

xd_widget *xd_image(xd_window *win, xd_rect r, const char *icon_name) {
    xd_widget *w = add(win, XD_W_IMAGE, r);
    w->text = copy_str(icon_name);
    return w;
}

xd_widget *xd_banner(xd_window *win, xd_rect r, const char *product, const char *edition) {
    xd_widget *w = add(win, XD_W_BANNER, r);
    w->text = copy_str(product);
    w->text2 = copy_str(edition);
    return w;
}

xd_widget *xd_button(xd_window *win, xd_rect r, const char *label, xd_widget_cb on_click,
                     void *user) {
    xd_widget *w = add(win, XD_W_BUTTON, r);
    w->text = copy_str(label);
    w->cb = on_click;
    w->user = user;
    return w;
}

xd_widget *xd_edit(xd_window *win, xd_rect r, const char *text) {
    xd_widget *w = add(win, XD_W_EDIT, r);
    w->edit.max_len = 4096;
    xd_textedit_set(&w->edit, xd_str_from_cstr(text));
    return w;
}

xd_widget *xd_combo(xd_window *win, xd_rect r, bool editable, xd_widget_cb on_change,
                    void *user) {
    xd_widget *w = add(win, XD_W_COMBO, r);
    w->editable = editable;
    w->cb = on_change;
    w->user = user;
    w->edit.max_len = 4096;
    return w;
}

void xd_combo_add(xd_widget *w, const char *item) { xd_widget_add_item(w, item); }

static void xd_widget_add_item_any(xd_widget *w, const char *item);

void xd_widget_add_item(xd_widget *w, const char *item) {
    XD_ASSERT(w->kind == XD_W_COMBO || w->kind == XD_W_LISTBOX);
    xd_widget_add_item_any(w, item);
}

static void xd_widget_add_item_any(xd_widget *w, const char *item) {
    if (w->count == w->cap) {
        w->cap = w->cap == 0 ? 8 : w->cap * 2;
        w->items = xd_realloc_array(w->items, w->cap, sizeof(*w->items));
    }
    w->items[w->count++] = copy_str(item);
    w->win->dirty = true;
}

int xd_combo_selected(const xd_widget *w) { return w->selected; }
int xd_widget_selected(const xd_widget *w) { return w->selected; }
size_t xd_widget_item_count(const xd_widget *w) { return w->count; }

void xd_combo_select(xd_widget *w, int index) {
    XD_ASSERT(w->kind == XD_W_COMBO);
    xd_widget_select(w, index);
}

void xd_widget_select(xd_widget *w, int index) {
    XD_ASSERT(w->kind == XD_W_COMBO || w->kind == XD_W_LISTBOX || w->kind == XD_W_TABS);
    if (index < 0 || (size_t)index >= w->count)
        return;
    w->selected = index;
    if (w->kind == XD_W_COMBO && w->editable)
        xd_textedit_set(&w->edit, xd_str_from_cstr(w->items[index]));
    if (w->kind == XD_W_LISTBOX)
        list_scroll_to(w, index);
    if (w->kind == XD_W_TABS && w->win->focus != nullptr && !is_visible(w->win->focus))
        w->win->focus = w; /* focus was on the page we left */
    w->win->dirty = true;
}

xd_widget *xd_tabs(xd_window *win, xd_rect r, const char *const *labels, size_t count,
                   xd_widget_cb on_change, void *user) {
    XD_ASSERT(win->tabs == nullptr); /* one per window */
    xd_widget *w = add(win, XD_W_TABS, r);
    w->cb = on_change;
    w->user = user;
    w->tab_x = xd_alloc_array(XD_MAX(count, 1), sizeof(int));
    w->tab_w = xd_alloc_array(XD_MAX(count, 1), sizeof(int));

    /* Tabs are as wide as their labels, measured at logical scale. */
    xd_theme t;
    xd_theme_init(&t, win->ui->style, &win->ui->scheme, 1.0);
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(scratch);
    xd_text_style st = xd_text(&t, XD_FONT_MESSAGE, xd_rgba_hex(0), XD_ALIGN_START);
    int x = 2;
    for (size_t i = 0; i < count; i++) {
        xd_widget_add_item_any(w, labels[i]);
        w->tab_x[i] = x;
        w->tab_w[i] = xd_text_width(cr, labels[i], &st) + 14;
        x += w->tab_w[i];
    }
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);
    w->selected = count > 0 ? 0 : -1;
    win->tabs = w;
    return w;
}

void xd_widget_set_page(xd_widget *w, int page) {
    w->page = page;
    w->win->dirty = true;
}

xd_widget *xd_checkbox(xd_window *win, xd_rect r, const char *label, bool checked,
                       xd_widget_cb on_change, void *user) {
    xd_widget *w = add(win, XD_W_CHECKBOX, r);
    w->text = copy_str(label);
    w->checked = checked;
    w->cb = on_change;
    w->user = user;
    return w;
}

bool xd_checkbox_checked(const xd_widget *w) { return w->checked; }

void xd_checkbox_set(xd_widget *w, bool checked) {
    w->checked = checked;
    w->win->dirty = true;
}

xd_widget *xd_listbox(xd_window *win, xd_rect r, xd_widget_cb on_change, void *user) {
    xd_widget *w = add(win, XD_W_LISTBOX, r);
    w->cb = on_change;
    w->user = user;
    return w;
}

xd_widget *xd_groupbox(xd_window *win, xd_rect r, const char *label) {
    xd_widget *w = add(win, XD_W_GROUPBOX, r);
    w->text = copy_str(label);
    return w;
}

xd_widget *xd_custom(xd_window *win, xd_rect r, xd_draw_cb draw, void *user) {
    xd_widget *w = add(win, XD_W_CUSTOM, r);
    w->draw = draw;
    w->user = user;
    return w;
}

void xd_widget_redraw(xd_widget *w) { w->win->dirty = true; }

const char *xd_widget_text(xd_widget *w) {
    switch (w->kind) {
    case XD_W_EDIT:
        return edit_cstr(w);
    case XD_W_COMBO:
        if (w->editable)
            return edit_cstr(w);
        return w->selected >= 0 ? w->items[w->selected] : "";
    case XD_W_LISTBOX:
    case XD_W_TABS:
        return w->selected >= 0 ? w->items[w->selected] : "";
    case XD_W_LABEL:
    case XD_W_IMAGE:
    case XD_W_BANNER:
    case XD_W_BUTTON:
    case XD_W_CHECKBOX:
    case XD_W_GROUPBOX:
    case XD_W_CUSTOM:
        break;
    }
    return w->text != nullptr ? w->text : "";
}

void xd_widget_set_text(xd_widget *w, const char *text) {
    if (is_text_widget(w)) {
        xd_textedit_set(&w->edit, xd_str_from_cstr(text));
    } else {
        XD_FREE(&w->text);
        w->text = copy_str(text);
    }
    w->win->dirty = true;
}

void xd_widget_set_enabled(xd_widget *w, bool enabled) {
    if (w->enabled == enabled)
        return;
    w->enabled = enabled;
    if (!enabled && w->win->focus == w)
        w->win->focus = nullptr;
    w->win->dirty = true;
}

/* ---- input ----------------------------------------------------------------------- */

/* Runs a button's action. The callback may close the window: callers must
 * return without touching the window afterwards. */
static void click(xd_widget *w) {
    if (w->enabled && w->cb != nullptr)
        w->cb(w, w->user);
}

static xd_widget *widget_at(xd_window *win, double x, double y) {
    for (size_t i = win->count; i-- > 0;) {
        xd_widget *w = win->widgets[i];
        if (is_focusable(w) && contains(w->r, x, y))
            return w;
    }
    return nullptr;
}

static bool over_text(xd_window *win, xd_widget *w, double x, double y) {
    if (w == nullptr || !is_text_widget(w))
        return false;
    xd_theme t;
    window_theme(win, &t);
    xd_rect a = text_area(&t, w);
    return contains(a, x * win->scale, y * win->scale);
}

/* Selection change from the user: notify. May close the window. */
static void select_changed(xd_widget *w) {
    w->win->dirty = true;
    if (w->cb != nullptr)
        w->cb(w, w->user);
}

static void select_index(xd_widget *w, int index) {
    if (index < 0 || (size_t)index >= w->count || index == w->selected)
        return;
    xd_widget_select(w, index);
    select_changed(w);
}

/* Scroll thumb dragging: map the pointer back to a first row. */
static void list_drag_thumb(xd_widget *w, double y) {
    xd_theme t;
    xd_theme_init(&t, w->win->ui->style, &w->win->ui->scheme, 1.0);
    xd_rect sb = list_scrollbar(w);
    int rows = list_rows(w);
    xd_rect thumb = xd_theme_vscrollbar_thumb(&t, sb, w->first, rows, (int)w->count);
    int arrow = XD_MIN(sb.w, sb.h / 2);
    double track_h = sb.h - 2 * arrow - thumb.h;
    int range = (int)w->count - rows;
    if (track_h <= 0 || range <= 0)
        return;
    double pos = (y - w->win->thumb_offset - (sb.y + arrow)) / track_h;
    int first = (int)lround(XD_CLAMP(pos, 0.0, 1.0) * range);
    if (first != w->first) {
        w->first = first;
        w->win->dirty = true;
    }
}

void xd_window_pointer_axis(xd_window *win, double value) {
    xd_widget *w = widget_at(win, win->ui->pointer_x, win->ui->pointer_y);
    if (w == nullptr || w->kind != XD_W_LISTBOX)
        return;
    w->first += value > 0 ? 3 : -3; /* Windows scrolls three lines a notch */
    list_clamp(w);
    win->dirty = true;
}

void xd_window_pointer_motion(xd_window *win, double x, double y) {
    if (win->thumb_drag && win->pressed != nullptr) {
        list_drag_thumb(win->pressed, y);
        return;
    }
    if (win->selecting && win->pressed != nullptr) {
        xd_textedit_set_cursor(&win->pressed->edit, text_pos_at(win, win->pressed, x), true);
        win->dirty = true;
        return;
    }
    xd_widget *hot = widget_at(win, x, y);
    if (hot != win->hot) {
        win->hot = hot;
        win->dirty = true;
    }
    xd_ui_set_cursor_text(win->ui, over_text(win, hot, x, y));
}

void xd_window_pointer_leave(xd_window *win) {
    if (win->hot != nullptr) {
        win->hot = nullptr;
        win->dirty = true;
    }
}

void xd_window_pointer_button(xd_window *win, uint32_t button, bool pressed) {
    if (button != BTN_LEFT)
        return;
    double x = win->ui->pointer_x;
    double y = win->ui->pointer_y;
    if (!pressed) {
        xd_widget *target = win->pressed;
        bool released_over = target != nullptr && win->hot == target;
        win->pressed = nullptr;
        win->selecting = false;
        win->thumb_drag = false;
        win->dirty = true;
        if (released_over && target->kind == XD_W_BUTTON) {
            click(target); /* may close the window */
        } else if (released_over && target->kind == XD_W_CHECKBOX) {
            target->checked = !target->checked;
            select_changed(target); /* may close the window */
        }
        return;
    }

    xd_widget *w = widget_at(win, x, y);
    if (w == nullptr)
        return;
    if (win->focus != w) {
        win->focus = w;
        win->dirty = true;
    }
    win->pressed = w;
    if (w->kind == XD_W_TABS) {
        for (size_t i = 0; i < w->count; i++) {
            if (contains(tab_rect(w, i), x, y)) {
                select_index(w, (int)i); /* may close the window */
                return;
            }
        }
        return;
    }
    if (w->kind == XD_W_LISTBOX) {
        if (list_has_scrollbar(w) && contains(list_scrollbar(w), x, y)) {
            xd_theme t;
            xd_theme_init(&t, win->ui->style, &win->ui->scheme, 1.0);
            xd_rect sb = list_scrollbar(w);
            int rows = list_rows(w);
            xd_rect thumb = xd_theme_vscrollbar_thumb(&t, sb, w->first, rows, (int)w->count);
            int arrow = XD_MIN(sb.w, sb.h / 2);
            if (y < sb.y + arrow)
                w->first -= 1;
            else if (y >= sb.y + sb.h - arrow)
                w->first += 1;
            else if (y < thumb.y)
                w->first -= rows;
            else if (y >= thumb.y + thumb.h)
                w->first += rows;
            else {
                win->thumb_drag = true;
                win->thumb_offset = y - thumb.y;
            }
            list_clamp(w);
            win->dirty = true;
            return;
        }
        double top = w->r.y + (double)XD_LIST_BORDER;
        int row = (int)((y - top) / (double)XD_LIST_ROW_H);
        if (y >= top && row >= 0 && row < list_rows(w))
            select_index(w, w->first + row); /* may close the window */
        return;
    }
    if (w->kind == XD_W_COMBO && (!w->editable || !over_text(win, w, x, y))) {
        if (win->popup != nullptr)
            popup_close(win);
        else
            popup_open(win, w);
    } else if (is_text_widget(w)) {
        xd_textedit_set_cursor(&w->edit, text_pos_at(win, w, x), false);
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
    size_t n = win->count;
    size_t i = start;
    for (size_t tries = 0; tries < n; tries++) {
        i = dir > 0 ? (i + 1) % n : (i + n - 1) % n;
        if (is_focusable(win->widgets[i])) {
            xd_window_set_focus(win, win->widgets[i]);
            return;
        }
    }
}

static void edit_changed(xd_widget *w) {
    w->win->dirty = true;
    if (w->kind == XD_W_COMBO) {
        w->selected = -1; /* typed text no longer matches an item */
        if (w->cb != nullptr)
            w->cb(w, w->user);
    }
}

/* Text editing keys; returns true if handled. */
static bool edit_key(xd_widget *w, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
    xd_textedit *e = &w->edit;
    bool shift = mods & XD_MOD_SHIFT;
    bool ctrl = mods & XD_MOD_CTRL;
    xd_ui *ui = w->win->ui;

    switch (sym) {
    case XKB_KEY_Left:
        xd_textedit_move(e, ctrl ? XD_MOTION_WORD_LEFT : XD_MOTION_LEFT, shift);
        w->win->dirty = true;
        return true;
    case XKB_KEY_Right:
        xd_textedit_move(e, ctrl ? XD_MOTION_WORD_RIGHT : XD_MOTION_RIGHT, shift);
        w->win->dirty = true;
        return true;
    case XKB_KEY_Home:
        xd_textedit_move(e, XD_MOTION_HOME, shift);
        w->win->dirty = true;
        return true;
    case XKB_KEY_End:
        xd_textedit_move(e, XD_MOTION_END, shift);
        w->win->dirty = true;
        return true;
    case XKB_KEY_BackSpace:
        xd_textedit_backspace(e, ctrl);
        edit_changed(w);
        return true;
    case XKB_KEY_Delete:
        if (shift) { /* Shift+Del = cut */
            xd_ui_clipboard_copy(ui, xd_textedit_selected(e));
            xd_textedit_delete_selection(e);
        } else {
            xd_textedit_delete(e, ctrl);
        }
        edit_changed(w);
        return true;
    default:
        break;
    }

    if (ctrl) {
        switch (sym) {
        case XKB_KEY_a:
        case XKB_KEY_A:
            xd_textedit_select_all(e);
            w->win->dirty = true;
            return true;
        case XKB_KEY_c:
        case XKB_KEY_C:
        case XKB_KEY_Insert:
            xd_ui_clipboard_copy(ui, xd_textedit_selected(e));
            return true;
        case XKB_KEY_x:
        case XKB_KEY_X:
            xd_ui_clipboard_copy(ui, xd_textedit_selected(e));
            xd_textedit_delete_selection(e);
            edit_changed(w);
            return true;
        case XKB_KEY_v:
        case XKB_KEY_V: {
            char *text = xd_ui_clipboard_paste(ui);
            if (text != nullptr) {
                xd_textedit_insert(e, xd_str_from_cstr(text));
                xd_free(text);
                edit_changed(w);
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
            xd_textedit_insert(e, xd_str_from_cstr(text));
            xd_free(text);
            edit_changed(w);
        }
        return true;
    }
    if (!(mods & XD_MOD_ALT) && utf8 != nullptr && (unsigned char)utf8[0] >= 0x20 &&
        utf8[0] != 0x7F) {
        xd_textedit_insert(e, xd_str_from_cstr(utf8));
        edit_changed(w);
        return true;
    }
    return false;
}

static void combo_step(xd_widget *w, int dir) {
    if (w->count == 0)
        return;
    int i = w->selected < 0 ? (dir > 0 ? 0 : (int)w->count - 1) : w->selected + dir;
    i = XD_CLAMP(i, 0, (int)w->count - 1);
    if (i != w->selected) {
        xd_combo_select(w, i);
        combo_changed(w); /* must be last: may close the window */
    }
}

void xd_window_key(xd_window *win, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
    xd_widget *f = win->focus;
    bool alt = mods & XD_MOD_ALT;

    /* An open drop-down list takes navigation keys. */
    xd_listpopup *p = win->popup;
    if (p != nullptr) {
        switch (sym) {
        case XKB_KEY_Up:
        case XKB_KEY_Down:
            if (alt) {
                popup_close(win);
                return;
            }
            p->hot = XD_CLAMP(p->hot + (sym == XKB_KEY_Down ? 1 : -1), 0, (int)p->combo->count - 1);
            popup_scroll_to(p, p->hot);
            p->dirty = true;
            return;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            popup_choose(win, p->hot);
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
        int n = (int)tabs->count;
        select_index(tabs, (tabs->selected + (back ? n - 1 : 1)) % n);
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
        if (f != nullptr && f->kind == XD_W_BUTTON)
            click(f);
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
    if (f == nullptr)
        return;

    if (f->kind == XD_W_BUTTON) {
        if (sym == XKB_KEY_space)
            click(f);
        return;
    }
    if (f->kind == XD_W_CHECKBOX) {
        if (sym == XKB_KEY_space) {
            f->checked = !f->checked;
            select_changed(f);
        }
        return;
    }
    if (f->kind == XD_W_TABS) {
        if (sym == XKB_KEY_Left || sym == XKB_KEY_Right)
            select_index(f, XD_CLAMP(f->selected + (sym == XKB_KEY_Right ? 1 : -1), 0,
                                     (int)f->count - 1));
        return;
    }
    if (f->kind == XD_W_LISTBOX) {
        int target = f->selected;
        int rows = list_rows(f);
        switch (sym) {
        case XKB_KEY_Up:
            target = XD_MAX(f->selected - 1, 0);
            break;
        case XKB_KEY_Down:
            target = XD_MIN(f->selected + 1, (int)f->count - 1);
            break;
        case XKB_KEY_Prior:
            target = XD_MAX(f->selected - rows, 0);
            break;
        case XKB_KEY_Next:
            target = XD_MIN(f->selected + rows, (int)f->count - 1);
            break;
        case XKB_KEY_Home:
            target = 0;
            break;
        case XKB_KEY_End:
            target = (int)f->count - 1;
            break;
        default:
            return;
        }
        select_index(f, target);
        return;
    }
    if (f->kind == XD_W_COMBO) {
        if ((alt && (sym == XKB_KEY_Down || sym == XKB_KEY_Up)) || sym == XKB_KEY_F4) {
            popup_open(win, f);
            return;
        }
        if (sym == XKB_KEY_Down || sym == XKB_KEY_Up) {
            combo_step(f, sym == XKB_KEY_Down ? 1 : -1);
            return;
        }
        if (!f->editable) {
            if (sym == XKB_KEY_Home)
                combo_step(f, -(int)f->count);
            else if (sym == XKB_KEY_End)
                combo_step(f, (int)f->count);
            return;
        }
    }
    if (is_text_widget(f))
        edit_key(f, sym, utf8, mods);
}
