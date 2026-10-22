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
static struct wlr_buffer *checker(int width, int height, double scale) {
    double fw = ceil(width * scale);
    double fh = ceil(height * scale);
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

static void set_edge(struct wlr_scene_buffer *edge, struct wlr_buffer *buf, int width, int height) {
    wlr_scene_buffer_set_buffer(edge, buf);
    wlr_scene_buffer_set_dest_size(edge, width, height);
}

void xd_outline_show(xd_server *server, struct wlr_box box) {
    xd_outline *outline = server->outline;
    if (outline == nullptr) {
        outline = XD_NEW(xd_outline);
        outline->tree = wlr_scene_tree_create(server->layer_drag);
        for (size_t i = 0; i < XD_ARRAY_LEN(outline->edges); i++)
            outline->edges[i] = wlr_scene_buffer_create(outline->tree, nullptr);
        server->outline = outline;
    }
    /* As thick as Windows' sizing border (SM_CXFRAME). */
    int thickness = xd_metric_px(&server->logical_theme, XD_METRIC_BORDER_WIDTH) + 3;
    box.width = XD_MAX(box.width, 2 * thickness + 1);
    box.height = XD_MAX(box.height, 2 * thickness + 1);
    double scale = xd_output_scale_at(server, box.x + box.width / 2.0, box.y + box.height / 2.0);
    if (box.width != outline->width || box.height != outline->height || scale != outline->scale) {
        int side = box.height - 2 * thickness;
        struct wlr_buffer *across = checker(box.width, thickness, scale);
        struct wlr_buffer *down = checker(thickness, side, scale);
        if (across != nullptr) {
            set_edge(outline->edges[0], across, box.width, thickness);
            set_edge(outline->edges[1], across, box.width, thickness);
            wlr_buffer_drop(across); /* the scene holds its own locks */
        }
        if (down != nullptr) {
            set_edge(outline->edges[2], down, thickness, side);
            set_edge(outline->edges[3], down, thickness, side);
            wlr_buffer_drop(down);
        }
        outline->width = box.width;
        outline->height = box.height;
        outline->scale = scale;
    }
    wlr_scene_node_set_position(&outline->edges[0]->node, 0, 0);
    wlr_scene_node_set_position(&outline->edges[1]->node, 0, box.height - thickness);
    wlr_scene_node_set_position(&outline->edges[2]->node, 0, thickness);
    wlr_scene_node_set_position(&outline->edges[3]->node, box.width - thickness, thickness);
    wlr_scene_node_set_position(&outline->tree->node, box.x, box.y);
    wlr_scene_node_raise_to_top(&outline->tree->node);
    wlr_scene_node_set_enabled(&outline->tree->node, true);
}

void xd_outline_hide(xd_server *server) {
    xd_outline *outline = server->outline;
    if (outline == nullptr)
        return;
    server->outline = nullptr;
    wlr_scene_node_destroy(&outline->tree->node);
    xd_free(outline);
}
