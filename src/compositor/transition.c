/* SPDX-License-Identifier: MIT
 * Display Properties' "Use the following transition effect for menus and
 * tooltips": the shell's menus and tooltips fade in, or scroll (slide) out
 * of the edge they open from, as Windows' menu animation does: down from
 * a menu bar or a point, up from a bottom taskbar, sideways for a submenu.
 *
 * Done here rather than in the shell, which would have to draw every step
 * itself: the scene graph fades a surface with its buffers' opacity and
 * slides it with a clip, whatever renderer draws it. Only popups rooted at
 * a layer surface (the shell's) animate; programs' own menus are left as
 * their toolkits draw them.
 */
#include <time.h>

#include "../core/alloc.h"
#include "server.h"

static constexpr int64_t DURATION_NS = 200 * 1000000LL;
static constexpr int FRAME_MS = 15;

typedef enum slide_dir : uint8_t { SLIDE_DOWN, SLIDE_UP, SLIDE_RIGHT, SLIDE_LEFT } slide_dir;

typedef struct xd_transition {
    xd_server *server;
    struct wlr_xdg_popup *popup;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_event_source *timer;
    bool started, done;
    xd_menu_transition effect;
    slide_dir dir;
    int64_t start;
    double t; /* progress, 0..1 */
} xd_transition;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* The popup's own surface (and subsurfaces), without its child popups:
 * the first child wlroots gives the popup's scene tree. */
static struct wlr_scene_node *surface_node(xd_transition *tr) {
    struct wlr_scene_tree *tree = tr->popup->base->data;
    if (tree == nullptr || wl_list_empty(&tree->children))
        return nullptr;
    struct wlr_scene_node *node;
    node = wl_container_of(tree->children.next, node, link);
    return node;
}

static void set_opacity(struct wlr_scene_buffer *buffer, [[maybe_unused]] int sx,
                        [[maybe_unused]] int sy, void *data) {
    wlr_scene_buffer_set_opacity(buffer, *(const float *)data);
}

/* Which way a popup opens, from where it went against what it's anchored
 * to (both relative to its parent): up or down from a point or a button,
 * sideways from a menu item beside it. */
static slide_dir direction(const struct wlr_xdg_popup *popup) {
    struct wlr_box g = popup->current.geometry;
    struct wlr_box a = popup->scheduled.rules.anchor_rect;
    if (a.width <= 1 && a.height <= 1)
        return g.y + g.height <= a.y + 1 ? SLIDE_UP : SLIDE_DOWN;
    if (g.y + g.height <= a.y + 2)
        return SLIDE_UP;
    if (g.y >= a.y + a.height - 2)
        return SLIDE_DOWN;
    return g.x + g.width / 2 >= a.x + a.width / 2 ? SLIDE_RIGHT : SLIDE_LEFT;
}

/* Puts the popup where progress t has it. The scene puts the tree back at
 * the popup's real position on every commit; this runs after that. */
static void apply(xd_transition *tr) {
    struct wlr_scene_tree *tree = tr->popup->base->data;
    struct wlr_scene_node *node = surface_node(tr);
    if (tree == nullptr || node == nullptr)
        return;
    double t = tr->done ? 1.0 : tr->t;
    if (tr->effect == XD_TRANSITION_FADE) {
        float alpha = (float)t;
        wlr_scene_node_for_each_buffer(node, set_opacity, &alpha);
        return;
    }
    /* Scroll: the far edge comes in first, as if pushed out from behind
     * the edge it opens at; the clip hides what hasn't come in yet. */
    struct wlr_box geo = tr->popup->base->geometry;
    struct wlr_box final = tr->popup->current.geometry;
    int w = geo.width > 0 ? geo.width : tr->popup->base->surface->current.width;
    int h = geo.height > 0 ? geo.height : tr->popup->base->surface->current.height;
    if (tr->done || w <= 0 || h <= 0) {
        wlr_scene_subsurface_tree_set_clip(node, nullptr);
        wlr_scene_node_set_position(&tree->node, final.x, final.y);
        return;
    }
    bool vertical = tr->dir == SLIDE_DOWN || tr->dir == SLIDE_UP;
    int len = vertical ? h : w;
    int shown = (int)(len * t + 0.5);
    int hidden = len - XD_MAX(shown, 1);
    struct wlr_box clip = {geo.x, geo.y, w, h};
    int x = final.x;
    int y = final.y;
    switch (tr->dir) {
    case SLIDE_DOWN:
        clip.y += hidden;
        clip.height -= hidden;
        y -= hidden;
        break;
    case SLIDE_UP:
        clip.height -= hidden;
        y += hidden;
        break;
    case SLIDE_RIGHT:
        clip.x += hidden;
        clip.width -= hidden;
        x -= hidden;
        break;
    case SLIDE_LEFT:
        clip.width -= hidden;
        x += hidden;
        break;
    }
    wlr_scene_subsurface_tree_set_clip(node, &clip);
    wlr_scene_node_set_position(&tree->node, x, y);
}

static void finish(xd_transition *tr) {
    tr->done = true;
    apply(tr);
    if (tr->timer != nullptr) {
        wl_event_source_remove(tr->timer);
        tr->timer = nullptr;
    }
}

static int tick(void *data) {
    xd_transition *tr = data;
    tr->t = (double)(now_ns() - tr->start) / (double)DURATION_NS;
    if (tr->t >= 1.0) {
        finish(tr);
        return 0;
    }
    apply(tr);
    wl_event_source_timer_update(tr->timer, FRAME_MS);
    return 0;
}

static void begin(xd_transition *tr) {
    tr->started = true;
    struct wlr_scene_tree *tree = tr->popup->base->data;
    xd_server *server = tr->server;
    if (!server->config.menu_transitions || tree == nullptr ||
        xd_layer_from_node(&tree->node) == nullptr) {
        tr->done = true;
        return;
    }
    tr->effect = server->config.menu_transition;
    tr->dir = direction(tr->popup);
    tr->start = now_ns();
    tr->t = 0;
    tr->timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->display), tick, tr);
    wl_event_source_timer_update(tr->timer, FRAME_MS);
    apply(tr);
}

static void handle_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_transition *tr = XD_CONTAINER_OF(listener, xd_transition, commit);
    if (tr->done)
        return;
    /* From its first frame on screen. */
    if (!tr->started) {
        if (tr->popup->base->surface->mapped)
            begin(tr);
        return;
    }
    apply(tr);
}

static void handle_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_transition *tr = XD_CONTAINER_OF(listener, xd_transition, destroy);
    if (tr->timer != nullptr)
        wl_event_source_remove(tr->timer);
    xd_unlisten(&tr->commit);
    xd_unlisten(&tr->destroy);
    xd_free(tr);
}

void xd_transition_watch_popup(xd_server *server, struct wlr_xdg_popup *popup) {
    xd_transition *tr = XD_NEW(xd_transition);
    tr->server = server;
    tr->popup = popup;
    XD_LISTEN(&popup->base->surface->events.commit, tr, commit, handle_commit);
    XD_LISTEN(&popup->events.destroy, tr, destroy, handle_destroy);
}
