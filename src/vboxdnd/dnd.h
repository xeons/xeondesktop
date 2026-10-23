/* SPDX-License-Identifier: MIT
 * VirtualBox's drag and drop service (VBoxDragAndDropSvc, protocol 3),
 * the guest's side, apart from Wayland so the unit tests can drive it
 * with a scripted host: the handshake, fetching the host's messages, its
 * formats lists, receiving a drop's files into a fresh directory (checked:
 * no absolute paths, no ".."), and the guest's replies.
 *
 * A host message is fetched in two calls: GET_NEXT_HOST_MSG says which
 * message and how many parameters; a call with that message's number
 * fetches them. Host-to-guest drops: ENTER, MOVE, LEAVE and DROPPED each
 * want one reply (ACK_OP with the action the target here takes, or for
 * DROPPED, REQ_DATA with the format wanted); then a data header, the
 * data (a URI list of the dropped roots, for files), then each directory
 * and file, depth first.
 */
#ifndef XD_VBOXDND_DND_H
#define XD_VBOXDND_DND_H

#include <glib.h>
#include <stdint.h>

#include "hgcm.h"

#define XD_DND_SERVICE "VBoxDragAndDropSvc"
#define XD_DND_URI_LIST "text/uri-list"
/* The host's data chunks are never larger. */
#define XD_DND_CHUNK 65536

/* Host messages and guest functions (DragAndDropSvc.h). */
enum {
    XD_DND_HOST_HG_ENTER = 200,
    XD_DND_HOST_HG_MOVE = 201,
    XD_DND_HOST_HG_LEAVE = 202,
    XD_DND_HOST_HG_DROPPED = 203,
    XD_DND_HOST_CANCEL = 204,
    XD_DND_HOST_HG_DATA = 205,
    XD_DND_HOST_HG_DIR = 207,
    XD_DND_HOST_HG_FILE_DATA = 208,
    XD_DND_HOST_HG_FILE_HEADER = 209,
    XD_DND_HOST_HG_DATA_HEADER = 210,
    XD_DND_HOST_GH_PENDING = 600,
    XD_DND_HOST_GH_DROPPED = 601,

    XD_DND_GUEST_CONNECT = 10,
    XD_DND_GUEST_REPORT_FEATURES = 12,
    XD_DND_GUEST_GET_NEXT = 300,
    XD_DND_GUEST_HG_ACK_OP = 400,
    XD_DND_GUEST_HG_REQ_DATA = 401,
    XD_DND_GUEST_GH_ACK_PENDING = 500,
    XD_DND_GUEST_GH_DATA = 501,
    XD_DND_GUEST_ERROR = 502,
    XD_DND_GUEST_GH_DATA_HEADER = 503,
    XD_DND_GUEST_GH_DIR = 700,
    XD_DND_GUEST_GH_FILE_DATA = 701,
    XD_DND_GUEST_GH_FILE_HEADER = 702,
};

/* Actions, as bits. */
enum {
    XD_DND_IGNORE = 0,
    XD_DND_COPY = 1,
    XD_DND_MOVE = 2,
    XD_DND_LINK = 4,
};

/* ---- formats and paths ------------------------------------------------------------------- */

/* A formats list (each MIME type followed by "\r\n", NUL-terminated) as a
 * string vector, empty entries skipped; and back. */
char **xd_dnd_formats_split(const char *list, size_t length);
char *xd_dnd_formats_join(const char *const *formats);
/* A path the host names, relative to the drop: not empty, not absolute,
 * no "." or ".." part, valid UTF-8. */
bool xd_dnd_name_valid(const char *name);
/* The roots of a dropped URI list ("file:///a.txt\r\nfile:///pics/\r\n"):
 * their paths relative to the drop ("a.txt", "pics"); nullptr if one
 * isn't valid. */
char **xd_dnd_meta_roots(const char *meta, size_t length);
/* The roots as a URI list of absolute file:// URIs under `drop_dir`, each
 * followed by "\r\n", for the program dropped on. */
char *xd_dnd_drop_uri_list(const char *drop_dir, char **roots);

/* ---- receiving a drop -------------------------------------------------------------------- */

typedef struct xd_dnd_receiver {
    char *base;     /* where drop directories are made */
    char *drop_dir; /* this drop's, once its header (or an object) came */
    bool has_header; /* objects may come first (VirtualBox 7.2) */
    bool uri_list;
    uint64_t total, meta_size, objects;
    uint64_t file_bytes, objects_done;
    GByteArray *meta;
    int file_fd; /* the file being received, or -1 */
    uint64_t file_left;
    GPtrArray *created; /* paths made, to remove if it fails */
} xd_dnd_receiver;

/* Drops go in `base`/<timestamp>/ (VirtualBox's own: $TMPDIR/VirtualBox
 * Dropped Files). */
void xd_dnd_receiver_init(xd_dnd_receiver *receiver, const char *base);
/* Each returns a VirtualBox status; on failure, roll back. */
int xd_dnd_receiver_header(xd_dnd_receiver *receiver, uint64_t total, uint32_t meta_size,
                           const char *meta_format, uint64_t objects);
int xd_dnd_receiver_data(xd_dnd_receiver *receiver, const void *data, uint32_t size);
int xd_dnd_receiver_dir(xd_dnd_receiver *receiver, const char *name, uint32_t mode);
int xd_dnd_receiver_file_header(xd_dnd_receiver *receiver, const char *name, uint32_t mode,
                                uint64_t size);
int xd_dnd_receiver_file_data(xd_dnd_receiver *receiver, const void *data, uint32_t size);
/* Everything the header announced has come. */
bool xd_dnd_receiver_complete(const xd_dnd_receiver *receiver);
/* Removes what this drop made, newest first. */
void xd_dnd_receiver_rollback(xd_dnd_receiver *receiver);
/* Forgets the drop (its files stay), ready for the next. */
void xd_dnd_receiver_reset(xd_dnd_receiver *receiver);
void xd_dnd_receiver_finish(xd_dnd_receiver *receiver);

/* ---- the session ----------------------------------------------------------------------- */

typedef enum xd_dnd_event_kind : uint8_t {
    XD_DND_EVENT_ENTER,
    XD_DND_EVENT_MOVE,
    XD_DND_EVENT_LEAVE,
    XD_DND_EVENT_DROPPED,
    XD_DND_EVENT_DATA,      /* the dropped data has all come */
    XD_DND_EVENT_FAILED,    /* receiving it failed (rc) */
    XD_DND_EVENT_CANCELLED, /* the host cancelled */
    XD_DND_EVENT_PENDING,   /* the host asks whether a guest drag is going on */
    XD_DND_EVENT_TAKEN,     /* the guest's drag was dropped on the host (mime, action) */
    XD_DND_EVENT_QUIT,      /* disconnected */
} xd_dnd_event_kind;

typedef struct xd_dnd_event {
    xd_dnd_event_kind kind;
    uint32_t screen;
    int32_t x, y;
    uint32_t default_action, allowed_actions;
    char **formats; /* ENTER, MOVE, DROPPED */
    char *mime;     /* DATA: the data's type; TAKEN: the type the host wants */
    GBytes *data;   /* DATA: a URI list of the dropped files, or the data */
    int rc;         /* FAILED */
} xd_dnd_event;

void xd_dnd_event_free(xd_dnd_event *event);

typedef struct xd_dnd_session {
    xd_hgcm *hgcm;
    xd_dnd_receiver receiver; /* the session's thread's */
    GMutex lock;              /* for these two, set from the replying thread */
    char *requested;          /* the format asked for at the drop */
    bool receiving;
    unsigned char *chunk;  /* XD_DND_CHUNK */
    char *formats_buffer;  /* XD_DND_CHUNK */
} xd_dnd_session;

/* Connects (protocol 3) and reports features; a VirtualBox status. */
int xd_dnd_session_init(xd_dnd_session *session, xd_hgcm *hgcm, const char *drop_base);
void xd_dnd_session_finish(xd_dnd_session *session);
/* Waits for the host's next message and handles it: a transfer's pieces
 * are received here, other messages returned as events (never nullptr:
 * QUIT when the connection ends). */
xd_dnd_event *xd_dnd_session_next(xd_dnd_session *session);

/* The replies (from any thread). */
int xd_dnd_ack(xd_hgcm *hgcm, uint32_t action);
/* Asks for the drop's data; the session then receives it. */
int xd_dnd_request_data(xd_dnd_session *session, const char *format);
int xd_dnd_report_error(xd_hgcm *hgcm, int rc);
/* The answer to PENDING: the guest's drag's actions and formats, or (no
 * formats) that there's none. */
int xd_dnd_ack_pending(xd_hgcm *hgcm, uint32_t default_action, uint32_t allowed_actions,
                       const char *const *formats);

/* ---- sending a drop to the host ------------------------------------------------------ */

/* Data of another type than a URI list (text gets its NUL here). */
int xd_dnd_send_data(xd_hgcm *hgcm, const char *mime, const void *data, size_t size);
/* The files a URI list names (local file:// URIs), directories with all
 * they hold, relative to their common parent; symbolic links skipped.
 * On failure the host is told why. */
int xd_dnd_send_files(xd_hgcm *hgcm, const char *uri_list, size_t size);

#endif
