/* SPDX-License-Identifier: MIT
 * "Show window contents while dragging" off: moving or sizing a window
 * drags its outline, and the window goes there on release, as Windows did
 * for slow graphics cards. Windows XORs a halftone frame as thick as the
 * sizing border onto the screen; a scene graph can't XOR, so the frame is
 * a black and white checkerboard, which shows on any background just the
 * same.
 */
#include <math.h>

#include "../core/alloc.h"
#include "server.h"

typedef struct xd_outline {
    struct wlr_scene_tree *tree;
    struct wlr_scene_buffer *edges[4]; /* top, bottom, left, right */
    int width, height;                 /* the box the buffers were drawn for */
    double scale;
} xd_outline;

bool xd_outline_dragging(const xd_server *server) { return !server->config.full_window_drag; }

/* A w x h (logical) strip of the checkerboard at `scale`. */
static struct wlr_buffer *checker(int w, int h, double scale) {
    double fw = ceil(w * scale);
    double fh = ceil(h * scale);
    int dw = XD_MAX((int)fw, 1);
    int dh = XD_MAX((int)fh, 1);
    cairo_surface_t *surface = nullptr;
    struct wlr_buffer *buf = xd_cairo_buffer_new(dw, dh, &surface);
    if (buf == nullptr)
        return nullptr;
    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < dh; y++) {
        uint32_t *row = (uint32_t *)(void *)(data + (size_t)y * (size_t)stride);
        for (int x = 0; x < dw; x++)
            row[x] = ((x + y) & 1) != 0 ? 0xFF000000u : 0xFFFFFFFFu;
    }
    cairo_surface_mark_dirty(surface);
    return buf;
}

static void set_edge(struct wlr_scene_buffer *edge, struct wlr_buffer *buf, int w, int h) {
    wlr_scene_buffer_set_buffer(edge, buf);
    wlr_scene_buffer_set_dest_size(edge, w, h);
}

void xd_outline_show(xd_server *server, struct wlr_box box) {
    xd_outline *o = server->outline;
    if (o == nullptr) {
        o = XD_NEW(xd_outline);
        o->tree = wlr_scene_tree_create(server->layer_drag);
        for (size_t i = 0; i < XD_ARRAY_LEN(o->edges); i++)
            o->edges[i] = wlr_scene_buffer_create(o->tree, nullptr);
        server->outline = o;
    }
    /* As thick as Windows' sizing border (SM_CXFRAME). */
    int t = xd_metric_px(&server->logical_theme, XD_METRIC_BORDER_WIDTH) + 3;
    box.width = XD_MAX(box.width, 2 * t + 1);
    box.height = XD_MAX(box.height, 2 * t + 1);
    double scale = xd_output_scale_at(server, box.x + box.width / 2.0, box.y + box.height / 2.0);
    if (box.width != o->width || box.height != o->height || scale != o->scale) {
        int side = box.height - 2 * t;
        struct wlr_buffer *across = checker(box.width, t, scale);
        struct wlr_buffer *down = checker(t, side, scale);
        if (across != nullptr) {
            set_edge(o->edges[0], across, box.width, t);
            set_edge(o->edges[1], across, box.width, t);
            wlr_buffer_drop(across); /* the scene holds its own locks */
        }
        if (down != nullptr) {
            set_edge(o->edges[2], down, t, side);
            set_edge(o->edges[3], down, t, side);
            wlr_buffer_drop(down);
        }
        o->width = box.width;
        o->height = box.height;
        o->scale = scale;
    }
    wlr_scene_node_set_position(&o->edges[0]->node, 0, 0);
    wlr_scene_node_set_position(&o->edges[1]->node, 0, box.height - t);
    wlr_scene_node_set_position(&o->edges[2]->node, 0, t);
    wlr_scene_node_set_position(&o->edges[3]->node, box.width - t, t);
    wlr_scene_node_set_position(&o->tree->node, box.x, box.y);
    wlr_scene_node_raise_to_top(&o->tree->node);
    wlr_scene_node_set_enabled(&o->tree->node, true);
}

void xd_outline_hide(xd_server *server) {
    xd_outline *o = server->outline;
    if (o == nullptr)
        return;
    server->outline = nullptr;
    wlr_scene_node_destroy(&o->tree->node);
    xd_free(o);
}
