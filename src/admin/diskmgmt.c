/* SPDX-License-Identifier: MIT
 * xd-diskmgmt: Windows XP's Disk Management (Computer Management >
 * Storage), over UDisks2 (org.freedesktop.UDisks2): the volume list
 * above, the disk map below in XP's colours (primary partitions, the
 * extended partition and its logical drives, free and unallocated
 * space).
 *
 * Everything goes through UDisks2, which asks polkit; nothing runs as
 * root here. Initialize Disk (a GPT or MBR partition table), New
 * Partition, Delete Partition, Format (quick or full, with a choice of
 * allocation unit size), Extend and Shrink Volume, Change Label, Mark
 * Partition as Active, Change Partition Type, mounting and unmounting
 * and the mount point used at startup (XP's Change Drive Letter and
 * Paths; /etc/fstab, written by UDisks2), Eject.
 *
 * As Windows does for the system and boot volumes, what the running
 * system uses ("/", "/boot", "/usr", swap in use, ...) can't be deleted,
 * formatted or shrunk: those are greyed with the reason. Other mounted
 * volumes are unmounted first. Every destructive action asks first,
 * naming the disk, the partition and its size.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../core/common.h"
#include "../files/luks.h"
#include "../files/luksdlg.h"
#include "../gtkapp/gtkapp.h"
#include "admin.h"

#define UD "org.freedesktop.UDisks2"
#define UD_PATH "/org/freedesktop/UDisks2"
#define UD_MANAGER UD ".Manager"
#define UD_DRIVE UD ".Drive"
#define UD_BLOCK UD ".Block"
#define UD_TABLE UD ".PartitionTable"
#define UD_PART UD ".Partition"
#define UD_FS UD ".Filesystem"
#define UD_SWAP UD ".Swapspace"
#define UD_ENC UD ".Encrypted"

/* The MBR boot flag in Partition.Flags. */
static constexpr guint64 DOS_ACTIVE = 0x80;

/* The disk map. */
static constexpr int HEADER_W = 112;
static constexpr int ROW_H = 74;
static constexpr int ROW_GAP = 8;
static constexpr int MARGIN = 6;
static constexpr int MIN_BLOCK = 76;
static constexpr int BAND = 10;

typedef struct rgb {
    double r, g, b;
} rgb;

typedef struct disk disk;

typedef struct volume {
    disk *d;
    char *path;    /* the block object */
    char *fs_path; /* where its Filesystem is: itself, or an unlocked volume's cleartext */
    char *device;  /* "/dev/sdb1" */
    char *label, *id_type, *id_version, *id_usage;
    char *fs_type; /* the file system's IdType: id_type, or what's inside an unlocked volume */
    char **mounts; /* nullptr-terminated, maybe empty */
    char *uuid;    /* the filesystem's UUID, "" when it has none */
    char **fstab;  /* its /etc/fstab mount points */
    GPtrArray *fstab_items; /* ... and their entries, "(sa{sv})" GVariants, in the same order */
    guint64 offset, size, flags;
    guint number;
    char *ptype;
    bool partition, container, contained, whole_disk;
    bool has_fs, swap_active, unlocked;
    /* An encrypted (LUKS) volume: its header's UUID, and its
     * /etc/crypttab entries ("(sa{sv})" GVariants) on `path`. */
    bool encrypted;
    char *luks_uuid;
    GPtrArray *crypttab_items;
    guint64 fs_total, fs_free; /* from statvfs when mounted; 0 when unknown */
} volume;

struct disk {
    int number; /* Disk N, or CD-ROM N */
    char *drive_path, *block_path, *device, *model, *serial, *bus, *table;
    guint64 size;
    bool removable, ejectable, optical, media, system;
    GPtrArray *vols;   /* volume *: partitions (containers too) or a whole-disk volume */
    GArray *segments;  /* xd_disk_segment, indexing vols */
};

/* What's selected: a disk (its header), or one block of its map. */
typedef struct selection {
    disk *d;
    int seg; /* index into d->segments, or -1 for the header */
} selection;

typedef struct dm {
    GtkWidget *window, *list, *map, *status;
    GtkListStore *store;
    GDBusConnection *bus;
    GDBusObjectManager *om;
    GPtrArray *disks;
    selection sel;
    GHashTable *can_format; /* fs type -> "yes" / "no" */
    GHashTable *resize;     /* fs type -> modes + 1 (0: not known yet) */
    GHashTable *asked;      /* "format:TYPE" / "resize:TYPE" asked of UDisks */
    char *version;          /* UDisks2's Manager.Version, once read */
    char *luks_type;        /* the encryption it makes ("luks2"), once read; nullptr: none */
    bool luks_read;
    GtkWidget *action_menu; /* the menu bar's Action menu */
    bool action_stale;      /* to refill once it closes */
    guint refresh_source;
    bool busy, updating;
} dm;

enum {
    COL_ICON,
    COL_VOLUME,
    COL_LAYOUT,
    COL_TYPE,
    COL_FS,
    COL_STATUS,
    COL_CAPACITY,
    COL_FREE,
    COL_PCT,
    COL_SIZE,      /* sort keys */
    COL_FREE_SIZE,
    COL_PCT_NUM,
    COL_VOL,       /* volume * */
    NCOLS
};

/* The filesystems New Partition and Format offer, when UDisks can make
 * them. */
static const struct {
    const char *type, *label;
} formats[] = {
    {"ext4", "ext4"}, {"btrfs", "Btrfs"}, {"xfs", "XFS"},
    {"vfat", "FAT32"}, {"exfat", "exFAT"}, {"ntfs", "NTFS"},
};

/* ---- reading UDisks2 ---------------------------------------------------------------- */

static GDBusProxy *iface(GDBusObject *dbus_object, const char *name) {
    return dbus_object != nullptr ? G_DBUS_PROXY(g_dbus_object_get_interface(dbus_object, name))
                                  : nullptr;
}

static GVariant *prop(GDBusProxy *proxy, const char *name) {
    return proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
}

static char *prop_str(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name);
    char *text = nullptr;
    if (variant != nullptr && (g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING) ||
                               g_variant_is_of_type(variant, G_VARIANT_TYPE_OBJECT_PATH)))
        text = g_variant_dup_string(variant, nullptr);
    else if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BYTESTRING))
        text = g_variant_dup_bytestring(variant, nullptr);
    if (variant != nullptr)
        g_variant_unref(variant);
    return text != nullptr ? text : g_strdup("");
}

static guint64 prop_u64(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name);
    guint64 value = 0;
    if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT64))
        value = g_variant_get_uint64(variant);
    else if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT32))
        value = g_variant_get_uint32(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

static bool prop_bool(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name);
    bool value = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BOOLEAN) &&
                 g_variant_get_boolean(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

/* MountPoints (aay) as strings. */
static char **prop_bytestrings(GDBusProxy *proxy, const char *name) {
    GVariant *variant = prop(proxy, name);
    char **out =
        variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BYTESTRING_ARRAY)
            ? g_variant_dup_bytestring_array(variant, nullptr)
            : nullptr;
    if (variant != nullptr)
        g_variant_unref(variant);
    return out != nullptr ? out : g_new0(char *, 1);
}

/* The fstab mount points in Block.Configuration, and (into `items`) the
 * entries themselves, as Update- and RemoveConfigurationItem want them
 * back. */
static char **fstab_dirs(GDBusProxy *block, GPtrArray *items) {
    GPtrArray *dirs = g_ptr_array_new();
    GVariant *variant = prop(block, "Configuration");
    if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE("a(sa{sv})"))) {
        GVariantIter it;
        GVariant *entry = nullptr;
        g_variant_iter_init(&it, variant);
        while ((entry = g_variant_iter_next_value(&it)) != nullptr) {
            const char *kind = nullptr;
            GVariant *details = nullptr;
            g_variant_get(entry, "(&s@a{sv})", &kind, &details);
            GVariant *dir = strcmp(kind, "fstab") == 0
                                ? g_variant_lookup_value(details, "dir", G_VARIANT_TYPE_BYTESTRING)
                                : nullptr;
            if (dir != nullptr) {
                g_ptr_array_add(dirs, g_variant_dup_bytestring(dir, nullptr));
                g_ptr_array_add(items, g_variant_ref(entry));
                g_variant_unref(dir);
            }
            g_variant_unref(details);
            g_variant_unref(entry);
        }
    }
    if (variant != nullptr)
        g_variant_unref(variant);
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

/* An encrypted volume's own block (not its cleartext's): its LUKS UUID
 * and /etc/crypttab entries. */
static void read_crypt(volume *volume_info, GDBusObject *dbus_object) {
    GDBusProxy *enc = iface(dbus_object, UD_ENC);
    GDBusProxy *proxy = iface(dbus_object, UD_BLOCK);
    volume_info->encrypted = enc != nullptr;
    g_free(volume_info->luks_uuid);
    volume_info->luks_uuid = prop_str(proxy, "IdUUID");
    if (volume_info->crypttab_items != nullptr)
        g_ptr_array_unref(volume_info->crypttab_items);
    volume_info->crypttab_items = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
    GVariant *variant = prop(proxy, "Configuration");
    if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE("a(sa{sv})"))) {
        GVariantIter it;
        GVariant *entry = nullptr;
        g_variant_iter_init(&it, variant);
        while ((entry = g_variant_iter_next_value(&it)) != nullptr) {
            const char *kind = nullptr;
            g_variant_get(entry, "(&s@a{sv})", &kind, nullptr);
            if (strcmp(kind, "crypttab") == 0)
                g_ptr_array_add(volume_info->crypttab_items, g_variant_ref(entry));
            g_variant_unref(entry);
        }
    }
    if (variant != nullptr)
        g_variant_unref(variant);
    g_clear_object(&enc);
    g_clear_object(&proxy);
}

static void volume_free(gpointer data) {
    volume *volume_info = data;
    g_free(volume_info->path);
    g_free(volume_info->fs_path);
    g_free(volume_info->device);
    g_free(volume_info->label);
    g_free(volume_info->id_type);
    g_free(volume_info->id_version);
    g_free(volume_info->id_usage);
    g_strfreev(volume_info->mounts);
    g_strfreev(volume_info->fstab);
    if (volume_info->fstab_items != nullptr)
        g_ptr_array_unref(volume_info->fstab_items);
    g_free(volume_info->uuid);
    g_free(volume_info->luks_uuid);
    g_free(volume_info->fs_type);
    if (volume_info->crypttab_items != nullptr)
        g_ptr_array_unref(volume_info->crypttab_items);
    g_free(volume_info->ptype);
    g_free(volume_info);
}

static void disk_free(gpointer data) {
    disk *disk_info = data;
    g_free(disk_info->drive_path);
    g_free(disk_info->block_path);
    g_free(disk_info->device);
    g_free(disk_info->model);
    g_free(disk_info->serial);
    g_free(disk_info->bus);
    g_free(disk_info->table);
    g_ptr_array_unref(disk_info->vols);
    if (disk_info->segments != nullptr)
        g_array_unref(disk_info->segments);
    g_free(disk_info);
}

static bool mounted(const volume *volume_info) {
    return volume_info->mounts != nullptr && volume_info->mounts[0] != nullptr;
}

/* The block's filesystem details, from `fs` (itself or its cleartext). */
static void read_fs(volume *volume_info, GDBusObject *fs_obj) {
    GDBusProxy *block_proxy = iface(fs_obj, UD_BLOCK);
    GDBusProxy *fs_proxy = iface(fs_obj, UD_FS);
    GDBusProxy *swap_proxy = iface(fs_obj, UD_SWAP);
    g_free(volume_info->label);
    g_free(volume_info->id_version);
    g_free(volume_info->id_usage);
    volume_info->label = prop_str(block_proxy, "IdLabel");
    volume_info->id_version = prop_str(block_proxy, "IdVersion");
    volume_info->id_usage = prop_str(block_proxy, "IdUsage");
    g_free(volume_info->fs_type);
    volume_info->fs_type = prop_str(block_proxy, "IdType");
    if (!volume_info->unlocked) {
        g_free(volume_info->id_type);
        volume_info->id_type = prop_str(block_proxy, "IdType");
    }
    g_strfreev(volume_info->mounts);
    volume_info->mounts =
        fs_proxy != nullptr ? prop_bytestrings(fs_proxy, "MountPoints") : g_new0(char *, 1);
    g_strfreev(volume_info->fstab);
    if (volume_info->fstab_items != nullptr)
        g_ptr_array_unref(volume_info->fstab_items);
    volume_info->fstab_items = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
    volume_info->fstab = fstab_dirs(block_proxy, volume_info->fstab_items);
    g_free(volume_info->uuid);
    volume_info->uuid = prop_str(block_proxy, "IdUUID");
    volume_info->has_fs = fs_proxy != nullptr;
    volume_info->swap_active = swap_proxy != nullptr && prop_bool(swap_proxy, "Active");
    g_free(volume_info->fs_path);
    volume_info->fs_path = g_strdup(g_dbus_object_get_object_path(fs_obj));
    struct statvfs st;
    if (mounted(volume_info) && statvfs(volume_info->mounts[0], &st) == 0) {
        volume_info->fs_total = (guint64)st.f_blocks * st.f_frsize;
        volume_info->fs_free = (guint64)st.f_bavail * st.f_frsize;
    }
    g_clear_object(&block_proxy);
    g_clear_object(&fs_proxy);
    g_clear_object(&swap_proxy);
}

static int compare_disks(gconstpointer left, gconstpointer right) {
    const disk *left_disk = left,
               *right_disk = right; /* g_ptr_array_sort_values passes the elements */
    if (left_disk->optical != right_disk->optical)
        return left_disk->optical ? 1 : -1;
    return left_disk->number - right_disk->number;
}

static int compare_vols(gconstpointer left, gconstpointer right) {
    const volume *left_volume = left, *right_volume = right;
    return left_volume->offset < right_volume->offset   ? -1
           : left_volume->offset > right_volume->offset ? 1
                                                        : 0;
}

static int compare_keys(gconstpointer left, gconstpointer right, gpointer keys) {
    return g_strcmp0(g_hash_table_lookup(keys, *(disk *const *)left),
                     g_hash_table_lookup(keys, *(disk *const *)right));
}

static void layout_disk(disk *disk_info) {
    xd_disk_part *parts = g_new0(xd_disk_part, disk_info->vols->len + 1);
    for (guint i = 0; i < disk_info->vols->len; i++) {
        volume *volume_info = g_ptr_array_index(disk_info->vols, i);
        parts[i] = (xd_disk_part){.offset = volume_info->offset,
                                  .size = volume_info->size,
                                  .index = (int)i,
                                  .container = volume_info->container};
    }
    if (disk_info->segments != nullptr)
        g_array_unref(disk_info->segments);
    disk_info->segments = xd_disk_segments(disk_info->size, parts, disk_info->vols->len);
    g_free(parts);
}

/* Everything from UDisks2's objects. */
static GPtrArray *read_disks(dm *disk_manager) {
    GPtrArray *disks = g_ptr_array_new_with_free_func(disk_free);
    if (disk_manager->om == nullptr)
        return disks;
    GList *objects = g_dbus_object_manager_get_objects(disk_manager->om);
    GHashTable *by_block = g_hash_table_new(g_str_hash, g_str_equal); /* block path -> disk */
    GHashTable *keys = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, g_free);
    /* Drives, and the block of each (the one that isn't a partition). */
    for (GList *list = objects; list != nullptr; list = list->next) {
        GDBusProxy *drive = iface(list->data, UD_DRIVE);
        if (drive == nullptr)
            continue;
        disk *disk_info = g_new0(disk, 1);
        disk_info->drive_path = g_strdup(g_dbus_object_get_object_path(list->data));
        disk_info->model = prop_str(drive, "Model");
        char *vendor = prop_str(drive, "Vendor");
        if (vendor[0] != '\0') {
            char *full = g_strdup_printf("%s %s", vendor, disk_info->model);
            g_free(disk_info->model);
            disk_info->model = full;
        }
        g_free(vendor);
        disk_info->serial = prop_str(drive, "Serial");
        disk_info->bus = prop_str(drive, "ConnectionBus");
        disk_info->size = prop_u64(drive, "Size");
        disk_info->removable = prop_bool(drive, "Removable") || prop_bool(drive, "MediaRemovable");
        disk_info->ejectable = prop_bool(drive, "Ejectable");
        disk_info->optical = prop_bool(drive, "Optical") ||
                             g_str_has_prefix(disk_info->model, "DVD") ||
                             g_str_has_prefix(disk_info->model, "CD");
        disk_info->media = prop_bool(drive, "MediaAvailable");
        disk_info->vols = g_ptr_array_new_with_free_func(volume_free);
        g_hash_table_insert(keys, disk_info, prop_str(drive, "SortKey"));
        g_ptr_array_add(disks, disk_info);
        g_object_unref(drive);
    }
    for (GList *list = objects; list != nullptr; list = list->next) {
        GDBusProxy *proxy = iface(list->data, UD_BLOCK);
        GDBusProxy *part = iface(list->data, UD_PART);
        if (proxy != nullptr && part == nullptr) {
            char *drive = prop_str(proxy, "Drive");
            for (guint i = 0; i < disks->len; i++) {
                disk *disk_info = g_ptr_array_index(disks, i);
                if (disk_info->block_path != nullptr || strcmp(disk_info->drive_path, drive) != 0)
                    continue;
                /* Not an unlocked volume's cleartext, which has the drive too. */
                char *backing = prop_str(proxy, "CryptoBackingDevice");
                bool cleartext = backing[0] != '\0' && strcmp(backing, "/") != 0;
                g_free(backing);
                if (!cleartext) {
                    disk_info->block_path = g_strdup(g_dbus_object_get_object_path(list->data));
                    disk_info->device = prop_str(proxy, "PreferredDevice");
                    guint64 size = prop_u64(proxy, "Size");
                    if (size > 0)
                        disk_info->size = size;
                    GDBusProxy *table = iface(list->data, UD_TABLE);
                    disk_info->table = table != nullptr ? prop_str(table, "Type") : nullptr;
                    g_clear_object(&table);
                    g_hash_table_insert(by_block, disk_info->block_path, disk_info);
                    /* A filesystem on the whole disk: a USB "superfloppy" or a CD. */
                    char *usage = prop_str(proxy, "IdUsage");
                    if (disk_info->table == nullptr && usage[0] != '\0') {
                        volume *volume_info = g_new0(volume, 1);
                        volume_info->d = disk_info;
                        volume_info->path = g_strdup(disk_info->block_path);
                        volume_info->device = g_strdup(disk_info->device);
                        volume_info->size = disk_info->size;
                        volume_info->whole_disk = true;
                        volume_info->id_type = prop_str(proxy, "IdType");
                        read_fs(volume_info, list->data);
                        read_crypt(volume_info, list->data);
                        g_ptr_array_add(disk_info->vols, volume_info);
                    }
                    g_free(usage);
                }
            }
            g_free(drive);
        }
        g_clear_object(&proxy);
        g_clear_object(&part);
    }
    /* Partitions, by their table. */
    for (GList *list = objects; list != nullptr; list = list->next) {
        GDBusProxy *part = iface(list->data, UD_PART);
        if (part == nullptr)
            continue;
        char *table = prop_str(part, "Table");
        disk *disk_info = g_hash_table_lookup(by_block, table);
        g_free(table);
        if (disk_info != nullptr) {
            GDBusProxy *proxy = iface(list->data, UD_BLOCK);
            volume *volume_info = g_new0(volume, 1);
            volume_info->d = disk_info;
            volume_info->path = g_strdup(g_dbus_object_get_object_path(list->data));
            volume_info->device = prop_str(proxy, "PreferredDevice");
            volume_info->partition = true;
            volume_info->offset = prop_u64(part, "Offset");
            volume_info->size = prop_u64(part, "Size");
            volume_info->flags = prop_u64(part, "Flags");
            volume_info->number = (guint)prop_u64(part, "Number");
            volume_info->ptype = prop_str(part, "Type");
            volume_info->container = prop_bool(part, "IsContainer");
            volume_info->contained = prop_bool(part, "IsContained");
            volume_info->id_type = prop_str(proxy, "IdType");
            read_fs(volume_info, list->data);
            read_crypt(volume_info, list->data);
            g_ptr_array_add(disk_info->vols, volume_info);
            g_clear_object(&proxy);
        }
        g_object_unref(part);
    }
    /* Unlocked encrypted volumes: show what's inside. */
    for (GList *list = objects; list != nullptr; list = list->next) {
        GDBusProxy *proxy = iface(list->data, UD_BLOCK);
        char *backing = proxy != nullptr ? prop_str(proxy, "CryptoBackingDevice") : g_strdup("/");
        for (guint i = 0; strcmp(backing, "/") != 0 && i < disks->len; i++) {
            disk *disk_info = g_ptr_array_index(disks, i);
            for (guint k = 0; k < disk_info->vols->len; k++) {
                volume *volume_info = g_ptr_array_index(disk_info->vols, k);
                if (strcmp(volume_info->path, backing) == 0) {
                    volume_info->unlocked = true;
                    read_fs(volume_info, list->data);
                }
            }
        }
        g_free(backing);
        g_clear_object(&proxy);
    }
    g_list_free_full(objects, g_object_unref);
    g_hash_table_unref(by_block);

    /* Disk 0, 1, ... in UDisks2's order; CD-ROMs counted apart. */
    g_ptr_array_sort_with_data(disks, compare_keys, keys);
    int n_disk = 0, n_cd = 0;
    for (guint i = 0; i < disks->len; i++) {
        disk *disk_info = g_ptr_array_index(disks, i);
        disk_info->number = disk_info->optical ? n_cd++ : n_disk++;
        g_ptr_array_sort_values(disk_info->vols, compare_vols);
        for (guint k = 0; k < disk_info->vols->len; k++) {
            volume *volume_info = g_ptr_array_index(disk_info->vols, k);
            for (char **mp = volume_info->mounts; mp != nullptr && *mp != nullptr; mp++)
                if (xd_mount_is_system(*mp))
                    disk_info->system = true;
            disk_info->system |= volume_info->swap_active;
        }
        layout_disk(disk_info);
    }
    g_hash_table_unref(keys);
    g_ptr_array_sort_values(disks, compare_disks);
    return disks;
}

/* ---- names ------------------------------------------------------------------------------ */

static const char *dev_name(const char *device) {
    const char *slash = strrchr(device != nullptr ? device : "", '/');
    return slash != nullptr ? slash + 1 : (device != nullptr ? device : "");
}

static char *disk_name(const disk *disk_info) {
    return g_strdup_printf("%s %d", disk_info->optical ? "CD-ROM" : "Disk", disk_info->number);
}

/* "Data (/run/media/me/Data)", "(sdb1)": the Volume column. */
static char *volume_name(const volume *volume_info) {
    const char *where =
        mounted(volume_info) ? volume_info->mounts[0] : dev_name(volume_info->device);
    return volume_info->label != nullptr && volume_info->label[0] != '\0'
               ? g_strdup_printf("%s (%s)", volume_info->label, where)
               : g_strdup_printf("(%s)", where);
}

static char *volume_fs(const volume *volume_info) {
    char *fs = xd_disk_fs_label(volume_info->unlocked ? volume_info->fs_type : volume_info->id_type,
                                volume_info->id_version);
    if (volume_info->unlocked && volume_info->has_fs) {
        /* What's inside, read into fs_type, id_version and label by
         * read_fs. */
        char *out = g_strdup_printf("%s (encrypted)", fs);
        g_free(fs);
        return out;
    }
    if (fs[0] == '\0' && volume_info->partition && !volume_info->container) {
        g_free(fs);
        return g_strdup("RAW");
    }
    return fs;
}

static bool volume_active(const volume *volume_info) {
    return g_strcmp0(volume_info->d->table, "dos") == 0 && (volume_info->flags & DOS_ACTIVE) != 0;
}

static char *volume_status(const volume *volume_info) {
    return xd_volume_status((const char *const *)volume_info->mounts, volume_info->swap_active,
                            xd_part_type_is_esp(volume_info->ptype), volume_active(volume_info));
}

/* "Disk 1 Partition 2 (sdb2), 2.00 GB NTFS "Data"", for confirmations. */
static char *volume_description(const volume *volume_info) {
    char *dn = disk_name(volume_info->d);
    char *size = xd_disk_size_text(volume_info->size);
    char *fs = volume_fs(volume_info);
    GString *string = g_string_new(dn);
    if (volume_info->partition)
        g_string_append_printf(string, " Partition %u", volume_info->number);
    g_string_append_printf(string, " (%s), %s", dev_name(volume_info->device), size);
    if (volume_info->container)
        g_string_append(string, " extended partition");
    else if (fs[0] != '\0')
        g_string_append_printf(string, " %s", fs);
    if (volume_info->label != nullptr && volume_info->label[0] != '\0')
        g_string_append_printf(string, " \"%s\"", volume_info->label);
    g_free(dn);
    g_free(size);
    g_free(fs);
    return g_string_free(string, FALSE);
}

/* ---- selection ---------------------------------------------------------------------- */

static volume *seg_volume(const disk *disk_info, int seg) {
    if (disk_info == nullptr || seg < 0 || (guint)seg >= disk_info->segments->len)
        return nullptr;
    const xd_disk_segment *disk_segment = &g_array_index(disk_info->segments, xd_disk_segment, seg);
    return disk_segment->index >= 0 ? g_ptr_array_index(disk_info->vols, (guint)disk_segment->index)
                                    : nullptr;
}

static const xd_disk_segment *sel_segment(dm *disk_manager) {
    if (disk_manager->sel.d == nullptr || disk_manager->sel.seg < 0 ||
        (guint)disk_manager->sel.seg >= disk_manager->sel.d->segments->len)
        return nullptr;
    return &g_array_index(disk_manager->sel.d->segments, xd_disk_segment,
                          (guint)disk_manager->sel.seg);
}

static volume *sel_volume(dm *disk_manager) {
    return seg_volume(disk_manager->sel.d, disk_manager->sel.seg);
}

/* The extended partition of the selected disk, if it has one. */
static volume *extended_of(const disk *disk_info) {
    for (guint i = 0; disk_info != nullptr && i < disk_info->vols->len; i++) {
        volume *volume_info = g_ptr_array_index(disk_info->vols, i);
        if (volume_info->container)
            return volume_info;
    }
    return nullptr;
}

static void show_selection_status(dm *disk_manager);
static void fill_action_menu(dm *disk_manager);

static void select_volume_row(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(disk_manager->list));
    disk_manager->updating = true;
    gtk_tree_selection_unselect_all(ts);
    GtkTreeIter it;
    bool ok = volume_info != nullptr &&
              gtk_tree_model_get_iter_first(GTK_TREE_MODEL(disk_manager->store), &it);
    while (ok) {
        volume *rv = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(disk_manager->store), &it, COL_VOL, &rv, -1);
        if (rv == volume_info) {
            gtk_tree_selection_select_iter(ts, &it);
            break;
        }
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(disk_manager->store), &it);
    }
    disk_manager->updating = false;
}

/* Redraws disk `disk_info`'s row of the map. */
static void queue_draw_disk(dm *disk_manager, const disk *disk_info) {
    for (guint i = 0; disk_info != nullptr && i < disk_manager->disks->len; i++)
        if (g_ptr_array_index(disk_manager->disks, i) == disk_info)
            gtk_widget_queue_draw_area(disk_manager->map, 0, MARGIN + (int)i * (ROW_H + ROW_GAP),
                                       gtk_widget_get_allocated_width(disk_manager->map),
                                       ROW_H + 1);
}

/* Moves the selection, redrawing only the rows it leaves and enters. */
static void move_selection(dm *disk_manager, disk *disk_info, int seg) {
    queue_draw_disk(disk_manager, disk_manager->sel.d);
    disk_manager->sel = (selection){.d = disk_info, .seg = seg};
    queue_draw_disk(disk_manager, disk_info);
}

static void set_selection(dm *disk_manager, disk *disk_info, int seg) {
    move_selection(disk_manager, disk_info, seg);
    select_volume_row(disk_manager);
    show_selection_status(disk_manager);
}

/* ---- capabilities ----------------------------------------------------------------- */

/* CanFormat and CanResize are asked of UDisks in the background when the
 * disks are read (UDisks may take its time looking for mkfs and resize
 * tools), for each type New Partition and Format offer and each file
 * system on the disks. Menus and dialogs only look the answers up: until
 * one is in, the type can't be made or resized, and the Action menu is
 * refilled when it comes. */

typedef struct capability_ask {
    dm *m;
    char *key; /* in m->asked: "format:TYPE" or "resize:TYPE" */
    char *type;
    bool format;
} capability_ask;

static void on_capability(GObject *src, GAsyncResult *res, gpointer data) {
    capability_ask *query = data;
    dm *disk_manager = query->m;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    if (variant == nullptr) {
        /* No answer: asked again at the next refresh. */
        g_hash_table_remove(disk_manager->asked, query->key);
    } else if (query->format) {
        gboolean available = FALSE;
        g_variant_get(variant, "((b&s))", &available, nullptr);
        g_hash_table_insert(disk_manager->can_format, g_strdup(query->type),
                            g_strdup(available ? "yes" : "no"));
    } else {
        gboolean available = FALSE;
        guint64 flags = 0;
        g_variant_get(variant, "((bt&s))", &available, &flags, nullptr);
        guint64 modes = available ? flags : 0;
        g_hash_table_insert(disk_manager->resize, g_strdup(query->type),
                            GSIZE_TO_POINTER((gsize)modes + 1));
        fill_action_menu(disk_manager);
    }
    if (variant != nullptr)
        g_variant_unref(variant);
    g_free(query->key);
    g_free(query->type);
    g_free(query);
}

static void ask_capability(dm *disk_manager, const char *type, bool format) {
    if (disk_manager->bus == nullptr || type == nullptr || type[0] == '\0')
        return;
    char *key = g_strconcat(format ? "format:" : "resize:", type, nullptr);
    /* The set takes key either way. */
    if (!g_hash_table_add(disk_manager->asked, key))
        return;
    capability_ask *query = g_new0(capability_ask, 1);
    query->m = disk_manager;
    query->key = g_strdup(key);
    query->type = g_strdup(type);
    query->format = format;
    g_dbus_connection_call(disk_manager->bus, UD, UD_PATH "/Manager", UD_MANAGER,
                           format ? "CanFormat" : "CanResize", g_variant_new("(s)", type),
                           G_VARIANT_TYPE(format ? "((bs))" : "((bts))"), G_DBUS_CALL_FLAGS_NONE,
                           -1, nullptr, on_capability, query);
}

static void ask_capabilities(dm *disk_manager) {
    for (size_t i = 0; i < G_N_ELEMENTS(formats); i++)
        ask_capability(disk_manager, formats[i].type, true);
    for (guint i = 0; i < disk_manager->disks->len; i++) {
        const disk *disk_info = g_ptr_array_index(disk_manager->disks, i);
        for (guint k = 0; k < disk_info->vols->len; k++) {
            const volume *volume_info = g_ptr_array_index(disk_info->vols, k);
            if (volume_info->has_fs)
                ask_capability(disk_manager, volume_info->id_type, false);
        }
    }
}

static bool can_format(dm *disk_manager, const char *type) {
    return g_strcmp0(g_hash_table_lookup(disk_manager->can_format, type), "yes") == 0;
}

/* CanResize's modes for a filesystem type, 0 if it can't (or isn't known
 * yet). */
static guint64 resize_modes(dm *disk_manager, const char *type) {
    gpointer known = g_hash_table_lookup(disk_manager->resize, type);
    return known != nullptr ? GPOINTER_TO_SIZE(known) - 1 : 0;
}

/* ---- what can be done ---------------------------------------------------------------- */

static xd_volume_use volume_use(const volume *volume_info) {
    bool logicals = false;
    for (guint i = 0; volume_info->container && i < volume_info->d->vols->len; i++)
        logicals |= ((volume *)g_ptr_array_index(volume_info->d->vols, i))->contained;
    return (xd_volume_use){
        .mounts = (const char *const *)volume_info->mounts,
        .swap_active = volume_info->swap_active,
        .unlocked = volume_info->unlocked,
        .container = volume_info->container,
        .has_logical = logicals,
        .id_type = volume_info->id_type,
    };
}

/* Why `action` can't be done to the selected volume, or nullptr. */
static const char *action_reason(dm *disk_manager, volume *volume_info, xd_disk_action action,
                                 bool *unmount) {
    *unmount = false;
    if (disk_manager->busy)
        return "Disk Management is busy with another operation.";
    if (volume_info == nullptr)
        return "Select a volume first.";
    if (volume_info->whole_disk &&
        (action == XD_ACTION_DELETE || action == XD_ACTION_SHRINK || action == XD_ACTION_EXTEND ||
         action == XD_ACTION_SET_TYPE || action == XD_ACTION_SET_ACTIVE))
        return "This volume fills the whole disk, without a partition table.";
    if (volume_info->d->optical && action != XD_ACTION_LABEL)
        return "CDs and DVDs can't be changed here.";
    xd_volume_use usage = volume_use(volume_info);
    const char *why = xd_volume_lock_reason(&usage, action, unmount);
    if (why != nullptr)
        return why;
    if (action == XD_ACTION_SET_ACTIVE) {
        if (g_strcmp0(volume_info->d->table, "dos") != 0)
            return "Only MBR disks have an active partition; GPT disks start from the EFI "
                   "System Partition.";
        if (volume_info->contained)
            return "Only a primary partition can be active.";
        if (volume_active(volume_info))
            return "This partition is already active.";
    }
    if (action == XD_ACTION_SHRINK || action == XD_ACTION_EXTEND) {
        bool shrink = action == XD_ACTION_SHRINK;
        if (!volume_info->has_fs || volume_info->unlocked)
            return "Only a partition holding a file system can be resized.";
        guint64 modes = resize_modes(disk_manager, volume_info->id_type);
        xd_resize_how how = xd_disk_resize_how(modes, shrink, mounted(volume_info));
        switch (how) {
        case XD_RESIZE_NO:
            return shrink ? "This file system can't be shrunk."
                          : "This file system can't be extended.";
        case XD_RESIZE_NEED_MOUNT:
            return "This file system can only be resized while it's mounted. Mount it first.";
        case XD_RESIZE_NEED_UNMOUNT:
            for (char **mp = volume_info->mounts; *mp != nullptr; mp++)
                if (xd_mount_is_system(*mp))
                    return "This file system can only be resized unmounted, and the running "
                           "system needs it mounted.";
            *unmount = true;
            break;
        case XD_RESIZE_AS_IS:
            break;
        }
        if (!shrink) {
            guint seg = 0;
            for (guint i = 0; i < volume_info->d->segments->len; i++)
                if (seg_volume(volume_info->d, (int)i) == volume_info)
                    seg = i;
            if (xd_disk_grow_max(volume_info->d->segments, seg, volume_info->d->size) <=
                volume_info->size)
                return "There's no free space right after this partition to extend it into.";
        } else if (xd_disk_shrink_min(
                       volume_info->fs_total > 0 ? volume_info->fs_total - volume_info->fs_free : 0,
                       volume_info->size) +
                       XD_MIB >
                   volume_info->size) {
            return "This volume is too full to shrink.";
        }
    }
    return nullptr;
}

/* ---- operations ---------------------------------------------------------------------- */

/* One UDisks2 call of an operation. */
typedef struct step {
    char *path; /* nullptr: the partition the previous step created */
    char *iface, *method;
    GVariant *args;
} step;

typedef struct op {
    dm *m;
    char *title; /* "Formatting Disk 1 Partition 2..." */
    GQueue steps;
    char *created; /* CreatePartition's result */
} op;

static void refresh(dm *disk_manager);

static op *op_new(dm *disk_manager, const char *title) {
    op *operation = g_new0(op, 1);
    operation->m = disk_manager;
    operation->title = g_strdup(title);
    g_queue_init(&operation->steps);
    return operation;
}

static void op_add(op *operation, const char *path, const char *iface, const char *method,
                   GVariant *args) {
    step *operation_step = g_new0(step, 1);
    operation_step->path = g_strdup(path);
    operation_step->iface = g_strdup(iface);
    operation_step->method = g_strdup(method);
    operation_step->args = g_variant_ref_sink(args);
    g_queue_push_tail(&operation->steps, operation_step);
}

static void step_free(gpointer data) {
    step *operation_step = data;
    g_free(operation_step->path);
    g_free(operation_step->iface);
    g_free(operation_step->method);
    g_variant_unref(operation_step->args);
    g_free(operation_step);
}

static void op_free(op *operation) {
    g_queue_clear_full(&operation->steps, step_free);
    g_free(operation->title);
    g_free(operation->created);
    g_free(operation);
}

static GVariant *no_options(void) { return g_variant_new("a{sv}", nullptr); }

static void set_busy(dm *disk_manager, const char *what) {
    disk_manager->busy = what != nullptr;
    GdkWindow *gdk_window = gtk_widget_get_window(disk_manager->window);
    if (gdk_window != nullptr) {
        GdkCursor *cursor =
            what != nullptr
                ? gdk_cursor_new_from_name(gdk_window_get_display(gdk_window), "progress")
                : nullptr;
        gdk_window_set_cursor(gdk_window, cursor);
        g_clear_object(&cursor);
    }
    if (what != nullptr) {
        gtk_label_set_text(GTK_LABEL(disk_manager->status), what);
        fill_action_menu(disk_manager);
    } else {
        show_selection_status(disk_manager);
    }
}

static void op_next(op *operation);

static void step_done(GObject *src, GAsyncResult *res, gpointer data) {
    op *operation = data;
    dm *disk_manager = operation->m;
    GError *error = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (variant == nullptr) {
        g_dbus_error_strip_remote_error(error);
        char *text = g_strdup_printf("Disk Management could not complete the operation.\n\n%s",
                                     error->message);
        set_busy(disk_manager, nullptr);
        xd_message_box(GTK_WINDOW(disk_manager->window), "Disk Management", XD_MB_ERROR, XD_MB_OK,
                       FALSE, text);
        g_free(text);
        g_error_free(error);
        op_free(operation);
        refresh(disk_manager);
        return;
    }
    if (g_variant_is_of_type(variant, G_VARIANT_TYPE("(o)"))) {
        g_free(operation->created);
        g_variant_get(variant, "(o)", &operation->created);
    }
    g_variant_unref(variant);
    op_next(operation);
}

static void op_next(op *operation) {
    dm *disk_manager = operation->m;
    step *operation_step = g_queue_pop_head(&operation->steps);
    if (operation_step == nullptr) {
        set_busy(disk_manager, nullptr);
        op_free(operation);
        refresh(disk_manager);
        return;
    }
    const char *path = operation_step->path != nullptr ? operation_step->path : operation->created;
    if (path == nullptr) {
        step_free(operation_step);
        op_next(operation);
        return;
    }
    /* The new partition encrypted: its file system is in the cleartext
     * device UDisks2 left unlocked. */
    g_autofree char *cleartext = nullptr;
    if (operation_step->path == nullptr && strcmp(operation_step->iface, UD_FS) == 0 &&
        disk_manager->om != nullptr) {
        GDBusInterface *enc = g_dbus_object_manager_get_interface(disk_manager->om, path, UD_ENC);
        if (enc != nullptr) {
            cleartext = prop_str(G_DBUS_PROXY(enc), "CleartextDevice");
            if (strcmp(cleartext, "/") != 0 && cleartext[0] != '\0')
                path = cleartext;
            g_object_unref(enc);
        }
    }
    /* No timeout: polkit's password box, and formatting, take a while. */
    g_dbus_connection_call(disk_manager->bus, UD, path, operation_step->iface,
                           operation_step->method, operation_step->args, nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                           step_done, operation);
    step_free(operation_step);
}

static void op_run(op *operation) {
    set_busy(operation->m, operation->title);
    op_next(operation);
}

static void op_unmount(op *operation, const volume *volume_info) {
    if (mounted(volume_info))
        op_add(operation, volume_info->fs_path, UD_FS, "Unmount",
               g_variant_new("(@a{sv})", no_options()));
}

static bool confirm(dm *disk_manager, const char *title, const char *text) {
    return xd_message_box(GTK_WINDOW(disk_manager->window), title, XD_MB_WARNING, XD_MB_YES_NO,
                          TRUE, text) == GTK_RESPONSE_YES;
}

/* ---- small dialogs ---------------------------------------------------------------- */

static GtkWidget *dialog_new(dm *disk_manager, const char *title, GtkWidget **content) {
    GtkWidget *widget =
        gtk_dialog_new_with_buttons(title, GTK_WINDOW(disk_manager->window), GTK_DIALOG_MODAL, "OK",
                                    GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(widget), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(*content), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(widget))), *content, TRUE,
                       TRUE, 0);
    return widget;
}

static GtkWidget *wrap_label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 56);
    return widget;
}

static GtkWidget *grid_new(void) {
    GtkWidget *widget = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(widget), 6);
    gtk_grid_set_column_spacing(GTK_GRID(widget), 12);
    return widget;
}

static void grid_row(GtkWidget *grid, int row, const char *key, GtkWidget *value) {
    GtkWidget *key_label = gtk_label_new_with_mnemonic(key);
    gtk_label_set_xalign(GTK_LABEL(key_label), 0);
    if (GTK_IS_LABEL(value) == FALSE)
        gtk_label_set_mnemonic_widget(GTK_LABEL(key_label), value);
    gtk_grid_attach(GTK_GRID(grid), key_label, 0, row, 1, 1);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
}

static GtkWidget *value_label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_selectable(GTK_LABEL(widget), TRUE);
    gtk_widget_set_can_focus(widget, FALSE);
    return widget;
}

/* The filesystems UDisks2 can make here, as a combo (ids are types). */
static GtkWidget *fs_combo(dm *disk_manager, const char *preferred) {
    GtkWidget *widget = gtk_combo_box_text_new();
    for (size_t i = 0; i < G_N_ELEMENTS(formats); i++)
        if (can_format(disk_manager, formats[i].type))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), formats[i].type,
                                      formats[i].label);
    if (preferred == nullptr || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), preferred))
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), "ext4"))
            gtk_combo_box_set_active(GTK_COMBO_BOX(widget), 0);
    return widget;
}

/* XP's format options: the file system, its allocation unit size, a
 * quick or full format, and compression (greyed: none of the file
 * systems made here compresses the way NTFS did). */
typedef struct format_opts {
    dm *m;
    GtkWidget *fs, *unit, *quick, *compress;
    GtkWidget *encrypt, *pass, *confirm, *encrypt_rows; /* Encrypt this volume (LUKS) */
    guint sector_size; /* the disk's logical sector size */
} format_opts;

/* The disk's logical sector size from sysfs (UDisks2 doesn't say), 512
 * when it can't be read. */
static guint sector_size_of(const disk *disk_info) {
    char *path = g_strdup_printf("/sys/class/block/%s/queue/logical_block_size",
                                 dev_name(disk_info->device));
    char *text = nullptr;
    guint size = 512;
    if (g_file_get_contents(path, &text, nullptr, nullptr)) {
        guint64 bytes = g_ascii_strtoull(text, nullptr, 10);
        if (bytes >= 512 && bytes <= 65536)
            size = (guint)bytes;
    }
    g_free(text);
    g_free(path);
    return size;
}

/* UDisks2 2.10 added Format's "mkfs-args", which the allocation unit
 * size goes through. */
static bool udisks_has_mkfs_args(dm *disk_manager) {
    if (disk_manager->version == nullptr && disk_manager->om != nullptr) {
        GDBusInterface *dbus_interface =
            g_dbus_object_manager_get_interface(disk_manager->om, UD_PATH "/Manager", UD_MANAGER);
        if (dbus_interface != nullptr) {
            disk_manager->version = prop_str(G_DBUS_PROXY(dbus_interface), "Version");
            g_object_unref(dbus_interface);
        }
    }
    return xd_udisks_version_at_least(disk_manager->version, 2, 10);
}

/* The allocation unit sizes the chosen file system can be made with:
 * Default only (greyed) when it, mkfs or UDisks2 can't choose. */
static void fill_units(format_opts *format_settings) {
    const char *type = gtk_combo_box_get_active_id(GTK_COMBO_BOX(format_settings->fs));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(format_settings->unit));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_settings->unit), "0", "Default");
    const guint *sizes = xd_format_unit_sizes(type);
    bool supported = sizes != nullptr && udisks_has_mkfs_args(format_settings->m);
    int count = 0;
    for (size_t i = 0; supported && sizes[i] != 0; i++) {
        char **args = xd_format_unit_args(type, sizes[i], format_settings->sector_size);
        if (args == nullptr)
            continue;
        g_strfreev(args);
        char *id = g_strdup_printf("%u", sizes[i]), *label = xd_format_unit_label(sizes[i]);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_settings->unit), id, label);
        g_free(id);
        g_free(label);
        count++;
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(format_settings->unit), "0");
    gtk_widget_set_sensitive(format_settings->unit,
                             count > 0 && gtk_widget_get_sensitive(format_settings->fs));
    gtk_widget_set_tooltip_text(format_settings->unit,
                                count > 0          ? nullptr
                                : sizes == nullptr ? "This file system is made with its default "
                                                     "size only."
                                                   : "This version of UDisks2 can't choose the "
                                                     "size (2.10 and later can).");
}

static void on_fs_changed([[maybe_unused]] GtkComboBox *combo, gpointer data) { fill_units(data); }

/* The File system and Allocation unit size rows, from `row` on. */
static void format_fs_rows(format_opts *format_settings, dm *disk_manager, const disk *disk_info,
                           GtkWidget *grid, int row, const char *preferred) {
    format_settings->m = disk_manager;
    format_settings->sector_size = sector_size_of(disk_info);
    format_settings->fs = fs_combo(disk_manager, preferred);
    grid_row(grid, row, "File s_ystem:", format_settings->fs);
    format_settings->unit = gtk_combo_box_text_new();
    grid_row(grid, row + 1, "_Allocation unit size:", format_settings->unit);
    fill_units(format_settings);
    g_signal_connect(format_settings->fs, "changed", G_CALLBACK(on_fs_changed), format_settings);
}

/* The encryption UDisks2 makes (Manager.DefaultEncryptionType, else
 * the first of SupportedEncryptionTypes: "luks2"), or nullptr when it
 * can't encrypt. */
static const char *encryption_type(dm *disk_manager) {
    if (!disk_manager->luks_read && disk_manager->om != nullptr) {
        disk_manager->luks_read = true;
        GDBusInterface *dbus_interface =
            g_dbus_object_manager_get_interface(disk_manager->om, UD_PATH "/Manager", UD_MANAGER);
        GVariant *types = dbus_interface != nullptr
                              ? prop(G_DBUS_PROXY(dbus_interface), "SupportedEncryptionTypes")
                              : nullptr;
        g_autofree char *def = dbus_interface != nullptr
                                   ? prop_str(G_DBUS_PROXY(dbus_interface), "DefaultEncryptionType")
                                   : nullptr;
        if (types != nullptr && g_variant_is_of_type(types, G_VARIANT_TYPE_STRING_ARRAY)) {
            g_autofree const char **all = g_variant_get_strv(types, nullptr);
            if (def != nullptr && g_strv_contains(all, def))
                disk_manager->luks_type = g_strdup(def);
            else if (all[0] != nullptr)
                disk_manager->luks_type = g_strdup(all[0]);
        }
        if (types != nullptr)
            g_variant_unref(types);
        g_clear_object(&dbus_interface);
    }
    return disk_manager->luks_type;
}

static void on_encrypt_toggled(GtkToggleButton *toggle, gpointer data) {
    format_opts *format_settings = data;
    gtk_widget_set_sensitive(format_settings->encrypt_rows,
                             gtk_toggle_button_get_active(toggle) &&
                                 gtk_widget_get_sensitive(GTK_WIDGET(toggle)));
}

static void format_checks(format_opts *format_settings, GtkWidget *box) {
    format_settings->quick = gtk_check_button_new_with_mnemonic("Perform a _quick format");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(format_settings->quick), TRUE);
    gtk_widget_set_tooltip_text(format_settings->quick,
                                "Unticked, the whole volume is overwritten with zeros "
                                "before it's formatted, which can take a long time.");
    format_settings->compress =
        gtk_check_button_new_with_mnemonic("_Enable file and folder compression");
    gtk_widget_set_sensitive(format_settings->compress, FALSE);
    gtk_box_pack_start(GTK_BOX(box), format_settings->quick, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), format_settings->compress, FALSE, FALSE, 0);
    /* Linux's counterpart of BitLocker, which XP didn't have: the volume
     * is LUKS, with the file system inside, unlocked by the passphrase. */
    format_settings->encrypt = gtk_check_button_new_with_mnemonic("Encrypt this volume (_LUKS)");
    const char *luks = encryption_type(format_settings->m);
    gtk_widget_set_sensitive(format_settings->encrypt, luks != nullptr);
    gtk_widget_set_tooltip_text(format_settings->encrypt,
                                luks != nullptr ? "What's on the volume can only be read with "
                                                  "the passphrase. Don't lose it: nobody can "
                                                  "get the files back without it."
                                                : "UDisks2 can't make encrypted volumes here "
                                                  "(cryptsetup is missing).");
    gtk_box_pack_start(GTK_BOX(box), format_settings->encrypt, FALSE, FALSE, 0);
    format_settings->encrypt_rows = grid_new();
    gtk_widget_set_margin_start(format_settings->encrypt_rows, 24);
    format_settings->pass = gtk_entry_new();
    format_settings->confirm = gtk_entry_new();
    GtkWidget *entries[] = {format_settings->pass, format_settings->confirm};
    for (int i = 0; i < 2; i++) {
        gtk_entry_set_visibility(GTK_ENTRY(entries[i]), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(entries[i]), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_activates_default(GTK_ENTRY(entries[i]), TRUE);
    }
    grid_row(format_settings->encrypt_rows, 0, "Pa_ssphrase:", format_settings->pass);
    grid_row(format_settings->encrypt_rows, 1, "Confi_rm passphrase:", format_settings->confirm);
    gtk_widget_set_sensitive(format_settings->encrypt_rows, FALSE);
    gtk_box_pack_start(GTK_BOX(box), format_settings->encrypt_rows, FALSE, FALSE, 0);
    g_signal_connect(format_settings->encrypt, "toggled", G_CALLBACK(on_encrypt_toggled),
                     format_settings);
}

static bool format_encrypt(const format_opts *format_settings) {
    return gtk_widget_get_sensitive(format_settings->encrypt) &&
           gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(format_settings->encrypt));
}

/* Why the passphrases typed can't encrypt the volume, or nullptr (also
 * when it isn't to be encrypted). */
static const char *format_encrypt_problem(const format_opts *format_settings) {
    return format_encrypt(format_settings)
               ? xd_luks_passphrase_problem(gtk_entry_get_text(GTK_ENTRY(format_settings->pass)),
                                            gtk_entry_get_text(GTK_ENTRY(format_settings->confirm)))
               : nullptr;
}

/* Empties the passphrase entries, so the passphrase isn't left in them. */
static void format_forget_passphrase(const format_opts *format_settings) {
    gtk_entry_set_text(GTK_ENTRY(format_settings->pass), "");
    gtk_entry_set_text(GTK_ENTRY(format_settings->confirm), "");
}

static bool format_quick(const format_opts *format_settings) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(format_settings->quick));
}

static guint format_unit(const format_opts *format_settings) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(format_settings->unit));
    return id != nullptr ? (guint)g_ascii_strtoull(id, nullptr, 10) : 0;
}

/* What a full format adds to its confirmation. */
static const char FULL_FORMAT_NOTE[] =
    "A full format writes zeros over the whole volume first. This can take a long time: hours "
    "on a large disk.";

/* What encrypting adds to a format's confirmation. */
#define ENCRYPT_NOTE                                                                              \
    "It will be encrypted: what's on it can only be read with the passphrase. If the "        \
    "passphrase is lost, so is everything on the volume."

/* UDisks2 Format's options. A full format is "erase": "zero"; an
 * allocation unit size goes to mkfs through "mkfs-args"; encryption is
 * "encrypt.passphrase" and "encrypt.type" (`luks`, nullptr for none). */
static GVariant *format_options(const char *label, const char *type, bool quick, guint unit,
                                guint sector_size, const char *passphrase, const char *luks) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    if (label != nullptr && label[0] != '\0')
        g_variant_builder_add(&builder, "{sv}", "label", g_variant_new_string(label));
    /* Removes the old filesystem's /etc/fstab and crypttab entries. */
    g_variant_builder_add(&builder, "{sv}", "tear-down", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&builder, "{sv}", "update-partition-type", g_variant_new_boolean(TRUE));
    if (!quick)
        g_variant_builder_add(&builder, "{sv}", "erase", g_variant_new_string("zero"));
    char **args = unit != 0 ? xd_format_unit_args(type, unit, sector_size) : nullptr;
    if (args != nullptr)
        g_variant_builder_add(&builder, "{sv}", "mkfs-args",
                              g_variant_new_strv((const char *const *)args, -1));
    g_strfreev(args);
    if (passphrase != nullptr && luks != nullptr) {
        g_variant_builder_add(&builder, "{sv}", "encrypt.passphrase",
                              g_variant_new_string(passphrase));
        g_variant_builder_add(&builder, "{sv}", "encrypt.type", g_variant_new_string(luks));
    }
    return g_variant_builder_end(&builder);
}

/* The options as the dialog has them; `type` is the file system's. */
static GVariant *format_opts_variant(const format_opts *format_settings, const char *type,
                                     const char *label) {
    bool encrypt = format_encrypt(format_settings);
    return format_options(label, type, format_quick(format_settings), format_unit(format_settings),
                          format_settings->sector_size,
                          encrypt ? gtk_entry_get_text(GTK_ENTRY(format_settings->pass)) : nullptr,
                          encrypt ? encryption_type(format_settings->m) : nullptr);
}

/* ---- Initialize Disk -------------------------------------------------------------- */

static bool disk_uninitialized(const disk *disk_info) {
    return disk_info != nullptr && !disk_info->optical && disk_info->media &&
           disk_info->table == nullptr && disk_info->vols->len == 0 &&
           disk_info->block_path != nullptr;
}

static void initialize_disk(dm *disk_manager) {
    disk *disk_info = disk_manager->sel.d;
    if (!disk_uninitialized(disk_info) || disk_manager->busy)
        return;
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(disk_manager, "Initialize Disk", &box);
    char *dn = disk_name(disk_info), *size = xd_disk_size_text(disk_info->size);
    char *text = g_strdup_printf("You must initialize a disk before Disk Management can use "
                                 "it.\n\n%s (%s, %s, %s)",
                                 dn, dev_name(disk_info->device), size, disk_info->model);
    gtk_box_pack_start(GTK_BOX(box), wrap_label(text), FALSE, FALSE, 0);
    g_free(text);
    GtkWidget *frame = gtk_frame_new("Use the following partition style");
    GtkWidget *radios = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(radios), 8);
    GtkWidget *mbr = gtk_radio_button_new_with_mnemonic(nullptr, "_MBR (Master Boot Record)");
    GtkWidget *gpt = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(mbr), "_GPT (GUID Partition Table)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gpt), TRUE);
    gtk_box_pack_start(GTK_BOX(radios), mbr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(radios), gpt, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), radios);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       wrap_label("GPT is recommended for disks larger than 2 TB and for "
                                  "computers that start with UEFI. MBR works with older "
                                  "computers and other systems."),
                       FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);
    bool use_gpt = false;
    int response = gtk_dialog_run(GTK_DIALOG(dlg));
    use_gpt = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gpt));
    gtk_widget_destroy(dlg);
    if (response == GTK_RESPONSE_OK) {
        text = g_strdup_printf("%s (%s, %s) will get a new, empty %s partition table. Anything "
                               "on the disk that Disk Management can't see will be lost.\n\nDo "
                               "you want to continue?",
                               dn, dev_name(disk_info->device), size, use_gpt ? "GPT" : "MBR");
        if (confirm(disk_manager, "Initialize Disk", text)) {
            char *title = g_strdup_printf("Initializing %s...", dn);
            op *operation = op_new(disk_manager, title);
            op_add(operation, disk_info->block_path, UD_BLOCK, "Format",
                   g_variant_new("(s@a{sv})", use_gpt ? "gpt" : "dos", no_options()));
            op_run(operation);
            g_free(title);
        }
        g_free(text);
    }
    g_free(dn);
    g_free(size);
}

/* ---- New Partition ---------------------------------------------------------------- */

typedef struct wizard {
    dm *m;
    GPtrArray *disks; /* keeps d alive across refreshes */
    disk *d;
    guint64 offset, max_size;
    xd_new_part_kinds kinds;
    GtkWidget *assistant, *primary, *extended, *logical, *size, *no_format, *do_format, *label,
        *mount, *summary, *fmt_page, *fmt_hint;
    format_opts fo;
} wizard;

static bool wizard_extended(wizard *partition_wizard) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(partition_wizard->extended));
}

static gint wizard_forward(gint current, gpointer data) {
    wizard *partition_wizard = data;
    /* An extended partition has no filesystem: skip Format. */
    if (current == 2 && wizard_extended(partition_wizard))
        return 4;
    return current + 1;
}

static void wizard_summary(wizard *partition_wizard) {
    GString *string = g_string_new("You have selected the following settings:\n\n");
    const char *kind = wizard_extended(partition_wizard) ? "Extended"
                       : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(partition_wizard->logical))
                           ? "Logical drive"
                           : "Primary";
    char *dn = disk_name(partition_wizard->d);
    g_string_append_printf(string, "Partition type: %s\nSelected disk: %s (%s)\n", kind, dn,
                           dev_name(partition_wizard->d->device));
    g_free(dn);
    guint64 size =
        (guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(partition_wizard->size)) * XD_MIB;
    char *st = xd_disk_size_text(size);
    g_string_append_printf(string, "Partition size: %s\n", st);
    g_free(st);
    if (!wizard_extended(partition_wizard)) {
        bool fmt = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(partition_wizard->do_format));
        char *fs =
            fmt ? gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(partition_wizard->fo.fs))
                : nullptr;
        char *unit =
            fmt ? gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(partition_wizard->fo.unit))
                : nullptr;
        g_string_append_printf(string, "File system: %s\n", fs != nullptr ? fs : "None");
        if (fmt)
            g_string_append_printf(string,
                                   "Allocation unit size: %s\nVolume label: %s\nQuick format: "
                                   "%s\n",
                                   unit != nullptr ? unit : "Default",
                                   gtk_entry_get_text(GTK_ENTRY(partition_wizard->label)),
                                   format_quick(&partition_wizard->fo) ? "Yes" : "No");
        bool encrypt = fmt && format_encrypt(&partition_wizard->fo);
        char *luks = encrypt ? g_ascii_strup(encryption_type(partition_wizard->m), -1) : nullptr;
        if (fmt)
            g_string_append_printf(string, "Encryption: %s\n", encrypt ? luks : "None");
        g_free(luks);
        g_string_append_printf(
            string, "Mount it: %s\n",
            fmt && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(partition_wizard->mount)) ? "Yes"
                                                                                            : "No");
        if (fmt && !format_quick(&partition_wizard->fo))
            g_string_append_printf(string, "\n%s\n", FULL_FORMAT_NOTE);
        if (encrypt)
            g_string_append_printf(string, "\n%s\n", ENCRYPT_NOTE);
        g_free(fs);
        g_free(unit);
    }
    g_string_append(string, "\nTo close this wizard and create the partition, click Apply.");
    gtk_label_set_text(GTK_LABEL(partition_wizard->summary), string->str);
    g_string_free(string, TRUE);
}

static void on_wizard_prepare(GtkAssistant *assistant, GtkWidget *page, gpointer data) {
    wizard *partition_wizard = data;
    if (page == gtk_assistant_get_nth_page(assistant, 4))
        wizard_summary(partition_wizard);
}

/* Next only once an encrypted volume's passphrase is typed twice alike;
 * the hint says why not. */
static void update_format_page(wizard *partition_wizard) {
    const char *why = format_encrypt_problem(&partition_wizard->fo);
    gtk_assistant_set_page_complete(GTK_ASSISTANT(partition_wizard->assistant),
                                    partition_wizard->fmt_page, why == nullptr);
    gtk_label_set_text(GTK_LABEL(partition_wizard->fmt_hint), why != nullptr ? why : "");
}

static void on_format_toggled(GtkToggleButton *toggle, gpointer data) {
    wizard *partition_wizard = data;
    bool on = gtk_toggle_button_get_active(toggle);
    gtk_widget_set_sensitive(partition_wizard->fo.fs, on);
    gtk_widget_set_sensitive(partition_wizard->label, on);
    gtk_widget_set_sensitive(partition_wizard->mount, on);
    gtk_widget_set_sensitive(partition_wizard->fo.quick, on);
    gtk_widget_set_sensitive(partition_wizard->fo.encrypt,
                             on && encryption_type(partition_wizard->m) != nullptr);
    on_encrypt_toggled(GTK_TOGGLE_BUTTON(partition_wizard->fo.encrypt), &partition_wizard->fo);
    /* Back on, only where the file system can choose. */
    fill_units(&partition_wizard->fo);
    update_format_page(partition_wizard);
}

static void on_wizard_apply([[maybe_unused]] GtkAssistant *assistant, gpointer data) {
    wizard *partition_wizard = data;
    dm *disk_manager = partition_wizard->m;
    guint64 size =
        (guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(partition_wizard->size)) * XD_MIB;
    if (size > partition_wizard->max_size)
        size = partition_wizard->max_size;
    bool extended = wizard_extended(partition_wizard);
    bool logical = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(partition_wizard->logical));
    bool fmt =
        !extended && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(partition_wizard->do_format));
    GVariantBuilder ob;
    g_variant_builder_init(&ob, G_VARIANT_TYPE("a{sv}"));
    if (g_strcmp0(partition_wizard->d->table, "dos") == 0)
        g_variant_builder_add(&ob, "{sv}", "partition-type",
                              g_variant_new_string(extended ? "extended"
                                                   : logical ? "logical"
                                                             : "primary"));
    GVariant *opts = g_variant_builder_end(&ob);
    char *dn = disk_name(partition_wizard->d);
    char *title = g_strdup_printf("Creating a partition on %s...", dn);
    op *operation = op_new(disk_manager, title);
    if (fmt) {
        const char *type = gtk_combo_box_get_active_id(GTK_COMBO_BOX(partition_wizard->fo.fs));
        if (type == nullptr)
            type = "ext4";
        op_add(operation, partition_wizard->d->block_path, UD_TABLE, "CreatePartitionAndFormat",
               g_variant_new(
                   "(ttss@a{sv}s@a{sv})", partition_wizard->offset, size, "", "", opts, type,
                   format_opts_variant(&partition_wizard->fo, type,
                                       gtk_entry_get_text(GTK_ENTRY(partition_wizard->label)))));
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(partition_wizard->mount)))
            op_add(operation, nullptr, UD_FS, "Mount", g_variant_new("(@a{sv})", no_options()));
    } else {
        op_add(operation, partition_wizard->d->block_path, UD_TABLE, "CreatePartition",
               g_variant_new("(ttss@a{sv})", partition_wizard->offset, size, "", "", opts));
    }
    g_free(title);
    g_free(dn);
    format_forget_passphrase(&partition_wizard->fo);
    /* GtkAssistant emits "close" after this, which destroys it. */
    op_run(operation);
}

static void on_wizard_close(GtkAssistant *assistant, [[maybe_unused]] gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(assistant));
}

static void on_wizard_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    wizard *partition_wizard = data;
    g_ptr_array_unref(partition_wizard->disks);
    g_free(partition_wizard);
}

static GtkWidget *wizard_page(wizard *partition_wizard, const char *title,
                              GtkAssistantPageType type, GtkWidget *body) {
    /* Each page in a box, its contents at the top. */
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    gtk_box_pack_start(GTK_BOX(page), body, FALSE, FALSE, 0);
    gtk_assistant_append_page(GTK_ASSISTANT(partition_wizard->assistant), page);
    gtk_assistant_set_page_title(GTK_ASSISTANT(partition_wizard->assistant), page, title);
    gtk_assistant_set_page_type(GTK_ASSISTANT(partition_wizard->assistant), page, type);
    gtk_assistant_set_page_complete(GTK_ASSISTANT(partition_wizard->assistant), page, TRUE);
    return page;
}

/* XP's wizards have no list of steps down the side: hide GtkAssistant's
 * (its internal child with the "sidebar" style class). */
static void hide_sidebar(GtkWidget *widget, [[maybe_unused]] gpointer data) {
    if (gtk_style_context_has_class(gtk_widget_get_style_context(widget), "sidebar")) {
        /* GtkAssistant shows it again on each page. */
        g_signal_connect_after(widget, "show", G_CALLBACK(gtk_widget_hide), nullptr);
        gtk_widget_hide(widget);
        return;
    }
    if (GTK_IS_CONTAINER(widget))
        gtk_container_forall(GTK_CONTAINER(widget), hide_sidebar, nullptr);
}

static void new_partition(dm *disk_manager) {
    const xd_disk_segment *disk_segment = sel_segment(disk_manager);
    disk *disk_info = disk_manager->sel.d;
    if (disk_segment == nullptr || disk_segment->kind == XD_SEGMENT_PARTITION ||
        disk_manager->busy || disk_info->table == nullptr)
        return;
    guint n_primary = 0;
    for (guint i = 0; i < disk_info->vols->len; i++)
        n_primary += !((volume *)g_ptr_array_index(disk_info->vols, i))->contained;
    wizard *partition_wizard = g_new0(wizard, 1);
    partition_wizard->m = disk_manager;
    partition_wizard->d = disk_info;
    partition_wizard->kinds = xd_disk_new_part_kinds(disk_info->table, disk_segment->kind,
                                                     n_primary, extended_of(disk_info) != nullptr);
    if (!xd_disk_new_part_range(disk_segment->offset, disk_segment->size, disk_info->size,
                                disk_segment->kind == XD_SEGMENT_FREE, &partition_wizard->offset,
                                &partition_wizard->max_size) ||
        !(partition_wizard->kinds.primary || partition_wizard->kinds.extended ||
          partition_wizard->kinds.logical)) {
        g_free(partition_wizard);
        xd_message_box(GTK_WINDOW(disk_manager->window), "Disk Management", XD_MB_INFO, XD_MB_OK,
                       FALSE,
                       "No new partition can be made here: an MBR disk has room for four "
                       "primary partitions (or three and an extended one), and the space must "
                       "be at least 1 MB.");
        return;
    }
    partition_wizard->disks = g_ptr_array_ref(disk_manager->disks);
    partition_wizard->assistant = gtk_assistant_new();
    gtk_window_set_title(GTK_WINDOW(partition_wizard->assistant), "New Partition Wizard");
    gtk_window_set_transient_for(GTK_WINDOW(partition_wizard->assistant),
                                 GTK_WINDOW(disk_manager->window));
    gtk_window_set_modal(GTK_WINDOW(partition_wizard->assistant), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(partition_wizard->assistant), 520, 380);

    wizard_page(partition_wizard, "Welcome to the New Partition Wizard", GTK_ASSISTANT_PAGE_INTRO,
                wrap_label("This wizard helps you create a partition on a basic disk.\n\nA "
                           "basic disk holds primary partitions and, on an MBR disk, an "
                           "extended partition with logical drives in it. Each partition can "
                           "have its own file system.\n\nTo continue, click Next."));

    GtkWidget *types = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(types), wrap_label("Select the partition you want to create:"),
                       FALSE, FALSE, 0);
    partition_wizard->primary = gtk_radio_button_new_with_mnemonic(nullptr, "_Primary partition");
    partition_wizard->extended = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(partition_wizard->primary), "_Extended partition");
    partition_wizard->logical = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(partition_wizard->primary), "_Logical drive");
    gtk_widget_set_sensitive(partition_wizard->primary, partition_wizard->kinds.primary);
    gtk_widget_set_sensitive(partition_wizard->extended, partition_wizard->kinds.extended);
    gtk_widget_set_sensitive(partition_wizard->logical, partition_wizard->kinds.logical);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(partition_wizard->kinds.primary
                                                       ? partition_wizard->primary
                                                       : partition_wizard->logical),
                                 TRUE);
    gtk_box_pack_start(GTK_BOX(types), partition_wizard->primary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(types), partition_wizard->extended, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(types), partition_wizard->logical, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(types),
                       wrap_label(g_strcmp0(disk_info->table, "dos") == 0
                                      ? "A primary partition is a volume you create using free "
                                        "space on a basic disk. An MBR disk has up to four, or "
                                        "three and an extended partition, which holds any "
                                        "number of logical drives."
                                      : "A GPT disk has only primary partitions, up to 128."),
                       FALSE, FALSE, 8);
    wizard_page(partition_wizard, "Select Partition Type", GTK_ASSISTANT_PAGE_CONTENT, types);

    GtkWidget *sizes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *widget = grid_new();
    guint64 max_mb = partition_wizard->max_size / XD_MIB;
    char *max_text = g_strdup_printf("%" G_GUINT64_FORMAT, max_mb);
    grid_row(widget, 0, "Maximum disk space in MB:", value_label(max_text));
    g_free(max_text);
    grid_row(widget, 1, "Minimum disk space in MB:", value_label("1"));
    partition_wizard->size = gtk_spin_button_new_with_range(1, (double)max_mb, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(partition_wizard->size), (double)max_mb);
    grid_row(widget, 2, "Partition _size in MB:", partition_wizard->size);
    gtk_box_pack_start(GTK_BOX(sizes), wrap_label("Choose a partition size between the "
                                                  "maximum and minimum sizes."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sizes), widget, FALSE, FALSE, 0);
    wizard_page(partition_wizard, "Specify Partition Size", GTK_ASSISTANT_PAGE_CONTENT, sizes);

    GtkWidget *fmt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(fmt), wrap_label("To store data on this partition, you must "
                                                "format it first."),
                       FALSE, FALSE, 0);
    partition_wizard->no_format =
        gtk_radio_button_new_with_mnemonic(nullptr, "_Do not format this partition");
    partition_wizard->do_format = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(partition_wizard->no_format),
        "_Format this partition with the following settings:");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(partition_wizard->do_format), TRUE);
    gtk_box_pack_start(GTK_BOX(fmt), partition_wizard->no_format, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fmt), partition_wizard->do_format, FALSE, FALSE, 0);
    GtkWidget *fg = grid_new();
    gtk_widget_set_margin_start(fg, 24);
    /* XP's order: File system, Allocation unit size, Volume label. */
    format_fs_rows(&partition_wizard->fo, disk_manager, disk_info, fg, 0, "ext4");
    partition_wizard->label = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(partition_wizard->label), "New Volume");
    grid_row(fg, 2, "_Volume label:", partition_wizard->label);
    gtk_box_pack_start(GTK_BOX(fmt), fg, FALSE, FALSE, 0);
    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(opts, 24);
    format_checks(&partition_wizard->fo, opts);
    partition_wizard->mount = gtk_check_button_new_with_mnemonic("_Mount it when it's formatted");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(partition_wizard->mount), TRUE);
    gtk_box_pack_start(GTK_BOX(opts), partition_wizard->mount, FALSE, FALSE, 0);
    partition_wizard->fmt_hint = wrap_label("");
    gtk_box_pack_start(GTK_BOX(opts), partition_wizard->fmt_hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fmt), opts, FALSE, FALSE, 0);
    g_signal_connect(partition_wizard->do_format, "toggled", G_CALLBACK(on_format_toggled),
                     partition_wizard);
    g_signal_connect_swapped(partition_wizard->fo.encrypt, "toggled",
                             G_CALLBACK(update_format_page), partition_wizard);
    g_signal_connect_swapped(partition_wizard->fo.pass, "changed", G_CALLBACK(update_format_page),
                             partition_wizard);
    g_signal_connect_swapped(partition_wizard->fo.confirm, "changed",
                             G_CALLBACK(update_format_page), partition_wizard);
    partition_wizard->fmt_page =
        wizard_page(partition_wizard, "Format Partition", GTK_ASSISTANT_PAGE_CONTENT, fmt);

    partition_wizard->summary = wrap_label("");
    wizard_page(partition_wizard, "Completing the New Partition Wizard", GTK_ASSISTANT_PAGE_CONFIRM,
                partition_wizard->summary);

    gtk_assistant_set_forward_page_func(GTK_ASSISTANT(partition_wizard->assistant), wizard_forward,
                                        partition_wizard, nullptr);
    g_signal_connect(partition_wizard->assistant, "prepare", G_CALLBACK(on_wizard_prepare),
                     partition_wizard);
    g_signal_connect(partition_wizard->assistant, "apply", G_CALLBACK(on_wizard_apply),
                     partition_wizard);
    g_signal_connect(partition_wizard->assistant, "cancel", G_CALLBACK(on_wizard_close),
                     partition_wizard);
    g_signal_connect(partition_wizard->assistant, "close", G_CALLBACK(on_wizard_close),
                     partition_wizard);
    g_signal_connect(partition_wizard->assistant, "destroy", G_CALLBACK(on_wizard_destroy),
                     partition_wizard);
    gtk_container_forall(GTK_CONTAINER(partition_wizard->assistant), hide_sidebar, nullptr);
    gtk_widget_show_all(partition_wizard->assistant);
}

/* ---- Delete, Format ---------------------------------------------------------------- */

static void delete_volume(dm *disk_manager, volume *volume_info) {
    bool unmount = false;
    if (action_reason(disk_manager, volume_info, XD_ACTION_DELETE, &unmount) != nullptr)
        return;
    char *desc = volume_description(volume_info);
    char *text = g_strdup_printf("%s\n\nAll data on the partition will be lost.%s Are you sure "
                                 "you want to delete it?",
                                 desc, unmount ? " It will be unmounted first." : "");
    if (confirm(disk_manager,
                volume_info->container ? "Delete Extended Partition" : "Delete Partition", text)) {
        char *title = g_strdup_printf("Deleting %s...", dev_name(volume_info->device));
        op *operation = op_new(disk_manager, title);
        if (unmount)
            op_unmount(operation, volume_info);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&builder, "{sv}", "tear-down", g_variant_new_boolean(TRUE));
        op_add(operation, volume_info->path, UD_PART, "Delete",
               g_variant_new("(@a{sv})", g_variant_builder_end(&builder)));
        op_run(operation);
        g_free(title);
    }
    g_free(text);
    g_free(desc);
}

static void delete_partition(dm *disk_manager) {
    delete_volume(disk_manager, sel_volume(disk_manager));
}

/* The extended partition has no block of its own in the map: it's
 * deleted from its free space. */
static void delete_extended(dm *disk_manager) {
    delete_volume(disk_manager, extended_of(disk_manager->sel.d));
}

static void format_volume(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    bool unmount = false;
    if (action_reason(disk_manager, volume_info, XD_ACTION_FORMAT, &unmount) != nullptr)
        return;
    char *dn = volume_name(volume_info);
    char *title = g_strdup_printf("Format %s", dn);
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(disk_manager, title, &box);
    g_free(title);
    GtkWidget *widget = grid_new();
    GtkWidget *label = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(label),
                       volume_info->label != nullptr && volume_info->label[0] != '\0'
                           ? volume_info->label
                           : "New Volume");
    gtk_entry_set_activates_default(GTK_ENTRY(label), TRUE);
    grid_row(widget, 0, "_Volume label:", label);
    format_opts fo = {};
    format_fs_rows(&fo, disk_manager, volume_info->d, widget, 1,
                   volume_info->has_fs ? volume_info->id_type : "ext4");
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    format_checks(&fo, box);
    gtk_widget_show_all(dlg);
    int response = 0;
    const char *bad = nullptr;
    while ((response = gtk_dialog_run(GTK_DIALOG(dlg))) == GTK_RESPONSE_OK &&
           (bad = format_encrypt_problem(&fo)) != nullptr) {
        xd_message_box(GTK_WINDOW(dlg), "Format", XD_MB_ERROR, XD_MB_OK, FALSE, bad);
        format_forget_passphrase(&fo);
        gtk_widget_grab_focus(fo.pass);
    }
    char *new_label = g_strdup(gtk_entry_get_text(GTK_ENTRY(label)));
    char *type = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(fo.fs)));
    bool quick = format_quick(&fo);
    bool encrypt = format_encrypt(&fo);
    GVariant *opts = type != nullptr ? g_variant_ref_sink(format_opts_variant(&fo, type, new_label))
                                     : nullptr;
    format_forget_passphrase(&fo);
    gtk_widget_destroy(dlg);
    if (response == GTK_RESPONSE_OK && type != nullptr) {
        char *desc = volume_description(volume_info);
        char *text = g_strdup_printf("%s\n\nFormatting this volume will erase all data on it.%s "
                                     "Back up any data you want to keep before formatting.%s%s%s"
                                     "\n\nDo you want to continue?",
                                     desc, unmount ? " It will be unmounted first." : "",
                                     quick ? "" : "\n\n", quick ? "" : FULL_FORMAT_NOTE,
                                     encrypt ? "\n\n" ENCRYPT_NOTE : "");
        if (confirm(disk_manager, "Format", text)) {
            char *busy = g_strdup_printf(quick ? "Formatting %s..."
                                               : "Formatting %s (a full format: this can take a "
                                                 "long time)...",
                                         dev_name(volume_info->device));
            op *operation = op_new(disk_manager, busy);
            if (unmount)
                op_unmount(operation, volume_info);
            op_add(operation, volume_info->path, UD_BLOCK, "Format",
                   g_variant_new("(s@a{sv})", type, opts));
            op_run(operation);
            g_free(busy);
        }
        g_free(text);
        g_free(desc);
    }
    if (opts != nullptr)
        g_variant_unref(opts);
    g_free(type);
    g_free(new_label);
    g_free(dn);
}

/* ---- Extend, Shrink -------------------------------------------------------------------- */

static guint seg_index_of(const volume *volume_info) {
    for (guint i = 0; i < volume_info->d->segments->len; i++)
        if (seg_volume(volume_info->d, (int)i) == volume_info)
            return i;
    return 0;
}

static void resize_volume(dm *disk_manager, bool shrink) {
    volume *volume_info = sel_volume(disk_manager);
    bool unmount = false;
    if (action_reason(disk_manager, volume_info, shrink ? XD_ACTION_SHRINK : XD_ACTION_EXTEND,
                      &unmount) != nullptr)
        return;
    guint64 used = volume_info->fs_total > 0 ? volume_info->fs_total - volume_info->fs_free : 0;
    guint64 min = shrink ? xd_disk_shrink_min(used, volume_info->size) : volume_info->size;
    guint64 max = shrink ? volume_info->size
                         : xd_disk_grow_max(volume_info->d->segments, seg_index_of(volume_info),
                                            volume_info->d->size);
    guint64 room_mb = (shrink ? volume_info->size - min : max - volume_info->size) / XD_MIB;
    char *dn = volume_name(volume_info);
    char *title = g_strdup_printf("%s %s", shrink ? "Shrink" : "Extend", dn);
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(disk_manager, title, &box);
    g_free(title);
    GtkWidget *widget = grid_new();
    char *size_text = g_strdup_printf("%" G_GUINT64_FORMAT, volume_info->size / XD_MIB);
    grid_row(widget, 0, shrink ? "Total size before shrink in MB:" : "Total volume size in MB:",
             value_label(size_text));
    g_free(size_text);
    size_text = g_strdup_printf("%" G_GUINT64_FORMAT, room_mb);
    grid_row(widget, 1,
             shrink ? "Size of available shrink space in MB:" : "Maximum available space in MB:",
             value_label(size_text));
    g_free(size_text);
    GtkWidget *amount = gtk_spin_button_new_with_range(1, (double)room_mb, 1);
    /* Not knowing how full it is, start at half the room rather than all. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(amount),
                              (double)(shrink && used == 0 ? XD_MAX(room_mb / 2, 1) : room_mb));
    grid_row(widget, 2,
             shrink ? "Enter the amount of space to _shrink in MB:"
                    : "Select the amount of _space in MB:",
             amount);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    if (shrink)
        gtk_box_pack_start(GTK_BOX(box),
                           wrap_label(used > 0 ? "The volume can't be shrunk below what it holds, "
                                                 "and a margin is kept free."
                                               : "How full the volume is isn't known while it's "
                                                 "unmounted; the file system's resize tool "
                                                 "refuses to shrink it below what it holds."),
                           FALSE, FALSE, 0);
    if (unmount)
        gtk_box_pack_start(GTK_BOX(box),
                           wrap_label("This file system can only be resized unmounted: it will "
                                      "be unmounted first."),
                           FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);
    int response = gtk_dialog_run(GTK_DIALOG(dlg));
    guint64 change = (guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(amount)) * XD_MIB;
    gtk_widget_destroy(dlg);
    if (response != GTK_RESPONSE_OK || change == 0) {
        g_free(dn);
        return;
    }
    guint64 target = shrink ? volume_info->size - change : volume_info->size + change;
    if (target < min)
        target = min;
    if (target > max)
        target = max;
    target = shrink ? xd_disk_align_up(target) : xd_disk_align_down(target);
    char *from = xd_disk_size_text(volume_info->size), *to = xd_disk_size_text(target);
    char *desc = volume_description(volume_info);
    char *text = g_strdup_printf("%s\n\n%s it from %s to %s?%s%s", desc,
                                 shrink ? "Shrink" : "Extend", from, to,
                                 shrink ? " If shrinking fails partway, data can be lost: back "
                                          "up first."
                                        : "",
                                 unmount ? " It will be unmounted first." : "");
    if (confirm(disk_manager, shrink ? "Shrink Volume" : "Extend Volume", text)) {
        char *busy = g_strdup_printf("%s %s...", shrink ? "Shrinking" : "Extending",
                                     dev_name(volume_info->device));
        op *operation = op_new(disk_manager, busy);
        if (unmount)
            op_unmount(operation, volume_info);
        /* Shrinking: the filesystem first, then its partition. Growing:
         * the partition first, then the filesystem to fill it. */
        if (shrink) {
            op_add(operation, volume_info->fs_path, UD_FS, "Resize",
                   g_variant_new("(t@a{sv})", target, no_options()));
            op_add(operation, volume_info->path, UD_PART, "Resize",
                   g_variant_new("(t@a{sv})", target, no_options()));
        } else {
            op_add(operation, volume_info->path, UD_PART, "Resize",
                   g_variant_new("(t@a{sv})", target, no_options()));
            op_add(operation, volume_info->fs_path, UD_FS, "Resize",
                   g_variant_new("(t@a{sv})", (guint64)0, no_options()));
        }
        op_run(operation);
        g_free(busy);
    }
    g_free(text);
    g_free(desc);
    g_free(from);
    g_free(to);
    g_free(dn);
}

/* ---- Label, Active, Type, mount points ---------------------------------------------------- */

static void change_label(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    bool unmount = false;
    if (action_reason(disk_manager, volume_info, XD_ACTION_LABEL, &unmount) != nullptr ||
        !volume_info->has_fs)
        return;
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(disk_manager, "Change Volume Label", &box);
    char *desc = volume_description(volume_info);
    gtk_box_pack_start(GTK_BOX(box), wrap_label(desc), FALSE, FALSE, 0);
    g_free(desc);
    GtkWidget *grid = grid_new();
    GtkWidget *label_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(label_entry),
                       volume_info->label != nullptr ? volume_info->label : "");
    gtk_entry_set_activates_default(GTK_ENTRY(label_entry), TRUE);
    grid_row(grid, 0, "_Volume label:", label_entry);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        op *operation = op_new(disk_manager, "Changing the volume label...");
        op_add(
            operation, volume_info->fs_path, UD_FS, "SetLabel",
            g_variant_new("(s@a{sv})", gtk_entry_get_text(GTK_ENTRY(label_entry)), no_options()));
        op_run(operation);
    }
    gtk_widget_destroy(dlg);
}

static void mark_active(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    bool unmount = false;
    if (action_reason(disk_manager, volume_info, XD_ACTION_SET_ACTIVE, &unmount) != nullptr)
        return;
    char *desc = volume_description(volume_info);
    char *text = g_strdup_printf("%s\n\nThe computer starts from the active partition. It "
                                 "won't start from this one unless it holds a boot loader.\n\n"
                                 "Do you want to mark it as active?",
                                 desc);
    if (confirm(disk_manager, "Mark Partition as Active", text)) {
        op *operation = op_new(disk_manager, "Marking the partition as active...");
        /* Only one partition of a disk is active. */
        for (guint i = 0; i < volume_info->d->vols->len; i++) {
            volume *other = g_ptr_array_index(volume_info->d->vols, i);
            if (other != volume_info && (other->flags & DOS_ACTIVE) != 0)
                op_add(operation, other->path, UD_PART, "SetFlags",
                       g_variant_new("(t@a{sv})", other->flags & ~DOS_ACTIVE, no_options()));
        }
        op_add(operation, volume_info->path, UD_PART, "SetFlags",
               g_variant_new("(t@a{sv})", volume_info->flags | DOS_ACTIVE, no_options()));
        op_run(operation);
    }
    g_free(text);
    g_free(desc);
}

/* A type typed in: "0x83" for MBR, a GUID for GPT. */
static bool valid_type(const char *table, const char *id) {
    if (g_strcmp0(table, "dos") == 0)
        return strlen(id) == 4 && id[0] == '0' && (id[1] == 'x' || id[1] == 'X') &&
               g_ascii_isxdigit(id[2]) && g_ascii_isxdigit(id[3]);
    return g_uuid_string_is_valid(id);
}

static void change_type(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    bool unmount = false;
    if (action_reason(disk_manager, volume_info, XD_ACTION_SET_TYPE, &unmount) != nullptr)
        return;
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(disk_manager, "Change Partition Type", &box);
    char *desc = volume_description(volume_info);
    gtk_box_pack_start(GTK_BOX(box), wrap_label(desc), FALSE, FALSE, 0);
    g_free(desc);
    GtkWidget *combo = gtk_combo_box_text_new_with_entry();
    for (size_t i = 0; i < xd_part_type_count; i++) {
        if (strcmp(xd_part_types[i].table, volume_info->d->table) != 0 ||
            g_str_has_prefix(xd_part_types[i].name, "Extended"))
            continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), xd_part_types[i].id,
                                  xd_part_types[i].name);
    }
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), volume_info->ptype))
        gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), volume_info->ptype);
    GtkWidget *widget = grid_new();
    grid_row(widget, 0, "Partition _type:", combo);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       wrap_label(strcmp(volume_info->d->table, "dos") == 0
                                      ? "Choose a type, or type its number (0x83)."
                                      : "Choose a type, or type its GUID."),
                       FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);
    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
        char *typed = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(
            GTK_BIN(combo))))));
        /* Typing over the name of a listed type picks that type. */
        for (size_t i = 0; id == nullptr && i < xd_part_type_count; i++)
            if (strcmp(xd_part_types[i].table, volume_info->d->table) == 0 &&
                g_ascii_strcasecmp(xd_part_types[i].name, typed) == 0)
                id = xd_part_types[i].id;
        char *new_type = g_ascii_strdown(id != nullptr ? id : typed, -1);
        g_free(typed);
        if (!valid_type(volume_info->d->table, new_type)) {
            xd_message_box(GTK_WINDOW(dlg), "Change Partition Type", XD_MB_ERROR, XD_MB_OK, FALSE,
                           "That isn't a partition type.");
            g_free(new_type);
            continue;
        }
        if (g_ascii_strcasecmp(new_type, volume_info->ptype) != 0) {
            op *operation = op_new(disk_manager, "Changing the partition type...");
            op_add(operation, volume_info->path, UD_PART, "SetType",
                   g_variant_new("(s@a{sv})", new_type, no_options()));
            op_run(operation);
        }
        g_free(new_type);
        break;
    }
    gtk_widget_destroy(dlg);
}

static void mount_volume(dm *disk_manager, volume *volume_info, bool mount) {
    if (volume_info == nullptr || !volume_info->has_fs || disk_manager->busy)
        return;
    char *title = g_strdup_printf("%s %s...", mount ? "Mounting" : "Unmounting",
                                  dev_name(volume_info->device));
    op *operation = op_new(disk_manager, title);
    op_add(operation, volume_info->fs_path, UD_FS, mount ? "Mount" : "Unmount",
           g_variant_new("(@a{sv})", no_options()));
    op_run(operation);
    g_free(title);
}

/* ---- encryption (UDisks2's Encrypted interface) ---------------------------------------- */

/* What the passphrase boxes call the volume: its label, else its device
 * ("sde1"). */
static const char *crypt_name(const volume *volume_info) {
    return volume_info->label != nullptr && volume_info->label[0] != '\0'
               ? volume_info->label
               : dev_name(volume_info->device);
}

static bool needed_by_system(const volume *volume_info) {
    for (char **mp = volume_info->mounts; mp != nullptr && *mp != nullptr; mp++)
        if (xd_mount_is_system(*mp))
            return true;
    return false;
}

/* Why Unlock (`unlock`), Lock or Change Passphrase (`change`) can't be
 * done to the volume, or nullptr. */
static const char *crypt_reason(dm *disk_manager, const volume *volume_info, bool unlock,
                                bool change) {
    if (disk_manager->busy)
        return "Disk Management is busy with another operation.";
    if (volume_info == nullptr || !volume_info->encrypted)
        return "This volume isn't encrypted.";
    if (change)
        return nullptr;
    if (unlock)
        return volume_info->unlocked ? "It's unlocked already." : nullptr;
    if (!volume_info->unlocked)
        return "It's locked already.";
    return needed_by_system(volume_info) ? "The running system needs this volume." : nullptr;
}

/* An Unlock or ChangePassphrase on its way; the volume may be gone when
 * it's done, so what's needed is copied. */
typedef struct crypt_req {
    dm *m;
    char *path, *name;
} crypt_req;

static crypt_req *crypt_req_new(dm *disk_manager, const volume *volume_info) {
    crypt_req *crypt_request = g_new0(crypt_req, 1);
    crypt_request->m = disk_manager;
    crypt_request->path = g_strdup(volume_info->path);
    crypt_request->name = g_strdup(crypt_name(volume_info));
    return crypt_request;
}

static void crypt_req_free(crypt_req *crypt_request) {
    g_free(crypt_request->path);
    g_free(crypt_request->name);
    g_free(crypt_request);
}

/* The call's error, or nullptr (then *wrong says whether the passphrase
 * was wrong; a cancelled polkit box is no error). */
static GError *crypt_finish(GObject *src, GAsyncResult *res, bool *wrong, bool *dismissed) {
    GError *err = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (variant != nullptr)
        g_variant_unref(variant);
    *wrong = *dismissed = false;
    if (err != nullptr) {
        char *name = g_dbus_error_get_remote_error(err);
        g_dbus_error_strip_remote_error(err);
        *wrong = xd_luks_wrong_passphrase(name, err->message);
        *dismissed = g_strcmp0(name, UD ".Error.NotAuthorizedDismissed") == 0;
        g_free(name);
    }
    return err;
}

static void ask_unlock(dm *disk_manager, const char *path, const char *name, bool retry);

static void on_unlocked(GObject *src, GAsyncResult *res, gpointer data) {
    crypt_req *crypt_request = data;
    dm *disk_manager = crypt_request->m;
    bool wrong = false, dismissed = false;
    GError *err = crypt_finish(src, res, &wrong, &dismissed);
    set_busy(disk_manager, nullptr);
    if (err != nullptr && wrong) {
        ask_unlock(disk_manager, crypt_request->path, crypt_request->name, true);
    } else if (err != nullptr && !dismissed) {
        char *text =
            g_strdup_printf("%s could not be unlocked.\n\n%s", crypt_request->name, err->message);
        xd_message_box(GTK_WINDOW(disk_manager->window), "Disk Management", XD_MB_ERROR, XD_MB_OK,
                       FALSE, text);
        g_free(text);
    }
    g_clear_error(&err);
    crypt_req_free(crypt_request);
    refresh(disk_manager);
}

/* The passphrase box (again, saying so, after a wrong one), then
 * Unlock. */
static void ask_unlock(dm *disk_manager, const char *path, const char *name, bool retry) {
    char *pass = xd_luks_ask_passphrase(GTK_WINDOW(disk_manager->window), name, retry, nullptr);
    if (pass == nullptr)
        return;
    crypt_req *crypt_request = g_new0(crypt_req, 1);
    crypt_request->m = disk_manager;
    crypt_request->path = g_strdup(path);
    crypt_request->name = g_strdup(name);
    char *busy = g_strdup_printf("Unlocking %s...", name);
    set_busy(disk_manager, busy);
    g_free(busy);
    g_dbus_connection_call(disk_manager->bus, UD, path, UD_ENC, "Unlock",
                           g_variant_new("(sa{sv})", pass, nullptr), G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                           on_unlocked, crypt_request);
    xd_luks_passphrase_free(pass);
}

static void unlock_volume(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    if (crypt_reason(disk_manager, volume_info, true, false) == nullptr)
        ask_unlock(disk_manager, volume_info->path, crypt_name(volume_info), false);
}

/* Lock: what's inside is unmounted first. */
static void lock_volume(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    if (crypt_reason(disk_manager, volume_info, false, false) != nullptr)
        return;
    char *title = g_strdup_printf("Locking %s...", crypt_name(volume_info));
    op *operation = op_new(disk_manager, title);
    op_unmount(operation, volume_info);
    op_add(operation, volume_info->path, UD_ENC, "Lock", g_variant_new("(@a{sv})", no_options()));
    op_run(operation);
    g_free(title);
}

static void on_passphrase_changed(GObject *src, GAsyncResult *res, gpointer data) {
    crypt_req *crypt_request = data;
    dm *disk_manager = crypt_request->m;
    bool wrong = false, dismissed = false;
    GError *err = crypt_finish(src, res, &wrong, &dismissed);
    set_busy(disk_manager, nullptr);
    if (err == nullptr) {
        xd_message_box(GTK_WINDOW(disk_manager->window), "Change Passphrase", XD_MB_INFO, XD_MB_OK,
                       FALSE, "The passphrase has been changed.");
    } else if (!dismissed) {
        char *text = wrong ? g_strdup("The old passphrase you typed is incorrect. The "
                                      "passphrase has not been changed.")
                           : g_strdup_printf("The passphrase could not be changed.\n\n%s",
                                             err->message);
        xd_message_box(GTK_WINDOW(disk_manager->window), "Change Passphrase", XD_MB_ERROR, XD_MB_OK,
                       FALSE, text);
        g_free(text);
    }
    g_clear_error(&err);
    crypt_req_free(crypt_request);
    refresh(disk_manager);
}

static void change_passphrase(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    if (crypt_reason(disk_manager, volume_info, false, true) != nullptr)
        return;
    char *old = nullptr, *new_passphrase = nullptr;
    if (!xd_luks_ask_change(GTK_WINDOW(disk_manager->window), crypt_name(volume_info), &old,
                            &new_passphrase))
        return;
    crypt_req *crypt_request = crypt_req_new(disk_manager, volume_info);
    char *busy = g_strdup_printf("Changing the passphrase of %s...", crypt_request->name);
    set_busy(disk_manager, busy);
    g_free(busy);
    g_dbus_connection_call(disk_manager->bus, UD, crypt_request->path, UD_ENC, "ChangePassphrase",
                           g_variant_new("(ssa{sv})", old, new_passphrase, nullptr), nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                           on_passphrase_changed, crypt_request);
    xd_luks_passphrase_free(old);
    xd_luks_passphrase_free(new_passphrase);
}

/* ---- startup mount points (/etc/fstab, written by UDisks2) ------------------------------ */

/* Whether the running system mounts the volume, now or at startup. */
static bool volume_is_system(const volume *volume_info) {
    for (char **mp = volume_info->mounts; mp != nullptr && *mp != nullptr; mp++)
        if (xd_mount_is_system(*mp))
            return true;
    for (char **fstab_point = volume_info->fstab; fstab_point != nullptr && *fstab_point != nullptr;
         fstab_point++)
        if (xd_mount_is_system(*fstab_point))
            return true;
    return false;
}

/* Why the volume's startup mount points can't be changed, or nullptr. */
static const char *startup_reason(dm *disk_manager, const volume *volume_info) {
    if (disk_manager->busy)
        return "Disk Management is busy with another operation.";
    if (volume_info->encrypted && !volume_info->unlocked)
        return "Unlock the volume first: the file system inside, which is what's mounted, "
               "can only be seen unlocked.";
    if (!volume_info->has_fs)
        return "There's no file system to mount.";
    if (volume_info->d->optical)
        return "CDs and DVDs are mounted when they're inserted.";
    if (volume_is_system(volume_info))
        return "The running system mounts this volume itself: its /etc/fstab entry can't be "
               "changed here.";
    if (volume_info->uuid == nullptr || volume_info->uuid[0] == '\0')
        return "This volume has no UUID to find it by when the computer starts.";
    return nullptr;
}

/* A field of an fstab entry ("(sa{sv})"): a string ("" when missing), or
 * a number. */
static char *item_str(GVariant *item, const char *key) {
    GVariant *details = g_variant_get_child_value(item, 1);
    GVariant *variant = g_variant_lookup_value(details, key, G_VARIANT_TYPE_BYTESTRING);
    char *text = variant != nullptr ? g_variant_dup_bytestring(variant, nullptr) : g_strdup("");
    if (variant != nullptr)
        g_variant_unref(variant);
    g_variant_unref(details);
    return text;
}

static gint32 item_int(GVariant *item, const char *key) {
    GVariant *details = g_variant_get_child_value(item, 1);
    gint32 value = 0;
    g_variant_lookup(details, key, "i", &value);
    g_variant_unref(details);
    return value;
}

static GVariant *fstab_item(const char *fsname, const char *dir, const char *type,
                            const char *opts, gint32 freq, gint32 passno) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "fsname", g_variant_new_bytestring(fsname));
    g_variant_builder_add(&builder, "{sv}", "dir", g_variant_new_bytestring(dir));
    g_variant_builder_add(&builder, "{sv}", "type", g_variant_new_bytestring(type));
    g_variant_builder_add(&builder, "{sv}", "opts", g_variant_new_bytestring(opts));
    g_variant_builder_add(&builder, "{sv}", "freq", g_variant_new_int32(freq));
    g_variant_builder_add(&builder, "{sv}", "passno", g_variant_new_int32(passno));
    return g_variant_new("(s@a{sv})", "fstab", g_variant_builder_end(&builder));
}

/* The file system type for /etc/fstab: blkid's, except that NTFS goes to
 * the kernel's ntfs3 driver unless ntfs-3g (mount.ntfs) is installed. */
static const char *fstab_type(const char *id_type) {
    if (g_strcmp0(id_type, "ntfs") == 0 && !g_file_test("/usr/bin/mount.ntfs", G_FILE_TEST_EXISTS) &&
        !g_file_test("/usr/sbin/mount.ntfs", G_FILE_TEST_EXISTS) &&
        !g_file_test("/sbin/mount.ntfs", G_FILE_TEST_EXISTS))
        return "ntfs3";
    return id_type != nullptr && id_type[0] != '\0' ? id_type : "auto";
}

/* The volume other than `volume_info` mounted at `dir` at startup, or nullptr. */
static volume *startup_owner(dm *disk_manager, const volume *volume_info, const char *dir) {
    for (guint i = 0; i < disk_manager->disks->len; i++) {
        disk *disk_info = g_ptr_array_index(disk_manager->disks, i);
        for (guint k = 0; k < disk_info->vols->len; k++) {
            volume *owner = g_ptr_array_index(disk_info->vols, k);
            if (owner != volume_info && owner->fstab != nullptr &&
                g_strv_contains((const char *const *)owner->fstab, dir))
                return owner;
        }
    }
    return nullptr;
}

/* Why the folder `dir` (cleaned) can't take the volume, or nullptr. */
static char *startup_dir_problem(dm *disk_manager, const volume *volume_info, const char *dir) {
    const char *why = xd_fstab_dir_problem(dir);
    if (why != nullptr)
        return g_strdup(why);
    volume *other = startup_owner(disk_manager, volume_info, dir);
    if (other != nullptr) {
        char *name = volume_name(other);
        char *text = g_strdup_printf("%s is mounted in %s when the computer starts. Choose "
                                     "another folder.",
                                     name, dir);
        g_free(name);
        return text;
    }
    if (g_file_test(dir, G_FILE_TEST_EXISTS) && !g_file_test(dir, G_FILE_TEST_IS_DIR))
        return g_strdup_printf("%s is a file, not a folder.", dir);
    /* As Windows mounts only in an empty folder: what's in it would be
     * hidden while the volume is mounted over it. */
    bool here = volume_info->mounts != nullptr &&
                g_strv_contains((const char *const *)volume_info->mounts, dir);
    GError *error = nullptr;
    GDir *gd = here ? nullptr : g_dir_open(dir, 0, &error);
    /* A folder that's there but can't be read may hold anything: not
     * empty as far as we know. (One that isn't there is made empty.) */
    if (error != nullptr && !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        char *text = g_strdup_printf("%s can't be read (%s), so it can't be checked to be "
                                     "empty. Choose another folder.",
                                     dir, error->message);
        g_error_free(error);
        return text;
    }
    g_clear_error(&error);
    bool empty = gd == nullptr || g_dir_read_name(gd) == nullptr;
    if (gd != nullptr)
        g_dir_close(gd);
    if (!empty)
        return g_strdup_printf("%s isn't empty. Choose an empty folder: what's in it would be "
                               "hidden while the volume is mounted there.",
                               dir);
    return nullptr;
}

static void on_browse_folder(GtkButton *button, gpointer entry) {
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(button));
    GtkFileChooserNative *chooser =
        gtk_file_chooser_native_new("Browse for Empty Folder", GTK_WINDOW(top),
                                    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "OK", "Cancel");
    char *current = xd_fstab_dir_clean(gtk_entry_get_text(GTK_ENTRY(entry)));
    char *parent = g_path_get_dirname(current);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                        g_file_test(parent, G_FILE_TEST_IS_DIR) ? parent : "/");
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (chosen != nullptr)
            gtk_entry_set_text(GTK_ENTRY(entry), chosen);
        g_free(chosen);
    }
    g_object_unref(chooser);
    g_free(parent);
    g_free(current);
}

/* How an encrypted volume is unlocked at startup (its /etc/crypttab
 * entry): asking for the passphrase, the default; or with a key file
 * UDisks2 writes in /etc/luks-keys, which holds the passphrase. */
typedef struct crypt_startup {
    GtkWidget *ask, *keyfile, *pass, *confirm;
    bool use_keyfile;
    char *passphrase; /* for the key file; free with xd_luks_passphrase_free */
} crypt_startup;

static void on_keyfile_toggled(GtkToggleButton *toggle, gpointer data) {
    crypt_startup *startup_unlock = data;
    bool on = gtk_toggle_button_get_active(toggle);
    gtk_widget_set_sensitive(startup_unlock->pass, on);
    gtk_widget_set_sensitive(startup_unlock->confirm, on);
}

/* The "Unlocking at startup" group of the Add box. */
static void crypt_startup_rows(crypt_startup *startup_unlock, const volume *volume_info,
                               GtkWidget *box) {
    GtkWidget *frame = gtk_frame_new("Unlocking at startup");
    GtkWidget *fb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(fb), 8);
    startup_unlock->ask =
        gtk_radio_button_new_with_mnemonic(nullptr, "As_k for the passphrase when the "
                                                    "computer starts");
    startup_unlock->keyfile = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(startup_unlock->ask), "Unlock it with a key _file kept on this computer");
    gtk_box_pack_start(GTK_BOX(fb), startup_unlock->ask, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fb), startup_unlock->keyfile, FALSE, FALSE, 0);
    char *name = xd_crypttab_name(volume_info->luks_uuid);
    char *path = xd_crypttab_key_path(name);
    char *warning = g_strdup_printf(
        "A key file (%s) holds the passphrase itself. Only administrators can read it, but "
        "anyone who can read this computer's system disk can too: unless that disk is "
        "encrypted as well, this volume's encryption then no longer protects it from someone "
        "who takes the computer.",
        path);
    GtkWidget *warning_label = wrap_label(warning);
    gtk_widget_set_margin_start(warning_label, 24);
    gtk_box_pack_start(GTK_BOX(fb), warning_label, FALSE, FALSE, 0);
    g_free(warning);
    g_free(path);
    g_free(name);
    GtkWidget *grid = grid_new();
    gtk_widget_set_margin_start(grid, 24);
    startup_unlock->pass = gtk_entry_new();
    startup_unlock->confirm = gtk_entry_new();
    GtkWidget *entries[] = {startup_unlock->pass, startup_unlock->confirm};
    for (int i = 0; i < 2; i++) {
        gtk_entry_set_visibility(GTK_ENTRY(entries[i]), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(entries[i]), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_activates_default(GTK_ENTRY(entries[i]), TRUE);
        gtk_widget_set_sensitive(entries[i], FALSE);
    }
    grid_row(grid, 0, "Pa_ssphrase:", startup_unlock->pass);
    grid_row(grid, 1, "_Confirm passphrase:", startup_unlock->confirm);
    gtk_box_pack_start(GTK_BOX(fb), grid, FALSE, FALSE, 0);
    g_signal_connect(startup_unlock->keyfile, "toggled", G_CALLBACK(on_keyfile_toggled),
                     startup_unlock);
    gtk_container_add(GTK_CONTAINER(frame), fb);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
}

/* XP's Add Drive Letter or Path, for a folder: where and how the volume
 * is mounted at startup, and with `crypt` (an encrypted volume without
 * an /etc/crypttab entry yet) how it's unlocked. False when cancelled;
 * else *dir and *opts are set (free them), and crypt's choice. */
static bool startup_dialog(dm *disk_manager, const volume *volume_info, const char *title,
                           const char *dir0, const char *opts0, char **dir, char **opts,
                           crypt_startup *crypt) {
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(disk_manager, title, &box);
    char *desc = volume_description(volume_info);
    gtk_box_pack_start(GTK_BOX(box), wrap_label(desc), FALSE, FALSE, 0);
    g_free(desc);
    GtkWidget *where = gtk_label_new_with_mnemonic(
        "Mount this volume in the following _empty folder when the computer starts:");
    gtk_label_set_xalign(GTK_LABEL(where), 0);
    gtk_box_pack_start(GTK_BOX(box), where, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), dir0);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
    gtk_label_set_mnemonic_widget(GTK_LABEL(where), entry);
    GtkWidget *browse = gtk_button_new_with_mnemonic("B_rowse...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_folder), entry);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    GtkWidget *widget = grid_new();
    GtkWidget *options = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(options), opts0);
    gtk_entry_set_activates_default(GTK_ENTRY(options), TRUE);
    grid_row(widget, 0, "Mount _options:", options);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       wrap_label("nofail: the computer still starts when the disk isn't there. "
                                  "x-mount.mkdir: the folder is made if it doesn't exist. "
                                  "uid and gid: whose the files are, on file systems without "
                                  "owners (FAT, exFAT, NTFS)."),
                       FALSE, FALSE, 0);
    if (crypt != nullptr)
        crypt_startup_rows(crypt, volume_info, box);
    gtk_widget_show_all(dlg);
    bool ok = false;
    while (!ok && gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        char *directory = xd_fstab_dir_clean(gtk_entry_get_text(GTK_ENTRY(entry)));
        char *mount_options = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(options))));
        char *why = startup_dir_problem(disk_manager, volume_info, directory);
        if (why == nullptr &&
            (mount_options[0] == '\0' || strpbrk(mount_options, " \t\n") != nullptr))
            why = g_strdup("Type the mount options separated by commas, without spaces (or "
                           "\"defaults\").");
        bool keyfile =
            crypt != nullptr && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(crypt->keyfile));
        if (why == nullptr && keyfile) {
            const char *bad =
                xd_luks_passphrase_problem(gtk_entry_get_text(GTK_ENTRY(crypt->pass)),
                                           gtk_entry_get_text(GTK_ENTRY(crypt->confirm)));
            why = bad != nullptr ? g_strdup(bad) : nullptr;
        }
        if (why != nullptr) {
            xd_message_box(GTK_WINDOW(dlg), title, XD_MB_ERROR, XD_MB_OK, FALSE, why);
            g_free(why);
            g_free(directory);
            g_free(mount_options);
            continue;
        }
        *dir = directory;
        *opts = mount_options;
        if (crypt != nullptr) {
            crypt->use_keyfile = keyfile;
            crypt->passphrase = keyfile ? g_strdup(gtk_entry_get_text(GTK_ENTRY(crypt->pass)))
                                        : nullptr;
        }
        ok = true;
    }
    if (crypt != nullptr) {
        /* Not left in the entries' buffers. */
        gtk_entry_set_text(GTK_ENTRY(crypt->pass), "");
        gtk_entry_set_text(GTK_ENTRY(crypt->confirm), "");
    }
    gtk_widget_destroy(dlg);
    return ok;
}

/* The /etc/crypttab entry that unlocks the volume at startup: named as
 * systemd and GNOME Disks name it, the volume by its LUKS UUID, "nofail"
 * so a missing disk doesn't stop the computer starting. With a key file,
 * UDisks2 writes passphrase-contents to passphrase-path (root only, and
 * only in /etc/luks-keys); without one, both are empty (UDisks2 wants
 * them there) and the passphrase is asked for at startup, as GNOME Disks
 * does it. */
static GVariant *crypttab_item(const volume *volume_info, const crypt_startup *startup_unlock) {
    char *name = xd_crypttab_name(volume_info->luks_uuid);
    char *device = g_strdup_printf("UUID=%s", volume_info->luks_uuid);
    char *key = startup_unlock->use_keyfile ? xd_crypttab_key_path(name) : g_strdup("");
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "name", g_variant_new_bytestring(name));
    g_variant_builder_add(&builder, "{sv}", "device", g_variant_new_bytestring(device));
    g_variant_builder_add(&builder, "{sv}", "passphrase-path", g_variant_new_bytestring(key));
    g_variant_builder_add(
        &builder, "{sv}", "passphrase-contents",
        g_variant_new_bytestring(startup_unlock->use_keyfile ? startup_unlock->passphrase : ""));
    g_variant_builder_add(&builder, "{sv}", "options", g_variant_new_bytestring("nofail"));
    g_free(name);
    g_free(device);
    g_free(key);
    return g_variant_new("(s@a{sv})", "crypttab", g_variant_builder_end(&builder));
}

/* Mounts the volume at its new startup mount point now, as Windows'
 * new drive letter works straight away: UDisks2 mounts a volume in
 * /etc/fstab where the entry says. One mounted elsewhere is moved if the
 * user agrees. */
static void startup_mount_now(dm *disk_manager, op *operation, const volume *volume_info,
                              const char *dir) {
    if (!mounted(volume_info)) {
        op_add(operation, volume_info->fs_path, UD_FS, "Mount",
               g_variant_new("(@a{sv})", no_options()));
        return;
    }
    if (g_strv_contains((const char *const *)volume_info->mounts, dir))
        return;
    char *name = volume_name(volume_info);
    char *text = g_strdup_printf("%s is mounted in %s now. Do you want to move it to %s now?\n\n"
                                 "Programs using files on it might not run correctly. If you "
                                 "choose No, it moves the next time the computer starts.",
                                 name, volume_info->mounts[0], dir);
    if (xd_message_box(GTK_WINDOW(disk_manager->window), "Disk Management", XD_MB_QUESTION,
                       XD_MB_YES_NO, FALSE, text) == GTK_RESPONSE_YES) {
        op_unmount(operation, volume_info);
        op_add(operation, volume_info->fs_path, UD_FS, "Mount",
               g_variant_new("(@a{sv})", no_options()));
    }
    g_free(text);
    g_free(name);
}

static void add_startup(dm *disk_manager, volume *volume_info) {
    if (startup_reason(disk_manager, volume_info) != nullptr)
        return;
    char *suggested = xd_fstab_suggest_dir(volume_info->label, volume_info->device);
    char *defaults =
        xd_fstab_default_options(volume_info->fs_type, (guint)getuid(), (guint)getgid());
    char *dir = nullptr, *opts = nullptr;
    /* An encrypted volume is unlocked first, by its /etc/crypttab entry
     * (one already there is kept). */
    bool needs_crypttab = volume_info->unlocked && volume_info->crypttab_items != nullptr &&
                          volume_info->crypttab_items->len == 0;
    crypt_startup crypt = {};
    if (startup_dialog(disk_manager, volume_info, "Add Startup Mount Point", suggested, defaults,
                       &dir, &opts, needs_crypttab ? &crypt : nullptr)) {
        char *fsname = g_strdup_printf("UUID=%s", volume_info->uuid);
        op *operation = op_new(disk_manager, "Adding the startup mount point...");
        if (needs_crypttab)
            op_add(operation, volume_info->path, UD_BLOCK, "AddConfigurationItem",
                   g_variant_new("(@(sa{sv})@a{sv})", crypttab_item(volume_info, &crypt),
                                 no_options()));
        /* freq 0, passno 0: no dump, and no fsck holding up the start
         * (as GNOME Disks adds them). */
        op_add(operation, volume_info->fs_path, UD_BLOCK, "AddConfigurationItem",
               g_variant_new("(@(sa{sv})@a{sv})",
                             fstab_item(fsname, dir, fstab_type(volume_info->fs_type), opts, 0, 0),
                             no_options()));
        startup_mount_now(disk_manager, operation, volume_info, dir);
        op_run(operation);
        g_free(fsname);
    }
    xd_luks_passphrase_free(crypt.passphrase);
    g_free(dir);
    g_free(opts);
    g_free(suggested);
    g_free(defaults);
}

static void change_startup(dm *disk_manager, volume *volume_info, guint index) {
    if (startup_reason(disk_manager, volume_info) != nullptr ||
        index >= volume_info->fstab_items->len)
        return;
    GVariant *old = g_ptr_array_index(volume_info->fstab_items, index);
    char *dir0 = item_str(old, "dir"), *opts0 = item_str(old, "opts");
    char *dir = nullptr, *opts = nullptr;
    if (startup_dialog(disk_manager, volume_info, "Change Startup Mount Point", dir0, opts0, &dir,
                       &opts, nullptr) &&
        (strcmp(dir, dir0) != 0 || strcmp(opts, opts0) != 0)) {
        /* The rest of the entry stays as it was. */
        char *fsname = item_str(old, "fsname"), *type = item_str(old, "type");
        op *operation = op_new(disk_manager, "Changing the startup mount point...");
        op_add(operation, volume_info->fs_path, UD_BLOCK, "UpdateConfigurationItem",
               g_variant_new("(@(sa{sv})@(sa{sv})@a{sv})", old,
                             fstab_item(fsname, dir, type, opts, item_int(old, "freq"),
                                        item_int(old, "passno")),
                             no_options()));
        startup_mount_now(disk_manager, operation, volume_info, dir);
        op_run(operation);
        g_free(fsname);
        g_free(type);
    }
    g_free(dir);
    g_free(opts);
    g_free(dir0);
    g_free(opts0);
}

static void remove_startup(dm *disk_manager, volume *volume_info, guint index) {
    if (startup_reason(disk_manager, volume_info) != nullptr ||
        index >= volume_info->fstab_items->len)
        return;
    GVariant *old = g_ptr_array_index(volume_info->fstab_items, index);
    char *dir = item_str(old, "dir");
    char *name = volume_name(volume_info);
    /* With its last startup mount point, an encrypted volume needn't be
     * unlocked at startup either. */
    bool crypttab = volume_info->fstab_items->len == 1 && volume_info->crypttab_items != nullptr &&
                    volume_info->crypttab_items->len > 0;
    char *text = g_strdup_printf("%s won't be mounted in %s when the computer starts.%s It "
                                 "stays mounted until you unmount it.\n\nDo you want to remove "
                                 "this startup mount point?",
                                 name, dir,
                                 crypttab ? " Nor will it be unlocked: its /etc/crypttab entry "
                                            "goes too, and its key file if it has one."
                                          : "");
    if (confirm(disk_manager, "Remove Startup Mount Point", text)) {
        op *operation = op_new(disk_manager, "Removing the startup mount point...");
        op_add(operation, volume_info->fs_path, UD_BLOCK, "RemoveConfigurationItem",
               g_variant_new("(@(sa{sv})@a{sv})", old, no_options()));
        for (guint i = 0; crypttab && i < volume_info->crypttab_items->len; i++)
            op_add(operation, volume_info->path, UD_BLOCK, "RemoveConfigurationItem",
                   g_variant_new("(@(sa{sv})@a{sv})",
                                 g_ptr_array_index(volume_info->crypttab_items, i), no_options()));
        op_run(operation);
    }
    g_free(text);
    g_free(name);
    g_free(dir);
}

/* The Change Mount Point dialog's responses beyond Mount and Unmount. */
enum { MP_MOUNT = 1, MP_UNMOUNT, MP_ADD, MP_CHANGE, MP_REMOVE };

static void on_startup_button(GtkButton *button, gpointer dlg) {
    gtk_dialog_response(GTK_DIALOG(dlg),
                        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "response")));
}

static void on_startup_selection(GtkTreeSelection *ts, gpointer buttons) {
    bool any = gtk_tree_selection_count_selected_rows(ts) > 0;
    GtkWidget **widget = buttons;
    bool allowed = g_object_get_data(G_OBJECT(widget[0]), "allowed") != nullptr;
    gtk_widget_set_sensitive(widget[0], any && allowed);
    gtk_widget_set_sensitive(widget[1], any && allowed);
}

static GtkWidget *path_list(GtkListStore *ls, GtkWidget **view) {
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ls));
    g_object_unref(ls);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), gtk_tree_view_column_new_with_attributes(
                                                       "", gtk_cell_renderer_text_new(), "text",
                                                       0, nullptr));
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sc, 380, 64);
    gtk_container_add(GTK_CONTAINER(sc), tv);
    if (view != nullptr)
        *view = tv;
    return sc;
}

/* XP's Change Drive Letter and Paths: where the volume is mounted now,
 * and where it's mounted when the computer starts (/etc/fstab), with
 * Add, Change and Remove for those. */
static void mount_points(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    if (volume_info == nullptr || !volume_info->has_fs || disk_manager->busy)
        return;
    char *dn = volume_name(volume_info);
    char *title = g_strdup_printf("Change Mount Point for %s", dn);
    g_free(dn);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, GTK_WINDOW(disk_manager->window),
                                                 GTK_DIALOG_MODAL, "_Mount", MP_MOUNT, "_Unmount",
                                                 MP_UNMOUNT, "Close", GTK_RESPONSE_CLOSE, nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), wrap_label("This volume is mounted at:"), FALSE, FALSE, 0);
    GtkListStore *ls = gtk_list_store_new(1, G_TYPE_STRING);
    for (char **mp = volume_info->mounts; *mp != nullptr; mp++)
        gtk_list_store_insert_with_values(ls, nullptr, -1, 0, *mp, -1);
    gtk_box_pack_start(GTK_BOX(box), path_list(ls, nullptr), FALSE, FALSE, 0);
    if (!mounted(volume_info))
        gtk_box_pack_start(GTK_BOX(box),
                           wrap_label(volume_info->fstab[0] != nullptr
                                          ? "It isn't mounted. Mount puts it in its startup "
                                            "mount point."
                                          : "It isn't mounted. Mount puts it in /run/media."),
                           FALSE, FALSE, 0);

    /* The startup mount points. */
    gtk_box_pack_start(GTK_BOX(box), wrap_label("Mount this volume when the computer starts at:"),
                       FALSE, FALSE, 6);
    ls = gtk_list_store_new(1, G_TYPE_STRING);
    for (char **fstab_point = volume_info->fstab; *fstab_point != nullptr; fstab_point++)
        gtk_list_store_insert_with_values(ls, nullptr, -1, 0, *fstab_point, -1);
    GtkWidget *startup = nullptr;
    gtk_box_pack_start(GTK_BOX(box), path_list(ls, &startup), FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    static const struct {
        const char *label;
        int response;
    } bs[] = {{"_Add...", MP_ADD}, {"C_hange...", MP_CHANGE}, {"_Remove", MP_REMOVE}};
    GtkWidget *widget[3];
    for (size_t i = 0; i < G_N_ELEMENTS(bs); i++) {
        widget[i] = gtk_button_new_with_mnemonic(bs[i].label);
        g_object_set_data(G_OBJECT(widget[i]), "response", GINT_TO_POINTER(bs[i].response));
        g_signal_connect(widget[i], "clicked", G_CALLBACK(on_startup_button), dlg);
        gtk_box_pack_start(GTK_BOX(buttons), widget[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    /* An encrypted volume's /etc/crypttab entry: how it's unlocked. */
    for (guint i = 0;
         volume_info->crypttab_items != nullptr && i < volume_info->crypttab_items->len; i++) {
        char *key = item_str(g_ptr_array_index(volume_info->crypttab_items, i), "passphrase-path");
        bool asks = key[0] == '\0' || strcmp(key, "none") == 0 || strcmp(key, "-") == 0;
        char *how = asks ? g_strdup("/etc/crypttab unlocks it when the computer starts, asking "
                                    "for its passphrase.")
                         : g_strdup_printf("/etc/crypttab unlocks it when the computer starts, "
                                           "with the key file %s.",
                                           key);
        gtk_box_pack_start(GTK_BOX(box), wrap_label(how), FALSE, FALSE, 0);
        g_free(how);
        g_free(key);
    }
    const char *why = startup_reason(disk_manager, volume_info);
    /* One startup mount point is enough; more can be in /etc/fstab
     * already, and are shown, changed and removed here. */
    bool has_one = volume_info->fstab[0] != nullptr;
    gtk_widget_set_sensitive(widget[0], why == nullptr && !has_one);
    gtk_widget_set_tooltip_text(widget[0], why != nullptr ? why
                                           : has_one ? "It has a startup mount point: change that "
                                                       "one instead."
                                                     : nullptr);
    GtkWidget *change_remove[2] = {widget[1], widget[2]};
    if (why == nullptr)
        g_object_set_data(G_OBJECT(widget[1]), "allowed", GINT_TO_POINTER(1));
    GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(startup));
    gulong selection_handler =
        g_signal_connect(ts, "changed", G_CALLBACK(on_startup_selection), change_remove);
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ls), &first))
        gtk_tree_selection_select_iter(ts, &first);
    on_startup_selection(ts, change_remove);
    if (why != nullptr)
        gtk_box_pack_start(GTK_BOX(box), wrap_label(why), FALSE, FALSE, 0);

    gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), MP_MOUNT, !mounted(volume_info));
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), MP_UNMOUNT,
                                      mounted(volume_info) && !volume_is_system(volume_info));
    gtk_widget_show_all(dlg);
    int response_id = gtk_dialog_run(GTK_DIALOG(dlg));
    /* Which startup mount point Change and Remove are for. */
    guint index = 0;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(ts, &model, &it)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &it);
        index = (guint)gtk_tree_path_get_indices(path)[0];
        gtk_tree_path_free(path);
    }
    g_signal_handler_disconnect(ts, selection_handler);
    gtk_widget_destroy(dlg);
    switch (response_id) {
    case MP_MOUNT:
    case MP_UNMOUNT:
        mount_volume(disk_manager, volume_info, response_id == MP_MOUNT);
        break;
    case MP_ADD:
        add_startup(disk_manager, volume_info);
        break;
    case MP_CHANGE:
        change_startup(disk_manager, volume_info, index);
        break;
    case MP_REMOVE:
        remove_startup(disk_manager, volume_info, index);
        break;
    default:
        break;
    }
}

static void explore(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    if (volume_info == nullptr || !mounted(volume_info))
        return;
    const char *argv[] = {"xd-files", volume_info->mounts[0], nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(void *)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        xd_message_box(GTK_WINDOW(disk_manager->window), "Disk Management", XD_MB_ERROR, XD_MB_OK,
                       FALSE, error->message);
        g_error_free(error);
    }
}

static void eject(dm *disk_manager) {
    disk *disk_info = disk_manager->sel.d;
    if (disk_info == nullptr || !disk_info->ejectable || disk_manager->busy || disk_info->system)
        return;
    char *dn = disk_name(disk_info);
    char *title = g_strdup_printf("Ejecting %s...", dn);
    op *operation = op_new(disk_manager, title);
    for (guint i = 0; i < disk_info->vols->len; i++)
        op_unmount(operation, g_ptr_array_index(disk_info->vols, i));
    op_add(operation, disk_info->drive_path, UD_DRIVE, "Eject",
           g_variant_new("(@a{sv})", no_options()));
    op_run(operation);
    g_free(title);
    g_free(dn);
}

/* ---- Properties ---------------------------------------------------------------------- */

static gboolean draw_pie(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    const volume *volume_info = data;
    double width = gtk_widget_get_allocated_width(widget),
           height = gtk_widget_get_allocated_height(widget);
    double rx = width / 2 - 4, ry = height / 2 - 10, cx = width / 2, cy = height / 2 - 6;
    double used =
        volume_info->fs_total > 0
            ? (double)(volume_info->fs_total - volume_info->fs_free) / (double)volume_info->fs_total
            : 0;
    /* XP's 3D pie: blue used space, magenta free space. */
    for (int layer = 8; layer >= 0; layer--) {
        bool top = layer == 0;
        cairo_save(cairo);
        cairo_translate(cairo, cx, cy + layer);
        cairo_scale(cairo, rx, ry);
        cairo_move_to(cairo, 0, 0);
        cairo_arc(cairo, 0, 0, 1, -XD_PI / 2, -XD_PI / 2 + used * 2 * XD_PI);
        cairo_close_path(cairo);
        cairo_restore(cairo);
        cairo_set_source_rgb(cairo, 0.0, 0.0, top ? 1.0 : 0.6);
        cairo_fill(cairo);
        cairo_save(cairo);
        cairo_translate(cairo, cx, cy + layer);
        cairo_scale(cairo, rx, ry);
        cairo_move_to(cairo, 0, 0);
        cairo_arc(cairo, 0, 0, 1, -XD_PI / 2 + used * 2 * XD_PI, 3 * XD_PI / 2);
        cairo_close_path(cairo);
        cairo_restore(cairo);
        cairo_set_source_rgb(cairo, top ? 1.0 : 0.6, 0.0, top ? 1.0 : 0.6);
        cairo_fill(cairo);
    }
    return FALSE;
}

static GtkWidget *swatch(double red, double green, double blue) {
    GtkWidget *widget = gtk_drawing_area_new();
    gtk_widget_set_size_request(widget, 14, 14);
    double *color = g_new(double, 3);
    color[0] = red;
    color[1] = green;
    color[2] = blue;
    g_object_set_data_full(G_OBJECT(widget), "rgb", color, g_free);
    return widget;
}

static gboolean draw_swatch(GtkWidget *widget, cairo_t *cairo, [[maybe_unused]] gpointer data) {
    const double *color = g_object_get_data(G_OBJECT(widget), "rgb");
    cairo_rectangle(cairo, 0.5, 0.5, gtk_widget_get_allocated_width(widget) - 1,
                    gtk_widget_get_allocated_height(widget) - 1);
    cairo_set_source_rgb(cairo, color[0], color[1], color[2]);
    cairo_fill_preserve(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
    return FALSE;
}

static char *bytes_text(guint64 bytes) {
    char *plain = g_strdup_printf("%" G_GUINT64_FORMAT, bytes);
    GString *string = g_string_new(nullptr);
    size_t len = strlen(plain);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            g_string_append_c(string, ',');
        g_string_append_c(string, plain[i]);
    }
    g_free(plain);
    char *human = xd_disk_size_text(bytes);
    g_string_append_printf(string, " bytes    %s", human);
    g_free(human);
    return g_string_free(string, FALSE);
}

static GtkWidget *hardware_page(const disk *disk_info) {
    GtkWidget *widget = grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(widget), 12);
    int row = 0;
    char *dn = disk_name(disk_info);
    grid_row(widget, row++, "Disk:", value_label(dn));
    g_free(dn);
    grid_row(widget, row++, "Model:", value_label(disk_info->model));
    grid_row(widget, row++, "Serial number:",
             value_label(disk_info->serial[0] != '\0' ? disk_info->serial : "None"));
    grid_row(widget, row++,
             "Connection:", value_label(disk_info->bus[0] != '\0' ? disk_info->bus : "Internal"));
    grid_row(widget, row++,
             "Device:", value_label(disk_info->device != nullptr ? disk_info->device : ""));
    grid_row(widget, row++, "Removable:", value_label(disk_info->removable ? "Yes" : "No"));
    grid_row(widget, row++, "Partition style:",
             value_label(disk_info->table == nullptr            ? "Not initialized"
                         : strcmp(disk_info->table, "dos") == 0 ? "Master Boot Record (MBR)"
                         : strcmp(disk_info->table, "gpt") == 0 ? "GUID Partition Table (GPT)"
                                                                : disk_info->table));
    char *size = disk_info->media ? xd_disk_size_text(disk_info->size) : g_strdup("No media");
    grid_row(widget, row++, "Capacity:", value_label(size));
    g_free(size);
    return widget;
}

static void on_props_response(GtkDialog *dlg, int response, gpointer data) {
    dm *disk_manager = data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(dlg), "label");
    const char *path = g_object_get_data(G_OBJECT(dlg), "fs-path");
    const char *old = g_object_get_data(G_OBJECT(dlg), "old-label");
    if ((response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY) && entry != nullptr &&
        g_strcmp0(gtk_entry_get_text(GTK_ENTRY(entry)), old) != 0 && !disk_manager->busy) {
        op *operation = op_new(disk_manager, "Changing the volume label...");
        op_add(operation, path, UD_FS, "SetLabel",
               g_variant_new("(s@a{sv})", gtk_entry_get_text(GTK_ENTRY(entry)), no_options()));
        op_run(operation);
        g_object_set_data_full(G_OBJECT(dlg), "old-label",
                               g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))), g_free);
    }
    if (response != GTK_RESPONSE_APPLY)
        gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void properties(dm *disk_manager) {
    volume *volume_info = sel_volume(disk_manager);
    disk *disk_info = disk_manager->sel.d;
    if (disk_info == nullptr)
        return;
    char *title = volume_info != nullptr ? volume_name(volume_info) : disk_name(disk_info);
    char *full = g_strdup_printf("%s Properties", title);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        full, GTK_WINDOW(disk_manager->window), GTK_DIALOG_DESTROY_WITH_PARENT, "OK",
        GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    g_free(full);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    if (volume_info != nullptr) {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(page), 12);
        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_pack_start(GTK_BOX(head),
                           gtk_image_new_from_icon_name(
                               disk_info->removable ? "drive-removable-media" : "drive-harddisk",
                               GTK_ICON_SIZE_DND),
                           FALSE, FALSE, 0);
        GtkWidget *label = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(label),
                           volume_info->label != nullptr ? volume_info->label : "");
        bool unused = false;
        gtk_widget_set_sensitive(label, volume_info->has_fs && !disk_manager->busy &&
                                            action_reason(disk_manager, volume_info,
                                                          XD_ACTION_LABEL, &unused) == nullptr);
        gtk_box_pack_start(GTK_BOX(head), label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(dlg), "label", label);
        g_object_set_data_full(G_OBJECT(dlg), "fs-path", g_strdup(volume_info->fs_path), g_free);
        g_object_set_data_full(G_OBJECT(dlg), "old-label", g_strdup(volume_info->label), g_free);
        gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                           FALSE, 0);
        GtkWidget *grid = grid_new();
        int row = 0;
        char *fs = volume_fs(volume_info);
        grid_row(grid, row++,
                 "Type:", value_label(disk_info->removable ? "Removable Disk" : "Local Disk"));
        grid_row(grid, row++, "File system:", value_label(fs[0] != '\0' ? fs : "None"));
        g_free(fs);
        grid_row(grid, row++, "Device:", value_label(volume_info->device));
        if (volume_info->partition) {
            const char *tn = xd_part_type_name(disk_info->table, volume_info->ptype);
            char *pt = tn != nullptr ? g_strdup_printf("%s (%s)", tn, volume_info->ptype)
                                     : g_strdup(volume_info->ptype);
            grid_row(grid, row++, "Partition type:", value_label(pt));
            g_free(pt);
        }
        grid_row(grid, row++, "Mounted at:",
                 value_label(mounted(volume_info) ? volume_info->mounts[0] : "Not mounted"));
        gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                           FALSE, 0);
        GtkWidget *sg = grid_new();
        char *used = volume_info->fs_total > 0
                         ? bytes_text(volume_info->fs_total - volume_info->fs_free)
                         : g_strdup("Unknown");
        char *fr =
            volume_info->fs_total > 0 ? bytes_text(volume_info->fs_free) : g_strdup("Unknown");
        char *cap =
            bytes_text(volume_info->fs_total > 0 ? volume_info->fs_total : volume_info->size);
        const struct {
            const char *key;
            char *value;
            bool swatch;
            rgb c;
        } rows[] = {
            {"Used space:", used, true, {0, 0, 1}},
            {"Free space:", fr, true, {1, 0, 1}},
            {"Capacity:", cap, false, {0, 0, 0}},
        };
        for (int row_index = 0; row_index < 3; row_index++) {
            if (rows[row_index].swatch) {
                GtkWidget *sw =
                    swatch(rows[row_index].c.r, rows[row_index].c.g, rows[row_index].c.b);
                g_signal_connect(sw, "draw", G_CALLBACK(draw_swatch), nullptr);
                gtk_grid_attach(GTK_GRID(sg), sw, 0, row_index, 1, 1);
            }
            GtkWidget *key_label = gtk_label_new(rows[row_index].key);
            gtk_label_set_xalign(GTK_LABEL(key_label), 0);
            gtk_grid_attach(GTK_GRID(sg), key_label, 1, row_index, 1, 1);
            gtk_grid_attach(GTK_GRID(sg), value_label(rows[row_index].value), 2, row_index, 1, 1);
            g_free(rows[row_index].value);
        }
        gtk_box_pack_start(GTK_BOX(page), sg, FALSE, FALSE, 0);
        if (volume_info->fs_total > 0) {
            /* The pie draws `volume_info`, which a refresh would free. */
            g_object_set_data_full(G_OBJECT(dlg), "disks", g_ptr_array_ref(disk_manager->disks),
                                   (GDestroyNotify)g_ptr_array_unref);
            GtkWidget *pie = gtk_drawing_area_new();
            gtk_widget_set_size_request(pie, 160, 90);
            gtk_widget_set_halign(pie, GTK_ALIGN_CENTER);
            g_signal_connect(pie, "draw", G_CALLBACK(draw_pie), volume_info);
            gtk_box_pack_start(GTK_BOX(page), pie, FALSE, FALSE, 0);
        } else {
            gtk_box_pack_start(GTK_BOX(page),
                               wrap_label("Used and free space are shown while it's mounted."),
                               FALSE, FALSE, 0);
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), page, gtk_label_new("General"));
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), hardware_page(disk_info),
                             gtk_label_new(volume_info != nullptr ? "Hardware" : "General"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), nb, TRUE, TRUE, 0);
    g_signal_connect(dlg, "response", G_CALLBACK(on_props_response), disk_manager);
    xd_gtk_notebook_ctrl_tab(dlg, GTK_NOTEBOOK(nb));
    gtk_widget_show_all(dlg);
    g_free(title);
}

/* ---- menus ---------------------------------------------------------------------------- */

typedef void (*action_fn)(dm *disk_manager);

typedef struct action_box {
    action_fn fn;
} action_box;

/* Runs an action. Its dialogs run nested main loops, where a refresh can
 * replace m->disks: the volumes and disks it holds stay alive until it's
 * done. */
static void run_action(dm *disk_manager, action_fn fn) {
    GPtrArray *held = g_ptr_array_ref(disk_manager->disks);
    fn(disk_manager);
    g_ptr_array_unref(held);
}

static void on_action(GtkMenuItem *menu_item, gpointer data) {
    const action_box *boxed_action = g_object_get_data(G_OBJECT(menu_item), "fn");
    if (boxed_action != nullptr && boxed_action->fn != nullptr)
        run_action(data, boxed_action->fn);
}

/* An item, greyed with the reason as its tooltip when it can't be done. */
static GtkWidget *item(GtkWidget *menu, const char *label, action_fn fn, dm *disk_manager,
                       const char *reason) {
    GtkWidget *widget = gtk_menu_item_new_with_mnemonic(label);
    action_box *boxed_action = g_new(action_box, 1);
    boxed_action->fn = fn;
    g_object_set_data_full(G_OBJECT(widget), "fn", boxed_action, g_free);
    g_signal_connect(widget, "activate", G_CALLBACK(on_action), disk_manager);
    if (reason != nullptr) {
        gtk_widget_set_sensitive(widget, FALSE);
        gtk_widget_set_tooltip_text(widget, reason);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), widget);
    return widget;
}

static void separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* Disk Benchmark and Health's Health tab, on the selected disk. */
static void show_health(dm *disk_manager) {
    const disk *disk_info = disk_manager->sel.d;
    if (disk_info == nullptr)
        return;
    const char *argv[] = {"xd-diskbench", "--page", "1", "--disk", disk_info->device, nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(void *)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        xd_message_box(GTK_WINDOW(disk_manager->window), "Disk Management", XD_MB_ERROR,
                       XD_MB_OK, FALSE, error->message);
        g_error_free(error);
    }
}

static void do_explore(dm *disk_manager) { explore(disk_manager); }
static void do_mount(dm *disk_manager) {
    mount_volume(disk_manager, sel_volume(disk_manager), true);
}
static void do_unmount(dm *disk_manager) {
    mount_volume(disk_manager, sel_volume(disk_manager), false);
}
static void do_extend(dm *disk_manager) { resize_volume(disk_manager, false); }
static void do_shrink(dm *disk_manager) { resize_volume(disk_manager, true); }

static const char *volume_reason(dm *disk_manager, xd_disk_action disk_action) {
    bool unmount = false;
    return action_reason(disk_manager, sel_volume(disk_manager), disk_action, &unmount);
}

/* The menu for what's selected: a volume, free space or a disk. */
/* Fills `menu` with what can be done to the selection. */
static void fill_selection_menu(dm *disk_manager, GtkWidget *menu) {
    const xd_disk_segment *disk_segment = sel_segment(disk_manager);
    volume *volume_info = sel_volume(disk_manager);
    disk *disk_info = disk_manager->sel.d;
    const char *busy =
        disk_manager->busy ? "Disk Management is busy with another operation." : nullptr;
    if (disk_info == nullptr) {
        item(menu, "Select a disk or volume", nullptr, disk_manager, "Nothing is selected.");
        return;
    }
    if (disk_segment == nullptr) {
        item(menu, "_Initialize Disk...", initialize_disk, disk_manager,
             busy                                      ? busy
             : disk_uninitialized(disk_info)           ? nullptr
             : disk_info->optical || !disk_info->media ? "There's no disk to initialize."
                                                       : "The disk is already initialized.");
        item(menu, "_Eject", eject, disk_manager,
             busy                    ? busy
             : !disk_info->ejectable ? "This disk can't be ejected."
             : disk_info->system     ? "The running system is on this disk."
                                     : nullptr);
        separator(menu);
        item(menu, "_Health...", show_health, disk_manager,
             disk_info->optical ? "Optical drives have no health data." : nullptr);
        item(menu, "P_roperties", properties, disk_manager, nullptr);
        return;
    }
    if (volume_info == nullptr) {
        item(menu, "_New Partition...", new_partition, disk_manager,
             busy ? busy
             : disk_info->table == nullptr
                 ? "Initialize the disk first: right-click it on the left."
                 : nullptr);
        if (disk_segment->logical) {
            bool unmount = false;
            item(menu, "_Delete Extended Partition...", delete_extended, disk_manager,
                 action_reason(disk_manager, extended_of(disk_info), XD_ACTION_DELETE, &unmount));
        }
        if (disk_info->table == nullptr)
            item(menu, "_Initialize Disk...", initialize_disk, disk_manager,
                 busy                            ? busy
                 : disk_uninitialized(disk_info) ? nullptr
                                                 : "There's no disk to initialize.");
        separator(menu);
        item(menu, "P_roperties", properties, disk_manager, nullptr);
        return;
    }
    if (!volume_info->container) {
        item(menu, "_Open", do_explore, disk_manager,
             mounted(volume_info) ? nullptr : "The volume isn't mounted.");
        item(menu, "E_xplore", do_explore, disk_manager,
             mounted(volume_info) ? nullptr : "The volume isn't mounted.");
        separator(menu);
        item(menu, "_Mount", do_mount, disk_manager,
             busy                   ? busy
             : !volume_info->has_fs ? "There's no file system to mount."
             : mounted(volume_info) ? "It's mounted already."
                                    : nullptr);
        bool system = false;
        for (char **mp = volume_info->mounts; mp != nullptr && *mp != nullptr; mp++)
            system |= xd_mount_is_system(*mp);
        item(menu, "_Unmount", do_unmount, disk_manager,
             busy                    ? busy
             : !mounted(volume_info) ? "It isn't mounted."
             : system                ? "The running system needs this volume."
                                     : nullptr);
        item(menu, "Change Mount _Point...", mount_points, disk_manager,
             busy                  ? busy
             : volume_info->has_fs ? nullptr
             : volume_info->encrypted
                 ? "Unlock the volume first: what's mounted is the file system "
                   "inside."
                 : "There's no file system to mount.");
        if (volume_info->encrypted) {
            separator(menu);
            if (volume_info->unlocked)
                item(menu, "Loc_k", lock_volume, disk_manager,
                     crypt_reason(disk_manager, volume_info, false, false));
            else
                item(menu, "Unloc_k...", unlock_volume, disk_manager,
                     crypt_reason(disk_manager, volume_info, true, false));
            item(menu, "Change Pa_ssphrase...", change_passphrase, disk_manager,
                 crypt_reason(disk_manager, volume_info, false, true));
        }
        item(menu, "Mark Partition as _Active", mark_active, disk_manager,
             volume_reason(disk_manager, XD_ACTION_SET_ACTIVE));
        separator(menu);
        item(menu, "_Format...", format_volume, disk_manager,
             volume_reason(disk_manager, XD_ACTION_FORMAT));
        item(menu, "Change _Label...", change_label, disk_manager,
             volume_reason(disk_manager, XD_ACTION_LABEL));
        item(menu, "Exte_nd Volume...", do_extend, disk_manager,
             volume_reason(disk_manager, XD_ACTION_EXTEND));
        item(menu, "S_hrink Volume...", do_shrink, disk_manager,
             volume_reason(disk_manager, XD_ACTION_SHRINK));
        item(menu, "Change Partition _Type...", change_type, disk_manager,
             volume_reason(disk_manager, XD_ACTION_SET_TYPE));
        separator(menu);
    }
    item(menu, "_Delete Partition...", delete_partition, disk_manager,
         volume_reason(disk_manager, XD_ACTION_DELETE));
    separator(menu);
    item(menu, "P_roperties", properties, disk_manager, nullptr);
}

/* GTK deactivates a menu before running the chosen item, so destroying it
 * on "deactivate" would drop the click; destroy it once that's done. */
static gboolean destroy_menu_idle(gpointer menu) {
    gtk_widget_destroy(menu);
    return G_SOURCE_REMOVE;
}

static void destroy_menu_later(GtkMenuShell *menu, [[maybe_unused]] gpointer data) {
    g_idle_add(destroy_menu_idle, menu);
}

/* The context menu for the selection, at the pointer (or, from the
 * keyboard, over `attach`). */
static void popup_selection_menu(dm *disk_manager, GtkWidget *attach, GdkEvent *ev,
                                 bool at_pointer) {
    GtkWidget *menu = gtk_menu_new();
    fill_selection_menu(disk_manager, menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), attach, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(destroy_menu_later), nullptr);
    gtk_widget_show_all(menu);
    if (at_pointer)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), attach, GDK_GRAVITY_CENTER,
                                 GDK_GRAVITY_NORTH_WEST, ev);
}

/* ---- the volume list ------------------------------------------------------------------ */

static void fill_list(dm *disk_manager) {
    disk_manager->updating = true;
    gtk_list_store_clear(disk_manager->store);
    for (guint i = 0; i < disk_manager->disks->len; i++) {
        disk *disk_info = g_ptr_array_index(disk_manager->disks, i);
        for (guint k = 0; disk_info->media && k < disk_info->vols->len; k++) {
            volume *volume_info = g_ptr_array_index(disk_info->vols, k);
            if (volume_info->container)
                continue;
            char *name = volume_name(volume_info), *fs = volume_fs(volume_info),
                 *status = volume_status(volume_info);
            char *cap = xd_disk_size_text(volume_info->size);
            char *free_text =
                volume_info->fs_total > 0 ? xd_disk_size_text(volume_info->fs_free) : g_strdup("");
            guint pct = volume_info->fs_total > 0
                            ? xd_disk_percent_free(volume_info->fs_free, volume_info->fs_total)
                            : 0;
            char *pct_text =
                volume_info->fs_total > 0 ? g_strdup_printf("%u %%", pct) : g_strdup("");
            gtk_list_store_insert_with_values(
                disk_manager->store, nullptr, -1, COL_ICON,
                disk_info->optical     ? "drive-optical"
                : disk_info->removable ? "drive-removable-media"
                                       : "drive-harddisk",
                COL_VOLUME, name, COL_LAYOUT, volume_info->whole_disk ? "Simple" : "Partition",
                COL_TYPE, "Basic", COL_FS, fs, COL_STATUS, status, COL_CAPACITY, cap, COL_FREE,
                free_text, COL_PCT, pct_text, COL_SIZE, volume_info->size, COL_FREE_SIZE,
                volume_info->fs_free, COL_PCT_NUM, pct, COL_VOL, volume_info, -1);
            g_free(name);
            g_free(fs);
            g_free(status);
            g_free(cap);
            g_free(free_text);
            g_free(pct_text);
        }
    }
    disk_manager->updating = false;
}

static void on_list_selection(GtkTreeSelection *ts, gpointer data) {
    dm *disk_manager = data;
    if (disk_manager->updating)
        return;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(ts, &model, &it))
        return;
    volume *volume_info = nullptr;
    gtk_tree_model_get(model, &it, COL_VOL, &volume_info, -1);
    if (volume_info == nullptr)
        return;
    move_selection(disk_manager, volume_info->d, (int)seg_index_of(volume_info));
    show_selection_status(disk_manager);
}

static gboolean on_list_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    dm *disk_manager = data;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
        explore(disk_manager);
        return FALSE;
    }
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (int)ev->x, (int)ev->y, &path, nullptr,
                                      nullptr, nullptr)) {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), path);
        gtk_tree_path_free(path);
    }
    popup_selection_menu(disk_manager, widget, (GdkEvent *)ev, true);
    return TRUE;
}

/* ---- the disk map ----------------------------------------------------------------------- */

/* XP's legend colours. */
static const rgb C_PRIMARY = {0.0, 0.0, 0.5};
static const rgb C_LOGICAL = {0.25, 0.4, 0.9};
static const rgb C_EXTENDED = {0.0, 0.6, 0.0};
static const rgb C_FREE = {0.55, 0.9, 0.55};
static const rgb C_UNALLOCATED = {0.0, 0.0, 0.0};

static rgb segment_colour(const xd_disk_segment *disk_segment) {
    switch (disk_segment->kind) {
    case XD_SEGMENT_PARTITION:
        return disk_segment->logical ? C_LOGICAL : C_PRIMARY;
    case XD_SEGMENT_FREE:
        return C_FREE;
    case XD_SEGMENT_UNALLOCATED:
        return C_UNALLOCATED;
    }
    return C_UNALLOCATED;
}

/* The x and width of each block of disk `disk_info`'s row, `width` wide. */
static int *block_xs(const disk *disk_info, int width, int **widths_out) {
    guint count = disk_info->segments->len;
    guint64 *sizes = g_new0(guint64, count + 1);
    for (guint i = 0; i < count; i++)
        sizes[i] = g_array_index(disk_info->segments, xd_disk_segment, i).size;
    int *widths = g_new0(int, count + 1);
    int avail = width - HEADER_W - 2 * MARGIN;
    xd_disk_block_widths(sizes, count, avail, MIN_BLOCK, widths);
    int *xs = g_new0(int, count + 1);
    int x = MARGIN + HEADER_W;
    for (guint i = 0; i < count; i++) {
        xs[i] = x;
        x += widths[i];
    }
    g_free(sizes);
    *widths_out = widths;
    return xs;
}

static void text_lines(cairo_t *cairo, GtkWidget *widget, double x, double y, double width,
                       const char *markup) {
    PangoLayout *pl = gtk_widget_create_pango_layout(widget, nullptr);
    pango_layout_set_markup(pl, markup, -1);
    pango_layout_set_width(pl, (int)(width * PANGO_SCALE));
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    cairo_move_to(cairo, x, y);
    pango_cairo_show_layout(cairo, pl);
    g_object_unref(pl);
}

/* XP marks the selection with diagonal hatching. */
static void hatch(cairo_t *cairo, double x, double y, double width, double height) {
    cairo_save(cairo);
    cairo_rectangle(cairo, x, y, width, height);
    cairo_clip(cairo);
    cairo_set_source_rgba(cairo, 0.3, 0.3, 0.3, 0.45);
    cairo_set_line_width(cairo, 1);
    for (double offset = -height; offset < width; offset += 5) {
        cairo_move_to(cairo, x + offset, y + height);
        cairo_line_to(cairo, x + offset + height, y);
    }
    cairo_stroke(cairo);
    cairo_restore(cairo);
}

static char *segment_markup(const xd_disk_segment *disk_segment, const volume *volume_info) {
    char *size = xd_disk_size_text(disk_segment->size);
    char *out;
    if (volume_info == nullptr) {
        out = g_markup_printf_escaped(
            "%s\n%s", size, disk_segment->kind == XD_SEGMENT_FREE ? "Free space" : "Unallocated");
    } else {
        char *name = volume_name(volume_info), *fs = volume_fs(volume_info),
             *status = volume_status(volume_info);
        out = g_markup_printf_escaped("<b>%s</b>\n%s %s\n%s", name, size, fs, status);
        g_free(name);
        g_free(fs);
        g_free(status);
    }
    g_free(size);
    return out;
}

static gboolean draw_map(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    dm *disk_manager = data;
    int width = gtk_widget_get_allocated_width(widget);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_paint(cairo);
    cairo_set_line_width(cairo, 1);
    /* Only the rows in the area being redrawn: a selection change redraws
     * one or two, and scrolling only what comes into view. */
    GdkRectangle clip = {0, 0, width, gtk_widget_get_allocated_height(widget)};
    gdk_cairo_get_clip_rectangle(cairo, &clip);
    for (guint i = 0; i < disk_manager->disks->len; i++) {
        disk *disk_info = g_ptr_array_index(disk_manager->disks, i);
        double y = MARGIN + i * (ROW_H + ROW_GAP);
        if (y + ROW_H < clip.y || y > clip.y + clip.height)
            continue;
        /* The disk's header, on the left. */
        cairo_rectangle(cairo, MARGIN + 0.5, y + 0.5, HEADER_W - 3, ROW_H - 1);
        cairo_set_source_rgb(cairo, 0.93, 0.92, 0.87);
        cairo_fill_preserve(cairo);
        cairo_set_source_rgb(cairo, 0.5, 0.5, 0.5);
        cairo_stroke(cairo);
        if (disk_manager->sel.d == disk_info && disk_manager->sel.seg < 0)
            hatch(cairo, MARGIN + 1, y + 1, HEADER_W - 4, ROW_H - 2);
        char *dn = disk_name(disk_info);
        char *size = disk_info->media ? xd_disk_size_text(disk_info->size) : g_strdup("");
        const char *kind = disk_info->optical     ? "DVD"
                           : disk_info->removable ? "Removable"
                                                  : "Basic";
        const char *state = !disk_info->media               ? "No Media"
                            : disk_uninitialized(disk_info) ? "Not Initialized"
                                                            : "Online";
        char *markup = g_markup_printf_escaped("<b>%s</b>\n%s\n%s\n%s", dn, kind, size, state);
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        GdkPixbuf *icon = gtk_icon_theme_load_icon(theme,
                                                   disk_info->optical     ? "drive-optical"
                                                   : disk_info->removable ? "drive-removable-media"
                                                                          : "drive-harddisk",
                                                   16, GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
        if (icon != nullptr) {
            gdk_cairo_set_source_pixbuf(cairo, icon, MARGIN + 4, y + 4);
            cairo_paint(cairo);
            g_object_unref(icon);
        }
        cairo_set_source_rgb(cairo, 0, 0, 0);
        text_lines(cairo, widget, MARGIN + 24, y + 3, HEADER_W - 28, markup);
        g_free(markup);
        g_free(dn);
        g_free(size);
        if (!disk_info->media)
            continue;
        int *widths = nullptr;
        int *xs = block_xs(disk_info, width, &widths);
        for (guint k = 0; k < disk_info->segments->len; k++) {
            const xd_disk_segment *disk_segment =
                &g_array_index(disk_info->segments, xd_disk_segment, k);
            volume *volume_info = seg_volume(disk_info, (int)k);
            double x = xs[k], bw = widths[k];
            rgb color = segment_colour(disk_segment);
            /* The extended partition: one green frame around its logical
             * drives and free space. */
            bool first =
                k == 0 || !g_array_index(disk_info->segments, xd_disk_segment, k - 1).logical;
            if (disk_segment->logical && first) {
                guint last = k;
                while (last + 1 < disk_info->segments->len &&
                       g_array_index(disk_info->segments, xd_disk_segment, last + 1).logical)
                    last++;
                cairo_rectangle(cairo, x + 1.5, y + 1.5, xs[last] + widths[last] - x - 3,
                                ROW_H - 3);
                cairo_set_source_rgb(cairo, C_EXTENDED.r, C_EXTENDED.g, C_EXTENDED.b);
                cairo_set_line_width(cairo, 3);
                cairo_stroke(cairo);
                cairo_set_line_width(cairo, 1);
            }
            double inset = disk_segment->logical ? 3 : 0;
            double bx = x + inset, by = y + inset, bww = bw - 2 * inset - 2, bh = ROW_H - 2 * inset;
            cairo_rectangle(cairo, bx + 0.5, by + 0.5, bww - 1, bh - 1);
            cairo_set_source_rgb(cairo, 1, 1, 1);
            cairo_fill_preserve(cairo);
            cairo_set_source_rgb(cairo, 0.5, 0.5, 0.5);
            cairo_stroke(cairo);
            cairo_rectangle(cairo, bx + 1, by + 1, bww - 2, BAND);
            cairo_set_source_rgb(cairo, color.r, color.g, color.b);
            cairo_fill(cairo);
            if (disk_manager->sel.d == disk_info && disk_manager->sel.seg == (int)k)
                hatch(cairo, bx + 1, by + BAND + 1, bww - 2, bh - BAND - 2);
            char *text = segment_markup(disk_segment, volume_info);
            cairo_set_source_rgb(cairo, 0, 0, 0);
            cairo_save(cairo);
            cairo_rectangle(cairo, bx + 2, by + BAND + 1, bww - 4, bh - BAND - 2);
            cairo_clip(cairo);
            text_lines(cairo, widget, bx + 4, by + BAND + 3, bww - 8, text);
            cairo_restore(cairo);
            g_free(text);
        }
        g_free(xs);
        g_free(widths);
    }
    return FALSE;
}

/* Which disk and block is at (x, y). */
static bool map_hit(dm *disk_manager, double x, double y, selection *out) {
    int width = gtk_widget_get_allocated_width(disk_manager->map);
    for (guint i = 0; i < disk_manager->disks->len; i++) {
        disk *disk_info = g_ptr_array_index(disk_manager->disks, i);
        double top = MARGIN + i * (ROW_H + ROW_GAP);
        if (y < top || y > top + ROW_H)
            continue;
        if (x >= MARGIN && x < MARGIN + HEADER_W) {
            *out = (selection){.d = disk_info, .seg = -1};
            return true;
        }
        if (!disk_info->media)
            return false;
        int *widths = nullptr;
        int *xs = block_xs(disk_info, width, &widths);
        bool hit = false;
        for (guint k = 0; k < disk_info->segments->len && !hit; k++) {
            if (x >= xs[k] && x < xs[k] + widths[k]) {
                *out = (selection){.d = disk_info, .seg = (int)k};
                hit = true;
            }
        }
        g_free(xs);
        g_free(widths);
        return hit;
    }
    return false;
}

static gboolean on_map_button(GtkWidget *widget, GdkEventButton *ev, gpointer data) {
    dm *disk_manager = data;
    selection hit;
    if (ev->type != GDK_BUTTON_PRESS && ev->type != GDK_2BUTTON_PRESS)
        return FALSE;
    gtk_widget_grab_focus(widget);
    if (!map_hit(disk_manager, ev->x, ev->y, &hit))
        return FALSE;
    set_selection(disk_manager, hit.d, hit.seg);
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
        if (sel_volume(disk_manager) != nullptr && mounted(sel_volume(disk_manager)))
            explore(disk_manager);
        else
            properties(disk_manager);
        return TRUE;
    }
    if (ev->button == GDK_BUTTON_SECONDARY) {
        popup_selection_menu(disk_manager, widget, (GdkEvent *)ev, true);
    }
    return TRUE;
}

static void resize_map(dm *disk_manager) {
    int rows = (int)disk_manager->disks->len;
    gtk_widget_set_size_request(disk_manager->map, 560, 2 * MARGIN + rows * (ROW_H + ROW_GAP));
}

/* ---- refreshing ---------------------------------------------------------------------- */

/* The status bar, and the Action menu, for the selection. */
static void show_selection_status(dm *disk_manager) {
    fill_action_menu(disk_manager);
    if (disk_manager->busy)
        return;
    volume *volume_info = sel_volume(disk_manager);
    const char *text = "";
    char *owned = nullptr;
    if (volume_info != nullptr) {
        bool unmount = false;
        xd_volume_use usage = volume_use(volume_info);
        const char *why = xd_volume_lock_reason(&usage, XD_ACTION_FORMAT, &unmount);
        if (why != nullptr && volume_info->container == false)
            text = why;
    } else if (sel_segment(disk_manager) != nullptr) {
        text = "Right-click the free space and choose New Partition to make a volume.";
    } else if (disk_manager->sel.d != nullptr && disk_uninitialized(disk_manager->sel.d)) {
        owned = disk_name(disk_manager->sel.d);
        char *message =
            g_strdup_printf("%s isn't initialized: right-click it and choose Initialize "
                            "Disk.",
                            owned);
        g_free(owned);
        owned = message;
        text = owned;
    }
    gtk_label_set_text(GTK_LABEL(disk_manager->status), text);
    g_free(owned);
}

/* Keeps the selection across a refresh: the same volume, or the free
 * block at the same place on the same disk. */
static void refresh(dm *disk_manager) {
    char *disk_key =
        disk_manager->sel.d != nullptr ? g_strdup(disk_manager->sel.d->drive_path) : nullptr;
    volume *volume_info = sel_volume(disk_manager);
    char *vol_key = volume_info != nullptr ? g_strdup(volume_info->path) : nullptr;
    const xd_disk_segment *disk_segment = sel_segment(disk_manager);
    guint64 offset = disk_segment != nullptr ? disk_segment->offset : 0;
    int seg = disk_manager->sel.seg;
    GPtrArray *old = disk_manager->disks;
    disk_manager->disks = read_disks(disk_manager);
    ask_capabilities(disk_manager);
    disk_manager->sel = (selection){.d = nullptr, .seg = -1};
    for (guint i = 0; disk_key != nullptr && i < disk_manager->disks->len; i++) {
        disk *disk_info = g_ptr_array_index(disk_manager->disks, i);
        if (strcmp(disk_info->drive_path, disk_key) != 0)
            continue;
        disk_manager->sel = (selection){.d = disk_info, .seg = -1};
        for (guint k = 0; seg >= 0 && k < disk_info->segments->len; k++) {
            volume *nv = seg_volume(disk_info, (int)k);
            const xd_disk_segment *ns = &g_array_index(disk_info->segments, xd_disk_segment, k);
            if ((vol_key != nullptr && nv != nullptr && strcmp(nv->path, vol_key) == 0) ||
                (vol_key == nullptr && nv == nullptr && ns->offset <= offset &&
                 offset < ns->offset + ns->size))
                disk_manager->sel.seg = (int)k;
        }
        if (seg >= 0 && disk_manager->sel.seg < 0)
            disk_manager->sel.d = nullptr;
    }
    fill_list(disk_manager);
    select_volume_row(disk_manager);
    resize_map(disk_manager);
    gtk_widget_queue_draw(disk_manager->map);
    show_selection_status(disk_manager);
    if (old != nullptr)
        g_ptr_array_unref(old);
    g_free(disk_key);
    g_free(vol_key);
}

static gboolean refresh_later(gpointer data) {
    dm *disk_manager = data;
    disk_manager->refresh_source = 0;
    /* An operation refreshes when it's done. */
    if (!disk_manager->busy)
        refresh(disk_manager);
    return G_SOURCE_REMOVE;
}

static void queue_refresh(dm *disk_manager) {
    if (disk_manager->refresh_source == 0)
        disk_manager->refresh_source = g_timeout_add(250, refresh_later, disk_manager);
}

/* Only drives and block devices are shown. Jobs, whose progress changes
 * many times a second while one runs, and RAID arrays' sync progress
 * would otherwise reread everything over and over. */
static void queue_refresh_for(dm *disk_manager, GDBusObject *dbus_object) {
    const char *path = g_dbus_object_get_object_path(dbus_object);
    if (g_str_has_prefix(path, UD_PATH "/drives/") ||
        g_str_has_prefix(path, UD_PATH "/block_devices/"))
        queue_refresh(disk_manager);
}

static void on_om_changed([[maybe_unused]] GDBusObjectManager *om, GDBusObject *dbus_object,
                          gpointer data) {
    queue_refresh_for(data, dbus_object);
}

static void on_om_iface_changed([[maybe_unused]] GDBusObjectManager *om, GDBusObject *dbus_object,
                                [[maybe_unused]] GDBusInterface *dbus_interface, gpointer data) {
    queue_refresh_for(data, dbus_object);
}

static void on_om_props([[maybe_unused]] GDBusObjectManagerClient *om,
                        GDBusObjectProxy *dbus_object_proxy, [[maybe_unused]] GDBusProxy *proxy,
                        [[maybe_unused]] GVariant *changed,
                        [[maybe_unused]] const char *const *invalidated, gpointer data) {
    queue_refresh_for(data, G_DBUS_OBJECT(dbus_object_proxy));
}

/* ---- the window ---------------------------------------------------------------------- */

static void on_refresh([[maybe_unused]] GtkWidget *widget, gpointer user_data) {
    refresh(user_data);
}

static void on_exit([[maybe_unused]] GtkWidget *widget, gpointer data) {
    dm *disk_manager = data;
    gtk_widget_destroy(disk_manager->window);
}

/* The Action menu follows the selection, refilled whenever it (or what's
 * on the disks) changes. GTK pops a submenu up before its item's
 * "activate" handlers run, so filling it as it opened showed it empty, or
 * as it was for the last selection. Replacing the submenu instead left
 * GTK grabbing input for a menu that never showed. */
static void fill_action_menu(dm *disk_manager) {
    GtkWidget *menu = disk_manager->action_menu;
    if (menu == nullptr)
        return;
    if (gtk_widget_get_mapped(menu)) {
        /* Open: once it has closed, and the chosen item has run. */
        disk_manager->action_stale = true;
        return;
    }
    disk_manager->action_stale = false;
    GList *old = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *list = old; list != nullptr; list = list->next)
        gtk_widget_destroy(list->data);
    g_list_free(old);
    fill_selection_menu(disk_manager, menu);
    separator(menu);
    GtkWidget *widget = gtk_menu_item_new_with_mnemonic("Re_fresh");
    g_signal_connect(widget, "activate", G_CALLBACK(on_refresh), disk_manager);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), widget);
    gtk_widget_show_all(menu);
}

static gboolean refill_action_menu(gpointer data) {
    dm *disk_manager = data;
    if (disk_manager->action_stale)
        fill_action_menu(disk_manager);
    return G_SOURCE_REMOVE;
}

static void on_action_menu_closed([[maybe_unused]] GtkMenuShell *menu, gpointer data) {
    dm *disk_manager = data;
    if (disk_manager->action_stale)
        g_idle_add(refill_action_menu, disk_manager);
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data) {
    dm *disk_manager = data;
    if (ev->keyval == GDK_KEY_F5) {
        on_refresh(widget, disk_manager);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Delete && sel_volume(disk_manager) != nullptr) {
        run_action(disk_manager, delete_partition);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Menu || (ev->keyval == GDK_KEY_F10 && (ev->state & GDK_SHIFT_MASK))) {
        popup_selection_menu(disk_manager, disk_manager->map, (GdkEvent *)ev, false);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *legend_item(rgb color, const char *text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *sw = swatch(color.r, color.g, color.b);
    g_signal_connect(sw, "draw", G_CALLBACK(draw_swatch), nullptr);
    gtk_box_pack_start(GTK_BOX(box), sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(text), FALSE, FALSE, 0);
    return box;
}

static GtkWidget *volume_list(dm *disk_manager) {
    disk_manager->store = gtk_list_store_new(
        NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_UINT64,
        G_TYPE_UINT, G_TYPE_POINTER);
    disk_manager->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(disk_manager->store));
    g_object_unref(disk_manager->store);
    static const struct {
        const char *title;
        int col, sort, width;
        float xalign;
    } cols[] = {
        {"Volume", COL_VOLUME, COL_VOLUME, 210, 0},  {"Layout", COL_LAYOUT, COL_LAYOUT, 70, 0},
        {"Type", COL_TYPE, COL_TYPE, 50, 0},         {"File System", COL_FS, COL_FS, 80, 0},
        {"Status", COL_STATUS, COL_STATUS, 170, 0},  {"Capacity", COL_CAPACITY, COL_SIZE, 75, 1},
        {"Free Space", COL_FREE, COL_FREE_SIZE, 75, 1}, {"% Free", COL_PCT, COL_PCT_NUM, 55, 1},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cols); i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, cols[i].title);
        if (cols[i].col == COL_VOLUME) {
            GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
            gtk_tree_view_column_pack_start(col, icon, FALSE);
            gtk_tree_view_column_add_attribute(col, icon, "icon-name", COL_ICON);
        }
        GtkCellRenderer *text = gtk_cell_renderer_text_new();
        g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", (double)cols[i].xalign,
                     nullptr);
        gtk_tree_view_column_pack_start(col, text, TRUE);
        gtk_tree_view_column_add_attribute(col, text, "text", cols[i].col);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, cols[i].width);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_sort_column_id(col, cols[i].sort);
        gtk_tree_view_append_column(GTK_TREE_VIEW(disk_manager->list), col);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(disk_manager->list)), "changed",
                     G_CALLBACK(on_list_selection), disk_manager);
    g_signal_connect(disk_manager->list, "button-press-event", G_CALLBACK(on_list_button),
                     disk_manager);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), disk_manager->list);
    return sc;
}

static void build(dm *disk_manager) {
    disk_manager->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(disk_manager->window), "Disk Management");
    gtk_window_set_icon_name(GTK_WINDOW(disk_manager->window), "drive-harddisk");
    gtk_window_set_default_size(GTK_WINDOW(disk_manager->window), 900, 620);
    g_signal_connect(disk_manager->window, "key-press-event", G_CALLBACK(on_key), disk_manager);
    g_signal_connect(disk_manager->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(disk_manager->window), vbox);

    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget *fm = gtk_menu_new();
    GtkWidget *exit_item = gtk_menu_item_new_with_mnemonic("E_xit");
    g_signal_connect(exit_item, "activate", G_CALLBACK(on_exit), disk_manager);
    gtk_menu_shell_append(GTK_MENU_SHELL(fm), exit_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fm);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file);
    GtkWidget *action = gtk_menu_item_new_with_mnemonic("_Action");
    disk_manager->action_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(action), disk_manager->action_menu);
    g_signal_connect(disk_manager->action_menu, "deactivate", G_CALLBACK(on_action_menu_closed),
                     disk_manager);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), action);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(paned), volume_list(disk_manager), TRUE, FALSE);
    disk_manager->map = gtk_drawing_area_new();
    gtk_widget_set_can_focus(disk_manager->map, TRUE);
    gtk_widget_add_events(disk_manager->map, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(disk_manager->map, "draw", G_CALLBACK(draw_map), disk_manager);
    g_signal_connect(disk_manager->map, "button-press-event", G_CALLBACK(on_map_button),
                     disk_manager);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sc), disk_manager->map);
    gtk_paned_pack2(GTK_PANED(paned), sc, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 180);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    GtkWidget *legend = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(legend), 4);
    gtk_box_pack_start(GTK_BOX(legend), legend_item(C_UNALLOCATED, "Unallocated"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(legend), legend_item(C_PRIMARY, "Primary partition"), FALSE, FALSE,
                       0);
    gtk_box_pack_start(GTK_BOX(legend), legend_item(C_EXTENDED, "Extended partition"), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(legend), legend_item(C_FREE, "Free space"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(legend), legend_item(C_LOGICAL, "Logical drive"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), legend, FALSE, FALSE, 0);
    disk_manager->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(disk_manager->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(disk_manager->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(disk_manager->status, 6);
    gtk_widget_set_margin_bottom(disk_manager->status, 2);
    gtk_box_pack_start(GTK_BOX(vbox), disk_manager->status, FALSE, FALSE, 0);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.DiskManagement");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static dm disk_manager;
    disk_manager.sel.seg = -1;
    disk_manager.can_format = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    disk_manager.resize = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    disk_manager.asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    build(&disk_manager);
    GError *error = nullptr;
    disk_manager.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (disk_manager.bus != nullptr)
        disk_manager.om = g_dbus_object_manager_client_new_sync(
            disk_manager.bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, UD, UD_PATH, nullptr,
            nullptr, nullptr, nullptr, &error);
    if (disk_manager.om == nullptr ||
        g_dbus_object_manager_client_get_name_owner(
            G_DBUS_OBJECT_MANAGER_CLIENT(disk_manager.om)) == nullptr) {
        char *text = g_strdup_printf("UDisks2 isn't running, so the disks can't be read.%s%s",
                                     error != nullptr ? "\n\n" : "",
                                     error != nullptr ? error->message : "");
        g_clear_error(&error);
        gtk_widget_show_all(disk_manager.window);
        xd_message_box(GTK_WINDOW(disk_manager.window), "Disk Management", XD_MB_ERROR, XD_MB_OK,
                       FALSE, text);
        g_free(text);
    } else {
        g_signal_connect(disk_manager.om, "object-added", G_CALLBACK(on_om_changed), &disk_manager);
        g_signal_connect(disk_manager.om, "object-removed", G_CALLBACK(on_om_changed),
                         &disk_manager);
        g_signal_connect(disk_manager.om, "interface-added", G_CALLBACK(on_om_iface_changed),
                         &disk_manager);
        g_signal_connect(disk_manager.om, "interface-removed", G_CALLBACK(on_om_iface_changed),
                         &disk_manager);
        g_signal_connect(disk_manager.om, "interface-proxy-properties-changed",
                         G_CALLBACK(on_om_props), &disk_manager);
    }
    refresh(&disk_manager);
    gtk_widget_show_all(disk_manager.window);
    gtk_main();
    return EXIT_SUCCESS;
}
