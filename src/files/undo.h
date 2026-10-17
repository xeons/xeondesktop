/* SPDX-License-Identifier: MIT
 * Edit > Undo, as Windows' Explorer has it: the last ten renames, copies,
 * moves and deletions to the Recycle Bin, taken back latest first.
 */
#ifndef XD_FILES_UNDO_H
#define XD_FILES_UNDO_H

#include "fileops.h"

G_BEGIN_DECLS

/* A finished operation (xd_fileop_start_undoable): `from` is what each of
 * `created` came from (a move's or copy's), or what went to the Recycle
 * Bin (`created` empty). Nothing done: nothing recorded. */
void xd_undo_add_fileop(XdFileOpKind kind, GList *from, GList *created);
/* A rename in place: `renamed` is the file now, and `old_text` what its
 * name (a launcher's Name) was. */
void xd_undo_add_rename(GFile *renamed, const char *old_text);

/* The Edit menu's item: "Undo Rename", "Undo Move", "Undo Copy",
 * "Undo Delete", or nullptr when there's nothing to undo. */
const char *xd_undo_label(void);

/* Undoes the latest: renames back, moves back, sends copies to the
 * Recycle Bin (asking) or restores what was sent there. `done` gets the
 * files as they are again (to select them). */
void xd_undo(GtkWindow *parent, XdFileOpDone done, gpointer user);

G_END_DECLS

#endif
