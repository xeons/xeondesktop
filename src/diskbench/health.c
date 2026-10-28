/* SPDX-License-Identifier: MIT */
#include "health.h"

#include <gio/gunixmounts.h>
#include <inttypes.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "benchlib.h"

#define UDISKS "org.freedesktop.UDisks2"
#define UDISKS_PATH "/org/freedesktop/UDisks2"
#define UDISKS_DRIVE UDISKS ".Drive"
#define UDISKS_ATA UDISKS ".Drive.Ata"
#define UDISKS_NVME UDISKS ".NVMe.Controller"
#define UDISKS_BLOCK UDISKS ".Block"
#define UDISKS_FILESYSTEM UDISKS ".Filesystem"

typedef struct health_drive {
    char *object_path, *vendor, *model, *revision, *serial, *bus, *sort_key;
    guint64 size;
    int rotation_rate;
    GDBusProxy *ata, *nvme; /* the S.M.A.R.T. interfaces it has, or nullptr */
    GPtrArray *devices, *mount_points;
    /* SmartGetAttributes' answer: ATA attributes (names owned by
     * attribute_names) or the NVMe health log. */
    GArray *attributes;
    GPtrArray *attribute_names;
    GVariant *nvme_log;
    bool loaded;
    char *load_error;
    xd_health_status status;
    char *reason;
} health_drive;

enum { DRIVE_ICON, DRIVE_TEXT, DRIVE_INDEX, DRIVE_COLUMNS };
enum { ATA_ID, ATA_NAME, ATA_CURRENT, ATA_WORST, ATA_THRESHOLD, ATA_RAW, ATA_BACKGROUND,
       ATA_COLUMNS };
enum { NVME_FIELD, NVME_VALUE, NVME_COLUMNS };

struct health_tab {
    GtkWindow *window;
    GtkLabel *status;
    GtkWidget *box, *drive_combo, *status_area, *reason_label, *temperature_area,
        *details_grid, *table_frame, *table_stack;
    GtkListStore *drive_store, *ata_store, *nvme_store;
    GDBusObjectManager *manager;
    GPtrArray *drives; /* health_drive * */
    char *select_mount, *select_device;
    GCancellable *cancellable;
    char *temperature;
};

/* ---- reading UDisks2 --------------------------------------------------------------- */

static void free_drive(gpointer data) {
    health_drive *drive = data;
    g_free(drive->object_path);
    g_free(drive->vendor);
    g_free(drive->model);
    g_free(drive->revision);
    g_free(drive->serial);
    g_free(drive->bus);
    g_free(drive->sort_key);
    g_clear_object(&drive->ata);
    g_clear_object(&drive->nvme);
    g_ptr_array_unref(drive->devices);
    g_ptr_array_unref(drive->mount_points);
    g_array_unref(drive->attributes);
    g_ptr_array_unref(drive->attribute_names);
    g_clear_pointer(&drive->nvme_log, g_variant_unref);
    g_free(drive->load_error);
    g_free(drive->reason);
    g_free(drive);
}

static GVariant *get_property(GDBusProxy *proxy, const char *name) {
    return proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, name) : nullptr;
}

static char *get_string_property(GDBusProxy *proxy, const char *name) {
    g_autoptr(GVariant) value = get_property(proxy, name);
    if (value == nullptr)
        return g_strdup("");
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
        return g_strdup(g_variant_get_string(value, nullptr));
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING))
        return g_strdup(g_variant_get_bytestring(value));
    return g_strdup("");
}

/* Any of D-Bus's integer types (or a double) as a uint64; false when
 * it's something else. */
static bool get_number(GVariant *value, guint64 *number) {
    if (value == nullptr)
        return false;
    switch (g_variant_classify(value)) {
    case G_VARIANT_CLASS_BYTE:
        *number = g_variant_get_byte(value);
        return true;
    case G_VARIANT_CLASS_UINT16:
        *number = g_variant_get_uint16(value);
        return true;
    case G_VARIANT_CLASS_UINT32:
        *number = g_variant_get_uint32(value);
        return true;
    case G_VARIANT_CLASS_UINT64:
        *number = g_variant_get_uint64(value);
        return true;
    case G_VARIANT_CLASS_INT16:
        *number = (guint64)XD_MAX(g_variant_get_int16(value), 0);
        return true;
    case G_VARIANT_CLASS_INT32:
        *number = (guint64)XD_MAX(g_variant_get_int32(value), 0);
        return true;
    case G_VARIANT_CLASS_INT64:
        *number = (guint64)XD_MAX(g_variant_get_int64(value), (gint64)0);
        return true;
    case G_VARIANT_CLASS_DOUBLE:
        *number = (guint64)XD_MAX(g_variant_get_double(value), 0.0);
        return true;
    case G_VARIANT_CLASS_BOOLEAN:
    case G_VARIANT_CLASS_HANDLE:
    case G_VARIANT_CLASS_STRING:
    case G_VARIANT_CLASS_OBJECT_PATH:
    case G_VARIANT_CLASS_SIGNATURE:
    case G_VARIANT_CLASS_VARIANT:
    case G_VARIANT_CLASS_MAYBE:
    case G_VARIANT_CLASS_ARRAY:
    case G_VARIANT_CLASS_TUPLE:
    case G_VARIANT_CLASS_DICT_ENTRY:
        break;
    }
    return false;
}

static guint64 get_number_property(GDBusProxy *proxy, const char *name) {
    g_autoptr(GVariant) value = get_property(proxy, name);
    guint64 number = 0;
    return get_number(value, &number) ? number : 0;
}

static bool get_bool_property(GDBusProxy *proxy, const char *name) {
    g_autoptr(GVariant) value = get_property(proxy, name);
    return value != nullptr && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) &&
           g_variant_get_boolean(value);
}

static double get_double_property(GDBusProxy *proxy, const char *name) {
    g_autoptr(GVariant) value = get_property(proxy, name);
    if (value != nullptr && g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE))
        return g_variant_get_double(value);
    guint64 number = 0;
    return get_number(value, &number) ? (double)number : 0;
}

static GDBusProxy *get_interface(GDBusObject *object, const char *name) {
    return G_DBUS_PROXY(g_dbus_object_get_interface(object, name));
}

static int compare_drives(gconstpointer left, gconstpointer right) {
    const health_drive *first = *(health_drive *const *)left;
    const health_drive *second = *(health_drive *const *)right;
    return g_strcmp0(first->sort_key, second->sort_key);
}

static health_drive *find_drive(health_tab *tab, const char *object_path) {
    for (guint i = 0; i < tab->drives->len; i++) {
        health_drive *drive = g_ptr_array_index(tab->drives, i);
        if (strcmp(drive->object_path, object_path) == 0)
            return drive;
    }
    return nullptr;
}

/* The block devices and mount points on each drive, so --drive and --disk
 * can find it. */
static void read_blocks(health_tab *tab, GList *objects) {
    for (GList *link = objects; link != nullptr; link = link->next) {
        GDBusObject *object = link->data;
        g_autoptr(GDBusProxy) block = get_interface(object, UDISKS_BLOCK);
        if (block == nullptr)
            continue;
        g_autoptr(GVariant) drive_path = get_property(block, "Drive");
        g_autoptr(GVariant) backing = get_property(block, "CryptoBackingDevice");
        const char *path = drive_path != nullptr ? g_variant_get_string(drive_path, nullptr) : "/";
        /* An unlocked encrypted volume's drive is its backing device's. */
        if (strcmp(path, "/") == 0 && backing != nullptr &&
            strcmp(g_variant_get_string(backing, nullptr), "/") != 0) {
            g_autoptr(GDBusProxy) backing_block = G_DBUS_PROXY(g_dbus_object_manager_get_interface(
                tab->manager, g_variant_get_string(backing, nullptr), UDISKS_BLOCK));
            g_autoptr(GVariant) backing_drive = get_property(backing_block, "Drive");
            if (backing_drive != nullptr) {
                g_clear_pointer(&drive_path, g_variant_unref);
                drive_path = g_variant_ref(backing_drive);
                path = g_variant_get_string(drive_path, nullptr);
            }
        }
        health_drive *drive = find_drive(tab, path);
        if (drive == nullptr)
            continue;
        g_ptr_array_add(drive->devices, get_string_property(block, "Device"));
        g_ptr_array_add(drive->devices, get_string_property(block, "PreferredDevice"));
        g_autoptr(GDBusProxy) filesystem = get_interface(object, UDISKS_FILESYSTEM);
        g_autoptr(GVariant) mounts = get_property(filesystem, "MountPoints");
        if (mounts != nullptr) {
            g_autofree const char **points = g_variant_get_bytestring_array(mounts, nullptr);
            for (size_t i = 0; points != nullptr && points[i] != nullptr; i++)
                g_ptr_array_add(drive->mount_points, g_strdup(points[i]));
        }
    }
}

static void read_drives(health_tab *tab) {
    g_ptr_array_set_size(tab->drives, 0);
    if (tab->manager == nullptr)
        return;
    GList *objects = g_dbus_object_manager_get_objects(tab->manager);
    for (GList *link = objects; link != nullptr; link = link->next) {
        GDBusObject *object = link->data;
        g_autoptr(GDBusProxy) proxy = get_interface(object, UDISKS_DRIVE);
        /* Optical drives have nothing to say about their health. */
        if (proxy == nullptr || get_bool_property(proxy, "Optical"))
            continue;
        g_autoptr(GVariant) compatibility = get_property(proxy, "MediaCompatibility");
        bool optical_only = false;
        if (compatibility != nullptr) {
            g_autofree const char **media = g_variant_get_strv(compatibility, nullptr);
            optical_only = media != nullptr && media[0] != nullptr &&
                           g_str_has_prefix(media[0], "optical");
        }
        if (optical_only)
            continue;
        health_drive *drive = g_new0(health_drive, 1);
        drive->object_path = g_strdup(g_dbus_object_get_object_path(object));
        drive->vendor = get_string_property(proxy, "Vendor");
        drive->model = get_string_property(proxy, "Model");
        drive->revision = get_string_property(proxy, "Revision");
        drive->serial = get_string_property(proxy, "Serial");
        drive->bus = get_string_property(proxy, "ConnectionBus");
        drive->sort_key = get_string_property(proxy, "SortKey");
        drive->size = get_number_property(proxy, "Size");
        g_autoptr(GVariant) rotation = get_property(proxy, "RotationRate");
        drive->rotation_rate =
            rotation != nullptr && g_variant_is_of_type(rotation, G_VARIANT_TYPE_INT32)
                ? g_variant_get_int32(rotation)
                : -1;
        drive->ata = get_interface(object, UDISKS_ATA);
        drive->nvme = get_interface(object, UDISKS_NVME);
        drive->devices = g_ptr_array_new_with_free_func(g_free);
        drive->mount_points = g_ptr_array_new_with_free_func(g_free);
        drive->attributes = g_array_new(FALSE, TRUE, sizeof(xd_ata_attribute));
        drive->attribute_names = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(tab->drives, drive);
    }
    g_ptr_array_sort(tab->drives, compare_drives);
    read_blocks(tab, objects);
    g_list_free_full(objects, g_object_unref);
}

/* ---- health ------------------------------------------------------------------------ */

static void evaluate_drive(health_drive *drive) {
    g_clear_pointer(&drive->reason, g_free);
    if (drive->ata != nullptr) {
        xd_ata_health health = {
            .smart_supported = get_bool_property(drive->ata, "SmartSupported"),
            .smart_enabled = get_bool_property(drive->ata, "SmartEnabled"),
            .smart_failing = get_bool_property(drive->ata, "SmartFailing"),
            .attributes = (const xd_ata_attribute *)(void *)drive->attributes->data,
            .attribute_count = drive->attributes->len,
        };
        drive->status = xd_health_get_ata_status(&health, &drive->reason);
    } else if (drive->nvme != nullptr) {
        g_autoptr(GVariant) warnings = get_property(drive->nvme, "SmartCriticalWarning");
        g_autofree const char **warning_list =
            warnings != nullptr && g_variant_is_of_type(warnings, G_VARIANT_TYPE_STRING_ARRAY)
                ? g_variant_get_strv(warnings, nullptr)
                : nullptr;
        xd_nvme_health health = {
            .has_log = drive->nvme_log != nullptr,
            .critical_warnings = warning_list,
            .available_spare = -1,
            .spare_threshold = -1,
            .percent_used = -1,
        };
        guint64 number = 0;
        if (drive->nvme_log != nullptr) {
            g_autoptr(GVariant) spare =
                g_variant_lookup_value(drive->nvme_log, "avail_spare", nullptr);
            g_autoptr(GVariant) threshold =
                g_variant_lookup_value(drive->nvme_log, "spare_thresh", nullptr);
            g_autoptr(GVariant) used =
                g_variant_lookup_value(drive->nvme_log, "percent_used", nullptr);
            if (get_number(spare, &number))
                health.available_spare = (int)XD_MIN(number, 255u);
            if (get_number(threshold, &number))
                health.spare_threshold = (int)XD_MIN(number, 255u);
            if (get_number(used, &number))
                health.percent_used = (int)XD_MIN(number, 1000u);
        }
        drive->status = xd_health_get_nvme_status(&health, &drive->reason);
    } else {
        drive->status = XD_HEALTH_UNKNOWN;
        drive->reason = g_strdup("This drive doesn't report S.M.A.R.T. data. USB drives, memory "
                                 "cards and virtual disks usually don't.");
    }
    if (drive->load_error != nullptr && drive->status == XD_HEALTH_UNKNOWN) {
        g_free(drive->reason);
        drive->reason = g_strdup_printf("The drive's S.M.A.R.T. data can't be read: %s",
                                        drive->load_error);
    }
}

static void get_status_colour(xd_health_status status, double rgb[3]) {
    static const double colours[][3] = {
        [XD_HEALTH_UNKNOWN] = {0.62, 0.62, 0.62},
        [XD_HEALTH_GOOD] = {0.20, 0.55, 0.85},
        [XD_HEALTH_CAUTION] = {0.96, 0.78, 0.10},
        [XD_HEALTH_BAD] = {0.85, 0.18, 0.15},
    };
    memcpy(rgb, colours[status], sizeof(colours[status]));
}

/* The picker's coloured dot. */
static GdkPixbuf *new_status_dot(xd_health_status status) {
    static constexpr int SIZE = 14;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SIZE, SIZE);
    cairo_t *context = cairo_create(surface);
    double rgb[3];
    get_status_colour(status, rgb);
    cairo_arc(context, SIZE / 2.0, SIZE / 2.0, SIZE / 2.0 - 1.5, 0, 2 * XD_PI);
    cairo_set_source_rgb(context, rgb[0], rgb[1], rgb[2]);
    cairo_fill_preserve(context);
    cairo_set_source_rgba(context, 0, 0, 0, 0.45);
    cairo_set_line_width(context, 1);
    cairo_stroke(context);
    cairo_destroy(context);
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, SIZE, SIZE);
    cairo_surface_destroy(surface);
    return pixbuf;
}

static char *get_drive_name(const health_drive *drive) {
    g_autofree char *size = xd_health_format_data_size(drive->size);
    const char *model = drive->model[0] != '\0' ? drive->model : "Disk";
    if (drive->vendor[0] != '\0' && !g_str_has_prefix(model, drive->vendor))
        return g_strdup_printf("%s %s (%s)", drive->vendor, model, size);
    return g_strdup_printf("%s (%s)", model, size);
}

static health_drive *get_selected_drive(health_tab *tab) {
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(tab->drive_combo), &iter))
        return nullptr;
    guint index = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(tab->drive_store), &iter, DRIVE_INDEX, &index, -1);
    return index < tab->drives->len ? g_ptr_array_index(tab->drives, index) : nullptr;
}

/* ---- what the tab shows ------------------------------------------------------------ */

typedef struct detail {
    char *label, *value;
} detail;

static void add_detail(GArray *details, const char *label, char *value) {
    detail item = {g_strdup(label), value};
    g_array_append_val(details, item);
}

static void clear_detail(gpointer data) {
    detail *item = data;
    g_free(item->label);
    g_free(item->value);
}

static const xd_ata_attribute *find_attribute(const health_drive *drive, uint8_t id) {
    for (guint i = 0; i < drive->attributes->len; i++) {
        const xd_ata_attribute *attribute =
            &g_array_index(drive->attributes, xd_ata_attribute, i);
        if (attribute->id == id)
            return attribute;
    }
    return nullptr;
}

static bool lookup_log_number(const health_drive *drive, const char *key, guint64 *number) {
    if (drive->nvme_log == nullptr)
        return false;
    g_autoptr(GVariant) value = g_variant_lookup_value(drive->nvme_log, key, nullptr);
    return get_number(value, number);
}

static char *get_interface_name(const health_drive *drive) {
    if (drive->nvme != nullptr)
        return g_strdup("NVMe");
    static const struct {
        const char *bus, *name;
    } buses[] = {{"usb", "USB"}, {"sdio", "SD card reader"}, {"ieee1394", "FireWire"}};
    for (size_t i = 0; i < XD_ARRAY_LEN(buses); i++)
        if (strcmp(drive->bus, buses[i].bus) == 0)
            return g_strdup_printf(drive->ata != nullptr ? "%s (ATA)" : "%s", buses[i].name);
    return g_strdup(drive->ata != nullptr ? "ATA" : "Unknown");
}

static void add_feature(GString *features, const char *name, bool enabled) {
    if (features->len > 0)
        g_string_append(features, ", ");
    g_string_append(features, name);
    if (!enabled)
        g_string_append(features, " (off)");
}

/* What UDisks2 knows the drive can do; it doesn't say whether it has TRIM
 * or NCQ. */
static char *get_features(const health_drive *drive) {
    GString *features = g_string_new("");
    if (drive->ata != nullptr) {
        static const char *const known[][3] = {
            {"S.M.A.R.T.", "SmartSupported", "SmartEnabled"},
            {"APM", "ApmSupported", "ApmEnabled"},
            {"AAM", "AamSupported", "AamEnabled"},
            {"write cache", "WriteCacheSupported", "WriteCacheEnabled"},
        };
        for (size_t i = 0; i < XD_ARRAY_LEN(known); i++)
            if (get_bool_property(drive->ata, known[i][1]))
                add_feature(features, known[i][0], get_bool_property(drive->ata, known[i][2]));
    } else if (drive->nvme != nullptr) {
        add_feature(features, "S.M.A.R.T.", true);
    }
    if (features->len == 0)
        g_string_append(features, "None known");
    return g_string_free(features, FALSE);
}

/* The Details panel's rows. */
static GArray *collect_details(const health_drive *drive) {
    GArray *details = g_array_new(FALSE, TRUE, sizeof(detail));
    g_array_set_clear_func(details, clear_detail);
    g_autofree char *model = drive->vendor[0] != '\0'
                                 ? g_strdup_printf("%s %s", drive->vendor, drive->model)
                                 : g_strdup(drive->model);
    add_detail(details, "Model:", g_steal_pointer(&model));
    add_detail(details, "Firmware:", g_strdup(drive->revision));
    add_detail(details, "Serial number:", g_strdup(drive->serial));
    add_detail(details, "Capacity:", xd_health_format_data_size(drive->size));
    add_detail(details, "Interface:", get_interface_name(drive));
    add_detail(details, "Rotation rate:",
               drive->rotation_rate == 0  ? g_strdup("SSD")
               : drive->rotation_rate > 0 ? g_strdup_printf("%d RPM", drive->rotation_rate)
                                          : g_strdup("Unknown"));
    add_detail(details, "Features:", get_features(drive));

    guint64 hours = 0, count = 0, reads = 0, writes = 0;
    bool has_hours = false, has_count = false, has_reads = false, has_writes = false;
    if (drive->ata != nullptr) {
        guint64 seconds = get_number_property(drive->ata, "SmartPowerOnSeconds");
        has_hours = seconds > 0;
        hours = seconds / 3600;
        const xd_ata_attribute *cycles = find_attribute(drive, 0x0C);
        has_count = cycles != nullptr;
        count = cycles != nullptr ? (guint64)XD_MAX(cycles->raw, (int64_t)0) : 0;
        /* F1 and F2 count host writes and reads; only when UDisks2 says
         * they're sectors is their size known (512 bytes). */
        const xd_ata_attribute *written = find_attribute(drive, 0xF1);
        const xd_ata_attribute *read = find_attribute(drive, 0xF2);
        has_writes = written != nullptr && written->raw_unit == 3;
        has_reads = read != nullptr && read->raw_unit == 3;
        writes = has_writes ? (guint64)written->raw * 512 : 0;
        reads = has_reads ? (guint64)read->raw * 512 : 0;
    } else if (drive->nvme != nullptr) {
        hours = get_number_property(drive->nvme, "SmartPowerOnHours");
        has_hours = drive->nvme_log != nullptr;
        has_count = lookup_log_number(drive, "power_cycles", &count);
        has_reads = lookup_log_number(drive, "total_data_read", &reads);
        has_writes = lookup_log_number(drive, "total_data_written", &writes);
    }
    add_detail(details, "Power-on hours:",
               has_hours ? g_strdup_printf("%" G_GUINT64_FORMAT " hours", hours)
                         : g_strdup("Unknown"));
    add_detail(details, "Power-on count:",
               has_count ? g_strdup_printf("%" G_GUINT64_FORMAT " times", count)
                         : g_strdup("Unknown"));
    if (has_reads)
        add_detail(details, "Host reads:", xd_health_format_data_size(reads));
    if (has_writes)
        add_detail(details, "Host writes:", xd_health_format_data_size(writes));
    return details;
}

static char *get_temperature(const health_drive *drive) {
    if (drive->ata != nullptr && get_bool_property(drive->ata, "SmartSupported"))
        return xd_health_format_kelvin(get_double_property(drive->ata, "SmartTemperature"));
    if (drive->nvme != nullptr && drive->nvme_log != nullptr) {
        guint64 kelvin = get_number_property(drive->nvme, "SmartTemperature");
        return xd_health_format_kelvin((double)kelvin);
    }
    return g_strdup("");
}

static char *format_minutes(guint64 minutes) {
    return g_strdup_printf("%" G_GUINT64_FORMAT " minute%s", minutes, minutes == 1 ? "" : "s");
}

/* The NVMe health log's rows, in the order the specification has them. */
static GArray *collect_nvme_log(const health_drive *drive) {
    GArray *rows = g_array_new(FALSE, TRUE, sizeof(detail));
    g_array_set_clear_func(rows, clear_detail);
    if (drive->nvme_log == nullptr)
        return rows;
    g_autoptr(GVariant) warnings = get_property(drive->nvme, "SmartCriticalWarning");
    g_autofree const char **warning_list =
        warnings != nullptr && g_variant_is_of_type(warnings, G_VARIANT_TYPE_STRING_ARRAY)
            ? g_variant_get_strv(warnings, nullptr)
            : nullptr;
    GString *critical = g_string_new("");
    for (size_t i = 0; warning_list != nullptr && warning_list[i] != nullptr; i++) {
        if (i > 0)
            g_string_append(critical, "; ");
        g_string_append(critical, xd_health_describe_critical_warning(warning_list[i]));
    }
    if (critical->len == 0)
        g_string_append(critical, "None");
    add_detail(rows, "Critical warning", g_string_free(critical, FALSE));
    guint64 kelvin = get_number_property(drive->nvme, "SmartTemperature");
    add_detail(rows, "Composite temperature", xd_health_format_kelvin((double)kelvin));
    static const struct {
        const char *key, *label;
        char kind; /* % percent, b bytes, m minutes, n a count */
    } fields[] = {
        {"avail_spare", "Available spare", '%'},
        {"spare_thresh", "Available spare threshold", '%'},
        {"percent_used", "Percentage used", '%'},
        {"total_data_read", "Data read", 'b'},
        {"total_data_written", "Data written", 'b'},
        {"ctrl_busy_time", "Controller busy time", 'm'},
        {"power_cycles", "Power cycles", 'n'},
        {"", "Power-on hours", 'h'},
        {"unsafe_shutdowns", "Unsafe shutdowns", 'n'},
        {"media_errors", "Media and data integrity errors", 'n'},
        {"num_err_log_entries", "Error log entries", 'n'},
        {"warning_temp_time", "Warning temperature time", 'm'},
        {"critical_temp_time", "Critical temperature time", 'm'},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(fields); i++) {
        guint64 number = 0;
        if (fields[i].kind == 'h') {
            number = get_number_property(drive->nvme, "SmartPowerOnHours");
            add_detail(rows, fields[i].label,
                       g_strdup_printf("%" G_GUINT64_FORMAT " hours", number));
            continue;
        }
        if (!lookup_log_number(drive, fields[i].key, &number))
            continue;
        switch (fields[i].kind) {
        case '%':
            add_detail(rows, fields[i].label, g_strdup_printf("%" G_GUINT64_FORMAT "%%", number));
            break;
        case 'b':
            add_detail(rows, fields[i].label, xd_health_format_data_size(number));
            break;
        case 'm':
            add_detail(rows, fields[i].label, format_minutes(number));
            break;
        default:
            add_detail(rows, fields[i].label, g_strdup_printf("%" G_GUINT64_FORMAT, number));
            break;
        }
    }
    return rows;
}

static char *format_attribute_value(int value) {
    return value >= 0 ? g_strdup_printf("%d", value) : g_strdup("");
}

static void fill_ata_store(health_tab *tab, const health_drive *drive) {
    gtk_list_store_clear(tab->ata_store);
    for (guint i = 0; i < drive->attributes->len; i++) {
        const xd_ata_attribute *attribute =
            &g_array_index(drive->attributes, xd_ata_attribute, i);
        g_autofree char *id = g_strdup_printf("%02X", attribute->id);
        g_autofree char *name = xd_health_format_attribute_name(attribute->name);
        g_autofree char *current = format_attribute_value(attribute->current);
        g_autofree char *worst = format_attribute_value(attribute->worst);
        g_autofree char *threshold = format_attribute_value(attribute->threshold);
        g_autofree char *raw = xd_health_format_raw(attribute->raw, attribute->raw_unit);
        xd_attribute_mark mark = xd_health_get_attribute_mark(attribute);
        const char *background = mark == XD_ATTRIBUTE_BAD       ? "#ff9e9e"
                                 : mark == XD_ATTRIBUTE_CAUTION ? "#fff08a"
                                                                : nullptr;
        gtk_list_store_insert_with_values(tab->ata_store, nullptr, -1, ATA_ID, id, ATA_NAME, name,
                                          ATA_CURRENT, current, ATA_WORST, worst, ATA_THRESHOLD,
                                          threshold, ATA_RAW, raw, ATA_BACKGROUND, background, -1);
    }
}

static GtkWidget *new_detail_label(const char *text, bool is_value) {
    GtkWidget *label = gtk_label_new(text);
    if (is_value) {
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_widget_set_can_focus(label, FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    }
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    return label;
}

static void destroy_child(GtkWidget *widget, [[maybe_unused]] gpointer data) {
    gtk_widget_destroy(widget);
}

static void show_drive(health_tab *tab) {
    health_drive *drive = get_selected_drive(tab);
    gtk_container_foreach(GTK_CONTAINER(tab->details_grid), destroy_child, nullptr);
    gtk_list_store_clear(tab->ata_store);
    gtk_list_store_clear(tab->nvme_store);
    g_free(tab->temperature);
    tab->temperature = drive != nullptr ? get_temperature(drive) : g_strdup("");
    gtk_widget_queue_draw(tab->status_area);
    gtk_widget_queue_draw(tab->temperature_area);
    if (drive == nullptr) {
        gtk_label_set_text(GTK_LABEL(tab->reason_label), "UDisks2 reports no drives.");
        gtk_widget_hide(tab->table_frame);
        return;
    }
    gtk_label_set_text(GTK_LABEL(tab->reason_label),
                       drive->loaded ? drive->reason : "Reading the drive's S.M.A.R.T. data...");
    g_autoptr(GArray) details = collect_details(drive);
    for (guint i = 0; i < details->len; i++) {
        const detail *item = &g_array_index(details, detail, i);
        gtk_grid_attach(GTK_GRID(tab->details_grid), new_detail_label(item->label, false), 0,
                        (int)i, 1, 1);
        GtkWidget *value = new_detail_label(item->value, true);
        gtk_widget_set_hexpand(value, TRUE);
        gtk_grid_attach(GTK_GRID(tab->details_grid), value, 1, (int)i, 1, 1);
    }
    gtk_widget_show_all(tab->details_grid);
    if (drive->ata != nullptr && drive->attributes->len > 0) {
        fill_ata_store(tab, drive);
        gtk_frame_set_label(GTK_FRAME(tab->table_frame), "S.M.A.R.T. attributes");
        gtk_stack_set_visible_child_name(GTK_STACK(tab->table_stack), "ata");
        gtk_widget_show(tab->table_frame);
    } else if (drive->nvme != nullptr && drive->nvme_log != nullptr) {
        g_autoptr(GArray) rows = collect_nvme_log(drive);
        for (guint i = 0; i < rows->len; i++) {
            const detail *item = &g_array_index(rows, detail, i);
            gtk_list_store_insert_with_values(tab->nvme_store, nullptr, -1, NVME_FIELD,
                                              item->label, NVME_VALUE, item->value, -1);
        }
        gtk_frame_set_label(GTK_FRAME(tab->table_frame), "Health log");
        gtk_stack_set_visible_child_name(GTK_STACK(tab->table_stack), "nvme");
        gtk_widget_show(tab->table_frame);
    } else {
        gtk_widget_hide(tab->table_frame);
    }
}

static void update_drive_row(health_tab *tab, guint index) {
    GtkTreeIter iter;
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(tab->drive_store), &iter, nullptr,
                                       (int)index))
        return;
    const health_drive *drive = g_ptr_array_index(tab->drives, index);
    g_autoptr(GdkPixbuf) dot = new_status_dot(drive->loaded ? drive->status : XD_HEALTH_UNKNOWN);
    gtk_list_store_set(tab->drive_store, &iter, DRIVE_ICON, dot, -1);
}

/* ---- SmartGetAttributes ------------------------------------------------------------ */

typedef struct load_request {
    health_tab *tab;
    char *object_path;
} load_request;

static void free_load_request(load_request *request) {
    g_free(request->object_path);
    g_free(request);
}

static void read_ata_attributes(health_drive *drive, GVariant *reply) {
    g_array_set_size(drive->attributes, 0);
    g_ptr_array_set_size(drive->attribute_names, 0);
    g_autoptr(GVariant) list = g_variant_get_child_value(reply, 0);
    GVariantIter iter;
    g_variant_iter_init(&iter, list);
    guchar id;
    const char *name;
    guint16 flags;
    gint32 current, worst, threshold, unit;
    gint64 pretty;
    GVariant *expansion;
    while (g_variant_iter_next(&iter, "(y&sqiiixi@a{sv})", &id, &name, &flags, &current, &worst,
                               &threshold, &pretty, &unit, &expansion)) {
        char *owned = g_strdup(name);
        g_ptr_array_add(drive->attribute_names, owned);
        xd_ata_attribute attribute = {
            .id = id,
            .name = owned,
            .current = current,
            .worst = worst,
            .threshold = threshold,
            .raw = pretty,
            .raw_unit = unit,
        };
        g_array_append_val(drive->attributes, attribute);
        g_variant_unref(expansion);
        (void)flags;
    }
}

static void on_attributes(GObject *source, GAsyncResult *result, gpointer data) {
    load_request *request = data;
    GError *error = nullptr;
    g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        free_load_request(request);
        return;
    }
    health_tab *tab = request->tab;
    health_drive *drive = find_drive(tab, request->object_path);
    free_load_request(request);
    if (drive == nullptr) {
        g_clear_error(&error);
        return;
    }
    g_clear_pointer(&drive->load_error, g_free);
    if (error != nullptr) {
        g_dbus_error_strip_remote_error(error);
        drive->load_error = g_strdup(error->message);
        g_error_free(error);
    } else if (drive->ata != nullptr) {
        read_ata_attributes(drive, reply);
    } else {
        g_clear_pointer(&drive->nvme_log, g_variant_unref);
        drive->nvme_log = g_variant_get_child_value(reply, 0);
    }
    drive->loaded = true;
    evaluate_drive(drive);
    for (guint i = 0; i < tab->drives->len; i++)
        if (g_ptr_array_index(tab->drives, i) == drive)
            update_drive_row(tab, i);
    if (get_selected_drive(tab) == drive)
        show_drive(tab);
}

static void load_drive(health_tab *tab, health_drive *drive) {
    GDBusProxy *proxy = drive->ata != nullptr ? drive->ata : drive->nvme;
    /* Nothing to ask of a drive without S.M.A.R.T., or with it off. */
    if (proxy == nullptr || (drive->ata != nullptr &&
                             (!get_bool_property(drive->ata, "SmartSupported") ||
                              !get_bool_property(drive->ata, "SmartEnabled")))) {
        drive->loaded = true;
        evaluate_drive(drive);
        return;
    }
    load_request *request = g_new0(load_request, 1);
    request->tab = tab;
    request->object_path = g_strdup(drive->object_path);
    g_dbus_proxy_call(proxy, "SmartGetAttributes", g_variant_new("(a{sv})", nullptr),
                      G_DBUS_CALL_FLAGS_NONE, -1, tab->cancellable, on_attributes, request);
}

/* ---- the drive picker -------------------------------------------------------------- */

static bool drive_has(const GPtrArray *list, const char *text) {
    for (guint i = 0; i < list->len; i++)
        if (strcmp(g_ptr_array_index(list, i), text) == 0)
            return true;
    return false;
}

/* The drive to select first: the one --disk or --drive names. */
static guint get_initial_drive(health_tab *tab) {
    g_autofree char *mount_point = nullptr;
    if (tab->select_mount != nullptr) {
        g_autoptr(GUnixMountEntry) mount = g_unix_mount_entry_for(tab->select_mount, nullptr);
        mount_point = g_strdup(mount != nullptr ? g_unix_mount_entry_get_mount_path(mount)
                                                : tab->select_mount);
    }
    for (guint i = 0; i < tab->drives->len; i++) {
        const health_drive *drive = g_ptr_array_index(tab->drives, i);
        if (tab->select_device != nullptr && drive_has(drive->devices, tab->select_device))
            return i;
        if (tab->select_mount != nullptr &&
            (drive_has(drive->mount_points, tab->select_mount) ||
             drive_has(drive->mount_points, mount_point)))
            return i;
    }
    return 0;
}

static void fill_drive_store(health_tab *tab) {
    gtk_list_store_clear(tab->drive_store);
    for (guint i = 0; i < tab->drives->len; i++) {
        health_drive *drive = g_ptr_array_index(tab->drives, i);
        g_autofree char *name = get_drive_name(drive);
        g_autoptr(GdkPixbuf) dot = new_status_dot(XD_HEALTH_UNKNOWN);
        gtk_list_store_insert_with_values(tab->drive_store, nullptr, -1, DRIVE_ICON, dot,
                                          DRIVE_TEXT, name, DRIVE_INDEX, i, -1);
    }
    if (tab->drives->len > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(tab->drive_combo), (int)get_initial_drive(tab));
    for (guint i = 0; i < tab->drives->len; i++) {
        load_drive(tab, g_ptr_array_index(tab->drives, i));
        update_drive_row(tab, i);
    }
    show_drive(tab);
}

static void on_drive_changed([[maybe_unused]] GtkComboBox *combo, gpointer data) {
    show_drive(data);
}

/* ---- Refresh ----------------------------------------------------------------------- */

static void on_smart_updated(GObject *source, GAsyncResult *result, gpointer data) {
    load_request *request = data;
    GError *error = nullptr;
    g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        free_load_request(request);
        return;
    }
    health_tab *tab = request->tab;
    health_drive *drive = find_drive(tab, request->object_path);
    free_load_request(request);
    if (error != nullptr) {
        g_autofree char *remote = g_dbus_error_get_remote_error(error);
        g_dbus_error_strip_remote_error(error);
        g_autofree char *text =
            remote != nullptr && strstr(remote, "NotAuthorized") != nullptr
                ? g_strdup("You aren't allowed to read this drive's S.M.A.R.T. data again. "
                           "Ask an administrator, or try again later: the system reads it by "
                           "itself every few minutes.")
                : g_strdup_printf("The drive's S.M.A.R.T. data can't be read again: %s",
                                  error->message);
        g_error_free(error);
        gtk_label_set_text(tab->status, "Refreshing failed.");
        xd_message_box(tab->window, "Disk Benchmark and Health", XD_MB_ERROR, XD_MB_OK, FALSE,
                       text);
        return;
    }
    gtk_label_set_text(tab->status, "Refreshed.");
    if (drive != nullptr)
        load_drive(tab, drive);
}

void health_refresh(health_tab *tab) {
    health_drive *drive = get_selected_drive(tab);
    if (drive == nullptr)
        return;
    GDBusProxy *proxy = drive->ata != nullptr ? drive->ata : drive->nvme;
    if (proxy == nullptr) {
        gtk_label_set_text(tab->status, "This drive has no S.M.A.R.T. data to refresh.");
        return;
    }
    gtk_label_set_text(tab->status, "Reading the drive's S.M.A.R.T. data...");
    load_request *request = g_new0(load_request, 1);
    request->tab = tab;
    request->object_path = g_strdup(drive->object_path);
    g_dbus_proxy_call(proxy, "SmartUpdate", g_variant_new("(a{sv})", nullptr),
                      G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1, tab->cancellable,
                      on_smart_updated, request);
}

/* ---- Copy -------------------------------------------------------------------------- */

char *health_format_text(health_tab *tab) {
    health_drive *drive = get_selected_drive(tab);
    if (drive == nullptr)
        return g_strdup("Disk Benchmark and Health: no drives\n");
    GString *text = g_string_new("Disk Benchmark and Health: drive health\n");
    g_autofree char *name = get_drive_name(drive);
    g_string_append_printf(text, "Drive: %s\n", name);
    g_string_append_printf(text, "Health status: %s\n",
                           xd_health_get_status_label(drive->loaded ? drive->status
                                                                    : XD_HEALTH_UNKNOWN));
    if (!drive->loaded)
        g_string_append(text, "Reading the drive's S.M.A.R.T. data...\n");
    else if (drive->reason != nullptr)
        g_string_append_printf(text, "%s\n", drive->reason);
    g_autofree char *temperature = get_temperature(drive);
    g_string_append_printf(text, "Temperature: %s\n\n",
                           temperature[0] != '\0' ? temperature : "Unknown");
    g_autoptr(GArray) details = collect_details(drive);
    for (guint i = 0; i < details->len; i++) {
        const detail *item = &g_array_index(details, detail, i);
        g_string_append_printf(text, "%s %s\n", item->label, item->value);
    }
    if (drive->ata != nullptr && drive->attributes->len > 0) {
        g_string_append(text, "\nS.M.A.R.T. attributes\nID\tAttribute name\tCurrent\tWorst\t"
                              "Threshold\tRaw value\n");
        for (guint i = 0; i < drive->attributes->len; i++) {
            const xd_ata_attribute *attribute =
                &g_array_index(drive->attributes, xd_ata_attribute, i);
            g_autofree char *attribute_name = xd_health_format_attribute_name(attribute->name);
            g_autofree char *current = format_attribute_value(attribute->current);
            g_autofree char *worst = format_attribute_value(attribute->worst);
            g_autofree char *threshold = format_attribute_value(attribute->threshold);
            g_autofree char *raw = xd_health_format_raw(attribute->raw, attribute->raw_unit);
            g_string_append_printf(text, "%02X\t%s\t%s\t%s\t%s\t%s\n", attribute->id,
                                   attribute_name, current, worst, threshold, raw);
        }
    } else if (drive->nvme != nullptr && drive->nvme_log != nullptr) {
        g_string_append(text, "\nHealth log\n");
        g_autoptr(GArray) rows = collect_nvme_log(drive);
        for (guint i = 0; i < rows->len; i++) {
            const detail *item = &g_array_index(rows, detail, i);
            g_string_append_printf(text, "%s: %s\n", item->label, item->value);
        }
    }
    return g_string_free(text, FALSE);
}

/* ---- the tab ----------------------------------------------------------------------- */

/* A panel's big rounded box with a word in it: the status in its colour,
 * or the temperature. */
static void draw_box(GtkWidget *widget, cairo_t *context, const double rgb[3], const char *text,
                     bool dark_text) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    double radius = 6;
    cairo_new_sub_path(context);
    cairo_arc(context, width - radius - 0.5, radius + 0.5, radius, -XD_PI / 2, 0);
    cairo_arc(context, width - radius - 0.5, height - radius - 0.5, radius, 0, XD_PI / 2);
    cairo_arc(context, radius + 0.5, height - radius - 0.5, radius, XD_PI / 2, XD_PI);
    cairo_arc(context, radius + 0.5, radius + 0.5, radius, XD_PI, 3 * XD_PI / 2);
    cairo_close_path(context);
    cairo_set_source_rgb(context, rgb[0], rgb[1], rgb[2]);
    cairo_fill_preserve(context);
    cairo_set_source_rgba(context, 0, 0, 0, 0.4);
    cairo_set_line_width(context, 1);
    cairo_stroke(context);
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, nullptr);
    g_autofree char *markup =
        g_markup_printf_escaped("<span size='xx-large' weight='bold'>%s</span>", text);
    pango_layout_set_markup(layout, markup, -1);
    int text_width = 0, text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    cairo_move_to(context, (width - text_width) / 2.0, (height - text_height) / 2.0);
    if (dark_text)
        cairo_set_source_rgb(context, 0.1, 0.1, 0.1);
    else
        cairo_set_source_rgb(context, 1, 1, 1);
    pango_cairo_show_layout(context, layout);
    g_object_unref(layout);
}

static gboolean on_draw_status(GtkWidget *widget, cairo_t *context, gpointer data) {
    health_tab *tab = data;
    health_drive *drive = get_selected_drive(tab);
    xd_health_status status =
        drive != nullptr && drive->loaded ? drive->status : XD_HEALTH_UNKNOWN;
    double rgb[3];
    get_status_colour(status, rgb);
    draw_box(widget, context, rgb, xd_health_get_status_label(status),
             status == XD_HEALTH_CAUTION);
    return FALSE;
}

static gboolean on_draw_temperature(GtkWidget *widget, cairo_t *context, gpointer data) {
    health_tab *tab = data;
    static const double white[3] = {0.98, 0.98, 0.98};
    const char *text = tab->temperature != nullptr && tab->temperature[0] != '\0'
                           ? tab->temperature
                           : "-- °C";
    draw_box(widget, context, white, text, true);
    return FALSE;
}

static GtkWidget *new_group(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

static GtkWidget *new_ata_view(health_tab *tab) {
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tab->ata_store));
    static const struct {
        const char *title;
        int column, width;
        float xalign;
    } columns[] = {
        {"ID", ATA_ID, 40, 0},
        {"Attribute name", ATA_NAME, 220, 0},
        {"Current", ATA_CURRENT, 60, 1},
        {"Worst", ATA_WORST, 55, 1},
        {"Threshold", ATA_THRESHOLD, 70, 1},
        {"Raw value", ATA_RAW, 110, 1},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(columns); i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "xalign", (double)columns[i].xalign, nullptr);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
            columns[i].title, renderer, "text", columns[i].column, "cell-background",
            ATA_BACKGROUND, nullptr);
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(column, columns[i].width);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    }
    return view;
}

static GtkWidget *new_nvme_view(health_tab *tab) {
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tab->nvme_store));
    const char *const titles[] = {"Field", "Value"};
    for (int i = 0; i < NVME_COLUMNS; i++) {
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
            titles[i], gtk_cell_renderer_text_new(), "text", i, nullptr);
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(column, i == 0 ? 260 : 200);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    }
    return view;
}

static GtkWidget *new_scrolled(GtkWidget *child) {
    GtkWidget *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), child);
    return scrolled;
}

static void build_tab(health_tab *tab) {
    tab->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(tab->box), 10);

    tab->drive_store = gtk_list_store_new(DRIVE_COLUMNS, GDK_TYPE_PIXBUF, G_TYPE_STRING,
                                          G_TYPE_UINT);
    tab->drive_combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(tab->drive_store));
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(tab->drive_combo), icon, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(tab->drive_combo), icon, "pixbuf", DRIVE_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(tab->drive_combo), text, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(tab->drive_combo), text, "text", DRIVE_TEXT);
    gtk_widget_set_hexpand(tab->drive_combo, TRUE);
    g_signal_connect(tab->drive_combo, "changed", G_CALLBACK(on_drive_changed), tab);
    GtkWidget *picker = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("D_rive:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), tab->drive_combo);
    gtk_box_pack_start(GTK_BOX(picker), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(picker), tab->drive_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab->box), picker, FALSE, FALSE, 0);

    GtkWidget *panels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *health = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    tab->status_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(tab->status_area, 170, 56);
    g_signal_connect(tab->status_area, "draw", G_CALLBACK(on_draw_status), tab);
    tab->reason_label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(tab->reason_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(tab->reason_label), 22);
    gtk_label_set_xalign(GTK_LABEL(tab->reason_label), 0);
    GtkWidget *temperature_label = gtk_label_new("Temperature");
    gtk_label_set_xalign(GTK_LABEL(temperature_label), 0);
    tab->temperature_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(tab->temperature_area, 170, 44);
    g_signal_connect(tab->temperature_area, "draw", G_CALLBACK(on_draw_temperature), tab);
    gtk_box_pack_start(GTK_BOX(health), tab->status_area, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(health), tab->reason_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(health), temperature_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(health), tab->temperature_area, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panels), new_group("Health status", health), FALSE, FALSE, 0);
    tab->details_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(tab->details_grid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(tab->details_grid), 12);
    gtk_box_pack_start(GTK_BOX(panels), new_group("Details", tab->details_grid), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab->box), panels, FALSE, FALSE, 0);

    tab->ata_store = gtk_list_store_new(ATA_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_STRING);
    tab->nvme_store = gtk_list_store_new(NVME_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);
    tab->table_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(tab->table_stack), new_scrolled(new_ata_view(tab)), "ata");
    gtk_stack_add_named(GTK_STACK(tab->table_stack), new_scrolled(new_nvme_view(tab)), "nvme");
    tab->table_frame = gtk_frame_new("S.M.A.R.T. attributes");
    gtk_container_set_border_width(GTK_CONTAINER(tab->table_stack), 6);
    gtk_container_add(GTK_CONTAINER(tab->table_frame), tab->table_stack);
    gtk_widget_set_no_show_all(tab->table_frame, TRUE);
    gtk_widget_show_all(tab->table_stack);
    gtk_box_pack_start(GTK_BOX(tab->box), tab->table_frame, TRUE, TRUE, 0);
}

health_tab *health_init_tab(GtkWindow *window, GtkLabel *status, const char *mount_point,
                            const char *device) {
    health_tab *tab = g_new0(health_tab, 1);
    tab->window = window;
    tab->status = status;
    tab->select_mount = g_strdup(mount_point);
    tab->select_device = g_strdup(device);
    tab->drives = g_ptr_array_new_with_free_func(free_drive);
    tab->cancellable = g_cancellable_new();
    build_tab(tab);
    GError *error = nullptr;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (bus != nullptr)
        tab->manager = g_dbus_object_manager_client_new_sync(
            bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, UDISKS, UDISKS_PATH, nullptr, nullptr,
            nullptr, nullptr, &error);
    if (tab->manager != nullptr &&
        g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(tab->manager)) ==
            nullptr)
        g_clear_object(&tab->manager);
    if (tab->manager == nullptr) {
        fprintf(stderr, "xd-diskbench: UDisks2 isn't running: %s\n",
                error != nullptr ? error->message : "no owner");
        g_clear_error(&error);
    }
    read_drives(tab);
    fill_drive_store(tab);
    if (tab->manager == nullptr)
        gtk_label_set_text(GTK_LABEL(tab->reason_label),
                           "UDisks2 isn't running, so the drives' health can't be read.");
    return tab;
}

GtkWidget *health_get_widget(health_tab *tab) { return tab->box; }

void health_free_tab(health_tab *tab) {
    g_cancellable_cancel(tab->cancellable);
    g_clear_object(&tab->cancellable);
    g_ptr_array_unref(tab->drives);
    g_clear_object(&tab->manager);
    g_object_unref(tab->drive_store);
    g_object_unref(tab->ata_store);
    g_object_unref(tab->nvme_store);
    g_free(tab->select_mount);
    g_free(tab->select_device);
    g_free(tab->temperature);
    g_free(tab);
}
