/* SPDX-License-Identifier: MIT
 * Encrypted (LUKS) volumes, for My Computer and Disk Management: what
 * they're called, where My Computer lists a locked one, the Secret
 * Service attributes a remembered passphrase is kept under, and the
 * /etc/crypttab entry that unlocks one at startup. No GTK or D-Bus here,
 * so it can be tested on its own.
 */
#ifndef XD_FILES_LUKS_H
#define XD_FILES_LUKS_H

#include <glib.h>

G_BEGIN_DECLS

/* Remembered passphrases go in the Secret Service as gvfs and GNOME
 * Disks keep them, so a passphrase remembered by either works here and
 * the other way round: this schema, the one attribute (the LUKS
 * header's UUID), and a label naming the volume. */
#define XD_LUKS_SECRET_SCHEMA "org.gnome.GVfs.Luks.Password"
#define XD_LUKS_SECRET_ATTRIBUTE "gvfs-luks-uuid"
char *xd_luks_secret_label(const char *volume_name);

/* A locked volume (or an unlocked one not mounted yet) in My Computer:
 * computer:///encrypted/ and its LUKS UUID. Free with g_free. */
#define XD_LUKS_ITEM_PREFIX "computer:///encrypted/"
char *xd_luks_item_uri(const char *uuid);
/* The UUID in such an item's URI, or nullptr for anything else. */
char *xd_luks_item_uuid(const char *uri);

/* The volume's name in My Computer: the file system's label inside
 * when it's unlocked, else the LUKS header's own label, else "<size>
 * Encrypted Volume" (`size`, already formatted, may be nullptr). */
char *xd_luks_volume_name(const char *inner_label, const char *luks_label, const char *size);

/* "LUKS2", "LUKS1", or "LUKS" from IdVersion ("2", "1"). */
char *xd_luks_version_label(const char *id_version);

/* Why a new passphrase typed twice can't be used, or nullptr: it's empty
 * or the two differ (as XP says of passwords). */
const char *xd_luks_passphrase_problem(const char *passphrase, const char *confirm);

/* Whether UDisks2's error for Unlock or ChangePassphrase means the
 * passphrase was wrong (cryptsetup says so in several ways), rather
 * than something else failing. `dbus_error` is the D-Bus error name. */
gboolean xd_luks_wrong_passphrase(const char *dbus_error, const char *message);

/* Wipes a passphrase from memory and frees it. */
void xd_luks_passphrase_free(char *passphrase);

/* /etc/crypttab: the mapping's name for the volume ("luks-<uuid>", as
 * systemd and GNOME Disks name it), and the key file UDisks2 writes when
 * the passphrase is to be kept on this computer (/etc/luks-keys/<name>).
 * Free with g_free. */
char *xd_crypttab_name(const char *luks_uuid);
char *xd_crypttab_key_path(const char *name);

G_END_DECLS

#endif
