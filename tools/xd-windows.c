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

static void handle_title(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                         const char *title) {
    window *w = data;
    replace(&w->title, title);
}

static void handle_app_id(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                          const char *app_id) {
    window *w = data;
    replace(&w->app_id, app_id);
}

static void handle_output_enter([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                                [[maybe_unused]] struct wl_output *o) {}
static void handle_output_leave([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                                [[maybe_unused]] struct wl_output *o) {}

static void handle_state(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                         struct wl_array *states) {
    window *w = data;
    w->activated = w->maximized = w->minimized = w->fullscreen = false;
    uint32_t *s;
    wl_array_for_each(s, states) {
        switch (*s) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            w->activated = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            w->maximized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            w->minimized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
            w->fullscreen = true;
            break;
        default:
            break;
        }
    }
}

static void handle_done([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h) {}

static void handle_closed(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h) {
    window *w = data;
    w->closed = true;
}

static void handle_parent([[maybe_unused]] void *data,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
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

static void manager_toplevel(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_manager_v1 *m,
                             struct zwlr_foreign_toplevel_handle_v1 *handle) {
    lister *l = data;
    if (l->count == XD_ARRAY_LEN(l->windows)) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    window *w = &l->windows[l->count++];
    w->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, w);
}

static void manager_finished([[maybe_unused]] void *data,
                             [[maybe_unused]] struct zwlr_foreign_toplevel_manager_v1 *m) {}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = manager_toplevel,
    .finished = manager_finished,
};

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                   uint32_t version) {
    lister *l = data;
    if (xd_str_eq(xd_str_from_cstr(iface),
                  xd_str_from_cstr(zwlr_foreign_toplevel_manager_v1_interface.name)))
        l->manager = wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface,
                                      XD_MIN(version, 3u));
}

static void global_remove([[maybe_unused]] void *data, [[maybe_unused]] struct wl_registry *reg,
                          [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {global, global_remove};

/* Prints a field on one line: tabs and newlines would split the record. */
static void print_field(const char *s) {
    for (; s != nullptr && *s != '\0'; s++)
        putchar(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
}

int main(void) {
    static lister l;
    struct wl_display *display = wl_display_connect(nullptr);
    if (display == nullptr) {
        fprintf(stderr, "xd-windows: no Wayland compositor (is WAYLAND_DISPLAY set?)\n");
        return EXIT_FAILURE;
    }
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &registry_listener, &l);
    wl_display_roundtrip(display);
    if (l.manager == nullptr) {
        fprintf(stderr, "xd-windows: the compositor has no foreign-toplevel manager\n");
        wl_display_disconnect(display);
        return EXIT_FAILURE;
    }
    zwlr_foreign_toplevel_manager_v1_add_listener(l.manager, &manager_listener, &l);
    /* The first roundtrip announces the handles, the second their state. */
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    for (size_t i = 0; i < l.count; i++) {
        const window *w = &l.windows[i];
        if (!w->closed) {
            print_field(w->app_id);
            putchar('\t');
            print_field(w->title);
            putchar('\t');
            const char *states[4];
            size_t n = 0;
            if (w->activated)
                states[n++] = "activated";
            if (w->maximized)
                states[n++] = "maximized";
            if (w->minimized)
                states[n++] = "minimized";
            if (w->fullscreen)
                states[n++] = "fullscreen";
            if (n == 0)
                fputs("-", stdout);
            for (size_t j = 0; j < n; j++)
                printf("%s%s", j > 0 ? "," : "", states[j]);
            putchar('\n');
        }
        zwlr_foreign_toplevel_handle_v1_destroy(w->handle);
        xd_free(w->app_id);
        xd_free(w->title);
    }
    zwlr_foreign_toplevel_manager_v1_stop(l.manager);
    wl_display_roundtrip(display);
    wl_registry_destroy(reg);
    wl_display_disconnect(display);
    return EXIT_SUCCESS;
}
