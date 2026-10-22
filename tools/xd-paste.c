/* SPDX-License-Identifier: MIT
 * xd-paste: reads a session's clipboard, for UI tests, through
 * ext-data-control (no window needed).
 *
 *   xd-paste           the clipboard's types, one a line
 *   xd-paste TYPE      its data of that type, on standard output
 *
 * Exits 1 when the clipboard is empty or hasn't that type.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "../src/ui/protocols.h"

typedef struct offer_types {
    struct ext_data_control_offer_v1 *offer;
    char *types[64];
    size_t count;
} offer_types;

typedef struct paster {
    struct wl_seat *seat;
    struct ext_data_control_manager_v1 *manager;
    offer_types offers[8]; /* the latest ones offered */
    size_t next;
    struct ext_data_control_offer_v1 *selection;
} paster;

static void handle_offer(void *data, struct ext_data_control_offer_v1 *offer,
                         const char *mime_type) {
    paster *clipboard = data;
    for (size_t i = 0; i < 8; i++) {
        offer_types *types = &clipboard->offers[i];
        if (types->offer == offer && types->count < 64)
            types->types[types->count++] = strdup(mime_type);
    }
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = handle_offer,
};

static void handle_data_offer(void *data,
                              [[maybe_unused]] struct ext_data_control_device_v1 *device,
                              struct ext_data_control_offer_v1 *offer) {
    paster *clipboard = data;
    offer_types *types = &clipboard->offers[clipboard->next++ % 8];
    types->offer = offer;
    types->count = 0;
    ext_data_control_offer_v1_add_listener(offer, &offer_listener, clipboard);
}

static void handle_selection(void *data,
                             [[maybe_unused]] struct ext_data_control_device_v1 *device,
                             struct ext_data_control_offer_v1 *offer) {
    paster *clipboard = data;
    clipboard->selection = offer;
}

static void handle_finished([[maybe_unused]] void *data,
                            [[maybe_unused]] struct ext_data_control_device_v1 *device) {}

static void handle_primary([[maybe_unused]] void *data,
                           [[maybe_unused]] struct ext_data_control_device_v1 *device,
                           [[maybe_unused]] struct ext_data_control_offer_v1 *offer) {}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = handle_data_offer,
    .selection = handle_selection,
    .finished = handle_finished,
    .primary_selection = handle_primary,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
                          const char *interface, [[maybe_unused]] uint32_t version) {
    paster *clipboard = data;
    if (strcmp(interface, wl_seat_interface.name) == 0 && clipboard->seat == nullptr)
        clipboard->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
        clipboard->manager =
            wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1);
}

static void handle_global_remove([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_registry *registry,
                                 [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

int main(int argc, char **argv) {
    struct wl_display *display = wl_display_connect(nullptr);
    if (display == nullptr) {
        fprintf(stderr, "xd-paste: cannot connect to the compositor\n");
        return 2;
    }
    static paster clipboard;
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &clipboard);
    wl_display_roundtrip(display);
    if (clipboard.seat == nullptr || clipboard.manager == nullptr) {
        fprintf(stderr, "xd-paste: no ext-data-control\n");
        return 2;
    }
    struct ext_data_control_device_v1 *device =
        ext_data_control_manager_v1_get_data_device(clipboard.manager, clipboard.seat);
    ext_data_control_device_v1_add_listener(device, &device_listener, &clipboard);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    const offer_types *types = nullptr;
    for (size_t i = 0; i < 8; i++) {
        if (clipboard.selection != nullptr && clipboard.offers[i].offer == clipboard.selection)
            types = &clipboard.offers[i];
    }
    if (types == nullptr)
        return 1;
    if (argc < 2) {
        for (size_t i = 0; i < types->count; i++)
            printf("%s\n", types->types[i]);
        return 0;
    }
    bool offered = false;
    for (size_t i = 0; i < types->count; i++)
        offered = offered || strcmp(types->types[i], argv[1]) == 0;
    if (!offered)
        return 1;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
        return 2;
    ext_data_control_offer_v1_receive(clipboard.selection, argv[1], pipe_fds[1]);
    wl_display_flush(display);
    close(pipe_fds[1]);
    char chunk[65536];
    for (;;) {
        ssize_t count = read(pipe_fds[0], chunk, sizeof(chunk));
        if (count <= 0)
            break;
        fwrite(chunk, 1, (size_t)count, stdout);
    }
    close(pipe_fds[0]);
    wl_display_disconnect(display);
    return 0;
}
