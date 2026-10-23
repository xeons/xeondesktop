/* SPDX-License-Identifier: MIT
 * The Previous Versions tab of a file's or folder's Properties (the
 * versions themselves: versions.h). Windows 7's with the Aero style:
 * Name, Date modified (and Location for a folder), Open, Copy... and
 * Restore...; otherwise XP's (its Shadow Copy client's, which 2000 had
 * too): Name and Time, View, Copy... and Restore.
 *
 * The versions are looked for in the background. The tab is added once
 * they're known, and only if the item's file system has snapshots; with
 * none of the item it says "There are no previous versions available."
 */
#ifndef XD_FILES_VERSIONSUI_H
#define XD_FILES_VERSIONSUI_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Adds the tab to `notebook`, in the Properties window `dialog`, for
 * `file` (a local file or folder). With `show_now` (7's "Restore previous
 * versions"), it's added at once and brought to the front, whether or not
 * there are snapshots. */
void xd_versions_page_attach(GtkWidget *dialog, GtkNotebook *notebook, GFile *file,
                             gboolean is_directory, gboolean show_now);

G_END_DECLS

#endif
