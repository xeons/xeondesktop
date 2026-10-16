/* SPDX-License-Identifier: MIT */
#include <linux/input-event-codes.h>
#include <math.h>
#include <sys/timerfd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../ui/icons.h"
#include "menu.h"
#include "shell.h"

/* Windows' default MenuShowDelay. */
enum { XD_MENU_DELAY_MS = 400, XD_MENU_MIN_W = 120 };

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

xd_menu_model *xd_menu_model_new(xd_menu_tree *tree) {
    xd_menu_model *m = XD_ARENA_NEW(&tree->arena, xd_menu_model);
    if (tree->count == tree->cap) {
        tree->cap = tree->cap == 0 ? 16 : tree->cap * 2;
        tree->models = xd_realloc_array(tree->models, tree->cap, sizeof(*tree->models));
    }
    tree->models[tree->count++] = m;
    return m;
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

/* ---- layout -------------------------------------------------------------------- */

/* Sizes the menu in logical units from a scale-1 theme. */
static void layout(xd_menu *menu) {
    const xd_menu_model *m = menu->model;
    xd_theme t;
    xd_theme_init(&t, menu->shell->style, &menu->shell->scheme, 1.0);
    xd_menu_metrics mm = xd_theme_menu_metrics(&t);

    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(scratch);
    int text_w = 0;
    for (size_t i = 0; i < m->count; i++) {
        const xd_menu_item *it = &m->items[i];
        if (it->kind == XD_MENU_SEPARATOR)
            continue;
        int w = it->bold ? xd_theme_menu_bold_text_width(&t, cr, it->label)
                         : xd_theme_menu_text_width(&t, cr, it->label);
        if (it->accel != nullptr)
            w += xd_theme_menu_text_width(&t, cr, it->accel) + 24;
        text_w = XD_MAX(text_w, w);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);

    int banner = m->banner_product != nullptr ? mm.banner_w : 0;
    int text_x = m->large ? mm.large_text_x : mm.text_x;
    /* Slack so hinting at other scales doesn't ellipsize labels. */
    int item_w = XD_MAX(text_x + text_w + mm.arrow_w + 12, XD_MENU_MIN_W);

    menu->item_y = xd_alloc_array(XD_MAX(m->count, 1), sizeof(int));
    menu->item_h = xd_alloc_array(XD_MAX(m->count, 1), sizeof(int));
    int y = mm.border;
    for (size_t i = 0; i < m->count; i++) {
        int h = m->items[i].kind == XD_MENU_SEPARATOR ? mm.separator_h
                : m->large                            ? mm.large_item_h
                                                      : mm.item_h;
        menu->item_y[i] = y;
        menu->item_h[i] = h;
        y += h;
    }
    menu->item_x = mm.border + banner;
    menu->item_w = item_w;
    menu->width = mm.border * 2 + banner + item_w;
    menu->height = y + mm.border;
}

static int item_at(const xd_menu *menu, double x, double y) {
    if (x < menu->item_x || x >= menu->item_x + menu->item_w)
        return -1;
    for (size_t i = 0; i < menu->model->count; i++) {
        if (y >= menu->item_y[i] && y < menu->item_y[i] + menu->item_h[i])
            return menu->model->items[i].kind == XD_MENU_SEPARATOR ? -1 : (int)i;
    }
    return -1;
}

/* ---- rendering ---------------------------------------------------------------- */

static int dev(double logical, double scale) {
    double v = round(logical * scale);
    XD_ASSERT(v > -65536.0 && v < 65536.0);
    return (int)v;
}

/* Logical rect -> device rect with shared edges, so adjacent items tile
 * without gaps at fractional scales. */
static xd_rect to_device(int x, int y, int w, int h, double s) {
    int x0 = dev(x, s);
    int y0 = dev(y, s);
    return (xd_rect){x0, y0, dev(x + w, s) - x0, dev(y + h, s) - y0};
}

static void draw_icon(xd_shell *shell, cairo_t *cr, const xd_menu_item *it, xd_rect r,
                      bool disabled) {
    /* Items without any icon (taskbar and window menus) leave the column
     * empty, or to their check mark or glyph. */
    if ((it->icon == nullptr && it->fallback_icon == nullptr && it->app == nullptr) ||
        it->checked || it->glyph != XD_MENU_GLYPH_NONE)
        return;
    const char *names[] = {it->icon, it->fallback_icon, "application-x-executable"};
    cairo_surface_t *icon = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names), r.w);
    if (icon == nullptr)
        return;
    cairo_save(cr);
    cairo_set_source_surface(cr, icon, r.x, r.y);
    cairo_paint_with_alpha(cr, disabled ? 0.45 : 1.0);
    cairo_restore(cr);
}

static void render(xd_menu *menu) {
    xd_shell *shell = menu->shell;
    double s = menu->scale;
    int dw = dev(menu->width, s);
    int dh = dev(menu->height, s);
    if (dw <= 0 || dh <= 0)
        return;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&menu->ring, shell->shm, dw, dh);
    if (buf == nullptr)
        return;

    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, s);
    xd_menu_metrics mm = xd_theme_menu_metrics(&t);
    const xd_menu_model *m = menu->model;
    cairo_t *cr = cairo_create(buf->surface);
    xd_theme_draw_menu_frame(&t, cr, (xd_rect){0, 0, dw, dh});

    if (m->banner_product != nullptr) {
        xd_theme t1;
        xd_theme_init(&t1, shell->style, &shell->scheme, 1.0);
        xd_menu_metrics m1 = xd_theme_menu_metrics(&t1);
        xd_rect banner = to_device(m1.border, m1.border, m1.banner_w,
                                   menu->height - 2 * m1.border, s);
        xd_theme_draw_start_banner(&t, cr, banner, m->banner_product,
                                   m->banner_edition != nullptr ? m->banner_edition : "");
    }

    for (size_t i = 0; i < m->count; i++) {
        const xd_menu_item *it = &m->items[i];
        xd_rect r = to_device(menu->item_x, menu->item_y[i], menu->item_w, menu->item_h[i], s);
        if (it->kind == XD_MENU_SEPARATOR) {
            xd_theme_draw_menu_separator(&t, cr, r);
            continue;
        }
        uint8_t state = 0;
        if (menu->hot == (int)i || menu->child_index == (int)i)
            state |= XD_STATE_HOT;
        if (it->disabled)
            state |= XD_STATE_DISABLED;
        xd_menu_item_extras extras = {
            .bold = it->bold, .checked = it->checked, .glyph = it->glyph, .accel = it->accel};
        xd_theme_draw_menu_item_ex(&t, cr, r, state, it->label, it->kind == XD_MENU_SUBMENU,
                                   m->large, &extras);
        int icon = m->large ? mm.large_icon : mm.icon;
        draw_icon(shell, cr, it, (xd_rect){r.x + mm.icon_x, r.y + (r.h - icon) / 2, icon, icon},
                  it->disabled);
    }
    cairo_destroy(cr);
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
    for (xd_menu *m = shell->start_menu; m != nullptr; m = m->child) {
        if (m->dirty && m->configured)
            render(m);
    }
    for (xd_menu *m = xd_start7_submenu(shell); m != nullptr; m = m->child) {
        if (m->dirty && m->configured)
            render(m);
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

static void popup_configure([[maybe_unused]] void *data, [[maybe_unused]] struct xdg_popup *p,
                            [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y,
                            [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}

static void popup_done(void *data, [[maybe_unused]] struct xdg_popup *p) {
    /* The compositor dismissed the menu (click outside). */
    xd_menu *menu = data;
    xd_menu_close_all(menu->shell);
}

static void popup_repositioned([[maybe_unused]] void *data,
                               [[maybe_unused]] struct xdg_popup *p,
                               [[maybe_unused]] uint32_t token) {}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static void preferred_scale(void *data, [[maybe_unused]] struct wp_fractional_scale_v1 *f,
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
    menu->child_index = -1;
    menu->scale = 1.0;
    layout(menu);

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
    xd_theme t;
    xd_theme_init(&t, shell->style, &shell->scheme, 1.0);
    int border = xd_theme_menu_metrics(&t).border;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(shell->wm_base);
    xdg_positioner_set_size(pos, child->width, child->height);
    xdg_positioner_set_anchor_rect(pos, menu->item_x, menu->item_y[index], menu->item_w,
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
    xd_free(menu->item_y);
    xd_free(menu->item_h);
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
    for (xd_menu *m = shell->start_menu; m != nullptr; m = m->child) {
        if (m->surface == surface)
            return m;
    }
    for (xd_menu *m = xd_start7_submenu(shell); m != nullptr; m = m->child) {
        if (m->surface == surface)
            return m;
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
    if (menu == nullptr)
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
    /* Moving into a submenu keeps its parent item highlighted. */
    if (menu->parent != nullptr && menu->parent->hot != menu->parent->child_index) {
        menu->parent->hot = menu->parent->child_index;
        menu->parent->dirty = true;
    }
    set_hot(menu, item_at(menu, x, y));
}

void xd_menu_pointer_leave(xd_menu *menu) {
    if (menu->shell->pointer_menu == menu)
        menu->shell->pointer_menu = nullptr;
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
    if (it->kind == XD_MENU_ACTION && it->action != nullptr) {
        void (*fn)(xd_shell *, void *) = it->action;
        void *data = it->action_data;
        xd_shell *shell = menu->shell;
        xd_menu_close_all(shell); /* frees `it` */
        fn(shell, data);
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

void xd_menu_pointer_button(xd_menu *menu, uint32_t button, bool pressed) {
    if (button != BTN_LEFT && button != BTN_RIGHT)
        return;
    int index = menu->hot;
    if (index < 0)
        return;
    const xd_menu_item *it = &menu->model->items[index];
    /* Submenus open on press; commands run on release, so a press-drag-
     * release from the Start button works as in Windows. */
    if (pressed ? it->kind == XD_MENU_SUBMENU : it->kind == XD_MENU_ACTION)
        activate(menu, index);
}

static int step(const xd_menu *menu, int from, int dir) {
    int n = (int)menu->model->count;
    if (n == 0)
        return -1;
    int i = from;
    for (int tries = 0; tries < n; tries++) {
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (menu->model->items[i].kind != XD_MENU_SEPARATOR)
            return i;
    }
    return -1;
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
        return false;
    }
}
