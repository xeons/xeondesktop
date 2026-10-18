/* SPDX-License-Identifier: MIT
 * The Properties dialog for files and folders (Windows 2000 style).
 *
 * General: name, type, default application, location, size and size on
 * disk (folders are counted in the background), contents, created,
 * modified and accessed times, Read-only and Hidden, and Linux file
 * attributes (chattr) behind Advanced....
 * Permissions: owner, group, read/write/execute for owner, group and
 * others, set-user-ID, set-group-ID and sticky, optionally applied to
 * everything inside a folder.
 * Details: path, MIME type, inode, links, device and file system.
 *
 * Several files can be shown at once; check boxes that differ between
 * them start in the mixed state and are left alone unless changed.
 */
#ifndef XD_FILES_PROPERTIES_H
#define XD_FILES_PROPERTIES_H

#include <gtk/gtk.h>

/* Shows a non-modal Properties window for `files` (GFile*, at least one;
 * the list and files are borrowed). The window belongs to the parent's
 * application, if any. Returns the window, or nullptr if none of the
 * files could be read (an error was shown). */
GtkWidget *xd_properties_show(GtkWindow *parent, GList *files);

/* A drive's Properties, from My Computer (`item` is its item there): XP's
 * General tab with the label (changed through UDisks2), type, file
 * system, used and free space, capacity, the pie chart and Disk Cleanup;
 * then Tools and Hardware (drives.h). nullptr if `item` isn't a drive
 * (My Documents is an ordinary folder). */
GtkWidget *xd_properties_show_drive(GtkWindow *parent, GFile *item);

/* "1.23 MB (1,291,234 bytes)", as Windows shows sizes in Properties.
 * Free with g_free. */
char *xd_format_size_long(guint64 bytes);

#endif
