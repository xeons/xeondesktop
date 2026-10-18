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

static GDBusProxy *iface(GDBusObject *o, const char *name) {
    return o != nullptr ? G_DBUS_PROXY(g_dbus_object_get_interface(o, name)) : nullptr;
}

static GVariant *prop(GDBusProxy *p, const char *name) {
    return p != nullptr ? g_dbus_proxy_get_cached_property(p, name) : nullptr;
}

static char *prop_str(GDBusProxy *p, const char *name) {
    GVariant *v = prop(p, name);
    char *s = nullptr;
    if (v != nullptr && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
                         g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
        s = g_variant_dup_string(v, nullptr);
    else if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING))
        s = g_variant_dup_bytestring(v, nullptr);
    if (v != nullptr)
        g_variant_unref(v);
    return s != nullptr ? s : g_strdup("");
}

static guint64 prop_u64(GDBusProxy *p, const char *name) {
    GVariant *v = prop(p, name);
    guint64 n = 0;
    if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64))
        n = g_variant_get_uint64(v);
    else if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
        n = g_variant_get_uint32(v);
    if (v != nullptr)
        g_variant_unref(v);
    return n;
}

static bool prop_bool(GDBusProxy *p, const char *name) {
    GVariant *v = prop(p, name);
    bool b = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
             g_variant_get_boolean(v);
    if (v != nullptr)
        g_variant_unref(v);
    return b;
}

/* MountPoints (aay) as strings. */
static char **prop_bytestrings(GDBusProxy *p, const char *name) {
    GVariant *v = prop(p, name);
    char **out = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING_ARRAY)
                     ? g_variant_dup_bytestring_array(v, nullptr)
                     : nullptr;
    if (v != nullptr)
        g_variant_unref(v);
    return out != nullptr ? out : g_new0(char *, 1);
}

/* The fstab mount points in Block.Configuration, and (into `items`) the
 * entries themselves, as Update- and RemoveConfigurationItem want them
 * back. */
static char **fstab_dirs(GDBusProxy *block, GPtrArray *items) {
    GPtrArray *dirs = g_ptr_array_new();
    GVariant *v = prop(block, "Configuration");
    if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE("a(sa{sv})"))) {
        GVariantIter it;
        GVariant *entry = nullptr;
        g_variant_iter_init(&it, v);
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
    if (v != nullptr)
        g_variant_unref(v);
    g_ptr_array_add(dirs, nullptr);
    return (char **)g_ptr_array_free(dirs, FALSE);
}

/* An encrypted volume's own block (not its cleartext's): its LUKS UUID
 * and /etc/crypttab entries. */
static void read_crypt(volume *v, GDBusObject *o) {
    GDBusProxy *enc = iface(o, UD_ENC);
    GDBusProxy *b = iface(o, UD_BLOCK);
    v->encrypted = enc != nullptr;
    g_free(v->luks_uuid);
    v->luks_uuid = prop_str(b, "IdUUID");
    if (v->crypttab_items != nullptr)
        g_ptr_array_unref(v->crypttab_items);
    v->crypttab_items = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
    GVariant *c = prop(b, "Configuration");
    if (c != nullptr && g_variant_is_of_type(c, G_VARIANT_TYPE("a(sa{sv})"))) {
        GVariantIter it;
        GVariant *entry = nullptr;
        g_variant_iter_init(&it, c);
        while ((entry = g_variant_iter_next_value(&it)) != nullptr) {
            const char *kind = nullptr;
            g_variant_get(entry, "(&s@a{sv})", &kind, nullptr);
            if (strcmp(kind, "crypttab") == 0)
                g_ptr_array_add(v->crypttab_items, g_variant_ref(entry));
            g_variant_unref(entry);
        }
    }
    if (c != nullptr)
        g_variant_unref(c);
    g_clear_object(&enc);
    g_clear_object(&b);
}

static void volume_free(gpointer data) {
    volume *v = data;
    g_free(v->path);
    g_free(v->fs_path);
    g_free(v->device);
    g_free(v->label);
    g_free(v->id_type);
    g_free(v->id_version);
    g_free(v->id_usage);
    g_strfreev(v->mounts);
    g_strfreev(v->fstab);
    if (v->fstab_items != nullptr)
        g_ptr_array_unref(v->fstab_items);
    g_free(v->uuid);
    g_free(v->luks_uuid);
    g_free(v->fs_type);
    if (v->crypttab_items != nullptr)
        g_ptr_array_unref(v->crypttab_items);
    g_free(v->ptype);
    g_free(v);
}

static void disk_free(gpointer data) {
    disk *d = data;
    g_free(d->drive_path);
    g_free(d->block_path);
    g_free(d->device);
    g_free(d->model);
    g_free(d->serial);
    g_free(d->bus);
    g_free(d->table);
    g_ptr_array_unref(d->vols);
    if (d->segments != nullptr)
        g_array_unref(d->segments);
    g_free(d);
}

static bool mounted(const volume *v) { return v->mounts != nullptr && v->mounts[0] != nullptr; }

/* The block's filesystem details, from `fs` (itself or its cleartext). */
static void read_fs(volume *v, GDBusObject *fs_obj) {
    GDBusProxy *b = iface(fs_obj, UD_BLOCK);
    GDBusProxy *f = iface(fs_obj, UD_FS);
    GDBusProxy *s = iface(fs_obj, UD_SWAP);
    g_free(v->label);
    g_free(v->id_version);
    g_free(v->id_usage);
    v->label = prop_str(b, "IdLabel");
    v->id_version = prop_str(b, "IdVersion");
    v->id_usage = prop_str(b, "IdUsage");
    g_free(v->fs_type);
    v->fs_type = prop_str(b, "IdType");
    if (!v->unlocked) {
        g_free(v->id_type);
        v->id_type = prop_str(b, "IdType");
    }
    g_strfreev(v->mounts);
    v->mounts = f != nullptr ? prop_bytestrings(f, "MountPoints") : g_new0(char *, 1);
    g_strfreev(v->fstab);
    if (v->fstab_items != nullptr)
        g_ptr_array_unref(v->fstab_items);
    v->fstab_items = g_ptr_array_new_with_free_func((GDestroyNotify)g_variant_unref);
    v->fstab = fstab_dirs(b, v->fstab_items);
    g_free(v->uuid);
    v->uuid = prop_str(b, "IdUUID");
    v->has_fs = f != nullptr;
    v->swap_active = s != nullptr && prop_bool(s, "Active");
    g_free(v->fs_path);
    v->fs_path = g_strdup(g_dbus_object_get_object_path(fs_obj));
    struct statvfs st;
    if (mounted(v) && statvfs(v->mounts[0], &st) == 0) {
        v->fs_total = (guint64)st.f_blocks * st.f_frsize;
        v->fs_free = (guint64)st.f_bavail * st.f_frsize;
    }
    g_clear_object(&b);
    g_clear_object(&f);
    g_clear_object(&s);
}

static int compare_disks(gconstpointer a, gconstpointer b) {
    const disk *x = a, *y = b; /* g_ptr_array_sort_values passes the elements */
    if (x->optical != y->optical)
        return x->optical ? 1 : -1;
    return x->number - y->number;
}

static int compare_vols(gconstpointer a, gconstpointer b) {
    const volume *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset ? 1 : 0;
}

static int compare_keys(gconstpointer a, gconstpointer b, gpointer keys) {
    return g_strcmp0(g_hash_table_lookup(keys, *(disk *const *)a),
                     g_hash_table_lookup(keys, *(disk *const *)b));
}

static void layout_disk(disk *d) {
    xd_disk_part *parts = g_new0(xd_disk_part, d->vols->len + 1);
    for (guint i = 0; i < d->vols->len; i++) {
        volume *v = g_ptr_array_index(d->vols, i);
        parts[i] = (xd_disk_part){
            .offset = v->offset, .size = v->size, .index = (int)i, .container = v->container};
    }
    if (d->segments != nullptr)
        g_array_unref(d->segments);
    d->segments = xd_disk_segments(d->size, parts, d->vols->len);
    g_free(parts);
}

/* Everything from UDisks2's objects. */
static GPtrArray *read_disks(dm *m) {
    GPtrArray *disks = g_ptr_array_new_with_free_func(disk_free);
    if (m->om == nullptr)
        return disks;
    GList *objects = g_dbus_object_manager_get_objects(m->om);
    GHashTable *by_block = g_hash_table_new(g_str_hash, g_str_equal); /* block path -> disk */
    GHashTable *keys = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, g_free);
    /* Drives, and the block of each (the one that isn't a partition). */
    for (GList *l = objects; l != nullptr; l = l->next) {
        GDBusProxy *drive = iface(l->data, UD_DRIVE);
        if (drive == nullptr)
            continue;
        disk *d = g_new0(disk, 1);
        d->drive_path = g_strdup(g_dbus_object_get_object_path(l->data));
        d->model = prop_str(drive, "Model");
        char *vendor = prop_str(drive, "Vendor");
        if (vendor[0] != '\0') {
            char *full = g_strdup_printf("%s %s", vendor, d->model);
            g_free(d->model);
            d->model = full;
        }
        g_free(vendor);
        d->serial = prop_str(drive, "Serial");
        d->bus = prop_str(drive, "ConnectionBus");
        d->size = prop_u64(drive, "Size");
        d->removable = prop_bool(drive, "Removable") || prop_bool(drive, "MediaRemovable");
        d->ejectable = prop_bool(drive, "Ejectable");
        d->optical = prop_bool(drive, "Optical") ||
                     g_str_has_prefix(d->model, "DVD") || g_str_has_prefix(d->model, "CD");
        d->media = prop_bool(drive, "MediaAvailable");
        d->vols = g_ptr_array_new_with_free_func(volume_free);
        g_hash_table_insert(keys, d, prop_str(drive, "SortKey"));
        g_ptr_array_add(disks, d);
        g_object_unref(drive);
    }
    for (GList *l = objects; l != nullptr; l = l->next) {
        GDBusProxy *b = iface(l->data, UD_BLOCK);
        GDBusProxy *part = iface(l->data, UD_PART);
        if (b != nullptr && part == nullptr) {
            char *drive = prop_str(b, "Drive");
            for (guint i = 0; i < disks->len; i++) {
                disk *d = g_ptr_array_index(disks, i);
                if (d->block_path != nullptr || strcmp(d->drive_path, drive) != 0)
                    continue;
                /* Not an unlocked volume's cleartext, which has the drive too. */
                char *backing = prop_str(b, "CryptoBackingDevice");
                bool cleartext = backing[0] != '\0' && strcmp(backing, "/") != 0;
                g_free(backing);
                if (!cleartext) {
                    d->block_path = g_strdup(g_dbus_object_get_object_path(l->data));
                    d->device = prop_str(b, "PreferredDevice");
                    guint64 size = prop_u64(b, "Size");
                    if (size > 0)
                        d->size = size;
                    GDBusProxy *table = iface(l->data, UD_TABLE);
                    d->table = table != nullptr ? prop_str(table, "Type") : nullptr;
                    g_clear_object(&table);
                    g_hash_table_insert(by_block, d->block_path, d);
                    /* A filesystem on the whole disk: a USB "superfloppy" or a CD. */
                    char *usage = prop_str(b, "IdUsage");
                    if (d->table == nullptr && usage[0] != '\0') {
                        volume *v = g_new0(volume, 1);
                        v->d = d;
                        v->path = g_strdup(d->block_path);
                        v->device = g_strdup(d->device);
                        v->size = d->size;
                        v->whole_disk = true;
                        v->id_type = prop_str(b, "IdType");
                        read_fs(v, l->data);
                        read_crypt(v, l->data);
                        g_ptr_array_add(d->vols, v);
                    }
                    g_free(usage);
                }
            }
            g_free(drive);
        }
        g_clear_object(&b);
        g_clear_object(&part);
    }
    /* Partitions, by their table. */
    for (GList *l = objects; l != nullptr; l = l->next) {
        GDBusProxy *part = iface(l->data, UD_PART);
        if (part == nullptr)
            continue;
        char *table = prop_str(part, "Table");
        disk *d = g_hash_table_lookup(by_block, table);
        g_free(table);
        if (d != nullptr) {
            GDBusProxy *b = iface(l->data, UD_BLOCK);
            volume *v = g_new0(volume, 1);
            v->d = d;
            v->path = g_strdup(g_dbus_object_get_object_path(l->data));
            v->device = prop_str(b, "PreferredDevice");
            v->partition = true;
            v->offset = prop_u64(part, "Offset");
            v->size = prop_u64(part, "Size");
            v->flags = prop_u64(part, "Flags");
            v->number = (guint)prop_u64(part, "Number");
            v->ptype = prop_str(part, "Type");
            v->container = prop_bool(part, "IsContainer");
            v->contained = prop_bool(part, "IsContained");
            v->id_type = prop_str(b, "IdType");
            read_fs(v, l->data);
            read_crypt(v, l->data);
            g_ptr_array_add(d->vols, v);
            g_clear_object(&b);
        }
        g_object_unref(part);
    }
    /* Unlocked encrypted volumes: show what's inside. */
    for (GList *l = objects; l != nullptr; l = l->next) {
        GDBusProxy *b = iface(l->data, UD_BLOCK);
        char *backing = b != nullptr ? prop_str(b, "CryptoBackingDevice") : g_strdup("/");
        for (guint i = 0; strcmp(backing, "/") != 0 && i < disks->len; i++) {
            disk *d = g_ptr_array_index(disks, i);
            for (guint k = 0; k < d->vols->len; k++) {
                volume *v = g_ptr_array_index(d->vols, k);
                if (strcmp(v->path, backing) == 0) {
                    v->unlocked = true;
                    read_fs(v, l->data);
                }
            }
        }
        g_free(backing);
        g_clear_object(&b);
    }
    g_list_free_full(objects, g_object_unref);
    g_hash_table_unref(by_block);

    /* Disk 0, 1, ... in UDisks2's order; CD-ROMs counted apart. */
    g_ptr_array_sort_with_data(disks, compare_keys, keys);
    int n_disk = 0, n_cd = 0;
    for (guint i = 0; i < disks->len; i++) {
        disk *d = g_ptr_array_index(disks, i);
        d->number = d->optical ? n_cd++ : n_disk++;
        g_ptr_array_sort_values(d->vols, compare_vols);
        for (guint k = 0; k < d->vols->len; k++) {
            volume *v = g_ptr_array_index(d->vols, k);
            for (char **mp = v->mounts; mp != nullptr && *mp != nullptr; mp++)
                if (xd_mount_is_system(*mp))
                    d->system = true;
            d->system |= v->swap_active;
        }
        layout_disk(d);
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

static char *disk_name(const disk *d) {
    return g_strdup_printf("%s %d", d->optical ? "CD-ROM" : "Disk", d->number);
}

/* "Data (/run/media/me/Data)", "(sdb1)": the Volume column. */
static char *volume_name(const volume *v) {
    const char *where = mounted(v) ? v->mounts[0] : dev_name(v->device);
    return v->label != nullptr && v->label[0] != '\0'
               ? g_strdup_printf("%s (%s)", v->label, where)
               : g_strdup_printf("(%s)", where);
}

static char *volume_fs(const volume *v) {
    char *fs = xd_disk_fs_label(v->unlocked ? v->fs_type : v->id_type, v->id_version);
    if (v->unlocked && v->has_fs) {
        /* What's inside, read into fs_type, id_version and label by
         * read_fs. */
        char *out = g_strdup_printf("%s (encrypted)", fs);
        g_free(fs);
        return out;
    }
    if (fs[0] == '\0' && v->partition && !v->container) {
        g_free(fs);
        return g_strdup("RAW");
    }
    return fs;
}

static bool volume_active(const volume *v) {
    return g_strcmp0(v->d->table, "dos") == 0 && (v->flags & DOS_ACTIVE) != 0;
}

static char *volume_status(const volume *v) {
    return xd_volume_status((const char *const *)v->mounts, v->swap_active,
                            xd_part_type_is_esp(v->ptype), volume_active(v));
}

/* "Disk 1 Partition 2 (sdb2), 2.00 GB NTFS "Data"", for confirmations. */
static char *volume_description(const volume *v) {
    char *dn = disk_name(v->d);
    char *size = xd_disk_size_text(v->size);
    char *fs = volume_fs(v);
    GString *s = g_string_new(dn);
    if (v->partition)
        g_string_append_printf(s, " Partition %u", v->number);
    g_string_append_printf(s, " (%s), %s", dev_name(v->device), size);
    if (v->container)
        g_string_append(s, " extended partition");
    else if (fs[0] != '\0')
        g_string_append_printf(s, " %s", fs);
    if (v->label != nullptr && v->label[0] != '\0')
        g_string_append_printf(s, " \"%s\"", v->label);
    g_free(dn);
    g_free(size);
    g_free(fs);
    return g_string_free(s, FALSE);
}

/* ---- selection ---------------------------------------------------------------------- */

static volume *seg_volume(const disk *d, int seg) {
    if (d == nullptr || seg < 0 || (guint)seg >= d->segments->len)
        return nullptr;
    const xd_disk_segment *s = &g_array_index(d->segments, xd_disk_segment, seg);
    return s->index >= 0 ? g_ptr_array_index(d->vols, (guint)s->index) : nullptr;
}

static const xd_disk_segment *sel_segment(dm *m) {
    if (m->sel.d == nullptr || m->sel.seg < 0 || (guint)m->sel.seg >= m->sel.d->segments->len)
        return nullptr;
    return &g_array_index(m->sel.d->segments, xd_disk_segment, (guint)m->sel.seg);
}

static volume *sel_volume(dm *m) { return seg_volume(m->sel.d, m->sel.seg); }

/* The extended partition of the selected disk, if it has one. */
static volume *extended_of(const disk *d) {
    for (guint i = 0; d != nullptr && i < d->vols->len; i++) {
        volume *v = g_ptr_array_index(d->vols, i);
        if (v->container)
            return v;
    }
    return nullptr;
}

static void show_selection_status(dm *m);
static void fill_action_menu(dm *m);

static void select_volume_row(dm *m) {
    volume *v = sel_volume(m);
    GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(m->list));
    m->updating = true;
    gtk_tree_selection_unselect_all(ts);
    GtkTreeIter it;
    bool ok = v != nullptr && gtk_tree_model_get_iter_first(GTK_TREE_MODEL(m->store), &it);
    while (ok) {
        volume *rv = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(m->store), &it, COL_VOL, &rv, -1);
        if (rv == v) {
            gtk_tree_selection_select_iter(ts, &it);
            break;
        }
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(m->store), &it);
    }
    m->updating = false;
}

/* Redraws disk `d`'s row of the map. */
static void queue_draw_disk(dm *m, const disk *d) {
    for (guint i = 0; d != nullptr && i < m->disks->len; i++)
        if (g_ptr_array_index(m->disks, i) == d)
            gtk_widget_queue_draw_area(m->map, 0, MARGIN + (int)i * (ROW_H + ROW_GAP),
                                       gtk_widget_get_allocated_width(m->map), ROW_H + 1);
}

/* Moves the selection, redrawing only the rows it leaves and enters. */
static void move_selection(dm *m, disk *d, int seg) {
    queue_draw_disk(m, m->sel.d);
    m->sel = (selection){.d = d, .seg = seg};
    queue_draw_disk(m, d);
}

static void set_selection(dm *m, disk *d, int seg) {
    move_selection(m, d, seg);
    select_volume_row(m);
    show_selection_status(m);
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
    capability_ask *a = data;
    dm *m = a->m;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr);
    if (r == nullptr) {
        /* No answer: asked again at the next refresh. */
        g_hash_table_remove(m->asked, a->key);
    } else if (a->format) {
        gboolean available = FALSE;
        g_variant_get(r, "((b&s))", &available, nullptr);
        g_hash_table_insert(m->can_format, g_strdup(a->type), g_strdup(available ? "yes" : "no"));
    } else {
        gboolean available = FALSE;
        guint64 flags = 0;
        g_variant_get(r, "((bt&s))", &available, &flags, nullptr);
        guint64 modes = available ? flags : 0;
        g_hash_table_insert(m->resize, g_strdup(a->type), GSIZE_TO_POINTER((gsize)modes + 1));
        fill_action_menu(m);
    }
    if (r != nullptr)
        g_variant_unref(r);
    g_free(a->key);
    g_free(a->type);
    g_free(a);
}

static void ask_capability(dm *m, const char *type, bool format) {
    if (m->bus == nullptr || type == nullptr || type[0] == '\0')
        return;
    char *key = g_strconcat(format ? "format:" : "resize:", type, nullptr);
    /* The set takes key either way. */
    if (!g_hash_table_add(m->asked, key))
        return;
    capability_ask *a = g_new0(capability_ask, 1);
    a->m = m;
    a->key = g_strdup(key);
    a->type = g_strdup(type);
    a->format = format;
    g_dbus_connection_call(m->bus, UD, UD_PATH "/Manager", UD_MANAGER,
                           format ? "CanFormat" : "CanResize", g_variant_new("(s)", type),
                           G_VARIANT_TYPE(format ? "((bs))" : "((bts))"), G_DBUS_CALL_FLAGS_NONE,
                           -1, nullptr, on_capability, a);
}

static void ask_capabilities(dm *m) {
    for (size_t i = 0; i < G_N_ELEMENTS(formats); i++)
        ask_capability(m, formats[i].type, true);
    for (guint i = 0; i < m->disks->len; i++) {
        const disk *d = g_ptr_array_index(m->disks, i);
        for (guint k = 0; k < d->vols->len; k++) {
            const volume *v = g_ptr_array_index(d->vols, k);
            if (v->has_fs)
                ask_capability(m, v->id_type, false);
        }
    }
}

static bool can_format(dm *m, const char *type) {
    return g_strcmp0(g_hash_table_lookup(m->can_format, type), "yes") == 0;
}

/* CanResize's modes for a filesystem type, 0 if it can't (or isn't known
 * yet). */
static guint64 resize_modes(dm *m, const char *type) {
    gpointer known = g_hash_table_lookup(m->resize, type);
    return known != nullptr ? GPOINTER_TO_SIZE(known) - 1 : 0;
}

/* ---- what can be done ---------------------------------------------------------------- */

static xd_volume_use volume_use(const volume *v) {
    bool logicals = false;
    for (guint i = 0; v->container && i < v->d->vols->len; i++)
        logicals |= ((volume *)g_ptr_array_index(v->d->vols, i))->contained;
    return (xd_volume_use){
        .mounts = (const char *const *)v->mounts,
        .swap_active = v->swap_active,
        .unlocked = v->unlocked,
        .container = v->container,
        .has_logical = logicals,
        .id_type = v->id_type,
    };
}

/* Why `action` can't be done to the selected volume, or nullptr. */
static const char *action_reason(dm *m, volume *v, xd_disk_action action, bool *unmount) {
    *unmount = false;
    if (m->busy)
        return "Disk Management is busy with another operation.";
    if (v == nullptr)
        return "Select a volume first.";
    if (v->whole_disk && (action == XD_ACTION_DELETE || action == XD_ACTION_SHRINK ||
                          action == XD_ACTION_EXTEND || action == XD_ACTION_SET_TYPE ||
                          action == XD_ACTION_SET_ACTIVE))
        return "This volume fills the whole disk, without a partition table.";
    if (v->d->optical && action != XD_ACTION_LABEL)
        return "CDs and DVDs can't be changed here.";
    xd_volume_use u = volume_use(v);
    const char *why = xd_volume_lock_reason(&u, action, unmount);
    if (why != nullptr)
        return why;
    if (action == XD_ACTION_SET_ACTIVE) {
        if (g_strcmp0(v->d->table, "dos") != 0)
            return "Only MBR disks have an active partition; GPT disks start from the EFI "
                   "System Partition.";
        if (v->contained)
            return "Only a primary partition can be active.";
        if (volume_active(v))
            return "This partition is already active.";
    }
    if (action == XD_ACTION_SHRINK || action == XD_ACTION_EXTEND) {
        bool shrink = action == XD_ACTION_SHRINK;
        if (!v->has_fs || v->unlocked)
            return "Only a partition holding a file system can be resized.";
        guint64 modes = resize_modes(m, v->id_type);
        xd_resize_how how = xd_disk_resize_how(modes, shrink, mounted(v));
        switch (how) {
        case XD_RESIZE_NO:
            return shrink ? "This file system can't be shrunk."
                          : "This file system can't be extended.";
        case XD_RESIZE_NEED_MOUNT:
            return "This file system can only be resized while it's mounted. Mount it first.";
        case XD_RESIZE_NEED_UNMOUNT:
            for (char **mp = v->mounts; *mp != nullptr; mp++)
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
            for (guint i = 0; i < v->d->segments->len; i++)
                if (seg_volume(v->d, (int)i) == v)
                    seg = i;
            if (xd_disk_grow_max(v->d->segments, seg, v->d->size) <= v->size)
                return "There's no free space right after this partition to extend it into.";
        } else if (xd_disk_shrink_min(v->fs_total > 0 ? v->fs_total - v->fs_free : 0, v->size) +
                       XD_MIB >
                   v->size) {
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

static void refresh(dm *m);

static op *op_new(dm *m, const char *title) {
    op *o = g_new0(op, 1);
    o->m = m;
    o->title = g_strdup(title);
    g_queue_init(&o->steps);
    return o;
}

static void op_add(op *o, const char *path, const char *iface, const char *method,
                   GVariant *args) {
    step *s = g_new0(step, 1);
    s->path = g_strdup(path);
    s->iface = g_strdup(iface);
    s->method = g_strdup(method);
    s->args = g_variant_ref_sink(args);
    g_queue_push_tail(&o->steps, s);
}

static void step_free(gpointer data) {
    step *s = data;
    g_free(s->path);
    g_free(s->iface);
    g_free(s->method);
    g_variant_unref(s->args);
    g_free(s);
}

static void op_free(op *o) {
    g_queue_clear_full(&o->steps, step_free);
    g_free(o->title);
    g_free(o->created);
    g_free(o);
}

static GVariant *no_options(void) { return g_variant_new("a{sv}", nullptr); }

static void set_busy(dm *m, const char *what) {
    m->busy = what != nullptr;
    GdkWindow *w = gtk_widget_get_window(m->window);
    if (w != nullptr) {
        GdkCursor *c = what != nullptr ? gdk_cursor_new_from_name(gdk_window_get_display(w),
                                                                  "progress")
                                       : nullptr;
        gdk_window_set_cursor(w, c);
        g_clear_object(&c);
    }
    if (what != nullptr) {
        gtk_label_set_text(GTK_LABEL(m->status), what);
        fill_action_menu(m);
    } else {
        show_selection_status(m);
    }
}

static void op_next(op *o);

static void step_done(GObject *src, GAsyncResult *res, gpointer data) {
    op *o = data;
    dm *m = o->m;
    GError *error = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (r == nullptr) {
        g_dbus_error_strip_remote_error(error);
        char *text = g_strdup_printf("Disk Management could not complete the operation.\n\n%s",
                                     error->message);
        set_busy(m, nullptr);
        xd_message_box(GTK_WINDOW(m->window), "Disk Management", XD_MB_ERROR, XD_MB_OK, FALSE,
                       text);
        g_free(text);
        g_error_free(error);
        op_free(o);
        refresh(m);
        return;
    }
    if (g_variant_is_of_type(r, G_VARIANT_TYPE("(o)"))) {
        g_free(o->created);
        g_variant_get(r, "(o)", &o->created);
    }
    g_variant_unref(r);
    op_next(o);
}

static void op_next(op *o) {
    dm *m = o->m;
    step *s = g_queue_pop_head(&o->steps);
    if (s == nullptr) {
        set_busy(m, nullptr);
        op_free(o);
        refresh(m);
        return;
    }
    const char *path = s->path != nullptr ? s->path : o->created;
    if (path == nullptr) {
        step_free(s);
        op_next(o);
        return;
    }
    /* The new partition encrypted: its file system is in the cleartext
     * device UDisks2 left unlocked. */
    g_autofree char *cleartext = nullptr;
    if (s->path == nullptr && strcmp(s->iface, UD_FS) == 0 && m->om != nullptr) {
        GDBusInterface *enc = g_dbus_object_manager_get_interface(m->om, path, UD_ENC);
        if (enc != nullptr) {
            cleartext = prop_str(G_DBUS_PROXY(enc), "CleartextDevice");
            if (strcmp(cleartext, "/") != 0 && cleartext[0] != '\0')
                path = cleartext;
            g_object_unref(enc);
        }
    }
    /* No timeout: polkit's password box, and formatting, take a while. */
    g_dbus_connection_call(m->bus, UD, path, s->iface, s->method, s->args, nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                           step_done, o);
    step_free(s);
}

static void op_run(op *o) {
    set_busy(o->m, o->title);
    op_next(o);
}

static void op_unmount(op *o, const volume *v) {
    if (mounted(v))
        op_add(o, v->fs_path, UD_FS, "Unmount", g_variant_new("(@a{sv})", no_options()));
}

static bool confirm(dm *m, const char *title, const char *text) {
    return xd_message_box(GTK_WINDOW(m->window), title, XD_MB_WARNING, XD_MB_YES_NO, TRUE,
                          text) == GTK_RESPONSE_YES;
}

/* ---- small dialogs ---------------------------------------------------------------- */

static GtkWidget *dialog_new(dm *m, const char *title, GtkWidget **content) {
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(m->window), GTK_DIALOG_MODAL,
                                               "OK", GTK_RESPONSE_OK, "Cancel",
                                               GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(*content), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), *content, TRUE, TRUE,
                       0);
    return d;
}

static GtkWidget *wrap_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
}

static GtkWidget *grid_new(void) {
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    return g;
}

static void grid_row(GtkWidget *g, int row, const char *key, GtkWidget *value) {
    GtkWidget *k = gtk_label_new_with_mnemonic(key);
    gtk_label_set_xalign(GTK_LABEL(k), 0);
    if (GTK_IS_LABEL(value) == FALSE)
        gtk_label_set_mnemonic_widget(GTK_LABEL(k), value);
    gtk_grid_attach(GTK_GRID(g), k, 0, row, 1, 1);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(g), value, 1, row, 1, 1);
}

static GtkWidget *value_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_set_can_focus(l, FALSE);
    return l;
}

/* The filesystems UDisks2 can make here, as a combo (ids are types). */
static GtkWidget *fs_combo(dm *m, const char *preferred) {
    GtkWidget *c = gtk_combo_box_text_new();
    for (size_t i = 0; i < G_N_ELEMENTS(formats); i++)
        if (can_format(m, formats[i].type))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), formats[i].type, formats[i].label);
    if (preferred == nullptr || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), preferred))
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), "ext4"))
            gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
    return c;
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
static guint sector_size_of(const disk *d) {
    char *path = g_strdup_printf("/sys/class/block/%s/queue/logical_block_size",
                                 dev_name(d->device));
    char *text = nullptr;
    guint size = 512;
    if (g_file_get_contents(path, &text, nullptr, nullptr)) {
        guint64 n = g_ascii_strtoull(text, nullptr, 10);
        if (n >= 512 && n <= 65536)
            size = (guint)n;
    }
    g_free(text);
    g_free(path);
    return size;
}

/* UDisks2 2.10 added Format's "mkfs-args", which the allocation unit
 * size goes through. */
static bool udisks_has_mkfs_args(dm *m) {
    if (m->version == nullptr && m->om != nullptr) {
        GDBusInterface *i =
            g_dbus_object_manager_get_interface(m->om, UD_PATH "/Manager", UD_MANAGER);
        if (i != nullptr) {
            m->version = prop_str(G_DBUS_PROXY(i), "Version");
            g_object_unref(i);
        }
    }
    return xd_udisks_version_at_least(m->version, 2, 10);
}

/* The allocation unit sizes the chosen file system can be made with:
 * Default only (greyed) when it, mkfs or UDisks2 can't choose. */
static void fill_units(format_opts *f) {
    const char *type = gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->fs));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(f->unit));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(f->unit), "0", "Default");
    const guint *sizes = xd_format_unit_sizes(type);
    bool supported = sizes != nullptr && udisks_has_mkfs_args(f->m);
    int n = 0;
    for (size_t i = 0; supported && sizes[i] != 0; i++) {
        char **args = xd_format_unit_args(type, sizes[i], f->sector_size);
        if (args == nullptr)
            continue;
        g_strfreev(args);
        char *id = g_strdup_printf("%u", sizes[i]), *label = xd_format_unit_label(sizes[i]);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(f->unit), id, label);
        g_free(id);
        g_free(label);
        n++;
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(f->unit), "0");
    gtk_widget_set_sensitive(f->unit, n > 0 && gtk_widget_get_sensitive(f->fs));
    gtk_widget_set_tooltip_text(f->unit,
                                n > 0                ? nullptr
                                : sizes == nullptr ? "This file system is made with its default "
                                                     "size only."
                                                   : "This version of UDisks2 can't choose the "
                                                     "size (2.10 and later can).");
}

static void on_fs_changed([[maybe_unused]] GtkComboBox *c, gpointer data) { fill_units(data); }

/* The File system and Allocation unit size rows, from `row` on. */
static void format_fs_rows(format_opts *f, dm *m, const disk *d, GtkWidget *grid, int row,
                           const char *preferred) {
    f->m = m;
    f->sector_size = sector_size_of(d);
    f->fs = fs_combo(m, preferred);
    grid_row(grid, row, "File s_ystem:", f->fs);
    f->unit = gtk_combo_box_text_new();
    grid_row(grid, row + 1, "_Allocation unit size:", f->unit);
    fill_units(f);
    g_signal_connect(f->fs, "changed", G_CALLBACK(on_fs_changed), f);
}

/* The encryption UDisks2 makes (Manager.DefaultEncryptionType, else
 * the first of SupportedEncryptionTypes: "luks2"), or nullptr when it
 * can't encrypt. */
static const char *encryption_type(dm *m) {
    if (!m->luks_read && m->om != nullptr) {
        m->luks_read = true;
        GDBusInterface *i =
            g_dbus_object_manager_get_interface(m->om, UD_PATH "/Manager", UD_MANAGER);
        GVariant *types = i != nullptr ? prop(G_DBUS_PROXY(i), "SupportedEncryptionTypes")
                                       : nullptr;
        g_autofree char *def = i != nullptr ? prop_str(G_DBUS_PROXY(i), "DefaultEncryptionType")
                                            : nullptr;
        if (types != nullptr && g_variant_is_of_type(types, G_VARIANT_TYPE_STRING_ARRAY)) {
            g_autofree const char **all = g_variant_get_strv(types, nullptr);
            if (def != nullptr && g_strv_contains(all, def))
                m->luks_type = g_strdup(def);
            else if (all[0] != nullptr)
                m->luks_type = g_strdup(all[0]);
        }
        if (types != nullptr)
            g_variant_unref(types);
        g_clear_object(&i);
    }
    return m->luks_type;
}

static void on_encrypt_toggled(GtkToggleButton *b, gpointer data) {
    format_opts *f = data;
    gtk_widget_set_sensitive(f->encrypt_rows, gtk_toggle_button_get_active(b) &&
                                                  gtk_widget_get_sensitive(GTK_WIDGET(b)));
}

static void format_checks(format_opts *f, GtkWidget *box) {
    f->quick = gtk_check_button_new_with_mnemonic("Perform a _quick format");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(f->quick), TRUE);
    gtk_widget_set_tooltip_text(f->quick, "Unticked, the whole volume is overwritten with zeros "
                                          "before it's formatted, which can take a long time.");
    f->compress = gtk_check_button_new_with_mnemonic("_Enable file and folder compression");
    gtk_widget_set_sensitive(f->compress, FALSE);
    gtk_box_pack_start(GTK_BOX(box), f->quick, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), f->compress, FALSE, FALSE, 0);
    /* Linux's counterpart of BitLocker, which XP didn't have: the volume
     * is LUKS, with the file system inside, unlocked by the passphrase. */
    f->encrypt = gtk_check_button_new_with_mnemonic("Encrypt this volume (_LUKS)");
    const char *luks = encryption_type(f->m);
    gtk_widget_set_sensitive(f->encrypt, luks != nullptr);
    gtk_widget_set_tooltip_text(f->encrypt, luks != nullptr
                                                ? "What's on the volume can only be read with "
                                                  "the passphrase. Don't lose it: nobody can "
                                                  "get the files back without it."
                                                : "UDisks2 can't make encrypted volumes here "
                                                  "(cryptsetup is missing).");
    gtk_box_pack_start(GTK_BOX(box), f->encrypt, FALSE, FALSE, 0);
    f->encrypt_rows = grid_new();
    gtk_widget_set_margin_start(f->encrypt_rows, 24);
    f->pass = gtk_entry_new();
    f->confirm = gtk_entry_new();
    GtkWidget *entries[] = {f->pass, f->confirm};
    for (int i = 0; i < 2; i++) {
        gtk_entry_set_visibility(GTK_ENTRY(entries[i]), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(entries[i]), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_activates_default(GTK_ENTRY(entries[i]), TRUE);
    }
    grid_row(f->encrypt_rows, 0, "Pa_ssphrase:", f->pass);
    grid_row(f->encrypt_rows, 1, "Confi_rm passphrase:", f->confirm);
    gtk_widget_set_sensitive(f->encrypt_rows, FALSE);
    gtk_box_pack_start(GTK_BOX(box), f->encrypt_rows, FALSE, FALSE, 0);
    g_signal_connect(f->encrypt, "toggled", G_CALLBACK(on_encrypt_toggled), f);
}

static bool format_encrypt(const format_opts *f) {
    return gtk_widget_get_sensitive(f->encrypt) &&
           gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->encrypt));
}

/* Why the passphrases typed can't encrypt the volume, or nullptr (also
 * when it isn't to be encrypted). */
static const char *format_encrypt_problem(const format_opts *f) {
    return format_encrypt(f) ? xd_luks_passphrase_problem(gtk_entry_get_text(GTK_ENTRY(f->pass)),
                                                          gtk_entry_get_text(GTK_ENTRY(f->confirm)))
                             : nullptr;
}

/* Empties the passphrase entries, so the passphrase isn't left in them. */
static void format_forget_passphrase(const format_opts *f) {
    gtk_entry_set_text(GTK_ENTRY(f->pass), "");
    gtk_entry_set_text(GTK_ENTRY(f->confirm), "");
}

static bool format_quick(const format_opts *f) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->quick));
}

static guint format_unit(const format_opts *f) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->unit));
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
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (label != nullptr && label[0] != '\0')
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string(label));
    /* Removes the old filesystem's /etc/fstab and crypttab entries. */
    g_variant_builder_add(&b, "{sv}", "tear-down", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&b, "{sv}", "update-partition-type", g_variant_new_boolean(TRUE));
    if (!quick)
        g_variant_builder_add(&b, "{sv}", "erase", g_variant_new_string("zero"));
    char **args = unit != 0 ? xd_format_unit_args(type, unit, sector_size) : nullptr;
    if (args != nullptr)
        g_variant_builder_add(&b, "{sv}", "mkfs-args",
                              g_variant_new_strv((const char *const *)args, -1));
    g_strfreev(args);
    if (passphrase != nullptr && luks != nullptr) {
        g_variant_builder_add(&b, "{sv}", "encrypt.passphrase", g_variant_new_string(passphrase));
        g_variant_builder_add(&b, "{sv}", "encrypt.type", g_variant_new_string(luks));
    }
    return g_variant_builder_end(&b);
}

/* The options as the dialog has them; `type` is the file system's. */
static GVariant *format_opts_variant(const format_opts *f, const char *type, const char *label) {
    bool encrypt = format_encrypt(f);
    return format_options(label, type, format_quick(f), format_unit(f), f->sector_size,
                          encrypt ? gtk_entry_get_text(GTK_ENTRY(f->pass)) : nullptr,
                          encrypt ? encryption_type(f->m) : nullptr);
}

/* ---- Initialize Disk -------------------------------------------------------------- */

static bool disk_uninitialized(const disk *d) {
    return d != nullptr && !d->optical && d->media && d->table == nullptr && d->vols->len == 0 &&
           d->block_path != nullptr;
}

static void initialize_disk(dm *m) {
    disk *d = m->sel.d;
    if (!disk_uninitialized(d) || m->busy)
        return;
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(m, "Initialize Disk", &box);
    char *dn = disk_name(d), *size = xd_disk_size_text(d->size);
    char *text = g_strdup_printf("You must initialize a disk before Disk Management can use "
                                 "it.\n\n%s (%s, %s, %s)",
                                 dn, dev_name(d->device), size, d->model);
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
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    use_gpt = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gpt));
    gtk_widget_destroy(dlg);
    if (r == GTK_RESPONSE_OK) {
        text = g_strdup_printf("%s (%s, %s) will get a new, empty %s partition table. Anything "
                               "on the disk that Disk Management can't see will be lost.\n\nDo "
                               "you want to continue?",
                               dn, dev_name(d->device), size, use_gpt ? "GPT" : "MBR");
        if (confirm(m, "Initialize Disk", text)) {
            char *title = g_strdup_printf("Initializing %s...", dn);
            op *o = op_new(m, title);
            op_add(o, d->block_path, UD_BLOCK, "Format",
                   g_variant_new("(s@a{sv})", use_gpt ? "gpt" : "dos", no_options()));
            op_run(o);
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

static bool wizard_extended(wizard *w) {
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->extended));
}

static gint wizard_forward(gint current, gpointer data) {
    wizard *w = data;
    /* An extended partition has no filesystem: skip Format. */
    if (current == 2 && wizard_extended(w))
        return 4;
    return current + 1;
}

static void wizard_summary(wizard *w) {
    GString *s = g_string_new("You have selected the following settings:\n\n");
    const char *kind = wizard_extended(w)                                         ? "Extended"
                       : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->logical)) ? "Logical drive"
                                                                                   : "Primary";
    char *dn = disk_name(w->d);
    g_string_append_printf(s, "Partition type: %s\nSelected disk: %s (%s)\n", kind, dn,
                           dev_name(w->d->device));
    g_free(dn);
    guint64 size = (guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->size)) * XD_MIB;
    char *st = xd_disk_size_text(size);
    g_string_append_printf(s, "Partition size: %s\n", st);
    g_free(st);
    if (!wizard_extended(w)) {
        bool fmt = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->do_format));
        char *fs = fmt ? gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(w->fo.fs)) : nullptr;
        char *unit = fmt ? gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(w->fo.unit))
                         : nullptr;
        g_string_append_printf(s, "File system: %s\n", fs != nullptr ? fs : "None");
        if (fmt)
            g_string_append_printf(s,
                                   "Allocation unit size: %s\nVolume label: %s\nQuick format: "
                                   "%s\n",
                                   unit != nullptr ? unit : "Default",
                                   gtk_entry_get_text(GTK_ENTRY(w->label)),
                                   format_quick(&w->fo) ? "Yes" : "No");
        bool encrypt = fmt && format_encrypt(&w->fo);
        char *luks = encrypt ? g_ascii_strup(encryption_type(w->m), -1) : nullptr;
        if (fmt)
            g_string_append_printf(s, "Encryption: %s\n", encrypt ? luks : "None");
        g_free(luks);
        g_string_append_printf(
            s, "Mount it: %s\n",
            fmt && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->mount)) ? "Yes" : "No");
        if (fmt && !format_quick(&w->fo))
            g_string_append_printf(s, "\n%s\n", FULL_FORMAT_NOTE);
        if (encrypt)
            g_string_append_printf(s, "\n%s\n", ENCRYPT_NOTE);
        g_free(fs);
        g_free(unit);
    }
    g_string_append(s, "\nTo close this wizard and create the partition, click Apply.");
    gtk_label_set_text(GTK_LABEL(w->summary), s->str);
    g_string_free(s, TRUE);
}

static void on_wizard_prepare(GtkAssistant *a, GtkWidget *page, gpointer data) {
    wizard *w = data;
    if (page == gtk_assistant_get_nth_page(a, 4))
        wizard_summary(w);
}

/* Next only once an encrypted volume's passphrase is typed twice alike;
 * the hint says why not. */
static void update_format_page(wizard *w) {
    const char *why = format_encrypt_problem(&w->fo);
    gtk_assistant_set_page_complete(GTK_ASSISTANT(w->assistant), w->fmt_page, why == nullptr);
    gtk_label_set_text(GTK_LABEL(w->fmt_hint), why != nullptr ? why : "");
}

static void on_format_toggled(GtkToggleButton *b, gpointer data) {
    wizard *w = data;
    bool on = gtk_toggle_button_get_active(b);
    gtk_widget_set_sensitive(w->fo.fs, on);
    gtk_widget_set_sensitive(w->label, on);
    gtk_widget_set_sensitive(w->mount, on);
    gtk_widget_set_sensitive(w->fo.quick, on);
    gtk_widget_set_sensitive(w->fo.encrypt, on && encryption_type(w->m) != nullptr);
    on_encrypt_toggled(GTK_TOGGLE_BUTTON(w->fo.encrypt), &w->fo);
    /* Back on, only where the file system can choose. */
    fill_units(&w->fo);
    update_format_page(w);
}

static void on_wizard_apply([[maybe_unused]] GtkAssistant *a, gpointer data) {
    wizard *w = data;
    dm *m = w->m;
    guint64 size = (guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->size)) * XD_MIB;
    if (size > w->max_size)
        size = w->max_size;
    bool extended = wizard_extended(w);
    bool logical = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->logical));
    bool fmt = !extended && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->do_format));
    GVariantBuilder ob;
    g_variant_builder_init(&ob, G_VARIANT_TYPE("a{sv}"));
    if (g_strcmp0(w->d->table, "dos") == 0)
        g_variant_builder_add(&ob, "{sv}", "partition-type",
                              g_variant_new_string(extended ? "extended"
                                                   : logical ? "logical"
                                                             : "primary"));
    GVariant *opts = g_variant_builder_end(&ob);
    char *dn = disk_name(w->d);
    char *title = g_strdup_printf("Creating a partition on %s...", dn);
    op *o = op_new(m, title);
    if (fmt) {
        const char *type = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w->fo.fs));
        if (type == nullptr)
            type = "ext4";
        op_add(o, w->d->block_path, UD_TABLE, "CreatePartitionAndFormat",
               g_variant_new("(ttss@a{sv}s@a{sv})", w->offset, size, "", "", opts, type,
                             format_opts_variant(&w->fo, type,
                                                 gtk_entry_get_text(GTK_ENTRY(w->label)))));
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->mount)))
            op_add(o, nullptr, UD_FS, "Mount", g_variant_new("(@a{sv})", no_options()));
    } else {
        op_add(o, w->d->block_path, UD_TABLE, "CreatePartition",
               g_variant_new("(ttss@a{sv})", w->offset, size, "", "", opts));
    }
    g_free(title);
    g_free(dn);
    format_forget_passphrase(&w->fo);
    /* GtkAssistant emits "close" after this, which destroys it. */
    op_run(o);
}

static void on_wizard_close(GtkAssistant *a, [[maybe_unused]] gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(a));
}

static void on_wizard_destroy([[maybe_unused]] GtkWidget *a, gpointer data) {
    wizard *w = data;
    g_ptr_array_unref(w->disks);
    g_free(w);
}

static GtkWidget *wizard_page(wizard *w, const char *title, GtkAssistantPageType type,
                              GtkWidget *body) {
    /* Each page in a box, its contents at the top. */
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    gtk_box_pack_start(GTK_BOX(page), body, FALSE, FALSE, 0);
    gtk_assistant_append_page(GTK_ASSISTANT(w->assistant), page);
    gtk_assistant_set_page_title(GTK_ASSISTANT(w->assistant), page, title);
    gtk_assistant_set_page_type(GTK_ASSISTANT(w->assistant), page, type);
    gtk_assistant_set_page_complete(GTK_ASSISTANT(w->assistant), page, TRUE);
    return page;
}

/* XP's wizards have no list of steps down the side: hide GtkAssistant's
 * (its internal child with the "sidebar" style class). */
static void hide_sidebar(GtkWidget *w, [[maybe_unused]] gpointer data) {
    if (gtk_style_context_has_class(gtk_widget_get_style_context(w), "sidebar")) {
        /* GtkAssistant shows it again on each page. */
        g_signal_connect_after(w, "show", G_CALLBACK(gtk_widget_hide), nullptr);
        gtk_widget_hide(w);
        return;
    }
    if (GTK_IS_CONTAINER(w))
        gtk_container_forall(GTK_CONTAINER(w), hide_sidebar, nullptr);
}

static void new_partition(dm *m) {
    const xd_disk_segment *s = sel_segment(m);
    disk *d = m->sel.d;
    if (s == nullptr || s->kind == XD_SEGMENT_PARTITION || m->busy || d->table == nullptr)
        return;
    guint n_primary = 0;
    for (guint i = 0; i < d->vols->len; i++)
        n_primary += !((volume *)g_ptr_array_index(d->vols, i))->contained;
    wizard *w = g_new0(wizard, 1);
    w->m = m;
    w->d = d;
    w->kinds = xd_disk_new_part_kinds(d->table, s->kind, n_primary, extended_of(d) != nullptr);
    if (!xd_disk_new_part_range(s->offset, s->size, d->size, s->kind == XD_SEGMENT_FREE,
                                &w->offset, &w->max_size) ||
        !(w->kinds.primary || w->kinds.extended || w->kinds.logical)) {
        g_free(w);
        xd_message_box(GTK_WINDOW(m->window), "Disk Management", XD_MB_INFO, XD_MB_OK, FALSE,
                       "No new partition can be made here: an MBR disk has room for four "
                       "primary partitions (or three and an extended one), and the space must "
                       "be at least 1 MB.");
        return;
    }
    w->disks = g_ptr_array_ref(m->disks);
    w->assistant = gtk_assistant_new();
    gtk_window_set_title(GTK_WINDOW(w->assistant), "New Partition Wizard");
    gtk_window_set_transient_for(GTK_WINDOW(w->assistant), GTK_WINDOW(m->window));
    gtk_window_set_modal(GTK_WINDOW(w->assistant), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(w->assistant), 520, 380);

    wizard_page(w, "Welcome to the New Partition Wizard", GTK_ASSISTANT_PAGE_INTRO,
                wrap_label("This wizard helps you create a partition on a basic disk.\n\nA "
                           "basic disk holds primary partitions and, on an MBR disk, an "
                           "extended partition with logical drives in it. Each partition can "
                           "have its own file system.\n\nTo continue, click Next."));

    GtkWidget *types = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(types), wrap_label("Select the partition you want to create:"),
                       FALSE, FALSE, 0);
    w->primary = gtk_radio_button_new_with_mnemonic(nullptr, "_Primary partition");
    w->extended = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(w->primary),
                                                                 "_Extended partition");
    w->logical = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(w->primary),
                                                                "_Logical drive");
    gtk_widget_set_sensitive(w->primary, w->kinds.primary);
    gtk_widget_set_sensitive(w->extended, w->kinds.extended);
    gtk_widget_set_sensitive(w->logical, w->kinds.logical);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->kinds.primary ? w->primary : w->logical),
                                 TRUE);
    gtk_box_pack_start(GTK_BOX(types), w->primary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(types), w->extended, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(types), w->logical, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(types),
                       wrap_label(g_strcmp0(d->table, "dos") == 0
                                      ? "A primary partition is a volume you create using free "
                                        "space on a basic disk. An MBR disk has up to four, or "
                                        "three and an extended partition, which holds any "
                                        "number of logical drives."
                                      : "A GPT disk has only primary partitions, up to 128."),
                       FALSE, FALSE, 8);
    wizard_page(w, "Select Partition Type", GTK_ASSISTANT_PAGE_CONTENT, types);

    GtkWidget *sizes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *g = grid_new();
    guint64 max_mb = w->max_size / XD_MIB;
    char *max_text = g_strdup_printf("%" G_GUINT64_FORMAT, max_mb);
    grid_row(g, 0, "Maximum disk space in MB:", value_label(max_text));
    g_free(max_text);
    grid_row(g, 1, "Minimum disk space in MB:", value_label("1"));
    w->size = gtk_spin_button_new_with_range(1, (double)max_mb, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->size), (double)max_mb);
    grid_row(g, 2, "Partition _size in MB:", w->size);
    gtk_box_pack_start(GTK_BOX(sizes), wrap_label("Choose a partition size between the "
                                                  "maximum and minimum sizes."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sizes), g, FALSE, FALSE, 0);
    wizard_page(w, "Specify Partition Size", GTK_ASSISTANT_PAGE_CONTENT, sizes);

    GtkWidget *fmt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(fmt), wrap_label("To store data on this partition, you must "
                                                "format it first."),
                       FALSE, FALSE, 0);
    w->no_format = gtk_radio_button_new_with_mnemonic(nullptr, "_Do not format this partition");
    w->do_format = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(w->no_format), "_Format this partition with the following settings:");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->do_format), TRUE);
    gtk_box_pack_start(GTK_BOX(fmt), w->no_format, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fmt), w->do_format, FALSE, FALSE, 0);
    GtkWidget *fg = grid_new();
    gtk_widget_set_margin_start(fg, 24);
    /* XP's order: File system, Allocation unit size, Volume label. */
    format_fs_rows(&w->fo, m, d, fg, 0, "ext4");
    w->label = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w->label), "New Volume");
    grid_row(fg, 2, "_Volume label:", w->label);
    gtk_box_pack_start(GTK_BOX(fmt), fg, FALSE, FALSE, 0);
    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(opts, 24);
    format_checks(&w->fo, opts);
    w->mount = gtk_check_button_new_with_mnemonic("_Mount it when it's formatted");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->mount), TRUE);
    gtk_box_pack_start(GTK_BOX(opts), w->mount, FALSE, FALSE, 0);
    w->fmt_hint = wrap_label("");
    gtk_box_pack_start(GTK_BOX(opts), w->fmt_hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fmt), opts, FALSE, FALSE, 0);
    g_signal_connect(w->do_format, "toggled", G_CALLBACK(on_format_toggled), w);
    g_signal_connect_swapped(w->fo.encrypt, "toggled", G_CALLBACK(update_format_page), w);
    g_signal_connect_swapped(w->fo.pass, "changed", G_CALLBACK(update_format_page), w);
    g_signal_connect_swapped(w->fo.confirm, "changed", G_CALLBACK(update_format_page), w);
    w->fmt_page = wizard_page(w, "Format Partition", GTK_ASSISTANT_PAGE_CONTENT, fmt);

    w->summary = wrap_label("");
    wizard_page(w, "Completing the New Partition Wizard", GTK_ASSISTANT_PAGE_CONFIRM, w->summary);

    gtk_assistant_set_forward_page_func(GTK_ASSISTANT(w->assistant), wizard_forward, w, nullptr);
    g_signal_connect(w->assistant, "prepare", G_CALLBACK(on_wizard_prepare), w);
    g_signal_connect(w->assistant, "apply", G_CALLBACK(on_wizard_apply), w);
    g_signal_connect(w->assistant, "cancel", G_CALLBACK(on_wizard_close), w);
    g_signal_connect(w->assistant, "close", G_CALLBACK(on_wizard_close), w);
    g_signal_connect(w->assistant, "destroy", G_CALLBACK(on_wizard_destroy), w);
    gtk_container_forall(GTK_CONTAINER(w->assistant), hide_sidebar, nullptr);
    gtk_widget_show_all(w->assistant);
}

/* ---- Delete, Format ---------------------------------------------------------------- */

static void delete_volume(dm *m, volume *v) {
    bool unmount = false;
    if (action_reason(m, v, XD_ACTION_DELETE, &unmount) != nullptr)
        return;
    char *desc = volume_description(v);
    char *text = g_strdup_printf("%s\n\nAll data on the partition will be lost.%s Are you sure "
                                 "you want to delete it?",
                                 desc, unmount ? " It will be unmounted first." : "");
    if (confirm(m, v->container ? "Delete Extended Partition" : "Delete Partition", text)) {
        char *title = g_strdup_printf("Deleting %s...", dev_name(v->device));
        op *o = op_new(m, title);
        if (unmount)
            op_unmount(o, v);
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "tear-down", g_variant_new_boolean(TRUE));
        op_add(o, v->path, UD_PART, "Delete", g_variant_new("(@a{sv})", g_variant_builder_end(&b)));
        op_run(o);
        g_free(title);
    }
    g_free(text);
    g_free(desc);
}

static void delete_partition(dm *m) { delete_volume(m, sel_volume(m)); }

/* The extended partition has no block of its own in the map: it's
 * deleted from its free space. */
static void delete_extended(dm *m) { delete_volume(m, extended_of(m->sel.d)); }

static void format_volume(dm *m) {
    volume *v = sel_volume(m);
    bool unmount = false;
    if (action_reason(m, v, XD_ACTION_FORMAT, &unmount) != nullptr)
        return;
    char *dn = volume_name(v);
    char *title = g_strdup_printf("Format %s", dn);
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(m, title, &box);
    g_free(title);
    GtkWidget *g = grid_new();
    GtkWidget *label = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(label), v->label != nullptr && v->label[0] != '\0'
                                             ? v->label
                                             : "New Volume");
    gtk_entry_set_activates_default(GTK_ENTRY(label), TRUE);
    grid_row(g, 0, "_Volume label:", label);
    format_opts fo = {};
    format_fs_rows(&fo, m, v->d, g, 1, v->has_fs ? v->id_type : "ext4");
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    format_checks(&fo, box);
    gtk_widget_show_all(dlg);
    int r = 0;
    const char *bad = nullptr;
    while ((r = gtk_dialog_run(GTK_DIALOG(dlg))) == GTK_RESPONSE_OK &&
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
    if (r == GTK_RESPONSE_OK && type != nullptr) {
        char *desc = volume_description(v);
        char *text = g_strdup_printf("%s\n\nFormatting this volume will erase all data on it.%s "
                                     "Back up any data you want to keep before formatting.%s%s%s"
                                     "\n\nDo you want to continue?",
                                     desc, unmount ? " It will be unmounted first." : "",
                                     quick ? "" : "\n\n", quick ? "" : FULL_FORMAT_NOTE,
                                     encrypt ? "\n\n" ENCRYPT_NOTE : "");
        if (confirm(m, "Format", text)) {
            char *busy = g_strdup_printf(quick ? "Formatting %s..."
                                               : "Formatting %s (a full format: this can take a "
                                                 "long time)...",
                                         dev_name(v->device));
            op *o = op_new(m, busy);
            if (unmount)
                op_unmount(o, v);
            op_add(o, v->path, UD_BLOCK, "Format", g_variant_new("(s@a{sv})", type, opts));
            op_run(o);
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

static guint seg_index_of(const volume *v) {
    for (guint i = 0; i < v->d->segments->len; i++)
        if (seg_volume(v->d, (int)i) == v)
            return i;
    return 0;
}

static void resize_volume(dm *m, bool shrink) {
    volume *v = sel_volume(m);
    bool unmount = false;
    if (action_reason(m, v, shrink ? XD_ACTION_SHRINK : XD_ACTION_EXTEND, &unmount) != nullptr)
        return;
    guint64 used = v->fs_total > 0 ? v->fs_total - v->fs_free : 0;
    guint64 min = shrink ? xd_disk_shrink_min(used, v->size) : v->size;
    guint64 max = shrink ? v->size : xd_disk_grow_max(v->d->segments, seg_index_of(v), v->d->size);
    guint64 room_mb = (shrink ? v->size - min : max - v->size) / XD_MIB;
    char *dn = volume_name(v);
    char *title = g_strdup_printf("%s %s", shrink ? "Shrink" : "Extend", dn);
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(m, title, &box);
    g_free(title);
    GtkWidget *g = grid_new();
    char *t = g_strdup_printf("%" G_GUINT64_FORMAT, v->size / XD_MIB);
    grid_row(g, 0, shrink ? "Total size before shrink in MB:" : "Total volume size in MB:",
             value_label(t));
    g_free(t);
    t = g_strdup_printf("%" G_GUINT64_FORMAT, room_mb);
    grid_row(g, 1, shrink ? "Size of available shrink space in MB:" : "Maximum available space in MB:",
             value_label(t));
    g_free(t);
    GtkWidget *amount = gtk_spin_button_new_with_range(1, (double)room_mb, 1);
    /* Not knowing how full it is, start at half the room rather than all. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(amount),
                              (double)(shrink && used == 0 ? XD_MAX(room_mb / 2, 1) : room_mb));
    grid_row(g, 2, shrink ? "Enter the amount of space to _shrink in MB:"
                          : "Select the amount of _space in MB:",
             amount);
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
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
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    guint64 change = (guint64)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(amount)) * XD_MIB;
    gtk_widget_destroy(dlg);
    if (r != GTK_RESPONSE_OK || change == 0) {
        g_free(dn);
        return;
    }
    guint64 target = shrink ? v->size - change : v->size + change;
    if (target < min)
        target = min;
    if (target > max)
        target = max;
    target = shrink ? xd_disk_align_up(target) : xd_disk_align_down(target);
    char *from = xd_disk_size_text(v->size), *to = xd_disk_size_text(target);
    char *desc = volume_description(v);
    char *text = g_strdup_printf("%s\n\n%s it from %s to %s?%s%s", desc,
                                 shrink ? "Shrink" : "Extend", from, to,
                                 shrink ? " If shrinking fails partway, data can be lost: back "
                                          "up first."
                                        : "",
                                 unmount ? " It will be unmounted first." : "");
    if (confirm(m, shrink ? "Shrink Volume" : "Extend Volume", text)) {
        char *busy = g_strdup_printf("%s %s...", shrink ? "Shrinking" : "Extending",
                                     dev_name(v->device));
        op *o = op_new(m, busy);
        if (unmount)
            op_unmount(o, v);
        /* Shrinking: the filesystem first, then its partition. Growing:
         * the partition first, then the filesystem to fill it. */
        if (shrink) {
            op_add(o, v->fs_path, UD_FS, "Resize", g_variant_new("(t@a{sv})", target, no_options()));
            op_add(o, v->path, UD_PART, "Resize", g_variant_new("(t@a{sv})", target, no_options()));
        } else {
            op_add(o, v->path, UD_PART, "Resize", g_variant_new("(t@a{sv})", target, no_options()));
            op_add(o, v->fs_path, UD_FS, "Resize",
                   g_variant_new("(t@a{sv})", (guint64)0, no_options()));
        }
        op_run(o);
        g_free(busy);
    }
    g_free(text);
    g_free(desc);
    g_free(from);
    g_free(to);
    g_free(dn);
}

/* ---- Label, Active, Type, mount points ---------------------------------------------------- */

static void change_label(dm *m) {
    volume *v = sel_volume(m);
    bool unmount = false;
    if (action_reason(m, v, XD_ACTION_LABEL, &unmount) != nullptr || !v->has_fs)
        return;
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(m, "Change Volume Label", &box);
    char *desc = volume_description(v);
    gtk_box_pack_start(GTK_BOX(box), wrap_label(desc), FALSE, FALSE, 0);
    g_free(desc);
    GtkWidget *g = grid_new();
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), v->label != nullptr ? v->label : "");
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    grid_row(g, 0, "_Volume label:", e);
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        op *o = op_new(m, "Changing the volume label...");
        op_add(o, v->fs_path, UD_FS, "SetLabel",
               g_variant_new("(s@a{sv})", gtk_entry_get_text(GTK_ENTRY(e)), no_options()));
        op_run(o);
    }
    gtk_widget_destroy(dlg);
}

static void mark_active(dm *m) {
    volume *v = sel_volume(m);
    bool unmount = false;
    if (action_reason(m, v, XD_ACTION_SET_ACTIVE, &unmount) != nullptr)
        return;
    char *desc = volume_description(v);
    char *text = g_strdup_printf("%s\n\nThe computer starts from the active partition. It "
                                 "won't start from this one unless it holds a boot loader.\n\n"
                                 "Do you want to mark it as active?",
                                 desc);
    if (confirm(m, "Mark Partition as Active", text)) {
        op *o = op_new(m, "Marking the partition as active...");
        /* Only one partition of a disk is active. */
        for (guint i = 0; i < v->d->vols->len; i++) {
            volume *other = g_ptr_array_index(v->d->vols, i);
            if (other != v && (other->flags & DOS_ACTIVE) != 0)
                op_add(o, other->path, UD_PART, "SetFlags",
                       g_variant_new("(t@a{sv})", other->flags & ~DOS_ACTIVE, no_options()));
        }
        op_add(o, v->path, UD_PART, "SetFlags",
               g_variant_new("(t@a{sv})", v->flags | DOS_ACTIVE, no_options()));
        op_run(o);
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

static void change_type(dm *m) {
    volume *v = sel_volume(m);
    bool unmount = false;
    if (action_reason(m, v, XD_ACTION_SET_TYPE, &unmount) != nullptr)
        return;
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(m, "Change Partition Type", &box);
    char *desc = volume_description(v);
    gtk_box_pack_start(GTK_BOX(box), wrap_label(desc), FALSE, FALSE, 0);
    g_free(desc);
    GtkWidget *combo = gtk_combo_box_text_new_with_entry();
    for (size_t i = 0; i < xd_part_type_count; i++) {
        if (strcmp(xd_part_types[i].table, v->d->table) != 0 ||
            g_str_has_prefix(xd_part_types[i].name, "Extended"))
            continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), xd_part_types[i].id,
                                  xd_part_types[i].name);
    }
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), v->ptype))
        gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), v->ptype);
    GtkWidget *g = grid_new();
    grid_row(g, 0, "Partition _type:", combo);
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       wrap_label(strcmp(v->d->table, "dos") == 0
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
            if (strcmp(xd_part_types[i].table, v->d->table) == 0 &&
                g_ascii_strcasecmp(xd_part_types[i].name, typed) == 0)
                id = xd_part_types[i].id;
        char *new_type = g_ascii_strdown(id != nullptr ? id : typed, -1);
        g_free(typed);
        if (!valid_type(v->d->table, new_type)) {
            xd_message_box(GTK_WINDOW(dlg), "Change Partition Type", XD_MB_ERROR, XD_MB_OK, FALSE,
                           "That isn't a partition type.");
            g_free(new_type);
            continue;
        }
        if (g_ascii_strcasecmp(new_type, v->ptype) != 0) {
            op *o = op_new(m, "Changing the partition type...");
            op_add(o, v->path, UD_PART, "SetType",
                   g_variant_new("(s@a{sv})", new_type, no_options()));
            op_run(o);
        }
        g_free(new_type);
        break;
    }
    gtk_widget_destroy(dlg);
}

static void mount_volume(dm *m, volume *v, bool mount) {
    if (v == nullptr || !v->has_fs || m->busy)
        return;
    char *title = g_strdup_printf("%s %s...", mount ? "Mounting" : "Unmounting",
                                  dev_name(v->device));
    op *o = op_new(m, title);
    op_add(o, v->fs_path, UD_FS, mount ? "Mount" : "Unmount",
           g_variant_new("(@a{sv})", no_options()));
    op_run(o);
    g_free(title);
}

/* ---- encryption (UDisks2's Encrypted interface) ---------------------------------------- */

/* What the passphrase boxes call the volume: its label, else its device
 * ("sde1"). */
static const char *crypt_name(const volume *v) {
    return v->label != nullptr && v->label[0] != '\0' ? v->label : dev_name(v->device);
}

static bool needed_by_system(const volume *v) {
    for (char **mp = v->mounts; mp != nullptr && *mp != nullptr; mp++)
        if (xd_mount_is_system(*mp))
            return true;
    return false;
}

/* Why Unlock (`unlock`), Lock or Change Passphrase (`change`) can't be
 * done to the volume, or nullptr. */
static const char *crypt_reason(dm *m, const volume *v, bool unlock, bool change) {
    if (m->busy)
        return "Disk Management is busy with another operation.";
    if (v == nullptr || !v->encrypted)
        return "This volume isn't encrypted.";
    if (change)
        return nullptr;
    if (unlock)
        return v->unlocked ? "It's unlocked already." : nullptr;
    if (!v->unlocked)
        return "It's locked already.";
    return needed_by_system(v) ? "The running system needs this volume." : nullptr;
}

/* An Unlock or ChangePassphrase on its way; the volume may be gone when
 * it's done, so what's needed is copied. */
typedef struct crypt_req {
    dm *m;
    char *path, *name;
} crypt_req;

static crypt_req *crypt_req_new(dm *m, const volume *v) {
    crypt_req *c = g_new0(crypt_req, 1);
    c->m = m;
    c->path = g_strdup(v->path);
    c->name = g_strdup(crypt_name(v));
    return c;
}

static void crypt_req_free(crypt_req *c) {
    g_free(c->path);
    g_free(c->name);
    g_free(c);
}

/* The call's error, or nullptr (then *wrong says whether the passphrase
 * was wrong; a cancelled polkit box is no error). */
static GError *crypt_finish(GObject *src, GAsyncResult *res, bool *wrong, bool *dismissed) {
    GError *err = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r != nullptr)
        g_variant_unref(r);
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

static void ask_unlock(dm *m, const char *path, const char *name, bool retry);

static void on_unlocked(GObject *src, GAsyncResult *res, gpointer data) {
    crypt_req *c = data;
    dm *m = c->m;
    bool wrong = false, dismissed = false;
    GError *err = crypt_finish(src, res, &wrong, &dismissed);
    set_busy(m, nullptr);
    if (err != nullptr && wrong) {
        ask_unlock(m, c->path, c->name, true);
    } else if (err != nullptr && !dismissed) {
        char *text = g_strdup_printf("%s could not be unlocked.\n\n%s", c->name, err->message);
        xd_message_box(GTK_WINDOW(m->window), "Disk Management", XD_MB_ERROR, XD_MB_OK, FALSE,
                       text);
        g_free(text);
    }
    g_clear_error(&err);
    crypt_req_free(c);
    refresh(m);
}

/* The passphrase box (again, saying so, after a wrong one), then
 * Unlock. */
static void ask_unlock(dm *m, const char *path, const char *name, bool retry) {
    char *pass = xd_luks_ask_passphrase(GTK_WINDOW(m->window), name, retry, nullptr);
    if (pass == nullptr)
        return;
    crypt_req *c = g_new0(crypt_req, 1);
    c->m = m;
    c->path = g_strdup(path);
    c->name = g_strdup(name);
    char *busy = g_strdup_printf("Unlocking %s...", name);
    set_busy(m, busy);
    g_free(busy);
    g_dbus_connection_call(m->bus, UD, path, UD_ENC, "Unlock",
                           g_variant_new("(sa{sv})", pass, nullptr), G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                           on_unlocked, c);
    xd_luks_passphrase_free(pass);
}

static void unlock_volume(dm *m) {
    volume *v = sel_volume(m);
    if (crypt_reason(m, v, true, false) == nullptr)
        ask_unlock(m, v->path, crypt_name(v), false);
}

/* Lock: what's inside is unmounted first. */
static void lock_volume(dm *m) {
    volume *v = sel_volume(m);
    if (crypt_reason(m, v, false, false) != nullptr)
        return;
    char *title = g_strdup_printf("Locking %s...", crypt_name(v));
    op *o = op_new(m, title);
    op_unmount(o, v);
    op_add(o, v->path, UD_ENC, "Lock", g_variant_new("(@a{sv})", no_options()));
    op_run(o);
    g_free(title);
}

static void on_passphrase_changed(GObject *src, GAsyncResult *res, gpointer data) {
    crypt_req *c = data;
    dm *m = c->m;
    bool wrong = false, dismissed = false;
    GError *err = crypt_finish(src, res, &wrong, &dismissed);
    set_busy(m, nullptr);
    if (err == nullptr) {
        xd_message_box(GTK_WINDOW(m->window), "Change Passphrase", XD_MB_INFO, XD_MB_OK, FALSE,
                       "The passphrase has been changed.");
    } else if (!dismissed) {
        char *text = wrong ? g_strdup("The old passphrase you typed is incorrect. The "
                                      "passphrase has not been changed.")
                           : g_strdup_printf("The passphrase could not be changed.\n\n%s",
                                             err->message);
        xd_message_box(GTK_WINDOW(m->window), "Change Passphrase", XD_MB_ERROR, XD_MB_OK, FALSE,
                       text);
        g_free(text);
    }
    g_clear_error(&err);
    crypt_req_free(c);
    refresh(m);
}

static void change_passphrase(dm *m) {
    volume *v = sel_volume(m);
    if (crypt_reason(m, v, false, true) != nullptr)
        return;
    char *old = nullptr, *new_passphrase = nullptr;
    if (!xd_luks_ask_change(GTK_WINDOW(m->window), crypt_name(v), &old, &new_passphrase))
        return;
    crypt_req *c = crypt_req_new(m, v);
    char *busy = g_strdup_printf("Changing the passphrase of %s...", c->name);
    set_busy(m, busy);
    g_free(busy);
    g_dbus_connection_call(m->bus, UD, c->path, UD_ENC, "ChangePassphrase",
                           g_variant_new("(ssa{sv})", old, new_passphrase, nullptr), nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                           on_passphrase_changed, c);
    xd_luks_passphrase_free(old);
    xd_luks_passphrase_free(new_passphrase);
}

/* ---- startup mount points (/etc/fstab, written by UDisks2) ------------------------------ */

/* Whether the running system mounts the volume, now or at startup. */
static bool volume_is_system(const volume *v) {
    for (char **mp = v->mounts; mp != nullptr && *mp != nullptr; mp++)
        if (xd_mount_is_system(*mp))
            return true;
    for (char **f = v->fstab; f != nullptr && *f != nullptr; f++)
        if (xd_mount_is_system(*f))
            return true;
    return false;
}

/* Why the volume's startup mount points can't be changed, or nullptr. */
static const char *startup_reason(dm *m, const volume *v) {
    if (m->busy)
        return "Disk Management is busy with another operation.";
    if (v->encrypted && !v->unlocked)
        return "Unlock the volume first: the file system inside, which is what's mounted, "
               "can only be seen unlocked.";
    if (!v->has_fs)
        return "There's no file system to mount.";
    if (v->d->optical)
        return "CDs and DVDs are mounted when they're inserted.";
    if (volume_is_system(v))
        return "The running system mounts this volume itself: its /etc/fstab entry can't be "
               "changed here.";
    if (v->uuid == nullptr || v->uuid[0] == '\0')
        return "This volume has no UUID to find it by when the computer starts.";
    return nullptr;
}

/* A field of an fstab entry ("(sa{sv})"): a string ("" when missing), or
 * a number. */
static char *item_str(GVariant *item, const char *key) {
    GVariant *details = g_variant_get_child_value(item, 1);
    GVariant *v = g_variant_lookup_value(details, key, G_VARIANT_TYPE_BYTESTRING);
    char *s = v != nullptr ? g_variant_dup_bytestring(v, nullptr) : g_strdup("");
    if (v != nullptr)
        g_variant_unref(v);
    g_variant_unref(details);
    return s;
}

static gint32 item_int(GVariant *item, const char *key) {
    GVariant *details = g_variant_get_child_value(item, 1);
    gint32 n = 0;
    g_variant_lookup(details, key, "i", &n);
    g_variant_unref(details);
    return n;
}

static GVariant *fstab_item(const char *fsname, const char *dir, const char *type,
                            const char *opts, gint32 freq, gint32 passno) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "fsname", g_variant_new_bytestring(fsname));
    g_variant_builder_add(&b, "{sv}", "dir", g_variant_new_bytestring(dir));
    g_variant_builder_add(&b, "{sv}", "type", g_variant_new_bytestring(type));
    g_variant_builder_add(&b, "{sv}", "opts", g_variant_new_bytestring(opts));
    g_variant_builder_add(&b, "{sv}", "freq", g_variant_new_int32(freq));
    g_variant_builder_add(&b, "{sv}", "passno", g_variant_new_int32(passno));
    return g_variant_new("(s@a{sv})", "fstab", g_variant_builder_end(&b));
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

/* The volume other than `v` mounted at `dir` at startup, or nullptr. */
static volume *startup_owner(dm *m, const volume *v, const char *dir) {
    for (guint i = 0; i < m->disks->len; i++) {
        disk *d = g_ptr_array_index(m->disks, i);
        for (guint k = 0; k < d->vols->len; k++) {
            volume *o = g_ptr_array_index(d->vols, k);
            if (o != v && o->fstab != nullptr && g_strv_contains((const char *const *)o->fstab, dir))
                return o;
        }
    }
    return nullptr;
}

/* Why the folder `dir` (cleaned) can't take the volume, or nullptr. */
static char *startup_dir_problem(dm *m, const volume *v, const char *dir) {
    const char *why = xd_fstab_dir_problem(dir);
    if (why != nullptr)
        return g_strdup(why);
    volume *other = startup_owner(m, v, dir);
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
    bool here = v->mounts != nullptr && g_strv_contains((const char *const *)v->mounts, dir);
    GDir *gd = here ? nullptr : g_dir_open(dir, 0, nullptr);
    bool empty = gd == nullptr || g_dir_read_name(gd) == nullptr;
    if (gd != nullptr)
        g_dir_close(gd);
    if (!empty)
        return g_strdup_printf("%s isn't empty. Choose an empty folder: what's in it would be "
                               "hidden while the volume is mounted there.",
                               dir);
    return nullptr;
}

static void on_browse_folder(GtkButton *b, gpointer entry) {
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));
    GtkWidget *fc = gtk_file_chooser_dialog_new("Browse for Empty Folder", GTK_WINDOW(top),
                                                GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel",
                                                GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_ACCEPT,
                                                nullptr);
    char *current = xd_fstab_dir_clean(gtk_entry_get_text(GTK_ENTRY(entry)));
    char *parent = g_path_get_dirname(current);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc),
                                        g_file_test(parent, G_FILE_TEST_IS_DIR) ? parent : "/");
    if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (chosen != nullptr)
            gtk_entry_set_text(GTK_ENTRY(entry), chosen);
        g_free(chosen);
    }
    gtk_widget_destroy(fc);
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

static void on_keyfile_toggled(GtkToggleButton *b, gpointer data) {
    crypt_startup *c = data;
    bool on = gtk_toggle_button_get_active(b);
    gtk_widget_set_sensitive(c->pass, on);
    gtk_widget_set_sensitive(c->confirm, on);
}

/* The "Unlocking at startup" group of the Add box. */
static void crypt_startup_rows(crypt_startup *c, const volume *v, GtkWidget *box) {
    GtkWidget *frame = gtk_frame_new("Unlocking at startup");
    GtkWidget *fb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(fb), 8);
    c->ask = gtk_radio_button_new_with_mnemonic(nullptr, "As_k for the passphrase when the "
                                                         "computer starts");
    c->keyfile = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(c->ask), "Unlock it with a key _file kept on this computer");
    gtk_box_pack_start(GTK_BOX(fb), c->ask, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fb), c->keyfile, FALSE, FALSE, 0);
    char *name = xd_crypttab_name(v->luks_uuid);
    char *path = xd_crypttab_key_path(name);
    char *warning = g_strdup_printf(
        "A key file (%s) holds the passphrase itself. Only administrators can read it, but "
        "anyone who can read this computer's system disk can too: unless that disk is "
        "encrypted as well, this volume's encryption then no longer protects it from someone "
        "who takes the computer.",
        path);
    GtkWidget *w = wrap_label(warning);
    gtk_widget_set_margin_start(w, 24);
    gtk_box_pack_start(GTK_BOX(fb), w, FALSE, FALSE, 0);
    g_free(warning);
    g_free(path);
    g_free(name);
    GtkWidget *g = grid_new();
    gtk_widget_set_margin_start(g, 24);
    c->pass = gtk_entry_new();
    c->confirm = gtk_entry_new();
    GtkWidget *entries[] = {c->pass, c->confirm};
    for (int i = 0; i < 2; i++) {
        gtk_entry_set_visibility(GTK_ENTRY(entries[i]), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(entries[i]), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_activates_default(GTK_ENTRY(entries[i]), TRUE);
        gtk_widget_set_sensitive(entries[i], FALSE);
    }
    grid_row(g, 0, "Pa_ssphrase:", c->pass);
    grid_row(g, 1, "_Confirm passphrase:", c->confirm);
    gtk_box_pack_start(GTK_BOX(fb), g, FALSE, FALSE, 0);
    g_signal_connect(c->keyfile, "toggled", G_CALLBACK(on_keyfile_toggled), c);
    gtk_container_add(GTK_CONTAINER(frame), fb);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
}

/* XP's Add Drive Letter or Path, for a folder: where and how the volume
 * is mounted at startup, and with `crypt` (an encrypted volume without
 * an /etc/crypttab entry yet) how it's unlocked. False when cancelled;
 * else *dir and *opts are set (free them), and crypt's choice. */
static bool startup_dialog(dm *m, const volume *v, const char *title, const char *dir0,
                           const char *opts0, char **dir, char **opts, crypt_startup *crypt) {
    GtkWidget *box = nullptr;
    GtkWidget *dlg = dialog_new(m, title, &box);
    char *desc = volume_description(v);
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
    GtkWidget *g = grid_new();
    GtkWidget *options = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(options), opts0);
    gtk_entry_set_activates_default(GTK_ENTRY(options), TRUE);
    grid_row(g, 0, "Mount _options:", options);
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       wrap_label("nofail: the computer still starts when the disk isn't there. "
                                  "x-mount.mkdir: the folder is made if it doesn't exist. "
                                  "uid and gid: whose the files are, on file systems without "
                                  "owners (FAT, exFAT, NTFS)."),
                       FALSE, FALSE, 0);
    if (crypt != nullptr)
        crypt_startup_rows(crypt, v, box);
    gtk_widget_show_all(dlg);
    bool ok = false;
    while (!ok && gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        char *d = xd_fstab_dir_clean(gtk_entry_get_text(GTK_ENTRY(entry)));
        char *o = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(options))));
        char *why = startup_dir_problem(m, v, d);
        if (why == nullptr && (o[0] == '\0' || strpbrk(o, " \t\n") != nullptr))
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
            g_free(d);
            g_free(o);
            continue;
        }
        *dir = d;
        *opts = o;
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
static GVariant *crypttab_item(const volume *v, const crypt_startup *c) {
    char *name = xd_crypttab_name(v->luks_uuid);
    char *device = g_strdup_printf("UUID=%s", v->luks_uuid);
    char *key = c->use_keyfile ? xd_crypttab_key_path(name) : g_strdup("");
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "name", g_variant_new_bytestring(name));
    g_variant_builder_add(&b, "{sv}", "device", g_variant_new_bytestring(device));
    g_variant_builder_add(&b, "{sv}", "passphrase-path", g_variant_new_bytestring(key));
    g_variant_builder_add(&b, "{sv}", "passphrase-contents",
                          g_variant_new_bytestring(c->use_keyfile ? c->passphrase : ""));
    g_variant_builder_add(&b, "{sv}", "options", g_variant_new_bytestring("nofail"));
    g_free(name);
    g_free(device);
    g_free(key);
    return g_variant_new("(s@a{sv})", "crypttab", g_variant_builder_end(&b));
}

/* Mounts the volume at its new startup mount point now, as Windows'
 * new drive letter works straight away: UDisks2 mounts a volume in
 * /etc/fstab where the entry says. One mounted elsewhere is moved if the
 * user agrees. */
static void startup_mount_now(dm *m, op *o, const volume *v, const char *dir) {
    if (!mounted(v)) {
        op_add(o, v->fs_path, UD_FS, "Mount", g_variant_new("(@a{sv})", no_options()));
        return;
    }
    if (g_strv_contains((const char *const *)v->mounts, dir))
        return;
    char *name = volume_name(v);
    char *text = g_strdup_printf("%s is mounted in %s now. Do you want to move it to %s now?\n\n"
                                 "Programs using files on it might not run correctly. If you "
                                 "choose No, it moves the next time the computer starts.",
                                 name, v->mounts[0], dir);
    if (xd_message_box(GTK_WINDOW(m->window), "Disk Management", XD_MB_QUESTION, XD_MB_YES_NO,
                       FALSE, text) == GTK_RESPONSE_YES) {
        op_unmount(o, v);
        op_add(o, v->fs_path, UD_FS, "Mount", g_variant_new("(@a{sv})", no_options()));
    }
    g_free(text);
    g_free(name);
}

static void add_startup(dm *m, volume *v) {
    if (startup_reason(m, v) != nullptr)
        return;
    char *suggested = xd_fstab_suggest_dir(v->label, v->device);
    char *defaults = xd_fstab_default_options(v->fs_type, (guint)getuid(), (guint)getgid());
    char *dir = nullptr, *opts = nullptr;
    /* An encrypted volume is unlocked first, by its /etc/crypttab entry
     * (one already there is kept). */
    bool needs_crypttab = v->unlocked && v->crypttab_items != nullptr &&
                          v->crypttab_items->len == 0;
    crypt_startup crypt = {};
    if (startup_dialog(m, v, "Add Startup Mount Point", suggested, defaults, &dir, &opts,
                       needs_crypttab ? &crypt : nullptr)) {
        char *fsname = g_strdup_printf("UUID=%s", v->uuid);
        op *o = op_new(m, "Adding the startup mount point...");
        if (needs_crypttab)
            op_add(o, v->path, UD_BLOCK, "AddConfigurationItem",
                   g_variant_new("(@(sa{sv})@a{sv})", crypttab_item(v, &crypt), no_options()));
        /* freq 0, passno 0: no dump, and no fsck holding up the start
         * (as GNOME Disks adds them). */
        op_add(o, v->fs_path, UD_BLOCK, "AddConfigurationItem",
               g_variant_new("(@(sa{sv})@a{sv})",
                             fstab_item(fsname, dir, fstab_type(v->fs_type), opts, 0, 0),
                             no_options()));
        startup_mount_now(m, o, v, dir);
        op_run(o);
        g_free(fsname);
    }
    xd_luks_passphrase_free(crypt.passphrase);
    g_free(dir);
    g_free(opts);
    g_free(suggested);
    g_free(defaults);
}

static void change_startup(dm *m, volume *v, guint index) {
    if (startup_reason(m, v) != nullptr || index >= v->fstab_items->len)
        return;
    GVariant *old = g_ptr_array_index(v->fstab_items, index);
    char *dir0 = item_str(old, "dir"), *opts0 = item_str(old, "opts");
    char *dir = nullptr, *opts = nullptr;
    if (startup_dialog(m, v, "Change Startup Mount Point", dir0, opts0, &dir, &opts, nullptr) &&
        (strcmp(dir, dir0) != 0 || strcmp(opts, opts0) != 0)) {
        /* The rest of the entry stays as it was. */
        char *fsname = item_str(old, "fsname"), *type = item_str(old, "type");
        op *o = op_new(m, "Changing the startup mount point...");
        op_add(o, v->fs_path, UD_BLOCK, "UpdateConfigurationItem",
               g_variant_new("(@(sa{sv})@(sa{sv})@a{sv})", old,
                             fstab_item(fsname, dir, type, opts, item_int(old, "freq"),
                                        item_int(old, "passno")),
                             no_options()));
        startup_mount_now(m, o, v, dir);
        op_run(o);
        g_free(fsname);
        g_free(type);
    }
    g_free(dir);
    g_free(opts);
    g_free(dir0);
    g_free(opts0);
}

static void remove_startup(dm *m, volume *v, guint index) {
    if (startup_reason(m, v) != nullptr || index >= v->fstab_items->len)
        return;
    GVariant *old = g_ptr_array_index(v->fstab_items, index);
    char *dir = item_str(old, "dir");
    char *name = volume_name(v);
    /* With its last startup mount point, an encrypted volume needn't be
     * unlocked at startup either. */
    bool crypttab = v->fstab_items->len == 1 && v->crypttab_items != nullptr &&
                    v->crypttab_items->len > 0;
    char *text = g_strdup_printf("%s won't be mounted in %s when the computer starts.%s It "
                                 "stays mounted until you unmount it.\n\nDo you want to remove "
                                 "this startup mount point?",
                                 name, dir,
                                 crypttab ? " Nor will it be unlocked: its /etc/crypttab entry "
                                            "goes too, and its key file if it has one."
                                          : "");
    if (confirm(m, "Remove Startup Mount Point", text)) {
        op *o = op_new(m, "Removing the startup mount point...");
        op_add(o, v->fs_path, UD_BLOCK, "RemoveConfigurationItem",
               g_variant_new("(@(sa{sv})@a{sv})", old, no_options()));
        for (guint i = 0; crypttab && i < v->crypttab_items->len; i++)
            op_add(o, v->path, UD_BLOCK, "RemoveConfigurationItem",
                   g_variant_new("(@(sa{sv})@a{sv})", g_ptr_array_index(v->crypttab_items, i),
                                 no_options()));
        op_run(o);
    }
    g_free(text);
    g_free(name);
    g_free(dir);
}

/* The Change Mount Point dialog's responses beyond Mount and Unmount. */
enum { MP_MOUNT = 1, MP_UNMOUNT, MP_ADD, MP_CHANGE, MP_REMOVE };

static void on_startup_button(GtkButton *b, gpointer dlg) {
    gtk_dialog_response(GTK_DIALOG(dlg),
                        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "response")));
}

static void on_startup_selection(GtkTreeSelection *ts, gpointer buttons) {
    bool any = gtk_tree_selection_count_selected_rows(ts) > 0;
    GtkWidget **b = buttons;
    bool allowed = g_object_get_data(G_OBJECT(b[0]), "allowed") != nullptr;
    gtk_widget_set_sensitive(b[0], any && allowed);
    gtk_widget_set_sensitive(b[1], any && allowed);
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
static void mount_points(dm *m) {
    volume *v = sel_volume(m);
    if (v == nullptr || !v->has_fs || m->busy)
        return;
    char *dn = volume_name(v);
    char *title = g_strdup_printf("Change Mount Point for %s", dn);
    g_free(dn);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, GTK_WINDOW(m->window), GTK_DIALOG_MODAL,
                                                 "_Mount", MP_MOUNT, "_Unmount", MP_UNMOUNT,
                                                 "Close", GTK_RESPONSE_CLOSE, nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), wrap_label("This volume is mounted at:"), FALSE, FALSE, 0);
    GtkListStore *ls = gtk_list_store_new(1, G_TYPE_STRING);
    for (char **mp = v->mounts; *mp != nullptr; mp++)
        gtk_list_store_insert_with_values(ls, nullptr, -1, 0, *mp, -1);
    gtk_box_pack_start(GTK_BOX(box), path_list(ls, nullptr), FALSE, FALSE, 0);
    if (!mounted(v))
        gtk_box_pack_start(GTK_BOX(box),
                           wrap_label(v->fstab[0] != nullptr
                                          ? "It isn't mounted. Mount puts it in its startup "
                                            "mount point."
                                          : "It isn't mounted. Mount puts it in /run/media."),
                           FALSE, FALSE, 0);

    /* The startup mount points. */
    gtk_box_pack_start(GTK_BOX(box), wrap_label("Mount this volume when the computer starts at:"),
                       FALSE, FALSE, 6);
    ls = gtk_list_store_new(1, G_TYPE_STRING);
    for (char **f = v->fstab; *f != nullptr; f++)
        gtk_list_store_insert_with_values(ls, nullptr, -1, 0, *f, -1);
    GtkWidget *startup = nullptr;
    gtk_box_pack_start(GTK_BOX(box), path_list(ls, &startup), FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    static const struct {
        const char *label;
        int response;
    } bs[] = {{"_Add...", MP_ADD}, {"C_hange...", MP_CHANGE}, {"_Remove", MP_REMOVE}};
    GtkWidget *b[3];
    for (size_t i = 0; i < G_N_ELEMENTS(bs); i++) {
        b[i] = gtk_button_new_with_mnemonic(bs[i].label);
        g_object_set_data(G_OBJECT(b[i]), "response", GINT_TO_POINTER(bs[i].response));
        g_signal_connect(b[i], "clicked", G_CALLBACK(on_startup_button), dlg);
        gtk_box_pack_start(GTK_BOX(buttons), b[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    /* An encrypted volume's /etc/crypttab entry: how it's unlocked. */
    for (guint i = 0; v->crypttab_items != nullptr && i < v->crypttab_items->len; i++) {
        char *key = item_str(g_ptr_array_index(v->crypttab_items, i), "passphrase-path");
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
    const char *why = startup_reason(m, v);
    /* One startup mount point is enough; more can be in /etc/fstab
     * already, and are shown, changed and removed here. */
    bool has_one = v->fstab[0] != nullptr;
    gtk_widget_set_sensitive(b[0], why == nullptr && !has_one);
    gtk_widget_set_tooltip_text(b[0], why != nullptr ? why
                                      : has_one     ? "It has a startup mount point: change that "
                                                      "one instead."
                                                    : nullptr);
    GtkWidget *change_remove[2] = {b[1], b[2]};
    if (why == nullptr)
        g_object_set_data(G_OBJECT(b[1]), "allowed", GINT_TO_POINTER(1));
    GtkTreeSelection *ts = gtk_tree_view_get_selection(GTK_TREE_VIEW(startup));
    gulong selection_handler =
        g_signal_connect(ts, "changed", G_CALLBACK(on_startup_selection), change_remove);
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ls), &first))
        gtk_tree_selection_select_iter(ts, &first);
    on_startup_selection(ts, change_remove);
    if (why != nullptr)
        gtk_box_pack_start(GTK_BOX(box), wrap_label(why), FALSE, FALSE, 0);

    gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), MP_MOUNT, !mounted(v));
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), MP_UNMOUNT,
                                      mounted(v) && !volume_is_system(v));
    gtk_widget_show_all(dlg);
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    /* Which startup mount point Change and Remove are for. */
    guint index = 0;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(ts, &model, &it)) {
        GtkTreePath *p = gtk_tree_model_get_path(model, &it);
        index = (guint)gtk_tree_path_get_indices(p)[0];
        gtk_tree_path_free(p);
    }
    g_signal_handler_disconnect(ts, selection_handler);
    gtk_widget_destroy(dlg);
    switch (r) {
    case MP_MOUNT:
    case MP_UNMOUNT:
        mount_volume(m, v, r == MP_MOUNT);
        break;
    case MP_ADD:
        add_startup(m, v);
        break;
    case MP_CHANGE:
        change_startup(m, v, index);
        break;
    case MP_REMOVE:
        remove_startup(m, v, index);
        break;
    default:
        break;
    }
}

static void explore(dm *m) {
    volume *v = sel_volume(m);
    if (v == nullptr || !mounted(v))
        return;
    const char *argv[] = {"xd-files", v->mounts[0], nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(void *)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        xd_message_box(GTK_WINDOW(m->window), "Disk Management", XD_MB_ERROR, XD_MB_OK, FALSE,
                       error->message);
        g_error_free(error);
    }
}

static void eject(dm *m) {
    disk *d = m->sel.d;
    if (d == nullptr || !d->ejectable || m->busy || d->system)
        return;
    char *dn = disk_name(d);
    char *title = g_strdup_printf("Ejecting %s...", dn);
    op *o = op_new(m, title);
    for (guint i = 0; i < d->vols->len; i++)
        op_unmount(o, g_ptr_array_index(d->vols, i));
    op_add(o, d->drive_path, UD_DRIVE, "Eject", g_variant_new("(@a{sv})", no_options()));
    op_run(o);
    g_free(title);
    g_free(dn);
}

/* ---- Properties ---------------------------------------------------------------------- */

static gboolean draw_pie(GtkWidget *w, cairo_t *cr, gpointer data) {
    const volume *v = data;
    double width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    double rx = width / 2 - 4, ry = height / 2 - 10, cx = width / 2, cy = height / 2 - 6;
    double used = v->fs_total > 0 ? (double)(v->fs_total - v->fs_free) / (double)v->fs_total : 0;
    /* XP's 3D pie: blue used space, magenta free space. */
    for (int layer = 8; layer >= 0; layer--) {
        bool top = layer == 0;
        cairo_save(cr);
        cairo_translate(cr, cx, cy + layer);
        cairo_scale(cr, rx, ry);
        cairo_move_to(cr, 0, 0);
        cairo_arc(cr, 0, 0, 1, -XD_PI / 2, -XD_PI / 2 + used * 2 * XD_PI);
        cairo_close_path(cr);
        cairo_restore(cr);
        cairo_set_source_rgb(cr, 0.0, 0.0, top ? 1.0 : 0.6);
        cairo_fill(cr);
        cairo_save(cr);
        cairo_translate(cr, cx, cy + layer);
        cairo_scale(cr, rx, ry);
        cairo_move_to(cr, 0, 0);
        cairo_arc(cr, 0, 0, 1, -XD_PI / 2 + used * 2 * XD_PI, 3 * XD_PI / 2);
        cairo_close_path(cr);
        cairo_restore(cr);
        cairo_set_source_rgb(cr, top ? 1.0 : 0.6, 0.0, top ? 1.0 : 0.6);
        cairo_fill(cr);
    }
    return FALSE;
}

static GtkWidget *swatch(double r, double g, double b) {
    GtkWidget *a = gtk_drawing_area_new();
    gtk_widget_set_size_request(a, 14, 14);
    double *c = g_new(double, 3);
    c[0] = r;
    c[1] = g;
    c[2] = b;
    g_object_set_data_full(G_OBJECT(a), "rgb", c, g_free);
    return a;
}

static gboolean draw_swatch(GtkWidget *w, cairo_t *cr, [[maybe_unused]] gpointer data) {
    const double *c = g_object_get_data(G_OBJECT(w), "rgb");
    cairo_rectangle(cr, 0.5, 0.5, gtk_widget_get_allocated_width(w) - 1,
                    gtk_widget_get_allocated_height(w) - 1);
    cairo_set_source_rgb(cr, c[0], c[1], c[2]);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    return FALSE;
}

static char *bytes_text(guint64 n) {
    char *plain = g_strdup_printf("%" G_GUINT64_FORMAT, n);
    GString *s = g_string_new(nullptr);
    size_t len = strlen(plain);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            g_string_append_c(s, ',');
        g_string_append_c(s, plain[i]);
    }
    g_free(plain);
    char *human = xd_disk_size_text(n);
    g_string_append_printf(s, " bytes    %s", human);
    g_free(human);
    return g_string_free(s, FALSE);
}

static GtkWidget *hardware_page(const disk *d) {
    GtkWidget *g = grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(g), 12);
    int row = 0;
    char *dn = disk_name(d);
    grid_row(g, row++, "Disk:", value_label(dn));
    g_free(dn);
    grid_row(g, row++, "Model:", value_label(d->model));
    grid_row(g, row++, "Serial number:", value_label(d->serial[0] != '\0' ? d->serial : "None"));
    grid_row(g, row++, "Connection:", value_label(d->bus[0] != '\0' ? d->bus : "Internal"));
    grid_row(g, row++, "Device:", value_label(d->device != nullptr ? d->device : ""));
    grid_row(g, row++, "Removable:", value_label(d->removable ? "Yes" : "No"));
    grid_row(g, row++, "Partition style:",
             value_label(d->table == nullptr                  ? "Not initialized"
                         : strcmp(d->table, "dos") == 0 ? "Master Boot Record (MBR)"
                         : strcmp(d->table, "gpt") == 0 ? "GUID Partition Table (GPT)"
                                                        : d->table));
    char *size = d->media ? xd_disk_size_text(d->size) : g_strdup("No media");
    grid_row(g, row++, "Capacity:", value_label(size));
    g_free(size);
    return g;
}

static void on_props_response(GtkDialog *dlg, int response, gpointer data) {
    dm *m = data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(dlg), "label");
    const char *path = g_object_get_data(G_OBJECT(dlg), "fs-path");
    const char *old = g_object_get_data(G_OBJECT(dlg), "old-label");
    if ((response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY) && entry != nullptr &&
        g_strcmp0(gtk_entry_get_text(GTK_ENTRY(entry)), old) != 0 && !m->busy) {
        op *o = op_new(m, "Changing the volume label...");
        op_add(o, path, UD_FS, "SetLabel",
               g_variant_new("(s@a{sv})", gtk_entry_get_text(GTK_ENTRY(entry)), no_options()));
        op_run(o);
        g_object_set_data_full(G_OBJECT(dlg), "old-label",
                               g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))), g_free);
    }
    if (response != GTK_RESPONSE_APPLY)
        gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void properties(dm *m) {
    volume *v = sel_volume(m);
    disk *d = m->sel.d;
    if (d == nullptr)
        return;
    char *title = v != nullptr ? volume_name(v) : disk_name(d);
    char *full = g_strdup_printf("%s Properties", title);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(full, GTK_WINDOW(m->window),
                                                 GTK_DIALOG_DESTROY_WITH_PARENT, "OK",
                                                 GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
                                                 nullptr);
    g_free(full);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    if (v != nullptr) {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(page), 12);
        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_pack_start(GTK_BOX(head),
                           gtk_image_new_from_icon_name(d->removable ? "drive-removable-media"
                                                                     : "drive-harddisk",
                                                        GTK_ICON_SIZE_DND),
                           FALSE, FALSE, 0);
        GtkWidget *label = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(label), v->label != nullptr ? v->label : "");
        bool unused = false;
        gtk_widget_set_sensitive(label, v->has_fs && !m->busy &&
                                            action_reason(m, v, XD_ACTION_LABEL, &unused) == nullptr);
        gtk_box_pack_start(GTK_BOX(head), label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(dlg), "label", label);
        g_object_set_data_full(G_OBJECT(dlg), "fs-path", g_strdup(v->fs_path), g_free);
        g_object_set_data_full(G_OBJECT(dlg), "old-label", g_strdup(v->label), g_free);
        gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                           FALSE, 0);
        GtkWidget *g = grid_new();
        int row = 0;
        char *fs = volume_fs(v);
        grid_row(g, row++, "Type:", value_label(d->removable ? "Removable Disk" : "Local Disk"));
        grid_row(g, row++, "File system:", value_label(fs[0] != '\0' ? fs : "None"));
        g_free(fs);
        grid_row(g, row++, "Device:", value_label(v->device));
        if (v->partition) {
            const char *tn = xd_part_type_name(d->table, v->ptype);
            char *pt = tn != nullptr ? g_strdup_printf("%s (%s)", tn, v->ptype) : g_strdup(v->ptype);
            grid_row(g, row++, "Partition type:", value_label(pt));
            g_free(pt);
        }
        grid_row(g, row++, "Mounted at:", value_label(mounted(v) ? v->mounts[0] : "Not mounted"));
        gtk_box_pack_start(GTK_BOX(page), g, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                           FALSE, 0);
        GtkWidget *sg = grid_new();
        char *used = v->fs_total > 0 ? bytes_text(v->fs_total - v->fs_free) : g_strdup("Unknown");
        char *fr = v->fs_total > 0 ? bytes_text(v->fs_free) : g_strdup("Unknown");
        char *cap = bytes_text(v->fs_total > 0 ? v->fs_total : v->size);
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
        for (int r = 0; r < 3; r++) {
            if (rows[r].swatch) {
                GtkWidget *sw = swatch(rows[r].c.r, rows[r].c.g, rows[r].c.b);
                g_signal_connect(sw, "draw", G_CALLBACK(draw_swatch), nullptr);
                gtk_grid_attach(GTK_GRID(sg), sw, 0, r, 1, 1);
            }
            GtkWidget *k = gtk_label_new(rows[r].key);
            gtk_label_set_xalign(GTK_LABEL(k), 0);
            gtk_grid_attach(GTK_GRID(sg), k, 1, r, 1, 1);
            gtk_grid_attach(GTK_GRID(sg), value_label(rows[r].value), 2, r, 1, 1);
            g_free(rows[r].value);
        }
        gtk_box_pack_start(GTK_BOX(page), sg, FALSE, FALSE, 0);
        if (v->fs_total > 0) {
            /* The pie draws `v`, which a refresh would free. */
            g_object_set_data_full(G_OBJECT(dlg), "disks", g_ptr_array_ref(m->disks),
                                   (GDestroyNotify)g_ptr_array_unref);
            GtkWidget *pie = gtk_drawing_area_new();
            gtk_widget_set_size_request(pie, 160, 90);
            gtk_widget_set_halign(pie, GTK_ALIGN_CENTER);
            g_signal_connect(pie, "draw", G_CALLBACK(draw_pie), v);
            gtk_box_pack_start(GTK_BOX(page), pie, FALSE, FALSE, 0);
        } else {
            gtk_box_pack_start(GTK_BOX(page),
                               wrap_label("Used and free space are shown while it's mounted."),
                               FALSE, FALSE, 0);
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), page, gtk_label_new("General"));
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), hardware_page(d),
                             gtk_label_new(v != nullptr ? "Hardware" : "General"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), nb, TRUE, TRUE, 0);
    g_signal_connect(dlg, "response", G_CALLBACK(on_props_response), m);
    xd_gtk_notebook_ctrl_tab(dlg, GTK_NOTEBOOK(nb));
    gtk_widget_show_all(dlg);
    g_free(title);
}

/* ---- menus ---------------------------------------------------------------------------- */

typedef void (*action_fn)(dm *m);

typedef struct action_box {
    action_fn fn;
} action_box;

/* Runs an action. Its dialogs run nested main loops, where a refresh can
 * replace m->disks: the volumes and disks it holds stay alive until it's
 * done. */
static void run_action(dm *m, action_fn fn) {
    GPtrArray *held = g_ptr_array_ref(m->disks);
    fn(m);
    g_ptr_array_unref(held);
}

static void on_action(GtkMenuItem *i, gpointer data) {
    const action_box *a = g_object_get_data(G_OBJECT(i), "fn");
    if (a != nullptr && a->fn != nullptr)
        run_action(data, a->fn);
}

/* An item, greyed with the reason as its tooltip when it can't be done. */
static GtkWidget *item(GtkWidget *menu, const char *label, action_fn fn, dm *m,
                       const char *reason) {
    GtkWidget *i = gtk_menu_item_new_with_mnemonic(label);
    action_box *a = g_new(action_box, 1);
    a->fn = fn;
    g_object_set_data_full(G_OBJECT(i), "fn", a, g_free);
    g_signal_connect(i, "activate", G_CALLBACK(on_action), m);
    if (reason != nullptr) {
        gtk_widget_set_sensitive(i, FALSE);
        gtk_widget_set_tooltip_text(i, reason);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
    return i;
}

static void separator(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static void do_explore(dm *m) { explore(m); }
static void do_mount(dm *m) { mount_volume(m, sel_volume(m), true); }
static void do_unmount(dm *m) { mount_volume(m, sel_volume(m), false); }
static void do_extend(dm *m) { resize_volume(m, false); }
static void do_shrink(dm *m) { resize_volume(m, true); }

static const char *volume_reason(dm *m, xd_disk_action a) {
    bool unmount = false;
    return action_reason(m, sel_volume(m), a, &unmount);
}

/* The menu for what's selected: a volume, free space or a disk. */
/* Fills `menu` with what can be done to the selection. */
static void fill_selection_menu(dm *m, GtkWidget *menu) {
    const xd_disk_segment *s = sel_segment(m);
    volume *v = sel_volume(m);
    disk *d = m->sel.d;
    const char *busy = m->busy ? "Disk Management is busy with another operation." : nullptr;
    if (d == nullptr) {
        item(menu, "Select a disk or volume", nullptr, m, "Nothing is selected.");
        return;
    }
    if (s == nullptr) {
        item(menu, "_Initialize Disk...", initialize_disk, m,
             busy                       ? busy
             : disk_uninitialized(d)    ? nullptr
             : d->optical || !d->media ? "There's no disk to initialize."
                                       : "The disk is already initialized.");
        item(menu, "_Eject", eject, m,
             busy           ? busy
             : !d->ejectable ? "This disk can't be ejected."
             : d->system    ? "The running system is on this disk."
                            : nullptr);
        separator(menu);
        item(menu, "P_roperties", properties, m, nullptr);
        return;
    }
    if (v == nullptr) {
        item(menu, "_New Partition...", new_partition, m,
             busy                   ? busy
             : d->table == nullptr ? "Initialize the disk first: right-click it on the left."
                                   : nullptr);
        if (s->logical) {
            bool unmount = false;
            item(menu, "_Delete Extended Partition...", delete_extended, m,
                 action_reason(m, extended_of(d), XD_ACTION_DELETE, &unmount));
        }
        if (d->table == nullptr)
            item(menu, "_Initialize Disk...", initialize_disk, m,
                 busy ? busy : disk_uninitialized(d) ? nullptr : "There's no disk to initialize.");
        separator(menu);
        item(menu, "P_roperties", properties, m, nullptr);
        return;
    }
    if (!v->container) {
        item(menu, "_Open", do_explore, m, mounted(v) ? nullptr : "The volume isn't mounted.");
        item(menu, "E_xplore", do_explore, m, mounted(v) ? nullptr : "The volume isn't mounted.");
        separator(menu);
        item(menu, "_Mount", do_mount, m,
             busy ? busy : !v->has_fs ? "There's no file system to mount."
                         : mounted(v) ? "It's mounted already." : nullptr);
        bool system = false;
        for (char **mp = v->mounts; mp != nullptr && *mp != nullptr; mp++)
            system |= xd_mount_is_system(*mp);
        item(menu, "_Unmount", do_unmount, m,
             busy ? busy : !mounted(v) ? "It isn't mounted."
                         : system ? "The running system needs this volume." : nullptr);
        item(menu, "Change Mount _Point...", mount_points, m,
             busy         ? busy
             : v->has_fs  ? nullptr
             : v->encrypted ? "Unlock the volume first: what's mounted is the file system "
                              "inside."
                            : "There's no file system to mount.");
        if (v->encrypted) {
            separator(menu);
            if (v->unlocked)
                item(menu, "Loc_k", lock_volume, m, crypt_reason(m, v, false, false));
            else
                item(menu, "Unloc_k...", unlock_volume, m, crypt_reason(m, v, true, false));
            item(menu, "Change Pa_ssphrase...", change_passphrase, m,
                 crypt_reason(m, v, false, true));
        }
        item(menu, "Mark Partition as _Active", mark_active, m,
             volume_reason(m, XD_ACTION_SET_ACTIVE));
        separator(menu);
        item(menu, "_Format...", format_volume, m, volume_reason(m, XD_ACTION_FORMAT));
        item(menu, "Change _Label...", change_label, m, volume_reason(m, XD_ACTION_LABEL));
        item(menu, "Exte_nd Volume...", do_extend, m, volume_reason(m, XD_ACTION_EXTEND));
        item(menu, "S_hrink Volume...", do_shrink, m, volume_reason(m, XD_ACTION_SHRINK));
        item(menu, "Change Partition _Type...", change_type, m,
             volume_reason(m, XD_ACTION_SET_TYPE));
        separator(menu);
    }
    item(menu, "_Delete Partition...", delete_partition, m, volume_reason(m, XD_ACTION_DELETE));
    separator(menu);
    item(menu, "P_roperties", properties, m, nullptr);
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
static void popup_selection_menu(dm *m, GtkWidget *attach, GdkEvent *ev, bool at_pointer) {
    GtkWidget *menu = gtk_menu_new();
    fill_selection_menu(m, menu);
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

static void fill_list(dm *m) {
    m->updating = true;
    gtk_list_store_clear(m->store);
    for (guint i = 0; i < m->disks->len; i++) {
        disk *d = g_ptr_array_index(m->disks, i);
        for (guint k = 0; d->media && k < d->vols->len; k++) {
            volume *v = g_ptr_array_index(d->vols, k);
            if (v->container)
                continue;
            char *name = volume_name(v), *fs = volume_fs(v), *status = volume_status(v);
            char *cap = xd_disk_size_text(v->size);
            char *free_text = v->fs_total > 0 ? xd_disk_size_text(v->fs_free) : g_strdup("");
            guint pct = v->fs_total > 0 ? xd_disk_percent_free(v->fs_free, v->fs_total) : 0;
            char *pct_text = v->fs_total > 0 ? g_strdup_printf("%u %%", pct) : g_strdup("");
            gtk_list_store_insert_with_values(
                m->store, nullptr, -1, COL_ICON,
                d->optical ? "drive-optical" : d->removable ? "drive-removable-media"
                                                            : "drive-harddisk",
                COL_VOLUME, name, COL_LAYOUT, v->whole_disk ? "Simple" : "Partition", COL_TYPE,
                "Basic", COL_FS, fs, COL_STATUS, status, COL_CAPACITY, cap, COL_FREE, free_text,
                COL_PCT, pct_text, COL_SIZE, v->size, COL_FREE_SIZE, v->fs_free, COL_PCT_NUM, pct,
                COL_VOL, v, -1);
            g_free(name);
            g_free(fs);
            g_free(status);
            g_free(cap);
            g_free(free_text);
            g_free(pct_text);
        }
    }
    m->updating = false;
}

static void on_list_selection(GtkTreeSelection *ts, gpointer data) {
    dm *m = data;
    if (m->updating)
        return;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(ts, &model, &it))
        return;
    volume *v = nullptr;
    gtk_tree_model_get(model, &it, COL_VOL, &v, -1);
    if (v == nullptr)
        return;
    move_selection(m, v->d, (int)seg_index_of(v));
    show_selection_status(m);
}

static gboolean on_list_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    dm *m = data;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
        explore(m);
        return FALSE;
    }
    if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreePath *path = nullptr;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, nullptr,
                                      nullptr, nullptr)) {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
        gtk_tree_path_free(path);
    }
    popup_selection_menu(m, w, (GdkEvent *)ev, true);
    return TRUE;
}

/* ---- the disk map ----------------------------------------------------------------------- */

/* XP's legend colours. */
static const rgb C_PRIMARY = {0.0, 0.0, 0.5};
static const rgb C_LOGICAL = {0.25, 0.4, 0.9};
static const rgb C_EXTENDED = {0.0, 0.6, 0.0};
static const rgb C_FREE = {0.55, 0.9, 0.55};
static const rgb C_UNALLOCATED = {0.0, 0.0, 0.0};

static rgb segment_colour(const xd_disk_segment *s) {
    switch (s->kind) {
    case XD_SEGMENT_PARTITION:
        return s->logical ? C_LOGICAL : C_PRIMARY;
    case XD_SEGMENT_FREE:
        return C_FREE;
    case XD_SEGMENT_UNALLOCATED:
        return C_UNALLOCATED;
    }
    return C_UNALLOCATED;
}

/* The x and width of each block of disk `d`'s row, `width` wide. */
static int *block_xs(const disk *d, int width, int **widths_out) {
    guint n = d->segments->len;
    guint64 *sizes = g_new0(guint64, n + 1);
    for (guint i = 0; i < n; i++)
        sizes[i] = g_array_index(d->segments, xd_disk_segment, i).size;
    int *widths = g_new0(int, n + 1);
    int avail = width - HEADER_W - 2 * MARGIN;
    xd_disk_block_widths(sizes, n, avail, MIN_BLOCK, widths);
    int *xs = g_new0(int, n + 1);
    int x = MARGIN + HEADER_W;
    for (guint i = 0; i < n; i++) {
        xs[i] = x;
        x += widths[i];
    }
    g_free(sizes);
    *widths_out = widths;
    return xs;
}

static void text_lines(cairo_t *cr, GtkWidget *w, double x, double y, double width,
                       const char *markup) {
    PangoLayout *pl = gtk_widget_create_pango_layout(w, nullptr);
    pango_layout_set_markup(pl, markup, -1);
    pango_layout_set_width(pl, (int)(width * PANGO_SCALE));
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
}

/* XP marks the selection with diagonal hatching. */
static void hatch(cairo_t *cr, double x, double y, double w, double h) {
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.45);
    cairo_set_line_width(cr, 1);
    for (double d = -h; d < w; d += 5) {
        cairo_move_to(cr, x + d, y + h);
        cairo_line_to(cr, x + d + h, y);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

static char *segment_markup(const xd_disk_segment *s, const volume *v) {
    char *size = xd_disk_size_text(s->size);
    char *out;
    if (v == nullptr) {
        out = g_markup_printf_escaped("%s\n%s", size,
                                      s->kind == XD_SEGMENT_FREE ? "Free space" : "Unallocated");
    } else {
        char *name = volume_name(v), *fs = volume_fs(v), *status = volume_status(v);
        out = g_markup_printf_escaped("<b>%s</b>\n%s %s\n%s", name, size, fs, status);
        g_free(name);
        g_free(fs);
        g_free(status);
    }
    g_free(size);
    return out;
}

static gboolean draw_map(GtkWidget *w, cairo_t *cr, gpointer data) {
    dm *m = data;
    int width = gtk_widget_get_allocated_width(w);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_line_width(cr, 1);
    /* Only the rows in the area being redrawn: a selection change redraws
     * one or two, and scrolling only what comes into view. */
    GdkRectangle clip = {0, 0, width, gtk_widget_get_allocated_height(w)};
    gdk_cairo_get_clip_rectangle(cr, &clip);
    for (guint i = 0; i < m->disks->len; i++) {
        disk *d = g_ptr_array_index(m->disks, i);
        double y = MARGIN + i * (ROW_H + ROW_GAP);
        if (y + ROW_H < clip.y || y > clip.y + clip.height)
            continue;
        /* The disk's header, on the left. */
        cairo_rectangle(cr, MARGIN + 0.5, y + 0.5, HEADER_W - 3, ROW_H - 1);
        cairo_set_source_rgb(cr, 0.93, 0.92, 0.87);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_stroke(cr);
        if (m->sel.d == d && m->sel.seg < 0)
            hatch(cr, MARGIN + 1, y + 1, HEADER_W - 4, ROW_H - 2);
        char *dn = disk_name(d);
        char *size = d->media ? xd_disk_size_text(d->size) : g_strdup("");
        const char *kind = d->optical      ? "DVD"
                           : d->removable  ? "Removable"
                           : "Basic";
        const char *state = !d->media              ? "No Media"
                            : disk_uninitialized(d) ? "Not Initialized"
                                                    : "Online";
        char *markup = g_markup_printf_escaped("<b>%s</b>\n%s\n%s\n%s", dn, kind, size, state);
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        GdkPixbuf *icon = gtk_icon_theme_load_icon(theme,
                                                   d->optical ? "drive-optical"
                                                   : d->removable ? "drive-removable-media"
                                                                  : "drive-harddisk",
                                                   16, GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
        if (icon != nullptr) {
            gdk_cairo_set_source_pixbuf(cr, icon, MARGIN + 4, y + 4);
            cairo_paint(cr);
            g_object_unref(icon);
        }
        cairo_set_source_rgb(cr, 0, 0, 0);
        text_lines(cr, w, MARGIN + 24, y + 3, HEADER_W - 28, markup);
        g_free(markup);
        g_free(dn);
        g_free(size);
        if (!d->media)
            continue;
        int *widths = nullptr;
        int *xs = block_xs(d, width, &widths);
        for (guint k = 0; k < d->segments->len; k++) {
            const xd_disk_segment *s = &g_array_index(d->segments, xd_disk_segment, k);
            volume *v = seg_volume(d, (int)k);
            double x = xs[k], bw = widths[k];
            rgb c = segment_colour(s);
            /* The extended partition: one green frame around its logical
             * drives and free space. */
            bool first = k == 0 || !g_array_index(d->segments, xd_disk_segment, k - 1).logical;
            if (s->logical && first) {
                guint last = k;
                while (last + 1 < d->segments->len &&
                       g_array_index(d->segments, xd_disk_segment, last + 1).logical)
                    last++;
                cairo_rectangle(cr, x + 1.5, y + 1.5, xs[last] + widths[last] - x - 3, ROW_H - 3);
                cairo_set_source_rgb(cr, C_EXTENDED.r, C_EXTENDED.g, C_EXTENDED.b);
                cairo_set_line_width(cr, 3);
                cairo_stroke(cr);
                cairo_set_line_width(cr, 1);
            }
            double inset = s->logical ? 3 : 0;
            double bx = x + inset, by = y + inset, bww = bw - 2 * inset - 2, bh = ROW_H - 2 * inset;
            cairo_rectangle(cr, bx + 0.5, by + 0.5, bww - 1, bh - 1);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
            cairo_stroke(cr);
            cairo_rectangle(cr, bx + 1, by + 1, bww - 2, BAND);
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_fill(cr);
            if (m->sel.d == d && m->sel.seg == (int)k)
                hatch(cr, bx + 1, by + BAND + 1, bww - 2, bh - BAND - 2);
            char *text = segment_markup(s, v);
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_save(cr);
            cairo_rectangle(cr, bx + 2, by + BAND + 1, bww - 4, bh - BAND - 2);
            cairo_clip(cr);
            text_lines(cr, w, bx + 4, by + BAND + 3, bww - 8, text);
            cairo_restore(cr);
            g_free(text);
        }
        g_free(xs);
        g_free(widths);
    }
    return FALSE;
}

/* Which disk and block is at (x, y). */
static bool map_hit(dm *m, double x, double y, selection *out) {
    int width = gtk_widget_get_allocated_width(m->map);
    for (guint i = 0; i < m->disks->len; i++) {
        disk *d = g_ptr_array_index(m->disks, i);
        double top = MARGIN + i * (ROW_H + ROW_GAP);
        if (y < top || y > top + ROW_H)
            continue;
        if (x >= MARGIN && x < MARGIN + HEADER_W) {
            *out = (selection){.d = d, .seg = -1};
            return true;
        }
        if (!d->media)
            return false;
        int *widths = nullptr;
        int *xs = block_xs(d, width, &widths);
        bool hit = false;
        for (guint k = 0; k < d->segments->len && !hit; k++) {
            if (x >= xs[k] && x < xs[k] + widths[k]) {
                *out = (selection){.d = d, .seg = (int)k};
                hit = true;
            }
        }
        g_free(xs);
        g_free(widths);
        return hit;
    }
    return false;
}

static gboolean on_map_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
    dm *m = data;
    selection hit;
    if (ev->type != GDK_BUTTON_PRESS && ev->type != GDK_2BUTTON_PRESS)
        return FALSE;
    gtk_widget_grab_focus(w);
    if (!map_hit(m, ev->x, ev->y, &hit))
        return FALSE;
    set_selection(m, hit.d, hit.seg);
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
        if (sel_volume(m) != nullptr && mounted(sel_volume(m)))
            explore(m);
        else
            properties(m);
        return TRUE;
    }
    if (ev->button == GDK_BUTTON_SECONDARY) {
        popup_selection_menu(m, w, (GdkEvent *)ev, true);
    }
    return TRUE;
}

static void resize_map(dm *m) {
    int rows = (int)m->disks->len;
    gtk_widget_set_size_request(m->map, 560, 2 * MARGIN + rows * (ROW_H + ROW_GAP));
}

/* ---- refreshing ---------------------------------------------------------------------- */

/* The status bar, and the Action menu, for the selection. */
static void show_selection_status(dm *m) {
    fill_action_menu(m);
    if (m->busy)
        return;
    volume *v = sel_volume(m);
    const char *text = "";
    char *owned = nullptr;
    if (v != nullptr) {
        bool unmount = false;
        xd_volume_use u = volume_use(v);
        const char *why = xd_volume_lock_reason(&u, XD_ACTION_FORMAT, &unmount);
        if (why != nullptr && v->container == false)
            text = why;
    } else if (sel_segment(m) != nullptr) {
        text = "Right-click the free space and choose New Partition to make a volume.";
    } else if (m->sel.d != nullptr && disk_uninitialized(m->sel.d)) {
        owned = disk_name(m->sel.d);
        char *t = g_strdup_printf("%s isn't initialized: right-click it and choose Initialize "
                                  "Disk.",
                                  owned);
        g_free(owned);
        owned = t;
        text = owned;
    }
    gtk_label_set_text(GTK_LABEL(m->status), text);
    g_free(owned);
}

/* Keeps the selection across a refresh: the same volume, or the free
 * block at the same place on the same disk. */
static void refresh(dm *m) {
    char *disk_key = m->sel.d != nullptr ? g_strdup(m->sel.d->drive_path) : nullptr;
    volume *v = sel_volume(m);
    char *vol_key = v != nullptr ? g_strdup(v->path) : nullptr;
    const xd_disk_segment *s = sel_segment(m);
    guint64 offset = s != nullptr ? s->offset : 0;
    int seg = m->sel.seg;
    GPtrArray *old = m->disks;
    m->disks = read_disks(m);
    ask_capabilities(m);
    m->sel = (selection){.d = nullptr, .seg = -1};
    for (guint i = 0; disk_key != nullptr && i < m->disks->len; i++) {
        disk *d = g_ptr_array_index(m->disks, i);
        if (strcmp(d->drive_path, disk_key) != 0)
            continue;
        m->sel = (selection){.d = d, .seg = -1};
        for (guint k = 0; seg >= 0 && k < d->segments->len; k++) {
            volume *nv = seg_volume(d, (int)k);
            const xd_disk_segment *ns = &g_array_index(d->segments, xd_disk_segment, k);
            if ((vol_key != nullptr && nv != nullptr && strcmp(nv->path, vol_key) == 0) ||
                (vol_key == nullptr && nv == nullptr && ns->offset <= offset &&
                 offset < ns->offset + ns->size))
                m->sel.seg = (int)k;
        }
        if (seg >= 0 && m->sel.seg < 0)
            m->sel.d = nullptr;
    }
    fill_list(m);
    select_volume_row(m);
    resize_map(m);
    gtk_widget_queue_draw(m->map);
    show_selection_status(m);
    if (old != nullptr)
        g_ptr_array_unref(old);
    g_free(disk_key);
    g_free(vol_key);
}

static gboolean refresh_later(gpointer data) {
    dm *m = data;
    m->refresh_source = 0;
    /* An operation refreshes when it's done. */
    if (!m->busy)
        refresh(m);
    return G_SOURCE_REMOVE;
}

static void queue_refresh(dm *m) {
    if (m->refresh_source == 0)
        m->refresh_source = g_timeout_add(250, refresh_later, m);
}

/* Only drives and block devices are shown. Jobs, whose progress changes
 * many times a second while one runs, and RAID arrays' sync progress
 * would otherwise reread everything over and over. */
static void queue_refresh_for(dm *m, GDBusObject *o) {
    const char *path = g_dbus_object_get_object_path(o);
    if (g_str_has_prefix(path, UD_PATH "/drives/") ||
        g_str_has_prefix(path, UD_PATH "/block_devices/"))
        queue_refresh(m);
}

static void on_om_changed([[maybe_unused]] GDBusObjectManager *om, GDBusObject *o,
                          gpointer data) {
    queue_refresh_for(data, o);
}

static void on_om_iface_changed([[maybe_unused]] GDBusObjectManager *om, GDBusObject *o,
                                [[maybe_unused]] GDBusInterface *i, gpointer data) {
    queue_refresh_for(data, o);
}

static void on_om_props([[maybe_unused]] GDBusObjectManagerClient *om, GDBusObjectProxy *o,
                        [[maybe_unused]] GDBusProxy *p, [[maybe_unused]] GVariant *changed,
                        [[maybe_unused]] const char *const *invalidated, gpointer data) {
    queue_refresh_for(data, G_DBUS_OBJECT(o));
}

/* ---- the window ---------------------------------------------------------------------- */

static void on_refresh([[maybe_unused]] GtkWidget *w, gpointer m) { refresh(m); }

static void on_exit([[maybe_unused]] GtkWidget *w, gpointer data) {
    dm *m = data;
    gtk_widget_destroy(m->window);
}

/* The Action menu follows the selection, refilled whenever it (or what's
 * on the disks) changes. GTK pops a submenu up before its item's
 * "activate" handlers run, so filling it as it opened showed it empty, or
 * as it was for the last selection. Replacing the submenu instead left
 * GTK grabbing input for a menu that never showed. */
static void fill_action_menu(dm *m) {
    GtkWidget *menu = m->action_menu;
    if (menu == nullptr)
        return;
    if (gtk_widget_get_mapped(menu)) {
        /* Open: once it has closed, and the chosen item has run. */
        m->action_stale = true;
        return;
    }
    m->action_stale = false;
    GList *old = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = old; l != nullptr; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(old);
    fill_selection_menu(m, menu);
    separator(menu);
    GtkWidget *r = gtk_menu_item_new_with_mnemonic("Re_fresh");
    g_signal_connect(r, "activate", G_CALLBACK(on_refresh), m);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), r);
    gtk_widget_show_all(menu);
}

static gboolean refill_action_menu(gpointer data) {
    dm *m = data;
    if (m->action_stale)
        fill_action_menu(m);
    return G_SOURCE_REMOVE;
}

static void on_action_menu_closed([[maybe_unused]] GtkMenuShell *menu, gpointer data) {
    dm *m = data;
    if (m->action_stale)
        g_idle_add(refill_action_menu, m);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    dm *m = data;
    if (ev->keyval == GDK_KEY_F5) {
        on_refresh(w, m);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Delete && sel_volume(m) != nullptr) {
        run_action(m, delete_partition);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Menu || (ev->keyval == GDK_KEY_F10 && (ev->state & GDK_SHIFT_MASK))) {
        popup_selection_menu(m, m->map, (GdkEvent *)ev, false);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *legend_item(rgb c, const char *text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *sw = swatch(c.r, c.g, c.b);
    g_signal_connect(sw, "draw", G_CALLBACK(draw_swatch), nullptr);
    gtk_box_pack_start(GTK_BOX(box), sw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(text), FALSE, FALSE, 0);
    return box;
}

static GtkWidget *volume_list(dm *m) {
    m->store = gtk_list_store_new(NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_UINT64,
                                  G_TYPE_UINT, G_TYPE_POINTER);
    m->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(m->store));
    g_object_unref(m->store);
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
        gtk_tree_view_append_column(GTK_TREE_VIEW(m->list), col);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(m->list)), "changed",
                     G_CALLBACK(on_list_selection), m);
    g_signal_connect(m->list, "button-press-event", G_CALLBACK(on_list_button), m);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), m->list);
    return sc;
}

static void build(dm *m) {
    m->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(m->window), "Disk Management");
    gtk_window_set_icon_name(GTK_WINDOW(m->window), "drive-harddisk");
    gtk_window_set_default_size(GTK_WINDOW(m->window), 900, 620);
    g_signal_connect(m->window, "key-press-event", G_CALLBACK(on_key), m);
    g_signal_connect(m->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(m->window), vbox);

    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget *fm = gtk_menu_new();
    GtkWidget *exit_item = gtk_menu_item_new_with_mnemonic("E_xit");
    g_signal_connect(exit_item, "activate", G_CALLBACK(on_exit), m);
    gtk_menu_shell_append(GTK_MENU_SHELL(fm), exit_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fm);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file);
    GtkWidget *action = gtk_menu_item_new_with_mnemonic("_Action");
    m->action_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(action), m->action_menu);
    g_signal_connect(m->action_menu, "deactivate", G_CALLBACK(on_action_menu_closed), m);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), action);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(paned), volume_list(m), TRUE, FALSE);
    m->map = gtk_drawing_area_new();
    gtk_widget_set_can_focus(m->map, TRUE);
    gtk_widget_add_events(m->map, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(m->map, "draw", G_CALLBACK(draw_map), m);
    g_signal_connect(m->map, "button-press-event", G_CALLBACK(on_map_button), m);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sc), m->map);
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
    m->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(m->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(m->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(m->status, 6);
    gtk_widget_set_margin_bottom(m->status, 2);
    gtk_box_pack_start(GTK_BOX(vbox), m->status, FALSE, FALSE, 0);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.DiskManagement");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static dm m;
    m.sel.seg = -1;
    m.can_format = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    m.resize = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    m.asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    build(&m);
    GError *error = nullptr;
    m.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (m.bus != nullptr)
        m.om = g_dbus_object_manager_client_new_sync(
            m.bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, UD, UD_PATH, nullptr, nullptr,
            nullptr, nullptr, &error);
    if (m.om == nullptr || g_dbus_object_manager_client_get_name_owner(
                               G_DBUS_OBJECT_MANAGER_CLIENT(m.om)) == nullptr) {
        char *text = g_strdup_printf("UDisks2 isn't running, so the disks can't be read.%s%s",
                                     error != nullptr ? "\n\n" : "",
                                     error != nullptr ? error->message : "");
        g_clear_error(&error);
        gtk_widget_show_all(m.window);
        xd_message_box(GTK_WINDOW(m.window), "Disk Management", XD_MB_ERROR, XD_MB_OK, FALSE,
                       text);
        g_free(text);
    } else {
        g_signal_connect(m.om, "object-added", G_CALLBACK(on_om_changed), &m);
        g_signal_connect(m.om, "object-removed", G_CALLBACK(on_om_changed), &m);
        g_signal_connect(m.om, "interface-added", G_CALLBACK(on_om_iface_changed), &m);
        g_signal_connect(m.om, "interface-removed", G_CALLBACK(on_om_iface_changed), &m);
        g_signal_connect(m.om, "interface-proxy-properties-changed", G_CALLBACK(on_om_props), &m);
    }
    refresh(&m);
    gtk_widget_show_all(m.window);
    gtk_main();
    return EXIT_SUCCESS;
}
