/* SPDX-License-Identifier: MIT
 * The taskbar as a drop target (wl_data_device), as in XP:
 *
 * - files or applications dropped on Quick Launch become shortcuts where
 *   they land;
 * - a drag held over a task button brings that window forward, so the
 *   drop can go into it.
 */
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "shell.h"

#define URI_LIST "text/uri-list"
enum { HOVER_ACTIVATE_MS = 800 };

struct xd_dnd {
    xd_shell *shell;
    struct wl_data_device_manager *manager;
    struct wl_data_device *device;
    /* The newest offer and whether it has a URI list (its mime types come
     * before the enter or selection event that uses it). */
    struct wl_data_offer *pending;
    bool pending_uris;
    struct wl_data_offer *selection;
    /* The drag over us. */
    struct wl_data_offer *offer;
    bool uris;
    uint32_t serial;
    xd_taskbar *bar;
    double x, y; /* logical, bar-local */
    int hover_task;
    guint hover_timer;
};

/* ---- offers ------------------------------------------------------------------- */

static void offer_mime(void *data, struct wl_data_offer *offer, const char *mime) {
    xd_dnd *d = data;
    if (offer == d->pending && strcmp(mime, URI_LIST) == 0)
        d->pending_uris = true;
}

static void offer_source_actions([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_data_offer *o,
                                 [[maybe_unused]] uint32_t actions) {}

static void offer_action([[maybe_unused]] void *data, [[maybe_unused]] struct wl_data_offer *o,
                         [[maybe_unused]] uint32_t action) {}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void device_offer(void *data, [[maybe_unused]] struct wl_data_device *dev,
                         struct wl_data_offer *offer) {
    xd_dnd *d = data;
    d->pending = offer;
    d->pending_uris = false;
    wl_data_offer_add_listener(offer, &offer_listener, d);
}

/* ---- the drag over the taskbar ------------------------------------------------- */

static void stop_hover(xd_dnd *d) {
    if (d->hover_timer != 0)
        g_source_remove(d->hover_timer);
    d->hover_timer = 0;
    d->hover_task = -1;
}

static void forget_drag(xd_dnd *d) {
    stop_hover(d);
    if (d->bar != nullptr) {
        d->bar->drop_insert = -1;
        d->bar->dirty = true;
    }
    d->bar = nullptr;
    if (d->offer != nullptr)
        wl_data_offer_destroy(d->offer);
    d->offer = nullptr;
    d->uris = false;
}

static gboolean hover_activate(gpointer data) {
    xd_dnd *d = data;
    d->hover_timer = 0;
    xd_taskbar *bar = d->bar;
    if (bar == nullptr || d->hover_task < 0 || (size_t)d->hover_task >= bar->task_count)
        return G_SOURCE_REMOVE;
    xd_toplevel *t = bar->task_views[d->hover_task];
    if (t->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
    if (d->shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(t->handle, d->shell->seat);
    return G_SOURCE_REMOVE;
}

static int task_at(const xd_taskbar *bar, double lx) {
    int x = (int)(lx * bar->scale);
    for (size_t i = 0; i < bar->task_count; i++) {
        xd_rect r = bar->task_rects[i];
        if (x >= r.x && x < r.x + r.w)
            return (int)i;
    }
    return -1;
}

static void track(xd_dnd *d) {
    xd_taskbar *bar = d->bar;
    if (bar == nullptr || d->offer == nullptr)
        return;
    xd_taskbar_reveal(bar); /* Auto-hide */
    int insert = d->uris ? xd_taskbar_quick_insert_at(bar, d->x, d->y) : -1;
    if (insert != bar->drop_insert) {
        bar->drop_insert = insert;
        bar->dirty = true;
    }
    if (insert >= 0) {
        wl_data_offer_accept(d->offer, d->serial, URI_LIST);
        /* A shortcut to it: the dragged file stays where it is. */
        wl_data_offer_set_actions(d->offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    } else {
        wl_data_offer_accept(d->offer, d->serial, nullptr);
    }
    /* Held over a task button: that window comes forward. */
    int task = insert < 0 ? task_at(bar, d->x) : -1;
    if (task != d->hover_task) {
        stop_hover(d);
        d->hover_task = task;
        if (task >= 0)
            d->hover_timer = g_timeout_add(HOVER_ACTIVATE_MS, hover_activate, d);
    }
}

static void device_enter(void *data, [[maybe_unused]] struct wl_data_device *dev, uint32_t serial,
                         struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                         struct wl_data_offer *offer) {
    xd_dnd *d = data;
    forget_drag(d);
    d->offer = offer;
    d->uris = offer != nullptr && offer == d->pending && d->pending_uris;
    if (offer == d->pending)
        d->pending = nullptr;
    d->serial = serial;
    d->bar = xd_taskbar_for_surface(d->shell, surface);
    d->x = wl_fixed_to_double(x);
    d->y = wl_fixed_to_double(y);
    if (d->bar == nullptr && offer != nullptr)
        wl_data_offer_accept(offer, serial, nullptr);
    track(d);
}

static void device_leave(void *data, [[maybe_unused]] struct wl_data_device *dev) {
    forget_drag(data);
}

static void device_motion(void *data, [[maybe_unused]] struct wl_data_device *dev,
                          [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    xd_dnd *d = data;
    d->x = wl_fixed_to_double(x);
    d->y = wl_fixed_to_double(y);
    track(d);
}

/* The dropped URI list, read until the source closes the pipe. */
typedef struct drop {
    xd_shell *shell;
    int fd;
    GString *text;
    size_t at;
} drop;

static gboolean read_drop(int fd, [[maybe_unused]] GIOCondition cond, gpointer data) {
    drop *dr = data;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        g_string_append_len(dr->text, buf, n);
        return G_SOURCE_CONTINUE;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return G_SOURCE_CONTINUE;
    char **uris = g_uri_list_extract_uris(dr->text->str);
    size_t at = dr->at;
    for (char **u = uris; u != nullptr && *u != nullptr; u++) {
        char *path = g_filename_from_uri(*u, nullptr, nullptr);
        if (path != nullptr && xd_quicklaunch_add(dr->shell, at, path))
            at++;
        g_free(path);
    }
    g_strfreev(uris);
    g_string_free(dr->text, TRUE);
    close(dr->fd);
    g_free(dr);
    return G_SOURCE_REMOVE;
}

static void device_drop(void *data, [[maybe_unused]] struct wl_data_device *dev) {
    xd_dnd *d = data;
    xd_taskbar *bar = d->bar;
    int at = bar != nullptr ? bar->drop_insert : -1;
    if (d->offer != nullptr && d->uris && at >= 0) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            wl_data_offer_receive(d->offer, URI_LIST, fds[1]);
            close(fds[1]);
            drop *dr = g_new0(drop, 1);
            dr->shell = d->shell;
            dr->fd = fds[0];
            dr->text = g_string_new(nullptr);
            dr->at = (size_t)at;
            g_unix_fd_add(fds[0], (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_drop, dr);
            /* The source writes once we've flushed the request. */
            wl_display_flush(d->shell->display);
            if (wl_data_offer_get_version(d->offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION)
                wl_data_offer_finish(d->offer);
        }
    }
    forget_drag(d);
}

static void device_selection(void *data, [[maybe_unused]] struct wl_data_device *dev,
                             struct wl_data_offer *offer) {
    /* The clipboard: not ours to use, but its offer is ours to free. */
    xd_dnd *d = data;
    if (d->selection != nullptr)
        wl_data_offer_destroy(d->selection);
    d->selection = offer;
    if (offer == d->pending)
        d->pending = nullptr;
}

static const struct wl_data_device_listener device_listener = {
    .data_offer = device_offer,
    .enter = device_enter,
    .leave = device_leave,
    .motion = device_motion,
    .drop = device_drop,
    .selection = device_selection,
};

void xd_dnd_init(xd_shell *shell, struct wl_data_device_manager *manager) {
    if (manager == nullptr || shell->seat == nullptr)
        return;
    xd_dnd *d = XD_NEW(xd_dnd);
    d->shell = shell;
    d->manager = manager;
    d->hover_task = -1;
    d->device = wl_data_device_manager_get_data_device(manager, shell->seat);
    wl_data_device_add_listener(d->device, &device_listener, d);
    shell->dnd = d;
}

void xd_dnd_finish(xd_shell *shell) {
    xd_dnd *d = shell->dnd;
    if (d == nullptr)
        return;
    forget_drag(d);
    if (d->selection != nullptr)
        wl_data_offer_destroy(d->selection);
    if (wl_data_device_get_version(d->device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
        wl_data_device_release(d->device);
    else
        wl_data_device_destroy(d->device);
    wl_data_device_manager_destroy(d->manager);
    xd_free(d);
    shell->dnd = nullptr;
}
