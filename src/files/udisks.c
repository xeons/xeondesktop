/* SPDX-License-Identifier: MIT
 * UDisks2 for My Computer's drives. See udisks.h.
 */
#include "udisks.h"

#include <string.h>

#include "places.h"

#define UD "org.freedesktop.UDisks2"
#define UD_PATH "/org/freedesktop/UDisks2"
#define UD_BLOCK UD ".Block"
#define UD_DRIVE UD ".Drive"
#define UD_PART UD ".Partition"
#define UD_TABLE UD ".PartitionTable"

GDBusObjectManager *xd_udisks_manager(void) {
    static GDBusObjectManager *manager;
    static gboolean asked;
    if (asked)
        return manager;
    asked = TRUE;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (bus == nullptr)
        return nullptr;
    manager = g_dbus_object_manager_client_new_sync(bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                    UD, UD_PATH, nullptr, nullptr, nullptr,
                                                    nullptr, nullptr);
    g_object_unref(bus);
    if (manager != nullptr) {
        char *owner =
            g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(manager));
        if (owner == nullptr)
            g_clear_object(&manager); /* not running, and couldn't be started */
        g_free(owner);
    }
    return manager; /* kept for the process */
}

/* ---- properties ---- */

static GDBusProxy *iface_of(GDBusObject *o, const char *name) {
    GDBusInterface *i = g_dbus_object_get_interface(o, name);
    return i != nullptr ? G_DBUS_PROXY(i) : nullptr;
}

static char *prop_str(GDBusProxy *p, const char *name) {
    GVariant *v = p != nullptr ? g_dbus_proxy_get_cached_property(p, name) : nullptr;
    char *s = nullptr;
    if (v != nullptr && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
                         g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
        s = g_variant_dup_string(v, nullptr);
    else if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING))
        s = g_variant_dup_bytestring(v, nullptr);
    g_clear_pointer(&v, g_variant_unref);
    return s != nullptr ? g_strstrip(s) : g_strdup("");
}

static guint64 prop_u64(GDBusProxy *p, const char *name) {
    GVariant *v = p != nullptr ? g_dbus_proxy_get_cached_property(p, name) : nullptr;
    guint64 n = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64)
                    ? g_variant_get_uint64(v)
                    : 0;
    g_clear_pointer(&v, g_variant_unref);
    return n;
}

static gint prop_int(GDBusProxy *p, const char *name, gint fallback) {
    GVariant *v = p != nullptr ? g_dbus_proxy_get_cached_property(p, name) : nullptr;
    gint n = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_INT32) ? g_variant_get_int32(v)
                                                                           : fallback;
    g_clear_pointer(&v, g_variant_unref);
    return n;
}

static gboolean prop_bool(GDBusProxy *p, const char *name) {
    GVariant *v = p != nullptr ? g_dbus_proxy_get_cached_property(p, name) : nullptr;
    gboolean b = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
                 g_variant_get_boolean(v);
    g_clear_pointer(&v, g_variant_unref);
    return b;
}

/* An "aay" property (MountPoints, Symlinks) as strings. */
static char **prop_strv(GDBusProxy *p, const char *name) {
    GVariant *v = p != nullptr ? g_dbus_proxy_get_cached_property(p, name) : nullptr;
    char **out = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING_ARRAY)
                     ? g_variant_dup_bytestring_array(v, nullptr)
                     : nullptr;
    g_clear_pointer(&v, g_variant_unref);
    return out != nullptr ? out : g_new0(char *, 1);
}

/* ---- filesystems ---- */

void xd_udisks_fs_free(XdUdisksFs *fs) {
    if (fs == nullptr)
        return;
    g_free(fs->object);
    g_free(fs->drive);
    g_free(fs->device);
    g_free(fs->label);
    g_free(fs->type);
    g_free(fs);
}

static XdUdisksFs *fs_new(GDBusObject *o, GDBusProxy *block, GDBusProxy *fsi) {
    XdUdisksFs *fs = g_new0(XdUdisksFs, 1);
    fs->object = g_strdup(g_dbus_object_get_object_path(o));
    fs->drive = prop_str(block, "Drive");
    fs->device = prop_str(block, "Device");
    fs->label = prop_str(block, "IdLabel");
    fs->type = prop_str(block, "IdType");
    g_auto(GStrv) points = prop_strv(fsi, "MountPoints");
    fs->mounted = points[0] != nullptr;
    return fs;
}

typedef gboolean (*fs_match)(GDBusProxy *block, GDBusProxy *fsi, gconstpointer want);

static XdUdisksFs *find_fs(fs_match match, gconstpointer want) {
    GDBusObjectManager *m = xd_udisks_manager();
    if (m == nullptr)
        return nullptr;
    XdUdisksFs *found = nullptr;
    GList *objects = g_dbus_object_manager_get_objects(m);
    for (GList *l = objects; l != nullptr && found == nullptr; l = l->next) {
        GDBusProxy *block = iface_of(l->data, UD_BLOCK);
        GDBusProxy *fsi = iface_of(l->data, XD_UDISKS_FILESYSTEM);
        if (block != nullptr && fsi != nullptr && match(block, fsi, want))
            found = fs_new(l->data, block, fsi);
        g_clear_object(&block);
        g_clear_object(&fsi);
    }
    g_list_free_full(objects, g_object_unref);
    return found;
}

static gboolean mounted_at(GDBusProxy *block, GDBusProxy *fsi, gconstpointer want) {
    (void)block;
    g_auto(GStrv) points = prop_strv(fsi, "MountPoints");
    return g_strv_contains((const char *const *)points, want);
}

/* A volume's device ("/dev/sdb1", or one of its /dev/disk/by-* links)
 * or UUID. */
typedef struct volume_id {
    const char *device, *uuid;
} volume_id;

static gboolean is_volume(GDBusProxy *block, [[maybe_unused]] GDBusProxy *fsi,
                          gconstpointer want) {
    const volume_id *id = want;
    gboolean same = FALSE;
    if (id->device != nullptr) {
        g_autofree char *device = prop_str(block, "Device");
        g_autofree char *preferred = prop_str(block, "PreferredDevice");
        g_auto(GStrv) links = prop_strv(block, "Symlinks");
        same = strcmp(device, id->device) == 0 || strcmp(preferred, id->device) == 0 ||
               g_strv_contains((const char *const *)links, id->device);
    }
    if (!same && id->uuid != nullptr && id->uuid[0] != '\0') {
        g_autofree char *uuid = prop_str(block, "IdUUID");
        same = g_ascii_strcasecmp(uuid, id->uuid) == 0;
    }
    return same;
}

XdUdisksFs *xd_udisks_fs_at(const char *path) {
    return path != nullptr ? find_fs(mounted_at, path) : nullptr;
}

static XdUdisksFs *fs_of_volume(GVolume *vol) {
    g_autofree char *device = g_volume_get_identifier(vol, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
    g_autofree char *uuid = g_volume_get_uuid(vol);
    volume_id id = {device, uuid};
    return device != nullptr || uuid != nullptr ? find_fs(is_volume, &id) : nullptr;
}

XdUdisksFs *xd_udisks_fs_for(GFile *item) {
    if (item == nullptr || xd_udisks_manager() == nullptr)
        return nullptr;
    XdUdisksFs *fs = nullptr;
    if (g_file_is_native(item)) {
        g_autofree char *path = g_file_get_path(item);
        fs = xd_udisks_fs_at(path);
        if (fs == nullptr) {
            /* Mounted somewhere UDisks2 doesn't list (in another
             * namespace's view): by the mount's volume. */
            GMount *mount = xd_computer_mount(item);
            GVolume *vol = mount != nullptr ? g_mount_get_volume(mount) : nullptr;
            if (vol != nullptr)
                fs = fs_of_volume(vol);
            g_clear_object(&vol);
            g_clear_object(&mount);
        }
        return fs;
    }
    GVolume *vol = xd_computer_volume(item);
    if (vol != nullptr) {
        fs = fs_of_volume(vol);
        g_object_unref(vol);
    }
    return fs;
}

/* ---- drives ---- */

static void drive_free(gpointer p) {
    XdUdisksDrive *d = p;
    g_free(d->object);
    g_free(d->name);
    g_free(d->type);
    g_free(d->vendor);
    g_free(d->model);
    g_free(d->revision);
    g_free(d->serial);
    g_free(d->bus);
    g_free(d->device);
    g_free(d->table);
    g_free(d->sort_key);
    g_free(d);
}

static gint by_sort_key(gconstpointer a, gconstpointer b) {
    const XdUdisksDrive *x = *(XdUdisksDrive *const *)a, *y = *(XdUdisksDrive *const *)b;
    return g_strcmp0(x->sort_key, y->sort_key);
}

GPtrArray *xd_udisks_drives(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(drive_free);
    GDBusObjectManager *m = xd_udisks_manager();
    if (m == nullptr)
        return out;
    GList *objects = g_dbus_object_manager_get_objects(m);
    GHashTable *by_path = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = objects; l != nullptr; l = l->next) {
        GDBusProxy *drive = iface_of(l->data, UD_DRIVE);
        if (drive == nullptr)
            continue;
        XdUdisksDrive *d = g_new0(XdUdisksDrive, 1);
        d->object = g_strdup(g_dbus_object_get_object_path(l->data));
        d->vendor = prop_str(drive, "Vendor");
        d->model = prop_str(drive, "Model");
        d->revision = prop_str(drive, "Revision");
        d->serial = prop_str(drive, "Serial");
        d->bus = prop_str(drive, "ConnectionBus");
        d->size = prop_u64(drive, "Size");
        d->rotation = prop_int(drive, "RotationRate", -1);
        d->removable = prop_bool(drive, "Removable") || prop_bool(drive, "MediaRemovable");
        d->optical = prop_bool(drive, "Optical");
        g_auto(GStrv) compat = nullptr;
        if (!d->optical) {
            /* An empty DVD drive isn't "Optical" (that's the disc). */
            GVariant *v = g_dbus_proxy_get_cached_property(drive, "MediaCompatibility");
            if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY))
                compat = g_variant_dup_strv(v, nullptr);
            g_clear_pointer(&v, g_variant_unref);
            for (char **c = compat; c != nullptr && *c != nullptr; c++)
                d->optical = d->optical || g_str_has_prefix(*c, "optical");
        }
        d->sort_key = prop_str(drive, "SortKey");
        d->type = g_strdup(d->optical ? "DVD/CD-ROM drives" : "Disk drives");
        GString *name = g_string_new(d->vendor);
        if (d->model[0] != '\0')
            g_string_append_printf(name, "%s%s", name->len > 0 ? " " : "", d->model);
        if (name->len == 0)
            g_string_append(name, d->optical ? "CD-ROM Drive" : "Disk drive");
        /* XP names a USB disk "<model> USB Device". */
        if (strcmp(d->bus, "usb") == 0)
            g_string_append(name, " USB Device");
        d->name = g_string_free(name, FALSE);
        d->device = g_strdup("");
        d->table = g_strdup("");
        g_hash_table_insert(by_path, d->object, d);
        g_ptr_array_add(out, d);
        g_object_unref(drive);
    }
    /* Each drive's own block device: the one that isn't a partition. */
    for (GList *l = objects; l != nullptr; l = l->next) {
        GDBusProxy *block = iface_of(l->data, UD_BLOCK);
        GDBusProxy *part = iface_of(l->data, UD_PART);
        if (block != nullptr && part == nullptr) {
            g_autofree char *drive = prop_str(block, "Drive");
            XdUdisksDrive *d = g_hash_table_lookup(by_path, drive);
            if (d != nullptr && d->device[0] == '\0') {
                g_free(d->device);
                d->device = prop_str(block, "Device");
                GDBusProxy *table = iface_of(l->data, UD_TABLE);
                if (table != nullptr) {
                    g_free(d->table);
                    d->table = prop_str(table, "Type");
                    g_object_unref(table);
                }
            }
        }
        g_clear_object(&block);
        g_clear_object(&part);
    }
    g_hash_table_unref(by_path);
    g_list_free_full(objects, g_object_unref);
    g_ptr_array_sort(out, by_sort_key);
    return out;
}

static void volume_free(gpointer p) {
    XdUdisksVolume *v = p;
    g_free(v->device);
    g_free(v->label);
    g_free(v->type);
    g_free(v);
}

static gint by_device(gconstpointer a, gconstpointer b) {
    const XdUdisksVolume *x = *(XdUdisksVolume *const *)a, *y = *(XdUdisksVolume *const *)b;
    return g_strcmp0(x->device, y->device);
}

GPtrArray *xd_udisks_volumes(const char *drive_object) {
    GPtrArray *out = g_ptr_array_new_with_free_func(volume_free);
    GDBusObjectManager *m = xd_udisks_manager();
    if (m == nullptr || drive_object == nullptr)
        return out;
    GList *objects = g_dbus_object_manager_get_objects(m);
    for (GList *l = objects; l != nullptr; l = l->next) {
        GDBusProxy *block = iface_of(l->data, UD_BLOCK);
        GDBusProxy *fsi = iface_of(l->data, XD_UDISKS_FILESYSTEM);
        g_autofree char *drive = block != nullptr ? prop_str(block, "Drive") : nullptr;
        if (fsi != nullptr && g_strcmp0(drive, drive_object) == 0) {
            XdUdisksVolume *v = g_new0(XdUdisksVolume, 1);
            v->device = prop_str(block, "Device");
            v->label = prop_str(block, "IdLabel");
            v->type = prop_str(block, "IdType");
            v->size = prop_u64(block, "Size");
            g_ptr_array_add(out, v);
        }
        g_clear_object(&block);
        g_clear_object(&fsi);
    }
    g_list_free_full(objects, g_object_unref);
    g_ptr_array_sort(out, by_device);
    return out;
}

/* ---- calls ---- */

static void call_done(GObject *src, GAsyncResult *res, gpointer data) {
    GTask *task = data;
    GError *err = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r != nullptr)
        g_task_return_pointer(task, r, (GDestroyNotify)g_variant_unref);
    else
        g_task_return_error(task, err);
    g_object_unref(task);
}

void xd_udisks_call(const char *object, const char *iface, const char *method, GVariant *args,
                    GCancellable *cancel, GAsyncReadyCallback done, gpointer data) {
    GDBusObjectManager *m = xd_udisks_manager();
    GTask *task = g_task_new(nullptr, cancel, done, data);
    if (m == nullptr) {
        if (args != nullptr)
            g_variant_unref(g_variant_ref_sink(args));
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "UDisks2 isn't running.");
        g_object_unref(task);
        return;
    }
    GDBusConnection *bus =
        g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(m));
    g_dbus_connection_call(bus, UD, object, iface, method, args, nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, cancel,
                           call_done, task);
}

GVariant *xd_udisks_call_finish(GAsyncResult *res, char **dbus_error, GError **err) {
    GError *e = nullptr;
    GVariant *r = g_task_propagate_pointer(G_TASK(res), &e);
    if (dbus_error != nullptr)
        *dbus_error = e != nullptr ? g_dbus_error_get_remote_error(e) : nullptr;
    if (e != nullptr) {
        g_dbus_error_strip_remote_error(e);
        g_propagate_error(err, e);
    }
    return r;
}

gboolean xd_udisks_can(const char *what, const char *fs_type) {
    GDBusObjectManager *m = xd_udisks_manager();
    if (m == nullptr || fs_type == nullptr || fs_type[0] == '\0')
        return FALSE;
    GDBusConnection *bus =
        g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(m));
    /* Quick: UDisks2 only looks for the program it would run. */
    GVariant *r = g_dbus_connection_call_sync(bus, UD, UD_PATH "/Manager", UD ".Manager", what,
                                              g_variant_new("(s)", fs_type),
                                              G_VARIANT_TYPE("((bs))"), G_DBUS_CALL_FLAGS_NONE,
                                              5000, nullptr, nullptr);
    gboolean can = FALSE;
    if (r != nullptr) {
        g_variant_get(r, "((bs))", &can, nullptr);
        g_variant_unref(r);
    }
    return can;
}
