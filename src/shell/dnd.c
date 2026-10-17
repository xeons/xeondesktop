/* SPDX-License-Identifier: MIT
 * The shell's wl_data_device: the taskbar as a drop target, as in XP:
 *
 * - files or applications dropped on Quick Launch become shortcuts where
 *   they land;
 * - a drag held over a task button brings that window forward, so the
 *   drop can go into it;
 *
 * the desktop as a drop target (into the Desktop folder, or onto a folder
 * or the Recycle Bin under the pointer) and its icons as a drag source;
 * and the clipboard's files for the desktop icons' Cut, Copy and Paste,
 * in the format file managers use (x-special/gnome-copied-files, with a
 * URI list besides).
 */
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "shell.h"

#define URI_LIST "text/uri-list"
#define GNOME_FILES "x-special/gnome-copied-files"
enum { HOVER_ACTIVATE_MS = 800 };

struct xd_dnd {
    xd_shell *shell;
    struct wl_data_device_manager *manager;
    struct wl_data_device *device;
    /* The newest offer and whether it has a URI list (its mime types come
     * before the enter or selection event that uses it). */
    struct wl_data_offer *pending;
    bool pending_uris, pending_gnome;
    struct wl_data_offer *selection;
    bool selection_uris, selection_gnome;
    /* Our own clipboard (the desktop's Cut or Copy). */
    struct wl_data_source *source;
    char **source_uris;
    bool source_cut;
    /* Our drag (desktop icons). */
    struct wl_data_source *drag_source;
    char **drag_uris;
    /* The drag over us. */
    struct wl_data_offer *offer;
    bool uris;
    uint32_t serial;
    xd_taskbar *bar;
    xd_desktop *desk;   /* or over a desktop */
    uint32_t action;    /* the action the compositor chose */
    double x, y; /* logical, surface-local */
    int hover_task;
    guint hover_timer;
};

/* ---- offers ------------------------------------------------------------------- */

static void offer_mime(void *data, struct wl_data_offer *offer, const char *mime) {
    xd_dnd *d = data;
    if (offer == d->pending && strcmp(mime, URI_LIST) == 0)
        d->pending_uris = true;
    if (offer == d->pending && strcmp(mime, GNOME_FILES) == 0)
        d->pending_gnome = true;
}

static void offer_source_actions([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_data_offer *o,
                                 [[maybe_unused]] uint32_t actions) {}

static void offer_action(void *data, struct wl_data_offer *o, uint32_t action) {
    xd_dnd *d = data;
    if (o == d->offer)
        d->action = action;
}

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
    d->pending_gnome = false;
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
    if (d->desk != nullptr)
        xd_deskicons_drag_over(d->shell, d->desk, -1, -1, false);
    d->desk = nullptr;
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
    if (d->desk != nullptr && d->offer != nullptr) {
        xd_drop_kind k = d->uris ? xd_deskicons_drag_over(d->shell, d->desk, d->x, d->y,
                                                          d->drag_source != nullptr)
                                 : XD_DROP_NONE;
        if (k == XD_DROP_NONE) {
            wl_data_offer_accept(d->offer, d->serial, nullptr);
            wl_data_offer_set_actions(d->offer, 0, 0);
        } else {
            wl_data_offer_accept(d->offer, d->serial, URI_LIST);
            /* Files move within the drive, as Windows does on one disk; to
             * the Recycle Bin they always move. */
            uint32_t move = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
            uint32_t copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
            wl_data_offer_set_actions(d->offer, k == XD_DROP_TRASH ? move : copy | move, move);
        }
        return;
    }
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
    d->desk = d->bar == nullptr ? xd_desktop_for_surface(d->shell, surface) : nullptr;
    d->action = 0;
    d->x = wl_fixed_to_double(x);
    d->y = wl_fixed_to_double(y);
    if (d->bar == nullptr && d->desk == nullptr && offer != nullptr)
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
/* A drop's offer is finished once its data has been read: a source may
 * stop sending when it hears the drag is over. */
static void finish_offer(struct wl_data_offer *offer) {
    if (wl_data_offer_get_version(offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION)
        wl_data_offer_finish(offer);
    wl_data_offer_destroy(offer);
}

typedef struct drop {
    xd_shell *shell;
    struct wl_data_offer *offer; /* ours until read */
    int fd;
    GString *text;
    size_t at;
} drop;

static gboolean read_drop(int fd, [[maybe_unused]] GIOCondition cond, gpointer data) {
    drop *dr = data;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0)
        g_string_append_len(dr->text, buf, n);
    if ((n > 0 && dr->text->len < (1u << 20)) || (n < 0 && (errno == EINTR || errno == EAGAIN)))
        return G_SOURCE_CONTINUE;
    /* The end, or a list too long to be one: dropped either way. */
    char **uris = n > 0 ? nullptr : g_uri_list_extract_uris(dr->text->str);
    size_t at = dr->at;
    for (char **u = uris; u != nullptr && *u != nullptr; u++) {
        char *path = g_filename_from_uri(*u, nullptr, nullptr);
        if (path != nullptr && xd_quicklaunch_add(dr->shell, at, path))
            at++;
        g_free(path);
    }
    g_strfreev(uris);
    finish_offer(dr->offer);
    g_string_free(dr->text, TRUE);
    close(dr->fd);
    g_free(dr);
    return G_SOURCE_REMOVE;
}

typedef struct desk_drop {
    xd_shell *shell;
    struct wl_data_offer *offer; /* ours until read */
    xd_desktop *desk;
    int fd;
    GString *text;
    double x, y;
    bool move;
} desk_drop;

static gboolean read_desk_drop(int fd, [[maybe_unused]] GIOCondition cond, gpointer data) {
    desk_drop *dd = data;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0)
        g_string_append_len(dd->text, buf, n);
    if ((n > 0 && dd->text->len < (1u << 20)) || (n < 0 && (errno == EINTR || errno == EAGAIN)))
        return G_SOURCE_CONTINUE;
    /* The end, or a list too long to be one: dropped either way. */
    char **uris = n > 0 ? nullptr : g_uri_list_extract_uris(dd->text->str);
    if (uris != nullptr && uris[0] != nullptr)
        xd_deskicons_drop(dd->shell, dd->desk, uris, dd->x, dd->y, dd->move);
    g_strfreev(uris);
    finish_offer(dd->offer);
    g_string_free(dd->text, TRUE);
    close(dd->fd);
    g_free(dd);
    return G_SOURCE_REMOVE;
}

static void drop_on_desktop(xd_dnd *d) {
    bool move = d->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    if (d->drag_source != nullptr) {
        /* Our own icons: the desktop knows what they are. */
        xd_deskicons_drop(d->shell, d->desk, nullptr, d->x, d->y, move);
    } else {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
            return;
        wl_data_offer_receive(d->offer, URI_LIST, fds[1]);
        close(fds[1]);
        desk_drop *dd = g_new0(desk_drop, 1);
        dd->shell = d->shell;
        dd->offer = d->offer;
        d->offer = nullptr; /* the read finishes it */
        dd->desk = d->desk;
        dd->fd = fds[0];
        dd->text = g_string_new(nullptr);
        dd->x = d->x;
        dd->y = d->y;
        dd->move = move;
        g_unix_fd_add(fds[0], (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_desk_drop, dd);
        wl_display_flush(d->shell->display);
        return;
    }
    if (wl_data_offer_get_version(d->offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION)
        wl_data_offer_finish(d->offer);
}

static void device_drop(void *data, [[maybe_unused]] struct wl_data_device *dev) {
    xd_dnd *d = data;
    if (d->desk != nullptr) {
        if (d->offer != nullptr && d->uris && d->action != 0)
            drop_on_desktop(d);
        forget_drag(d);
        return;
    }
    xd_taskbar *bar = d->bar;
    int at = bar != nullptr ? bar->drop_insert : -1;
    if (d->offer != nullptr && d->uris && at >= 0) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            wl_data_offer_receive(d->offer, URI_LIST, fds[1]);
            close(fds[1]);
            drop *dr = g_new0(drop, 1);
            dr->shell = d->shell;
            dr->offer = d->offer;
            d->offer = nullptr; /* the read finishes it */
            dr->fd = fds[0];
            dr->text = g_string_new(nullptr);
            dr->at = (size_t)at;
            g_unix_fd_add(fds[0], (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_drop, dr);
            /* The source writes once we've flushed the request. */
            wl_display_flush(d->shell->display);
        }
    }
    forget_drag(d);
}

static void device_selection(void *data, [[maybe_unused]] struct wl_data_device *dev,
                             struct wl_data_offer *offer) {
    /* The clipboard: its offer is ours to free. */
    xd_dnd *d = data;
    if (d->selection != nullptr)
        wl_data_offer_destroy(d->selection);
    d->selection = offer;
    d->selection_uris = offer != nullptr && offer == d->pending && d->pending_uris;
    d->selection_gnome = offer != nullptr && offer == d->pending && d->pending_gnome;
    if (offer == d->pending)
        d->pending = nullptr;
}

/* ---- the clipboard ------------------------------------------------------------- */

static char *clip_text(char *const *uris, bool cut, bool gnome) {
    GString *t = g_string_new(gnome ? (cut ? "cut" : "copy") : nullptr);
    for (char *const *u = uris; *u != nullptr; u++) {
        if (gnome)
            g_string_append_c(t, '\n');
        g_string_append(t, *u);
        if (!gnome)
            g_string_append(t, "\r\n");
    }
    return g_string_free(t, FALSE);
}

static void source_target([[maybe_unused]] void *data, [[maybe_unused]] struct wl_data_source *s,
                          [[maybe_unused]] const char *mime) {}

static void source_send(void *data, [[maybe_unused]] struct wl_data_source *s, const char *mime,
                        int32_t fd) {
    xd_dnd *d = data;
    if (d->source_uris != nullptr) {
        char *text = clip_text(d->source_uris, d->source_cut, strcmp(mime, GNOME_FILES) == 0);
        /* A short list: written at once (blocking), as other clients do. */
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        size_t len = strlen(text), off = 0;
        while (off < len) {
            ssize_t n = write(fd, text + off, len - off);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        g_free(text);
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *s) {
    xd_dnd *d = data;
    if (s == d->source) {
        d->source = nullptr;
        g_strfreev(d->source_uris);
        d->source_uris = nullptr;
        xd_deskicons_clipboard_lost(d->shell);
    }
    wl_data_source_destroy(s);
}

static void source_dnd_drop([[maybe_unused]] void *data,
                            [[maybe_unused]] struct wl_data_source *s) {}
static void source_dnd_finished([[maybe_unused]] void *data,
                                [[maybe_unused]] struct wl_data_source *s) {}
static void source_action([[maybe_unused]] void *data, [[maybe_unused]] struct wl_data_source *s,
                          [[maybe_unused]] uint32_t action) {}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_dnd_drop,
    .dnd_finished = source_dnd_finished,
    .action = source_action,
};

void xd_clipboard_set_files(xd_shell *shell, char *const *uris, bool cut, uint32_t serial) {
    xd_dnd *d = shell->dnd;
    if (d == nullptr || uris == nullptr || uris[0] == nullptr)
        return;
    struct wl_data_source *src = wl_data_device_manager_create_data_source(d->manager);
    wl_data_source_add_listener(src, &source_listener, d);
    wl_data_source_offer(src, GNOME_FILES);
    wl_data_source_offer(src, URI_LIST);
    /* The old one is cancelled by the compositor. */
    d->source = src;
    g_strfreev(d->source_uris);
    d->source_uris = g_strdupv((char **)(uintptr_t)uris);
    d->source_cut = cut;
    wl_data_device_set_selection(d->device, src, serial);
}

bool xd_clipboard_has_files(xd_shell *shell) {
    const xd_dnd *d = shell->dnd;
    return d != nullptr && (d->source_uris != nullptr ||
                            (d->selection != nullptr && (d->selection_gnome || d->selection_uris)));
}

typedef struct clip_read {
    xd_shell *shell;
    int fd;
    GString *text;
    bool gnome;
    xd_clipboard_got got;
    void *user;
} clip_read;

static void deliver(xd_shell *shell, const char *text, bool gnome, xd_clipboard_got got,
                    void *user) {
    bool cut = false;
    char **uris = nullptr;
    if (gnome) {
        char **lines = g_strsplit(text, "\n", -1);
        cut = lines[0] != nullptr && strcmp(g_strstrip(lines[0]), "cut") == 0;
        GPtrArray *a = g_ptr_array_new();
        for (size_t i = 1; lines[0] != nullptr && lines[i] != nullptr; i++) {
            char *u = g_strstrip(lines[i]);
            if (*u != '\0')
                g_ptr_array_add(a, g_strdup(u));
        }
        g_ptr_array_add(a, nullptr);
        uris = (char **)g_ptr_array_free(a, FALSE);
        g_strfreev(lines);
    } else {
        uris = g_uri_list_extract_uris(text);
    }
    got(shell, uris != nullptr && uris[0] != nullptr ? uris : nullptr, cut, user);
    g_strfreev(uris);
}

static gboolean read_clip(int fd, [[maybe_unused]] GIOCondition cond, gpointer data) {
    clip_read *r = data;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0)
        g_string_append_len(r->text, buf, n);
    if ((n > 0 && r->text->len < (1u << 20)) || (n < 0 && (errno == EINTR || errno == EAGAIN)))
        return G_SOURCE_CONTINUE;
    /* The end, or a list too long to be one: nothing to paste then. */
    if (n > 0)
        r->got(r->shell, nullptr, false, r->user);
    else
        deliver(r->shell, r->text->str, r->gnome, r->got, r->user);
    g_string_free(r->text, TRUE);
    close(r->fd);
    g_free(r);
    return G_SOURCE_REMOVE;
}

void xd_clipboard_get_files(xd_shell *shell, xd_clipboard_got got, void *user) {
    xd_dnd *d = shell->dnd;
    if (d == nullptr) {
        got(shell, nullptr, false, user);
        return;
    }
    if (d->source_uris != nullptr) {
        /* Ours: no need to ask ourselves through the compositor. */
        got(shell, d->source_uris, d->source_cut, user);
        return;
    }
    int fds[2];
    if (d->selection == nullptr || !(d->selection_gnome || d->selection_uris) ||
        pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        got(shell, nullptr, false, user);
        return;
    }
    clip_read *r = g_new0(clip_read, 1);
    r->shell = shell;
    r->fd = fds[0];
    r->text = g_string_new(nullptr);
    r->gnome = d->selection_gnome;
    r->got = got;
    r->user = user;
    wl_data_offer_receive(d->selection, r->gnome ? GNOME_FILES : URI_LIST, fds[1]);
    close(fds[1]);
    g_unix_fd_add(fds[0], (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_clip, r);
    wl_display_flush(shell->display);
}

/* ---- dragging the desktop's icons ------------------------------------------------ */

static void drag_end(xd_dnd *d, struct wl_data_source *s) {
    if (s == d->drag_source) {
        d->drag_source = nullptr;
        g_strfreev(d->drag_uris);
        d->drag_uris = nullptr;
        xd_deskicons_drag_ended(d->shell);
    }
    wl_data_source_destroy(s);
}

static void drag_send(void *data, [[maybe_unused]] struct wl_data_source *s, const char *mime,
                      int32_t fd) {
    xd_dnd *d = data;
    if (d->drag_uris != nullptr && strcmp(mime, URI_LIST) == 0) {
        char *text = clip_text(d->drag_uris, false, false);
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        size_t len = strlen(text), off = 0;
        while (off < len) {
            ssize_t n = write(fd, text + off, len - off);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        g_free(text);
    }
    close(fd);
}

static void drag_cancelled(void *data, struct wl_data_source *s) { drag_end(data, s); }
static void drag_finished(void *data, struct wl_data_source *s) { drag_end(data, s); }

static const struct wl_data_source_listener drag_listener = {
    .target = source_target,
    .send = drag_send,
    .cancelled = drag_cancelled,
    .dnd_drop_performed = source_dnd_drop,
    .dnd_finished = drag_finished,
    .action = source_action,
};

bool xd_dnd_start_drag(xd_shell *shell, char *const *uris, struct wl_surface *origin,
                       struct wl_surface *icon, uint32_t serial) {
    xd_dnd *d = shell->dnd;
    if (d == nullptr || d->drag_source != nullptr || uris == nullptr || uris[0] == nullptr ||
        wl_data_device_manager_get_version(d->manager) < 3)
        return false;
    struct wl_data_source *src = wl_data_device_manager_create_data_source(d->manager);
    wl_data_source_add_listener(src, &drag_listener, d);
    wl_data_source_offer(src, URI_LIST);
    wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                        WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    d->drag_source = src;
    d->drag_uris = g_strdupv((char **)(uintptr_t)uris);
    wl_data_device_start_drag(d->device, src, origin, icon, serial);
    return true;
}

void xd_clipboard_clear_cut(xd_shell *shell) {
    xd_dnd *d = shell->dnd;
    if (d != nullptr && d->source != nullptr && d->source_cut) {
        wl_data_source_destroy(d->source);
        d->source = nullptr;
        g_strfreev(d->source_uris);
        d->source_uris = nullptr;
        xd_deskicons_clipboard_lost(shell);
    }
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
    if (d->source != nullptr)
        wl_data_source_destroy(d->source);
    g_strfreev(d->source_uris);
    if (d->drag_source != nullptr)
        wl_data_source_destroy(d->drag_source);
    g_strfreev(d->drag_uris);
    if (wl_data_device_get_version(d->device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
        wl_data_device_release(d->device);
    else
        wl_data_device_destroy(d->device);
    wl_data_device_manager_destroy(d->manager);
    xd_free(d);
    shell->dnd = nullptr;
}
