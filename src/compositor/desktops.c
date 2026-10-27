/* SPDX-License-Identifier: MIT
 * Virtual desktops, as Compiz's viewports: General Options' Desktop Size
 * sets how many there are across and down (one by default, as Windows
 * has). Each window is on one desktop, or on every one (sticky: "Always
 * on Visible Workspace"); the shell's taskbar, desktop and other layer
 * surfaces always show. Switching hides the other desktops' windows and
 * shows this one's, and takes away their taskbar buttons (their
 * foreign-toplevel handles), so the shell's taskbar, thumbnails and
 * Alt+Tab see just this desktop's windows. Desktop Wall and Desktop Cube
 * animate a switch; without them it's at once.
 */
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "desktops_grid.h"
#include "server.h"

enum { MAX_ANIMATORS = 4 };

typedef struct desktops_animator {
    xd_desktops_animate_fn animate;
    void *data;
    int priority;
} desktops_animator;

struct xd_desktops {
    xd_server *server;
    xd_desktop_grid grid;
    int current;
    uint64_t opened; /* windows opened so far: the next one's open_order */
    desktops_animator animators[MAX_ANIMATORS];
    size_t animator_count;
};

void xd_desktops_init(xd_server *server) {
    xd_desktops *desktops = XD_NEW(xd_desktops);
    desktops->server = server;
    desktops->grid = xd_desktop_grid_make(1, 1, false);
    server->desktops = desktops;
}

void xd_desktops_finish(xd_server *server) {
    xd_free(server->desktops);
    server->desktops = nullptr;
}

int xd_desktops_get_count(const xd_server *server) {
    return server->desktops != nullptr ? xd_desktop_grid_get_count(&server->desktops->grid) : 1;
}

int xd_desktops_get_current(const xd_server *server) {
    return server->desktops != nullptr ? server->desktops->current : 0;
}

const xd_desktop_grid *xd_desktops_get_grid(const xd_server *server) {
    return &server->desktops->grid;
}

bool xd_desktops_is_view_shown(const xd_view *view) {
    return xd_desktop_grid_is_shown(view->desktop, view->sticky,
                                    xd_desktops_get_current(view->server));
}

void xd_desktops_view_mapped(xd_view *view) {
    xd_desktops *desktops = view->server->desktops;
    if (desktops == nullptr)
        return;
    view->open_order = ++desktops->opened;
    /* A dialog opens with its owner, wherever that is. */
    const xd_view *owner = xd_view_get_owner(view);
    view->desktop = owner != nullptr ? owner->desktop : desktops->current;
    view->sticky = owner != nullptr && owner->sticky;
}

/* The windows showing, by when they were opened: taskbar buttons made
 * again come in the order the windows opened, as they were. */
static int desktops_compare_opened(const void *left, const void *right) {
    const xd_view *left_view = *(xd_view *const *)left;
    const xd_view *right_view = *(xd_view *const *)right;
    return (left_view->open_order > right_view->open_order) -
           (left_view->open_order < right_view->open_order);
}

void xd_desktops_apply(xd_server *server) {
    size_t count = 0;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        count++;
    }
    xd_view **order = XD_NEW_ARRAY(xd_view *, XD_MAX(count, (size_t)1));
    size_t ordered = 0;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        if (!view->mapped)
            continue;
        bool shown = xd_desktops_is_view_shown(view);
        wlr_scene_node_set_enabled(&view->tree->node, shown && !view->minimized);
        wlr_scene_node_set_position(&view->tree->node, view->x, view->y);
        order[ordered++] = view;
    }
    qsort(order, ordered, sizeof(*order), desktops_compare_opened);
    /* Buttons going first, then those coming, oldest first. */
    for (size_t i = 0; i < ordered; i++) {
        if (!xd_desktops_is_view_shown(order[i]))
            xd_view_update_taskbar_button(order[i]);
    }
    for (size_t i = 0; i < ordered; i++) {
        if (xd_desktops_is_view_shown(order[i]))
            xd_view_update_taskbar_button(order[i]);
    }
    xd_free(order);
}

/* After a switch: the window in front stays active if it still shows,
 * else the one used last here gets the focus (or nothing has it). */
static void desktops_refocus(xd_server *server, xd_view *before) {
    if (before != nullptr && before->mapped && xd_desktops_is_view_shown(before) &&
        !before->minimized) {
        if (xd_view_focused(server) != before)
            xd_view_focus(before);
        return;
    }
    /* The old front window is drawn inactive, as it would be on its own
     * desktop when we come back. */
    xd_view_deactivate_front(server);
    xd_view_focus_next(server);
}

static void desktops_animate(xd_desktops *desktops, int from, int to, xd_view *carried) {
    for (size_t i = 0; i < desktops->animator_count; i++) {
        const desktops_animator *animator = &desktops->animators[i];
        if (animator->animate(desktops->server, animator->data, from, to, carried))
            return;
    }
}

void xd_desktops_switch_to(xd_server *server, int desktop, xd_view *carried) {
    xd_desktops *desktops = server->desktops;
    if (desktops == nullptr || desktop < 0 ||
        desktop >= xd_desktop_grid_get_count(&desktops->grid) || desktop == desktops->current)
        return;
    xd_view *before = xd_view_focused(server);
    int from = desktops->current;
    if (carried != nullptr && !carried->sticky)
        xd_desktops_move_view(carried, desktop);
    desktops->current = desktop;
    wlr_log(WLR_DEBUG, "desktops: %d -> %d", from, desktop);
    xd_desktops_apply(server);
    desktops_refocus(server, carried != nullptr ? carried : before);
    xd_arrange_desktops_changed(server);
    desktops_animate(desktops, from, desktop, carried);
}

bool xd_desktops_switch_by(xd_server *server, int columns, int rows, xd_view *carried) {
    xd_desktops *desktops = server->desktops;
    if (desktops == nullptr)
        return false;
    int to = xd_desktop_grid_find_neighbour(&desktops->grid, desktops->current, columns, rows,
                                            false);
    if (to < 0 || to == desktops->current)
        return false;
    xd_desktops_switch_to(server, to, carried);
    return true;
}

void xd_desktops_move_view(xd_view *view, int desktop) {
    xd_server *server = view->server;
    if (desktop < 0 || desktop >= xd_desktops_get_count(server))
        return;
    bool was_focused = xd_view_focused(server) == view;
    view->desktop = desktop;
    view->sticky = false;
    /* Its dialogs come along (an owner's owner's too). */
    for (int depth = 0; depth < 4; depth++) {
        XD_LIST_FOR_EACH(other, &server->views, xd_view, link) {
            const xd_view *owner = xd_view_get_owner(other);
            if (owner != nullptr && (owner->desktop != other->desktop || other->sticky)) {
                other->desktop = owner->desktop;
                other->sticky = owner->sticky;
            }
        }
    }
    xd_desktops_apply(server);
    xd_arrange_view_desktop(view);
    if (was_focused && !xd_desktops_is_view_shown(view)) {
        xd_view_deactivate_front(server);
        xd_view_focus_next(server);
    }
}

void xd_desktops_set_sticky(xd_view *view, bool sticky) {
    xd_server *server = view->server;
    view->sticky = sticky;
    if (!sticky)
        view->desktop = xd_desktops_get_current(server);
    XD_LIST_FOR_EACH(other, &server->views, xd_view, link) {
        if (xd_view_get_owner(other) == view) {
            other->sticky = sticky;
            other->desktop = view->desktop;
        }
    }
    xd_desktops_apply(server);
    xd_arrange_view_desktop(view);
}

void xd_desktops_set_grid(xd_server *server, int columns, int rows, bool wrap) {
    xd_desktops *desktops = server->desktops;
    if (desktops == nullptr)
        return;
    xd_desktop_grid grid = xd_desktop_grid_make(columns, rows, wrap);
    bool same = grid.columns == desktops->grid.columns && grid.rows == desktops->grid.rows;
    desktops->grid.wrap = grid.wrap;
    if (same)
        return;
    /* Each window keeps its place in the grid where that's still there,
     * else goes to the nearest desktop left. */
    xd_desktop_grid old = desktops->grid;
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        int column = 0, row = 0;
        xd_desktop_grid_get_cell(&old, view->desktop, &column, &row);
        view->desktop = xd_desktop_grid_get_index(&grid, column, row);
    }
    int column = 0, row = 0;
    xd_desktop_grid_get_cell(&old, desktops->current, &column, &row);
    desktops->grid = grid;
    desktops->current = xd_desktop_grid_get_index(&grid, column, row);
    wlr_log(WLR_INFO, "desktops: %d x %d", grid.columns, grid.rows);
    xd_view *before = xd_view_focused(server);
    xd_desktops_apply(server);
    if (before == nullptr || !xd_desktops_is_view_shown(before))
        desktops_refocus(server, before);
    xd_arrange_desktops_changed(server);
}

void xd_desktops_add_animator(xd_server *server, int priority, xd_desktops_animate_fn animate,
                              void *data) {
    xd_desktops *desktops = server->desktops;
    if (desktops == nullptr || desktops->animator_count == MAX_ANIMATORS)
        return;
    /* Kept highest first. */
    size_t at = 0;
    while (at < desktops->animator_count && desktops->animators[at].priority >= priority)
        at++;
    memmove(&desktops->animators[at + 1], &desktops->animators[at],
            (desktops->animator_count - at) * sizeof(desktops->animators[0]));
    desktops->animators[at] = (desktops_animator){animate, data, priority};
    desktops->animator_count++;
}

void xd_desktops_remove_animator(xd_server *server, xd_desktops_animate_fn animate, void *data) {
    xd_desktops *desktops = server->desktops;
    if (desktops == nullptr)
        return;
    for (size_t i = 0; i < desktops->animator_count; i++) {
        if (desktops->animators[i].animate == animate && desktops->animators[i].data == data) {
            memmove(&desktops->animators[i], &desktops->animators[i + 1],
                    (desktops->animator_count - i - 1) * sizeof(desktops->animators[0]));
            desktops->animator_count--;
            return;
        }
    }
}
