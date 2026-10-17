/* SPDX-License-Identifier: MIT
 * Windows XP's special folders for xd-files: My Computer (computer:///,
 * the drives and mounts from GIO's volume monitor) and the Recycle Bin
 * (trash:///, the freedesktop.org trash in $XDG_DATA_HOME/Trash, read
 * directly so GNOME's gvfs isn't needed).
 */
#ifndef XD_FILES_PLACES_H
#define XD_FILES_PLACES_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum { XD_PLACE_FOLDER, XD_PLACE_COMPUTER, XD_PLACE_TRASH } XdPlace;

#define XD_COMPUTER_URI "computer:///"
#define XD_TRASH_URI "trash:///"

/* What `location` is: an ordinary folder or one of the special ones. */
XdPlace xd_place_of(GFile *location);
/* The special folders' locations (new references). */
GFile *xd_place_computer(void);
GFile *xd_place_trash(void);
/* "My Computer", "Recycle Bin" or nullptr; and the icon to show. */
const char *xd_place_title(GFile *location);
const char *xd_place_icon(GFile *location);
/* "/" and mount points: their "Up" is My Computer, as drives are in XP. */
gboolean xd_place_is_drive_root(GFile *dir);
/* A location typed in the address bar or given on the command line:
 * "My Computer", "Recycle Bin", "computer:", "trash:" (else nullptr). */
GFile *xd_place_parse(const char *text);

/* ---- My Computer ---- */

typedef struct XdDrive {
    GFile *root;
    char *name;
    char *kind; /* "Local Disk", "Removable Disk", "CD Drive", ... */
    GIcon *icon;
    guint64 total, free; /* bytes; 0 when unknown */
} XdDrive;

/* The drives: the root file system, the user's documents, then mounts.
 * Free with g_ptr_array_unref. */
GPtrArray *xd_computer_drives(void);
/* "12.3 GB", as Windows shows drive sizes. Free with g_free. */
char *xd_format_drive_size(guint64 bytes);
/* The volume monitor, for live updates (owned by GIO). */
GVolumeMonitor *xd_computer_monitor(void);

/* ---- Recycle Bin ---- */

/* $XDG_DATA_HOME/Trash/files, created if missing. */
GFile *xd_trash_files_dir(void);
/* For a file in the trash: where it came from (full path) and when it
 * was deleted (local time, may be nullptr). FALSE without its .trashinfo. */
gboolean xd_trash_item_info(GFile *trashed, char **original_path, GDateTime **deleted);
gboolean xd_trash_is_empty(void);
/* Moves the files back where they came from; reports problems. */
void xd_trash_restore(GtkWindow *parent, GList *files);
/* Empty Recycle Bin: asks, then deletes everything in it (fileops). */
void xd_trash_empty(GtkWindow *parent, void (*done)(GList *created, gpointer user),
                    gpointer user);
/* Deletes .trashinfo files whose item is gone (after deleting or moving
 * items out of the trash). */
void xd_trash_tidy(void);

/* ---- renaming ---- */

/* The name Rename starts from: a launcher's Name (what's shown), else the
 * file's name. Free with g_free. */
char *xd_rename_text(GFile *file);
/* Renames as Windows renames a shortcut: a launcher keeps its file and
 * gets a new Name; anything else is renamed. Returns the file's new
 * location (a new reference), or nullptr with `err` set. */
GFile *xd_rename(GFile *file, const char *text, GError **err);

G_END_DECLS

#endif
