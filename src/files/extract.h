/* SPDX-License-Identifier: MIT
 * Extracting archives (libarchive) for xd-files' "Extract Here" and
 * "Extract to "name"": which files are archives, the folder an archive
 * extracts to, and the extraction itself (run by fileops.c's worker).
 */
#ifndef XD_FILES_EXTRACT_H
#define XD_FILES_EXTRACT_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Whether xd-files can extract archives at all (built with libarchive). */
gboolean xd_extract_available(void);

/* Whether a file of this content type is an archive or a compressed file
 * libarchive reads. Exact types only: documents that are zips inside
 * (.docx, .odt, .epub, .jar) aren't offered. */
gboolean xd_is_archive_type(const char *content_type);

/* Whether `file` (local) is one, by its name's content type. */
gboolean xd_is_archive(GFile *file);

/* The folder an archive extracts to, and the name a compressed file
 * (not an archive) decompresses to: its name without the archive's
 * extension ("photos.tar.gz", "photos.tgz" and "photos.zip" are
 * "photos"; "notes.txt.gz" is "notes.txt"). Free with g_free. */
char *xd_archive_base_name(const char *file_name);

/* An archive entry's path made safe to put under the folder extracted
 * to: "." and empty parts and a leading "/" dropped (as tar does). Paths
 * with a ".." part, or with nothing left, give nullptr. Free with
 * g_free. */
char *xd_archive_safe_path(const char *path);

/* Extracts `archive` into the folder `into` (which exists). Entries that
 * would land outside it are left out; so are device files, FIFOs and
 * sockets, and set-user-ID and set-group-ID bits. A compressed file that
 * isn't an archive becomes `raw_name`. `progress` (may be nullptr) gets
 * how many of the archive's bytes have been read. */
typedef void (*XdExtractProgress)(guint64 read, gpointer data);
gboolean xd_extract_archive(const char *archive, const char *into, const char *raw_name,
                            GCancellable *cancel, XdExtractProgress progress, gpointer data,
                            GError **error);

G_END_DECLS

#endif
