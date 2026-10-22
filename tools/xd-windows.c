/* SPDX-License-Identifier: MIT
 * xd-windows: lists a session's windows, for UI tests.
 *
 * One line per toplevel, from wlr-foreign-toplevel-management (what the
 * taskbar sees), tab-separated:
 *
 *   APP_ID <tab> TITLE <tab> STATES
 *
 * STATES is a comma-separated subset of activated, maximized, minimized
 * and fullscreen ("-" for none). Tabs and newlines in titles become spaces
 * so every window stays one line.
 */
#include <stdlib.h>
#include <wayland-client.h>

#include "../src/core/alloc.h"
#include "../src/core/str.h"
#include "../src/ui/protocols.h"

typedef struct window {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char *app_id;
    char *title;
    bool activated, maximized, minimized, fullscreen;
    bool closed;
} window;

typedef struct lister {
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    window windows[256];
    size_t count;
} lister;

static void replace(char **field, const char *value) {
    xd_free(*field);
    *field = xd_str_dup_cstr(xd_str_from_cstr(value));
}

static void handle_title(void *data,
                         [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *handle,
                         const char *title) {
    window *toplevel = data;
    replace(&toplevel->title, title);
}

static void handle_app_id(void *data,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *handle,
                          const char *app_id) {
    window *toplevel = data;
    replace(&toplevel->app_id, app_id);
}

static void handle_output_enter([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *handle,
                                [[maybe_unused]] struct wl_output *monitor) {}
static void handle_output_leave([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *handle,
                                [[maybe_unused]] struct wl_output *monitor) {}

static void handle_state(void *data,
                         [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *handle,
                         struct wl_array *states) {
    window *toplevel = data;
    toplevel->activated = toplevel->maximized = toplevel->minimized = toplevel->fullscreen = false;
    uint32_t *state;
    wl_array_for_each(state, states) {
        switch (*state) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            toplevel->activated = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            toplevel->maximized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            toplevel->minimized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
            toplevel->fullscreen = true;
            break;
        default:
            break;
        }
    }
}

static void handle_done([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *handle) {}

static void handle_closed(void *data,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *handle) {
    window *toplevel = data;
    toplevel->closed = true;
}

static void handle_parent([[maybe_unused]] void *data,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *handle,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *parent) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = handle_title,
    .app_id = handle_app_id,
    .output_enter = handle_output_enter,
    .output_leave = handle_output_leave,
    .state = handle_state,
    .done = handle_done,
    .closed = handle_closed,
    .parent = handle_parent,
};

static void manager_toplevel(void *data,
                             [[maybe_unused]] struct zwlr_foreign_toplevel_manager_v1 *manager,
                             struct zwlr_foreign_toplevel_handle_v1 *handle) {
    lister *listing = data;
    if (listing->count == XD_ARRAY_LEN(listing->windows)) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    window *toplevel = &listing->windows[listing->count++];
    toplevel->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, toplevel);
}

static void manager_finished([[maybe_unused]] void *data,
                             [[maybe_unused]] struct zwlr_foreign_toplevel_manager_v1 *manager) {}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = manager_toplevel,
    .finished = manager_finished,
};

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                   uint32_t version) {
    lister *listing = data;
    if (xd_str_eq(xd_str_from_cstr(iface),
                  xd_str_from_cstr(zwlr_foreign_toplevel_manager_v1_interface.name)))
        listing->manager = wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface,
                                            XD_MIN(version, 3u));
}

static void global_remove([[maybe_unused]] void *data, [[maybe_unused]] struct wl_registry *reg,
                          [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global, global_remove};

/* Prints a field on one line: tabs and newlines would split the record. */
static void print_field(const char *text) {
    for (; text != nullptr && *text != '\0'; text++)
        putchar(*text == '\t' || *text == '\n' || *text == '\r' ? ' ' : *text);
}

int main(void) {
    static lister listing;
    struct wl_display *display = wl_display_connect(nullptr);
    if (display == nullptr) {
        fprintf(stderr, "xd-windows: no Wayland compositor (is WAYLAND_DISPLAY set?)\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &registry_listener, &listing);
    wl_display_roundtrip(display);
    if (listing.manager == nullptr) {
        fprintf(stderr, "xd-windows: the compositor has no foreign-toplevel manager\n");
        wl_display_disconnect(display);
        return EXIT_FAILURE;
    }
    zwlr_foreign_toplevel_manager_v1_add_listener(listing.manager, &manager_listener, &listing);
    /* The first roundtrip announces the handles, the second their state. */
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    for (size_t i = 0; i < listing.count; i++) {
        const window *toplevel = &listing.windows[i];
        if (!toplevel->closed) {
            print_field(toplevel->app_id);
            putchar('\t');
            print_field(toplevel->title);
            putchar('\t');
            const char *states[4];
            size_t count = 0;
            if (toplevel->activated)
                states[count++] = "activated";
            if (toplevel->maximized)
                states[count++] = "maximized";
            if (toplevel->minimized)
                states[count++] = "minimized";
            if (toplevel->fullscreen)
                states[count++] = "fullscreen";
            if (count == 0)
                fputs("-", stdout);
            for (size_t j = 0; j < count; j++)
                printf("%s%s", j > 0 ? "," : "", states[j]);
            putchar('\n');
        }
        zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
        xd_free(toplevel->app_id);
        xd_free(toplevel->title);
    }
    zwlr_foreign_toplevel_manager_v1_stop(listing.manager);
    wl_display_roundtrip(display);
    wl_registry_destroy(reg);
    wl_display_disconnect(display);
    return EXIT_SUCCESS;
}
