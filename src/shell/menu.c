/* SPDX-License-Identifier: MIT */
#include <linux/input-event-codes.h>
#include <math.h>
#include <sys/timerfd.h>
#include <glib.h>
#include <gio/gdesktopappinfo.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../settings/soundplay.h"
#include "../theme/draw.h"
#include "../ui/icons.h"
#include "gpu.h"
#include "menu.h"
#include "shell.h"

/* Windows' default MenuShowDelay; how far a press moves before it drags. */
enum { XD_MENU_DELAY_MS = 400, XD_MENU_MIN_W = 120 };
static constexpr double XD_MENU_DRAG_PX = 5;

/* Windows 7's jump lists, logical px (from memory): items with a 16px
 * icon, grey headers, a band at the bottom; items stop short of the
 * panel's edges, and a hovered one shows its pin button at the right. */
enum {
    JL_ITEM_H = 26,
    JL_HEADER_H = 24,
    JL_PAD = 6,     /* above the first item, between sections, around the band */
    JL_INSET = 4,   /* an item's box from the panel's sides */
    JL_ICON_X = 6,  /* the icon, from the item's left */
    JL_TEXT_X = 30, /* the text */
    JL_HEADER_X = 10,
    JL_PIN_W = 26,
    JL_MIN_W = 256,
    JL_MAX_W = 360,
};

/* ---- models ------------------------------------------------------------------ */

void xd_menu_tree_init(xd_menu_tree *tree) {
    *tree = (xd_menu_tree){0};
    xd_arena_init(&tree->arena, 16 * 1024);
}

void xd_menu_tree_free(xd_menu_tree *tree) {
    for (size_t i = 0; i < tree->count; i++)
        xd_free(tree->models[i]->items);
    xd_free(tree->models);
    xd_arena_destroy(&tree->arena);
    *tree = (xd_menu_tree){0};
}

void xd_menu_model_set_look(xd_menu_model *model, uint8_t look) {
    if (model == nullptr || model->look == look)
        return;
    model->look = look;
    for (size_t i = 0; i < model->count; i++)
        xd_menu_model_set_look(model->items[i].submenu, look);
}

xd_menu_model *xd_menu_model_new(xd_menu_tree *tree) {
    xd_menu_model *menu_model = XD_ARENA_NEW(&tree->arena, xd_menu_model);
    if (tree->count == tree->cap) {
        tree->cap = tree->cap == 0 ? 16 : tree->cap * 2;
        tree->models = xd_realloc_array(tree->models, tree->cap, sizeof(*tree->models));
    }
    tree->models[tree->count++] = menu_model;
    return menu_model;
}

xd_menu_item *xd_menu_add(xd_menu_tree *tree, xd_menu_model *model, xd_menu_kind kind,
                          const char *label, const char *icon) {
    if (model->count == model->cap) {
        model->cap = model->cap == 0 ? 8 : model->cap * 2;
        model->items = xd_realloc_array(model->items, model->cap, sizeof(*model->items));
    }
    xd_menu_item *it = &model->items[model->count++];
    *it = (xd_menu_item){
        .kind = kind,
        .label = label != nullptr ? xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(label))
                                  : "",
        .icon = icon != nullptr ? xd_str_arena_cstr(&tree->arena, xd_str_from_cstr(icon))
                                : nullptr,
    };
    return it;
}

xd_menu_item *xd_menu_add_keyed(xd_menu_tree *tree, xd_menu_model *model, xd_menu_kind kind,
                                const char *label, const char *icon) {
    int key = -1;
    char *text = xd_text_strip_prefix(label, &key);
    xd_menu_item *it = xd_menu_add(tree, model, kind, text, icon);
    g_free(text);
    if (key >= 0) {
        it->key = it->label + key;
        gunichar letter = g_utf8_get_char_validated(it->key, -1);
        if (letter != (gunichar)-1 && letter != (gunichar)-2)
            it->mnemonic = g_unichar_tolower(letter);
        else
            it->key = nullptr;
    }
    return it;
}

/* ---- keyboard cues --------------------------------------------------------------- */

bool xd_menu_cues(const xd_shell *shell) {
    return !shell->settings.hide_keyboard_cues || shell->menu_cues;
}

void xd_menu_show_cues(xd_shell *shell) {
    if (shell->menu_cues)
        return;
    shell->menu_cues = true;
    for (xd_menu *menu = shell->start_menu; menu != nullptr; menu = menu->child)
        menu->dirty = true;
    for (xd_menu *menu = xd_start7_submenu(shell); menu != nullptr; menu = menu->child)
        menu->dirty = true;
    xd_start7_redraw(shell);
}

/* ---- layout -------------------------------------------------------------------- */

/* A jump list: one column, sections under their headers, then the band
 * (from the separator on). */
static void layout_jumplist(xd_menu *menu, int text_w) {
    const xd_menu_model *menu_model = menu->model;
    size_t count = XD_MAX(menu_model->count, 1);
    menu->item_y = xd_alloc_array(count, sizeof(int));
    menu->item_h = xd_alloc_array(count, sizeof(int));
    menu->item_xs = xd_alloc_array(count, sizeof(int));
    menu->width = XD_CLAMP(JL_TEXT_X + text_w + JL_PIN_W + 2 * JL_INSET + 12, JL_MIN_W, JL_MAX_W);
    menu->item_x = JL_INSET;
    menu->item_w = menu->width - 2 * JL_INSET;
    menu->band_y = -1;
    int y = 1 + JL_PAD;
    bool any = false;
    for (size_t i = 0; i < menu_model->count; i++) {
        const xd_menu_item *it = &menu_model->items[i];
        int height = JL_ITEM_H;
        if (it->kind == XD_MENU_SEPARATOR) {
            menu->band_y = any ? y + JL_PAD : 0;
            y = menu->band_y + JL_PAD;
            height = 0;
        } else if (it->kind == XD_MENU_HEADER) {
            if (any)
                y += JL_PAD;
            height = JL_HEADER_H;
        }
        menu->item_xs[i] = JL_INSET;
        menu->item_y[i] = y;
        menu->item_h[i] = height;
        y += height;
        any = true;
    }
    menu->height = y + JL_PAD + 1;
    if (menu->band_y < 0)
        menu->band_y = menu->height;
}

/* Sizes the menu in logical units from a scale-1 theme. */
static void layout(xd_menu *menu) {
    const xd_menu_model *menu_model = menu->model;
    xd_theme theme;
    xd_theme_init(&theme, menu->shell->style, &menu->shell->scheme, 1.0);
    xd_menu_metrics mm = xd_theme_menu_metrics(&theme);

    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cairo = cairo_create(scratch);
    int text_w = 0;
    for (size_t i = 0; i < menu_model->count; i++) {
        const xd_menu_item *it = &menu_model->items[i];
        if (it->kind == XD_MENU_SEPARATOR)
            continue;
        int width = it->bold || it->kind == XD_MENU_HEADER
                        ? xd_theme_menu_bold_text_width(&theme, cairo, it->label)
                        : xd_theme_menu_text_width(&theme, cairo, it->label);
        if (it->accel != nullptr)
            width += xd_theme_menu_text_width(&theme, cairo, it->accel) + 24;
        text_w = XD_MAX(text_w, width);
    }
    cairo_destroy(cairo);
    cairo_surface_destroy(scratch);
    if (menu_model->jumplist) {
        layout_jumplist(menu, text_w);
        return;
    }

    int banner = menu_model->banner_product != nullptr ? mm.banner_w : 0;
    int text_x = menu_model->large ? mm.large_text_x : mm.text_x;
    /* Slack so hinting at other scales doesn't ellipsize labels. */
    int item_w = XD_MAX(text_x + text_w + mm.arrow_w + 12, XD_MENU_MIN_W);

    menu->item_y = xd_alloc_array(XD_MAX(menu_model->count, 1), sizeof(int));
    menu->item_h = xd_alloc_array(XD_MAX(menu_model->count, 1), sizeof(int));
    menu->item_xs = xd_alloc_array(XD_MAX(menu_model->count, 1), sizeof(int));
    menu->item_x = mm.border + banner;
    menu->item_w = item_w;
    /* A menu taller than the screen continues in another column, as
     * Windows 2000's Programs folders do. */
    int max_h = xd_shell_screen_height(menu->shell) - 2 * mm.border;
    int x = menu->item_x;
    int y = mm.border;
    int tallest = 0;
    int columns = 1;
    for (size_t i = 0; i < menu_model->count; i++) {
        int height = menu_model->items[i].kind == XD_MENU_SEPARATOR ? mm.separator_h
                     : menu_model->large                            ? mm.large_item_h
                                                                    : mm.item_h;
        if (y + height > mm.border + max_h && y > mm.border) {
            x += item_w;
            y = mm.border;
            columns++;
        }
        menu->item_xs[i] = x;
        menu->item_y[i] = y;
        menu->item_h[i] = height;
        y += height;
        tallest = XD_MAX(tallest, y);
    }
    menu->width = mm.border * 2 + banner + item_w * columns;
    menu->height = XD_MAX(tallest, mm.border) + mm.border;
}

/* A jump list item's pin button (logical), at its right end. */
static xd_rect pin_rect(const xd_menu *menu, int i) {
    return (xd_rect){menu->item_xs[i] + menu->item_w - JL_PIN_W, menu->item_y[i], JL_PIN_W,
                     menu->item_h[i]};
}

static bool on_pin(const xd_menu *menu, int i, double x, double y) {
    if (!menu->model->jumplist || i < 0 || menu->model->items[i].pin == nullptr)
        return false;
    xd_rect rect = pin_rect(menu, i);
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

static int item_at(const xd_menu *menu, double x, double y) {
    if (x < 0 || x >= menu->width)
        return -1;
    for (size_t i = 0; i < menu->model->count; i++) {
        /* The Windows 2000 banner beside the first column isn't part of
         * any item (as in Windows); the last column reaches the right edge. */
        int left = menu->item_xs[i];
        int right = menu->item_xs[i] + menu->item_w;
        if (right + menu->item_w > menu->width)
            right = menu->width;
        if (x < left || x >= right)
            continue;
        if (y >= menu->item_y[i] && y < menu->item_y[i] + menu->item_h[i])
            return menu->model->items[i].kind == XD_MENU_SEPARATOR ||
                           menu->model->items[i].kind == XD_MENU_HEADER
                       ? -1
                       : (int)i;
    }
    return -1;
}

/* ---- rendering ---------------------------------------------------------------- */

static int dev(double logical, double scale) {
    double rounded = round(logical * scale);
    XD_ASSERT(rounded > -65536.0 && rounded < 65536.0);
    return (int)rounded;
}

/* Logical rect -> device rect with shared edges, so adjacent items tile
 * without gaps at fractional scales. */
static xd_rect to_device(int x, int y, int width, int height, double scale) {
    int x0 = dev(x, scale);
    int y0 = dev(y, scale);
    return (xd_rect){x0, y0, dev(x + width, scale) - x0, dev(y + height, scale) - y0};
}

static void draw_icon(xd_shell *shell, cairo_t *cairo, const xd_menu_item *it, xd_rect rect,
                      bool disabled) {
    /* Items without any icon (taskbar and window menus) leave the column
     * empty, or to their check mark or glyph. */
    if ((it->icon == nullptr && it->fallback_icon == nullptr && it->app == nullptr) ||
        it->checked || it->glyph != XD_MENU_GLYPH_NONE)
        return;
    const char *names[] = {it->icon, it->fallback_icon, "application-x-executable"};
    cairo_surface_t *icon = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names), rect.w);
    if (icon == nullptr)
        return;
    cairo_save(cairo);
    cairo_set_source_surface(cairo, icon, rect.x, rect.y);
    cairo_paint_with_alpha(cairo, disabled ? 0.45 : 1.0);
    cairo_restore(cairo);
}

static void render_jumplist(xd_menu *menu, const xd_theme *theme, cairo_t *cairo, int dw, int dh) {
    xd_shell *shell = menu->shell;
    const xd_menu_model *menu_model = menu->model;
    double scale = menu->scale;
    xd_theme_draw_jumplist_frame(theme, cairo, (xd_rect){0, 0, dw, dh}, dev(menu->band_y, scale));
    int icon = xd_tpx(theme, 16);
    for (size_t i = 0; i < menu_model->count; i++) {
        const xd_menu_item *it = &menu_model->items[i];
        if (it->kind == XD_MENU_SEPARATOR)
            continue;
        xd_rect rect =
            to_device(menu->item_xs[i], menu->item_y[i], menu->item_w, menu->item_h[i], scale);
        if (it->kind == XD_MENU_HEADER) {
            int hx = dev(JL_HEADER_X - JL_INSET, scale);
            xd_theme_draw_jumplist_header(
                theme, cairo, (xd_rect){rect.x + hx, rect.y, rect.w - hx, rect.h}, it->label);
            continue;
        }
        bool hot = menu->hot == (int)i;
        uint8_t state = (uint8_t)((hot ? XD_STATE_HOT : 0) | (it->disabled ? XD_STATE_DISABLED : 0));
        xd_rect text_r = rect;
        if (it->pin != nullptr)
            text_r.w -= dev(JL_PIN_W, scale);
        xd_theme_draw_jumplist_item(theme, cairo, text_r, state, it->label, dev(JL_TEXT_X, scale));
        xd_rect ir = {rect.x + dev(JL_ICON_X, scale), rect.y + (rect.h - icon) / 2, icon, icon};
        if (it->pushpin != 0)
            xd_theme_draw_pushpin(theme, cairo, ir, it->pushpin == 2);
        else
            draw_icon(shell, cairo, it, ir, it->disabled);
        if (hot && it->pin != nullptr) {
            xd_rect pr = pin_rect(menu, (int)i);
            uint8_t ps = menu->hot_pin ? XD_STATE_HOT : 0;
            if (menu->hot_pin && menu->pressed == (int)i)
                ps |= XD_STATE_PRESSED;
            xd_theme_draw_jumplist_pin(theme, cairo, to_device(pr.x, pr.y, pr.w, pr.h, scale), ps,
                                       it->pinned);
        }
    }
}

static void render(xd_menu *menu) {
    xd_shell *shell = menu->shell;
    double scale = menu->scale;
    int dw = dev(menu->width, scale);
    int dh = dev(menu->height, scale);
    if (dw <= 0 || dh <= 0)
        return;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&menu->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;

    xd_theme theme;
    xd_theme_init(&theme, shell->style, &shell->scheme, scale);
    xd_menu_metrics mm = xd_theme_menu_metrics(&theme);
    const xd_menu_model *menu_model = menu->model;
    theme.menu_look = (enum xd_menu_look)menu_model->look;
    bool cues = xd_menu_cues(shell);
    cairo_t *cairo = cairo_create(buf->surface);
    if (menu_model->jumplist) {
        render_jumplist(menu, &theme, cairo, dw, dh);
        cairo_destroy(cairo);
        goto commit;
    }
    xd_theme_draw_menu_frame(&theme, cairo, (xd_rect){0, 0, dw, dh});

    if (menu_model->banner_product != nullptr) {
        xd_theme t1;
        xd_theme_init(&t1, shell->style, &shell->scheme, 1.0);
        xd_menu_metrics m1 = xd_theme_menu_metrics(&t1);
        xd_rect banner =
            to_device(m1.border, m1.border, m1.banner_w, menu->height - 2 * m1.border, scale);
        xd_theme_draw_start_banner(
            &theme, cairo, banner, menu_model->banner_product,
            menu_model->banner_edition != nullptr ? menu_model->banner_edition : "");
    }

    for (size_t i = 0; i < menu_model->count; i++) {
        const xd_menu_item *it = &menu_model->items[i];
        xd_rect rect =
            to_device(menu->item_xs[i], menu->item_y[i], menu->item_w, menu->item_h[i], scale);
        if (it->kind == XD_MENU_SEPARATOR) {
            xd_theme_draw_menu_separator(&theme, cairo, rect);
            continue;
        }
        if (it->highlight && menu->hot != (int)i && !xd_theme_draw_new_program(&theme, cairo, rect))
            xd_fill_rect(cairo, rect, xd_rgba_hex(XD_NEW_PROGRAM_COLOR));
        uint8_t state = 0;
        /* Only the item under the pointer: the one whose submenu is still
         * open (it closes after the hover delay) loses its highlight at
         * once, as in Windows. Moving into the submenu or off the menu
         * sets `hot` back to it. */
        if (menu->hot == (int)i)
            state |= XD_STATE_HOT;
        bool header = it->kind == XD_MENU_HEADER;
        if (it->disabled || header)
            state |= XD_STATE_DISABLED;
        xd_menu_item_extras extras = {.bold = it->bold || header,
                                      .checked = it->checked,
                                      .glyph = it->glyph,
                                      .accel = it->accel,
                                      .underline = cues ? it->key : nullptr};
        xd_theme_draw_menu_item_ex(&theme, cairo, rect, state, it->label,
                                   it->kind == XD_MENU_SUBMENU, menu_model->large, &extras);
        int icon = menu_model->large ? mm.large_icon : mm.icon;
        draw_icon(shell, cairo, it,
                  (xd_rect){rect.x + mm.icon_x, rect.y + (rect.h - icon) / 2, icon, icon},
                  it->disabled);
    }
    cairo_destroy(cairo);
commit:
    cairo_surface_flush(buf->surface);

    wl_surface_attach(menu->surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(menu->surface, 0, 0, dw, dh);
    if (menu->viewport != nullptr)
        wp_viewport_set_destination(menu->viewport, menu->width, menu->height);
    wl_surface_commit(menu->surface);
    buf->busy = true;
    menu->dirty = false;
    menu->shown = true;
}

void xd_menus_render_dirty(xd_shell *shell) {
    for (xd_menu *menu = shell->start_menu; menu != nullptr; menu = menu->child) {
        if (menu->dirty && menu->configured)
            render(menu);
    }
    for (xd_menu *menu = xd_start7_submenu(shell); menu != nullptr; menu = menu->child) {
        if (menu->dirty && menu->configured)
            render(menu);
    }
}

/* ---- popup lifecycle ---------------------------------------------------------- */

static void menu_destroy(xd_menu *menu);

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
    xd_menu *menu = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    menu->configured = true;
    menu->dirty = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *popup,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *popup) {
    /* The compositor dismissed the menu (click outside). */
    xd_menu *menu = data;
    xd_menu_close_all(menu->shell);
}

static void popup_repositioned([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_popup *popup,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static void preferred_scale(void *data,
                            [[maybe_unused]] struct wp_fractional_scale_v1 *fractional_scale,
                            uint32_t scale_120) {
    xd_menu *menu = data;
    double scale = scale_120 / 120.0;
    if (scale >= 0.5 && scale <= 8.0 && scale != menu->scale) {
        menu->scale = scale;
        menu->dirty = true;
    }
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = preferred_scale,
};

static xd_menu *menu_create(xd_shell *shell, const xd_menu_model *model) {
    xd_menu *menu = XD_NEW(xd_menu);
    menu->shell = shell;
    menu->model = model;
    menu->hot = -1;
    menu->pressed = -1;
    menu->child_index = -1;
    menu->scale = 1.0;
    layout(menu);

    xd_sound_play_event(XD_SOUND_MENU_POPUP);
    menu->surface = wl_compositor_create_surface(shell->compositor);
    if (shell->fractional_manager != nullptr && shell->viewporter != nullptr) {
        menu->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            shell->fractional_manager, menu->surface);
        wp_fractional_scale_v1_add_listener(menu->fractional, &fractional_listener, menu);
        menu->viewport = wp_viewporter_get_viewport(shell->viewporter, menu->surface);
    }
    menu->xdg_surface = xdg_wm_base_get_xdg_surface(shell->wm_base, menu->surface);
    xdg_surface_add_listener(menu->xdg_surface, &xdg_surface_listener, menu);
    return menu;
}

static void menu_finish_popup(xd_menu *menu, uint32_t serial) {
    xdg_popup_add_listener(menu->popup, &popup_listener, menu);
    if (serial != 0 && menu->shell->seat != nullptr)
        xdg_popup_grab(menu->popup, menu->shell->seat, serial);
    wl_surface_commit(menu->surface);
}

xd_menu *xd_menu_open_root(xd_shell *shell, const xd_menu_model *model,
                           struct zwlr_layer_surface_v1 *layer, xd_rect anchor,
                           uint32_t serial) {
    return xd_menu_open_root_dir(shell, model, layer, anchor, serial, false);
}

xd_menu *xd_menu_open_root_bar(xd_shell *shell, const xd_menu_model *model, xd_taskbar *bar,
                               xd_rect anchor, uint32_t serial) {
    xd_menu_close_all(shell);
    xd_menu *menu = menu_create(shell, model);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, menu->width, menu->height);
    xd_taskbar_popup_position(bar, anchor, pos);
    menu->bar = bar;
    menu->anchor = anchor;
    menu->popup = xdg_surface_get_popup(menu->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(bar->layer, menu->popup);
    menu_finish_popup(menu, serial);
    shell->start_menu = menu;
    shell->menu_serial = serial;
    shell->menu_cues = false; /* the keyboard's openers say otherwise */
    return menu;
}

xd_menu *xd_menu_open_root_dir(xd_shell *shell, const xd_menu_model *model,
                               struct zwlr_layer_surface_v1 *layer, xd_rect anchor,
                               uint32_t serial, bool downwards) {
    xd_menu_close_all(shell);
    xd_menu *menu = menu_create(shell, model);

    /* Grows upwards from the Start button's top-left corner (or down from
     * the anchor, for the window menu). */
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, menu->width, menu->height);
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, XD_MAX(anchor.w, 1),
                                   XD_MAX(anchor.h, 1));
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, downwards ? XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT
                                              : XDG_POSITIONER_GRAVITY_TOP_RIGHT);
    xdg_positioner_set_constraint_adjustment(
        pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    menu->popup = xdg_surface_get_popup(menu->xdg_surface, nullptr, pos);
    xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(layer, menu->popup);
    menu_finish_popup(menu, serial);

    shell->start_menu = menu;
    shell->menu_serial = serial;
    shell->menu_cues = false;
    return menu;
}

xd_menu *xd_menu_open_child(xd_menu *menu, int index) {
    if (index < 0 || (size_t)index >= menu->model->count)
        return nullptr;
    const xd_menu_item *it = &menu->model->items[index];
    if (it->kind != XD_MENU_SUBMENU || it->disabled || it->submenu == nullptr ||
        it->submenu->count == 0)
        return nullptr;
    /* The compositor places a submenu relative to its parent's position,
     * which only takes effect once the parent has acked and committed. */
    if (!menu->shown)
        return nullptr;
    if (menu->child != nullptr) {
        if (menu->child_index == index)
            return menu->child;
        menu_destroy(menu->child);
    }

    xd_shell *shell = menu->shell;
    xd_menu *child = menu_create(shell, it->submenu);
    child->parent = menu;

    /* Opens to the right of the item, overlapping the parent's border as
     * Windows does; flips left at the screen edge. */
    xd_theme theme;
    xd_theme_init(&theme, shell->style, &shell->scheme, 1.0);
    int border = xd_theme_menu_metrics(&theme).border;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, child->width, child->height);
    xdg_positioner_set_anchor_rect(pos, menu->item_xs[index], menu->item_y[index], menu->item_w,
                                   menu->item_h[index]);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(pos, border - 1, -border);
    xdg_positioner_set_constraint_adjustment(
        pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);
    child->popup = xdg_surface_get_popup(child->xdg_surface, menu->xdg_surface, pos);
    xdg_positioner_destroy(pos);
    menu_finish_popup(child, shell->menu_serial);

    menu->child = child;
    menu->child_index = index;
    menu->dirty = true;
    return child;
}

xd_menu *xd_menu_open_attached(xd_shell *shell, const xd_menu_model *model,
                               struct xdg_surface *parent, xd_rect anchor, uint32_t serial) {
    xd_menu *menu = menu_create(shell, model);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, menu->width, menu->height);
    xdg_positioner_set_anchor_rect(pos, anchor.x, anchor.y, XD_MAX(anchor.w, 1),
                                   XD_MAX(anchor.h, 1));
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(
        pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);
    menu->popup = xdg_surface_get_popup(menu->xdg_surface, parent, pos);
    xdg_positioner_destroy(pos);
    shell->menu_serial = serial;
    menu_finish_popup(menu, serial);
    return menu;
}

void xd_menu_destroy(xd_menu *menu) { menu_destroy(menu); }

void xd_menu_replace_model(xd_menu *menu, const xd_menu_model *model) {
    int old_w = menu->width, old_h = menu->height;
    XD_FREE(&menu->item_y);
    XD_FREE(&menu->item_h);
    XD_FREE(&menu->item_xs);
    menu->model = model;
    menu->hot = -1;
    menu->pressed = -1;
    menu->hot_pin = false;
    layout(menu);
    /* The pointer resting on it is on whichever item is there now. */
    if (menu->shell->pointer_menu == menu) {
        menu->hot = item_at(menu, menu->ptr_x, menu->ptr_y);
        menu->hot_pin = on_pin(menu, menu->hot, menu->ptr_x, menu->ptr_y);
    }
    menu->dirty = true;
    xd_shell *shell = menu->shell;
    if ((menu->width == old_w && menu->height == old_h) || menu->bar == nullptr ||
        xdg_popup_get_version(menu->popup) < XDG_POPUP_REPOSITION_SINCE_VERSION)
        return;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, menu->width, menu->height);
    xd_taskbar_popup_position(menu->bar, menu->anchor, pos);
    xdg_popup_reposition(menu->popup, pos, 0);
    xdg_positioner_destroy(pos);
}

/* Destroys `menu` and everything opened from it, topmost first as the
 * protocol requires. */
static void menu_destroy(xd_menu *menu) {
    if (menu->child != nullptr)
        menu_destroy(menu->child);
    xd_shell *shell = menu->shell;
    if (shell->pending_menu == menu)
        shell->pending_menu = nullptr;
    if (shell->pointer_menu == menu)
        shell->pointer_menu = nullptr;
    if (menu->parent != nullptr) {
        menu->parent->child = nullptr;
        menu->parent->child_index = -1;
        menu->parent->dirty = true;
    }
    xdg_popup_destroy(menu->popup);
    xdg_surface_destroy(menu->xdg_surface);
    if (menu->viewport != nullptr)
        wp_viewport_destroy(menu->viewport);
    if (menu->fractional != nullptr)
        wp_fractional_scale_v1_destroy(menu->fractional);
    wl_surface_destroy(menu->surface);
    xd_shm_ring_finish(&menu->ring);
    if (menu->own_tree != nullptr) {
        xd_menu_tree_free(menu->own_tree);
        xd_free(menu->own_tree);
    }
    xd_free(menu->item_y);
    xd_free(menu->item_h);
    xd_free(menu->item_xs);
    xd_free(menu);
}

void xd_menu_close_all(xd_shell *shell) {
    if (shell->start7 != nullptr) {
        xd_start7_close(shell); /* calls xd_shell_start_menu_closed */
        return;
    }
    xd_menu *root = shell->start_menu;
    if (root == nullptr)
        return;
    shell->start_menu = nullptr;
    menu_destroy(root);
    xd_shell_start_menu_closed(shell);
}

xd_menu *xd_menu_for_surface(xd_shell *shell, struct wl_surface *surface) {
    for (xd_menu *menu = shell->start_menu; menu != nullptr; menu = menu->child) {
        if (menu->surface == surface)
            return menu;
    }
    for (xd_menu *menu = xd_start7_submenu(shell); menu != nullptr; menu = menu->child) {
        if (menu->surface == surface)
            return menu;
    }
    return nullptr;
}

/* ---- interaction -------------------------------------------------------------- */

static void arm_timer(xd_shell *shell, xd_menu *menu) {
    shell->pending_menu = menu;
    if (shell->menu_timer_fd < 0)
        return;
    struct itimerspec spec = {.it_value = {.tv_nsec = XD_MENU_DELAY_MS * 1000000L}};
    timerfd_settime(shell->menu_timer_fd, 0, &spec, nullptr);
}

void xd_menu_timer_fired(xd_shell *shell) {
    if (shell->pending_start7)
        xd_start7_timer_fired(shell);
    xd_menu *menu = shell->pending_menu;
    shell->pending_menu = nullptr;
    if (menu == nullptr || (menu->child != nullptr && menu->child->context))
        return;
    /* Settle on whatever the pointer rests on: open its submenu, or close
     * a submenu the pointer has left. */
    if (menu->hot >= 0 && menu->model->items[menu->hot].kind == XD_MENU_SUBMENU)
        xd_menu_open_child(menu, menu->hot);
    else if (menu->child != nullptr && menu->hot >= 0)
        menu_destroy(menu->child);
}

static void set_hot(xd_menu *menu, int hot) {
    if (menu->hot == hot)
        return;
    menu->hot = hot;
    menu->dirty = true;
    if (hot >= 0 && hot != menu->child_index)
        arm_timer(menu->shell, menu);
}

void xd_menu_pointer_motion(xd_menu *menu, double x, double y) {
    menu->shell->pointer_menu = menu;
    menu->ptr_x = x;
    menu->ptr_y = y;
    /* A program's right-click menu is up: this one waits for it. */
    if (menu->child != nullptr && menu->child->context)
        return;
    /* A program pressed and pulled: dragged (to Quick Launch, the desktop). */
    if (menu->pressed >= 0 && menu->pressed == menu->hot &&
        menu->model->items[menu->pressed].app != nullptr &&
        fabs(x - menu->press_x) + fabs(y - menu->press_y) > XD_MENU_DRAG_PX) {
        const xd_app *app = menu->model->items[menu->pressed].app;
        menu->pressed = -1;
        if (xd_dnd_start_app_drag(menu->shell, app, menu->surface, menu->scale))
            return;
    }
    /* A jump list's pinned item pulled over another: moved there, as
     * Windows 7 reorders its Pinned group. */
    if (menu->pressed >= 0 && menu->model->items[menu->pressed].move != nullptr) {
        menu->moving = menu->moving ||
                       fabs(x - menu->press_x) + fabs(y - menu->press_y) > XD_MENU_DRAG_PX;
        if (menu->moving) {
            const xd_menu_item *from = &menu->model->items[menu->pressed];
            int onto = item_at(menu, x, y);
            if (onto >= 0 && onto != menu->pressed && menu->model->items[onto].move == from->move) {
                from->move(menu->shell, from->action_data,
                           menu->model->items[onto].action_data); /* replaces the model */
                menu->pressed = menu->hot = onto;
                menu->moving = true;
                menu->hot_pin = false;
                menu->dirty = true;
            }
            return;
        }
    }
    /* Moving into a submenu keeps its parent item highlighted. */
    if (menu->parent != nullptr && menu->parent->hot != menu->parent->child_index) {
        menu->parent->hot = menu->parent->child_index;
        menu->parent->dirty = true;
    }
    set_hot(menu, item_at(menu, x, y));
    bool pin = on_pin(menu, menu->hot, x, y);
    if (pin != menu->hot_pin) {
        menu->hot_pin = pin;
        menu->dirty = true;
    }
}

void xd_menu_pointer_leave(xd_menu *menu) {
    if (menu->shell->pointer_menu == menu)
        menu->shell->pointer_menu = nullptr;
    if (menu->child != nullptr && menu->child->context)
        return;
    /* Keep the item whose submenu is open highlighted. */
    int keep = menu->child != nullptr ? menu->child_index : -1;
    if (menu->hot != keep) {
        menu->hot = keep;
        menu->dirty = true;
    }
}

static void activate(xd_menu *menu, int index) {
    const xd_menu_item *it = &menu->model->items[index];
    if (it->disabled)
        return;
    if (it->kind == XD_MENU_SUBMENU) {
        xd_menu_open_child(menu, index);
        return;
    }
    xd_sound_play_event(XD_SOUND_MENU_COMMAND);
    if (it->kind == XD_MENU_ACTION && it->action != nullptr) {
        void (*fn)(xd_shell *, void *) = it->action;
        void *data = it->action_data;
        xd_shell *shell = menu->shell;
        /* A right-click menu's data is in its own tree: kept until the
         * action has run. */
        xd_menu_tree *own = menu->own_tree;
        menu->own_tree = nullptr;
        if (it->keep_open && menu->parent != nullptr)
            menu_destroy(menu); /* frees `it` */
        else
            xd_menu_close_all(shell); /* frees `it` */
        fn(shell, data);
        if (own != nullptr) {
            xd_menu_tree_free(own);
            xd_free(own);
        }
    } else if (it->kind == XD_MENU_ACTION && it->app != nullptr) {
        const xd_app *app = it->app; /* owned by shell->apps, outlives the menu */
        xd_menu_close_all(menu->shell);   /* frees `it` */
        if (xd_app_launch(app))
            xd_start7_count_launch(app->id);
    } else if (it->kind == XD_MENU_ACTION && it->command != nullptr) {
        xd_shell *shell = menu->shell;
        char *cmd = xd_str_dup_cstr(xd_str_from_cstr(it->command));
        xd_menu_close_all(shell); /* frees `it` */
        xd_shell_launch(shell, cmd);
        xd_free(cmd);
    }
}

/* A program's right-click menu: what its own name does, and pinning it
 * (XP's All Programs, the XP and Windows 7 menus' Control Panel). The
 * actions run after the menus close; the app belongs to shell->apps. */
static void ctx_open([[maybe_unused]] xd_shell *shell, void *data) {
    const xd_app *app = data;
    if (xd_app_launch(app))
        xd_start7_count_launch(app->id);
}
static void ctx_pin(xd_shell *shell, void *data) {
    xd_start7_set_pinned(shell, ((const xd_app *)data)->id, true);
}
static void ctx_unpin(xd_shell *shell, void *data) {
    xd_start7_set_pinned(shell, ((const xd_app *)data)->id, false);
}

/* Send To and Properties act on the program's desktop entry, through the
 * file manager as its own commands do: a copy on the desktop is Linux's
 * shortcut to a program, as dropping one there makes. */
static const char *program_file(const xd_app *app) {
    return app->info != nullptr ? g_desktop_app_info_get_filename(app->info) : nullptr;
}

static void run_files(xd_shell *shell, const char *option, const char *path) {
    char *exe = xd_shell_own_program("xd-files");
    char *qexe = g_shell_quote(exe);
    char *qopt = g_shell_quote(option);
    char *qpath = g_shell_quote(path);
    char *cmd = g_strdup_printf("%s %s %s", qexe, qopt, qpath);
    xd_shell_launch(shell, cmd);
    g_free(cmd);
    g_free(qpath);
    g_free(qopt);
    g_free(qexe);
    g_free(exe);
}

/* Desktop (create shortcut) links to the program's launcher, as the
 * Files' Send To does (a copy wouldn't be trusted to run); My Documents
 * gets a copy. */
static void send_to(xd_shell *shell, const xd_app *app, GUserDirectory which,
                    const char *fallback) {
    const char *file = program_file(app);
    if (file == nullptr)
        return;
    const char *dir = g_get_user_special_dir(which);
    char *own = dir == nullptr ? g_build_filename(g_get_home_dir(), fallback, nullptr) : nullptr;
    const char *job = which == G_USER_DIRECTORY_DESKTOP ? "--link-to=" : "--copy-to=";
    char *option = g_strconcat(job, dir != nullptr ? dir : own, nullptr);
    run_files(shell, option, file);
    g_free(option);
    g_free(own);
}

static void ctx_send_desktop(xd_shell *shell, void *data) {
    send_to(shell, data, G_USER_DIRECTORY_DESKTOP, "Desktop");
}
static void ctx_send_documents(xd_shell *shell, void *data) {
    send_to(shell, data, G_USER_DIRECTORY_DOCUMENTS, "Documents");
}
static void ctx_properties(xd_shell *shell, void *data) {
    const char *file = program_file(data);
    if (file != nullptr)
        run_files(shell, "--properties", file);
}

void xd_menu_add_program_items(xd_menu_tree *tree, xd_menu_model *model, const xd_app *app) {
    void *data = (void *)(uintptr_t)app;
    bool file = program_file(app) != nullptr;
    xd_menu_add(tree, model, XD_MENU_SEPARATOR, nullptr, nullptr);
    xd_menu_item *send = xd_menu_add_keyed(tree, model, XD_MENU_SUBMENU, "Se&nd To", nullptr);
    send->submenu = xd_menu_model_new(tree);
    send->disabled = !file;
    xd_menu_item *it = xd_menu_add(tree, send->submenu, XD_MENU_ACTION,
                                   "Desktop (create shortcut)", "user-desktop");
    it->action = ctx_send_desktop;
    it->action_data = data;
    it = xd_menu_add(tree, send->submenu, XD_MENU_ACTION, "My Documents", "folder-documents");
    it->action = ctx_send_documents;
    it->action_data = data;
    xd_menu_add(tree, model, XD_MENU_SEPARATOR, nullptr, nullptr);
    it = xd_menu_add_keyed(tree, model, XD_MENU_ACTION, "P&roperties", nullptr);
    it->action = ctx_properties;
    it->action_data = data;
    it->disabled = !file;
}

/* Hybrid graphics: run on another card than the program's own, once or
 * always (gpu.h). One action per card, as the data is the app. */
static void run_on(const xd_app *app, int gpu) {
    if (xd_app_launch_gpu(app, gpu))
        xd_start7_count_launch(app->id);
}
static void ctx_gpu0([[maybe_unused]] xd_shell *shell, void *data) { run_on(data, 0); }
static void ctx_gpu1([[maybe_unused]] xd_shell *shell, void *data) { run_on(data, 1); }
static void ctx_gpu2([[maybe_unused]] xd_shell *shell, void *data) { run_on(data, 2); }
static void ctx_gpu3([[maybe_unused]] xd_shell *shell, void *data) { run_on(data, 3); }
static void (*const ctx_gpu[])(xd_shell *, void *) = {ctx_gpu0, ctx_gpu1, ctx_gpu2, ctx_gpu3};

/* "Always run with dedicated graphics card" ticked or unticked: what the
 * user chose, kept only where it differs from the desktop entry's own. */
static void ctx_gpu_always([[maybe_unused]] xd_shell *shell, void *data) {
    GDesktopAppInfo *info = ((const xd_app *)data)->info;
    bool other = !xd_gpu_app_uses_other(info);
    bool prefers = g_desktop_app_info_get_boolean(info, "PrefersNonDefaultGPU");
    char *key = xd_gpu_app_key(info);
    xd_gpu_pref_set(nullptr, key, other == prefers ? XD_GPU_PREF_AUTO
                         : other         ? XD_GPU_PREF_OTHER
                                         : XD_GPU_PREF_DEFAULT);
    g_free(key);
}

void xd_menu_add_gpu_items(xd_menu_tree *tree, xd_menu_model *model, const xd_app *app) {
    const xd_gpu_list *gpus = xd_gpu_current();
    if (gpus->count < 2 || app->info == nullptr)
        return;
    void *data = (void *)(uintptr_t)app;
    bool other = xd_gpu_app_uses_other(app->info);
    /* GNOME's wording; with several other cards, each by its name. */
    size_t others = 0;
    for (size_t i = 0; i < gpus->count; i++)
        others += gpus->items[i].is_default ? 0 : 1;
    for (size_t i = 0; i < gpus->count && i < XD_ARRAY_LEN(ctx_gpu); i++) {
        const xd_gpu *gpu = &gpus->items[i];
        /* Offered: the cards it doesn't start on by itself. */
        if (gpu->is_default != other)
            continue;
        char *label = gpu->is_default ? g_strdup("Run with integrated graphics card")
                      : others == 1   ? g_strdup("Run with dedicated graphics card")
                                      : g_strdup_printf("Run with %s", gpu->name);
        xd_menu_item *it = xd_menu_add(tree, model, XD_MENU_ACTION, label, nullptr);
        it->action = ctx_gpu[i];
        it->action_data = data;
        g_free(label);
        if (gpu->is_default)
            break; /* one default card */
    }
    xd_menu_item *it = xd_menu_add(tree, model, XD_MENU_ACTION,
                                   "Always run with dedicated graphics card", nullptr);
    it->checked = other;
    it->action = ctx_gpu_always;
    it->action_data = data;
}

static xd_menu_model *app_context(xd_shell *shell, xd_menu_tree *tree, const xd_app *app) {
    xd_menu_model *mm = xd_menu_model_new(tree);
    void *data = (void *)(uintptr_t)app;
    xd_menu_item *it = xd_menu_add_keyed(tree, mm, XD_MENU_ACTION, "&Open", nullptr);
    it->bold = true;
    it->action = ctx_open;
    it->action_data = data;
    xd_menu_add_gpu_items(tree, mm, app);
    xd_menu_add(tree, mm, XD_MENU_SEPARATOR, nullptr, nullptr);
    bool full = false;
    bool pinned = xd_start7_pinned(shell, app->id, &full);
    it = xd_menu_add(tree, mm, XD_MENU_ACTION,
                     pinned ? "Unpin from Start menu" : "Pin to Start menu", nullptr);
    it->action = pinned ? ctx_unpin : ctx_pin;
    it->action_data = data;
    it->disabled = !pinned && full;
    xd_menu_add_program_items(tree, mm, app);
    return mm;
}

static void open_context(xd_menu *menu, int index) {
    const xd_menu_item *item = &menu->model->items[index];
    xd_shell *shell = menu->shell;
    if (!menu->shown)
        return;
    if (menu->child != nullptr)
        menu_destroy(menu->child);
    xd_menu_tree *tree = XD_NEW(xd_menu_tree);
    xd_menu_tree_init(tree);
    xd_menu_model *mm = item->context != nullptr ? item->context(shell, tree, item->action_data)
                                                 : app_context(shell, tree, item->app);
    if (mm == nullptr || mm->count == 0) {
        xd_menu_tree_free(tree);
        xd_free(tree);
        return;
    }
    xd_menu *child = menu_create(shell, mm);
    child->parent = menu;
    child->context = true;
    child->own_tree = tree;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, child->width, child->height);
    xdg_positioner_set_anchor_rect(pos, (int)menu->ptr_x, (int)menu->ptr_y, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(
        pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    child->popup = xdg_surface_get_popup(child->xdg_surface, menu->xdg_surface, pos);
    xdg_positioner_destroy(pos);
    menu_finish_popup(child, shell->menu_serial);
    menu->child = child;
    menu->child_index = index;
    menu->hot = index;
    menu->dirty = true;
}

void xd_menu_pointer_button(xd_menu *menu, uint32_t button, bool pressed) {
    if (button != BTN_LEFT && button != BTN_RIGHT)
        return;
    /* A click beside a program's right-click menu closes just that. */
    if (menu->child != nullptr && menu->child->context) {
        if (pressed)
            menu_destroy(menu->child);
        menu->pressed = -1;
        return;
    }
    /* A jump list item dragged to its new place: let go, nothing runs. */
    if (menu->moving) {
        if (!pressed) {
            menu->moving = false;
            menu->pressed = -1;
            menu->dirty = true;
        }
        return;
    }
    int index = menu->hot;
    if (index < 0)
        return;
    const xd_menu_item *it = &menu->model->items[index];
    int was_pressed = menu->pressed;
    menu->pressed = pressed && button == BTN_LEFT ? index : -1;
    if (pressed) {
        menu->press_x = menu->ptr_x;
        menu->press_y = menu->ptr_y;
    }
    /* A program's own menu, on release as in Windows; in the XP and
     * Windows 7 menus only, which have a pinned list. A jump list item's
     * own, wherever it is. */
    if (button == BTN_RIGHT && it->kind == XD_MENU_ACTION &&
        (it->context != nullptr || (it->app != nullptr && menu->shell->start7 != nullptr))) {
        if (!pressed && !it->disabled)
            open_context(menu, index);
        return;
    }
    /* A jump list item's pin button: pinned or unpinned, and the list
     * stays open, laid out again. */
    if (menu->hot_pin && it->pin != nullptr && button == BTN_LEFT) {
        menu->dirty = true;
        if (!pressed && was_pressed == index) {
            void (*fn)(xd_shell *, void *) = it->pin;
            fn(menu->shell, it->action_data); /* may replace the model and `it` */
        }
        return;
    }
    /* A click on a submenu with its own command (Windows 2000's Control
     * Panel) runs it: pressed and released here, not dragged onto. */
    if (!pressed && button == BTN_LEFT && was_pressed == index && it->kind == XD_MENU_SUBMENU &&
        it->command != nullptr && !it->disabled) {
        xd_shell *shell = menu->shell;
        char *cmd = xd_str_dup_cstr(xd_str_from_cstr(it->command));
        xd_menu_close_all(shell); /* frees `it` */
        xd_shell_launch(shell, cmd);
        xd_free(cmd);
        return;
    }
    /* Submenus open on press; commands run on release, so a press-drag-
     * release from the Start button works as in Windows. */
    if (pressed ? it->kind == XD_MENU_SUBMENU : it->kind == XD_MENU_ACTION)
        activate(menu, index);
}

static int step(const xd_menu *menu, int from, int dir) {
    int count = (int)menu->model->count;
    if (count == 0)
        return -1;
    int i = from;
    for (int tries = 0; tries < count; tries++) {
        i = i < 0 ? (dir > 0 ? 0 : count - 1) : (i + dir + count) % count;
        if (menu->model->items[i].kind != XD_MENU_SEPARATOR &&
            menu->model->items[i].kind != XD_MENU_HEADER)
            return i;
    }
    return -1;
}

/* The letter that picks an item: its mnemonic, else its label's first. */
static uint32_t item_letter(const xd_menu_item *it) {
    if (it->mnemonic != 0)
        return it->mnemonic;
    gunichar letter = g_utf8_get_char_validated(it->label, -1);
    return letter == (gunichar)-1 || letter == (gunichar)-2 ? 0 : g_unichar_tolower(letter);
}

/* Typing a letter: the items it picks (mnemonics first, as Windows' own
 * menus do when some items have one), counted in *count; returns the next
 * after the highlighted one, round from the top. */
static int letter_next(const xd_menu *menu, uint32_t ch, int *count) {
    const xd_menu_model *menu_model = menu->model;
    bool explicit = false;
    for (size_t i = 0; i < menu_model->count; i++)
        explicit =
            explicit || (menu_model->items[i].mnemonic == ch && !menu_model->items[i].disabled);
    *count = 0;
    int first = -1, next = -1;
    for (size_t i = 0; i < menu_model->count; i++) {
        const xd_menu_item *it = &menu_model->items[i];
        if (it->kind == XD_MENU_SEPARATOR || it->kind == XD_MENU_HEADER || it->disabled ||
            (explicit ? it->mnemonic != ch : item_letter(it) != ch))
            continue;
        (*count)++;
        if (first < 0)
            first = (int)i;
        if (next < 0 && (int)i > menu->hot)
            next = (int)i;
    }
    return next >= 0 ? next : first;
}

bool xd_menu_key(xd_shell *shell, uint32_t sym) {
    return shell->start_menu != nullptr && xd_menu_key_chain(shell->start_menu, sym);
}

bool xd_menu_key_chain(xd_menu *root, uint32_t sym) {
    xd_shell *shell = root->shell;
    xd_menu *menu = root;
    while (menu->child != nullptr)
        menu = menu->child;

    switch (sym) {
    case XKB_KEY_Down:
        menu->hot = step(menu, menu->hot, +1);
        menu->dirty = true;
        return true;
    case XKB_KEY_Up:
        menu->hot = step(menu, menu->hot, -1);
        menu->dirty = true;
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        if (menu->hot >= 0) {
            bool opens = menu->model->items[menu->hot].kind == XD_MENU_SUBMENU;
            int hot = menu->hot;
            if (opens || sym != XKB_KEY_Right) {
                activate(menu, hot);
                /* Keyboard-opened submenus start on their first item. */
                if (opens && menu->child != nullptr) {
                    menu->child->hot = step(menu->child, -1, +1);
                    menu->child->dirty = true;
                }
            }
        }
        return true;
    case XKB_KEY_Left:
        if (menu->parent != nullptr)
            menu_destroy(menu);
        else if (menu != shell->start_menu)
            return false; /* an attached submenu: its owner closes it */
        return true;
    case XKB_KEY_Escape:
        if (menu->parent != nullptr)
            menu_destroy(menu);
        else if (menu != shell->start_menu)
            return false;
        else
            xd_menu_close_all(shell);
        return true;
    default:
        break;
    }
    /* A letter: the one item it picks runs (or opens); several take turns
     * being highlighted, as in Windows. */
    uint32_t ch = xkb_keysym_to_utf32(sym);
    if (ch <= 0x20 || ch == 0x7F)
        return false;
    int count = 0;
    int next = letter_next(menu, g_unichar_tolower(ch), &count);
    if (count > 0) {
        menu->hot = next;
        menu->dirty = true;
        if (count == 1)
            return xd_menu_key_chain(root, XKB_KEY_Return);
    }
    return true;
}
