/* SPDX-License-Identifier: MIT
 * The passphrase boxes of encrypted volumes, for My Computer and Disk
 * Management: unlocking one (as XP's Connect to box asks for a
 * password) and changing its passphrase (as XP's Change Password).
 */
#ifndef XD_FILES_LUKSDLG_H
#define XD_FILES_LUKSDLG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* "Unlock <name>": the passphrase, or nullptr when cancelled (free with
 * xd_luks_passphrase_free). `retry`: the last one typed was wrong. With
 * `remember` given, "Remember my passphrase" is offered, ticked as
 * *remember is, and *remember is set from it. */
char *xd_luks_ask_passphrase(GtkWindow *parent, const char *name, gboolean retry,
                             gboolean *remember);

/* "Change Passphrase for <name>": the current passphrase, and the new
 * one typed twice (checked to match). FALSE when cancelled; else *old
 * and *new_passphrase are set (free with xd_luks_passphrase_free). */
gboolean xd_luks_ask_change(GtkWindow *parent, const char *name, char **old,
                            char **new_passphrase);

G_END_DECLS

#endif
