/* SPDX-License-Identifier: MIT
 * The primary monitor. Windows puts the desktop icons, and the only
 * taskbar when "Show the taskbar on all displays" is off, on the primary
 * monitor, which is always the one at (0,0) of the desktop's
 * coordinates. Here it's the same: the monitor at the layout's origin
 * (the compositor keeps the desktop starting there), learnt from
 * xdg-output. Its taskbar is kept first in xd_shell.taskbars, which is
 * what the rest of the shell means by "the first monitor's".
 *
 * Without this, the first monitor was whichever output the compositor
 * announced first, which on a real machine follows connector order and
 * on the headless backend is the last one.
 */
#include "shell.h"

typedef struct watched {
    struct wl_list link; /* xd_primary.outputs */
    xd_shell *shell;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg;
    int x, y;
    bool known;
    char *name; /* the connector's ("DP-1"), for Display Properties' Identify */
} watched;

struct xd_primary {
    struct zxdg_output_manager_v1 *manager;
    struct wl_list outputs; /* watched */
};

static xd_primary *state(xd_shell *shell) {
    if (shell->primary == nullptr) {
        shell->primary = XD_NEW(xd_primary);
        wl_list_init(&shell->primary->outputs);
    }
    return shell->primary;
}

/* Moves the taskbar of the monitor at (0,0) to the front of the list; if
 * that changes which monitor is the primary one, the taskbars ("all
 * displays" off) and the desktop icons follow. */
static void reorder(xd_shell *shell) {
    if (wl_list_empty(&shell->taskbars))
        return;
    xd_taskbar *first = XD_CONTAINER_OF(shell->taskbars.next, xd_taskbar, link);
    watched *watched_output;
    wl_list_for_each(watched_output, &shell->primary->outputs, link) {
        if (!watched_output->known || watched_output->x != 0 || watched_output->y != 0)
            continue;
        XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
            if (bar->output != watched_output->output || bar == first)
                continue;
            wl_list_remove(&bar->link);
            wl_list_insert(&shell->taskbars, &bar->link);
            xd_taskbars_update(shell);
            XD_LIST_FOR_EACH(taskbar, &shell->taskbars, xd_taskbar, link) {
                if (taskbar->desktop != nullptr)
                    taskbar->desktop->dirty = true;
            }
            xd_deskicons_relayout(shell);
            return;
        }
        return;
    }
}

static void logical_position(void *data, [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
                             int32_t x, int32_t y) {
    watched *watched_output = data;
    watched_output->x = x;
    watched_output->y = y;
    watched_output->known = true;
    reorder(watched_output->shell);
}

static void logical_size([[maybe_unused]] void *data,
                         [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
                         [[maybe_unused]] int32_t width, [[maybe_unused]] int32_t height) {}
static void done([[maybe_unused]] void *data, [[maybe_unused]] struct zxdg_output_v1 *xdg_output) {}
static void name(void *data, [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
                 const char *connector) {
    watched *watched_output = data;
    xd_free(watched_output->name);
    watched_output->name = xd_str_dup_cstr(xd_str_from_cstr(connector));
}
static void description([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zxdg_output_v1 *xdg_output,
                        [[maybe_unused]] const char *text) {}

static const struct zxdg_output_v1_listener xdg_listener = {
    .logical_position = logical_position,
    .logical_size = logical_size,
    .done = done,
    .name = name,
    .description = description,
};

static void watch(xd_shell *shell, struct wl_output *output) {
    xd_primary *primary = state(shell);
    if (primary->manager == nullptr)
        return;
    watched *watched_output;
    wl_list_for_each(watched_output, &primary->outputs, link) {
        if (watched_output->output == output)
            return;
    }
    watched_output = XD_NEW(watched);
    watched_output->shell = shell;
    watched_output->output = output;
    watched_output->xdg = zxdg_output_manager_v1_get_xdg_output(primary->manager, output);
    zxdg_output_v1_add_listener(watched_output->xdg, &xdg_listener, watched_output);
    wl_list_insert(primary->outputs.prev, &watched_output->link);
}

void xd_primary_bind(xd_shell *shell, struct wl_registry *reg, uint32_t name, uint32_t version) {
    xd_primary *primary = state(shell);
    if (primary->manager != nullptr)
        return;
    primary->manager =
        wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, XD_MIN(version, 3u));
    /* Outputs that already have a taskbar. */
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        watch(shell, bar->output);
    }
}

void xd_primary_track(xd_shell *shell, struct wl_output *output) { watch(shell, output); }

struct wl_output *xd_primary_output_named(xd_shell *shell, const char *output_name) {
    if (shell->primary == nullptr)
        return nullptr;
    watched *watched_output;
    wl_list_for_each(watched_output, &shell->primary->outputs, link) {
        if (watched_output->name != nullptr && strcmp(watched_output->name, output_name) == 0)
            return watched_output->output;
    }
    return nullptr;
}

void xd_primary_forget(xd_shell *shell, struct wl_output *output) {
    if (shell->primary == nullptr)
        return;
    XD_LIST_FOR_EACH_SAFE(watched_output, tmp, &shell->primary->outputs, watched, link) {
        if (watched_output->output == output) {
            zxdg_output_v1_destroy(watched_output->xdg);
            wl_list_remove(&watched_output->link);
            xd_free(watched_output->name);
            xd_free(watched_output);
        }
    }
}

void xd_primary_finish(xd_shell *shell) {
    xd_primary *primary = shell->primary;
    if (primary == nullptr)
        return;
    XD_LIST_FOR_EACH_SAFE(watched_output, tmp, &primary->outputs, watched, link) {
        zxdg_output_v1_destroy(watched_output->xdg);
        wl_list_remove(&watched_output->link);
        xd_free(watched_output->name);
        xd_free(watched_output);
    }
    if (primary->manager != nullptr)
        zxdg_output_manager_v1_destroy(primary->manager);
    xd_free(primary);
    shell->primary = nullptr;
}
