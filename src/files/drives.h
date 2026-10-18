/* SPDX-License-Identifier: MIT
 * What My Computer does with a drive beyond opening it, as XP: renaming
 * it (its file system's label, through UDisks2), Disk Cleanup, and its
 * Properties' Tools tab (Check Now..., Defragment Now..., Backup
 * Now...) and Hardware tab (the disk drives, each with Properties).
 */
#ifndef XD_FILES_DRIVES_H
#define XD_FILES_DRIVES_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Whether `item` (in My Computer) is a drive whose label can be changed:
 * UDisks2 knows its file system. Its label (maybe "") goes in *label if
 * given (free with g_free). */
gboolean xd_drive_label(GFile *item, char **label);

/* Sets the drive's label (UDisks2's Filesystem.SetLabel; polkit may
 * ask). Problems are shown over `parent`; then done(ok, data) is called
 * (if given), unless the parent was closed meanwhile. */
void xd_drive_set_label(GtkWindow *parent, GFile *item, const char *name, const char *label,
                        void (*done)(gboolean ok, gpointer data), gpointer data);

/* Whether Disk Cleanup applies to the drive (a local or removable disk
 * that's mounted), and runs it: scanning, then XP's Disk Cleanup for
 * <name> window. */
gboolean xd_drive_can_cleanup(GFile *item, const char *kind);
void xd_drive_cleanup(GtkWindow *parent, GFile *item, const char *name);

/* A drive's Properties pages: Tools (Error-checking, Defragmentation,
 * Backup) and Hardware (every disk drive, the drive's own selected).
 * `dialog` is the Properties window. */
GtkWidget *xd_drive_tools_page(GtkWidget *dialog, GFile *item, const char *name,
                               const char *kind);
GtkWidget *xd_drive_hardware_page(GtkWidget *dialog, GFile *item);

G_END_DECLS

#endif
