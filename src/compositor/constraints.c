/* SPDX-License-Identifier: MIT
 * Programs that capture the mouse and keyboard: games, Blender, virtual
 * machines, remote desktops.
 *
 * - pointer-constraints: a locked pointer stays where it is (the program
 *   reads relative motion instead), a confined one stays inside the window
 *   or a region of it. As in sway and labwc, a constraint is in force only
 *   for the window with the keyboard, from when the pointer is over it
 *   (inside its region); it ends when the window loses the keyboard, and
 *   while one of our own grabs runs (moving or sizing a window, from the
 *   mouse or the window menu). A lock that ends puts the pointer where the
 *   program hinted.
 * - relative-pointer: every motion's deltas, accelerated and raw, locked
 *   or not.
 * - keyboard-shortcuts-inhibit, and Xwayland's keyboard grab (offered to
 *   Xwayland alone, which asks for it when an X11 program grabs the
 *   keyboard): while the window with the keyboard asks, our shortcuts go
 *   to it, all but those grabkeys.h keeps. Always honoured, as sway does:
 *   Windows asks no permission of a program's low-level keyboard hook
 *   either, and it works only while the user keeps that window in front.
 *
 * Nothing strands the user: Ctrl+Alt pressed and let go on its own (as in
 * VMware) gives the pointer and the shortcuts back until the window is
 * clicked or gets the keyboard again; so does any of our shortcuts the
 * program didn't take (Alt+Tab, the Windows key, Ctrl+Esc), and
 * Ctrl+Alt+Delete, which no program takes.
 */
#include <math.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/xwayland/server.h>

#include "../core/alloc.h"
#include "grabkeys.h"
#include "server.h"
#include "xwayland-keyboard-grab-unstable-v1-protocol.h"

typedef struct xd_constraint {
    xd_constraints *owner;
    struct wlr_pointer_constraint_v1 *wlr;
    struct wl_listener set_region;
    struct wl_listener destroy;
} xd_constraint;

typedef struct xd_shortcuts_inhibitor {
    struct wl_list link; /* xd_constraints.inhibitors */
    xd_constraints *owner;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *wlr;
    struct wl_listener destroy;
} xd_shortcuts_inhibitor;

/* An X11 program's keyboard grab, from Xwayland. */
typedef struct xd_x11_grab {
    struct wl_list link; /* xd_constraints.x11_grabs */
    xd_constraints *owner;
    struct wlr_surface *surface; /* nullptr once it's gone */
    struct wl_listener surface_destroy;
} xd_x11_grab;

struct xd_constraints {
    xd_server *server;
    struct wlr_pointer_constraints_v1 *manager;
    struct wlr_relative_pointer_manager_v1 *relative;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *inhibit;
    struct wl_global *x11_grab;
    xd_constraint *active;
    struct wl_list inhibitors; /* xd_shortcuts_inhibitor.link */
    struct wl_list x11_grabs;  /* xd_x11_grab.link */
    /* Ctrl+Alt or one of our shortcuts gave the input back from this
     * surface (compared, never used): until it's clicked or gets the
     * keyboard again, it constrains nothing and takes no shortcuts. */
    struct wlr_surface *released;
    bool inhibited; /* last seen, for the log */
    xd_release_chord chord;
    struct wl_listener new_constraint;
    struct wl_listener new_inhibitor;
    struct wl_listener keyboard_focus;
    struct wl_listener pointer_focus;
};

/* ---- where a surface is ------------------------------------------------------- */

typedef struct find_origin {
    struct wlr_surface *surface;
    int x, y;
    bool found;
} find_origin;

static void find_origin_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    find_origin *finder = data;
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
    if (!finder->found && ss != nullptr && ss->surface == finder->surface) {
        finder->found = true;
        finder->x = sx;
        finder->y = sy;
    }
}

/* The surface's top left corner in layout coordinates, found in the scene
 * (the window may have moved since the constraint began). */
static bool surface_origin(xd_server *server, struct wlr_surface *surface, double *origin_x,
                           double *origin_y) {
    find_origin finder = {.surface = surface};
    wlr_scene_node_for_each_buffer(&server->scene->tree.node, find_origin_buffer, &finder);
    *origin_x = finder.x;
    *origin_y = finder.y;
    return finder.found;
}

/* ---- which window takes the input ------------------------------------------------- */

static struct wlr_surface *keyboard_focus(const xd_constraints *cs) {
    struct wlr_surface *focus = cs->server->seat->keyboard_state.focused_surface;
    return focus != nullptr && focus != cs->released ? focus : nullptr;
}

/* An X11 grab counts for the window with the keyboard, or for a menu of
 * the same program (an override-redirect window, which never has the
 * keyboard itself): X11 programs grab the keyboard while a menu is open,
 * and in X11 the window manager's keys wait meanwhile too. */
static bool x11_grab_applies(const xd_x11_grab *x11_grab, struct wlr_surface *focus) {
    if (x11_grab->surface == nullptr || focus == nullptr)
        return false;
    if (x11_grab->surface == focus)
        return true;
    struct wlr_xwayland_surface *gx = wlr_xwayland_surface_try_from_wlr_surface(x11_grab->surface);
    struct wlr_xwayland_surface *fx = wlr_xwayland_surface_try_from_wlr_surface(focus);
    return gx != nullptr && fx != nullptr && gx->override_redirect && gx->pid > 0 &&
           gx->pid == fx->pid;
}

static bool inhibited_now(const xd_constraints *cs) {
    struct wlr_surface *focus = keyboard_focus(cs);
    if (focus == nullptr || cs->server->locked)
        return false;
    XD_LIST_FOR_EACH(in, &cs->inhibitors, xd_shortcuts_inhibitor, link) {
        if (in->wlr->surface == focus)
            return true;
    }
    XD_LIST_FOR_EACH(x11_grab, &cs->x11_grabs, xd_x11_grab, link) {
        if (x11_grab_applies(x11_grab, focus))
            return true;
    }
    return false;
}

bool xd_shortcuts_inhibited(const xd_server *server) {
    return server->constraints != nullptr && inhibited_now(server->constraints);
}

/* Tells each inhibitor whether it's in force: only the focused window's. */
static void update_inhibitors(xd_constraints *cs) {
    struct wlr_surface *focus = cs->server->locked ? nullptr : keyboard_focus(cs);
    XD_LIST_FOR_EACH(in, &cs->inhibitors, xd_shortcuts_inhibitor, link) {
        bool on = focus != nullptr && in->wlr->surface == focus;
        if (on && !in->wlr->active)
            wlr_keyboard_shortcuts_inhibitor_v1_activate(in->wlr);
        else if (!on && in->wlr->active)
            wlr_keyboard_shortcuts_inhibitor_v1_deactivate(in->wlr);
    }
    bool now = inhibited_now(cs);
    if (now != cs->inhibited)
        wlr_log(WLR_INFO, "%s", now ? "the focused program takes the shortcuts"
                                    : "shortcuts are the desktop's again");
    cs->inhibited = now;
}

/* ---- pointer constraints --------------------------------------------------------- */

/* Our grabs, a drag and the lock screen come first. */
static bool may_constrain(const xd_constraints *cs) {
    const xd_server *server = cs->server;
    return server->mode == XD_CURSOR_PASSTHROUGH && !server->locked && server->seat->drag == nullptr;
}

/* The pixel a surface-local point is in. */
static int floor_int(double value) {
    double floored = floor(value);
    return (int)floored;
}

/* The region a constraint holds to (into `out`, to be finished): what it
 * asked for within the surface's input region. wlroots works that out at
 * the surface's next commit, which a window that doesn't redraw (an X11
 * one, often) may not make for long; until then, as wlroots will. */
static void constraint_region(const struct wlr_pointer_constraint_v1 *pointer_constraint_v1,
                              pixman_region32_t *out) {
    pixman_region32_init(out);
    if (!pixman_region32_empty(&pointer_constraint_v1->region))
        pixman_region32_copy(out, &pointer_constraint_v1->region);
    else if (!pixman_region32_empty(&pointer_constraint_v1->current.region))
        pixman_region32_intersect(out, &pointer_constraint_v1->surface->input_region,
                                  &pointer_constraint_v1->current.region);
    else
        pixman_region32_copy(out, &pointer_constraint_v1->surface->input_region);
}

static bool region_has(const struct wlr_pointer_constraint_v1 *pointer_constraint_v1, double sx,
                       double sy) {
    pixman_region32_t region;
    constraint_region(pointer_constraint_v1, &region);
    bool in = pixman_region32_contains_point(&region, floor_int(sx), floor_int(sy), nullptr);
    pixman_region32_fini(&region);
    return in;
}

/* Whether the pointer is over the constraint's surface, in its region. */
static bool pointer_inside(const xd_constraints *cs,
                           struct wlr_pointer_constraint_v1 *pointer_constraint_v1) {
    const struct wlr_seat_pointer_state *ps = &cs->server->seat->pointer_state;
    return ps->focused_surface == pointer_constraint_v1->surface &&
           region_has(pointer_constraint_v1, ps->sx, ps->sy);
}

/* A confined pointer outside its region (the window moved under it, or the
 * region changed) goes to the middle of its first rectangle. */
static void confine_inside(xd_constraints *cs,
                           struct wlr_pointer_constraint_v1 *pointer_constraint_v1) {
    xd_server *server = cs->server;
    double ox, oy;
    if (!surface_origin(server, pointer_constraint_v1->surface, &ox, &oy))
        return;
    double sx = server->cursor->x - ox, sy = server->cursor->y - oy;
    if (region_has(pointer_constraint_v1, sx, sy))
        return;
    pixman_region32_t region;
    constraint_region(pointer_constraint_v1, &region);
    int count = 0;
    const pixman_box32_t *boxes = pixman_region32_rectangles(&region, &count);
    if (count > 0) {
        sx = (boxes[0].x1 + boxes[0].x2) / 2.0;
        sy = (boxes[0].y1 + boxes[0].y2) / 2.0;
        wlr_cursor_warp_closest(server->cursor, nullptr, ox + sx, oy + sy);
        wlr_seat_pointer_warp(server->seat, sx, sy);
    }
    pixman_region32_fini(&region);
}

static void deactivate(xd_constraints *cs) {
    xd_constraint *constraint = cs->active;
    if (constraint == nullptr)
        return;
    cs->active = nullptr;
    struct wlr_pointer_constraint_v1 *pointer_constraint_v1 = constraint->wlr;
    xd_server *server = cs->server;
    /* The program said where its own cursor was while locked: ours goes
     * there, as sway does. */
    double ox, oy;
    if (pointer_constraint_v1->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
        pointer_constraint_v1->current.cursor_hint.enabled &&
        surface_origin(server, pointer_constraint_v1->surface, &ox, &oy)) {
        double sx = pointer_constraint_v1->current.cursor_hint.x,
               sy = pointer_constraint_v1->current.cursor_hint.y;
        wlr_cursor_warp(server->cursor, nullptr, ox + sx, oy + sy);
        if (server->seat->pointer_state.focused_surface == pointer_constraint_v1->surface)
            wlr_seat_pointer_warp(server->seat, sx, sy);
    }
    wlr_log(WLR_DEBUG, "pointer %s ended",
            pointer_constraint_v1->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? "lock"
                                                                            : "confinement");
    /* A oneshot constraint is destroyed by this (handle_constraint_destroy). */
    wlr_pointer_constraint_v1_send_deactivated(pointer_constraint_v1);
}

static void activate(xd_constraints *cs, xd_constraint *constraint) {
    cs->active = constraint;
    wlr_log(WLR_DEBUG, "pointer %s",
            constraint->wlr->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? "locked" : "confined");
    wlr_pointer_constraint_v1_send_activated(constraint->wlr);
    if (constraint->wlr->type == WLR_POINTER_CONSTRAINT_V1_CONFINED)
        confine_inside(cs, constraint->wlr);
}

void xd_constraints_update(xd_server *server) {
    xd_constraints *cs = server->constraints;
    if (cs == nullptr)
        return;
    struct wlr_surface *focus = keyboard_focus(cs);
    if (cs->active != nullptr && (cs->active->wlr->surface != focus || !may_constrain(cs)))
        deactivate(cs);
    if (cs->active != nullptr || focus == nullptr || !may_constrain(cs))
        return;
    struct wlr_pointer_constraint_v1 *pointer_constraint_v1 =
        wlr_pointer_constraints_v1_constraint_for_surface(cs->manager, focus, server->seat);
    if (pointer_constraint_v1 != nullptr && pointer_inside(cs, pointer_constraint_v1))
        activate(cs, pointer_constraint_v1->data);
}

bool xd_constraints_motion(xd_server *server, uint32_t time_msec, double *dx, double *dy,
                           double raw_dx, double raw_dy) {
    xd_constraints *cs = server->constraints;
    if (cs == nullptr)
        return true;
    wlr_relative_pointer_manager_v1_send_relative_motion(
        cs->relative, server->seat, (uint64_t)time_msec * 1000, *dx, *dy, raw_dx, raw_dy);
    if (cs->active != nullptr && !may_constrain(cs))
        deactivate(cs);
    xd_constraint *constraint = cs->active;
    if (constraint == nullptr)
        return true;
    if (constraint->wlr->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        *dx = *dy = 0;
        return false;
    }
    double ox, oy;
    if (!surface_origin(server, constraint->wlr->surface, &ox, &oy))
        return true;
    double sx = server->cursor->x - ox, sy = server->cursor->y - oy;
    double nx, ny;
    pixman_region32_t region;
    constraint_region(constraint->wlr, &region);
    bool inside = wlr_region_confine(&region, sx, sy, sx + *dx, sy + *dy, &nx, &ny);
    pixman_region32_fini(&region);
    if (!inside) {
        /* It wasn't inside to begin with: back in, and no further. */
        confine_inside(cs, constraint->wlr);
        *dx = *dy = 0;
        return true;
    }
    *dx = nx - sx;
    *dy = ny - sy;
    return true;
}

static void handle_set_region(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_constraint *constraint = XD_CONTAINER_OF(listener, xd_constraint, set_region);
    xd_constraints *cs = constraint->owner;
    if (cs->active == constraint && constraint->wlr->type == WLR_POINTER_CONSTRAINT_V1_CONFINED)
        confine_inside(cs, constraint->wlr);
    else if (cs->active == nullptr)
        xd_constraints_update(cs->server);
}

static void handle_constraint_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_constraint *constraint = XD_CONTAINER_OF(listener, xd_constraint, destroy);
    xd_constraints *cs = constraint->owner;
    if (cs->active == constraint) {
        /* Its program ended it (or went): the hint still counts. */
        double ox, oy;
        struct wlr_pointer_constraint_v1 *pointer_constraint_v1 = constraint->wlr;
        if (pointer_constraint_v1->type == WLR_POINTER_CONSTRAINT_V1_LOCKED &&
            pointer_constraint_v1->current.cursor_hint.enabled &&
            surface_origin(cs->server, pointer_constraint_v1->surface, &ox, &oy))
            wlr_cursor_warp(cs->server->cursor, nullptr,
                            ox + pointer_constraint_v1->current.cursor_hint.x,
                            oy + pointer_constraint_v1->current.cursor_hint.y);
        cs->active = nullptr;
    }
    xd_unlisten(&constraint->set_region);
    xd_unlisten(&constraint->destroy);
    constraint->wlr->data = nullptr;
    xd_free(constraint);
}

static void handle_new_constraint(struct wl_listener *listener, void *data) {
    xd_constraints *cs = XD_CONTAINER_OF(listener, xd_constraints, new_constraint);
    struct wlr_pointer_constraint_v1 *pointer_constraint_v1 = data;
    xd_constraint *constraint = XD_NEW(xd_constraint);
    constraint->owner = cs;
    constraint->wlr = pointer_constraint_v1;
    pointer_constraint_v1->data = constraint;
    XD_LISTEN(&pointer_constraint_v1->events.set_region, constraint, set_region, handle_set_region);
    XD_LISTEN(&pointer_constraint_v1->events.destroy, constraint, destroy,
              handle_constraint_destroy);
    wlr_log(WLR_DEBUG, "a program asks to %s the pointer",
            pointer_constraint_v1->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? "lock" : "confine");
    xd_constraints_update(cs->server);
}

/* ---- giving the input back --------------------------------------------------------- */

void xd_constraints_release(xd_server *server) {
    xd_constraints *cs = server->constraints;
    if (cs == nullptr)
        return;
    struct wlr_surface *focus = keyboard_focus(cs);
    if (focus == nullptr || (cs->active == nullptr && !inhibited_now(cs)))
        return; /* nothing taken */
    wlr_log(WLR_INFO, "the pointer and shortcuts are the desktop's until the window is clicked");
    cs->released = focus;
    deactivate(cs);
    update_inhibitors(cs);
}

void xd_constraints_key(xd_server *server, const uint32_t *syms, int nsyms, bool pressed) {
    xd_constraints *cs = server->constraints;
    if (cs != nullptr && xd_release_chord_key(&cs->chord, syms, nsyms, pressed))
        xd_constraints_release(server);
}

void xd_constraints_click(xd_server *server) {
    xd_constraints *cs = server->constraints;
    if (cs == nullptr || cs->released == nullptr)
        return;
    const struct wlr_seat *seat = server->seat;
    if (seat->pointer_state.focused_surface != cs->released ||
        seat->keyboard_state.focused_surface != cs->released)
        return;
    cs->released = nullptr;
    update_inhibitors(cs);
    xd_constraints_update(server);
}

static void handle_keyboard_focus(struct wl_listener *listener, void *data) {
    xd_constraints *cs = XD_CONTAINER_OF(listener, xd_constraints, keyboard_focus);
    const struct wlr_seat_keyboard_focus_change_event *seat_keyboard_focus_change_event = data;
    /* Another window has the keyboard: what was released is forgiven, for
     * when it comes back. */
    if (seat_keyboard_focus_change_event->new_surface != cs->released)
        cs->released = nullptr;
    update_inhibitors(cs);
    xd_constraints_update(cs->server);
}

static void handle_pointer_focus(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_constraints *cs = XD_CONTAINER_OF(listener, xd_constraints, pointer_focus);
    xd_constraints_update(cs->server);
}

/* ---- keyboard-shortcuts-inhibit ------------------------------------------------- */

static void handle_inhibitor_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_shortcuts_inhibitor *in = XD_CONTAINER_OF(listener, xd_shortcuts_inhibitor, destroy);
    xd_constraints *cs = in->owner;
    xd_unlisten(&in->destroy);
    wl_list_remove(&in->link);
    xd_free(in);
    update_inhibitors(cs);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data) {
    xd_constraints *cs = XD_CONTAINER_OF(listener, xd_constraints, new_inhibitor);
    xd_shortcuts_inhibitor *in = XD_NEW(xd_shortcuts_inhibitor);
    in->owner = cs;
    in->wlr = data;
    XD_LISTEN(&in->wlr->events.destroy, in, destroy, handle_inhibitor_destroy);
    wl_list_insert(&cs->inhibitors, &in->link);
    update_inhibitors(cs);
}

/* ---- Xwayland's keyboard grab (wlroots has none of its own) ---------------------- */

static void x11_grab_forget_surface(xd_x11_grab *x11_grab) {
    xd_unlisten(&x11_grab->surface_destroy);
    x11_grab->surface = nullptr;
}

static void handle_x11_grab_surface_destroy(struct wl_listener *listener,
                                            [[maybe_unused]] void *data) {
    xd_x11_grab *x11_grab = XD_CONTAINER_OF(listener, xd_x11_grab, surface_destroy);
    x11_grab_forget_surface(x11_grab);
    update_inhibitors(x11_grab->owner);
}

static void x11_grab_resource_destroy(struct wl_resource *resource) {
    xd_x11_grab *x11_grab = wl_resource_get_user_data(resource);
    xd_constraints *cs = x11_grab->owner;
    x11_grab_forget_surface(x11_grab);
    wl_list_remove(&x11_grab->link);
    xd_free(x11_grab);
    update_inhibitors(cs);
}

static void x11_grab_destroy([[maybe_unused]] struct wl_client *client,
                             struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_xwayland_keyboard_grab_v1_interface x11_grab_impl = {
    .destroy = x11_grab_destroy,
};

static void x11_grab_manager_destroy([[maybe_unused]] struct wl_client *client,
                                     struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void x11_grab_manager_grab(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id, struct wl_resource *surface_resource,
                                  [[maybe_unused]] struct wl_resource *seat_resource) {
    xd_constraints *cs = wl_resource_get_user_data(resource);
    struct wl_resource *grab_resource = wl_resource_create(
        client, &zwp_xwayland_keyboard_grab_v1_interface, wl_resource_get_version(resource), id);
    if (grab_resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    xd_x11_grab *x11_grab = XD_NEW(xd_x11_grab);
    x11_grab->owner = cs;
    x11_grab->surface = wlr_surface_from_resource(surface_resource);
    XD_LISTEN(&x11_grab->surface->events.destroy, x11_grab, surface_destroy,
              handle_x11_grab_surface_destroy);
    wl_list_insert(&cs->x11_grabs, &x11_grab->link);
    wl_resource_set_implementation(grab_resource, &x11_grab_impl, x11_grab,
                                   x11_grab_resource_destroy);
    wlr_log(WLR_DEBUG, "an X11 program grabbed the keyboard");
    update_inhibitors(cs);
}

static const struct zwp_xwayland_keyboard_grab_manager_v1_interface x11_grab_manager_impl = {
    .destroy = x11_grab_manager_destroy,
    .grab_keyboard = x11_grab_manager_grab,
};

static void bind_x11_grab(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(
        client, &zwp_xwayland_keyboard_grab_manager_v1_interface, (int)version, id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &x11_grab_manager_impl, data, nullptr);
    wlr_log(WLR_DEBUG, "Xwayland bound the keyboard grab");
}

bool xd_constraints_global_private(const xd_server *server, const struct wl_global *global) {
    return server->constraints != nullptr && global == server->constraints->x11_grab;
}

bool xd_constraints_client_allowed(const xd_server *server, const struct wl_client *client) {
    const struct wlr_xwayland *xw = server->xwayland;
    return xw != nullptr && xw->server != nullptr && xw->server->client != nullptr &&
           client == xw->server->client;
}

/* ---- setup ----------------------------------------------------------------------- */

void xd_constraints_init(xd_server *server) {
    xd_constraints *cs = XD_NEW(xd_constraints);
    cs->server = server;
    wl_list_init(&cs->inhibitors);
    wl_list_init(&cs->x11_grabs);
    cs->manager = wlr_pointer_constraints_v1_create(server->display);
    XD_LISTEN(&cs->manager->events.new_constraint, cs, new_constraint, handle_new_constraint);
    cs->relative = wlr_relative_pointer_manager_v1_create(server->display);
    cs->inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(server->display);
    XD_LISTEN(&cs->inhibit->events.new_inhibitor, cs, new_inhibitor, handle_new_inhibitor);
    cs->x11_grab = wl_global_create(server->display, &zwp_xwayland_keyboard_grab_manager_v1_interface,
                                    1, cs, bind_x11_grab);
    XD_LISTEN(&server->seat->keyboard_state.events.focus_change, cs, keyboard_focus,
              handle_keyboard_focus);
    XD_LISTEN(&server->seat->pointer_state.events.focus_change, cs, pointer_focus,
              handle_pointer_focus);
    server->constraints = cs;
}

/* After the clients are gone (which took their constraints, inhibitors
 * and grabs with them). */
void xd_constraints_finish(xd_server *server) {
    xd_constraints *cs = server->constraints;
    if (cs == nullptr)
        return;
    xd_unlisten(&cs->new_constraint);
    xd_unlisten(&cs->new_inhibitor);
    xd_unlisten(&cs->keyboard_focus);
    xd_unlisten(&cs->pointer_focus);
    server->constraints = nullptr;
    xd_free(cs);
}
