/* SPDX-License-Identifier: MIT
 * XP's Send To menu for xd-files: Compressed (zipped) Folder (with
 * libarchive), Desktop (create shortcut), Mail Recipient (when the mail
 * program takes attachments), My Documents and the removable drives.
 */
#ifndef XD_FILES_SENDTO_H
#define XD_FILES_SENDTO_H

#include "fileops.h"

G_BEGIN_DECLS

/* The submenu for `files` (GFile*; the list is copied). `done` gets what
 * each command made, as fileops' callbacks do (not called once `parent`
 * has closed). */
GtkWidget *xd_send_to_menu(GtkWindow *parent, GList *files, XdFileOpDone done, gpointer user);

G_END_DECLS

#endif
