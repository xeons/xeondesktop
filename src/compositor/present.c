/* SPDX-License-Identifier: MIT
 * How frames reach the monitor, for games above all:
 *
 * - explicit sync (linux-drm-syncobj): programs hand over buffers with
 *   timeline points to wait for and to signal, instead of the kernel's
 *   implicit fences. NVIDIA's own driver needs it (without it, windows
 *   flicker and stutter). Offered only where both the renderer and the
 *   backend can wait on and signal timelines, as sway does; the scene
 *   graph (wlroots' and SceneFX's alike) then waits for a buffer's acquire
 *   point before drawing it or scanning it out, and signals its release
 *   point once the frame that showed it is done with it.
 * - variable refresh rate (adaptive sync): the monitor's setting
 *   (Display Properties > Settings > Advanced > Monitor, monitors.ini, or
 *   wlr-output-management), used while a full-screen window is in front.
 * - tearing (tearing-control): a full-screen window that asks for
 *   asynchronous presentation (a game with vsync off, Vulkan's immediate
 *   mode) is flipped at once, tearing, when the frame is its own buffer
 *   alone (direct scanout). No setting: the program's hint is already the
 *   user's choice (the game's own vsync option), as Windows honours
 *   DXGI's ALLOW_TEARING and KWin the same hint.
 *
 * wlroots 0.20 has no fifo-v1 or commit-timing-v1; Mesa's Vulkan
 * presents FIFO with frame callbacks without them.
 */
#include <wlr/backend.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/log.h>

#include "present.h"
#include "server.h"

void xd_present_init(xd_server *server) {
    int drm_fd = wlr_renderer_get_drm_fd(server->renderer);
    if (server->renderer->features.timeline && server->backend->features.timeline &&
        drm_fd >= 0) {
        server->syncobj = wlr_linux_drm_syncobj_manager_v1_create(server->display, 1, drm_fd);
        if (server->syncobj != nullptr)
            wlr_log(WLR_INFO, "explicit sync (linux-drm-syncobj) offered");
    } else {
        wlr_log(WLR_INFO, "no explicit sync: the %s can't use timelines",
                !server->renderer->features.timeline ? "renderer" : "backend");
    }
    server->tearing = wlr_tearing_control_manager_v1_create(server->display, 1);
}

/* The window in front on that output: the most recently focused one there
 * (views are kept in that order). */
static xd_view *front_view(xd_output *out) {
    xd_server *server = out->server;
    XD_LIST_FOR_EACH(v, &server->views, xd_view, link) {
        if (!v->mapped || v->minimized)
            continue;
        struct wlr_box b = xd_view_outer_box(v);
        if (xd_output_at(server, b.x + b.width / 2.0, b.y + b.height / 2.0) == out)
            return v;
    }
    return nullptr;
}

/* What the choice for the output's next frame depends on; state is the
 * frame's, or nullptr before there is one. */
static xd_present_state gather(xd_output *out, const struct wlr_output_state *state) {
    struct wlr_output *o = out->wlr_output;
    xd_view *view = front_view(out);
    struct wlr_surface *surface =
        view != nullptr && view->fullscreen ? xd_view_surface(view) : nullptr;
    xd_present_state s = {
        .vrr = out->vrr,
        .vrr_on = o->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED,
        .vrr_rejected = out->vrr_rejected,
        .locked = out->server->locked,
        .fullscreen = surface != nullptr,
    };
    if (surface != nullptr && state != nullptr) {
        /* The scene graph scans a buffer out when it's all there is to
         * see: then the frame is the window's own buffer. */
        s.scanout = (state->committed & WLR_OUTPUT_STATE_BUFFER) && surface->buffer != nullptr &&
                    state->buffer == &surface->buffer->base;
        s.async_hint = wlr_tearing_control_manager_v1_surface_hint_from_surface(
                           out->server->tearing, surface) ==
                       WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
    }
    return s;
}

bool xd_present_pending(xd_output *out) {
    return out->wlr_output->enabled && xd_present_choose(gather(out, nullptr)).set_vrr;
}

void xd_present_frame(xd_output *out, struct wlr_output_state *state) {
    struct wlr_output *o = out->wlr_output;
    if (!o->enabled)
        return;
    xd_present_choice c = xd_present_choose(gather(out, state));
    if (c.set_vrr) {
        wlr_output_state_set_adaptive_sync_enabled(state, c.vrr);
        if (!wlr_output_test_state(o, state)) {
            /* Not this frame; and not again until the setting changes. */
            state->committed &= ~(uint32_t)WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
            if (c.vrr) {
                out->vrr_rejected = true;
                wlr_log(WLR_INFO, "output %s: variable refresh rate refused", o->name);
            }
        } else {
            wlr_log(WLR_DEBUG, "output %s: variable refresh rate %s", o->name,
                    c.vrr ? "on" : "off");
        }
    }
    if (c.tearing) {
        state->tearing_page_flip = true;
        /* The driver may not flip asynchronously (or not this buffer). */
        if (!wlr_output_test_state(o, state))
            state->tearing_page_flip = false;
    }
}

bool xd_present_vrr_reported(const xd_output *out) {
    /* Only where the monitor can: a client restating it must not fail. */
    return out->vrr != XD_VRR_OFF && out->wlr_output->adaptive_sync_supported;
}

void xd_present_vrr_set(xd_output *out, bool on) {
    if (on == xd_present_vrr_reported(out))
        return;
    /* On keeps "always" (monitors.ini's) if that's what it was. */
    out->vrr = !on ? XD_VRR_OFF : out->vrr == XD_VRR_ALWAYS ? XD_VRR_ALWAYS : XD_VRR_FULLSCREEN;
    out->vrr_rejected = false;
    wlr_output_schedule_frame(out->wlr_output);
}
