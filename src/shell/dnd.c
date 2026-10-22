/* SPDX-License-Identifier: MIT
 * The shell's wl_data_device: the taskbar as a drop target, as in XP:
 *
 * - files or applications dropped on Quick Launch become shortcuts where
 *   they land;
 * - a drag held over a task button brings that window forward, so the
 *   drop can go into it;
 * - on Windows 7's taskbar (Aero), a file dropped on a program's button is
 *   pinned to its jump list, and a program dropped on the bar is pinned;
 *
 * the desktop as a drop target (into the Desktop folder, or onto a folder
 * or the Recycle Bin under the pointer) and its icons as a drag source;
 * and the clipboard's files for the desktop icons' Cut, Copy and Paste,
 * in the format file managers use (x-special/gnome-copied-files, with a
 * URI list besides).
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <glib-unix.h>
#include <gio/gdesktopappinfo.h>
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
    /* Our drag (desktop icons, or a program from the Start menu). */
    struct wl_data_source *drag_source;
    char **drag_uris;
    bool menu_drag;               /* from the Start menu, not the desktop */
    struct wl_surface *drag_icon; /* the menu drag's image */
    xd_shm_ring drag_ring;
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
    xd_dnd *dnd = data;
    if (offer == dnd->pending && strcmp(mime, URI_LIST) == 0)
        dnd->pending_uris = true;
    if (offer == dnd->pending && strcmp(mime, GNOME_FILES) == 0)
        dnd->pending_gnome = true;
}

static void offer_source_actions([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_data_offer *data_offer,
                                 [[maybe_unused]] uint32_t actions) {}

static void offer_action(void *data, struct wl_data_offer *data_offer, uint32_t action) {
    xd_dnd *dnd = data;
    if (data_offer == dnd->offer)
        dnd->action = action;
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void device_offer(void *data, [[maybe_unused]] struct wl_data_device *dev,
                         struct wl_data_offer *offer) {
    xd_dnd *dnd = data;
    dnd->pending = offer;
    dnd->pending_uris = false;
    dnd->pending_gnome = false;
    wl_data_offer_add_listener(offer, &offer_listener, dnd);
}

/* ---- the drag over the taskbar ------------------------------------------------- */

static void stop_hover(xd_dnd *dnd) {
    if (dnd->hover_timer != 0)
        g_source_remove(dnd->hover_timer);
    dnd->hover_timer = 0;
    dnd->hover_task = -1;
}

static void forget_drag(xd_dnd *dnd) {
    stop_hover(dnd);
    if (dnd->desk != nullptr)
        xd_deskicons_drag_over(dnd->shell, dnd->desk, -1, -1, false);
    dnd->desk = nullptr;
    if (dnd->bar != nullptr) {
        dnd->bar->drop_insert = -1;
        dnd->bar->dirty = true;
    }
    dnd->bar = nullptr;
    if (dnd->offer != nullptr)
        wl_data_offer_destroy(dnd->offer);
    dnd->offer = nullptr;
    dnd->uris = false;
}

void xd_dnd_bar_gone(xd_shell *shell, xd_taskbar *bar) {
    xd_dnd *dnd = shell->dnd;
    if (dnd != nullptr && dnd->bar == bar) {
        /* Its monitor went mid-drag: nothing more to tell it. */
        stop_hover(dnd);
        dnd->bar = nullptr;
    }
}

static gboolean hover_activate(gpointer data) {
    xd_dnd *dnd = data;
    dnd->hover_timer = 0;
    xd_taskbar *bar = dnd->bar;
    if (bar == nullptr || dnd->hover_task < 0 || (size_t)dnd->hover_task >= bar->task_count)
        return G_SOURCE_REMOVE;
    xd_toplevel *toplevel = bar->task_views[dnd->hover_task];
    if (toplevel == nullptr)
        return G_SOURCE_REMOVE; /* Aero: a pinned program that isn't running */
    if (toplevel->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(toplevel->handle);
    if (dnd->shell->seat != nullptr)
        zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, dnd->shell->seat);
    return G_SOURCE_REMOVE;
}

/* The desktop's own icons being dragged (they know where they are). */
static bool own_icons(const xd_dnd *dnd) { return dnd->drag_source != nullptr && !dnd->menu_drag; }

static int task_at(const xd_taskbar *bar, double lx) {
    int x = (int)(lx * bar->scale);
    for (size_t i = 0; i < bar->task_count; i++) {
        xd_rect rect = bar->task_rects[i];
        if (x >= rect.x && x < rect.x + rect.w)
            return (int)i;
    }
    return -1;
}

/* Aero: over Windows 7's buttons, or the bar's free space between the
 * Start button and the notification area, a drop pins. */
static bool pins_here(const xd_taskbar *bar, double lx, double ly) {
    if (!bar->superbar)
        return false;
    if (task_at(bar, lx) >= 0)
        return true;
    int x = (int)(lx * bar->scale), y = (int)(ly * bar->scale);
    xd_rect rect = bar->quick_band;
    bool on_quick = x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    return !on_quick && x >= bar->start_rect.x + bar->start_rect.w &&
           (xd_rect_empty(bar->tray_rect) || x < bar->tray_rect.x);
}

static void track(xd_dnd *dnd) {
    if (dnd->desk != nullptr && dnd->offer != nullptr) {
        xd_drop_kind drop_kind = dnd->uris ? xd_deskicons_drag_over(dnd->shell, dnd->desk, dnd->x,
                                                                    dnd->y, own_icons(dnd))
                                           : XD_DROP_NONE;
        if (drop_kind == XD_DROP_NONE) {
            wl_data_offer_accept(dnd->offer, dnd->serial, nullptr);
            wl_data_offer_set_actions(dnd->offer, 0, 0);
        } else {
            wl_data_offer_accept(dnd->offer, dnd->serial, URI_LIST);
            /* Files move within the drive, as Windows does on one disk; to
             * the Recycle Bin they always move. */
            uint32_t move = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
            uint32_t copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
            wl_data_offer_set_actions(dnd->offer, drop_kind == XD_DROP_TRASH ? move : copy | move,
                                      move);
        }
        return;
    }
    xd_taskbar *bar = dnd->bar;
    if (bar == nullptr || dnd->offer == nullptr)
        return;
    xd_taskbar_reveal(bar); /* Auto-hide */
    int insert = dnd->uris ? xd_taskbar_quick_insert_at(bar, dnd->x, dnd->y) : -1;
    if (insert != bar->drop_insert) {
        bar->drop_insert = insert;
        bar->dirty = true;
    }
    if (insert >= 0 || (dnd->uris && pins_here(bar, dnd->x, dnd->y))) {
        wl_data_offer_accept(dnd->offer, dnd->serial, URI_LIST);
        /* A shortcut to it, or a pin: the dragged file stays where it is. */
        wl_data_offer_set_actions(dnd->offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    } else {
        wl_data_offer_accept(dnd->offer, dnd->serial, nullptr);
    }
    /* Held over a task button: that window comes forward. */
    int task = insert < 0 ? task_at(bar, dnd->x) : -1;
    if (task != dnd->hover_task) {
        stop_hover(dnd);
        dnd->hover_task = task;
        if (task >= 0)
            dnd->hover_timer = g_timeout_add(HOVER_ACTIVATE_MS, hover_activate, dnd);
    }
}

static void device_enter(void *data, [[maybe_unused]] struct wl_data_device *dev, uint32_t serial,
                         struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                         struct wl_data_offer *offer) {
    xd_dnd *dnd = data;
    forget_drag(dnd);
    dnd->offer = offer;
    dnd->uris = offer != nullptr && offer == dnd->pending && dnd->pending_uris;
    if (offer == dnd->pending)
        dnd->pending = nullptr;
    dnd->serial = serial;
    dnd->bar = xd_taskbar_for_surface(dnd->shell, surface);
    dnd->desk = dnd->bar == nullptr ? xd_desktop_for_surface(dnd->shell, surface) : nullptr;
    dnd->action = 0;
    dnd->x = wl_fixed_to_double(x);
    dnd->y = wl_fixed_to_double(y);
    if (dnd->bar == nullptr && dnd->desk == nullptr && offer != nullptr)
        wl_data_offer_accept(offer, serial, nullptr);
    track(dnd);
}

static void device_leave(void *data, [[maybe_unused]] struct wl_data_device *dev) {
    forget_drag(data);
}

static void device_motion(void *data, [[maybe_unused]] struct wl_data_device *dev,
                          [[maybe_unused]] uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    xd_dnd *dnd = data;
    dnd->x = wl_fixed_to_double(x);
    dnd->y = wl_fixed_to_double(y);
    track(dnd);
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
    ssize_t bytes_read = read(fd, buf, sizeof(buf));
    if (bytes_read > 0)
        g_string_append_len(dr->text, buf, bytes_read);
    if ((bytes_read > 0 && dr->text->len < (1u << 20)) ||
        (bytes_read < 0 && (errno == EINTR || errno == EAGAIN)))
        return G_SOURCE_CONTINUE;
    /* The end, or a list too long to be one: dropped either way. */
    char **uris = bytes_read > 0 ? nullptr : g_uri_list_extract_uris(dr->text->str);
    size_t at = dr->at;
    for (char **uri = uris; uri != nullptr && *uri != nullptr; uri++) {
        char *path = g_filename_from_uri(*uri, nullptr, nullptr);
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

/* Files dropped to be pinned (Aero), and the button they were dropped on. */
typedef struct pin_drop {
    xd_shell *shell;
    struct wl_data_offer *offer; /* ours until read */
    int fd;
    GString *text;
    char *key; /* the program's, or nullptr */
} pin_drop;

static gboolean read_pin_drop(int fd, [[maybe_unused]] GIOCondition cond, gpointer data) {
    pin_drop *pd = data;
    char buf[4096];
    ssize_t bytes_read = read(fd, buf, sizeof(buf));
    if (bytes_read > 0)
        g_string_append_len(pd->text, buf, bytes_read);
    if ((bytes_read > 0 && pd->text->len < (1u << 20)) ||
        (bytes_read < 0 && (errno == EINTR || errno == EAGAIN)))
        return G_SOURCE_CONTINUE;
    char **uris = bytes_read > 0 ? nullptr : g_uri_list_extract_uris(pd->text->str);
    if (uris != nullptr && uris[0] != nullptr)
        xd_superbar_dropped(pd->shell, pd->key, uris);
    g_strfreev(uris);
    finish_offer(pd->offer);
    g_string_free(pd->text, TRUE);
    g_free(pd->key);
    close(pd->fd);
    g_free(pd);
    return G_SOURCE_REMOVE;
}

static void drop_to_pin(xd_dnd *dnd) {
    xd_taskbar *bar = dnd->bar;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    wl_data_offer_receive(dnd->offer, URI_LIST, fds[1]);
    close(fds[1]);
    pin_drop *pd = g_new0(pin_drop, 1);
    pd->shell = dnd->shell;
    pd->offer = dnd->offer;
    dnd->offer = nullptr; /* the read finishes it */
    pd->fd = fds[0];
    pd->text = g_string_new(nullptr);
    const char *key = xd_superbar_key(bar, task_at(bar, dnd->x), nullptr);
    pd->key = key != nullptr ? g_strdup(key) : nullptr;
    g_unix_fd_add(fds[0], (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_pin_drop, pd);
    wl_display_flush(dnd->shell->display);
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
    ssize_t bytes_read = read(fd, buf, sizeof(buf));
    if (bytes_read > 0)
        g_string_append_len(dd->text, buf, bytes_read);
    if ((bytes_read > 0 && dd->text->len < (1u << 20)) ||
        (bytes_read < 0 && (errno == EINTR || errno == EAGAIN)))
        return G_SOURCE_CONTINUE;
    /* The end, or a list too long to be one: dropped either way. */
    char **uris = bytes_read > 0 ? nullptr : g_uri_list_extract_uris(dd->text->str);
    if (uris != nullptr && uris[0] != nullptr)
        xd_deskicons_drop(dd->shell, dd->desk, uris, dd->x, dd->y, dd->move);
    g_strfreev(uris);
    finish_offer(dd->offer);
    g_string_free(dd->text, TRUE);
    close(dd->fd);
    g_free(dd);
    return G_SOURCE_REMOVE;
}

static void drop_on_desktop(xd_dnd *dnd) {
    bool move = dnd->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    if (own_icons(dnd)) {
        /* Our own icons: the desktop knows what they are. */
        xd_deskicons_drop(dnd->shell, dnd->desk, nullptr, dnd->x, dnd->y, move);
    } else {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
            return;
        wl_data_offer_receive(dnd->offer, URI_LIST, fds[1]);
        close(fds[1]);
        desk_drop *dd = g_new0(desk_drop, 1);
        dd->shell = dnd->shell;
        dd->offer = dnd->offer;
        dnd->offer = nullptr; /* the read finishes it */
        dd->desk = dnd->desk;
        dd->fd = fds[0];
        dd->text = g_string_new(nullptr);
        dd->x = dnd->x;
        dd->y = dnd->y;
        dd->move = move;
        g_unix_fd_add(fds[0], (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_desk_drop, dd);
        wl_display_flush(dnd->shell->display);
        return;
    }
    if (wl_data_offer_get_version(dnd->offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION)
        wl_data_offer_finish(dnd->offer);
}

static void device_drop(void *data, [[maybe_unused]] struct wl_data_device *dev) {
    xd_dnd *dnd = data;
    if (dnd->desk != nullptr) {
        if (dnd->offer != nullptr && dnd->uris && dnd->action != 0)
            drop_on_desktop(dnd);
        forget_drag(dnd);
        return;
    }
    xd_taskbar *bar = dnd->bar;
    int at = bar != nullptr ? bar->drop_insert : -1;
    if (dnd->offer != nullptr && dnd->uris && at < 0 && bar != nullptr &&
        pins_here(bar, dnd->x, dnd->y)) {
        drop_to_pin(dnd);
    } else if (dnd->offer != nullptr && dnd->uris && at >= 0) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            wl_data_offer_receive(dnd->offer, URI_LIST, fds[1]);
            close(fds[1]);
            drop *dr = g_new0(drop, 1);
            dr->shell = dnd->shell;
            dr->offer = dnd->offer;
            dnd->offer = nullptr; /* the read finishes it */
            dr->fd = fds[0];
            dr->text = g_string_new(nullptr);
            dr->at = (size_t)at;
            g_unix_fd_add(fds[0], (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_drop, dr);
            /* The source writes once we've flushed the request. */
            wl_display_flush(dnd->shell->display);
        }
    }
    forget_drag(dnd);
}

static void device_selection(void *data, [[maybe_unused]] struct wl_data_device *dev,
                             struct wl_data_offer *offer) {
    /* The clipboard: its offer is ours to free. */
    xd_dnd *dnd = data;
    if (dnd->selection != nullptr)
        wl_data_offer_destroy(dnd->selection);
    dnd->selection = offer;
    dnd->selection_uris = offer != nullptr && offer == dnd->pending && dnd->pending_uris;
    dnd->selection_gnome = offer != nullptr && offer == dnd->pending && dnd->pending_gnome;
    if (offer == dnd->pending)
        dnd->pending = nullptr;
}

/* ---- the clipboard ------------------------------------------------------------- */

static char *clip_text(char *const *uris, bool cut, bool gnome) {
    GString *string = g_string_new(gnome ? (cut ? "cut" : "copy") : nullptr);
    for (char *const *uri = uris; *uri != nullptr; uri++) {
        if (gnome)
            g_string_append_c(string, '\n');
        g_string_append(string, *uri);
        if (!gnome)
            g_string_append(string, "\r\n");
    }
    return g_string_free(string, FALSE);
}

/* ---- sending what's cut, copied or dragged ---------------------------------------- */

/* Text going to a program's pipe, as the pipe takes it: never waiting on
 * it (a program that doesn't read, or shrank its pipe, would hold the
 * shell up), and given up after a few seconds. */
typedef struct sending {
    int fd;
    char *text;
    size_t len, off;
    guint watch, timeout;
} sending;

static void sending_end(sending *out) {
    if (out->watch != 0)
        g_source_remove(out->watch);
    if (out->timeout != 0)
        g_source_remove(out->timeout);
    close(out->fd);
    g_free(out->text);
    g_free(out);
}

/* True while there's more to write. */
static bool sending_write(sending *out) {
    while (out->off < out->len) {
        ssize_t written = write(out->fd, out->text + out->off, out->len - out->off);
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && errno == EAGAIN)
            return true;
        if (written <= 0)
            return false;
        out->off += (size_t)written;
    }
    return false;
}

static gboolean sending_ready([[maybe_unused]] gint fd, [[maybe_unused]] GIOCondition condition,
                              gpointer data) {
    sending *out = data;
    if (sending_write(out))
        return G_SOURCE_CONTINUE;
    out->watch = 0;
    sending_end(out);
    return G_SOURCE_REMOVE;
}

static gboolean sending_gave_up(gpointer data) {
    sending *out = data;
    out->timeout = 0;
    sending_end(out);
    return G_SOURCE_REMOVE;
}

/* Takes `fd` and `text`. */
static void send_text(int fd, char *text) {
    sending *out = g_new0(sending, 1);
    out->fd = fd;
    out->text = text;
    out->len = strlen(text);
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (!sending_write(out)) {
        sending_end(out);
        return;
    }
    out->watch =
        g_unix_fd_add(fd, (GIOCondition)(G_IO_OUT | G_IO_ERR | G_IO_HUP), sending_ready, out);
    out->timeout = g_timeout_add_seconds(5, sending_gave_up, out);
}

static void source_target([[maybe_unused]] void *data,
                          [[maybe_unused]] struct wl_data_source *data_source,
                          [[maybe_unused]] const char *mime) {}

static void source_send(void *data, [[maybe_unused]] struct wl_data_source *data_source,
                        const char *mime, int32_t fd) {
    xd_dnd *dnd = data;
    if (dnd->source_uris != nullptr) {
        send_text(fd, clip_text(dnd->source_uris, dnd->source_cut, strcmp(mime, GNOME_FILES) == 0));
        return;
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *data_source) {
    xd_dnd *dnd = data;
    if (data_source == dnd->source) {
        dnd->source = nullptr;
        g_strfreev(dnd->source_uris);
        dnd->source_uris = nullptr;
        xd_deskicons_clipboard_lost(dnd->shell);
    }
    wl_data_source_destroy(data_source);
}

static void source_dnd_drop([[maybe_unused]] void *data,
                            [[maybe_unused]] struct wl_data_source *data_source) {}
static void source_dnd_finished([[maybe_unused]] void *data,
                                [[maybe_unused]] struct wl_data_source *data_source) {}
static void source_action([[maybe_unused]] void *data,
                          [[maybe_unused]] struct wl_data_source *data_source,
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
    xd_dnd *dnd = shell->dnd;
    if (dnd == nullptr || uris == nullptr || uris[0] == nullptr)
        return;
    struct wl_data_source *src = wl_data_device_manager_create_data_source(dnd->manager);
    wl_data_source_add_listener(src, &source_listener, dnd);
    wl_data_source_offer(src, GNOME_FILES);
    wl_data_source_offer(src, URI_LIST);
    /* The old one is cancelled by the compositor. */
    dnd->source = src;
    g_strfreev(dnd->source_uris);
    dnd->source_uris = g_strdupv((char **)(uintptr_t)uris);
    dnd->source_cut = cut;
    wl_data_device_set_selection(dnd->device, src, serial);
}

bool xd_clipboard_has_files(xd_shell *shell) {
    const xd_dnd *dnd = shell->dnd;
    return dnd != nullptr &&
           (dnd->source_uris != nullptr ||
            (dnd->selection != nullptr && (dnd->selection_gnome || dnd->selection_uris)));
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
        GPtrArray *array = g_ptr_array_new();
        for (size_t i = 1; lines[0] != nullptr && lines[i] != nullptr; i++) {
            char *uri = g_strstrip(lines[i]);
            if (*uri != '\0')
                g_ptr_array_add(array, g_strdup(uri));
        }
        g_ptr_array_add(array, nullptr);
        uris = (char **)g_ptr_array_free(array, FALSE);
        g_strfreev(lines);
    } else {
        uris = g_uri_list_extract_uris(text);
    }
    got(shell, uris != nullptr && uris[0] != nullptr ? uris : nullptr, cut, user);
    g_strfreev(uris);
}

static gboolean read_clip(int fd, [[maybe_unused]] GIOCondition cond, gpointer data) {
    clip_read *clip = data;
    char buf[4096];
    ssize_t bytes_read = read(fd, buf, sizeof(buf));
    if (bytes_read > 0)
        g_string_append_len(clip->text, buf, bytes_read);
    if ((bytes_read > 0 && clip->text->len < (1u << 20)) ||
        (bytes_read < 0 && (errno == EINTR || errno == EAGAIN)))
        return G_SOURCE_CONTINUE;
    /* The end, or a list too long to be one: nothing to paste then. */
    if (bytes_read > 0)
        clip->got(clip->shell, nullptr, false, clip->user);
    else
        deliver(clip->shell, clip->text->str, clip->gnome, clip->got, clip->user);
    g_string_free(clip->text, TRUE);
    close(clip->fd);
    g_free(clip);
    return G_SOURCE_REMOVE;
}

void xd_clipboard_get_files(xd_shell *shell, xd_clipboard_got got, void *user) {
    xd_dnd *dnd = shell->dnd;
    if (dnd == nullptr) {
        got(shell, nullptr, false, user);
        return;
    }
    if (dnd->source_uris != nullptr) {
        /* Ours: no need to ask ourselves through the compositor. */
        got(shell, dnd->source_uris, dnd->source_cut, user);
        return;
    }
    int fds[2];
    if (dnd->selection == nullptr || !(dnd->selection_gnome || dnd->selection_uris) ||
        pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        got(shell, nullptr, false, user);
        return;
    }
    clip_read *clip = g_new0(clip_read, 1);
    clip->shell = shell;
    clip->fd = fds[0];
    clip->text = g_string_new(nullptr);
    clip->gnome = dnd->selection_gnome;
    clip->got = got;
    clip->user = user;
    wl_data_offer_receive(dnd->selection, clip->gnome ? GNOME_FILES : URI_LIST, fds[1]);
    close(fds[1]);
    g_unix_fd_add(fds[0], (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), read_clip, clip);
    wl_display_flush(shell->display);
}

/* ---- dragging the desktop's icons ------------------------------------------------ */

static void drag_end(xd_dnd *dnd, struct wl_data_source *data_source) {
    bool menu = false;
    if (data_source == dnd->drag_source) {
        dnd->drag_source = nullptr;
        g_strfreev(dnd->drag_uris);
        dnd->drag_uris = nullptr;
        menu = dnd->menu_drag;
        dnd->menu_drag = false;
        if (dnd->drag_icon != nullptr) {
            wl_surface_destroy(dnd->drag_icon);
            dnd->drag_icon = nullptr;
        }
        xd_shm_ring_finish(&dnd->drag_ring);
        if (!menu)
            xd_deskicons_drag_ended(dnd->shell);
    }
    wl_data_source_destroy(data_source);
    /* Dropped (or not): the Start menu it came from closes, as in XP. */
    if (menu)
        xd_menu_close_all(dnd->shell);
}

static void drag_send(void *data, [[maybe_unused]] struct wl_data_source *data_source,
                      const char *mime, int32_t fd) {
    xd_dnd *dnd = data;
    if (dnd->drag_uris != nullptr && strcmp(mime, URI_LIST) == 0) {
        send_text(fd, clip_text(dnd->drag_uris, false, false));
        return;
    }
    close(fd);
}

static void drag_cancelled(void *data, struct wl_data_source *data_source) {
    drag_end(data, data_source);
}
static void drag_finished(void *data, struct wl_data_source *data_source) {
    drag_end(data, data_source);
}

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
    xd_dnd *dnd = shell->dnd;
    if (dnd == nullptr || dnd->drag_source != nullptr || uris == nullptr || uris[0] == nullptr ||
        wl_data_device_manager_get_version(dnd->manager) < 3)
        return false;
    struct wl_data_source *src = wl_data_device_manager_create_data_source(dnd->manager);
    wl_data_source_add_listener(src, &drag_listener, dnd);
    wl_data_source_offer(src, URI_LIST);
    wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                        WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    dnd->drag_source = src;
    dnd->drag_uris = g_strdupv((char **)(uintptr_t)uris);
    wl_data_device_start_drag(dnd->device, src, origin, icon, serial);
    return true;
}

/* The program's icon, half see-through, under the pointer. */
static struct wl_surface *app_drag_image(xd_dnd *dnd, const xd_app *app, double scale) {
    xd_shell *shell = dnd->shell;
    double up = ceil(scale);
    int bs = XD_CLAMP((int)up, 1, 4);
    int px = 32 * bs;
    const char *names[] = {app->icon, "application-x-executable"};
    cairo_surface_t *img = xd_icons_get_any(shell->icons, names, XD_ARRAY_LEN(names), px);
    if (img == nullptr || shell->compositor == nullptr || shell->shm == nullptr)
        return nullptr;
    xd_shm_buffer *buf = xd_shm_ring_acquire(&dnd->drag_ring, shell->shm, px, px);
    if (buf == nullptr)
        return nullptr;
    cairo_t *cairo = cairo_create(buf->surface);
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    cairo_set_source_surface(cairo, img, 0, 0);
    cairo_paint_with_alpha(cairo, 0.5);
    cairo_destroy(cairo);
    cairo_surface_flush(buf->surface);
    struct wl_surface *surface = wl_compositor_create_surface(shell->compositor);
    wl_surface_set_buffer_scale(surface, bs);
    /* Its middle on the pointer (wl_compositor 4: attach's offset). */
    wl_surface_attach(surface, buf->buffer, -16, -16);
    wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
    buf->busy = true;
    wl_surface_commit(surface);
    return surface;
}

bool xd_dnd_start_app_drag(xd_shell *shell, const xd_app *app, struct wl_surface *origin,
                           double scale) {
    xd_dnd *dnd = shell->dnd;
    const char *file = app->info != nullptr ? g_desktop_app_info_get_filename(app->info) : nullptr;
    char *uri = file != nullptr ? g_filename_to_uri(file, nullptr, nullptr) : nullptr;
    if (dnd == nullptr || uri == nullptr || dnd->drag_source != nullptr ||
        wl_data_device_manager_get_version(dnd->manager) < 3) {
        g_free(uri);
        return false;
    }
    char *uris[] = {uri, nullptr};
    struct wl_data_source *src = wl_data_device_manager_create_data_source(dnd->manager);
    wl_data_source_add_listener(src, &drag_listener, dnd);
    wl_data_source_offer(src, URI_LIST);
    /* Only ever a copy: the program's own desktop file stays where it is. */
    wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    dnd->drag_source = src;
    dnd->drag_uris = g_strdupv(uris);
    dnd->menu_drag = true;
    dnd->drag_icon = app_drag_image(dnd, app, scale);
    wl_data_device_start_drag(dnd->device, src, origin, dnd->drag_icon, shell->button_serial);
    g_free(uri);
    return true;
}

void xd_clipboard_clear_cut(xd_shell *shell) {
    xd_dnd *dnd = shell->dnd;
    if (dnd != nullptr && dnd->source != nullptr && dnd->source_cut) {
        wl_data_source_destroy(dnd->source);
        dnd->source = nullptr;
        g_strfreev(dnd->source_uris);
        dnd->source_uris = nullptr;
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
    xd_dnd *dnd = XD_NEW(xd_dnd);
    dnd->shell = shell;
    dnd->manager = manager;
    dnd->hover_task = -1;
    dnd->device = wl_data_device_manager_get_data_device(manager, shell->seat);
    wl_data_device_add_listener(dnd->device, &device_listener, dnd);
    shell->dnd = dnd;
}

void xd_dnd_finish(xd_shell *shell) {
    xd_dnd *dnd = shell->dnd;
    if (dnd == nullptr)
        return;
    forget_drag(dnd);
    if (dnd->selection != nullptr)
        wl_data_offer_destroy(dnd->selection);
    if (dnd->source != nullptr)
        wl_data_source_destroy(dnd->source);
    g_strfreev(dnd->source_uris);
    if (dnd->drag_source != nullptr)
        wl_data_source_destroy(dnd->drag_source);
    g_strfreev(dnd->drag_uris);
    if (dnd->drag_icon != nullptr)
        wl_surface_destroy(dnd->drag_icon);
    xd_shm_ring_finish(&dnd->drag_ring);
    if (wl_data_device_get_version(dnd->device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
        wl_data_device_release(dnd->device);
    else
        wl_data_device_destroy(dnd->device);
    wl_data_device_manager_destroy(dnd->manager);
    xd_free(dnd);
    shell->dnd = nullptr;
}
