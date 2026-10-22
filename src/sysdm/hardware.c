/* SPDX-License-Identifier: MIT
 * System Properties' Hardware tab: Device Manager (from udev), Kernel
 * Modules (where Windows had the Hardware Wizard: Linux loads drivers by
 * itself), Driver Signing (Secure Boot and module signature enforcement)
 * and Power Profiles (where Windows had Hardware Profiles).
 */
#include <libudev.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "sysdm.h"

/* ---- categories ----------------------------------------------------------------- */

typedef enum cat : uint8_t {
    CAT_BATTERIES,
    CAT_BLUETOOTH,
    CAT_COMPUTER,
    CAT_DISKS,
    CAT_DISPLAY,
    CAT_OPTICAL,
    CAT_HID,
    CAT_IDE,
    CAT_IMAGING,
    CAT_KEYBOARDS,
    CAT_MICE,
    CAT_MONITORS,
    CAT_NETWORK,
    CAT_PROCESSORS,
    CAT_SOUND,
    CAT_STORAGE,
    CAT_SYSTEM,
    CAT_USB,
    CAT_OTHER,
    CAT_COUNT,
} cat;

static const struct {
    const char *name;
    const char *icon;
} cats[CAT_COUNT] = {
    [CAT_BATTERIES] = {"Batteries", "battery"},
    [CAT_BLUETOOTH] = {"Bluetooth Radios", "bluetooth"},
    [CAT_COMPUTER] = {"Computer", "computer"},
    [CAT_DISKS] = {"Disk drives", "drive-harddisk"},
    [CAT_DISPLAY] = {"Display adapters", "video-display"},
    [CAT_OPTICAL] = {"DVD/CD-ROM drives", "drive-optical"},
    [CAT_HID] = {"Human Interface Devices", "input-tablet"},
    [CAT_IDE] = {"IDE ATA/ATAPI controllers", "drive-harddisk"},
    [CAT_IMAGING] = {"Imaging devices", "camera-web"},
    [CAT_KEYBOARDS] = {"Keyboards", "input-keyboard"},
    [CAT_MICE] = {"Mice and other pointing devices", "input-mouse"},
    [CAT_MONITORS] = {"Monitors", "video-display"},
    [CAT_NETWORK] = {"Network adapters", "network-wired"},
    [CAT_PROCESSORS] = {"Processors", "cpu"},
    [CAT_SOUND] = {"Sound, video and game controllers", "audio-card"},
    [CAT_STORAGE] = {"Storage controllers", "drive-harddisk"},
    [CAT_SYSTEM] = {"System devices", "preferences-system"},
    [CAT_USB] = {"Universal Serial Bus controllers", "drive-removable-media-usb"},
    [CAT_OTHER] = {"Other devices", "dialog-question"},
};

enum { DM_ICON, DM_NAME, DM_SYSPATH, DM_CAT, DM_NCOLS };

typedef struct devmgr {
    GtkWidget *window;
    GtkTreeStore *store;
    GtkWidget *view;
    gboolean show_hidden;
} devmgr;

/* ---- naming ------------------------------------------------------------------- */

static const char *prop(struct udev_device *device, const char *key) {
    const char *value = udev_device_get_property_value(device, key);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

/* Looks up a property on the device or its ancestors. */
static const char *prop_up(struct udev_device *device, const char *key) {
    for (struct udev_device *ancestor = device; ancestor != nullptr;
         ancestor = udev_device_get_parent(ancestor)) {
        const char *value = prop(ancestor, key);
        if (value != nullptr)
            return value;
    }
    return nullptr;
}

static char *clean(const char *text) {
    char *out = g_strdup(text);
    g_strdelimit(out, "_", ' ');
    return g_strstrip(out);
}

/* "Vendor Model" from the hardware database, else what the device says. */
static char *pci_usb_name(struct udev_device *device) {
    const char *vendor = prop(device, "ID_VENDOR_FROM_DATABASE");
    const char *model = prop(device, "ID_MODEL_FROM_DATABASE");
    if (model != nullptr)
        return vendor != nullptr ? g_strdup_printf("%s %s", vendor, model) : g_strdup(model);
    const char *product = udev_device_get_sysattr_value(device, "product");
    const char *manu = udev_device_get_sysattr_value(device, "manufacturer");
    if (product != nullptr)
        return manu != nullptr ? g_strdup_printf("%s %s", manu, product) : g_strdup(product);
    return nullptr;
}

static unsigned pci_class(struct udev_device *device) {
    const char *class_text = udev_device_get_sysattr_value(device, "class");
    return class_text != nullptr ? (unsigned)strtoul(class_text, nullptr, 16) : 0;
}

/* Where a PCI or USB device sits, as Windows' "Location". */
static char *location(struct udev_device *device) {
    const char *sub = udev_device_get_subsystem(device);
    const char *sys = udev_device_get_sysname(device);
    if (sub == nullptr)
        return g_strdup("Unknown");
    if (strcmp(sub, "pci") == 0) {
        unsigned dom = 0, bus = 0, dev = 0, fn = 0;
        if (sscanf(sys, "%x:%x:%x.%x", &dom, &bus, &dev, &fn) == 4)
            return g_strdup_printf("PCI bus %u, device %u, function %u", bus, dev, fn);
    }
    for (struct udev_device *ancestor = device; ancestor != nullptr;
         ancestor = udev_device_get_parent(ancestor)) {
        const char *ps = udev_device_get_subsystem(ancestor);
        if (ps != nullptr && strcmp(ps, "usb") == 0 &&
            g_strcmp0(udev_device_get_devtype(ancestor), "usb_device") == 0)
            return g_strdup_printf("USB port %s", udev_device_get_sysname(ancestor));
        if (ps != nullptr && strcmp(ps, "pci") == 0 && ancestor != device) {
            char *parent = location(ancestor);
            char *out = g_strdup_printf("On %s", parent);
            g_free(parent);
            return out;
        }
    }
    return g_strdup(sub);
}

/* ---- classification ------------------------------------------------------------- */

typedef struct found {
    cat c;
    char *name;
    gboolean hidden; /* shown only with View > Show hidden devices */
} found;

static gboolean classify_pci(struct udev_device *device, found *classification) {
    unsigned cls = pci_class(device) >> 8; /* class and subclass */
    unsigned base = cls >> 8;
    classification->name = pci_usb_name(device);
    switch (base) {
    case 0x01:
        classification->c = cls == 0x0101 || cls == 0x0106 ? CAT_IDE : CAT_STORAGE;
        break;
    case 0x02:
        classification->c = CAT_NETWORK;
        break;
    case 0x03:
        classification->c = CAT_DISPLAY;
        break;
    case 0x04:
        classification->c = CAT_SOUND;
        break;
    case 0x0C:
        classification->c = cls == 0x0C03 ? CAT_USB : CAT_SYSTEM;
        break;
    case 0x05:
    case 0x06:
    case 0x08:
    case 0x11:
        classification->c = CAT_SYSTEM;
        break;
    default:
        classification->c = CAT_OTHER;
        break;
    }
    return TRUE;
}

static gboolean classify(struct udev_device *device, found *classification) {
    const char *sub = udev_device_get_subsystem(device);
    const char *sys = udev_device_get_sysname(device);
    const char *type = udev_device_get_devtype(device);
    *classification = (found){.c = CAT_OTHER};
    if (sub == nullptr || sys == nullptr)
        return FALSE;
    if (strcmp(sub, "pci") == 0)
        return classify_pci(device, classification);
    if (strcmp(sub, "usb") == 0) {
        /* Only hubs; other USB devices show up as what they are. */
        const char *dc = udev_device_get_sysattr_value(device, "bDeviceClass");
        if (g_strcmp0(type, "usb_device") != 0 || dc == nullptr || strcmp(dc, "09") != 0)
            return FALSE;
        classification->c = CAT_USB;
        struct udev_device *parent = udev_device_get_parent(device);
        const char *parent_sub = parent != nullptr ? udev_device_get_subsystem(parent) : nullptr;
        classification->name =
            g_strdup(g_strcmp0(parent_sub, "usb") == 0 ? "Generic USB Hub" : "USB Root Hub");
        return TRUE;
    }
    if (strcmp(sub, "block") == 0 && g_strcmp0(type, "disk") == 0) {
        gboolean virt = g_str_has_prefix(sys, "loop") || g_str_has_prefix(sys, "ram") ||
                        g_str_has_prefix(sys, "zram") || g_str_has_prefix(sys, "dm-") ||
                        g_str_has_prefix(sys, "md");
        classification->hidden = virt;
        classification->c = prop(device, "ID_CDROM") != nullptr ? CAT_OPTICAL : CAT_DISKS;
        const char *model = prop(device, "ID_MODEL");
        const char *attr = udev_device_get_sysattr_value(device, "device/model");
        if (model != nullptr)
            classification->name = clean(model);
        else if (attr != nullptr)
            classification->name = clean(attr);
        else if (g_str_has_prefix(sys, "zram"))
            classification->name = g_strdup_printf("Compressed RAM disk (%s)", sys);
        else
            classification->name =
                g_strdup_printf("%s (%s)", virt ? "Virtual disk" : "Disk drive", sys);
        return TRUE;
    }
    if (strcmp(sub, "input") == 0 && g_str_has_prefix(sys, "input")) {
        const char *name = udev_device_get_sysattr_value(device, "name");
        if (prop(device, "ID_INPUT_KEYBOARD") != nullptr)
            classification->c = CAT_KEYBOARDS;
        else if (prop(device, "ID_INPUT_MOUSE") != nullptr ||
                 prop(device, "ID_INPUT_TOUCHPAD") != nullptr ||
                 prop(device, "ID_INPUT_TABLET") != nullptr)
            classification->c = CAT_MICE;
        else
            classification->c = CAT_HID;
        /* Virtual and built-in buttons (power, lid) are "hidden". */
        classification->hidden =
            classification->c == CAT_HID || prop_up(device, "ID_BUS") == nullptr;
        classification->name = g_strdup(name != nullptr ? name : sys);
        return TRUE;
    }
    if (strcmp(sub, "net") == 0) {
        gboolean physical = udev_device_get_parent(device) != nullptr &&
                            udev_device_get_subsystem(udev_device_get_parent(device)) != nullptr;
        classification->hidden = !physical;
        classification->c = CAT_NETWORK;
        const char *model = prop(device, "ID_MODEL_FROM_DATABASE");
        classification->name =
            model != nullptr
                ? g_strdup_printf("%s (%s)", model, sys)
                : g_strdup_printf("%s (%s)", physical ? "Network adapter" : "Virtual adapter", sys);
        /* The PCI function is already listed; its interface is the same
         * device, so keep only virtual interfaces here. */
        return !physical;
    }
    if (strcmp(sub, "sound") == 0 && g_str_has_prefix(sys, "card")) {
        struct udev_device *parent = udev_device_get_parent(device);
        const char *psub = parent != nullptr ? udev_device_get_subsystem(parent) : nullptr;
        if (g_strcmp0(psub, "pci") == 0)
            return FALSE; /* the PCI device is listed */
        classification->c = CAT_SOUND;
        const char *id = udev_device_get_sysattr_value(device, "id");
        char *name = parent != nullptr ? pci_usb_name(parent) : nullptr;
        if (name == nullptr && parent != nullptr && udev_device_get_parent(parent) != nullptr)
            name = pci_usb_name(udev_device_get_parent(parent));
        classification->name = name != nullptr ? name : g_strdup(id != nullptr ? id : sys);
        return TRUE;
    }
    if (strcmp(sub, "power_supply") == 0) {
        const char *supply_type = udev_device_get_sysattr_value(device, "type");
        if (g_strcmp0(supply_type, "Battery") == 0) {
            const char *model = udev_device_get_sysattr_value(device, "model_name");
            classification->c = CAT_BATTERIES;
            classification->name =
                g_strdup_printf("%s (%s)", model != nullptr ? model : "Battery", sys);
            return TRUE;
        }
        if (g_strcmp0(supply_type, "Mains") == 0) {
            classification->c = CAT_BATTERIES;
            classification->name = g_strdup("AC Adapter");
            return TRUE;
        }
        return FALSE;
    }
    if (strcmp(sub, "bluetooth") == 0 && g_str_has_prefix(sys, "hci") && strchr(sys, ':') == nullptr) {
        classification->c = CAT_BLUETOOTH;
        char *parent = udev_device_get_parent(device) != nullptr
                           ? pci_usb_name(udev_device_get_parent(device))
                           : nullptr;
        classification->name =
            parent != nullptr ? parent : g_strdup_printf("Bluetooth radio (%s)", sys);
        return TRUE;
    }
    if (strcmp(sub, "video4linux") == 0) {
        const char *idx = udev_device_get_sysattr_value(device, "index");
        if (idx != nullptr && strcmp(idx, "0") != 0)
            return FALSE; /* one entry per camera */
        const char *name = udev_device_get_sysattr_value(device, "name");
        classification->c = CAT_IMAGING;
        classification->name = g_strdup(name != nullptr ? name : sys);
        return TRUE;
    }
    if (strcmp(sub, "drm") == 0 && strchr(sys, '-') != nullptr) {
        const char *status = udev_device_get_sysattr_value(device, "status");
        if (g_strcmp0(status, "connected") != 0)
            return FALSE;
        classification->c = CAT_MONITORS;
        const char *conn = strchr(sys, '-') + 1;
        classification->name = g_strdup_printf("Monitor on %s", conn);
        return TRUE;
    }
    if (strcmp(sub, "cpu") == 0 && g_str_has_prefix(sys, "cpu")) {
        classification->c = CAT_PROCESSORS;
        return TRUE; /* named from /proc/cpuinfo by the caller */
    }
    return FALSE;
}

/* ---- the tree --------------------------------------------------------------------- */

static char *cpu_model(void) {
    char *text = nullptr;
    if (!g_file_get_contents("/proc/cpuinfo", &text, nullptr, nullptr))
        return g_strdup("Processor");
    char *model = nullptr;
    char **lines = g_strsplit(text, "\n", -1);
    for (char **line = lines; *line != nullptr && model == nullptr; line++) {
        if (g_str_has_prefix(*line, "model name")) {
            char *colon = strchr(*line, ':');
            if (colon != nullptr)
                model = g_strdup(g_strstrip(colon + 1));
        }
    }
    g_strfreev(lines);
    g_free(text);
    return model != nullptr ? model : g_strdup("Processor");
}

static int compare_rows(GtkTreeModel *model, GtkTreeIter *left, GtkTreeIter *right,
                        [[maybe_unused]] gpointer data) {
    char *left_name = nullptr;
    char *right_name = nullptr;
    gtk_tree_model_get(model, left, DM_NAME, &left_name, -1);
    gtk_tree_model_get(model, right, DM_NAME, &right_name, -1);
    int order = g_utf8_collate(left_name != nullptr ? left_name : "",
                               right_name != nullptr ? right_name : "");
    g_free(left_name);
    g_free(right_name);
    return order;
}

static void fill(devmgr *dm) {
    gtk_tree_store_clear(dm->store);
    GtkTreeIter parents[CAT_COUNT];
    gboolean have[CAT_COUNT] = {FALSE};

    char host[256] = "localhost";
    gethostname(host, sizeof(host) - 1);
    GtkTreeIter root;
    gtk_tree_store_append(dm->store, &root, nullptr);
    gtk_tree_store_set(dm->store, &root, DM_ICON, "computer", DM_NAME, host, DM_CAT, -1, -1);

    struct udev *udev = udev_new();
    struct udev_enumerate *en = udev_enumerate_new(udev);
    static const char *const subsystems[] = {"pci",          "usb",       "block", "input",
                                             "net",          "sound",     "power_supply",
                                             "bluetooth",    "video4linux", "drm", "cpu"};
    for (size_t i = 0; i < G_N_ELEMENTS(subsystems); i++)
        udev_enumerate_add_match_subsystem(en, subsystems[i]);
    udev_enumerate_scan_devices(en);
    char *cpu = cpu_model();

    /* The Computer category has one entry: the machine itself, named the
     * way Windows XP names it ("ACPI x64-based PC"). */
    {
        struct utsname system_info;
        uname(&system_info);
        const char *arch = strcmp(system_info.machine, "x86_64") == 0 ? "x64"
                           : g_str_has_prefix(system_info.machine, "i") &&
                                   strstr(system_info.machine, "86") != nullptr
                               ? "x86"
                               : system_info.machine;
        gboolean acpi = g_file_test("/sys/firmware/acpi", G_FILE_TEST_IS_DIR);
        char *name = g_strdup_printf("%s%s-based PC", acpi ? "ACPI " : "", arch);
        gtk_tree_store_append(dm->store, &parents[CAT_COMPUTER], &root);
        gtk_tree_store_set(dm->store, &parents[CAT_COMPUTER], DM_ICON, cats[CAT_COMPUTER].icon,
                           DM_NAME, cats[CAT_COMPUTER].name, DM_CAT, (int)CAT_COMPUTER, -1);
        have[CAT_COMPUTER] = TRUE;
        GtkTreeIter it;
        gtk_tree_store_append(dm->store, &it, &parents[CAT_COMPUTER]);
        gtk_tree_store_set(dm->store, &it, DM_ICON, cats[CAT_COMPUTER].icon, DM_NAME, name,
                           DM_SYSPATH, "/sys/class/dmi/id", DM_CAT, (int)CAT_COMPUTER, -1);
        g_free(name);
    }

    struct udev_list_entry *list_entry;
    udev_list_entry_foreach(list_entry, udev_enumerate_get_list_entry(en)) {
        const char *path = udev_list_entry_get_name(list_entry);
        struct udev_device *device = udev_device_new_from_syspath(udev, path);
        if (device == nullptr)
            continue;
        found classification;
        if (classify(device, &classification) && (!classification.hidden || dm->show_hidden)) {
            if (!have[classification.c]) {
                gtk_tree_store_append(dm->store, &parents[classification.c], &root);
                gtk_tree_store_set(dm->store, &parents[classification.c], DM_ICON,
                                   cats[classification.c].icon, DM_NAME,
                                   cats[classification.c].name, DM_CAT, (int)classification.c, -1);
                have[classification.c] = TRUE;
            }
            GtkTreeIter it;
            gtk_tree_store_append(dm->store, &it, &parents[classification.c]);
            gtk_tree_store_set(dm->store, &it, DM_ICON, cats[classification.c].icon, DM_NAME,
                               classification.name != nullptr ? classification.name
                               : classification.c == CAT_PROCESSORS
                                   ? cpu
                                   : udev_device_get_sysname(device),
                               DM_SYSPATH, path, DM_CAT, (int)classification.c, -1);
        }
        g_free(classification.name);
        udev_device_unref(device);
    }
    g_free(cpu);
    udev_enumerate_unref(en);
    udev_unref(udev);

    /* Categories alphabetically, like Windows; devices sorted within. */
    gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(dm->store), compare_rows, nullptr,
                                            nullptr);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(dm->store),
                                         GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                         GTK_SORT_ASCENDING);
    GtkTreePath *rp = gtk_tree_path_new_first();
    gtk_tree_view_expand_row(GTK_TREE_VIEW(dm->view), rp, FALSE);
    gtk_tree_path_free(rp);
}

/* ---- device properties -------------------------------------------------------------- */

static GtkWidget *kv_grid(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    return grid;
}

static void kv(GtkWidget *grid, int *row, const char *key, const char *value) {
    gtk_grid_attach(GTK_GRID(grid), sysdm_label(key), 0, *row, 1, 1);
    GtkWidget *value_label = sysdm_label(value != nullptr ? value : "Not available");
    gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
    gtk_widget_set_can_focus(value_label, FALSE);
    gtk_widget_set_hexpand(value_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), value_label, 1, (*row)++, 1, 1);
}

static char *link_name(const char *syspath, const char *link) {
    char *link_path = g_build_filename(syspath, link, nullptr);
    char *target = g_file_read_link(link_path, nullptr);
    g_free(link_path);
    if (target == nullptr)
        return nullptr;
    char *base = g_path_get_basename(target);
    g_free(target);
    return base;
}

static void device_properties(devmgr *dm, const char *syspath, const char *name, int category) {
    /* Over Device Manager, which outlives the System Properties window
     * that opened it. */
    sysdm over = {.window = dm->window};
    char *title = g_strdup_printf("%s Properties", name);
    GtkWidget *box = sysdm_dialog(&over, title, cats[category].icon, FALSE, nullptr);
    g_free(title);
    GtkWidget *toplevel = gtk_widget_get_toplevel(box);
    gtk_window_set_resizable(GTK_WINDOW(toplevel), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(toplevel), 420, 440);

    struct udev *udev = udev_new();
    struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
    GtkWidget *nb = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(box), nb, TRUE, TRUE, 0);

    /* General */
    GtkWidget *gen = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(gen), 12);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(head),
                       gtk_image_new_from_icon_name(cats[category].icon, GTK_ICON_SIZE_DND), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), sysdm_label(name), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gen), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gen), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    GtkWidget *grid = kv_grid();
    int row = 0;
    kv(grid, &row, "Device type:", cats[category].name);
    const char *vendor = dev != nullptr ? prop_up(dev, "ID_VENDOR_FROM_DATABASE") : nullptr;
    if (vendor == nullptr && dev != nullptr)
        vendor = prop_up(dev, "ID_VENDOR");
    char *vendor_clean = vendor != nullptr ? clean(vendor) : nullptr;
    kv(grid, &row, "Manufacturer:", vendor_clean);
    g_free(vendor_clean);
    char *loc = dev != nullptr ? location(dev) : g_strdup("Not available");
    kv(grid, &row, "Location:", loc);
    g_free(loc);
    gtk_box_pack_start(GTK_BOX(gen), grid, FALSE, FALSE, 0);
    const char *driver = dev != nullptr ? udev_device_get_driver(dev) : nullptr;
    if (driver == nullptr && dev != nullptr && udev_device_get_parent(dev) != nullptr)
        driver = udev_device_get_driver(udev_device_get_parent(dev));
    GtkWidget *status =
        sysdm_label(dev == nullptr ? "This device is no longer present."
                    : driver != nullptr || category == CAT_PROCESSORS || category == CAT_COMPUTER ||
                            category == CAT_MONITORS || category == CAT_DISKS ||
                            category == CAT_OPTICAL || category == CAT_BATTERIES ||
                            category == CAT_KEYBOARDS || category == CAT_MICE || category == CAT_HID
                        ? "This device is working properly."
                        : "No driver is loaded for this device.");
    gtk_box_pack_start(GTK_BOX(gen), sysdm_group("Device status", status), FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gen, gtk_label_new("General"));

    /* Driver */
    GtkWidget *drv = kv_grid();
    gtk_container_set_border_width(GTK_CONTAINER(drv), 12);
    row = 0;
    kv(drv, &row, "Driver:", driver);
    char *module = driver != nullptr ? link_name(syspath, "driver/module") : nullptr;
    if (module == nullptr && dev != nullptr && udev_device_get_parent(dev) != nullptr)
        module = link_name(udev_device_get_syspath(udev_device_get_parent(dev)), "driver/module");
    kv(drv, &row, "Kernel module:", module != nullptr ? module : driver != nullptr ? "Built in" : nullptr);
    char *version = nullptr;
    if (module != nullptr) {
        char *vp = g_strdup_printf("/sys/module/%s/version", module);
        version = sysdm_read_line(vp);
        g_free(vp);
    }
    kv(drv, &row, "Driver version:", version != nullptr ? version : "Same as the kernel");
    kv(drv, &row, "Device file:", dev != nullptr ? udev_device_get_devnode(dev) : nullptr);
    kv(drv, &row, "Device path:", syspath);
    g_free(module);
    g_free(version);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), drv, gtk_label_new("Driver"));

    /* Details: every udev property, Linux's equivalent of Windows' list. */
    GtkListStore *ls = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    if (dev != nullptr) {
        struct udev_list_entry *list_entry;
        udev_list_entry_foreach(list_entry, udev_device_get_properties_list_entry(dev)) {
            GtkTreeIter it;
            gtk_list_store_append(ls, &it);
            gtk_list_store_set(ls, &it, 0, udev_list_entry_get_name(list_entry), 1,
                               udev_list_entry_get_value(list_entry), -1);
        }
    }
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ls));
    g_object_unref(ls);
    const char *heads[] = {"Property", "Value"};
    for (int i = 0; i < 2; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            heads[i], gtk_cell_renderer_text_new(), "text", i, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_set_border_width(GTK_CONTAINER(sc), 12);
    gtk_container_add(GTK_CONTAINER(sc), tv);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sc, gtk_label_new("Details"));

    if (dev != nullptr)
        udev_device_unref(dev);
    udev_unref(udev);
    gtk_widget_show_all(toplevel);
}

static void open_selected(devmgr *dm) {
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(dm->view)),
                                         &model, &it))
        return;
    char *path = nullptr;
    char *name = nullptr;
    int category = -1;
    gtk_tree_model_get(model, &it, DM_SYSPATH, &path, DM_NAME, &name, DM_CAT, &category, -1);
    if (path != nullptr && category >= 0)
        device_properties(dm, path, name, category);
    else {
        GtkTreePath *tp = gtk_tree_model_get_path(model, &it);
        if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(dm->view), tp))
            gtk_tree_view_collapse_row(GTK_TREE_VIEW(dm->view), tp);
        else
            gtk_tree_view_expand_row(GTK_TREE_VIEW(dm->view), tp, FALSE);
        gtk_tree_path_free(tp);
    }
    g_free(path);
    g_free(name);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *tree_view,
                             [[maybe_unused]] GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    open_selected(data);
}

static void on_menu_properties([[maybe_unused]] GtkMenuItem *menu_item, gpointer data) {
    open_selected(data);
}

static void on_menu_scan([[maybe_unused]] GtkMenuItem *menu_item, gpointer data) { fill(data); }

static void on_menu_hidden(GtkCheckMenuItem *menu_item, gpointer data) {
    devmgr *dm = data;
    dm->show_hidden = gtk_check_menu_item_get_active(menu_item);
    fill(dm);
}

static void on_menu_close([[maybe_unused]] GtkMenuItem *menu_item, gpointer data) {
    devmgr *dm = data;
    gtk_widget_destroy(dm->window);
}

static void on_devmgr_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) { g_free(data); }

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data) {
    GtkWidget *widget = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(widget, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), widget);
    return widget;
}

void sysdm_device_manager(sysdm *properties) {
    devmgr *dm = g_new0(devmgr, 1);
    dm->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_application(GTK_WINDOW(dm->window), properties->app);
    gtk_window_set_title(GTK_WINDOW(dm->window), "Device Manager");
    gtk_window_set_icon_name(GTK_WINDOW(dm->window), "computer");
    gtk_window_set_default_size(GTK_WINDOW(dm->window), 480, 520);
    g_signal_connect(dm->window, "destroy", G_CALLBACK(on_devmgr_destroy), dm);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dm->window), vbox);
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *action = gtk_menu_item_new_with_mnemonic("_Action");
    GtkWidget *am = gtk_menu_new();
    menu_item(am, "_Scan for hardware changes", G_CALLBACK(on_menu_scan), dm);
    gtk_menu_shell_append(GTK_MENU_SHELL(am), gtk_separator_menu_item_new());
    menu_item(am, "P_roperties", G_CALLBACK(on_menu_properties), dm);
    gtk_menu_shell_append(GTK_MENU_SHELL(am), gtk_separator_menu_item_new());
    menu_item(am, "E_xit", G_CALLBACK(on_menu_close), dm);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(action), am);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), action);
    GtkWidget *view_item = gtk_menu_item_new_with_mnemonic("_View");
    GtkWidget *vm = gtk_menu_new();
    GtkWidget *hidden = gtk_check_menu_item_new_with_mnemonic("Show hi_dden devices");
    g_signal_connect(hidden, "toggled", G_CALLBACK(on_menu_hidden), dm);
    gtk_menu_shell_append(GTK_MENU_SHELL(vm), hidden);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), vm);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view_item);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    dm->store = gtk_tree_store_new(DM_NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    dm->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(dm->store));
    g_object_unref(dm->store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(dm->view), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", DM_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", DM_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(dm->view), col);
    g_signal_connect(dm->view, "row-activated", G_CALLBACK(on_row_activated), dm);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), dm->view);
    gtk_box_pack_start(GTK_BOX(vbox), sc, TRUE, TRUE, 0);
    fill(dm);
    gtk_widget_show_all(dm->window);
}

/* ---- Kernel Modules ------------------------------------------------------------------ */

static void on_modules([[maybe_unused]] GtkButton *button, gpointer data) {
    sysdm *properties = data;
    GtkWidget *widget = nullptr;
    GtkWidget *box =
        sysdm_dialog(properties, "Kernel Modules", "application-x-sharedlib", FALSE, nullptr);
    widget = gtk_widget_get_toplevel(box);
    gtk_window_set_resizable(GTK_WINDOW(widget), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(widget), 440, 460);
    gtk_box_pack_start(GTK_BOX(box),
                       sysdm_label("Linux finds drivers for your hardware by itself and loads them "
                                   "as kernel modules. These modules are loaded now:"),
                       FALSE, FALSE, 0);
    GtkListStore *ls = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_STRING);
    char *text = nullptr;
    if (g_file_get_contents("/proc/modules", &text, nullptr, nullptr)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **line = lines; *line != nullptr; line++) {
            char **fields = g_strsplit(*line, " ", 6);
            if (g_strv_length(fields) >= 4) {
                char *users = g_strdup(strcmp(fields[3], "-") == 0 ? "" : fields[3]);
                g_strdelimit(users, ",", ' ');
                GtkTreeIter it;
                gtk_list_store_append(ls, &it);
                gtk_list_store_set(ls, &it, 0, fields[0], 1,
                                   g_ascii_strtoull(fields[1], nullptr, 10) / 1024, 2,
                                   g_strstrip(users), -1);
                g_free(users);
            }
            g_strfreev(fields);
        }
        g_strfreev(lines);
        g_free(text);
    }
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(ls), 0, GTK_SORT_ASCENDING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ls));
    g_object_unref(ls);
    const char *heads[] = {"Module", "Size (KB)", "Used by"};
    for (int i = 0; i < 3; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            heads[i], gtk_cell_renderer_text_new(), "text", i, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_sort_column_id(col, i);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), tv);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    gtk_widget_show_all(widget);
}

/* ---- Driver Signing -------------------------------------------------------------------- */

/* An EFI variable's value: 4 attribute bytes, then the data. */
static GBytes *efi_var(const char *name) {
    char *path = g_strdup_printf("/sys/firmware/efi/efivars/%s", name);
    char *data = nullptr;
    gsize len = 0;
    gboolean ok = g_file_get_contents(path, &data, &len, nullptr);
    g_free(path);
    if (!ok || len < 5) {
        g_free(data);
        return nullptr;
    }
    GBytes *bytes = g_bytes_new(data + 4, len - 4);
    g_free(data);
    return bytes;
}

static void on_signing([[maybe_unused]] GtkButton *button, gpointer data) {
    sysdm *properties = data;
    GtkWidget *box =
        sysdm_dialog(properties, "Driver Signing Options", "security-high", FALSE, nullptr);
    gtk_box_pack_start(GTK_BOX(box),
                       sysdm_label("Linux can check that kernel modules (drivers) are signed by "
                                   "your distribution. These settings come from the firmware and "
                                   "the kernel's boot options."),
                       FALSE, FALSE, 0);

    GBytes *sb = efi_var("SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c");
    const char *secure = sb == nullptr ? "Not supported (legacy BIOS boot)"
                         : ((const guint8 *)g_bytes_get_data(sb, nullptr))[0] == 1
                             ? "On"
                             : "Off";
    char *enforce = sysdm_read_line("/sys/module/module/parameters/sig_enforce");
    char *lockdown = sysdm_read_line("/sys/kernel/security/lockdown");
    char *tainted_s = sysdm_read_line("/proc/sys/kernel/tainted");
    guint64 tainted = tainted_s != nullptr ? g_ascii_strtoull(tainted_s, nullptr, 10) : 0;

    /* Windows' three choices, set by module.sig_enforce: unsigned modules
     * load, or don't. */
    gboolean block = enforce != nullptr && enforce[0] == 'Y';
    GtkWidget *ignore = gtk_radio_button_new_with_label(nullptr,
                                                        "Ignore - Load modules whether or not "
                                                        "they're signed");
    GtkWidget *warn = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ignore), "Warn - Load them, and mark the kernel as tainted");
    GtkWidget *blk = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(ignore), "Block - Never load unsigned modules");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(block ? blk : warn), TRUE);
    GtkWidget *radios = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(radios), ignore, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(radios), warn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(radios), blk, FALSE, FALSE, 0);
    GtkWidget *note = sysdm_label("Set with the module.sig_enforce kernel option.");
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_box_pack_start(GTK_BOX(radios), note, FALSE, FALSE, 4);
    gtk_widget_set_sensitive(ignore, FALSE);
    gtk_widget_set_sensitive(warn, FALSE);
    gtk_widget_set_sensitive(blk, FALSE);
    gtk_box_pack_start(GTK_BOX(box),
                       sysdm_group("What action do you want Linux to take with unsigned modules?",
                                   radios),
                       FALSE, FALSE, 0);

    GtkWidget *widget = kv_grid();
    int row = 0;
    kv(widget, &row, "Secure Boot:", secure);
    char *lock = nullptr;
    if (lockdown != nullptr) {
        /* "[none] integrity confidentiality": the bracketed one is active. */
        char *open = strchr(lockdown, '[');
        char *close = open != nullptr ? strchr(open, ']') : nullptr;
        if (close != nullptr)
            lock = g_strndup(open + 1, (gsize)(close - open - 1));
    }
    kv(widget, &row, "Kernel lockdown:", lock != nullptr ? lock : "Unknown");
    GString *string = g_string_new(nullptr);
    if (tainted == 0)
        g_string_append(string, "No");
    if (tainted & 1)
        g_string_append(string, "Proprietary module loaded. ");
    if (tainted & (1u << 12))
        g_string_append(string, "Out-of-tree module loaded. ");
    if (tainted & (1u << 13))
        g_string_append(string, "Unsigned module loaded. ");
    if (tainted != 0 && string->len == 0)
        g_string_append_printf(string, "Yes (flags %" G_GUINT64_FORMAT ")", tainted);
    kv(widget, &row, "Kernel tainted:", g_strstrip(string->str));
    gtk_box_pack_start(GTK_BOX(box), sysdm_group("Status", widget), FALSE, FALSE, 0);
    g_string_free(string, TRUE);
    g_free(lock);
    g_free(enforce);
    g_free(lockdown);
    g_free(tainted_s);
    if (sb != nullptr)
        g_bytes_unref(sb);
    gtk_widget_show_all(gtk_widget_get_toplevel(box));
}

/* ---- Power Profiles ------------------------------------------------------------------ */

typedef struct power_dlg {
    GDBusProxy *proxy;
    GtkWidget *dialog;
    GSList *radios;
    gboolean busy; /* setting the profile */
} power_dlg;

static void power_close(power_dlg *power_dialog) {
    gtk_widget_destroy(power_dialog->dialog);
    g_slist_free(power_dialog->radios);
    g_object_unref(power_dialog->proxy);
    g_free(power_dialog);
}

static void power_set(GObject *src, GAsyncResult *res, gpointer data) {
    power_dlg *power_dialog = data;
    GError *err = nullptr;
    GVariant *variant = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    power_dialog->busy = FALSE;
    gtk_widget_set_sensitive(power_dialog->dialog, TRUE);
    if (variant == nullptr) {
        g_dbus_error_strip_remote_error(err);
        xd_message_box(GTK_WINDOW(power_dialog->dialog), "Power Profiles", XD_MB_ERROR, XD_MB_OK,
                       FALSE, err->message);
        g_error_free(err);
        return;
    }
    g_variant_unref(variant);
    power_close(power_dialog);
}

static void on_power_response([[maybe_unused]] GtkDialog *gtk_dialog, int response, gpointer data) {
    power_dlg *power_dialog = data;
    if (power_dialog->busy)
        return; /* closes once the profile is set */
    for (GSList *list = power_dialog->radios; response == GTK_RESPONSE_OK && list != nullptr;
         list = list->next) {
        if (!gtk_toggle_button_get_active(list->data))
            continue;
        const char *profile = g_object_get_data(list->data, "profile");
        /* Not waiting for it: polkit may ask for a password. */
        power_dialog->busy = TRUE;
        gtk_widget_set_sensitive(power_dialog->dialog, FALSE);
        g_dbus_proxy_call(power_dialog->proxy, "org.freedesktop.DBus.Properties.Set",
                          g_variant_new("(ssv)",
                                        g_dbus_proxy_get_interface_name(power_dialog->proxy),
                                        "ActiveProfile", g_variant_new_string(profile)),
                          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 120000, nullptr,
                          power_set, power_dialog);
        return;
    }
    power_close(power_dialog);
}

static const char *profile_label(const char *id) {
    if (strcmp(id, "power-saver") == 0)
        return "Power saver - Longer battery life, slower";
    if (strcmp(id, "balanced") == 0)
        return "Balanced - Speed and power use in balance";
    if (strcmp(id, "performance") == 0)
        return "Performance - Fastest, uses the most power";
    return id;
}

static void on_power(GtkButton *button, gpointer data) {
    sysdm *properties = data;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.freedesktop.UPower.PowerProfiles",
        "/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles", nullptr,
        nullptr);
    GVariant *profiles = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, "Profiles")
                                          : nullptr;
    if (profiles == nullptr) {
        g_clear_object(&proxy);
        xd_message_box(GTK_WINDOW(properties->window), "Power Profiles", XD_MB_INFO, XD_MB_OK,
                       FALSE,
                       "Power profiles aren't available. Install and start "
                       "power-profiles-daemon (or tuned-ppd) to use them.");
        gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
        return;
    }
    GVariant *active_v = g_dbus_proxy_get_cached_property(proxy, "ActiveProfile");
    const char *active = active_v != nullptr ? g_variant_get_string(active_v, nullptr) : "";

    power_dlg *power_dialog = g_new0(power_dlg, 1);
    power_dialog->proxy = proxy;
    GtkWidget *box =
        sysdm_dialog(properties, "Power Profiles", "preferences-system-power-management", TRUE,
                     &power_dialog->dialog);
    gtk_box_pack_start(GTK_BOX(box),
                       sysdm_label("Power profiles trade speed against power use and heat. "
                                   "Choose how this computer should run:"),
                       FALSE, FALSE, 0);
    GtkWidget *radios = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *first = nullptr;
    GVariantIter it;
    g_variant_iter_init(&it, profiles);
    GVariant *dict;
    while ((dict = g_variant_iter_next_value(&it)) != nullptr) {
        const char *id = nullptr;
        if (g_variant_lookup(dict, "Profile", "&s", &id)) {
            GtkWidget *radio = first == nullptr
                                   ? gtk_radio_button_new_with_label(nullptr, profile_label(id))
                                   : gtk_radio_button_new_with_label_from_widget(
                                         GTK_RADIO_BUTTON(first), profile_label(id));
            if (first == nullptr)
                first = radio;
            g_object_set_data_full(G_OBJECT(radio), "profile", g_strdup(id), g_free);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), strcmp(id, active) == 0);
            gtk_box_pack_start(GTK_BOX(radios), radio, FALSE, FALSE, 0);
            power_dialog->radios = g_slist_append(power_dialog->radios, radio);
        }
        g_variant_unref(dict);
    }
    gtk_box_pack_start(GTK_BOX(box), sysdm_group("Power profile", radios), FALSE, FALSE, 0);
    g_signal_connect(power_dialog->dialog, "response", G_CALLBACK(on_power_response), power_dialog);
    gtk_widget_show_all(power_dialog->dialog);
    g_variant_unref(profiles);
    if (active_v != nullptr)
        g_variant_unref(active_v);
}

/* ---- the tab ------------------------------------------------------------------------- */

static void on_devmgr([[maybe_unused]] GtkButton *button, gpointer data) {
    sysdm_device_manager(data);
}

GtkWidget *sysdm_hardware_page(sysdm *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    gtk_box_pack_start(
        GTK_BOX(page),
        sysdm_group_with_icon(
            "Drivers", "application-x-sharedlib",
            "Linux finds and loads the drivers your hardware needs by itself. "
            "Kernel Modules lists the drivers loaded now.",
            sysdm_button_row(sysdm_button("_Kernel Modules...", G_CALLBACK(on_modules), properties),
                             nullptr)),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(page),
        sysdm_group_with_icon(
            "Device Manager", "computer",
            "The Device Manager lists all the hardware devices installed on "
            "your computer. Use the Device Manager to see the properties of "
            "any device.",
            sysdm_button_row(sysdm_button("_Driver Signing...", G_CALLBACK(on_signing), properties),
                             sysdm_button("D_evice Manager...", G_CALLBACK(on_devmgr), properties),
                             nullptr)),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(page),
        sysdm_group_with_icon(
            "Power Profiles", "preferences-system-power-management",
            "Power profiles set how fast this computer runs against how much "
            "power it uses.",
            sysdm_button_row(sysdm_button("_Power Profiles...", G_CALLBACK(on_power), properties),
                             nullptr)),
        FALSE, FALSE, 0);
    return page;
}
