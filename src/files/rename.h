/* SPDX-License-Identifier: MIT
 * Renaming files and shortcuts (launchers), as Windows does.
 */
#ifndef XD_FILES_RENAME_H
#define XD_FILES_RENAME_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* The name Rename starts from: a launcher's Name (what's shown), else the
 * file's name. Free with g_free. */
char *xd_rename_text(GFile *file);
/* Renames as Windows renames a shortcut: a launcher keeps its file and
 * gets a new Name; anything else is renamed. Returns the file's new
 * location (a new reference), or nullptr with `err` set. */
GFile *xd_rename(GFile *file, const char *text, GError **err);

G_END_DECLS

#endif
