/* SPDX-License-Identifier: MIT
 * Asking xd-thumbnailer for thumbnails missing from the cache, in the
 * background: a few at a time, those asked for most recently first (what
 * a view drew last is what's on screen), each in its own sandboxed
 * process, so a file that crashes or hangs its decoder costs nothing but
 * its thumbnail.
 */
#ifndef XD_THUMBNAILER_QUEUE_H
#define XD_THUMBNAILER_QUEUE_H

#include "thumbcache.h"

G_BEGIN_DECLS

typedef struct XdThumbQueue XdThumbQueue;

/* `path` is the new thumbnail, or nullptr when the file couldn't be
 * thumbnailed. */
typedef void (*XdThumbDone)(const char *uri, const char *path, gpointer data);

XdThumbQueue *xd_thumb_queue_new(XdThumbDone done, gpointer data);
/* Cancels everything (without calling `done`) and frees the queue. */
void xd_thumb_queue_free(XdThumbQueue *thumb_queue);
/* Asks for `uri`'s thumbnail, or asks again: requests made in the same
 * main loop iteration count as one batch, and the latest batch goes
 * first. */
void xd_thumb_queue_request(XdThumbQueue *thumb_queue, const char *uri, XdThumbSize size);
/* Forgets the requests and stops those running (another folder). */
void xd_thumb_queue_cancel(XdThumbQueue *thumb_queue);

G_END_DECLS

#endif
