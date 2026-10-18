/* SPDX-License-Identifier: MIT
 * The window list, from wlr-foreign-toplevel-management.
 */
#include <sys/timerfd.h>

#include <glib.h>

#include "../core/alloc.h"
#include "apps.h"
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
    if (t->app_id != nullptr && strcmp(t->app_id, app_id) == 0)
        return;
    replace_string(&t->app_id, app_id);
    g_free(t->icon);
    g_free(t->app_name);
    g_free(t->desktop_id);
    xd_app_for_window_id(app_id, &t->desktop_id, &t->icon, &t->app_name);
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
    if (t->activated) {
        t->attention = false;
        t->flashes = 0;
        t->active_seq = ++t->shell->active_seq;
    }
}

static void handle_done(void *data, [[maybe_unused]] struct zwlr_foreign_toplevel_handle_v1 *h) {
    xd_toplevel *t = data;
    xd_taskbars_mark_dirty(t->shell);
}

static void forget_hidden(xd_shell *shell, xd_toplevel *t) {
    for (size_t i = 0; i < shell->desktop_hidden_n; i++) {
        if (shell->desktop_hidden[i] == t) {
            shell->desktop_hidden[i] = shell->desktop_hidden[--shell->desktop_hidden_n];
            return;
        }
    }
}

static void handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
    xd_toplevel *t = data;
    forget_hidden(t->shell, t);
    xd_taskbars_forget(t->shell, t);
    wl_list_remove(&t->link);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    XD_FREE(&t->title);
    XD_FREE(&t->app_id);
    g_free(t->icon);
    g_free(t->app_name);
    g_free(t->desktop_id);
    g_free(t->identifier);
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
    t->id = ++shell->next_toplevel_id;
    wl_list_insert(shell->toplevels.prev, &t->link);
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, t);
    /* Which window it is to the capture protocols, for its thumbnail. */
    if (shell->arranger != nullptr && xd_window_arranger_v1_get_version(shell->arranger) >=
                                          XD_WINDOW_ARRANGER_V1_IDENTIFY_SINCE_VERSION)
        xd_window_arranger_v1_identify(shell->arranger, handle);
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

/* Like Windows' default (ForegroundFlashCount = 7): seven flashes about
 * twice a second, then the button stays lit. */
enum { FLASH_HALF_CYCLES = 14, FLASH_INTERVAL_NS = 530000000 };

static void arm_flash_timer(xd_shell *shell, bool on) {
    if (shell->flash_timer_fd < 0)
        return;
    struct itimerspec spec = {0};
    if (on)
        spec.it_value = spec.it_interval = (struct timespec){.tv_nsec = FLASH_INTERVAL_NS};
    timerfd_settime(shell->flash_timer_fd, 0, &spec, nullptr);
}

void xd_toplevel_attention(xd_shell *shell, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    XD_LIST_FOR_EACH(t, &shell->toplevels, xd_toplevel, link) {
        if (t->handle == handle && !t->activated) {
            t->attention = true;
            t->flashes = FLASH_HALF_CYCLES;
            arm_flash_timer(shell, true);
            xd_taskbars_mark_dirty(shell);
        }
    }
}

void xd_toplevels_flash_tick(xd_shell *shell) {
    bool more = false;
    XD_LIST_FOR_EACH(t, &shell->toplevels, xd_toplevel, link) {
        if (t->attention && t->flashes > 0) {
            t->flashes--;
            more = more || t->flashes > 0;
            xd_taskbars_mark_dirty(shell);
        }
    }
    if (!more)
        arm_flash_timer(shell, false);
}

void xd_show_desktop_toggle(xd_shell *shell) {
    bool any_visible = false;
    XD_LIST_FOR_EACH(t, &shell->toplevels, xd_toplevel, link) {
        any_visible = any_visible || !t->minimized;
    }
    /* Second click with nothing shown since: bring back what we hid. */
    if (!any_visible && shell->desktop_hidden_n > 0) {
        for (size_t i = 0; i < shell->desktop_hidden_n; i++)
            zwlr_foreign_toplevel_handle_v1_unset_minimized(shell->desktop_hidden[i]->handle);
        shell->desktop_hidden_n = 0;
        return;
    }
    shell->desktop_hidden_n = 0;
    XD_LIST_FOR_EACH(t, &shell->toplevels, xd_toplevel, link) {
        if (t->minimized)
            continue;
        if (shell->desktop_hidden_n == shell->desktop_hidden_cap) {
            shell->desktop_hidden_cap = shell->desktop_hidden_cap == 0 ? 8 : shell->desktop_hidden_cap * 2;
            shell->desktop_hidden = xd_realloc_array(shell->desktop_hidden, shell->desktop_hidden_cap,
                                                     sizeof(*shell->desktop_hidden));
        }
        shell->desktop_hidden[shell->desktop_hidden_n++] = t;
        zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
    }
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
