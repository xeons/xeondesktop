/* SPDX-License-Identifier: MIT
 * A scripted VirtualBox host for tests (xd-vboxdnd --fake-host SCRIPT):
 * it answers the drag and drop service's calls as the host does, from a
 * script of what the user does on the host, and logs what the guest
 * answers to SCRIPT.log. One step a line:
 *
 *   wait MS              pause before the next message
 *   until PATH           wait for PATH to exist; "X Y" in it is the point @
 *   enter X Y [FORMATS]  a drag enters the VM's window (FORMATS |-separated;
 *                        text/uri-list by default), at device pixels X, Y
 *                        (or @)
 *   move X Y             it moves
 *   leave                it leaves without a drop
 *   drop X Y             it's let go there
 *   dir NAME             what's dragged: a directory, a file (the rest of
 *   file NAME TEXT       the line its contents), or text; sent, as the host
 *   text TEXT            does, when the guest asks for the drop's data
 *   cancel               the host cancels (a waiting GET_NEXT returns it)
 *   pending              the host asks whether a guest drag is going on
 *   dropped FORMAT       a guest drag, reported pending, was dropped on the
 *                        host, which wants FORMAT; what the guest sends is
 *                        logged ("send ...")
 *
 * Like the real host, a fetch must match the message waiting: its
 * number, parameter types, and buffers big enough; anything else is
 * logged as an error. And as its window (Qt's) does, a drag whose ENTER
 * is answered with no action gets no more: its moves and drop are
 * skipped ("enter refused").
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dnd.h"
#include "fakehost.h"

typedef struct message {
    uint32_t id;
    GArray *parms; /* xd_hgcm_parm: buffers point into `data` */
    GPtrArray *data;
} message;

typedef struct fake_object {
    bool dir;
    char *name;
    GBytes *contents;
} fake_object;

typedef struct fake_host {
    xd_hgcm base;
    GMutex lock;
    GCond changed;
    char **steps;
    size_t step;
    gint64 ready_at; /* monotonic µs: when the next scripted step may go */
    GQueue queue;    /* message: generated, waiting to be fetched */
    message *current; /* returned by GET_NEXT, not fetched yet */
    GPtrArray *objects; /* fake_object */
    char *text;
    bool disconnected;
    FILE *log;
    int point_x, point_y; /* @, from an until step's file */
    uint32_t last_fetched; /* the last message the guest fetched */
    bool refused;          /* its ENTER answered with no action */
    bool waiting;          /* a GET_NEXT is waiting for a message */
    bool early;            /* the first object goes before the header */
} fake_host;

static void message_free(message *msg) {
    if (msg == nullptr)
        return;
    g_array_unref(msg->parms);
    g_ptr_array_unref(msg->data);
    g_free(msg);
}

static message *message_new(uint32_t id) {
    message *msg = g_new0(message, 1);
    msg->id = id;
    msg->parms = g_array_new(FALSE, FALSE, sizeof(xd_hgcm_parm));
    msg->data = g_ptr_array_new_with_free_func(g_free);
    return msg;
}

static void add_u32(message *msg, uint32_t value) {
    xd_hgcm_parm parm = xd_hgcm_u32(value);
    g_array_append_val(msg->parms, parm);
}

static void add_u64(message *msg, uint64_t value) {
    xd_hgcm_parm parm = xd_hgcm_u64(value);
    g_array_append_val(msg->parms, parm);
}

static void add_buffer(message *msg, const void *bytes, uint32_t size) {
    void *copy = g_memdup2(bytes, size);
    g_ptr_array_add(msg->data, copy);
    xd_hgcm_parm parm = xd_hgcm_buffer(copy, size);
    g_array_append_val(msg->parms, parm);
}

static void log_line(fake_host *host, const char *format, ...) G_GNUC_PRINTF(2, 3);
static void log_line(fake_host *host, const char *format, ...) {
    if (host->log == nullptr)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(host->log, format, args);
    va_end(args);
    fputc('\n', host->log);
    fflush(host->log);
}

/* ENTER, MOVE and DROPPED: ctx, screen, x, y, default and allowed actions,
 * formats. */
static message *position_message(uint32_t id, int x, int y, const char *formats) {
    message *msg = message_new(id);
    g_auto(GStrv) parts = g_strsplit(formats, "|", -1);
    g_autofree char *list = xd_dnd_formats_join((const char *const *)parts);
    add_u32(msg, 0);
    add_u32(msg, 0);
    add_u32(msg, (uint32_t)x);
    add_u32(msg, (uint32_t)y);
    add_u32(msg, XD_DND_COPY);
    add_u32(msg, XD_DND_COPY | XD_DND_MOVE);
    add_buffer(msg, list, (uint32_t)strlen(list) + 1);
    add_u32(msg, (uint32_t)strlen(list) + 1);
    return msg;
}

static void queue_chunks(fake_host *host, uint32_t id, const void *bytes, size_t size) {
    const unsigned char *at = bytes;
    for (size_t sent = 0; sent < size;) {
        uint32_t chunk = (uint32_t)MIN(size - sent, (size_t)XD_DND_CHUNK);
        message *msg = message_new(id);
        add_u32(msg, 0);
        add_buffer(msg, at + sent, chunk);
        add_u32(msg, chunk);
        add_buffer(msg, "", 0);
        add_u32(msg, 0);
        g_queue_push_tail(&host->queue, msg);
        sent += chunk;
    }
}

/* What the guest asked for at the drop, as the host sends it. */
static void queue_transfer(fake_host *host) {
    GString *meta = g_string_new(nullptr);
    uint64_t file_bytes = 0;
    bool files = host->objects->len > 0;
    if (files) {
        for (guint i = 0; i < host->objects->len; i++) {
            const fake_object *object = g_ptr_array_index(host->objects, i);
            file_bytes += object->dir ? 0 : g_bytes_get_size(object->contents);
            if (strchr(object->name, '/') != nullptr)
                continue; /* not a root */
            g_autofree char *escaped =
                g_uri_escape_string(object->name, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
            g_string_append_printf(meta, "file:///%s%s\r\n", escaped, object->dir ? "/" : "");
        }
    } else {
        g_string_append(meta, host->text != nullptr ? host->text : "");
    }
    uint32_t meta_size = (uint32_t)meta->len + 1; /* its NUL too */
    const char *format = files ? XD_DND_URI_LIST : "text/plain;charset=utf-8";
    message *header = message_new(XD_DND_HOST_HG_DATA_HEADER);
    add_u32(header, 0);
    add_u32(header, 0);
    add_u32(header, 0);
    add_u64(header, meta_size + file_bytes);
    add_u32(header, meta_size);
    add_buffer(header, format, (uint32_t)strlen(format) + 1);
    add_u32(header, (uint32_t)strlen(format) + 1);
    add_u64(header, files ? host->objects->len : 0);
    add_u32(header, 0);
    add_u32(header, 0);
    add_buffer(header, "", 0);
    add_u32(header, 0);
    g_queue_push_tail(&host->queue, header);
    queue_chunks(host, XD_DND_HOST_HG_DATA, meta->str, meta_size);
    g_string_free(meta, TRUE);
    guint before_objects = g_queue_get_length(&host->queue);
    for (guint i = 0; i < host->objects->len; i++) {
        const fake_object *object = g_ptr_array_index(host->objects, i);
        message *msg = message_new(object->dir ? XD_DND_HOST_HG_DIR : XD_DND_HOST_HG_FILE_HEADER);
        add_u32(msg, 0);
        add_buffer(msg, object->name, (uint32_t)strlen(object->name) + 1);
        add_u32(msg, (uint32_t)strlen(object->name) + 1);
        if (object->dir) {
            add_u32(msg, 040755);
            g_queue_push_tail(&host->queue, msg);
            continue;
        }
        gsize size;
        const void *bytes = g_bytes_get_data(object->contents, &size);
        add_u32(msg, 0);
        add_u32(msg, 0100644);
        add_u64(msg, size);
        g_queue_push_tail(&host->queue, msg);
        queue_chunks(host, XD_DND_HOST_HG_FILE_DATA, bytes, size);
    }
    /* VirtualBox 7.2 hands out its objects as the guest asks for its next
     * message, and the header only once queued: a guest that asks right
     * after its request may get the first object (and its data) first. */
    if (host->early && host->objects->len > 0) {
        guint moved = 0;
        for (GList *at = g_queue_peek_nth_link(&host->queue, before_objects); at != nullptr;
             at = at->next, moved++) {
            const message *msg = at->data;
            bool object = msg->id == XD_DND_HOST_HG_DIR || msg->id == XD_DND_HOST_HG_FILE_HEADER;
            if (object && moved > 0)
                break;
        }
        for (guint i = 0; i < moved; i++) {
            message *msg = g_queue_pop_nth(&host->queue, before_objects + i);
            g_queue_push_nth(&host->queue, msg, (gint)i);
        }
    }
}

/* Runs script steps until one makes a message (or it waits, or ends).
 * Called with the lock held. */
static void advance(fake_host *host) {
    while (g_queue_is_empty(&host->queue) && host->steps[host->step] != nullptr) {
        if (g_get_monotonic_time() < host->ready_at)
            return;
        const char *line = g_strstrip(host->steps[host->step++]);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        char word[16] = "";
        int x = 0, y = 0;
        char rest[4096] = "";
        sscanf(line, "%15s", word);
        const char *after = line + strlen(word);
        while (*after == ' ')
            after++;
        if (strcmp(word, "wait") == 0) {
            host->ready_at = g_get_monotonic_time() + g_ascii_strtoll(after, nullptr, 10) * 1000;
        } else if (strcmp(word, "until") == 0) {
            g_autofree char *point = nullptr;
            if (!g_file_get_contents(after, &point, nullptr, nullptr)) {
                /* Not yet: this step again in a moment. */
                host->step--;
                host->ready_at = g_get_monotonic_time() + 100 * 1000;
                continue;
            }
            sscanf(point, "%d %d", &host->point_x, &host->point_y);
        } else if (strcmp(word, "enter") == 0 || strcmp(word, "move") == 0 ||
                   strcmp(word, "drop") == 0) {
            int fields;
            if (after[0] == '@') {
                x = host->point_x;
                y = host->point_y;
                fields = 2 + sscanf(after + 1, "%4095s", rest);
            } else {
                fields = sscanf(after, "%d %d %4095s", &x, &y, rest);
            }
            const char *formats = fields == 3 ? rest : XD_DND_URI_LIST;
            uint32_t id = word[0] == 'e'   ? XD_DND_HOST_HG_ENTER
                          : word[0] == 'm' ? XD_DND_HOST_HG_MOVE
                                           : XD_DND_HOST_HG_DROPPED;
            if (id == XD_DND_HOST_HG_ENTER)
                host->refused = false;
            else if (host->refused)
                continue; /* Qt sends a refused drag nothing more */
            g_queue_push_tail(&host->queue, position_message(id, x, y, formats));
        } else if (strcmp(word, "leave") == 0) {
            message *msg = message_new(XD_DND_HOST_HG_LEAVE);
            add_u32(msg, 0);
            g_queue_push_tail(&host->queue, msg);
        } else if (strcmp(word, "pending") == 0) {
            message *msg = message_new(XD_DND_HOST_GH_PENDING);
            add_u32(msg, 0);
            add_u32(msg, 0);
            g_queue_push_tail(&host->queue, msg);
        } else if (strcmp(word, "dropped") == 0) {
            message *msg = message_new(XD_DND_HOST_GH_DROPPED);
            add_u32(msg, 0);
            add_buffer(msg, after, (uint32_t)strlen(after) + 1);
            add_u32(msg, (uint32_t)strlen(after) + 1);
            add_u32(msg, XD_DND_COPY);
            g_queue_push_tail(&host->queue, msg);
        } else if (strcmp(word, "early") == 0) {
            host->early = true;
        } else if (strcmp(word, "cancel") == 0) {
            message *msg = message_new(XD_DND_HOST_CANCEL);
            add_u32(msg, 0);
            g_queue_push_tail(&host->queue, msg);
        } else if (strcmp(word, "dir") == 0 || strcmp(word, "file") == 0 ||
                   strcmp(word, "text") == 0) {
            /* What's dragged: read with the script (read_dragged). */
        } else {
            log_line(host, "script: unknown step '%s'", line);
        }
    }
}

static gint64 next_wakeup(const fake_host *host) {
    return host->steps[host->step] != nullptr && host->ready_at > 0 ? host->ready_at
                                                                      : G_MAXINT64;
}

/* GET_NEXT_HOST_MSG: waits for a message, then says which. */
static int wait_for_message(fake_host *host, xd_hgcm_parm *parms) {
    for (;;) {
        if (host->disconnected)
            return XD_VERR_CANCELLED;
        advance(host);
        message *head = g_queue_peek_head(&host->queue);
        if (head != nullptr && head->id == XD_DND_HOST_CANCEL) {
            /* As the host does: a waiting GET_NEXT completes, cancelled. */
            message_free(g_queue_pop_head(&host->queue));
            parms[0].u32 = XD_DND_HOST_CANCEL;
            parms[1].u32 = 1;
            log_line(host, "cancelled");
            return XD_VERR_CANCELLED;
        }
        if (head != nullptr) {
            host->current = g_queue_pop_head(&host->queue);
            parms[0].u32 = head->id;
            parms[1].u32 = head->parms->len;
            return XD_VINF_SUCCESS;
        }
        gint64 wake = next_wakeup(host);
        if (wake == G_MAXINT64)
            g_cond_wait(&host->changed, &host->lock);
        else
            g_cond_wait_until(&host->changed, &host->lock, wake);
    }
}

/* Any other call while it waits, the real host forgets the wait (its
 * client is completed, the wait no longer marked): the guest must not. */
static int get_next(fake_host *host, xd_hgcm_parm *parms, uint32_t count) {
    if (count < 3)
        return XD_VERR_INVALID_PARAMETER;
    host->waiting = true;
    int rc = wait_for_message(host, parms);
    host->waiting = false;
    return rc;
}

/* Fetching the message GET_NEXT said: checked as the host checks. */
static int fetch(fake_host *host, uint32_t function, xd_hgcm_parm *parms, uint32_t count) {
    message *msg = host->current;
    if (msg != nullptr)
        host->last_fetched = msg->id;
    if (msg == nullptr || msg->id != function || count < msg->parms->len) {
        log_line(host, "error: fetch %u doesn't match", function);
        return XD_VERR_INVALID_PARAMETER;
    }
    host->current = nullptr;
    for (guint i = 0; i < msg->parms->len; i++) {
        const xd_hgcm_parm *from = &g_array_index(msg->parms, xd_hgcm_parm, i);
        xd_hgcm_parm *to = &parms[i];
        if (to->type != from->type) {
            log_line(host, "error: message %u parameter %u has the wrong type", function, i);
            message_free(msg);
            return XD_VERR_INVALID_PARAMETER;
        }
        if (from->type == XD_HGCM_U32) {
            to->u32 = from->u32;
        } else if (from->type == XD_HGCM_U64) {
            to->u64 = from->u64;
        } else if (to->size < from->size) {
            log_line(host, "error: message %u parameter %u's buffer is too small", function, i);
            message_free(msg);
            return XD_VERR_BUFFER_OVERFLOW;
        } else if (from->size > 0) {
            memcpy(to->buffer, from->buffer, from->size);
        }
    }
    message_free(msg);
    return XD_VINF_SUCCESS;
}

/* A buffer and the length that goes with it must agree, as the host
 * checks; logged as an error otherwise. */
static bool sized(fake_host *host, const xd_hgcm_parm *parms, uint32_t buffer, uint32_t length) {
    if (parms[buffer].type == XD_HGCM_BUFFER && parms[length].type == XD_HGCM_U32 &&
        parms[buffer].size == parms[length].u32)
        return true;
    log_line(host, "error: a buffer and its length disagree");
    return false;
}

static char *escaped(const xd_hgcm_parm *parm, uint32_t size) {
    g_autofree char *text = g_strndup(parm->buffer != nullptr ? parm->buffer : "", size);
    return g_strescape(text, nullptr);
}

/* What the guest sends of a drop on the host. */
static int guest_sends(fake_host *host, uint32_t function, xd_hgcm_parm *parms, uint32_t count) {
    if (function == XD_DND_GUEST_GH_DATA_HEADER && count == 12 && sized(host, parms, 5, 6)) {
        g_autofree char *format = escaped(&parms[5], parms[6].u32);
        log_line(host, "send header total %lu meta %u format %s objects %lu",
                 (unsigned long)parms[3].u64, parms[4].u32, format, (unsigned long)parms[7].u64);
        return XD_VINF_SUCCESS;
    }
    if ((function == XD_DND_GUEST_GH_DATA || function == XD_DND_GUEST_GH_FILE_DATA) &&
        count == 5 && sized(host, parms, 1, 2) && parms[2].u32 <= XD_DND_CHUNK) {
        g_autofree char *data = escaped(&parms[1], parms[2].u32);
        log_line(host, "send %s %s", function == XD_DND_GUEST_GH_DATA ? "data" : "file-data",
                 data);
        return XD_VINF_SUCCESS;
    }
    if (function == XD_DND_GUEST_GH_DIR && count == 4 && sized(host, parms, 1, 2)) {
        log_line(host, "send dir %s", (const char *)parms[1].buffer);
        return XD_VINF_SUCCESS;
    }
    if (function == XD_DND_GUEST_GH_FILE_HEADER && count == 6 && sized(host, parms, 1, 2)) {
        if (parms[4].u32 == 0)
            log_line(host, "error: a file's mode is 0");
        log_line(host, "send file %s %lu", (const char *)parms[1].buffer,
                 (unsigned long)parms[5].u64);
        return XD_VINF_SUCCESS;
    }
    log_line(host, "error: guest function %u isn't right", function);
    return XD_VERR_INVALID_PARAMETER;
}

static int fake_call(xd_hgcm *hgcm, uint32_t function, xd_hgcm_parm *parms, uint32_t count) {
    fake_host *host = (fake_host *)hgcm;
    g_mutex_lock(&host->lock);
    int rc = XD_VINF_SUCCESS;
    if (function != XD_DND_GUEST_GET_NEXT && host->waiting)
        log_line(host, "error: call %u while GET_NEXT waits (the host forgets that wait)", function);
    if (host->disconnected) {
        rc = XD_VERR_CANCELLED;
    } else if (function == XD_DND_GUEST_CONNECT) {
        if (count >= 2)
            parms[1].u32 = MIN(parms[1].u32, 3u);
        log_line(host, "connect");
    } else if (function == XD_DND_GUEST_REPORT_FEATURES) {
        if (count >= 2)
            parms[1].u64 = 0;
    } else if (function == XD_DND_GUEST_GET_NEXT) {
        rc = get_next(host, parms, count);
    } else if (function == XD_DND_GUEST_HG_ACK_OP) {
        uint32_t action = count >= 2 ? parms[1].u32 : 0;
        log_line(host, "ack %u", action);
        if (host->last_fetched == XD_DND_HOST_HG_ENTER && action == XD_DND_IGNORE) {
            host->refused = true;
            log_line(host, "enter refused");
        }
    } else if (function == XD_DND_GUEST_HG_REQ_DATA) {
        log_line(host, "request %s", count >= 2 ? (const char *)parms[1].buffer : "?");
        queue_transfer(host);
        g_cond_broadcast(&host->changed);
    } else if (function == XD_DND_GUEST_GH_ACK_PENDING) {
        g_autofree char *formats =
            count >= 4 ? g_strescape((const char *)parms[3].buffer, nullptr) : g_strdup("?");
        log_line(host, "pending %u %u %s", parms[1].u32, parms[2].u32, formats);
    } else if (function == XD_DND_GUEST_ERROR) {
        log_line(host, "guest error %d", count >= 2 ? (int)parms[1].u32 : 0);
    } else if (function == XD_DND_GUEST_GH_DATA_HEADER || function == XD_DND_GUEST_GH_DATA ||
               function == XD_DND_GUEST_GH_DIR || function == XD_DND_GUEST_GH_FILE_HEADER ||
               function == XD_DND_GUEST_GH_FILE_DATA) {
        rc = guest_sends(host, function, parms, count);
    } else {
        rc = fetch(host, function, parms, count);
    }
    g_mutex_unlock(&host->lock);
    return rc;
}

static void fake_disconnect(xd_hgcm *hgcm) {
    fake_host *host = (fake_host *)hgcm;
    g_mutex_lock(&host->lock);
    host->disconnected = true;
    g_cond_broadcast(&host->changed);
    g_mutex_unlock(&host->lock);
}

static void object_free(gpointer data) {
    fake_object *object = data;
    g_free(object->name);
    g_bytes_unref(object->contents);
    g_free(object);
}

static void fake_free(xd_hgcm *hgcm) {
    fake_host *host = (fake_host *)hgcm;
    fake_disconnect(hgcm);
    g_queue_clear_full(&host->queue, (GDestroyNotify)message_free);
    message_free(host->current);
    g_ptr_array_unref(host->objects);
    g_strfreev(host->steps);
    g_free(host->text);
    if (host->log != nullptr)
        fclose(host->log);
    g_mutex_clear(&host->lock);
    g_cond_clear(&host->changed);
    g_free(host);
}

/* What's dragged, from the script's dir, file and text lines, wherever
 * they are: the host has it all from the start. */
static void read_dragged(fake_host *host) {
    for (char **step = host->steps; *step != nullptr; step++) {
        const char *line = *step;
        while (*line == ' ')
            line++;
        bool dir = g_str_has_prefix(line, "dir "), file = g_str_has_prefix(line, "file ");
        if (g_str_has_prefix(line, "text ")) {
            g_free(host->text);
            host->text = g_strdup(line + strlen("text "));
            continue;
        }
        if (!dir && !file)
            continue;
        const char *after = line + (dir ? strlen("dir ") : strlen("file "));
        fake_object *object = g_new0(fake_object, 1);
        object->dir = dir;
        const char *space = strchr(after, ' ');
        object->name =
            space != nullptr ? g_strndup(after, (gsize)(space - after)) : g_strdup(after);
        const char *contents = space != nullptr ? space + 1 : "";
        object->contents = g_bytes_new(contents, strlen(contents));
        g_ptr_array_add(host->objects, object);
    }
}

xd_hgcm *xd_fake_host_open(const char *script, GError **error) {
    g_autofree char *text = nullptr;
    if (!g_file_get_contents(script, &text, nullptr, error))
        return nullptr;
    fake_host *host = g_new0(fake_host, 1);
    host->base = (xd_hgcm){.call = fake_call, .disconnect = fake_disconnect, .free = fake_free};
    g_mutex_init(&host->lock);
    g_cond_init(&host->changed);
    host->steps = g_strsplit(text, "\n", -1);
    host->objects = g_ptr_array_new_with_free_func(object_free);
    read_dragged(host);
    g_queue_init(&host->queue);
    g_autofree char *log_path = g_strconcat(script, ".log", nullptr);
    host->log = fopen(log_path, "w");
    return &host->base;
}
