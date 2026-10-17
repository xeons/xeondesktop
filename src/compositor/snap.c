/* SPDX-License-Identifier: MIT
 * Windows 7's window handling, in the Aero style only (XP had none):
 *
 * - Aero Snap: moving a window to the top edge of the screen maximizes it,
 *   to the left or right edge fills that half; a glass outline previews
 *   where it goes. A snapped window gets its size back when it's dragged
 *   away. Win+Left/Right snap from the keyboard; pressed again they go on
 *   to the next monitor's far half, and on round to the window's own size.
 *   Win+Shift+Up makes it as tall as the screen, Win+Shift+Left/Right move
 *   it to the next monitor.
 * - A maximized window dragged by its caption is restored under the
 *   pointer and keeps moving.
 * - Aero Shake: shaking a window by its caption minimizes every other
 *   window; shaking it again brings them back.
 */
#include <math.h>
#include <time.h>

#include "../core/alloc.h"
#include "server.h"

static constexpr int EDGE = 2;             /* px from the screen edge that snaps */
static constexpr int DRAG_START = 4;       /* px before a maximized window lets go */
static constexpr int SHAKE_TRAVEL = 25;    /* px a swing must cover */
static constexpr int SHAKE_SWINGS = 4;     /* direction changes that make a shake */
static constexpr int64_t SHAKE_MS = 1000;  /* ... within this long */

static bool aero(const xd_server *server) { return server->config.style == XD_STYLE_AERO; }

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The area a snap fills: the work area, or a half of it. */
static struct wlr_box snap_box(xd_server *server, double x, double y, xd_snap side) {
    struct wlr_box a = xd_output_work_area_at(server, x, y);
    switch (side) {
    case XD_SNAP_LEFT:
        a.width /= 2;
        break;
    case XD_SNAP_RIGHT:
        a.x += a.width - a.width / 2;
        a.width -= a.width - a.width / 2;
        break;
    case XD_SNAP_MAXIMIZE:
    case XD_SNAP_TALL:
    case XD_SNAP_NONE:
        break;
    }
    return a;
}

/* ---- monitors ------------------------------------------------------------------------ */

static struct wlr_box layout_box(xd_server *server, xd_output *out) {
    struct wlr_box b;
    wlr_output_layout_get_box(server->output_layout, out->wlr_output, &b);
    return b;
}

/* The monitor a window is on: under its middle, else under the pointer. */
static xd_output *view_output(xd_view *view) {
    struct wlr_box b = xd_view_outer_box(view);
    xd_output *out = xd_output_at(view->server, b.x + b.width / 2.0, b.y + b.height / 2.0);
    return out != nullptr ? out
                          : xd_output_at(view->server, view->server->cursor->x,
                                         view->server->cursor->y);
}

/* The next monitor left (dir < 0) or right, in order of their middles,
 * round the end; `from` itself when it's the only one. */
static xd_output *next_output(xd_server *server, xd_output *from, int dir) {
    struct wlr_box f = layout_box(server, from);
    double fx = f.x + f.width / 2.0;
    xd_output *best = nullptr, *wrap = nullptr;
    double best_d = 0, wrap_d = 0;
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        struct wlr_box b = layout_box(server, out);
        if (out == from || wlr_box_empty(&b))
            continue;
        double d = (b.x + b.width / 2.0 - fx) * dir; /* > 0: that way */
        if (d > 0 && (best == nullptr || d < best_d)) {
            best = out;
            best_d = d;
        }
        if (d <= 0 && (wrap == nullptr || d < wrap_d)) {
            wrap = out; /* the farthest the other way */
            wrap_d = d;
        }
    }
    return best != nullptr ? best : wrap != nullptr ? wrap : from;
}

/* `b` moved from one monitor to the same place on another, inside it. */
static struct wlr_box carry_box(struct wlr_box b, struct wlr_box from, struct wlr_box to) {
    double rx = from.width > 0 ? (b.x + b.width / 2.0 - from.x) / from.width : 0.5;
    double ry = from.height > 0 ? (b.y + b.height / 2.0 - from.y) / from.height : 0.5;
    b.width = XD_MIN(b.width, to.width);
    b.height = XD_MIN(b.height, to.height);
    b.x = XD_CLAMP((int)(to.x + rx * to.width - b.width / 2.0), to.x, to.x + to.width - b.width);
    b.y = XD_CLAMP((int)(to.y + ry * to.height - b.height / 2.0), to.y,
                   to.y + to.height - b.height);
    return b;
}

static void show_preview(xd_server *server, xd_view *view, xd_snap target) {
    if (target == server->snap_target)
        return;
    server->snap_target = target;
    if (target == XD_SNAP_NONE) {
        if (server->snap_preview != nullptr)
            wlr_scene_node_set_enabled(&server->snap_preview->node, false);
        return;
    }
    struct wlr_box b = snap_box(server, server->cursor->x, server->cursor->y, target);
    if (server->snap_preview == nullptr) {
        /* Windows 7's outline: a light glass sheet over the windows. */
        static const float color[4] = {0.55f * 0.35f, 0.75f * 0.35f, 0.95f * 0.35f, 0.35f};
        server->snap_preview = wlr_scene_rect_create(server->layer_on_top, 1, 1, color);
    }
    /* Under the window being moved, over the others. */
    wlr_scene_node_reparent(&server->snap_preview->node, view->tree->node.parent);
    wlr_scene_node_place_below(&server->snap_preview->node, &view->tree->node);
    wlr_scene_node_set_position(&server->snap_preview->node, b.x, b.y);
    wlr_scene_rect_set_size(server->snap_preview, b.width, b.height);
    wlr_scene_node_set_enabled(&server->snap_preview->node, true);
}

/* Which edge the pointer is at, on the output it's on. */
static xd_snap edge_at(xd_server *server) {
    xd_output *out = xd_output_at(server, server->cursor->x, server->cursor->y);
    if (out == nullptr)
        return XD_SNAP_NONE;
    struct wlr_box o;
    wlr_output_layout_get_box(server->output_layout, out->wlr_output, &o);
    double x = server->cursor->x, y = server->cursor->y;
    if (y < o.y + EDGE)
        return XD_SNAP_MAXIMIZE;
    if (x < o.x + EDGE)
        return XD_SNAP_LEFT;
    if (x >= o.x + o.width - EDGE)
        return XD_SNAP_RIGHT;
    return XD_SNAP_NONE;
}

/* ---- Aero Shake ---------------------------------------------------------------------- */

static void shake(xd_server *server, xd_view *view) {
    if (server->shaken_n > 0) {
        /* Shaken again: the others come back. */
        for (size_t i = 0; i < server->shaken_n; i++) {
            xd_view *v = server->shaken[i];
            if (v != nullptr && v->mapped && v->minimized)
                xd_view_focus(v);
        }
        server->shaken_n = 0;
        xd_view_focus(view);
        return;
    }
    size_t n = (size_t)wl_list_length(&server->views);
    server->shaken = xd_realloc_array(server->shaken, XD_MAX(n, 1), sizeof(*server->shaken));
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (v != view && v->mapped && !v->minimized && server->shaken_n < n) {
            server->shaken[server->shaken_n++] = v;
        }
    }
    for (size_t i = 0; i < server->shaken_n; i++)
        xd_view_minimize(server->shaken[i]);
    xd_view_focus(view);
}

static void track_shake(xd_server *server, xd_view *view) {
    double x = server->cursor->x;
    int64_t t = now_ms();
    double moved = x - server->shake_anchor;
    int dir = moved > SHAKE_TRAVEL ? 1 : moved < -SHAKE_TRAVEL ? -1 : 0;
    if (dir == 0)
        return;
    if (t - server->shake_start > SHAKE_MS) {
        server->shake_start = t;
        server->shake_swings = 0;
    }
    if (dir != server->shake_dir) {
        server->shake_dir = dir;
        server->shake_swings++;
        if (server->shake_swings >= SHAKE_SWINGS) {
            server->shake_swings = 0;
            server->shake_start = t;
            shake(server, view);
        }
    }
    server->shake_anchor = x;
}

/* ---- the move grab ------------------------------------------------------------------- */

bool xd_snap_begin(xd_server *server, xd_view *view) {
    server->snap_target = XD_SNAP_NONE;
    server->snap_unmaximize = false;
    server->shake_anchor = server->cursor->x;
    server->shake_dir = 0;
    server->shake_swings = 0;
    server->shake_start = now_ms();
    if (!aero(server))
        return !view->maximized;
    /* A maximized window lets go once it's dragged a little. */
    server->snap_unmaximize = view->maximized;
    return true;
}

void xd_snap_motion(xd_server *server, xd_view *view) {
    if (!aero(server))
        return;
    double cx = server->cursor->x, cy = server->cursor->y;
    if (server->snap_unmaximize) {
        if (fabs(cx - server->grab_press_x) < DRAG_START &&
            fabs(cy - server->grab_press_y) < DRAG_START)
            return;
        server->snap_unmaximize = false;
        /* Restored under the pointer, at the same place along the caption. */
        struct wlr_box max = xd_view_outer_box(view);
        double along = max.width > 0 ? (server->grab_x) / max.width : 0.5;
        xd_view_set_maximized(view, false);
        struct wlr_box r = xd_view_outer_box(view);
        server->grab_x = r.width * along;
        xd_view_set_position(view, (int)(cx - server->grab_x), (int)(cy - server->grab_y));
    } else if (view->snapped != XD_SNAP_NONE) {
        /* Dragged out of a snap: its size comes back. */
        struct wlr_box cur = xd_view_outer_box(view);
        double along = cur.width > 0 ? server->grab_x / cur.width : 0.5;
        struct wlr_box r = view->snap_restore;
        view->snapped = XD_SNAP_NONE;
        server->grab_x = r.width * along;
        r.x = (int)(cx - server->grab_x);
        r.y = (int)(cy - server->grab_y);
        xd_view_set_outer_box(view, r);
    }
    show_preview(server, view, edge_at(server));
    track_shake(server, view);
}

static void snap_to(xd_view *view, xd_snap side, double x, double y) {
    xd_server *server = view->server;
    if (side == XD_SNAP_MAXIMIZE) {
        xd_view_set_maximized(view, true);
        return;
    }
    if (view->snapped == XD_SNAP_NONE)
        view->snap_restore = xd_view_outer_box(view);
    if (view->maximized)
        xd_view_set_maximized(view, false);
    view->snapped = side;
    xd_view_set_outer_box(view, snap_box(server, x, y, side));
}

void xd_snap_end(xd_server *server, xd_view *view) {
    xd_snap target = server->snap_target;
    show_preview(server, view, XD_SNAP_NONE);
    server->snap_unmaximize = false;
    if (!aero(server) || target == XD_SNAP_NONE)
        return;
    snap_to(view, target, server->cursor->x, server->cursor->y);
}

void xd_snap_cancel(xd_server *server) {
    server->snap_target = XD_SNAP_NONE;
    server->snap_unmaximize = false;
    if (server->snap_preview != nullptr)
        wlr_scene_node_set_enabled(&server->snap_preview->node, false);
}

void xd_snap_key(xd_view *view, xd_snap side) {
    xd_server *server = view->server;
    if (!aero(server))
        return;
    struct wlr_box b = xd_view_outer_box(view);
    double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
    xd_snap other = side == XD_SNAP_LEFT ? XD_SNAP_RIGHT : XD_SNAP_LEFT;
    /* Win+Left on a window snapped right (or the other way) puts it back. */
    if (view->snapped == other) {
        struct wlr_box r = view->snap_restore;
        view->snapped = XD_SNAP_NONE;
        xd_view_set_outer_box(view, r);
        return;
    }
    /* Again: the far half of the next monitor that way (with one monitor,
     * this one's other half), as Windows 7 does. */
    if (view->snapped == side) {
        xd_output *out = view_output(view);
        if (out == nullptr)
            return;
        struct wlr_box to = layout_box(server, next_output(server, out, side == XD_SNAP_LEFT ? -1 : 1));
        snap_to(view, other, to.x + to.width / 2.0, to.y + to.height / 2.0);
        return;
    }
    snap_to(view, side, cx, cy);
}

void xd_snap_tall(xd_view *view) {
    xd_server *server = view->server;
    if (!aero(server) || view->maximized || view->dialog)
        return;
    struct wlr_box b = xd_view_outer_box(view);
    struct wlr_box area = xd_output_work_area_at(server, b.x + b.width / 2.0, b.y + b.height / 2.0);
    if (view->snapped == XD_SNAP_NONE)
        view->snap_restore = b;
    view->snapped = XD_SNAP_TALL;
    xd_view_set_outer_box(view, (struct wlr_box){b.x, area.y, b.width, area.height});
}

void xd_snap_to_monitor(xd_view *view, int dir) {
    xd_server *server = view->server;
    if (!aero(server))
        return;
    xd_output *out = view_output(view);
    xd_output *next = out != nullptr ? next_output(server, out, dir) : nullptr;
    if (next == nullptr || next == out)
        return;
    struct wlr_box from = layout_box(server, out), to = layout_box(server, next);
    struct wlr_box b = xd_view_outer_box(view);
    double tx = to.x + to.width / 2.0, ty = to.y + to.height / 2.0;
    if (view->maximized) {
        /* Maximized there, and restores to the same place there. */
        view->restore = carry_box(view->restore, from, to);
        xd_view_set_outer_box(view, xd_output_work_area_at(server, tx, ty));
        return;
    }
    switch (view->snapped) {
    case XD_SNAP_LEFT:
    case XD_SNAP_RIGHT:
        view->snap_restore = carry_box(view->snap_restore, from, to);
        xd_view_set_outer_box(view, snap_box(server, tx, ty, view->snapped));
        return;
    case XD_SNAP_TALL: {
        view->snap_restore = carry_box(view->snap_restore, from, to);
        struct wlr_box area = xd_output_work_area_at(server, tx, ty);
        struct wlr_box c = carry_box(b, from, to);
        xd_view_set_outer_box(view, (struct wlr_box){c.x, area.y, c.width, area.height});
        return;
    }
    case XD_SNAP_MAXIMIZE:
    case XD_SNAP_NONE:
        break;
    }
    xd_view_set_outer_box(view, carry_box(b, from, to));
}

void xd_snap_forget_view(xd_server *server, xd_view *view) {
    for (size_t i = 0; i < server->shaken_n; i++)
        if (server->shaken[i] == view)
            server->shaken[i] = nullptr;
}

void xd_snap_finish(xd_server *server) {
    xd_free(server->shaken);
    server->shaken = nullptr;
    server->shaken_n = 0;
}
