/* SPDX-License-Identifier: MIT
 * xd-globals [--sandboxed]: the Wayland globals a client gets, one
 * interface name per line. With --sandboxed, as a sandboxed program
 * would: connected, as Flatpak connects its programs, through a listening
 * socket made with security-context-v1 (engine "xd-test"). For the UI
 * tests of what sandboxed programs may use.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "security-context-v1-client-protocol.h"

typedef struct probe {
    struct wp_security_context_manager_v1 *security;
    bool print;
} probe;

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    probe *state = data;
    if (state->print)
        printf("%s\n", interface);
    else if (strcmp(interface, wp_security_context_manager_v1_interface.name) == 0)
        state->security = wl_registry_bind(
            registry, name, &wp_security_context_manager_v1_interface, version < 1 ? version : 1);
}

static void registry_global_remove([[maybe_unused]] void *data,
                                   [[maybe_unused]] struct wl_registry *registry,
                                   [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int list_globals(struct wl_display *display) {
    probe state = {.print = true};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    int ok = wl_display_roundtrip(display) >= 0;
    wl_registry_destroy(registry);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv) {
    bool sandboxed = argc > 1 && strcmp(argv[1], "--sandboxed") == 0;
    struct wl_display *display = wl_display_connect(nullptr);
    if (display == nullptr) {
        fprintf(stderr, "xd-globals: no Wayland display\n");
        return EXIT_FAILURE;
    }
    if (!sandboxed) {
        int status = list_globals(display);
        wl_display_disconnect(display);
        return status;
    }
    probe state = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    wl_display_roundtrip(display);
    if (state.security == nullptr) {
        fprintf(stderr, "xd-globals: no security-context-v1\n");
        return EXIT_FAILURE;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/xd-globals-%d",
             runtime != nullptr ? runtime : ".", (int)getpid());
    unlink(address.sun_path);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int closing[2];
    if (listener < 0 || bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 4) != 0 || pipe2(closing, O_CLOEXEC) != 0) {
        perror("xd-globals: socket");
        return EXIT_FAILURE;
    }
    /* The compositor listens on our socket until the pipe's other end
     * (kept here) closes. */
    struct wp_security_context_v1 *context =
        wp_security_context_manager_v1_create_listener(state.security, listener, closing[0]);
    wp_security_context_v1_set_sandbox_engine(context, "xd-test");
    wp_security_context_v1_set_app_id(context, "org.xeondesktop.SandboxTest");
    wp_security_context_v1_commit(context);
    wp_security_context_v1_destroy(context);
    wl_display_roundtrip(display);
    close(listener);
    close(closing[0]);
    struct wl_display *inside = wl_display_connect(address.sun_path);
    int status = EXIT_FAILURE;
    if (inside != nullptr) {
        status = list_globals(inside);
        wl_display_disconnect(inside);
    } else {
        fprintf(stderr, "xd-globals: cannot connect through the security context\n");
    }
    close(closing[1]);
    unlink(address.sun_path);
    wp_security_context_manager_v1_destroy(state.security);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return status;
}
