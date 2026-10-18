/* SPDX-License-Identifier: MIT
 * The freedesktop thumbnail cache (Thumbnail Managing Standard 0.9.0):
 * where a file's thumbnail lives, writing one with the metadata other
 * programs check it by, failure markers, and which types can be
 * thumbnailed (gdk-pixbuf, or an installed .thumbnailer).
 */
#ifndef XD_THUMBNAILER_THUMBCACHE_H
#define XD_THUMBNAILER_THUMBCACHE_H

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* The spec's sizes: normal 128, large 256, x-large 512, xx-large 1024. */
typedef enum {
    XD_THUMB_NORMAL,
    XD_THUMB_LARGE,
    XD_THUMB_XLARGE,
    XD_THUMB_XXLARGE,
    XD_THUMB_SIZES
} XdThumbSize;

int xd_thumb_size_px(XdThumbSize size);
/* The cache's folder name for a size ("normal", "large", ...). */
const char *xd_thumb_size_name(XdThumbSize size);
gboolean xd_thumb_size_parse(const char *name, XdThumbSize *size);
/* The smallest size at least `px` across (the largest past 1024). */
XdThumbSize xd_thumb_size_for(int px);

/* $XDG_CACHE_HOME/thumbnails. Free with g_free. */
char *xd_thumb_cache_dir(void);
/* `root`/SIZE/MD5(uri).png. Free with g_free. */
char *xd_thumb_path(const char *root, const char *uri, XdThumbSize size);
/* `root`/fail/xd-thumbnailer-VERSION/MD5(uri).png. Free with g_free. */
char *xd_thumb_fail_path(const char *root, const char *uri);
/* Whether `path` is inside the cache `root` (never thumbnailed: the
 * spec's guard against thumbnails of thumbnails). */
gboolean xd_thumb_in_cache(const char *root, const char *path);

/* Writes `pb` to `dest` as the thumbnail of `uri`: a PNG with Thumb::URI,
 * Thumb::MTime and (when known: size >= 0, mime) Thumb::Size and
 * Thumb::Mimetype, made private (0600, folders 0700) and renamed into
 * place so a reader never sees half a file. */
gboolean xd_thumb_save(GdkPixbuf *pb, const char *dest, const char *uri, gint64 mtime,
                       gint64 size, const char *mime, GError **err);
/* Whether the PNG at `png` is the thumbnail of `uri` as it is now (its
 * Thumb::URI and Thumb::MTime match). */
gboolean xd_thumb_is_current(const char *png, const char *uri, gint64 mtime);
/* The failure marker: an empty PNG under fail/, so the file isn't tried
 * again until it changes. */
gboolean xd_thumb_mark_failed(const char *root, const char *uri, gint64 mtime, GError **err);
gboolean xd_thumb_has_failed(const char *root, const char *uri, gint64 mtime);

/* An installed thumbnailer's Exec line for `mime`: the first .thumbnailer
 * file that lists it and whose TryExec is there, in the "thumbnailers"
 * folders of $XDG_DATA_HOME then $XDG_DATA_DIRS. nullptr: none. Owned by
 * the cache. */
const char *xd_thumbnailer_exec(const char *mime);
/* Whether gdk-pixbuf reads `mime`. */
gboolean xd_thumb_pixbuf_reads(const char *mime);
/* Whether files of type `mime` can be thumbnailed at all. */
gboolean xd_thumb_supported(const char *mime);

G_END_DECLS

#endif
