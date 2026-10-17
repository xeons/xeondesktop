/* SPDX-License-Identifier: MIT
 * File types for Folder Options' File Types tab: the extensions each
 * MIME type registers in shared-mime-info.
 */
#ifndef XD_FOLDERS_FILETYPES_H
#define XD_FOLDERS_FILETYPES_H

#include <glib.h>

/* Adds the extensions (without the dot) that one MIME type's XML (the
 * per-type <type>.xml update-mime-database writes) registers with simple
 * "*.ext" globs to `out` (char *, g_free), skipping ones already there.
 * Case-insensitive globs are lowercased. Malformed XML adds what was
 * read before the error. */
void xd_mime_xml_extensions(const char *xml, gssize len, GPtrArray *out);

/* A type's extensions from the user's and the system's MIME databases
 * ($XDG_DATA_HOME/mime, $XDG_DATA_DIRS/mime). Free with g_strfreev. */
char **xd_content_type_extensions(const char *type);

#endif
