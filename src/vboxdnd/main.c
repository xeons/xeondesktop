/* SPDX-License-Identifier: MIT
 * xd-vboxdnd: VirtualBox drag and drop for a XeonDesktop guest, which
 * VirtualBox's own VBoxClient refuses to do under Wayland. Files (or
 * text) dragged from the host onto the VM's window are dropped on what's
 * under the pointer here.
 *
 * A thread waits on the host's drag and drop service (dnd.c) and hands
 * each event to the main loop. There, a drag entering starts a Wayland
 * drag with a data source offering the host's formats; the compositor
 * (dragrelay.c, which started this program) lets it start without a
 * button held and moves it where the host says. Each move is answered
 * with the action the target under it last accepted. At the drop, the
 * host's data is asked for and received into a fresh directory under
 * $TMPDIR/VirtualBox Dropped Files (as VirtualBox's own guest does); only
 * then does the drag drop, offering the received files' URI list.
 *
 * The other way: the compositor tells of drags programs here start. When
 * the host asks (the pointer left the VM's window with a button held),
 * one going on is reported with its types; dropped on the host, its data
 * is fetched from its program and sent (files: everything they hold),
 * then the drag is cancelled here.
 *
 *   xd-vboxdnd                     against the real host (/dev/vboxuser)
 *   xd-vboxdnd --fake-host SCRIPT  against a scripted one, for tests
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/prctl.h>
#include <glib-unix.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "../ui/protocols.h"
#include "dnd.h"
#include "fakehost.h"

typedef struct relay {
    GMainLoop *loop;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_data_device_manager *data_manager;
    struct wl_data_device *data_device;
    struct xd_drag_relay_manager_v1 *relay_manager;
    struct xd_drag_relay_v1 *drag_relay;
    struct wl_surface *origin; /* the drag's, never shown */

    xd_hgcm *hgcm;
    xd_dnd_session session;
    GThread *worker;
    GMutex worker_lock;
    GCond worker_ended;
    bool worker_done;
    /* The host's service forgets a client's wait for its next message
     * once any other call of the client's completes: so the host thread
     * asks for the next only once the last one's answered (the host's
     * turn), never while another call may go. */
    GCond host_turn;
    bool answered;
    bool held;     /* the answer comes later (the send thread's) */
    bool quitting;

    /* The drag in progress. */
    struct wl_data_source *source;
    bool accepted;           /* the target under it takes one of its types */
    uint32_t action;         /* and this action (wl_data_device_manager's) */
    bool dropping;           /* the data was asked for */
    bool awaiting_accept;    /* it came: offered again, dropped once taken */
    guint drop_timeout;
    gint64 last_heard;       /* monotonic µs: the host's last word on it */
    gint64 data_asked;       /* when its data was asked for */
    guint watchdog;
    GBytes *data;            /* what's dropped, once it came */
    char *data_mime;

    /* A drag a program here started, which may leave for the host. */
    GPtrArray *outgoing_next;  /* its types, as they're told */
    GPtrArray *outgoing_types; /* once it started; nullptr: none */
    uint32_t outgoing_actions; /* wl_data_device_manager's */
    bool sending;              /* its data is going to the host */
} dnd_relay;

/* How long the host may leave a drag of its unexplained: after the drop,
 * for its data; before, between its moves. */
static constexpr gint64 DATA_WAIT_US = 15 * G_USEC_PER_SEC;
static constexpr gint64 QUIET_US = 10 * G_USEC_PER_SEC;

/* A step worth knowing, in the session's log. */
static void note(const char *format, ...) G_GNUC_PRINTF(1, 2);
static void note(const char *format, ...) {
    va_list args;
    va_start(args, format);
    g_autofree char *text = g_strdup_vprintf(format, args);
    va_end(args);
    fprintf(stderr, "xd-vboxdnd: %s\n", text);
}

/* The answer's gone: the host thread may wait for the next message. */
static void host_answered(dnd_relay *relay) {
    g_mutex_lock(&relay->worker_lock);
    relay->answered = true;
    g_cond_signal(&relay->host_turn);
    g_mutex_unlock(&relay->worker_lock);
}

typedef struct posted {
    dnd_relay *owner;
    xd_dnd_event *event;
} posted;

/* ---- the drag ---------------------------------------------------------------------------- */

static uint32_t vbox_action(const dnd_relay *relay) {
    if (!relay->accepted)
        return XD_DND_IGNORE;
    if (relay->action & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
        return XD_DND_MOVE;
    if (relay->action & (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                         WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK))
        return XD_DND_COPY;
    return XD_DND_IGNORE;
}

static void end_drag(dnd_relay *relay) {
    if (relay->watchdog != 0)
        g_source_remove(relay->watchdog);
    relay->watchdog = 0;
    if (relay->drop_timeout != 0)
        g_source_remove(relay->drop_timeout);
    relay->drop_timeout = 0;
    relay->awaiting_accept = false;
    if (relay->source != nullptr)
        wl_data_source_destroy(relay->source);
    relay->source = nullptr;
    relay->accepted = false;
    relay->action = 0;
    relay->dropping = false;
    g_clear_pointer(&relay->data, g_bytes_unref);
    g_clear_pointer(&relay->data_mime, g_free);
    wl_display_flush(relay->display);
}

static void drop_now(dnd_relay *relay) {
    if (relay->drop_timeout != 0)
        g_source_remove(relay->drop_timeout);
    relay->drop_timeout = 0;
    relay->awaiting_accept = false;
    xd_drag_relay_v1_drop(relay->drag_relay);
    wl_display_flush(relay->display);
}

/* The data's here and offered again: dropped once the target takes it. */
static void maybe_drop(dnd_relay *relay) {
    if (relay->awaiting_accept && relay->accepted && relay->action != 0)
        drop_now(relay);
}

static gboolean drop_timed_out(gpointer data) {
    dnd_relay *relay = data;
    relay->drop_timeout = 0;
    /* It never said: drop anyway (it may take it, or the drag just ends). */
    drop_now(relay);
    return G_SOURCE_REMOVE;
}

static void source_target(void *data, [[maybe_unused]] struct wl_data_source *source,
                          const char *mime_type) {
    dnd_relay *relay = data;
    relay->accepted = mime_type != nullptr;
    xd_hgcm_trace("target takes %s", mime_type != nullptr ? mime_type : "nothing");
    maybe_drop(relay);
}

/* Text types are all served from the same text. */
static bool same_kind(const char *offered, const char *have) {
    return strcmp(offered, have) == 0 ||
           (g_str_has_prefix(offered, "text/plain") && g_str_has_prefix(have, "text/plain"));
}

static void source_send(void *data, [[maybe_unused]] struct wl_data_source *source,
                        const char *mime_type, int32_t fd) {
    dnd_relay *relay = data;
    if (relay->data != nullptr && relay->data_mime != nullptr &&
        same_kind(mime_type, relay->data_mime)) {
        gsize size;
        const char *bytes = g_bytes_get_data(relay->data, &size);
        while (size > 0) {
            ssize_t written = write(fd, bytes, size);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            bytes += written;
            size -= (gsize)written;
        }
    }
    close(fd);
}

static void source_cancelled(void *data, [[maybe_unused]] struct wl_data_source *source) {
    dnd_relay *relay = data;
    /* Dropped where it wasn't taken, or ended: the host's done too. */
    if (!relay->dropping)
        end_drag(relay);
}

static void source_drop_performed([[maybe_unused]] void *data,
                                  [[maybe_unused]] struct wl_data_source *source) {}

static void source_finished(void *data, [[maybe_unused]] struct wl_data_source *source) {
    end_drag(data);
}

static void source_action(void *data, [[maybe_unused]] struct wl_data_source *source,
                          uint32_t action) {
    dnd_relay *relay = data;
    relay->action = action;
    xd_hgcm_trace("target's action %u", action);
    maybe_drop(relay);
}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_drop_performed,
    .dnd_finished = source_finished,
    .action = source_action,
};

/* ---- guest to host --------------------------------------------------------------------- */

static void outgoing_type(void *data, [[maybe_unused]] struct xd_drag_relay_v1 *drag_relay,
                          const char *mime_type) {
    dnd_relay *relay = data;
    g_ptr_array_add(relay->outgoing_next, g_strdup(mime_type));
}

static void outgoing_started(void *data, [[maybe_unused]] struct xd_drag_relay_v1 *drag_relay,
                             uint32_t actions) {
    dnd_relay *relay = data;
    if (relay->outgoing_types != nullptr)
        g_ptr_array_unref(relay->outgoing_types);
    relay->outgoing_types = relay->outgoing_next;
    relay->outgoing_next = g_ptr_array_new_with_free_func(g_free);
    relay->outgoing_actions = actions;
}

static void outgoing_ended(void *data, [[maybe_unused]] struct xd_drag_relay_v1 *drag_relay) {
    dnd_relay *relay = data;
    g_clear_pointer(&relay->outgoing_types, g_ptr_array_unref);
    g_ptr_array_set_size(relay->outgoing_next, 0);
}

static const struct xd_drag_relay_v1_listener relay_listener = {
    .outgoing_type = outgoing_type,
    .outgoing_started = outgoing_started,
    .outgoing_ended = outgoing_ended,
};

/* The host asks whether a drag here is going on: answered at once, from
 * what the compositor told (files first: the host prefers them). */
static void answer_pending(dnd_relay *relay) {
    if (relay->outgoing_types == nullptr || relay->outgoing_types->len == 0 || relay->sending) {
        xd_dnd_ack_pending(relay->hgcm, XD_DND_IGNORE, 0, nullptr);
        return;
    }
    GPtrArray *formats = g_ptr_array_new();
    for (guint i = 0; i < relay->outgoing_types->len; i++) {
        char *type = g_ptr_array_index(relay->outgoing_types, i);
        if (g_ascii_strcasecmp(type, XD_DND_URI_LIST) == 0)
            g_ptr_array_insert(formats, 0, type);
        else
            g_ptr_array_add(formats, type);
    }
    g_ptr_array_add(formats, nullptr);
    uint32_t allowed = XD_DND_COPY;
    if (relay->outgoing_actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
        allowed |= XD_DND_MOVE;
    xd_dnd_ack_pending(relay->hgcm, XD_DND_COPY, allowed, (const char *const *)formats->pdata);
    g_ptr_array_unref(formats);
}

typedef struct outgoing_send {
    dnd_relay *relay;
    char *mime;
    int fd; /* what the program writes */
} outgoing_send;

/* All the program writes, waiting at most 10 seconds for each piece. */
static GByteArray *read_all(int fd) {
    GByteArray *bytes = g_byte_array_new();
    unsigned char chunk[16384];
    for (;;) {
        struct pollfd wait = {.fd = fd, .events = POLLIN};
        int ready = poll(&wait, 1, 10000);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            g_byte_array_unref(bytes);
            return nullptr;
        }
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            g_byte_array_unref(bytes);
            return nullptr;
        }
        if (got == 0)
            return bytes;
        g_byte_array_append(bytes, chunk, (guint)got);
    }
}

static gboolean outgoing_sent(gpointer data) {
    dnd_relay *relay = data;
    relay->sending = false;
    /* Dropped on the host: here it ends without a drop (a copy). */
    xd_drag_relay_v1_outgoing_cancel(relay->drag_relay);
    wl_display_flush(relay->display);
    return G_SOURCE_REMOVE;
}

/* On a thread of its own: files take a while, and the host waits. */
static gpointer send_outgoing(gpointer data) {
    outgoing_send *send = data;
    GByteArray *bytes = read_all(send->fd);
    close(send->fd);
    if (bytes == nullptr) {
        xd_dnd_report_error(send->relay->hgcm, XD_VERR_TIMEOUT);
    } else if (g_ascii_strcasecmp(send->mime, XD_DND_URI_LIST) == 0) {
        xd_dnd_send_files(send->relay->hgcm, (const char *)bytes->data, bytes->len);
    } else {
        xd_dnd_send_data(send->relay->hgcm, send->mime, bytes->data, bytes->len);
    }
    if (bytes != nullptr)
        g_byte_array_unref(bytes);
    host_answered(send->relay);
    g_main_context_invoke(nullptr, outgoing_sent, send->relay);
    g_free(send->mime);
    g_free(send);
    return nullptr;
}

/* Dropped on the host: its data, from the program, to the host. */
static void take_outgoing(dnd_relay *relay, const xd_dnd_event *event) {
    int pipe_fds[2];
    if (relay->outgoing_types == nullptr || relay->sending || event->mime == nullptr ||
        pipe2(pipe_fds, O_CLOEXEC) != 0) {
        xd_dnd_report_error(relay->hgcm, XD_VERR_NOT_FOUND);
        return;
    }
    relay->sending = true;
    relay->held = true; /* until the send thread has sent it all */
    xd_drag_relay_v1_outgoing_receive(relay->drag_relay, event->mime, pipe_fds[1]);
    wl_display_flush(relay->display);
    close(pipe_fds[1]);
    outgoing_send *send = g_new0(outgoing_send, 1);
    send->relay = relay;
    send->mime = g_strdup(event->mime);
    send->fd = pipe_fds[0];
    g_thread_unref(g_thread_new("vboxdnd-send", send_outgoing, send));
}

/* ---- host to guest --------------------------------------------------------------------- */

/* The host's drag mustn't hold the pointer here if the host goes quiet:
 * no data long after the drop, or no word for a while before it. */
static gboolean watch_drag(gpointer data) {
    dnd_relay *relay = data;
    gint64 now = g_get_monotonic_time();
    if (relay->dropping && now - relay->data_asked > DATA_WAIT_US) {
        /* Not told to the host: a call now would cost it the wait for
         * its next message (see host_turn). */
        note("the dropped data never came from the host; the drag ends");
    } else if (!relay->dropping && !relay->awaiting_accept && now - relay->last_heard > QUIET_US) {
        note("the host said nothing more of its drag; it ends");
    } else {
        return G_SOURCE_CONTINUE;
    }
    relay->watchdog = 0; /* this source ends with G_SOURCE_REMOVE */
    end_drag(relay);
    return G_SOURCE_REMOVE;
}

/* A drag entered: start ours, offering the host's formats. */
static void start_drag(dnd_relay *relay, const xd_dnd_event *event) {
    end_drag(relay);
    g_autofree char *formats = g_strjoinv(", ", event->formats);
    note("the host's drag entered (%s; actions %u)", formats, event->allowed_actions);
    relay->last_heard = g_get_monotonic_time();
    relay->watchdog = g_timeout_add_seconds(1, watch_drag, relay);
    relay->source = wl_data_device_manager_create_data_source(relay->data_manager);
    wl_data_source_add_listener(relay->source, &source_listener, relay);
    for (char **format = event->formats; format != nullptr && *format != nullptr; format++) {
        if (strchr(*format, '/') != nullptr)
            wl_data_source_offer(relay->source, *format);
    }
    uint32_t actions = 0;
    if (event->allowed_actions & XD_DND_COPY)
        actions |= WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    if (event->allowed_actions & XD_DND_MOVE)
        actions |= WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    wl_data_source_set_actions(relay->source,
                               actions != 0 ? actions : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    /* No button is held; the compositor lets its relay start anyway. */
    wl_data_device_start_drag(relay->data_device, relay->source, relay->origin, nullptr, 0);
}

static void move_to(dnd_relay *relay, const xd_dnd_event *event) {
    xd_drag_relay_v1_move(relay->drag_relay, event->x, event->y);
    wl_display_flush(relay->display);
}

/* The format to ask the host for: files if it has them, else text. */
static const char *wanted_format(char **formats) {
    const char *text = nullptr;
    for (char **format = formats; format != nullptr && *format != nullptr; format++) {
        if (g_ascii_strcasecmp(*format, XD_DND_URI_LIST) == 0)
            return *format;
        if (text == nullptr && g_str_has_prefix(*format, "text/plain"))
            text = *format;
    }
    return text != nullptr ? text : (formats != nullptr ? formats[0] : nullptr);
}

static gboolean handle_event(gpointer data) {
    posted *post = data;
    dnd_relay *relay = post->owner;
    xd_dnd_event *event = post->event;
    relay->last_heard = g_get_monotonic_time();
    xd_hgcm_trace("event %d at %d, %d (default %u, allowed %u)", event->kind, event->x, event->y,
                  event->default_action, event->allowed_actions);
    int rc = XD_VINF_SUCCESS;
    switch (event->kind) {
    case XD_DND_EVENT_ENTER: {
        start_drag(relay, event);
        move_to(relay, event);
        /* The target here hasn't had time to answer; refused, the host's
         * window (Qt's) would hear nothing more of the drag. So yes, with
         * the host's own action: the moves then get the target's answer. */
        uint32_t action = event->default_action & event->allowed_actions;
        if (action == XD_DND_IGNORE)
            action = event->allowed_actions & XD_DND_COPY ? XD_DND_COPY : XD_DND_MOVE;
        rc = xd_dnd_ack(relay->hgcm, action);
        break;
    }
    case XD_DND_EVENT_MOVE:
        if (relay->source == nullptr)
            start_drag(relay, event);
        move_to(relay, event);
        /* What the target last said; it answers the move in its own time. */
        rc = xd_dnd_ack(relay->hgcm, vbox_action(relay));
        break;
    case XD_DND_EVENT_LEAVE:
        note("the host's drag left the window");
        end_drag(relay);
        rc = xd_dnd_ack(relay->hgcm, XD_DND_IGNORE);
        break;
    case XD_DND_EVENT_DROPPED: {
        move_to(relay, event);
        const char *format = wanted_format(event->formats);
        if (relay->source == nullptr || vbox_action(relay) == XD_DND_IGNORE || format == nullptr) {
            note("dropped where nothing takes it (at %d, %d)", event->x, event->y);
            rc = xd_dnd_ack(relay->hgcm, XD_DND_IGNORE);
            end_drag(relay);
            break;
        }
        relay->dropping = true;
        relay->data_asked = g_get_monotonic_time();
        rc = xd_dnd_request_data(&relay->session, format);
        note("dropped at %d, %d: asked the host for %s (%d)", event->x, event->y, format, rc);
        if (rc < 0)
            end_drag(relay);
        break;
    }
    case XD_DND_EVENT_DATA:
        if (relay->source == nullptr || !relay->dropping)
            break;
        note("the dropped data came (%s, %zu bytes)", event->mime, g_bytes_get_size(event->data));
        relay->data = g_bytes_ref(event->data);
        relay->data_mime = g_strdup(event->mime);
        relay->dropping = false;
        /* Everything's here. Toolkits keep what an offer first gave them
         * (nothing, while it hovered): offered again, the target asks for
         * it anew and gets it; dropped once it takes it. */
        relay->accepted = false;
        relay->action = 0;
        relay->awaiting_accept = true;
        relay->drop_timeout = g_timeout_add(3000, drop_timed_out, relay);
        xd_drag_relay_v1_refocus(relay->drag_relay);
        wl_display_flush(relay->display);
        break;
    case XD_DND_EVENT_FAILED:
        note("receiving the dropped data failed (%d)", event->rc);
        end_drag(relay);
        break;
    case XD_DND_EVENT_CANCELLED:
        note("the host cancelled");
        end_drag(relay);
        break;
    case XD_DND_EVENT_PENDING:
        answer_pending(relay);
        break;
    case XD_DND_EVENT_TAKEN:
        note("a drag from here was dropped on the host, which wants %s", event->mime);
        take_outgoing(relay, event);
        break;
    case XD_DND_EVENT_QUIT:
        note("the host's drag and drop service went away");
        g_main_loop_quit(relay->loop);
        break;
    }
    if (rc < 0)
        note("the host refused an answer (%d)", rc);
    if (relay->held)
        relay->held = false;
    else if (event->kind != XD_DND_EVENT_QUIT)
        host_answered(relay);
    return G_SOURCE_REMOVE;
}

static void free_posted(gpointer data) {
    posted *post = data;
    xd_dnd_event_free(post->event);
    g_free(post);
}

static gpointer worker(gpointer data) {
    dnd_relay *relay = data;
    for (;;) {
        xd_dnd_event *event = xd_dnd_session_next(&relay->session);
        bool quit = event->kind == XD_DND_EVENT_QUIT;
        posted *post = g_new0(posted, 1);
        post->owner = relay;
        post->event = event;
        g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, handle_event, post, free_posted);
        if (!quit) {
            g_mutex_lock(&relay->worker_lock);
            while (!relay->answered && !relay->quitting)
                g_cond_wait(&relay->host_turn, &relay->worker_lock);
            relay->answered = false;
            quit = relay->quitting;
            g_mutex_unlock(&relay->worker_lock);
        }
        if (quit) {
            g_mutex_lock(&relay->worker_lock);
            relay->worker_done = true;
            g_cond_signal(&relay->worker_ended);
            g_mutex_unlock(&relay->worker_lock);
            return nullptr;
        }
    }
}

/* Waits up to `timeout_us` for the host thread to end. */
static bool worker_ended(dnd_relay *relay, gint64 timeout_us) {
    gint64 until = g_get_monotonic_time() + timeout_us;
    g_mutex_lock(&relay->worker_lock);
    while (!relay->worker_done) {
        if (!g_cond_wait_until(&relay->worker_ended, &relay->worker_lock, until))
            break;
    }
    bool done = relay->worker_done;
    g_mutex_unlock(&relay->worker_lock);
    return done;
}

/* ---- Wayland ----------------------------------------------------------------------------- */

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t version) {
    dnd_relay *relay = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        relay->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (strcmp(interface, wl_seat_interface.name) == 0 && relay->seat == nullptr)
        relay->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0)
        relay->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface,
                                               MIN(version, 3u));
    else if (strcmp(interface, xd_drag_relay_manager_v1_interface.name) == 0)
        relay->relay_manager =
            wl_registry_bind(registry, name, &xd_drag_relay_manager_v1_interface, 1);
}

static void handle_global_remove([[maybe_unused]] void *data,
                                 [[maybe_unused]] struct wl_registry *registry,
                                 [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static gboolean on_display([[maybe_unused]] gint fd, GIOCondition condition, gpointer data) {
    dnd_relay *relay = data;
    if ((condition & (G_IO_ERR | G_IO_HUP)) != 0 || wl_display_dispatch(relay->display) < 0) {
        g_main_loop_quit(relay->loop);
        return G_SOURCE_REMOVE;
    }
    wl_display_flush(relay->display);
    return G_SOURCE_CONTINUE;
}

static gboolean on_signal(gpointer data) {
    dnd_relay *relay = data;
    g_main_loop_quit(relay->loop);
    return G_SOURCE_CONTINUE;
}

static bool connect_wayland(dnd_relay *relay) {
    relay->display = wl_display_connect(nullptr);
    if (relay->display == nullptr)
        return false;
    relay->registry = wl_display_get_registry(relay->display);
    wl_registry_add_listener(relay->registry, &registry_listener, relay);
    wl_display_roundtrip(relay->display);
    if (relay->compositor == nullptr || relay->seat == nullptr || relay->data_manager == nullptr ||
        relay->relay_manager == nullptr)
        return false;
    relay->data_device = wl_data_device_manager_get_data_device(relay->data_manager, relay->seat);
    relay->drag_relay = xd_drag_relay_manager_v1_get_relay(relay->relay_manager);
    relay->outgoing_next = g_ptr_array_new_with_free_func(g_free);
    xd_drag_relay_v1_add_listener(relay->drag_relay, &relay_listener, relay);
    relay->origin = wl_compositor_create_surface(relay->compositor);
    wl_display_roundtrip(relay->display);
    return true;
}

int main(int argc, char **argv) {
    /* Never outlive the compositor that started us (see above). */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        return 0;
    const char *script = nullptr;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--fake-host") == 0)
            script = argv[i + 1];
    }
    static dnd_relay relay;
    int rc = XD_VINF_SUCCESS;
    if (script != nullptr) {
        GError *error = nullptr;
        relay.hgcm = xd_fake_host_open(script, &error);
        if (relay.hgcm == nullptr) {
            fprintf(stderr, "xd-vboxdnd: %s\n", error->message);
            g_error_free(error);
            return 1;
        }
    } else {
        g_autofree char *trace = g_build_filename(g_get_user_runtime_dir(), "xd-vboxdnd.trace", nullptr);
        xd_hgcm_trace_to(trace);
        relay.hgcm = xd_hgcm_open(XD_DND_SERVICE, &rc);
        if (relay.hgcm == nullptr) {
            /* Not a VirtualBox guest, or no drag and drop service: nothing to do. */
            fprintf(stderr, "xd-vboxdnd: no VirtualBox drag and drop service (%d)\n", rc);
            return 0;
        }
    }
    g_autofree char *drop_base =
        g_build_filename(g_get_tmp_dir(), "VirtualBox Dropped Files", nullptr);
    rc = xd_dnd_session_init(&relay.session, relay.hgcm, drop_base);
    if (rc < 0) {
        fprintf(stderr, "xd-vboxdnd: the host's drag and drop didn't connect (%d)\n", rc);
        relay.hgcm->free(relay.hgcm);
        return 0;
    }
    if (!connect_wayland(&relay)) {
        fprintf(stderr, "xd-vboxdnd: the compositor offers no drag relay\n");
        relay.hgcm->free(relay.hgcm);
        return 1;
    }
    relay.loop = g_main_loop_new(nullptr, FALSE);
    GIOCondition readable = (GIOCondition)(G_IO_IN | G_IO_ERR | G_IO_HUP);
    g_unix_fd_add(wl_display_get_fd(relay.display), readable, on_display, &relay);
    g_unix_signal_add(SIGTERM, on_signal, &relay);
    g_unix_signal_add(SIGINT, on_signal, &relay);
    g_mutex_init(&relay.worker_lock);
    g_cond_init(&relay.worker_ended);
    g_cond_init(&relay.host_turn);
    relay.worker = g_thread_new("vboxdnd-host", worker, &relay);
    g_main_loop_run(relay.loop);

    /* The host thread waits on the host. Disconnecting should wake it, but
     * the guest driver may leave it waiting: then the process just ends,
     * and closing the device lets the host forget this client (a client
     * left waiting would take the host's next drags, unanswered). */
    g_mutex_lock(&relay.worker_lock);
    relay.quitting = true;
    g_cond_signal(&relay.host_turn);
    g_mutex_unlock(&relay.worker_lock);
    relay.hgcm->disconnect(relay.hgcm);
    if (!worker_ended(&relay, G_USEC_PER_SEC)) {
        note("leaving the host's drag and drop service");
        fflush(stderr);
        _exit(0);
    }
    g_thread_join(relay.worker);
    end_drag(&relay);
    xd_dnd_session_finish(&relay.session);
    relay.hgcm->free(relay.hgcm);
    wl_display_disconnect(relay.display);
    g_main_loop_unref(relay.loop);
    return 0;
}
