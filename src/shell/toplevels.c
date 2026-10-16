/* SPDX-License-Identifier: MIT
 * The window list, from wlr-foreign-toplevel-management.
 */
#include "../core/alloc.h"
#include "shell.h"

static void replace_string(char **dst, const char *src) {
    XD_FREE(dst);
    *dst = xd_str_dup_cstr(xd_str_from_cstr(src));
}

static void handle_title(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                         const char *title) {
    xd_toplevel *t = data;
    replace_string(&t->title, title);
}

static void handle_app_id(void *data,
                          [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                          const char *app_id) {
    xd_toplevel *t = data;
    replace_string(&t->app_id, app_id);
}

/* Like XP, every taskbar shows every window, so outputs are ignored for
 * now. "Show windows only on the taskbar where they're open" (Windows 8+)
 * will use these. */
static void handle_output_enter([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                                [[maybe_unused]] struct wl_output *output) {}

static void handle_output_leave([[maybe_unused]] void *data,
                                [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                                [[maybe_unused]] struct wl_output *output) {}

static void handle_state(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h,
                         struct wl_array *state) {
    xd_toplevel *t = data;
    t->activated = false;
    t->minimized = false;
    t->maximized = false;
    const uint32_t *v = state->data;
    size_t n = state->size / sizeof(uint32_t);
    for (size_t i = 0; i < n; i++) {
        switch (v[i]) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
            t->activated = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
            t->minimized = true;
            break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
            t->maximized = true;
            break;
        default:
            break;
        }
    }
}

static void handle_done(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h) {
    xd_toplevel *t = data;
    xd_taskbars_mark_dirty(t->shell);
}

static void handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
    xd_toplevel *t = data;
    xd_taskbars_forget(t->shell, t);
    wl_list_remove(&t->link);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    XD_FREE(&t->title);
    XD_FREE(&t->app_id);
    xd_taskbars_mark_dirty(t->shell);
    xd_free(t);
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

static void handle_toplevel(void *data,
                            [[maybe_unused]] struct zwlr_foreign_toplevel_manager_v1 *manager,
                            struct zwlr_foreign_toplevel_handle_v1 *handle) {
    xd_shell *shell = data;
    xd_toplevel *t = XD_NEW(xd_toplevel);
    t->shell = shell;
    t->handle = handle;
    wl_list_insert(shell->toplevels.prev, &t->link);
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, t);
}

static void handle_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {
    xd_shell *shell = data;
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
    shell->toplevel_manager = nullptr;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = handle_toplevel,
    .finished = handle_finished,
};

void xd_toplevels_init(xd_shell *shell) {
    if (shell->toplevel_manager != nullptr)
        zwlr_foreign_toplevel_manager_v1_add_listener(shell->toplevel_manager, &manager_listener,
                                                      shell);
}

/* Windows' rule: clicking the button of the active window minimizes it;
 * clicking any other button brings that window forward. */
void xd_toplevel_click(xd_toplevel *t) {
    if (t->activated && !t->minimized) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
        return;
    }
    if (t->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
    if (t->shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(t->handle, t->shell->seat);
}
