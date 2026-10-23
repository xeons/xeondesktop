/* SPDX-License-Identifier: MIT */
#include "dnd.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROTOCOL 3
#define FEATURES_MUST_BE_ONE (UINT64_C(1) << 63)
#define SEPARATOR "\r\n"
/* Room for a path the host names (RTPATH_MAX and its terminator). */
#define NAME_MAX_BYTES 4100

/* ---- formats and paths ------------------------------------------------------------------- */

char **xd_dnd_formats_split(const char *list, size_t length) {
    g_autofree char *text = g_strndup(list, length);
    char **parts = g_strsplit(text, SEPARATOR, -1);
    GPtrArray *kept = g_ptr_array_new();
    for (char **part = parts; *part != nullptr; part++) {
        if ((*part)[0] != '\0')
            g_ptr_array_add(kept, g_strdup(*part));
    }
    g_strfreev(parts);
    g_ptr_array_add(kept, nullptr);
    return (char **)g_ptr_array_free(kept, FALSE);
}

char *xd_dnd_formats_join(const char *const *formats) {
    GString *list = g_string_new(nullptr);
    for (const char *const *format = formats; format != nullptr && *format != nullptr; format++)
        g_string_append_printf(list, "%s" SEPARATOR, *format);
    /* Never empty: the host refuses an empty list. */
    if (list->len == 0)
        g_string_append(list, SEPARATOR);
    return g_string_free(list, FALSE);
}

bool xd_dnd_name_valid(const char *name) {
    if (name == nullptr || name[0] == '\0' || name[0] == '/' || !g_utf8_validate(name, -1, nullptr))
        return false;
    g_auto(GStrv) parts = g_strsplit(name, "/", -1);
    for (char **part = parts; *part != nullptr; part++) {
        if (strcmp(*part, "..") == 0 || strcmp(*part, ".") == 0)
            return false;
    }
    return true;
}

/* A name with any trailing slash (a directory's) taken off. */
static char *trimmed_name(const char *name) {
    char *copy = g_strdup(name);
    size_t length = strlen(copy);
    while (length > 0 && copy[length - 1] == '/')
        copy[--length] = '\0';
    return copy;
}

char **xd_dnd_meta_roots(const char *meta, size_t length) {
    g_autofree char *text = g_strndup(meta, length);
    g_auto(GStrv) lines = g_strsplit_set(text, "\r\n", -1);
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    for (char **line = lines; *line != nullptr; line++) {
        if ((*line)[0] == '\0' || (*line)[0] == '#')
            continue;
        if (!g_str_has_prefix(*line, "file://")) {
            g_ptr_array_unref(roots);
            return nullptr;
        }
        /* "file:///a.txt": the path after the (empty) host, its leading
         * slash dropped: relative to the transfer's root. */
        const char *path = *line + strlen("file://");
        g_autofree char *decoded = g_uri_unescape_string(path, nullptr);
        const char *relative = decoded != nullptr ? decoded : "";
        while (*relative == '/')
            relative++;
        g_autofree char *name = trimmed_name(relative);
        if (!xd_dnd_name_valid(name)) {
            g_ptr_array_unref(roots);
            return nullptr;
        }
        g_ptr_array_add(roots, g_steal_pointer(&name));
    }
    g_ptr_array_set_free_func(roots, nullptr);
    g_ptr_array_add(roots, nullptr);
    return (char **)g_ptr_array_free(roots, FALSE);
}

char *xd_dnd_drop_uri_list(const char *drop_dir, char **roots) {
    GString *list = g_string_new(nullptr);
    for (char **root = roots; root != nullptr && *root != nullptr; root++) {
        g_autofree char *path = g_build_filename(drop_dir, *root, nullptr);
        g_autofree char *uri = g_filename_to_uri(path, nullptr, nullptr);
        if (uri != nullptr)
            g_string_append_printf(list, "%s" SEPARATOR, uri);
    }
    return g_string_free(list, FALSE);
}

/* ---- receiving a drop -------------------------------------------------------------------- */

void xd_dnd_receiver_init(xd_dnd_receiver *receiver, const char *base) {
    *receiver = (xd_dnd_receiver){
        .base = g_strdup(base),
        .meta = g_byte_array_new(),
        .file_fd = -1,
        .created = g_ptr_array_new_with_free_func(g_free),
    };
}

static int status_of_errno(int error) {
    switch (error) {
    case ENOENT:
        return XD_VERR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
        return XD_VERR_ACCESS_DENIED;
    case ENOSPC:
        return XD_VERR_DISK_FULL;
    default:
        return XD_VERR_GENERAL_FAILURE;
    }
}

/* A fresh directory for this drop: <base>/<UTC timestamp>, -1, -2... if
 * taken; both levels only the user's. */
static int make_drop_dir(xd_dnd_receiver *receiver) {
    if (g_mkdir_with_parents(receiver->base, 0700) != 0)
        return status_of_errno(errno);
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    g_autofree char *stamp = g_date_time_format_iso8601(now);
    for (int attempt = 0; attempt < 100; attempt++) {
        g_autofree char *name =
            attempt == 0 ? g_strdup(stamp) : g_strdup_printf("%s-%d", stamp, attempt);
        char *path = g_build_filename(receiver->base, name, nullptr);
        if (g_mkdir(path, 0700) == 0) {
            receiver->drop_dir = path;
            g_ptr_array_add(receiver->created, g_strdup(path));
            return XD_VINF_SUCCESS;
        }
        int error = errno;
        g_free(path);
        if (error != EEXIST)
            return status_of_errno(error);
    }
    return XD_VERR_GENERAL_FAILURE;
}

int xd_dnd_receiver_header(xd_dnd_receiver *receiver, uint64_t total, uint32_t meta_size,
                           const char *meta_format, uint64_t objects) {
    xd_dnd_receiver_reset(receiver);
    if (meta_size > total)
        return XD_VERR_INVALID_PARAMETER;
    receiver->total = total;
    receiver->meta_size = meta_size;
    receiver->objects = objects;
    receiver->uri_list = meta_format != nullptr &&
                         (g_ascii_strcasecmp(meta_format, XD_DND_URI_LIST) == 0 ||
                          g_ascii_strcasecmp(meta_format, "x-special/gnome-icon-list") == 0);
    if (!receiver->uri_list)
        return objects == 0 ? XD_VINF_SUCCESS : XD_VERR_INVALID_PARAMETER;
    return make_drop_dir(receiver);
}

int xd_dnd_receiver_data(xd_dnd_receiver *receiver, const void *data, uint32_t size) {
    if (receiver->meta->len + (uint64_t)size > receiver->meta_size)
        return XD_VERR_BUFFER_OVERFLOW;
    g_byte_array_append(receiver->meta, data, size);
    return XD_VINF_SUCCESS;
}

/* The host's relative name joined to the drop directory, checked. */
static char *drop_path(const xd_dnd_receiver *receiver, const char *name) {
    if (receiver->drop_dir == nullptr)
        return nullptr;
    g_autofree char *trimmed = trimmed_name(name);
    if (!xd_dnd_name_valid(trimmed))
        return nullptr;
    return g_build_filename(receiver->drop_dir, trimmed, nullptr);
}

int xd_dnd_receiver_dir(xd_dnd_receiver *receiver, const char *name, uint32_t mode) {
    if (receiver->file_fd >= 0 || receiver->objects_done >= receiver->objects)
        return XD_VERR_INVALID_PARAMETER;
    g_autofree char *path = drop_path(receiver, name);
    if (path == nullptr)
        return XD_VERR_INVALID_PARAMETER;
    /* The host's permissions, but always the user's to fill. */
    if (mkdir(path, (mode_t)((mode & 0777) | 0700)) != 0 && errno != EEXIST)
        return status_of_errno(errno);
    g_ptr_array_add(receiver->created, g_steal_pointer(&path));
    receiver->objects_done++;
    return XD_VINF_SUCCESS;
}

static void close_file(xd_dnd_receiver *receiver) {
    if (receiver->file_fd >= 0)
        close(receiver->file_fd);
    receiver->file_fd = -1;
}

int xd_dnd_receiver_file_header(xd_dnd_receiver *receiver, const char *name, uint32_t mode,
                                uint64_t size) {
    if (receiver->file_fd >= 0 || receiver->objects_done >= receiver->objects ||
        receiver->file_bytes + size > receiver->total - receiver->meta_size)
        return XD_VERR_INVALID_PARAMETER;
    g_autofree char *path = drop_path(receiver, name);
    if (path == nullptr)
        return XD_VERR_INVALID_PARAMETER;
    /* Never through a link the drop itself made. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                  (mode & 0777) | 0600);
    if (fd < 0)
        return status_of_errno(errno);
    g_ptr_array_add(receiver->created, g_steal_pointer(&path));
    receiver->file_fd = fd;
    receiver->file_left = size;
    if (size == 0) {
        close_file(receiver);
        receiver->objects_done++;
    }
    return XD_VINF_SUCCESS;
}

int xd_dnd_receiver_file_data(xd_dnd_receiver *receiver, const void *data, uint32_t size) {
    if (receiver->file_fd < 0 || size > receiver->file_left)
        return XD_VERR_INVALID_PARAMETER;
    const unsigned char *at = data;
    uint32_t left = size;
    while (left > 0) {
        ssize_t written = write(receiver->file_fd, at, left);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return status_of_errno(errno);
        at += written;
        left -= (uint32_t)written;
    }
    receiver->file_left -= size;
    receiver->file_bytes += size;
    if (receiver->file_left == 0) {
        close_file(receiver);
        receiver->objects_done++;
    }
    return XD_VINF_SUCCESS;
}

bool xd_dnd_receiver_complete(const xd_dnd_receiver *receiver) {
    return receiver->meta->len == receiver->meta_size && receiver->file_fd < 0 &&
           receiver->objects_done == receiver->objects &&
           receiver->file_bytes == receiver->total - receiver->meta_size;
}

void xd_dnd_receiver_rollback(xd_dnd_receiver *receiver) {
    close_file(receiver);
    for (guint i = receiver->created->len; i-- > 0;) {
        const char *path = g_ptr_array_index(receiver->created, i);
        if (g_remove(path) != 0)
            g_rmdir(path);
    }
    xd_dnd_receiver_reset(receiver);
}

void xd_dnd_receiver_reset(xd_dnd_receiver *receiver) {
    close_file(receiver);
    g_clear_pointer(&receiver->drop_dir, g_free);
    g_byte_array_set_size(receiver->meta, 0);
    g_ptr_array_set_size(receiver->created, 0);
    receiver->total = receiver->meta_size = receiver->objects = 0;
    receiver->file_bytes = receiver->objects_done = receiver->file_left = 0;
    receiver->uri_list = false;
}

void xd_dnd_receiver_finish(xd_dnd_receiver *receiver) {
    xd_dnd_receiver_reset(receiver);
    g_byte_array_unref(receiver->meta);
    g_ptr_array_unref(receiver->created);
    g_free(receiver->base);
    *receiver = (xd_dnd_receiver){.file_fd = -1};
}

/* ---- the session ----------------------------------------------------------------------- */

void xd_dnd_event_free(xd_dnd_event *event) {
    if (event == nullptr)
        return;
    g_strfreev(event->formats);
    g_free(event->mime);
    if (event->data != nullptr)
        g_bytes_unref(event->data);
    g_free(event);
}

int xd_dnd_session_init(xd_dnd_session *session, xd_hgcm *hgcm, const char *drop_base) {
    *session = (xd_dnd_session){
        .hgcm = hgcm,
        .chunk = g_malloc(XD_DND_CHUNK),
        .formats_buffer = g_malloc(XD_DND_CHUNK),
    };
    g_mutex_init(&session->lock);
    xd_dnd_receiver_init(&session->receiver, drop_base);
    xd_hgcm_parm connect[] = {xd_hgcm_u32(0), xd_hgcm_u32(PROTOCOL), xd_hgcm_u32(0)};
    int rc = hgcm->call(hgcm, XD_DND_GUEST_CONNECT, connect, 3);
    if (rc < 0)
        return rc;
    if (connect[1].u32 < PROTOCOL)
        return XD_VERR_NOT_SUPPORTED;
    /* No features of ours; an older host not knowing the call is fine. */
    xd_hgcm_parm features[] = {xd_hgcm_u64(0), xd_hgcm_u64(FEATURES_MUST_BE_ONE)};
    hgcm->call(hgcm, XD_DND_GUEST_REPORT_FEATURES, features, 2);
    return XD_VINF_SUCCESS;
}

void xd_dnd_session_finish(xd_dnd_session *session) {
    xd_dnd_receiver_finish(&session->receiver);
    g_free(session->requested);
    g_free(session->chunk);
    g_free(session->formats_buffer);
    g_mutex_clear(&session->lock);
}

static xd_dnd_event *event_new(xd_dnd_event_kind kind) {
    xd_dnd_event *event = g_new0(xd_dnd_event, 1);
    event->kind = kind;
    return event;
}

/* ENTER, MOVE and DROPPED: where, which actions, which formats. */
static xd_dnd_event *fetch_position(xd_dnd_session *session, uint32_t message,
                                    xd_dnd_event_kind kind) {
    xd_hgcm_parm parms[] = {
        xd_hgcm_u32(0), xd_hgcm_u32(0), xd_hgcm_u32(0), xd_hgcm_u32(0),
        xd_hgcm_u32(0), xd_hgcm_u32(0), xd_hgcm_buffer(session->formats_buffer, XD_DND_CHUNK),
        xd_hgcm_u32(0),
    };
    int rc = session->hgcm->call(session->hgcm, message, parms, 8);
    if (rc < 0) {
        g_printerr("xd-vboxdnd: fetching host message %u failed (%d)\n", message, rc);
        return nullptr;
    }
    xd_dnd_event *event = event_new(kind);
    event->screen = parms[1].u32;
    event->x = (int32_t)parms[2].u32;
    event->y = (int32_t)parms[3].u32;
    event->default_action = parms[4].u32;
    event->allowed_actions = parms[5].u32;
    uint32_t length = MIN(parms[7].u32, (uint32_t)XD_DND_CHUNK);
    event->formats = xd_dnd_formats_split(session->formats_buffer, length);
    return event;
}

/* A transfer's piece failed: tell the host, remove what came. */
static xd_dnd_event *transfer_failed(xd_dnd_session *session, int rc) {
    g_printerr("xd-vboxdnd: receiving the drop failed (%d)\n", rc);
    xd_dnd_report_error(session->hgcm, rc);
    xd_dnd_receiver_rollback(&session->receiver);
    g_mutex_lock(&session->lock);
    session->receiving = false;
    g_mutex_unlock(&session->lock);
    xd_dnd_event *event = event_new(XD_DND_EVENT_FAILED);
    event->rc = rc;
    return event;
}

/* Everything came: the URI list of the dropped roots, or the data. */
static xd_dnd_event *transfer_done(xd_dnd_session *session) {
    xd_dnd_receiver *receiver = &session->receiver;
    xd_dnd_event *event = event_new(XD_DND_EVENT_DATA);
    if (receiver->uri_list) {
        g_auto(GStrv) roots =
            xd_dnd_meta_roots((const char *)receiver->meta->data, receiver->meta->len);
        if (roots == nullptr || roots[0] == nullptr) {
            xd_dnd_event_free(event);
            return transfer_failed(session, XD_VERR_INVALID_PARAMETER);
        }
        char *list = xd_dnd_drop_uri_list(receiver->drop_dir, roots);
        event->mime = g_strdup(XD_DND_URI_LIST);
        event->data = g_bytes_new_take(list, strlen(list));
    } else {
        /* Text comes NUL-terminated; the program dropped on gets it without. */
        guint length = receiver->meta->len;
        while (length > 0 && receiver->meta->data[length - 1] == '\0')
            length--;
        g_mutex_lock(&session->lock);
        event->mime = g_strdup(session->requested != nullptr ? session->requested
                                                             : "text/plain;charset=utf-8");
        g_mutex_unlock(&session->lock);
        event->data = g_bytes_new(receiver->meta->data, length);
    }
    xd_dnd_receiver_reset(receiver);
    g_mutex_lock(&session->lock);
    session->receiving = false;
    g_mutex_unlock(&session->lock);
    return event;
}

/* One piece of a transfer; an event when it ends (or fails). */
static xd_dnd_event *receive_piece(xd_dnd_session *session, uint32_t message, uint32_t count) {
    xd_hgcm *hgcm = session->hgcm;
    xd_dnd_receiver *receiver = &session->receiver;
    int rc;
    switch (message) {
    case XD_DND_HOST_HG_DATA_HEADER: {
        char checksum[64];
        xd_hgcm_parm parms[] = {
            xd_hgcm_u32(0),
            xd_hgcm_u32(0),
            xd_hgcm_u32(0),
            xd_hgcm_u64(0),
            xd_hgcm_u32(0), /* the meta data's size: 32 bits, whatever the comment says */
            xd_hgcm_buffer(session->formats_buffer, XD_DND_CHUNK),
            xd_hgcm_u32(0),
            xd_hgcm_u64(0),
            xd_hgcm_u32(0),
            xd_hgcm_u32(0),
            xd_hgcm_buffer(checksum, sizeof(checksum)),
            xd_hgcm_u32(0),
        };
        rc = hgcm->call(hgcm, message, parms, 12);
        if (rc < 0)
            return transfer_failed(session, rc);
        session->formats_buffer[XD_DND_CHUNK - 1] = '\0';
        g_printerr("xd-vboxdnd: the host sends %s: %lu bytes, %lu files and folders\n",
                   session->formats_buffer, (unsigned long)parms[3].u64, (unsigned long)parms[7].u64);
        rc = xd_dnd_receiver_header(receiver, parms[3].u64, parms[4].u32, session->formats_buffer,
                                    parms[7].u64);
        break;
    }
    case XD_DND_HOST_HG_DATA:
    case XD_DND_HOST_HG_FILE_DATA: {
        char checksum[64];
        xd_hgcm_parm parms[] = {
            xd_hgcm_u32(0),
            xd_hgcm_buffer(session->chunk, XD_DND_CHUNK),
            xd_hgcm_u32(0),
            xd_hgcm_buffer(checksum, sizeof(checksum)),
            xd_hgcm_u32(0),
        };
        rc = hgcm->call(hgcm, message, parms, 5);
        if (rc < 0)
            return transfer_failed(session, rc);
        uint32_t size = parms[2].u32;
        if (size > XD_DND_CHUNK)
            return transfer_failed(session, XD_VERR_BUFFER_OVERFLOW);
        rc = message == XD_DND_HOST_HG_DATA
                 ? xd_dnd_receiver_data(receiver, session->chunk, size)
                 : xd_dnd_receiver_file_data(receiver, session->chunk, size);
        break;
    }
    case XD_DND_HOST_HG_DIR: {
        char name[NAME_MAX_BYTES];
        xd_hgcm_parm parms[] = {xd_hgcm_u32(0), xd_hgcm_buffer(name, sizeof(name)),
                                xd_hgcm_u32(0), xd_hgcm_u32(0)};
        rc = hgcm->call(hgcm, message, parms, 4);
        if (rc < 0)
            return transfer_failed(session, rc);
        name[sizeof(name) - 1] = '\0';
        rc = xd_dnd_receiver_dir(receiver, name, parms[3].u32 & 0xFFFF);
        break;
    }
    case XD_DND_HOST_HG_FILE_HEADER: {
        char name[NAME_MAX_BYTES];
        xd_hgcm_parm parms[] = {xd_hgcm_u32(0), xd_hgcm_buffer(name, sizeof(name)),
                                xd_hgcm_u32(0), xd_hgcm_u32(0),
                                xd_hgcm_u32(0), xd_hgcm_u64(0)};
        rc = hgcm->call(hgcm, message, parms, 6);
        if (rc < 0)
            return transfer_failed(session, rc);
        name[sizeof(name) - 1] = '\0';
        rc = xd_dnd_receiver_file_header(receiver, name, parms[4].u32 & 0xFFFF, parms[5].u64);
        break;
    }
    default:
        (void)count;
        return nullptr;
    }
    if (rc < 0)
        return transfer_failed(session, rc);
    return xd_dnd_receiver_complete(receiver) ? transfer_done(session) : nullptr;
}

static bool is_transfer_message(uint32_t message) {
    return message == XD_DND_HOST_HG_DATA_HEADER || message == XD_DND_HOST_HG_DATA ||
           message == XD_DND_HOST_HG_DIR || message == XD_DND_HOST_HG_FILE_HEADER ||
           message == XD_DND_HOST_HG_FILE_DATA;
}

xd_dnd_event *xd_dnd_session_next(xd_dnd_session *session) {
    xd_hgcm *hgcm = session->hgcm;
    for (;;) {
        xd_hgcm_parm next[] = {xd_hgcm_u32(0), xd_hgcm_u32(0), xd_hgcm_u32(1)};
        int rc = hgcm->call(hgcm, XD_DND_GUEST_GET_NEXT, next, 3);
        uint32_t message = next[0].u32, count = next[1].u32;
        if (rc == XD_VERR_INTERRUPTED)
            continue;
        if (rc == XD_VERR_CANCELLED && message == XD_DND_HOST_CANCEL) {
            /* The host gave up on what was going on. */
            g_mutex_lock(&session->lock);
            bool receiving = session->receiving;
            session->receiving = false;
            g_mutex_unlock(&session->lock);
            if (receiving)
                xd_dnd_receiver_rollback(&session->receiver);
            return event_new(XD_DND_EVENT_CANCELLED);
        }
        if (rc < 0)
            return event_new(XD_DND_EVENT_QUIT);
        xd_dnd_event *event = nullptr;
        switch (message) {
        case XD_DND_HOST_HG_ENTER:
            event = fetch_position(session, message, XD_DND_EVENT_ENTER);
            break;
        case XD_DND_HOST_HG_MOVE:
            event = fetch_position(session, message, XD_DND_EVENT_MOVE);
            break;
        case XD_DND_HOST_HG_DROPPED:
            event = fetch_position(session, message, XD_DND_EVENT_DROPPED);
            break;
        case XD_DND_HOST_HG_LEAVE: {
            xd_hgcm_parm parms[] = {xd_hgcm_u32(0)};
            if (hgcm->call(hgcm, message, parms, 1) >= 0)
                event = event_new(XD_DND_EVENT_LEAVE);
            break;
        }
        case XD_DND_HOST_CANCEL: {
            xd_hgcm_parm parms[] = {xd_hgcm_u32(0)};
            hgcm->call(hgcm, message, parms, 1);
            g_mutex_lock(&session->lock);
            bool receiving = session->receiving;
            session->receiving = false;
            g_mutex_unlock(&session->lock);
            if (receiving)
                xd_dnd_receiver_rollback(&session->receiver);
            event = event_new(XD_DND_EVENT_CANCELLED);
            break;
        }
        case XD_DND_HOST_GH_DROPPED: {
            xd_hgcm_parm parms[] = {xd_hgcm_u32(0),
                                    xd_hgcm_buffer(session->formats_buffer, XD_DND_CHUNK),
                                    xd_hgcm_u32(0), xd_hgcm_u32(0)};
            if (hgcm->call(hgcm, message, parms, 4) >= 0) {
                event = event_new(XD_DND_EVENT_TAKEN);
                uint32_t length = MIN(parms[2].u32, (uint32_t)XD_DND_CHUNK);
                event->mime = g_strndup(session->formats_buffer, length);
                event->default_action = parms[3].u32;
            }
            break;
        }
        case XD_DND_HOST_GH_PENDING: {
            xd_hgcm_parm parms[] = {xd_hgcm_u32(0), xd_hgcm_u32(0)};
            if (hgcm->call(hgcm, message, parms, 2) >= 0) {
                event = event_new(XD_DND_EVENT_PENDING);
                event->screen = parms[1].u32;
            }
            break;
        }
        default:
            if (is_transfer_message(message)) {
                event = receive_piece(session, message, count);
            } else {
                /* Not ours to understand (guest-to-host's drop comes
                 * later): fetching it anyway keeps the queue moving. */
                g_printerr("xd-vboxdnd: host message %u (%u parameters) not handled\n", message,
                           count);
                event = nullptr;
            }
            break;
        }
        if (event != nullptr)
            return event;
    }
}

int xd_dnd_ack(xd_hgcm *hgcm, uint32_t action) {
    xd_hgcm_parm parms[] = {xd_hgcm_u32(0), xd_hgcm_u32(action)};
    return hgcm->call(hgcm, XD_DND_GUEST_HG_ACK_OP, parms, 2);
}

int xd_dnd_request_data(xd_dnd_session *session, const char *format) {
    g_mutex_lock(&session->lock);
    g_free(session->requested);
    session->requested = g_strdup(format);
    session->receiving = true;
    g_mutex_unlock(&session->lock);
    g_autofree char *copy = g_strdup(format);
    xd_hgcm_parm parms[] = {xd_hgcm_u32(0),
                            xd_hgcm_buffer(copy, (uint32_t)strlen(copy) + 1),
                            xd_hgcm_u32((uint32_t)strlen(copy) + 1)};
    return session->hgcm->call(session->hgcm, XD_DND_GUEST_HG_REQ_DATA, parms, 3);
}

int xd_dnd_ack_pending(xd_hgcm *hgcm, uint32_t default_action, uint32_t allowed_actions,
                       const char *const *formats) {
    g_autofree char *list = xd_dnd_formats_join(formats);
    uint32_t size = (uint32_t)strlen(list) + 1;
    xd_hgcm_parm parms[] = {xd_hgcm_u32(0), xd_hgcm_u32(default_action),
                            xd_hgcm_u32(allowed_actions), xd_hgcm_buffer(list, size),
                            xd_hgcm_u32(size)};
    return hgcm->call(hgcm, XD_DND_GUEST_GH_ACK_PENDING, parms, 5);
}

int xd_dnd_report_error(xd_hgcm *hgcm, int rc) {
    xd_hgcm_parm parms[] = {xd_hgcm_u32(0), xd_hgcm_u32((uint32_t)rc)};
    return hgcm->call(hgcm, XD_DND_GUEST_ERROR, parms, 2);
}

/* ---- sending a drop to the host ------------------------------------------------------ */

static int send_header(xd_hgcm *hgcm, uint64_t total, uint32_t meta_size, const char *meta_format,
                       uint64_t objects) {
    g_autofree char *format = g_strdup(meta_format);
    uint32_t format_size = format != nullptr ? (uint32_t)strlen(format) + 1 : 0;
    xd_hgcm_parm parms[] = {
        xd_hgcm_u32(0),
        xd_hgcm_u32(0),
        xd_hgcm_u32(0),
        xd_hgcm_u64(total),
        xd_hgcm_u32(meta_size), /* 32 bits, whatever the header says */
        xd_hgcm_buffer(format, format_size),
        xd_hgcm_u32(format_size),
        xd_hgcm_u64(objects),
        xd_hgcm_u32(0),
        xd_hgcm_u32(0),
        xd_hgcm_buffer(nullptr, 0),
        xd_hgcm_u32(0),
    };
    return hgcm->call(hgcm, XD_DND_GUEST_GH_DATA_HEADER, parms, 12);
}

/* Data in chunks of at most XD_DND_CHUNK, as `function` (data or file data). */
static int send_chunks(xd_hgcm *hgcm, uint32_t function, const void *data, size_t size) {
    const unsigned char *at = data;
    for (size_t sent = 0; sent < size;) {
        uint32_t chunk = (uint32_t)MIN(size - sent, (size_t)XD_DND_CHUNK);
        xd_hgcm_parm parms[] = {xd_hgcm_u32(0), xd_hgcm_buffer((void *)(uintptr_t)(at + sent), chunk),
                                xd_hgcm_u32(chunk), xd_hgcm_buffer(nullptr, 0), xd_hgcm_u32(0)};
        int rc = hgcm->call(hgcm, function, parms, 5);
        if (rc < 0)
            return rc;
        sent += chunk;
    }
    return XD_VINF_SUCCESS;
}

int xd_dnd_send_data(xd_hgcm *hgcm, const char *mime, const void *data, size_t size) {
    /* Text goes with its terminating NUL, as the host expects. */
    bool text = g_str_has_prefix(mime, "text/") || g_str_equal(mime, "UTF8_STRING") ||
                g_str_equal(mime, "STRING") || g_str_equal(mime, "TEXT");
    GByteArray *bytes = g_byte_array_new();
    g_byte_array_append(bytes, data, (guint)size);
    if (text && (bytes->len == 0 || bytes->data[bytes->len - 1] != '\0'))
        g_byte_array_append(bytes, (const guint8 *)"", 1);
    int rc = send_header(hgcm, bytes->len, bytes->len, nullptr, 0);
    if (rc >= 0)
        rc = send_chunks(hgcm, XD_DND_GUEST_GH_DATA, bytes->data, bytes->len);
    g_byte_array_unref(bytes);
    if (rc < 0)
        xd_dnd_report_error(hgcm, rc);
    return rc;
}

/* One thing to send: a directory or a regular file, relative to the root. */
typedef struct outgoing_object {
    char *path;     /* local, absolute */
    char *relative; /* to the root, '/'-separated */
    bool dir;
    uint32_t mode;
    uint64_t size;
} outgoing_object;

static void outgoing_object_free(gpointer data) {
    outgoing_object *object = data;
    g_free(object->path);
    g_free(object->relative);
    g_free(object);
}

/* Adds `path` and, for a directory, everything in it (sorted, depth
 * first: a directory before what it holds); links are left out. */
static bool collect(GPtrArray *objects, const char *path, const char *relative) {
    struct stat info;
    if (lstat(path, &info) != 0)
        return false;
    if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))
        return true; /* a link, device or socket: not sent */
    outgoing_object *object = g_new0(outgoing_object, 1);
    object->path = g_strdup(path);
    object->relative = g_strdup(relative);
    object->dir = S_ISDIR(info.st_mode);
    object->mode = info.st_mode & 0xFFFF;
    object->size = object->dir ? 0 : (uint64_t)info.st_size;
    g_ptr_array_add(objects, object);
    if (!object->dir)
        return true;
    GDir *dir = g_dir_open(path, 0, nullptr);
    if (dir == nullptr)
        return false;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (const char *name = g_dir_read_name(dir); name != nullptr; name = g_dir_read_name(dir))
        g_ptr_array_add(names, g_strdup(name));
    g_dir_close(dir);
    g_ptr_array_sort_values(names, (GCompareFunc)strcmp);
    bool ok = true;
    for (guint i = 0; i < names->len && ok; i++) {
        const char *name = g_ptr_array_index(names, i);
        g_autofree char *child = g_build_filename(path, name, nullptr);
        g_autofree char *child_relative = g_strconcat(relative, "/", name, nullptr);
        ok = collect(objects, child, child_relative);
    }
    g_ptr_array_unref(names);
    return ok;
}

/* The longest directory all the paths are in. */
static char *common_parent(char **paths) {
    char *parent = g_path_get_dirname(paths[0]);
    for (char **path = paths + 1; *path != nullptr; path++) {
        while (!(g_str_has_prefix(*path, parent) &&
                 ((*path)[strlen(parent)] == '/' || strcmp(parent, "/") == 0))) {
            char *up = g_path_get_dirname(parent);
            bool top = strcmp(up, parent) == 0;
            g_free(parent);
            parent = up;
            if (top)
                break;
        }
    }
    return parent;
}

static int send_objects(xd_hgcm *hgcm, GPtrArray *objects) {
    unsigned char *chunk = g_malloc(XD_DND_CHUNK);
    int rc = XD_VINF_SUCCESS;
    for (guint i = 0; i < objects->len && rc >= 0; i++) {
        const outgoing_object *object = g_ptr_array_index(objects, i);
        uint32_t path_size = (uint32_t)strlen(object->relative) + 1;
        if (object->dir) {
            xd_hgcm_parm parms[] = {xd_hgcm_u32(0), xd_hgcm_buffer(object->relative, path_size),
                                    xd_hgcm_u32(path_size), xd_hgcm_u32(object->mode)};
            rc = hgcm->call(hgcm, XD_DND_GUEST_GH_DIR, parms, 4);
            continue;
        }
        int fd = open(object->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            rc = status_of_errno(errno);
            break;
        }
        xd_hgcm_parm header[] = {xd_hgcm_u32(0), xd_hgcm_buffer(object->relative, path_size),
                                 xd_hgcm_u32(path_size), xd_hgcm_u32(0),
                                 xd_hgcm_u32(object->mode), xd_hgcm_u64(object->size)};
        rc = hgcm->call(hgcm, XD_DND_GUEST_GH_FILE_HEADER, header, 6);
        /* Exactly the size announced, even if the file changed since. */
        for (uint64_t left = object->size; rc >= 0 && left > 0;) {
            size_t want = (size_t)MIN(left, (uint64_t)XD_DND_CHUNK);
            ssize_t got = read(fd, chunk, want);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0) {
                rc = got < 0 ? status_of_errno(errno) : XD_VERR_GENERAL_FAILURE;
                break;
            }
            rc = send_chunks(hgcm, XD_DND_GUEST_GH_FILE_DATA, chunk, (size_t)got);
            left -= (uint64_t)got;
        }
        close(fd);
    }
    g_free(chunk);
    return rc;
}

int xd_dnd_send_files(xd_hgcm *hgcm, const char *uri_list, size_t size) {
    g_autofree char *text = g_strndup(uri_list, size);
    g_auto(GStrv) lines = g_strsplit_set(text, "\r\n", -1);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (char **line = lines; *line != nullptr; line++) {
        if ((*line)[0] == '\0' || (*line)[0] == '#')
            continue;
        char *path = g_filename_from_uri(*line, nullptr, nullptr);
        if (path != nullptr) {
            /* A directory's trailing slash isn't part of its name. */
            size_t length = strlen(path);
            while (length > 1 && path[length - 1] == '/')
                path[--length] = '\0';
            g_ptr_array_add(paths, path);
        }
    }
    g_ptr_array_add(paths, nullptr);
    char **roots = (char **)paths->pdata;
    int rc = XD_VINF_SUCCESS;
    GPtrArray *objects = g_ptr_array_new_with_free_func(outgoing_object_free);
    GString *meta = g_string_new(nullptr);
    if (roots[0] == nullptr) {
        rc = XD_VERR_NOT_FOUND;
    } else {
        g_autofree char *parent = common_parent(roots);
        size_t skip = strcmp(parent, "/") == 0 ? 1 : strlen(parent) + 1;
        for (char **root = roots; *root != nullptr && rc >= 0; root++) {
            guint before = objects->len;
            if (!collect(objects, *root, *root + skip)) {
                rc = XD_VERR_NOT_FOUND;
                break;
            }
            if (objects->len == before)
                continue; /* a link: left out */
            const outgoing_object *first = g_ptr_array_index(objects, before);
            g_autofree char *escaped = g_uri_escape_string(first->relative,
                                                           G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
            g_string_append_printf(meta, "file:///%s%s\r\n", escaped, first->dir ? "/" : "");
        }
        if (rc >= 0 && objects->len == 0)
            rc = XD_VERR_NOT_FOUND;
    }
    if (rc >= 0) {
        uint64_t files = 0;
        for (guint i = 0; i < objects->len; i++)
            files += ((const outgoing_object *)g_ptr_array_index(objects, i))->size;
        uint32_t meta_size = (uint32_t)meta->len + 1; /* and its NUL */
        rc = send_header(hgcm, meta_size + files, meta_size, XD_DND_URI_LIST, objects->len);
        if (rc >= 0)
            rc = send_chunks(hgcm, XD_DND_GUEST_GH_DATA, meta->str, meta_size);
        if (rc >= 0)
            rc = send_objects(hgcm, objects);
    }
    if (rc < 0)
        xd_dnd_report_error(hgcm, rc);
    g_string_free(meta, TRUE);
    g_ptr_array_unref(objects);
    g_ptr_array_unref(paths);
    return rc;
}
