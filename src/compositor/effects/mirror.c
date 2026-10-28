/* SPDX-License-Identifier: MIT
 * Window mirrors (mirror.h): the window's scene buffers copied into a
 * tree of our own, placed and scaled as an effect says.
 */
#include "mirror.h"

#include <math.h>

#include "../../core/alloc.h"
#include "effects.h"
#ifdef XD_SCENEFX
#include "glfx.h"
#endif

typedef struct mirror_piece {
    struct wlr_scene_buffer *node;
    struct wlr_buffer *buffer; /* what the node shows (the node holds it) */
    int x, y;                  /* from the window's outer origin, layout units */
    int width, height;         /* its size there */
    struct wlr_fbox source;
    enum wl_output_transform transform;
} mirror_piece;

struct xd_mirror {
    xd_view *view; /* nullptr once detached */
    struct wlr_scene_tree *tree;
    mirror_piece *pieces;
    size_t piece_count, piece_capacity;
    struct wlr_box box;
    double x, y, scale_x, scale_y;
    float opacity;
    bool dirty; /* the window committed since the last update */
    uint64_t picture_serial; /* glfx's picture of the window last shown */
    struct wlr_surface *surface;
    struct wl_listener commit;
    struct wl_listener surface_destroy;
};

/* The pictures are only pictures: the pointer reaches what's below. */
static bool mirror_reject_input([[maybe_unused]] struct wlr_scene_buffer *buffer,
                                [[maybe_unused]] double *sx, [[maybe_unused]] double *sy) {
    return false;
}

static int mirror_round(double value) {
    return (int)lround(value);
}

static void mirror_apply_piece(const xd_mirror *mirror, const mirror_piece *piece) {
    int left = mirror_round(piece->x * mirror->scale_x);
    int top = mirror_round(piece->y * mirror->scale_y);
    int right = mirror_round((piece->x + piece->width) * mirror->scale_x);
    int bottom = mirror_round((piece->y + piece->height) * mirror->scale_y);
    wlr_scene_node_set_position(&piece->node->node, left, top);
    wlr_scene_buffer_set_dest_size(piece->node, XD_MAX(right - left, 1), XD_MAX(bottom - top, 1));
    bool whole = piece->source.width <= 0 || piece->source.height <= 0;
    wlr_scene_buffer_set_source_box(piece->node, whole ? nullptr : &piece->source);
    wlr_scene_buffer_set_transform(piece->node, piece->transform);
    wlr_scene_buffer_set_opacity(piece->node, mirror->opacity);
}

static void mirror_add_piece(xd_mirror *mirror) {
    if (mirror->piece_count == mirror->piece_capacity) {
        mirror->piece_capacity = XD_MAX(mirror->piece_capacity * 2, 8);
        mirror->pieces =
            xd_realloc_array(mirror->pieces, mirror->piece_capacity, sizeof(*mirror->pieces));
    }
    mirror_piece *piece = &mirror->pieces[mirror->piece_count++];
    *piece = (mirror_piece){.node = wlr_scene_buffer_create(mirror->tree, nullptr)};
    piece->node->point_accepts_input = mirror_reject_input;
    wlr_scene_buffer_set_filter_mode(piece->node, WLR_SCALE_FILTER_BILINEAR);
}

/* The index-th buffer of the window, as `source` at (x, y) shows it. */
static void mirror_copy_piece(xd_mirror *mirror, struct wlr_scene_buffer *source, int x, int y,
                              size_t *index) {
    xd_view *view = mirror->view;
    struct wlr_buffer *buffer = source->buffer;
    /* The scene lets go of the frame's pixels once it has a texture;
     * frame.c keeps them for us. */
    if (source == view->frame && view->kept_frame != nullptr)
        buffer = view->kept_frame;
    if (buffer == nullptr && *index >= mirror->piece_count)
        return; /* nothing to show yet */
    if (*index >= mirror->piece_count)
        mirror_add_piece(mirror);
    mirror_piece *piece = &mirror->pieces[(*index)++];
    if (buffer != nullptr && (buffer != piece->buffer || mirror->dirty)) {
        wlr_scene_buffer_set_buffer(piece->node, buffer);
        piece->buffer = buffer;
    }
    piece->x = x;
    piece->y = y;
    if (buffer != nullptr) {
        piece->width = source->dst_width > 0 ? source->dst_width : buffer->width;
        piece->height = source->dst_height > 0 ? source->dst_height : buffer->height;
    }
    piece->source = source->src_box;
    piece->transform = source->transform;
}

/* Every buffer under `node` that shows, in drawing order; the window's
 * own tree counts even while it's hidden. */
static void mirror_visit(xd_mirror *mirror, struct wlr_scene_node *node, int x, int y,
                         size_t *index, bool root) {
    if (!root && !node->enabled)
        return;
    if (!root) {
        x += node->x;
        y += node->y;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link) {
            mirror_visit(mirror, child, x, y, index, false);
        }
    } else if (node->type == WLR_SCENE_NODE_BUFFER) {
        mirror_copy_piece(mirror, wlr_scene_buffer_from_node(node), x, y, index);
    }
    /* Rects, and SceneFX's blur and shadow, aren't the window's picture. */
}

#ifdef XD_SCENEFX
/* A window whose colours Window Opacity changes is drawn from glfx's
 * picture of it, its own buffers fully transparent: the mirror shows that
 * picture alone, as the window looks. */
static bool mirror_copy_picture(xd_mirror *mirror, size_t *index) {
    xd_glfx_window_picture picture;
    if (!xd_glfx_get_window_picture(mirror->view, &picture))
        return false;
    if (mirror->piece_count == 0)
        mirror_add_piece(mirror);
    mirror_piece *piece = &mirror->pieces[(*index)++];
    /* A swapchain's buffer comes back with a new picture in it. */
    if (picture.buffer != piece->buffer || picture.serial != mirror->picture_serial) {
        wlr_scene_buffer_set_buffer(piece->node, picture.buffer);
        piece->buffer = picture.buffer;
        mirror->picture_serial = picture.serial;
    }
    piece->x = picture.box.x;
    piece->y = picture.box.y;
    piece->width = picture.box.width;
    piece->height = picture.box.height;
    piece->source = picture.source;
    piece->transform = WL_OUTPUT_TRANSFORM_NORMAL;
    return true;
}
#endif

void xd_mirror_update(xd_mirror *mirror) {
    xd_view *view = mirror->view;
    if (view == nullptr)
        return;
    mirror->box = xd_view_outer_box(view);
    size_t index = 0;
#ifdef XD_SCENEFX
    if (!mirror_copy_picture(mirror, &index))
#endif
        mirror_visit(mirror, &view->tree->node, 0, 0, &index, true);
    /* Subsurfaces or popups that went. */
    while (mirror->piece_count > index) {
        mirror->piece_count--;
        wlr_scene_node_destroy(&mirror->pieces[mirror->piece_count].node->node);
    }
    mirror->dirty = false;
    for (size_t i = 0; i < mirror->piece_count; i++)
        mirror_apply_piece(mirror, &mirror->pieces[i]);
}

static void mirror_handle_commit(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_mirror *mirror = XD_CONTAINER_OF(listener, xd_mirror, commit);
    mirror->dirty = true;
    /* The window may be hidden, so its commit damages nothing: the
     * mirror's update needs a frame of its own. */
    if (mirror->view != nullptr)
        xd_effects_schedule_frames(mirror->view->server);
}

static void mirror_handle_surface_destroy(struct wl_listener *listener,
                                          [[maybe_unused]] void *data) {
    xd_mirror_detach(XD_CONTAINER_OF(listener, xd_mirror, surface_destroy));
}

xd_mirror *xd_mirror_create(xd_view *view, struct wlr_scene_tree *parent) {
    xd_mirror *mirror = XD_NEW(xd_mirror);
    mirror->view = view;
    mirror->tree = wlr_scene_tree_create(parent);
    mirror->scale_x = mirror->scale_y = 1.0;
    mirror->opacity = 1.0f;
    mirror->dirty = true;
    xd_frame_keep_pixels(view, true);
    mirror->surface = xd_view_surface(view);
    if (mirror->surface != nullptr) {
        XD_LISTEN(&mirror->surface->events.commit, mirror, commit, mirror_handle_commit);
        XD_LISTEN(&mirror->surface->events.destroy, mirror, surface_destroy,
                  mirror_handle_surface_destroy);
    }
    xd_mirror_update(mirror);
    xd_mirror_place(mirror, mirror->box.x, mirror->box.y, 1.0, 1.0);
    return mirror;
}

void xd_mirror_detach(xd_mirror *mirror) {
    if (mirror->view == nullptr)
        return;
    xd_mirror_update(mirror);
    xd_frame_keep_pixels(mirror->view, false);
    xd_unlisten(&mirror->commit);
    xd_unlisten(&mirror->surface_destroy);
    mirror->view = nullptr;
    mirror->surface = nullptr;
}

void xd_mirror_destroy(xd_mirror *mirror) {
    if (mirror == nullptr)
        return;
    if (mirror->view != nullptr) {
        xd_frame_keep_pixels(mirror->view, false);
        xd_unlisten(&mirror->commit);
        xd_unlisten(&mirror->surface_destroy);
    }
    wlr_scene_node_destroy(&mirror->tree->node);
    xd_free(mirror->pieces);
    xd_free(mirror);
}

struct wlr_box xd_mirror_get_box(const xd_mirror *mirror) {
    return mirror->box;
}

struct wlr_scene_tree *xd_mirror_get_tree(const xd_mirror *mirror) {
    return mirror->tree;
}

void xd_mirror_place(xd_mirror *mirror, double x, double y, double scale_x, double scale_y) {
    wlr_scene_node_set_position(&mirror->tree->node, mirror_round(x), mirror_round(y));
    if (mirror->scale_x == scale_x && mirror->scale_y == scale_y)
        return;
    mirror->scale_x = scale_x;
    mirror->scale_y = scale_y;
    for (size_t i = 0; i < mirror->piece_count; i++)
        mirror_apply_piece(mirror, &mirror->pieces[i]);
}

void xd_mirror_set_opacity(xd_mirror *mirror, float opacity) {
    mirror->opacity = opacity;
    for (size_t i = 0; i < mirror->piece_count; i++)
        wlr_scene_buffer_set_opacity(mirror->pieces[i].node, opacity);
}

static void mirror_send_surface_frame_done(struct wlr_surface *surface,
                                           [[maybe_unused]] int sx, [[maybe_unused]] int sy,
                                           void *data) {
    wlr_surface_send_frame_done(surface, data);
}

void xd_mirror_send_frame_done(xd_mirror *mirror, const struct timespec *now) {
    if (mirror->surface == nullptr)
        return;
    /* wlr_surface_send_frame_done takes a const timespec; the iterator
     * passes void *. */
    struct timespec when = *now;
    wlr_surface_for_each_surface(mirror->surface, mirror_send_surface_frame_done, &when);
}
