/* SPDX-License-Identifier: MIT
 * UDisks2 (org.freedesktop.UDisks2) for My Computer's drives: which
 * filesystem a drive is, its label, the disk drives themselves, and the
 * calls that change them (SetLabel, Check, Repair). UDisks asks polkit
 * for anything that needs it; nothing here runs as root.
 */
#ifndef XD_FILES_UDISKS_H
#define XD_FILES_UDISKS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define XD_UDISKS_FILESYSTEM "org.freedesktop.UDisks2.Filesystem"

/* UDisks2's objects, kept up to date for the life of the process;
 * nullptr when there's no system bus or UDisks2 isn't running (asked
 * once). Owned here. */
GDBusObjectManager *xd_udisks_manager(void);

/* A filesystem UDisks2 knows: what a drive in My Computer is. */
typedef struct XdUdisksFs {
    char *object; /* the block device's object path */
    char *drive;  /* its Drive's object path, or "/" */
    char *device; /* "/dev/sda2" */
    char *label;  /* IdLabel, "" when none */
    char *type;   /* IdType: "ext4", "vfat", ... */
    gboolean mounted;
} XdUdisksFs;

/* The filesystem behind a My Computer item: a drive's root (by its mount
 * points) or a volume not mounted yet (computer:///volume/..., by its
 * device or UUID); nullptr if UDisks2 doesn't know it. */
XdUdisksFs *xd_udisks_fs_for(GFile *item);
/* The filesystem mounted at `path` exactly, or nullptr. */
XdUdisksFs *xd_udisks_fs_at(const char *path);
void xd_udisks_fs_free(XdUdisksFs *fs);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdUdisksFs, xd_udisks_fs_free)

/* A disk drive, for a drive's Hardware tab. */
typedef struct XdUdisksDrive {
    char *object;
    char *name;      /* "WDC WD800JB", "+ USB Device" on USB, as XP names them */
    char *type;      /* "Disk drives", "DVD/CD-ROM drives" */
    char *vendor, *model, *revision, *serial, *bus;
    char *device;    /* the whole disk's block device ("/dev/sda"), or "" */
    char *table;     /* the partition table: "gpt", "dos" or "" */
    guint64 size;    /* bytes; 0 without media */
    gint rotation;   /* rpm; 0 for solid state, -1 unknown */
    gboolean removable, optical;
    char *sort_key;
} XdUdisksDrive;

/* Every disk drive, in UDisks2's order (SortKey). Free with
 * g_ptr_array_unref. */
GPtrArray *xd_udisks_drives(void);

/* A volume on a drive, for its Volumes tab. */
typedef struct XdUdisksVolume {
    char *device; /* "/dev/sda2" */
    char *label;
    char *type;   /* IdType */
    guint64 size;
} XdUdisksVolume;

/* The volumes (filesystems) on the drive at `drive_object`, by device.
 * Free with g_ptr_array_unref. */
GPtrArray *xd_udisks_volumes(const char *drive_object);

#define XD_UDISKS_BLOCK "org.freedesktop.UDisks2.Block"
#define XD_UDISKS_ENCRYPTED "org.freedesktop.UDisks2.Encrypted"

/* An encrypted (LUKS) volume UDisks2 knows: locked, or unlocked with its
 * cleartext device (what's inside). */
typedef struct XdUdisksCrypt {
    char *object;      /* the encrypted block device's object path */
    char *drive;       /* its Drive's object path, or "/" */
    char *device;      /* "/dev/sdb1" */
    char *uuid;        /* the LUKS header's UUID */
    char *label;       /* the LUKS2 header's label, "" when none */
    char *version;     /* IdVersion: "2", "1" */
    guint64 size;
    gboolean removable; /* on a removable drive */
    gboolean ignore;    /* HintIgnore: not to be shown */
    char *cleartext;   /* the cleartext block's object path; nullptr when locked */
    gboolean has_fs;   /* the cleartext holds a file system */
    char *inner_label; /* the file system's label, "" when locked or none */
    char *mount_point; /* where it's mounted, or nullptr */
} XdUdisksCrypt;

void xd_udisks_crypt_free(XdUdisksCrypt *udisks_crypt);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdUdisksCrypt, xd_udisks_crypt_free)

/* Every encrypted volume, by device. Free with g_ptr_array_unref. */
GPtrArray *xd_udisks_encrypted(void);
/* The encrypted volume a My Computer item is: its computer:///encrypted/
 * item, or the root of its cleartext's mount; nullptr for anything
 * else. */
XdUdisksCrypt *xd_udisks_crypt_for(GFile *item);
/* Whether `device` ("/dev/sdb1", "/dev/dm-0", a /dev/mapper or
 * /dev/disk link) is an encrypted volume or one's cleartext, which My
 * Computer lists itself rather than as GIO's volumes. */
gboolean xd_udisks_is_crypt_device(const char *device);

/* Calls `method` of `iface` on `object`, letting polkit ask (no
 * timeout: polkit's box, and checking a disk, take a while). Finish with
 * xd_udisks_call_finish, which strips D-Bus's error name from messages
 * and keeps it in *dbus_error (if given, free with g_free). */
void xd_udisks_call(const char *object, const char *iface, const char *method, GVariant *args,
                    GCancellable *cancel, GAsyncReadyCallback done, gpointer data);
GVariant *xd_udisks_call_finish(GAsyncResult *res, char **dbus_error, GError **err);

/* Manager.CanCheck / CanRepair for a filesystem type: whether UDisks2
 * (2.7 and later) can check or repair it here. FALSE when it can't say. */
gboolean xd_udisks_can(const char *what, const char *fs_type);

G_END_DECLS

#endif
