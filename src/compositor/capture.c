/* SPDX-License-Identifier: MIT
 * Window pictures for Windows 7's taskbar thumbnails (Aero): every window
 * with a taskbar button gets an ext-foreign-toplevel-list handle, and the
 * shell captures it through ext-image-copy-capture-v1 with
 * ext-foreign-toplevel-image-capture-source-v1.
 *
 * Window contents are private: like xd-window-arranger-v1, these globals
 * are only offered to the shell the compositor started (arrange.c's global
 * filter), not to every client that asks.
 *
 * The capture source is our own rather than wlroots' scene-node source,
 * which renders whatever the scene shows where the window is (windows in
 * front of it included, nothing while it's minimized) and doesn't work on
 * SceneFX's scene graph. Ours draws the window alone, frame and client
 * surfaces, with the renderer, minimized or covered, and already scaled
 * down: no thumbnail needs more than THUMB_MAX pixels across.
 */
#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

/* Device px: the longest side of a captured window. Windows 7's
 * thumbnails are at most about 200 logical px wide; this leaves room for
 * a scale of 2 and a little more. */
static constexpr int THUMB_MAX = 512;

typedef struct xd_capture_source {
    struct wlr_ext_image_capture_source_v1 base;
    xd_server *server;
    xd_view *view;
    int started;
    bool dirty;  /* the window changed since the last frame */
    bool wanted; /* a frame was asked for; sent at the next change */
    struct wl_event_source *idle;
} xd_capture_source;

/* What a frame event carries to copy_frame: the picture. */
typedef struct capture_frame {
    struct wlr_ext_image_capture_source_v1_frame_event base;
    struct wlr_buffer *buffer;
} capture_frame;

/* ---- drawing the window -------------------------------------------------------- */

/* Explicitly synced surfaces drawn (linux-drm-syncobj): each is told when
 * our drawing is done with its buffer. Any more (subsurfaces) are left out
 * of the picture. */
enum { SYNCED_MAX = 16 };

typedef struct draw_ctx {
    struct wlr_render_pass *pass;
    double sx, sy; /* buffer px per layout unit */
    int ox, oy;    /* the client surfaces' origin within the frame */
    struct wlr_linux_drm_syncobj_surface_v1_state *synced[SYNCED_MAX];
    size_t synced_n;
} draw_ctx;

static void draw_surface(struct wlr_surface *surface, int x, int y, void *data) {
    draw_ctx *c = data;
    struct wlr_texture *tex = wlr_surface_get_texture(surface);
    if (tex == nullptr)
        return;
    /* The program's GPU may still be drawing it: wait for its acquire
     * point, as the scene graph does. */
    struct wlr_linux_drm_syncobj_surface_v1_state *sync =
        wlr_linux_drm_syncobj_v1_get_surface_state(surface);
    if (sync != nullptr && c->synced_n == SYNCED_MAX)
        return;
    if (sync != nullptr)
        c->synced[c->synced_n++] = sync;
    struct wlr_fbox src;
    wlr_surface_get_buffer_source_box(surface, &src);
    int lx = c->ox + x, ly = c->oy + y;
    struct wlr_box dst = {
        .x = (int)(lx * c->sx),
        .y = (int)(ly * c->sy),
        .width = (int)((lx + surface->current.width) * c->sx) - (int)(lx * c->sx),
        .height = (int)((ly + surface->current.height) * c->sy) - (int)(ly * c->sy),
    };
    if (dst.width <= 0 || dst.height <= 0)
        return;
    wlr_render_pass_add_texture(c->pass, &(struct wlr_render_texture_options){
                                             .texture = tex,
                                             .src_box = src,
                                             .dst_box = dst,
                                             .transform = surface->current.transform,
                                             .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                                             .wait_timeline = sync != nullptr
                                                                  ? sync->acquire_timeline
                                                                  : nullptr,
                                             .wait_point = sync != nullptr ? sync->acquire_point
                                                                           : 0,
                                         });
}

/* The window's frame and client surfaces, scaled into a w x h buffer. */
static struct wlr_buffer *render_view(xd_server *server, xd_view *view, int w, int h) {
    /* Whatever layout the allocator picks for ARGB (an implicit modifier):
     * it's only drawn into and read back. */
    uint64_t modifiers[] = {DRM_FORMAT_MOD_INVALID};
    struct wlr_drm_format fmt_set = {
        .format = DRM_FORMAT_ARGB8888, .len = 1, .capacity = 1, .modifiers = modifiers};
    const struct wlr_drm_format *fmt = &fmt_set;
    struct wlr_buffer *buf = wlr_allocator_create_buffer(server->allocator, w, h, fmt);
    if (buf == nullptr)
        return nullptr;
    /* With explicit sync, the pass signals a point of our own timeline
     * once it's done, which releases the programs' buffers it read. */
    struct wlr_buffer_pass_options opts = {0};
    if (server->syncobj != nullptr && server->capture_timeline == nullptr)
        server->capture_timeline =
            wlr_drm_syncobj_timeline_create(wlr_renderer_get_drm_fd(server->renderer));
    if (server->capture_timeline != nullptr) {
        opts.signal_timeline = server->capture_timeline;
        opts.signal_point = ++server->capture_point;
    }
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer, buf, &opts);
    if (pass == nullptr) {
        wlr_buffer_drop(buf);
        return nullptr;
    }
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                       .box = {0, 0, w, h},
                                       .color = {0, 0, 0, 0},
                                       .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                                   });
    struct wlr_box outer = xd_view_outer_box(view);
    draw_ctx c = {
        .pass = pass,
        .sx = (double)w / XD_MAX(outer.width, 1),
        .sy = (double)h / XD_MAX(outer.height, 1),
    };
    struct wlr_texture *frame_tex = nullptr;
    if (view->frame->node.enabled && view->frame->buffer != nullptr) {
        frame_tex = wlr_texture_from_buffer(server->renderer, view->frame->buffer);
        if (frame_tex != nullptr)
            wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
                                                  .texture = frame_tex,
                                                  .dst_box = {0, 0, w, h},
                                                  .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                                              });
    }
    struct wlr_surface *surface = xd_view_surface(view);
    if (surface != nullptr && view->content != nullptr) {
        c.ox = view->content->node.x;
        c.oy = view->content->node.y;
        wlr_surface_for_each_surface(surface, draw_surface, &c);
    }
    bool ok = wlr_render_pass_submit(pass);
    if (frame_tex != nullptr)
        wlr_texture_destroy(frame_tex);
    for (size_t i = 0; ok && opts.signal_timeline != nullptr && i < c.synced_n; i++)
        wlr_linux_drm_syncobj_v1_state_add_release_point(c.synced[i], opts.signal_timeline,
                                                         opts.signal_point, server->loop);
    if (!ok) {
        wlr_buffer_drop(buf);
        return nullptr;
    }
    return buf;
}

/* The picture's size: the window's at its output's scale, scaled down to
 * fit THUMB_MAX. */
static void capture_size(xd_view *view, uint32_t *w, uint32_t *h) {
    struct wlr_box outer = xd_view_outer_box(view);
    double scale = xd_view_scale(view);
    double dw = XD_MAX(outer.width, 1) * scale, dh = XD_MAX(outer.height, 1) * scale;
    double fit = XD_MIN(1.0, (double)THUMB_MAX / XD_MAX(dw, dh));
    *w = (uint32_t)XD_MAX(1.0, dw * fit + 0.5);
    *h = (uint32_t)XD_MAX(1.0, dh * fit + 0.5);
}

/* ---- the capture source -------------------------------------------------------- */

static void send_frame(void *data) {
    xd_capture_source *src = data;
    src->idle = nullptr;
    if (src->view == nullptr || !src->view->mapped)
        return;
    uint32_t w = 0, h = 0;
    capture_size(src->view, &w, &h);
    if (w != src->base.width || h != src->base.height) {
        /* Clients reallocate; the frame they're waiting on fails. */
        src->base.width = w;
        src->base.height = h;
        wl_signal_emit_mutable(&src->base.events.constraints_update, nullptr);
    }
    struct wlr_buffer *buf = render_view(src->server, src->view, (int)w, (int)h);
    if (buf == nullptr)
        return;
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, w, h);
    capture_frame ev = {.base = {.damage = &damage}, .buffer = buf};
    src->dirty = false;
    src->wanted = false;
    wl_signal_emit_mutable(&src->base.events.frame, &ev.base);
    pixman_region32_fini(&damage);
    wlr_buffer_drop(buf);
}

static void schedule_frame(xd_capture_source *src) {
    if (src->idle == nullptr)
        src->idle = wl_event_loop_add_idle(src->server->loop, send_frame, src);
}

static void source_start(struct wlr_ext_image_capture_source_v1 *base,
                         [[maybe_unused]] bool with_cursors) {
    XD_CONTAINER_OF(base, xd_capture_source, base)->started++;
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *base) {
    xd_capture_source *src = XD_CONTAINER_OF(base, xd_capture_source, base);
    if (src->started > 0)
        src->started--;
}

/* A client is waiting for a frame: at once if it hasn't had this picture
 * yet, else when the window next changes. */
static void source_request_frame(struct wlr_ext_image_capture_source_v1 *base, bool now) {
    xd_capture_source *src = XD_CONTAINER_OF(base, xd_capture_source, base);
    if (now || src->dirty)
        schedule_frame(src);
    else
        src->wanted = true;
}

static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
                              struct wlr_ext_image_copy_capture_frame_v1 *frame,
                              struct wlr_ext_image_capture_source_v1_frame_event *event) {
    xd_capture_source *src = XD_CONTAINER_OF(base, xd_capture_source, base);
    capture_frame *ev = XD_CONTAINER_OF(event, capture_frame, base);
    if (!wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, ev->buffer,
                                                          src->server->renderer))
        return; /* failed, and told so */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &now);
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
    .start = source_start,
    .stop = source_stop,
    .request_frame = source_request_frame,
    .copy_frame = source_copy_frame,
};

static xd_capture_source *source_for(xd_view *view) {
    if (view->capture != nullptr)
        return view->capture;
    xd_capture_source *src = XD_NEW(xd_capture_source);
    wlr_ext_image_capture_source_v1_init(&src->base, &source_impl);
    src->server = view->server;
    src->view = view;
    src->dirty = true;
    capture_size(view, &src->base.width, &src->base.height);
    /* wlroots frees these with the source. */
    uint32_t *formats = calloc(2, sizeof(*formats));
    if (formats != nullptr) {
        formats[0] = DRM_FORMAT_ARGB8888;
        formats[1] = DRM_FORMAT_XRGB8888;
        src->base.shm_formats = formats;
        src->base.shm_formats_len = 2;
    }
    view->capture = src;
    return src;
}

/* Clients' sessions stop; their source objects become inert. */
static void source_destroy(xd_capture_source *src) {
    if (src->idle != nullptr)
        wl_event_source_remove(src->idle);
    if (src->view != nullptr)
        src->view->capture = nullptr;
    wlr_ext_image_capture_source_v1_finish(&src->base);
    xd_free(src);
}

static void handle_new_request(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, capture_request);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req = data;
    xd_view *view = nullptr;
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (v->ext_toplevel == req->toplevel_handle && v->mapped)
            view = v;
    }
    /* A window that's gone gets an inert source, whose sessions stop at
     * once. */
    if (!wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
            req, view != nullptr ? &source_for(view)->base : nullptr))
        wlr_log(WLR_ERROR, "cannot create a capture source");
}

/* The display is going: wlroots wants its listeners off first. */
static void handle_sources_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, capture_sources_destroy);
    xd_unlisten(&server->capture_request);
    xd_unlisten(&server->capture_sources_destroy);
}


/* ---- windows ------------------------------------------------------------------- */

static struct wlr_ext_foreign_toplevel_handle_v1_state state_of(const xd_view *view) {
    return (struct wlr_ext_foreign_toplevel_handle_v1_state){
        .title = xd_view_title(view),
        .app_id = xd_view_app_id(view),
    };
}

void xd_capture_view_shown(xd_view *view) {
    xd_server *server = view->server;
    if (server->ext_toplevels == nullptr || view->ext_toplevel != nullptr)
        return;
    struct wlr_ext_foreign_toplevel_handle_v1_state st = state_of(view);
    view->ext_toplevel = wlr_ext_foreign_toplevel_handle_v1_create(server->ext_toplevels, &st);
}

void xd_capture_view_hidden(xd_view *view) {
    if (view->capture != nullptr) {
        view->capture->view = nullptr;
        source_destroy(view->capture);
    }
    if (view->ext_toplevel != nullptr)
        wlr_ext_foreign_toplevel_handle_v1_destroy(view->ext_toplevel);
    view->ext_toplevel = nullptr;
}

void xd_capture_view_changed(xd_view *view) {
    if (view->ext_toplevel == nullptr)
        return;
    struct wlr_ext_foreign_toplevel_handle_v1_state st = state_of(view);
    wlr_ext_foreign_toplevel_handle_v1_update_state(view->ext_toplevel, &st);
}

void xd_capture_view_committed(xd_view *view) {
    xd_capture_source *src = view->capture;
    if (src == nullptr)
        return;
    src->dirty = true;
    if (src->wanted)
        schedule_frame(src);
}

bool xd_capture_global_private(const xd_server *server, const struct wl_global *global) {
    for (size_t i = 0; i < XD_ARRAY_LEN(server->capture_globals); i++) {
        if (server->capture_globals[i] != nullptr && server->capture_globals[i] == global)
            return true;
    }
    return false;
}

void xd_capture_init(xd_server *server) {
    server->ext_toplevels = wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *sources =
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(server->display, 1);
    struct wlr_ext_image_copy_capture_manager_v1 *copy =
        wlr_ext_image_copy_capture_manager_v1_create(server->display, 1);
    if (server->ext_toplevels == nullptr || sources == nullptr || copy == nullptr) {
        wlr_log(WLR_ERROR, "no window capture: taskbar thumbnails won't show");
        return;
    }
    server->capture_globals[0] = server->ext_toplevels->global;
    server->capture_globals[1] = sources->global;
    server->capture_globals[2] = copy->global;
    XD_LISTEN(&sources->events.new_request, server, capture_request, handle_new_request);
    XD_LISTEN(&sources->events.destroy, server, capture_sources_destroy, handle_sources_destroy);
}
