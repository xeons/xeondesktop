/* SPDX-License-Identifier: MIT
 * UDisks2 for My Computer's drives. See udisks.h.
 */
#include "udisks.h"

#include <string.h>

#include "luks.h"
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

static GDBusProxy *iface_of(GDBusObject *dbus_object, const char *name) {
    GDBusInterface *interface = g_dbus_object_get_interface(dbus_object, name);
    return interface != nullptr ? G_DBUS_PROXY(interface) : nullptr;
}

static char *prop_str(GDBusProxy *proxy, const char *name) {
    GVariant *variant = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
    char *text = nullptr;
    if (variant != nullptr && (g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING) ||
                               g_variant_is_of_type(variant, G_VARIANT_TYPE_OBJECT_PATH)))
        text = g_variant_dup_string(variant, nullptr);
    else if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BYTESTRING))
        text = g_variant_dup_bytestring(variant, nullptr);
    g_clear_pointer(&variant, g_variant_unref);
    return text != nullptr ? g_strstrip(text) : g_strdup("");
}

static guint64 prop_u64(GDBusProxy *proxy, const char *name) {
    GVariant *variant = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
    guint64 number = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT64)
                         ? g_variant_get_uint64(variant)
                         : 0;
    g_clear_pointer(&variant, g_variant_unref);
    return number;
}

static gint prop_int(GDBusProxy *proxy, const char *name, gint fallback) {
    GVariant *variant = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
    gint number = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_INT32)
                      ? g_variant_get_int32(variant)
                      : fallback;
    g_clear_pointer(&variant, g_variant_unref);
    return number;
}

static gboolean prop_bool(GDBusProxy *proxy, const char *name) {
    GVariant *variant = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
    gboolean flag = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BOOLEAN) &&
                    g_variant_get_boolean(variant);
    g_clear_pointer(&variant, g_variant_unref);
    return flag;
}

/* An "aay" property (MountPoints, Symlinks) as strings. */
static char **prop_strv(GDBusProxy *proxy, const char *name) {
    GVariant *variant = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
    char **out =
        variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BYTESTRING_ARRAY)
            ? g_variant_dup_bytestring_array(variant, nullptr)
            : nullptr;
    g_clear_pointer(&variant, g_variant_unref);
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

static XdUdisksFs *fs_new(GDBusObject *dbus_object, GDBusProxy *block, GDBusProxy *fsi) {
    XdUdisksFs *fs = g_new0(XdUdisksFs, 1);
    fs->object = g_strdup(g_dbus_object_get_object_path(dbus_object));
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
    GDBusObjectManager *object_manager = xd_udisks_manager();
    if (object_manager == nullptr)
        return nullptr;
    XdUdisksFs *found = nullptr;
    GList *objects = g_dbus_object_manager_get_objects(object_manager);
    for (GList *list = objects; list != nullptr && found == nullptr; list = list->next) {
        GDBusProxy *block = iface_of(list->data, UD_BLOCK);
        GDBusProxy *fsi = iface_of(list->data, XD_UDISKS_FILESYSTEM);
        if (block != nullptr && fsi != nullptr && match(block, fsi, want))
            found = fs_new(list->data, block, fsi);
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

static void drive_free(gpointer user_data) {
    XdUdisksDrive *udisks_drive = user_data;
    g_free(udisks_drive->object);
    g_free(udisks_drive->name);
    g_free(udisks_drive->type);
    g_free(udisks_drive->vendor);
    g_free(udisks_drive->model);
    g_free(udisks_drive->revision);
    g_free(udisks_drive->serial);
    g_free(udisks_drive->bus);
    g_free(udisks_drive->device);
    g_free(udisks_drive->table);
    g_free(udisks_drive->sort_key);
    g_free(udisks_drive);
}

static gint by_sort_key(gconstpointer left, gconstpointer right) {
    const XdUdisksDrive *left_drive = *(XdUdisksDrive *const *)left,
                        *right_drive = *(XdUdisksDrive *const *)right;
    return g_strcmp0(left_drive->sort_key, right_drive->sort_key);
}

GPtrArray *xd_udisks_drives(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(drive_free);
    GDBusObjectManager *object_manager = xd_udisks_manager();
    if (object_manager == nullptr)
        return out;
    GList *objects = g_dbus_object_manager_get_objects(object_manager);
    GHashTable *by_path = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *list = objects; list != nullptr; list = list->next) {
        GDBusProxy *drive = iface_of(list->data, UD_DRIVE);
        if (drive == nullptr)
            continue;
        XdUdisksDrive *udisks_drive = g_new0(XdUdisksDrive, 1);
        udisks_drive->object = g_strdup(g_dbus_object_get_object_path(list->data));
        udisks_drive->vendor = prop_str(drive, "Vendor");
        udisks_drive->model = prop_str(drive, "Model");
        udisks_drive->revision = prop_str(drive, "Revision");
        udisks_drive->serial = prop_str(drive, "Serial");
        udisks_drive->bus = prop_str(drive, "ConnectionBus");
        udisks_drive->size = prop_u64(drive, "Size");
        udisks_drive->rotation = prop_int(drive, "RotationRate", -1);
        udisks_drive->removable =
            prop_bool(drive, "Removable") || prop_bool(drive, "MediaRemovable");
        udisks_drive->optical = prop_bool(drive, "Optical");
        g_auto(GStrv) compat = nullptr;
        if (!udisks_drive->optical) {
            /* An empty DVD drive isn't "Optical" (that's the disc). */
            GVariant *variant = g_dbus_proxy_get_cached_property(drive, "MediaCompatibility");
            if (variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING_ARRAY))
                compat = g_variant_dup_strv(variant, nullptr);
            g_clear_pointer(&variant, g_variant_unref);
            for (char **compat_entry = compat; compat_entry != nullptr && *compat_entry != nullptr;
                 compat_entry++)
                udisks_drive->optical =
                    udisks_drive->optical || g_str_has_prefix(*compat_entry, "optical");
        }
        udisks_drive->sort_key = prop_str(drive, "SortKey");
        udisks_drive->type = g_strdup(udisks_drive->optical ? "DVD/CD-ROM drives" : "Disk drives");
        GString *name = g_string_new(udisks_drive->vendor);
        if (udisks_drive->model[0] != '\0')
            g_string_append_printf(name, "%s%s", name->len > 0 ? " " : "", udisks_drive->model);
        if (name->len == 0)
            g_string_append(name, udisks_drive->optical ? "CD-ROM Drive" : "Disk drive");
        /* XP names a USB disk "<model> USB Device". */
        if (strcmp(udisks_drive->bus, "usb") == 0)
            g_string_append(name, " USB Device");
        udisks_drive->name = g_string_free(name, FALSE);
        udisks_drive->device = g_strdup("");
        udisks_drive->table = g_strdup("");
        g_hash_table_insert(by_path, udisks_drive->object, udisks_drive);
        g_ptr_array_add(out, udisks_drive);
        g_object_unref(drive);
    }
    /* Each drive's own block device: the one that isn't a partition. */
    for (GList *list = objects; list != nullptr; list = list->next) {
        GDBusProxy *block = iface_of(list->data, UD_BLOCK);
        GDBusProxy *part = iface_of(list->data, UD_PART);
        if (block != nullptr && part == nullptr) {
            g_autofree char *drive = prop_str(block, "Drive");
            XdUdisksDrive *udisks_drive = g_hash_table_lookup(by_path, drive);
            if (udisks_drive != nullptr && udisks_drive->device[0] == '\0') {
                g_free(udisks_drive->device);
                udisks_drive->device = prop_str(block, "Device");
                GDBusProxy *table = iface_of(list->data, UD_TABLE);
                if (table != nullptr) {
                    g_free(udisks_drive->table);
                    udisks_drive->table = prop_str(table, "Type");
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

static void volume_free(gpointer user_data) {
    XdUdisksVolume *udisks_volume = user_data;
    g_free(udisks_volume->device);
    g_free(udisks_volume->label);
    g_free(udisks_volume->type);
    g_free(udisks_volume);
}

static gint by_device(gconstpointer left, gconstpointer right) {
    const XdUdisksVolume *left_volume = *(XdUdisksVolume *const *)left,
                         *right_volume = *(XdUdisksVolume *const *)right;
    return g_strcmp0(left_volume->device, right_volume->device);
}

GPtrArray *xd_udisks_volumes(const char *drive_object) {
    GPtrArray *out = g_ptr_array_new_with_free_func(volume_free);
    GDBusObjectManager *object_manager = xd_udisks_manager();
    if (object_manager == nullptr || drive_object == nullptr)
        return out;
    GList *objects = g_dbus_object_manager_get_objects(object_manager);
    for (GList *list = objects; list != nullptr; list = list->next) {
        GDBusProxy *block = iface_of(list->data, UD_BLOCK);
        GDBusProxy *fsi = iface_of(list->data, XD_UDISKS_FILESYSTEM);
        g_autofree char *drive = block != nullptr ? prop_str(block, "Drive") : nullptr;
        if (fsi != nullptr && g_strcmp0(drive, drive_object) == 0) {
            XdUdisksVolume *udisks_volume = g_new0(XdUdisksVolume, 1);
            udisks_volume->device = prop_str(block, "Device");
            udisks_volume->label = prop_str(block, "IdLabel");
            udisks_volume->type = prop_str(block, "IdType");
            udisks_volume->size = prop_u64(block, "Size");
            g_ptr_array_add(out, udisks_volume);
        }
        g_clear_object(&block);
        g_clear_object(&fsi);
    }
    g_list_free_full(objects, g_object_unref);
    g_ptr_array_sort(out, by_device);
    return out;
}

/* ---- encrypted volumes ---- */

void xd_udisks_crypt_free(XdUdisksCrypt *udisks_crypt) {
    if (udisks_crypt == nullptr)
        return;
    g_free(udisks_crypt->object);
    g_free(udisks_crypt->drive);
    g_free(udisks_crypt->device);
    g_free(udisks_crypt->uuid);
    g_free(udisks_crypt->label);
    g_free(udisks_crypt->version);
    g_free(udisks_crypt->cleartext);
    g_free(udisks_crypt->inner_label);
    g_free(udisks_crypt->mount_point);
    g_free(udisks_crypt);
}

static void crypt_free(gpointer user_data) { xd_udisks_crypt_free(user_data); }

static gint by_crypt_device(gconstpointer left, gconstpointer right) {
    const XdUdisksCrypt *left_crypt = *(XdUdisksCrypt *const *)left,
                        *right_crypt = *(XdUdisksCrypt *const *)right;
    return g_strcmp0(left_crypt->device, right_crypt->device);
}

GPtrArray *xd_udisks_encrypted(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(crypt_free);
    GDBusObjectManager *object_manager = xd_udisks_manager();
    if (object_manager == nullptr)
        return out;
    GList *objects = g_dbus_object_manager_get_objects(object_manager);
    for (GList *list = objects; list != nullptr; list = list->next) {
        GDBusProxy *enc = iface_of(list->data, XD_UDISKS_ENCRYPTED);
        GDBusProxy *block = iface_of(list->data, UD_BLOCK);
        if (enc != nullptr && block != nullptr) {
            XdUdisksCrypt *udisks_crypt = g_new0(XdUdisksCrypt, 1);
            udisks_crypt->object = g_strdup(g_dbus_object_get_object_path(list->data));
            udisks_crypt->drive = prop_str(block, "Drive");
            udisks_crypt->device = prop_str(block, "Device");
            udisks_crypt->uuid = prop_str(block, "IdUUID");
            udisks_crypt->label = prop_str(block, "IdLabel");
            udisks_crypt->version = prop_str(block, "IdVersion");
            udisks_crypt->size = prop_u64(block, "Size");
            udisks_crypt->ignore = prop_bool(block, "HintIgnore");
            udisks_crypt->inner_label = g_strdup("");
            GDBusInterface *drive_interface =
                g_dbus_object_manager_get_interface(object_manager, udisks_crypt->drive, UD_DRIVE);
            if (drive_interface != nullptr) {
                udisks_crypt->removable =
                    prop_bool(G_DBUS_PROXY(drive_interface), "Removable") ||
                    prop_bool(G_DBUS_PROXY(drive_interface), "MediaRemovable");
                g_object_unref(drive_interface);
            }
            g_autofree char *clear = prop_str(enc, "CleartextDevice");
            GDBusObject *co = clear[0] != '\0' && strcmp(clear, "/") != 0
                                  ? g_dbus_object_manager_get_object(object_manager, clear)
                                  : nullptr;
            if (co != nullptr) {
                udisks_crypt->cleartext = g_strdup(clear);
                GDBusProxy *cb = iface_of(co, UD_BLOCK);
                GDBusProxy *fsi = iface_of(co, XD_UDISKS_FILESYSTEM);
                udisks_crypt->has_fs = fsi != nullptr;
                g_free(udisks_crypt->inner_label);
                udisks_crypt->inner_label = prop_str(cb, "IdLabel");
                g_auto(GStrv) points = prop_strv(fsi, "MountPoints");
                udisks_crypt->mount_point = g_strdup(points[0]);
                g_clear_object(&cb);
                g_clear_object(&fsi);
                g_object_unref(co);
            }
            g_ptr_array_add(out, udisks_crypt);
        }
        g_clear_object(&enc);
        g_clear_object(&block);
    }
    g_list_free_full(objects, g_object_unref);
    g_ptr_array_sort(out, by_crypt_device);
    return out;
}

XdUdisksCrypt *xd_udisks_crypt_for(GFile *item) {
    if (item == nullptr || xd_udisks_manager() == nullptr)
        return nullptr;
    g_autofree char *uri = g_file_get_uri(item);
    g_autofree char *uuid = xd_luks_item_uuid(uri);
    g_autofree char *path = g_file_is_native(item) ? g_file_get_path(item) : nullptr;
    if (uuid == nullptr && path == nullptr)
        return nullptr;
    GPtrArray *all = xd_udisks_encrypted();
    XdUdisksCrypt *found = nullptr;
    for (guint i = 0; i < all->len && found == nullptr; i++) {
        XdUdisksCrypt *udisks_crypt = g_ptr_array_index(all, i);
        if ((uuid != nullptr && g_ascii_strcasecmp(udisks_crypt->uuid, uuid) == 0) ||
            (path != nullptr && g_strcmp0(udisks_crypt->mount_point, path) == 0))
            found = g_ptr_array_steal_index(all, i);
    }
    g_ptr_array_unref(all);
    return found;
}

gboolean xd_udisks_is_crypt_device(const char *device) {
    GDBusObjectManager *object_manager = xd_udisks_manager();
    if (object_manager == nullptr || device == nullptr)
        return FALSE;
    volume_id id = {device, nullptr};
    gboolean is = FALSE;
    GList *objects = g_dbus_object_manager_get_objects(object_manager);
    for (GList *list = objects; list != nullptr && !is; list = list->next) {
        GDBusProxy *block = iface_of(list->data, UD_BLOCK);
        if (block != nullptr && is_volume(block, nullptr, &id)) {
            GDBusProxy *enc = iface_of(list->data, XD_UDISKS_ENCRYPTED);
            g_autofree char *backing = prop_str(block, "CryptoBackingDevice");
            is = enc != nullptr || (backing[0] != '\0' && strcmp(backing, "/") != 0);
            g_clear_object(&enc);
        }
        g_clear_object(&block);
    }
    g_list_free_full(objects, g_object_unref);
    return is;
}

/* ---- calls ---- */

static void call_done(GObject *src, GAsyncResult *res, gpointer data) {
    GTask *task = data;
    GError *err = nullptr;
    GVariant *variant = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (variant != nullptr)
        g_task_return_pointer(task, variant, (GDestroyNotify)g_variant_unref);
    else
        g_task_return_error(task, err);
    g_object_unref(task);
}

void xd_udisks_call(const char *object, const char *iface, const char *method, GVariant *args,
                    GCancellable *cancel, GAsyncReadyCallback done, gpointer data) {
    GDBusObjectManager *object_manager = xd_udisks_manager();
    GTask *task = g_task_new(nullptr, cancel, done, data);
    if (object_manager == nullptr) {
        if (args != nullptr)
            g_variant_unref(g_variant_ref_sink(args));
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "UDisks2 isn't running.");
        g_object_unref(task);
        return;
    }
    GDBusConnection *bus =
        g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(object_manager));
    g_dbus_connection_call(bus, UD, object, iface, method, args, nullptr,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, cancel,
                           call_done, task);
}

GVariant *xd_udisks_call_finish(GAsyncResult *res, char **dbus_error, GError **err) {
    GError *error = nullptr;
    GVariant *variant = g_task_propagate_pointer(G_TASK(res), &error);
    if (dbus_error != nullptr)
        *dbus_error = error != nullptr ? g_dbus_error_get_remote_error(error) : nullptr;
    if (error != nullptr) {
        g_dbus_error_strip_remote_error(error);
        g_propagate_error(err, error);
    }
    return variant;
}

gboolean xd_udisks_can(const char *what, const char *fs_type) {
    GDBusObjectManager *object_manager = xd_udisks_manager();
    if (object_manager == nullptr || fs_type == nullptr || fs_type[0] == '\0')
        return FALSE;
    GDBusConnection *bus =
        g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(object_manager));
    /* Quick: UDisks2 only looks for the program it would run. */
    GVariant *variant = g_dbus_connection_call_sync(
        bus, UD, UD_PATH "/Manager", UD ".Manager", what, g_variant_new("(s)", fs_type),
        G_VARIANT_TYPE("((bs))"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    gboolean can = FALSE;
    if (variant != nullptr) {
        g_variant_get(variant, "((bs))", &can, nullptr);
        g_variant_unref(variant);
    }
    return can;
}
