/* SPDX-License-Identifier: MIT
 * Previous Versions: a file's or folder's copies in the file system's
 * snapshots, found without GTK so the tests can run it on a fake tree.
 *
 * Where they come from:
 * - Snapper (btrfs), asked over D-Bus (org.opensuse.Snapper on the system
 *   bus): each config's SUBVOLUME, and its snapshots' numbers, dates and
 *   descriptions. A snapshot's tree is SUBVOLUME/.snapshots/N/snapshot.
 * - ZFS: the dataset's mount point's .zfs/snapshot/NAME, dated by the
 *   snapshot's directory.
 * - Timeshift: BACKUP/timeshift/snapshots/YYYY-MM-DD_HH-MM-SS/localhost
 *   (rsync), or BACKUP/timeshift-btrfs/snapshots/DATE/@ (and @home) while
 *   Timeshift has its btrfs backup mounted, BACKUP being one of
 *   /run/timeshift/<n>/backup or / (Timeshift's /timeshift).
 * Snapshots we can't read are left out quietly, as if there were none:
 * Snapper's are root's unless its ALLOW_USERS and SYNC_ACL let users in.
 *
 * XD_SNAPSHOT_ROOTS, when set, replaces all of that with its own list,
 * separated by colons: "snapper" (ask Snapper), "zfs=MOUNTPOINT" and
 * "timeshift=BACKUP". The tests use it to look only in scratch trees.
 */
#ifndef XD_FILES_VERSIONS_H
#define XD_FILES_VERSIONS_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct xd_version {
    char *path;                  /* the item's copy in the snapshot */
    char *location;              /* "Snapper snapshot 42", for 7's Location column */
    gint64 snapshot_time;        /* when the snapshot was taken (seconds since the epoch) */
    gint64 modified_time;        /* the copy's modification time (seconds) */
    gint64 modified_nanoseconds; /* and below the second, to tell copies apart */
    guint64 size;
    gboolean is_directory;
} xd_version;

void xd_version_free(xd_version *version);

/* The previous versions of `path` (xd_version*), newest snapshot first.
 * Versions identical to the newer one before them (the same size and
 * modification time), or to the item as it is now, are left out, so only
 * those where it changed are listed. `has_snapshots` (may be nullptr) says
 * whether the file system holding `path` has any snapshots we can read at
 * all, even if none has the item. Blocks (D-Bus, many stats): call it from
 * a thread. Free with g_ptr_array_unref. */
GPtrArray *xd_versions_list(const char *path, gboolean *has_snapshots);

/* Whether any snapshot source covers `path` (a Snapper config's
 * subvolume holds it, it's on ZFS, a Timeshift backup is there), without
 * looking for its versions: the Restore previous versions menu item.
 * Snapper's configs are remembered for a minute. */
gboolean xd_versions_can_have_versions(const char *path);

/* Copies `source` (a version's path) over `target`: a file replaces the
 * file there; a folder's contents are copied into the folder, replacing
 * what's there of the same name and leaving what the snapshot didn't
 * have. Never follows a symbolic link at `target` or inside it. Everything
 * that can be copied is; the first error is returned. */
gboolean xd_versions_restore(const char *source, const char *target, GCancellable *cancellable,
                             GError **error);

G_END_DECLS

#endif
