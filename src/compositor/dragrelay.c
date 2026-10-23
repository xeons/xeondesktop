/* SPDX-License-Identifier: MIT
 * Drags from outside the desktop (xd-drag-relay-v1): VirtualBox's
 * host-to-guest drag and drop. The relay, xd-vboxdnd, hears from the host
 * that a drag entered the VM's window and where it goes; it starts a
 * Wayland drag with its own data source, and the compositor lets it
 * without a button held (no serial to check), holds the drag's button
 * for it, moves the pointer where the host says, and drops when told.
 *
 * The other way (guest to host): drags programs here start are told to
 * the relay, which hands their data to the host when the user drops
 * outside the VM's window, then cancels them here.
 *
 * The relay is started here, on a connection of its own (trust.c), and
 * only that connection sees the global: inside a VirtualBox guest (its
 * /dev/vboxuser), never in a UI test's session unless the test runs it
 * against a scripted host ($XD_VBOXDND_FAKE).
 */
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "../core/alloc.h"
#include "../core/str.h"
#include "server.h"
#include "xd-drag-relay-v1-protocol.h"

#define VBOX_DEVICE "/dev/vboxuser"

static uint32_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

/* A position in the desktop's device pixels as a layout position: on the
 * monitor whose device-pixel box (its logical position times its scale,
 * its size in pixels) holds it. */
static bool layout_position(xd_server *server, int x, int y, double *lx, double *ly) {
    struct wlr_output_layout_output *placed;
    wl_list_for_each(placed, &server->output_layout->outputs, link) {
        struct wlr_output *output = placed->output;
        double scale = output->scale > 0 ? (double)output->scale : 1.0;
        double left = placed->x * scale, top = placed->y * scale;
        if (x >= left && x < left + output->width && y >= top && y < top + output->height) {
            *lx = placed->x + (x - left) / scale;
            *ly = placed->y + (y - top) / scale;
            return true;
        }
    }
    return false;
}

static void handle_move([[maybe_unused]] struct wl_client *client, struct wl_resource *resource,
                        int32_t x, int32_t y) {
    xd_server *server = wl_resource_get_user_data(resource);
    double lx, ly;
    if (!layout_position(server, x, y, &lx, &ly))
        return;
    wlr_cursor_warp_closest(server->cursor, nullptr, lx, ly);
    xd_input_pointer_moved(server);
}

static void handle_refocus([[maybe_unused]] struct wl_client *client,
                           struct wl_resource *resource) {
    xd_server *server = wl_resource_get_user_data(resource);
    if (!server->relay_dragging)
        return;
    /* The drag's grab takes the focus off what's under it; the motion
     * then gives it back, with a new offer. */
    wlr_seat_pointer_notify_clear_focus(server->seat);
    xd_input_pointer_moved(server);
}

static void handle_drop([[maybe_unused]] struct wl_client *client, struct wl_resource *resource) {
    xd_server *server = wl_resource_get_user_data(resource);
    if (!server->relay_dragging)
        return;
    server->relay_dragging = false;
    /* The drag's grab takes the release and drops (or, refused, ends). */
    wlr_seat_pointer_notify_button(server->seat, now_ms(), BTN_LEFT,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void handle_outgoing_receive([[maybe_unused]] struct wl_client *client,
                                    struct wl_resource *resource, const char *mime_type,
                                    int32_t fd) {
    xd_server *server = wl_resource_get_user_data(resource);
    struct wlr_drag *drag = server->outgoing_drag;
    if (drag == nullptr || drag->source == nullptr) {
        close(fd);
        return;
    }
    /* The source writes and closes it, as for an offer's receive. */
    wlr_data_source_send(drag->source, mime_type, fd);
}

static void handle_outgoing_cancel([[maybe_unused]] struct wl_client *client,
                                   struct wl_resource *resource) {
    xd_server *server = wl_resource_get_user_data(resource);
    struct wlr_drag *drag = server->outgoing_drag;
    /* Its program hears "cancelled", and the drag ends with its source. */
    if (drag != nullptr && drag->source != nullptr)
        wlr_data_source_destroy(drag->source);
}

static void handle_relay_destroy([[maybe_unused]] struct wl_client *client,
                                 struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xd_drag_relay_v1_interface relay_impl = {
    .move = handle_move,
    .refocus = handle_refocus,
    .drop = handle_drop,
    .outgoing_receive = handle_outgoing_receive,
    .outgoing_cancel = handle_outgoing_cancel,
    .destroy = handle_relay_destroy,
};

static void relay_resource_destroyed(struct wl_resource *resource) {
    xd_server *server = wl_resource_get_user_data(resource);
    if (server->relay_resource == resource)
        server->relay_resource = nullptr;
}

static void handle_get_relay(struct wl_client *client, struct wl_resource *manager, uint32_t id) {
    xd_server *server = wl_resource_get_user_data(manager);
    struct wl_resource *resource = wl_resource_create(client, &xd_drag_relay_v1_interface,
                                                      wl_resource_get_version(manager), id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &relay_impl, server, relay_resource_destroyed);
    server->relay_resource = resource;
}

static void handle_manager_destroy([[maybe_unused]] struct wl_client *client,
                                   struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct xd_drag_relay_manager_v1_interface manager_impl = {
    .get_relay = handle_get_relay,
    .destroy = handle_manager_destroy,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource =
        wl_resource_create(client, &xd_drag_relay_manager_v1_interface, (int)version, id);
    if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

bool xd_relay_global_private(const xd_server *server, const struct wl_global *global) {
    return global != nullptr && global == server->drag_relay;
}

bool xd_relay_client_allowed(const xd_server *server, const struct wl_client *client) {
    return xd_trust_is(&server->relay_client, client);
}

bool xd_relay_start_drag(xd_server *server, struct wlr_drag *drag) {
    if (server->relay_resource == nullptr || drag->seat_client == nullptr ||
        !xd_trust_is(&server->relay_client, drag->seat_client->client))
        return false;
    /* No button is held: the drag takes the pointer, then the compositor
     * presses one for it, which the drag's grab keeps to itself, so that
     * drop's release ends it. */
    wlr_seat_start_pointer_drag(server->seat, drag, 0);
    wlr_seat_pointer_notify_button(server->seat, now_ms(), BTN_LEFT,
                                   WL_POINTER_BUTTON_STATE_PRESSED);
    wlr_seat_pointer_notify_frame(server->seat);
    server->relay_dragging = true;
    xd_input_pointer_moved(server);
    return true;
}

bool xd_relay_cancel(xd_server *server) {
    if (!server->relay_dragging)
        return false;
    wlr_log(WLR_INFO, "drag relay: the host's drag cancelled here");
    /* The relay hears "cancelled"; the drag ends with its source, and the
     * button held for it is let go (xd_relay_drag_ended). */
    struct wlr_drag *drag = server->seat->drag;
    if (drag != nullptr && drag->source != nullptr)
        wlr_data_source_destroy(drag->source);
    else
        xd_relay_drag_ended(server);
    return true;
}

void xd_relay_drag_started(xd_server *server, struct wlr_drag *drag) {
    if (server->relay_resource == nullptr || drag->source == nullptr || server->relay_dragging ||
        (drag->seat_client != nullptr && xd_trust_is(&server->relay_client, drag->seat_client->client)))
        return;
    server->outgoing_drag = drag;
    char **mime_type;
    wl_array_for_each(mime_type, &drag->source->mime_types)
        xd_drag_relay_v1_send_outgoing_type(server->relay_resource, *mime_type);
    xd_drag_relay_v1_send_outgoing_started(server->relay_resource, (uint32_t)drag->source->actions);
}

void xd_relay_drag_ended(xd_server *server) {
    if (server->outgoing_drag != nullptr) {
        server->outgoing_drag = nullptr;
        if (server->relay_resource != nullptr)
            xd_drag_relay_v1_send_outgoing_ended(server->relay_resource);
    }
    if (!server->relay_dragging)
        return;
    /* Ended without a drop (the relay took its data source back): the
     * button it held is let go, though there's no drag left to take it. */
    server->relay_dragging = false;
    wlr_seat_pointer_notify_button(server->seat, now_ms(), BTN_LEFT,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    wlr_seat_pointer_notify_frame(server->seat);
}

void xd_relay_start(xd_server *server) {
    if (server->config.vboxdnd == nullptr || server->terminating)
        return;
    const char *fake = getenv("XD_VBOXDND_FAKE");
    bool scripted = fake != nullptr && fake[0] != '\0';
    /* A UI test's session never talks to a real VirtualBox host. */
    if (!scripted && (server->config.virtual_input || access(VBOX_DEVICE, R_OK | W_OK) != 0))
        return;
    char *program = xd_str_dup_cstr(xd_str_from_cstr(server->config.vboxdnd));
    char option[] = "--fake-host";
    char *script = scripted ? xd_str_dup_cstr(xd_str_from_cstr(fake)) : nullptr;
    char *argv[] = {program, scripted ? option : nullptr, script, nullptr};
    server->relay_pid = xd_trust_spawn(server, argv, &server->relay_client);
    if (server->relay_pid < 0)
        wlr_log(WLR_ERROR, "cannot start %s", program);
    xd_free(script);
    xd_free(program);
}

bool xd_relay_child_exited(xd_server *server, pid_t pid, int status) {
    if (pid <= 0 || pid != server->relay_pid)
        return false;
    server->relay_pid = 0;
    /* It exits quietly when the host has no drag and drop; it isn't
     * restarted, a crash included (logged). */
    if (WIFSIGNALED(status))
        wlr_log(WLR_ERROR, "xd-vboxdnd killed by signal %d", WTERMSIG(status));
    return true;
}

void xd_relay_init(xd_server *server) {
    server->drag_relay = wl_global_create(server->display, &xd_drag_relay_manager_v1_interface, 1,
                                          server, bind_manager);
}
