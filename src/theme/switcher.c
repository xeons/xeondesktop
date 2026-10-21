/* SPDX-License-Identifier: MIT
 * Alt+Tab's window. Windows 2000 and XP draw it the same in every style
 * (XP's visual styles never themed it): a raised box in the dialog colour,
 * the windows' 32 px icons in rows of up to seven, a square around the
 * selected one, and its title in a sunken box below. Vista and 7 style it
 * with their AltTab class: from a package that has it, 7's look without
 * glass thumbnails, the title above the icons and the selection a glassy
 * frame behind its icon.
 */
#include "style.h"

/* Icons in a row; the box's least width in icons, so a title has room. */
static constexpr size_t COLUMNS = 7;
static constexpr size_t MIN_COLUMNS = 5;
enum {
    ICON = 32,
};

typedef struct metrics {
    int pad;     /* around the contents */
    int cell;    /* an icon's square */
    int title_h; /* the title's box */
    int gap;     /* between the icons and the title */
    bool title_on_top;
} metrics;

static metrics metrics_for(const xd_theme *t) {
    if (xd_pkgstyle_has_switcher(t))
        return (metrics){xd_tpx(t, 12), xd_tpx(t, 52), xd_tpx(t, 24), xd_tpx(t, 6), true};
    return (metrics){xd_tpx(t, 10), xd_tpx(t, 43), xd_tpx(t, 24), xd_tpx(t, 8), false};
}

static size_t rows_for(size_t count) { return (XD_MAX(count, (size_t)1) + COLUMNS - 1) / COLUMNS; }

void xd_theme_switcher_size(const xd_theme *t, size_t count, int *w, int *h) {
    metrics m = metrics_for(t);
    size_t cols = XD_CLAMP(count, MIN_COLUMNS, COLUMNS);
    *w = 2 * m.pad + (int)cols * m.cell;
    *h = 2 * m.pad + (int)rows_for(count) * m.cell + m.gap + m.title_h;
}

static xd_rgba sys(const xd_theme *t, xd_syscolor c) {
    return xd_rgba_from_rgb(t->scheme.colors[c]);
}

static void draw_icon(const xd_theme *t, cairo_t *cr, cairo_surface_t *icon, xd_rect cell) {
    int px = xd_tpx(t, ICON);
    int x = cell.x + (cell.w - px) / 2, y = cell.y + (cell.h - px) / 2;
    /* The taskbar's generic window, twice its size. */
    xd_theme big = *t;
    big.scale = t->scale * 2;
    if (icon == nullptr) {
        xd_style_task_icon(&big, cr, nullptr, x, y);
        return;
    }
    cairo_save(cr);
    cairo_rectangle(cr, x, y, px, px);
    cairo_clip(cr);
    cairo_set_source_surface(cr, icon, x, y);
    cairo_paint(cr);
    cairo_restore(cr);
}

void xd_theme_draw_switcher(const xd_theme *t, cairo_t *cr, xd_rect r,
                            cairo_surface_t *const *icons, size_t count, size_t selected,
                            const char *title) {
    metrics m = metrics_for(t);
    bool pkg = xd_pkgstyle_has_switcher(t);
    if (pkg) {
        xd_pkgstyle_draw_switcher_box(t, cr, r);
    } else {
        /* Raised, as a dialog's edge. */
        xd_rect in = xd_draw_bevel(cr, r, xd_tpx(t, 1), sys(t, XD_COLOR_BUTTON_LIGHT),
                                   sys(t, XD_COLOR_BUTTON_DK_SHADOW));
        in = xd_draw_bevel(cr, in, xd_tpx(t, 1), sys(t, XD_COLOR_BUTTON_HILIGHT),
                           sys(t, XD_COLOR_BUTTON_SHADOW));
        xd_fill_rect(cr, in, sys(t, XD_COLOR_BUTTON_FACE));
    }
    xd_rect title_r = {r.x + m.pad,
                       m.title_on_top ? r.y + m.pad : r.y + r.h - m.pad - m.title_h,
                       r.w - 2 * m.pad, m.title_h};
    int grid_y = m.title_on_top ? title_r.y + m.title_h + m.gap : r.y + m.pad;
    size_t cols = XD_CLAMP(count, (size_t)1, COLUMNS);
    for (size_t i = 0; i < count; i++) {
        size_t row = i / COLUMNS, col = i % COLUMNS;
        /* The last row centred under the full ones. */
        size_t in_row = XD_MIN(count - row * COLUMNS, cols);
        int row_w = (int)in_row * m.cell;
        xd_rect cell = {r.x + (r.w - row_w) / 2 + (int)col * m.cell, grid_y + (int)row * m.cell,
                        m.cell, m.cell};
        if (pkg)
            xd_pkgstyle_draw_switcher_item(t, cr, cell, i == selected, false);
        draw_icon(t, cr, icons != nullptr ? icons[i] : nullptr, cell);
        if (!pkg && i == selected) {
            /* A square round the icon, in the selection colour. */
            int b = xd_tpx(t, 2), px = xd_tpx(t, ICON) + 2 * xd_tpx(t, 4);
            xd_rect sq = {cell.x + (cell.w - px) / 2, cell.y + (cell.h - px) / 2, px, px};
            xd_rgba c = sys(t, XD_COLOR_HILIGHT);
            (void)xd_draw_bevel(cr, sq, b, c, c);
        }
    }
    if (pkg) {
        xd_pkgstyle_draw_switcher_title(t, cr, title_r, title);
        return;
    }
    xd_rect in = xd_draw_bevel(cr, title_r, xd_tpx(t, 1), sys(t, XD_COLOR_BUTTON_SHADOW),
                               sys(t, XD_COLOR_BUTTON_HILIGHT));
    if (title == nullptr || *title == '\0')
        return;
    xd_text_style ts = xd_text(t, XD_FONT_MESSAGE, sys(t, XD_COLOR_BUTTON_TEXT), XD_ALIGN_CENTER);
    ts.ellipsize = true;
    xd_draw_text(cr, xd_rect_inset(in, xd_tpx(t, 4), 0), title, &ts);
}
