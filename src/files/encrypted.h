/* SPDX-License-Identifier: MIT
 * My Computer's encrypted (LUKS) drives, through UDisks2's Encrypted
 * interface: opening a locked one asks for its passphrase (or uses the
 * one remembered in the Secret Service), unlocks and mounts it; Lock
 * unmounts and locks it again; its Properties have an Encryption tab
 * (Change Passphrase..., Lock, Forget the remembered passphrase).
 */
#ifndef XD_FILES_ENCRYPTED_H
#define XD_FILES_ENCRYPTED_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Whether `item` is an encrypted drive that opening must unlock or
 * mount first (computer:///encrypted/...). */
gboolean xd_encrypted_needs_unlock(GFile *item);

/* Unlocks (asking for the passphrase unless one is remembered) and
 * mounts the drive, then calls opened(root, data) with its mount's
 * root. Problems are shown over `parent`; opened isn't called after a
 * problem, a Cancel, or once `parent` is closed. */
void xd_encrypted_open(GtkWindow *parent, GFile *item,
                       void (*opened)(GFile *root, gpointer data), gpointer data);

/* Whether `item` is an unlocked encrypted drive, which Lock applies to;
 * and locking it (unmounting it first). */
gboolean xd_encrypted_can_lock(GFile *item);
void xd_encrypted_lock(GtkWindow *parent, GFile *item);

/* The drive's Encryption page for its Properties (`dialog`), or nullptr
 * when it isn't encrypted. */
GtkWidget *xd_encrypted_page(GtkWidget *dialog, GFile *item, const char *name);

G_END_DECLS

#endif
