/* SPDX-License-Identifier: MIT
 * Desktop protocols the shell, settings and other tools rely on:
 *
 * - wlr-output-management: resolution, refresh, scale, position and
 *   enabling outputs (wlr-randr, kanshi, Display Properties' Settings tab);
 * - wlr-output-power-management: turning monitors off and on (screen
 *   savers, swayidle);
 * - ext-idle-notify and idle-inhibit: idle timers for screen savers, held
 *   off by video players and the like;
 * - xdg-activation: apps bringing their own windows forward.
 */
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "server.h"

/* ---- output management ---------------------------------------------------------- */

/* Tells output management clients how outputs are set up now. */
void xd_output_config_changed(xd_server *server) {
    if (server->output_manager == nullptr)
        return;
    struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
    if (config == nullptr)
        return;
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(config, out->wlr_output);
        if (head == nullptr)
            continue;
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
        if (!wlr_box_empty(&box)) {
            head->state.x = box.x;
            head->state.y = box.y;
        }
    }
    wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

static void place_in_layout(xd_server *server, struct wlr_output *output, int x, int y) {
    bool was_in = wlr_output_layout_get(server->output_layout, output) != nullptr;
    struct wlr_output_layout_output *lo =
        wlr_output_layout_add(server->output_layout, output, x, y);
    if (!was_in && lo != nullptr) {
        struct wlr_scene_output *so = wlr_scene_get_scene_output(server->scene, output);
        if (so == nullptr)
            so = wlr_scene_output_create(server->scene, output);
        wlr_scene_output_layout_add_output(server->scene_layout, lo, so);
    }
}

static void apply_config(xd_server *server, struct wlr_output_configuration_v1 *config,
                         bool test_only) {
    bool ok = true;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_head_v1_state_apply(&head->state, &st);
        bool head_ok = test_only ? wlr_output_test_state(head->state.output, &st)
                                 : wlr_output_commit_state(head->state.output, &st);
        wlr_output_state_finish(&st);
        if (!head_ok) {
            wlr_log(WLR_ERROR, "output %s: configuration rejected", head->state.output->name);
            ok = false;
            continue;
        }
        if (test_only)
            continue;
        if (head->state.enabled)
            place_in_layout(server, head->state.output, head->state.x, head->state.y);
        else
            wlr_output_layout_remove(server->output_layout, head->state.output);
    }
    if (ok)
        wlr_output_configuration_v1_send_succeeded(config);
    else
        wlr_output_configuration_v1_send_failed(config);
    wlr_output_configuration_v1_destroy(config);
    if (!test_only)
        xd_output_config_changed(server);
}

static void handle_output_apply(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, output_apply);
    apply_config(server, data, false);
}

static void handle_output_test(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, output_test);
    apply_config(server, data, true);
}

/* ---- output power -------------------------------------------------------------- */

static void handle_output_power(struct wl_listener *listener, void *data) {
    (void)listener;
    const struct wlr_output_power_v1_set_mode_event *e = data;
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, e->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
    if (!wlr_output_commit_state(e->output, &st))
        wlr_log(WLR_ERROR, "output %s: cannot change power", e->output->name);
    wlr_output_state_finish(&st);
}

/* ---- idle ---------------------------------------------------------------------- */

typedef struct xd_inhibitor {
    xd_server *server;
    struct wl_listener destroy;
} xd_inhibitor;

static void update_inhibited(xd_server *server) {
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, server->idle_inhibitors > 0);
}

static void handle_inhibitor_destroy(struct wl_listener *listener, [[maybe_unused]] void *data) {
    xd_inhibitor *in = XD_CONTAINER_OF(listener, xd_inhibitor, destroy);
    xd_server *server = in->server;
    server->idle_inhibitors--;
    xd_unlisten(&in->destroy);
    xd_free(in);
    update_inhibited(server);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, new_idle_inhibitor);
    struct wlr_idle_inhibitor_v1 *inhibitor = data;
    xd_inhibitor *in = XD_NEW(xd_inhibitor);
    in->server = server;
    XD_LISTEN(&inhibitor->events.destroy, in, destroy, handle_inhibitor_destroy);
    server->idle_inhibitors++;
    update_inhibited(server);
}

void xd_idle_activity(xd_server *server) {
    if (server->idle_notifier != nullptr)
        wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
}

/* ---- activation ---------------------------------------------------------------- */

static void handle_request_activate(struct wl_listener *listener, void *data) {
    xd_server *server = XD_CONTAINER_OF(listener, xd_server, request_activate);
    const struct wlr_xdg_activation_v1_request_activate_event *e = data;
    struct wlr_xdg_toplevel *top = wlr_xdg_toplevel_try_from_wlr_surface(e->surface);
    struct wlr_scene_tree *content = top != nullptr ? top->base->data : nullptr;
    xd_view *view = content != nullptr ? xd_view_from_node(&content->node) : nullptr;
    if (view != nullptr && view->mapped && !server->locked)
        xd_view_focus(view);
}

void xd_protocols_init(xd_server *server) {
    server->output_manager = wlr_output_manager_v1_create(server->display);
    XD_LISTEN(&server->output_manager->events.apply, server, output_apply, handle_output_apply);
    XD_LISTEN(&server->output_manager->events.test, server, output_test, handle_output_test);

    struct wlr_output_power_manager_v1 *power = wlr_output_power_manager_v1_create(server->display);
    XD_LISTEN(&power->events.set_mode, server, output_power, handle_output_power);

    server->idle_notifier = wlr_idle_notifier_v1_create(server->display);
    struct wlr_idle_inhibit_manager_v1 *inhibit = wlr_idle_inhibit_v1_create(server->display);
    XD_LISTEN(&inhibit->events.new_inhibitor, server, new_idle_inhibitor, handle_new_inhibitor);

    struct wlr_xdg_activation_v1 *activation = wlr_xdg_activation_v1_create(server->display);
    XD_LISTEN(&activation->events.request_activate, server, request_activate,
              handle_request_activate);
}
