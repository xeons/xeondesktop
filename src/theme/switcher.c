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

static metrics metrics_for(const xd_theme *theme) {
    if (xd_pkgstyle_has_switcher(theme))
        return (metrics){xd_tpx(theme, 12), xd_tpx(theme, 52), xd_tpx(theme, 24), xd_tpx(theme, 6),
                         true};
    return (metrics){xd_tpx(theme, 10), xd_tpx(theme, 43), xd_tpx(theme, 24), xd_tpx(theme, 8),
                     false};
}

static size_t rows_for(size_t count) { return (XD_MAX(count, (size_t)1) + COLUMNS - 1) / COLUMNS; }

void xd_theme_switcher_size(const xd_theme *theme, size_t count, int *width, int *height) {
    metrics layout = metrics_for(theme);
    size_t cols = XD_CLAMP(count, MIN_COLUMNS, COLUMNS);
    *width = 2 * layout.pad + (int)cols * layout.cell;
    *height = 2 * layout.pad + (int)rows_for(count) * layout.cell + layout.gap + layout.title_h;
}

static xd_rgba sys(const xd_theme *theme, xd_syscolor color_index) {
    return xd_rgba_from_rgb(theme->scheme.colors[color_index]);
}

static void draw_icon(const xd_theme *theme, cairo_t *cairo, cairo_surface_t *icon, xd_rect cell) {
    int px = xd_tpx(theme, ICON);
    int x = cell.x + (cell.w - px) / 2, y = cell.y + (cell.h - px) / 2;
    /* The taskbar's generic window, twice its size. */
    xd_theme big = *theme;
    big.scale = theme->scale * 2;
    if (icon == nullptr) {
        xd_style_task_icon(&big, cairo, nullptr, x, y);
        return;
    }
    cairo_save(cairo);
    cairo_rectangle(cairo, x, y, px, px);
    cairo_clip(cairo);
    cairo_set_source_surface(cairo, icon, x, y);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

void xd_theme_draw_switcher(const xd_theme *theme, cairo_t *cairo, xd_rect rect,
                            cairo_surface_t *const *icons, size_t count, size_t selected,
                            const char *title) {
    metrics layout = metrics_for(theme);
    bool pkg = xd_pkgstyle_has_switcher(theme);
    if (pkg) {
        xd_pkgstyle_draw_switcher_box(theme, cairo, rect);
    } else {
        /* Raised, as a dialog's edge. */
        xd_rect in = xd_draw_bevel(cairo, rect, xd_tpx(theme, 1), sys(theme, XD_COLOR_BUTTON_LIGHT),
                                   sys(theme, XD_COLOR_BUTTON_DK_SHADOW));
        in = xd_draw_bevel(cairo, in, xd_tpx(theme, 1), sys(theme, XD_COLOR_BUTTON_HILIGHT),
                           sys(theme, XD_COLOR_BUTTON_SHADOW));
        xd_fill_rect(cairo, in, sys(theme, XD_COLOR_BUTTON_FACE));
    }
    xd_rect title_r = {rect.x + layout.pad,
                       layout.title_on_top ? rect.y + layout.pad
                                           : rect.y + rect.h - layout.pad - layout.title_h,
                       rect.w - 2 * layout.pad, layout.title_h};
    int grid_y =
        layout.title_on_top ? title_r.y + layout.title_h + layout.gap : rect.y + layout.pad;
    size_t cols = XD_CLAMP(count, (size_t)1, COLUMNS);
    for (size_t i = 0; i < count; i++) {
        size_t row = i / COLUMNS, col = i % COLUMNS;
        /* The last row centred under the full ones. */
        size_t in_row = XD_MIN(count - row * COLUMNS, cols);
        int row_w = (int)in_row * layout.cell;
        xd_rect cell = {rect.x + (rect.w - row_w) / 2 + (int)col * layout.cell,
                        grid_y + (int)row * layout.cell, layout.cell, layout.cell};
        if (pkg)
            xd_pkgstyle_draw_switcher_item(theme, cairo, cell, i == selected, false);
        draw_icon(theme, cairo, icons != nullptr ? icons[i] : nullptr, cell);
        if (!pkg && i == selected) {
            /* A square round the icon, in the selection colour. */
            int border = xd_tpx(theme, 2), px = xd_tpx(theme, ICON) + 2 * xd_tpx(theme, 4);
            xd_rect sq = {cell.x + (cell.w - px) / 2, cell.y + (cell.h - px) / 2, px, px};
            xd_rgba color = sys(theme, XD_COLOR_HILIGHT);
            (void)xd_draw_bevel(cairo, sq, border, color, color);
        }
    }
    if (pkg) {
        xd_pkgstyle_draw_switcher_title(theme, cairo, title_r, title);
        return;
    }
    xd_rect in = xd_draw_bevel(cairo, title_r, xd_tpx(theme, 1), sys(theme, XD_COLOR_BUTTON_SHADOW),
                               sys(theme, XD_COLOR_BUTTON_HILIGHT));
    if (title == nullptr || *title == '\0')
        return;
    xd_text_style ts =
        xd_text(theme, XD_FONT_MESSAGE, sys(theme, XD_COLOR_BUTTON_TEXT), XD_ALIGN_CENTER);
    ts.ellipsize = true;
    xd_draw_text(cairo, xd_rect_inset(in, xd_tpx(theme, 4), 0), title, &ts);
}
