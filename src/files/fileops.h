/* SPDX-License-Identifier: MIT
 * File operations for xd-files, as Windows XP's Explorer does them: copy,
 * move, shortcut, Recycle Bin and permanent delete, with the "Copying..."
 * progress dialog, "Confirm File Replace" questions and the Edit menu's
 * clipboard (shared with other file managers through
 * x-special/gnome-copied-files).
 *
 * The work runs on a worker thread with GIO's synchronous calls;
 * questions and progress come back to the GTK main thread.
 */
#ifndef XD_FILES_FILEOPS_H
#define XD_FILES_FILEOPS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
    XD_FILEOP_COPY,
    XD_FILEOP_MOVE,
    XD_FILEOP_LINK,   /* symbolic links ("Shortcut to ...") */
    XD_FILEOP_TRASH,  /* to the Recycle Bin */
    XD_FILEOP_DELETE, /* permanently */
    /* Archives (extract.h), into the destination folder as they are, or
     * each into a folder of its own name there. */
    XD_FILEOP_EXTRACT_HERE,
    XD_FILEOP_EXTRACT_FOLDER,
} XdFileOpKind;

/* Called on the main thread when the operation ends: `created` lists the
 * new files at the destination (GFile*, owned by the operation). Not
 * called if the parent window has closed meanwhile; always called when
 * started without one. */
typedef void (*XdFileOpDone)(GList *created, gpointer user);

/* Starts an operation on `sources` (GFile*; the list is copied).
 * `dest` is the destination folder for copy, move, link and extract, and is
 * ignored for trash and delete. Trash and delete ask for confirmation
 * first, as Windows does. */
void xd_fileop_start(GtkWindow *parent, XdFileOpKind kind, GList *sources, GFile *dest,
                     XdFileOpDone done, gpointer user);

/* As xd_fileop_start; returns FALSE (and calls nothing) when the user
 * says No to deleting. */
gboolean xd_fileop_start_checked(GtkWindow *parent, XdFileOpKind kind, GList *sources,
                                 GFile *dest, XdFileOpDone done, gpointer user);

/* As xd_fileop_start_checked, and Edit > Undo can take back what it did
 * once it's finished (undo.h): the user's copies, moves and deletions
 * to the Recycle Bin, not the undoing itself. */
gboolean xd_fileop_start_undoable(GtkWindow *parent, XdFileOpKind kind, GList *sources,
                                  GFile *dest, XdFileOpDone done, gpointer user);

/* The clipboard: Cut (move on paste) or Copy. */
void xd_clipboard_set_files(GtkWidget *widget, GList *files, gboolean cut);
/* Asks for the clipboard's files; `got` receives them (GFile*, or
 * nullptr when there are none) and whether they were cut. */
typedef void (*XdClipboardFiles)(GList *files, gboolean cut, gpointer user);
void xd_clipboard_get_files(GtkWidget *widget, XdClipboardFiles got, gpointer user);
/* Whether the clipboard has files to paste (tracked as it changes). */
gboolean xd_clipboard_has_files(void);
/* After a cut's files are pasted, the clipboard is emptied, as in
 * Windows. */
void xd_clipboard_clear_cut(GtkWidget *widget);

/* A name for a new item in `dir`: `base`, else "base (2)", ...;
 * `ext` (may be nullptr) follows the number. Free with g_free. */
char *xd_unique_name(GFile *dir, const char *base, const char *ext);

G_END_DECLS

#endif
