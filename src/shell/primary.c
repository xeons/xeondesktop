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
    watched *w;
    wl_list_for_each(w, &shell->primary->outputs, link) {
        if (!w->known || w->x != 0 || w->y != 0)
            continue;
        XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
            if (bar->output != w->output || bar == first)
                continue;
            wl_list_remove(&bar->link);
            wl_list_insert(&shell->taskbars, &bar->link);
            xd_taskbars_update(shell);
            XD_LIST_FOR_EACH(b, &shell->taskbars, xd_taskbar, link) {
                if (b->desktop != nullptr)
                    b->desktop->dirty = true;
            }
            xd_deskicons_relayout(shell);
            return;
        }
        return;
    }
}

static void logical_position(void *data, [[maybe_unused]] struct zxdg_output_v1 *o, int32_t x,
                             int32_t y) {
    watched *w = data;
    w->x = x;
    w->y = y;
    w->known = true;
    reorder(w->shell);
}

static void logical_size([[maybe_unused]] void *data, [[maybe_unused]] struct zxdg_output_v1 *o,
                         [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h) {}
static void done([[maybe_unused]] void *data, [[maybe_unused]] struct zxdg_output_v1 *o) {}
static void name([[maybe_unused]] void *data, [[maybe_unused]] struct zxdg_output_v1 *o,
                 [[maybe_unused]] const char *n) {}
static void description([[maybe_unused]] void *data, [[maybe_unused]] struct zxdg_output_v1 *o,
                        [[maybe_unused]] const char *d) {}

static const struct zxdg_output_v1_listener xdg_listener = {
    .logical_position = logical_position,
    .logical_size = logical_size,
    .done = done,
    .name = name,
    .description = description,
};

static void watch(xd_shell *shell, struct wl_output *output) {
    xd_primary *p = state(shell);
    if (p->manager == nullptr)
        return;
    watched *w;
    wl_list_for_each(w, &p->outputs, link) {
        if (w->output == output)
            return;
    }
    w = XD_NEW(watched);
    w->shell = shell;
    w->output = output;
    w->xdg = zxdg_output_manager_v1_get_xdg_output(p->manager, output);
    zxdg_output_v1_add_listener(w->xdg, &xdg_listener, w);
    wl_list_insert(p->outputs.prev, &w->link);
}

void xd_primary_bind(xd_shell *shell, struct wl_registry *reg, uint32_t name, uint32_t version) {
    xd_primary *p = state(shell);
    if (p->manager != nullptr)
        return;
    p->manager = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface,
                                  XD_MIN(version, 3u));
    /* Outputs that already have a taskbar. */
    XD_LIST_FOR_EACH(bar, &shell->taskbars, xd_taskbar, link) {
        watch(shell, bar->output);
    }
}

void xd_primary_track(xd_shell *shell, struct wl_output *output) { watch(shell, output); }

void xd_primary_forget(xd_shell *shell, struct wl_output *output) {
    if (shell->primary == nullptr)
        return;
    XD_LIST_FOR_EACH_SAFE(w, tmp, &shell->primary->outputs, watched, link) {
        if (w->output == output) {
            zxdg_output_v1_destroy(w->xdg);
            wl_list_remove(&w->link);
            xd_free(w);
        }
    }
}

void xd_primary_finish(xd_shell *shell) {
    xd_primary *p = shell->primary;
    if (p == nullptr)
        return;
    XD_LIST_FOR_EACH_SAFE(w, tmp, &p->outputs, watched, link) {
        zxdg_output_v1_destroy(w->xdg);
        wl_list_remove(&w->link);
        xd_free(w);
    }
    if (p->manager != nullptr)
        zxdg_output_manager_v1_destroy(p->manager);
    xd_free(p);
    shell->primary = nullptr;
}
