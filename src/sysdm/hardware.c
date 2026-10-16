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
    sysdm *s;
    GtkWidget *window;
    GtkTreeStore *store;
    GtkWidget *view;
    gboolean show_hidden;
} devmgr;

/* ---- naming ------------------------------------------------------------------- */

static const char *prop(struct udev_device *d, const char *key) {
    const char *v = udev_device_get_property_value(d, key);
    return v != nullptr && v[0] != '\0' ? v : nullptr;
}

/* Looks up a property on the device or its ancestors. */
static const char *prop_up(struct udev_device *d, const char *key) {
    for (struct udev_device *p = d; p != nullptr; p = udev_device_get_parent(p)) {
        const char *v = prop(p, key);
        if (v != nullptr)
            return v;
    }
    return nullptr;
}

static char *clean(const char *s) {
    char *out = g_strdup(s);
    g_strdelimit(out, "_", ' ');
    return g_strstrip(out);
}

/* "Vendor Model" from the hardware database, else what the device says. */
static char *pci_usb_name(struct udev_device *d) {
    const char *vendor = prop(d, "ID_VENDOR_FROM_DATABASE");
    const char *model = prop(d, "ID_MODEL_FROM_DATABASE");
    if (model != nullptr)
        return vendor != nullptr ? g_strdup_printf("%s %s", vendor, model) : g_strdup(model);
    const char *product = udev_device_get_sysattr_value(d, "product");
    const char *manu = udev_device_get_sysattr_value(d, "manufacturer");
    if (product != nullptr)
        return manu != nullptr ? g_strdup_printf("%s %s", manu, product) : g_strdup(product);
    return nullptr;
}

static unsigned pci_class(struct udev_device *d) {
    const char *c = udev_device_get_sysattr_value(d, "class");
    return c != nullptr ? (unsigned)strtoul(c, nullptr, 16) : 0;
}

/* Where a PCI or USB device sits, as Windows' "Location". */
static char *location(struct udev_device *d) {
    const char *sub = udev_device_get_subsystem(d);
    const char *sys = udev_device_get_sysname(d);
    if (sub == nullptr)
        return g_strdup("Unknown");
    if (strcmp(sub, "pci") == 0) {
        unsigned dom = 0, bus = 0, dev = 0, fn = 0;
        if (sscanf(sys, "%x:%x:%x.%x", &dom, &bus, &dev, &fn) == 4)
            return g_strdup_printf("PCI bus %u, device %u, function %u", bus, dev, fn);
    }
    for (struct udev_device *p = d; p != nullptr; p = udev_device_get_parent(p)) {
        const char *ps = udev_device_get_subsystem(p);
        if (ps != nullptr && strcmp(ps, "usb") == 0 &&
            g_strcmp0(udev_device_get_devtype(p), "usb_device") == 0)
            return g_strdup_printf("USB port %s", udev_device_get_sysname(p));
        if (ps != nullptr && strcmp(ps, "pci") == 0 && p != d) {
            char *parent = location(p);
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

static gboolean classify_pci(struct udev_device *d, found *f) {
    unsigned cls = pci_class(d) >> 8; /* class and subclass */
    unsigned base = cls >> 8;
    f->name = pci_usb_name(d);
    switch (base) {
    case 0x01:
        f->c = cls == 0x0101 || cls == 0x0106 ? CAT_IDE : CAT_STORAGE;
        break;
    case 0x02:
        f->c = CAT_NETWORK;
        break;
    case 0x03:
        f->c = CAT_DISPLAY;
        break;
    case 0x04:
        f->c = CAT_SOUND;
        break;
    case 0x0C:
        f->c = cls == 0x0C03 ? CAT_USB : CAT_SYSTEM;
        break;
    case 0x05:
    case 0x06:
    case 0x08:
    case 0x11:
        f->c = CAT_SYSTEM;
        break;
    default:
        f->c = CAT_OTHER;
        break;
    }
    return TRUE;
}

static gboolean classify(struct udev_device *d, found *f) {
    const char *sub = udev_device_get_subsystem(d);
    const char *sys = udev_device_get_sysname(d);
    const char *type = udev_device_get_devtype(d);
    *f = (found){.c = CAT_OTHER};
    if (sub == nullptr || sys == nullptr)
        return FALSE;
    if (strcmp(sub, "pci") == 0)
        return classify_pci(d, f);
    if (strcmp(sub, "usb") == 0) {
        /* Only hubs; other USB devices show up as what they are. */
        const char *dc = udev_device_get_sysattr_value(d, "bDeviceClass");
        if (g_strcmp0(type, "usb_device") != 0 || dc == nullptr || strcmp(dc, "09") != 0)
            return FALSE;
        f->c = CAT_USB;
        const char *parent_sub = udev_device_get_subsystem(udev_device_get_parent(d));
        f->name = g_strdup(g_strcmp0(parent_sub, "usb") == 0 ? "Generic USB Hub"
                                                              : "USB Root Hub");
        return TRUE;
    }
    if (strcmp(sub, "block") == 0 && g_strcmp0(type, "disk") == 0) {
        gboolean virt = g_str_has_prefix(sys, "loop") || g_str_has_prefix(sys, "ram") ||
                        g_str_has_prefix(sys, "zram") || g_str_has_prefix(sys, "dm-") ||
                        g_str_has_prefix(sys, "md");
        f->hidden = virt;
        f->c = prop(d, "ID_CDROM") != nullptr ? CAT_OPTICAL : CAT_DISKS;
        const char *model = prop(d, "ID_MODEL");
        const char *attr = udev_device_get_sysattr_value(d, "device/model");
        if (model != nullptr)
            f->name = clean(model);
        else if (attr != nullptr)
            f->name = clean(attr);
        else if (g_str_has_prefix(sys, "zram"))
            f->name = g_strdup_printf("Compressed RAM disk (%s)", sys);
        else
            f->name = g_strdup_printf("%s (%s)", virt ? "Virtual disk" : "Disk drive", sys);
        return TRUE;
    }
    if (strcmp(sub, "input") == 0 && g_str_has_prefix(sys, "input")) {
        const char *name = udev_device_get_sysattr_value(d, "name");
        if (prop(d, "ID_INPUT_KEYBOARD") != nullptr)
            f->c = CAT_KEYBOARDS;
        else if (prop(d, "ID_INPUT_MOUSE") != nullptr || prop(d, "ID_INPUT_TOUCHPAD") != nullptr ||
                 prop(d, "ID_INPUT_TABLET") != nullptr)
            f->c = CAT_MICE;
        else
            f->c = CAT_HID;
        /* Virtual and built-in buttons (power, lid) are "hidden". */
        f->hidden = f->c == CAT_HID || prop_up(d, "ID_BUS") == nullptr;
        f->name = g_strdup(name != nullptr ? name : sys);
        return TRUE;
    }
    if (strcmp(sub, "net") == 0) {
        gboolean physical = udev_device_get_parent(d) != nullptr &&
                            udev_device_get_subsystem(udev_device_get_parent(d)) != nullptr;
        f->hidden = !physical;
        f->c = CAT_NETWORK;
        const char *model = prop(d, "ID_MODEL_FROM_DATABASE");
        f->name = model != nullptr ? g_strdup_printf("%s (%s)", model, sys)
                                   : g_strdup_printf("%s (%s)", physical ? "Network adapter"
                                                                         : "Virtual adapter",
                                                     sys);
        /* The PCI function is already listed; its interface is the same
         * device, so keep only virtual interfaces here. */
        return !physical;
    }
    if (strcmp(sub, "sound") == 0 && g_str_has_prefix(sys, "card")) {
        struct udev_device *parent = udev_device_get_parent(d);
        const char *psub = parent != nullptr ? udev_device_get_subsystem(parent) : nullptr;
        if (g_strcmp0(psub, "pci") == 0)
            return FALSE; /* the PCI device is listed */
        f->c = CAT_SOUND;
        const char *id = udev_device_get_sysattr_value(d, "id");
        char *name = parent != nullptr ? pci_usb_name(parent) : nullptr;
        if (name == nullptr && parent != nullptr && udev_device_get_parent(parent) != nullptr)
            name = pci_usb_name(udev_device_get_parent(parent));
        f->name = name != nullptr ? name : g_strdup(id != nullptr ? id : sys);
        return TRUE;
    }
    if (strcmp(sub, "power_supply") == 0) {
        const char *t = udev_device_get_sysattr_value(d, "type");
        if (g_strcmp0(t, "Battery") == 0) {
            const char *model = udev_device_get_sysattr_value(d, "model_name");
            f->c = CAT_BATTERIES;
            f->name = g_strdup_printf("%s (%s)", model != nullptr ? model : "Battery", sys);
            return TRUE;
        }
        if (g_strcmp0(t, "Mains") == 0) {
            f->c = CAT_BATTERIES;
            f->name = g_strdup("AC Adapter");
            return TRUE;
        }
        return FALSE;
    }
    if (strcmp(sub, "bluetooth") == 0 && g_str_has_prefix(sys, "hci") && strchr(sys, ':') == nullptr) {
        f->c = CAT_BLUETOOTH;
        char *parent = udev_device_get_parent(d) != nullptr
                           ? pci_usb_name(udev_device_get_parent(d))
                           : nullptr;
        f->name = parent != nullptr ? parent : g_strdup_printf("Bluetooth radio (%s)", sys);
        return TRUE;
    }
    if (strcmp(sub, "video4linux") == 0) {
        const char *idx = udev_device_get_sysattr_value(d, "index");
        if (idx != nullptr && strcmp(idx, "0") != 0)
            return FALSE; /* one entry per camera */
        const char *name = udev_device_get_sysattr_value(d, "name");
        f->c = CAT_IMAGING;
        f->name = g_strdup(name != nullptr ? name : sys);
        return TRUE;
    }
    if (strcmp(sub, "drm") == 0 && strchr(sys, '-') != nullptr) {
        const char *status = udev_device_get_sysattr_value(d, "status");
        if (g_strcmp0(status, "connected") != 0)
            return FALSE;
        f->c = CAT_MONITORS;
        const char *conn = strchr(sys, '-') + 1;
        f->name = g_strdup_printf("Monitor on %s", conn);
        return TRUE;
    }
    if (strcmp(sub, "cpu") == 0 && g_str_has_prefix(sys, "cpu")) {
        f->c = CAT_PROCESSORS;
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
    for (char **l = lines; *l != nullptr && model == nullptr; l++) {
        if (g_str_has_prefix(*l, "model name")) {
            char *colon = strchr(*l, ':');
            if (colon != nullptr)
                model = g_strdup(g_strstrip(colon + 1));
        }
    }
    g_strfreev(lines);
    g_free(text);
    return model != nullptr ? model : g_strdup("Processor");
}

static int compare_rows(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b,
                        [[maybe_unused]] gpointer data) {
    char *x = nullptr;
    char *y = nullptr;
    gtk_tree_model_get(m, a, DM_NAME, &x, -1);
    gtk_tree_model_get(m, b, DM_NAME, &y, -1);
    int r = g_utf8_collate(x != nullptr ? x : "", y != nullptr ? y : "");
    g_free(x);
    g_free(y);
    return r;
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
        struct utsname u;
        uname(&u);
        const char *arch = strcmp(u.machine, "x86_64") == 0 ? "x64"
                           : g_str_has_prefix(u.machine, "i") && strstr(u.machine, "86") != nullptr
                               ? "x86"
                               : u.machine;
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

    struct udev_list_entry *e;
    udev_list_entry_foreach(e, udev_enumerate_get_list_entry(en)) {
        const char *path = udev_list_entry_get_name(e);
        struct udev_device *d = udev_device_new_from_syspath(udev, path);
        if (d == nullptr)
            continue;
        found f;
        if (classify(d, &f) && (!f.hidden || dm->show_hidden)) {
            if (!have[f.c]) {
                gtk_tree_store_append(dm->store, &parents[f.c], &root);
                gtk_tree_store_set(dm->store, &parents[f.c], DM_ICON, cats[f.c].icon, DM_NAME,
                                   cats[f.c].name, DM_CAT, (int)f.c, -1);
                have[f.c] = TRUE;
            }
            GtkTreeIter it;
            gtk_tree_store_append(dm->store, &it, &parents[f.c]);
            gtk_tree_store_set(dm->store, &it, DM_ICON, cats[f.c].icon, DM_NAME,
                               f.name != nullptr ? f.name
                               : f.c == CAT_PROCESSORS ? cpu
                                                       : udev_device_get_sysname(d),
                               DM_SYSPATH, path, DM_CAT, (int)f.c, -1);
        }
        g_free(f.name);
        udev_device_unref(d);
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
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    return g;
}

static void kv(GtkWidget *grid, int *row, const char *key, const char *value) {
    gtk_grid_attach(GTK_GRID(grid), sysdm_label(key), 0, *row, 1, 1);
    GtkWidget *v = sysdm_label(value != nullptr ? value : "Not available");
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_can_focus(v, FALSE);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), v, 1, (*row)++, 1, 1);
}

static char *link_name(const char *syspath, const char *link) {
    char *p = g_build_filename(syspath, link, nullptr);
    char *target = g_file_read_link(p, nullptr);
    g_free(p);
    if (target == nullptr)
        return nullptr;
    char *base = g_path_get_basename(target);
    g_free(target);
    return base;
}

static void device_properties(devmgr *dm, const char *syspath, const char *name, int c) {
    GtkWidget *d = nullptr;
    char *title = g_strdup_printf("%s Properties", name);
    GtkWidget *box = sysdm_dialog(dm->s, title, cats[c].icon, FALSE, nullptr);
    g_free(title);
    d = gtk_widget_get_toplevel(box);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(dm->window));
    gtk_window_set_resizable(GTK_WINDOW(d), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(d), 420, 440);

    struct udev *udev = udev_new();
    struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
    GtkWidget *nb = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(box), nb, TRUE, TRUE, 0);

    /* General */
    GtkWidget *gen = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(gen), 12);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(head), gtk_image_new_from_icon_name(cats[c].icon, GTK_ICON_SIZE_DND),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), sysdm_label(name), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gen), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gen), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    GtkWidget *g = kv_grid();
    int row = 0;
    kv(g, &row, "Device type:", cats[c].name);
    const char *vendor = dev != nullptr ? prop_up(dev, "ID_VENDOR_FROM_DATABASE") : nullptr;
    if (vendor == nullptr && dev != nullptr)
        vendor = prop_up(dev, "ID_VENDOR");
    char *vendor_clean = vendor != nullptr ? clean(vendor) : nullptr;
    kv(g, &row, "Manufacturer:", vendor_clean);
    g_free(vendor_clean);
    char *loc = dev != nullptr ? location(dev) : g_strdup("Not available");
    kv(g, &row, "Location:", loc);
    g_free(loc);
    gtk_box_pack_start(GTK_BOX(gen), g, FALSE, FALSE, 0);
    const char *driver = dev != nullptr ? udev_device_get_driver(dev) : nullptr;
    if (driver == nullptr && dev != nullptr && udev_device_get_parent(dev) != nullptr)
        driver = udev_device_get_driver(udev_device_get_parent(dev));
    GtkWidget *status = sysdm_label(
        dev == nullptr ? "This device is no longer present."
        : driver != nullptr || c == CAT_PROCESSORS || c == CAT_COMPUTER || c == CAT_MONITORS ||
                c == CAT_DISKS || c == CAT_OPTICAL || c == CAT_BATTERIES || c == CAT_KEYBOARDS ||
                c == CAT_MICE || c == CAT_HID
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
        struct udev_list_entry *e;
        udev_list_entry_foreach(e, udev_device_get_properties_list_entry(dev)) {
            GtkTreeIter it;
            gtk_list_store_append(ls, &it);
            gtk_list_store_set(ls, &it, 0, udev_list_entry_get_name(e), 1,
                               udev_list_entry_get_value(e), -1);
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
    gtk_widget_show_all(d);
}

static void open_selected(devmgr *dm) {
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(dm->view)), &m,
                                         &it))
        return;
    char *path = nullptr;
    char *name = nullptr;
    int c = -1;
    gtk_tree_model_get(m, &it, DM_SYSPATH, &path, DM_NAME, &name, DM_CAT, &c, -1);
    if (path != nullptr && c >= 0)
        device_properties(dm, path, name, c);
    else {
        GtkTreePath *tp = gtk_tree_model_get_path(m, &it);
        if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(dm->view), tp))
            gtk_tree_view_collapse_row(GTK_TREE_VIEW(dm->view), tp);
        else
            gtk_tree_view_expand_row(GTK_TREE_VIEW(dm->view), tp, FALSE);
        gtk_tree_path_free(tp);
    }
    g_free(path);
    g_free(name);
}

static void on_row_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    open_selected(data);
}

static void on_menu_properties([[maybe_unused]] GtkMenuItem *i, gpointer data) {
    open_selected(data);
}

static void on_menu_scan([[maybe_unused]] GtkMenuItem *i, gpointer data) { fill(data); }

static void on_menu_hidden(GtkCheckMenuItem *i, gpointer data) {
    devmgr *dm = data;
    dm->show_hidden = gtk_check_menu_item_get_active(i);
    fill(dm);
}

static void on_menu_close([[maybe_unused]] GtkMenuItem *i, gpointer data) {
    devmgr *dm = data;
    gtk_widget_destroy(dm->window);
}

static void on_devmgr_destroy([[maybe_unused]] GtkWidget *w, gpointer data) { g_free(data); }

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data) {
    GtkWidget *i = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(i, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
    return i;
}

void sysdm_device_manager(sysdm *s) {
    devmgr *dm = g_new0(devmgr, 1);
    dm->s = s;
    dm->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_application(GTK_WINDOW(dm->window), s->app);
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

static void on_modules([[maybe_unused]] GtkButton *b, gpointer data) {
    sysdm *s = data;
    GtkWidget *d = nullptr;
    GtkWidget *box = sysdm_dialog(s, "Kernel Modules", "application-x-sharedlib", FALSE, nullptr);
    d = gtk_widget_get_toplevel(box);
    gtk_window_set_resizable(GTK_WINDOW(d), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(d), 440, 460);
    gtk_box_pack_start(GTK_BOX(box),
                       sysdm_label("Linux finds drivers for your hardware by itself and loads them "
                                   "as kernel modules. These modules are loaded now:"),
                       FALSE, FALSE, 0);
    GtkListStore *ls = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_STRING);
    char *text = nullptr;
    if (g_file_get_contents("/proc/modules", &text, nullptr, nullptr)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l != nullptr; l++) {
            char **f = g_strsplit(*l, " ", 6);
            if (g_strv_length(f) >= 4) {
                char *users = g_strdup(strcmp(f[3], "-") == 0 ? "" : f[3]);
                g_strdelimit(users, ",", ' ');
                GtkTreeIter it;
                gtk_list_store_append(ls, &it);
                gtk_list_store_set(ls, &it, 0, f[0], 1, g_ascii_strtoull(f[1], nullptr, 10) / 1024,
                                   2, g_strstrip(users), -1);
                g_free(users);
            }
            g_strfreev(f);
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
    gtk_widget_show_all(d);
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
    GBytes *b = g_bytes_new(data + 4, len - 4);
    g_free(data);
    return b;
}

static void on_signing([[maybe_unused]] GtkButton *b, gpointer data) {
    sysdm *s = data;
    GtkWidget *box = sysdm_dialog(s, "Driver Signing Options", "security-high", FALSE, nullptr);
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

    GtkWidget *g = kv_grid();
    int row = 0;
    kv(g, &row, "Secure Boot:", secure);
    char *lock = nullptr;
    if (lockdown != nullptr) {
        /* "[none] integrity confidentiality": the bracketed one is active. */
        char *open = strchr(lockdown, '[');
        char *close = open != nullptr ? strchr(open, ']') : nullptr;
        if (close != nullptr)
            lock = g_strndup(open + 1, (gsize)(close - open - 1));
    }
    kv(g, &row, "Kernel lockdown:", lock != nullptr ? lock : "Unknown");
    GString *t = g_string_new(nullptr);
    if (tainted == 0)
        g_string_append(t, "No");
    if (tainted & 1)
        g_string_append(t, "Proprietary module loaded. ");
    if (tainted & (1u << 12))
        g_string_append(t, "Out-of-tree module loaded. ");
    if (tainted & (1u << 13))
        g_string_append(t, "Unsigned module loaded. ");
    if (tainted != 0 && t->len == 0)
        g_string_append_printf(t, "Yes (flags %" G_GUINT64_FORMAT ")", tainted);
    kv(g, &row, "Kernel tainted:", g_strstrip(t->str));
    gtk_box_pack_start(GTK_BOX(box), sysdm_group("Status", g), FALSE, FALSE, 0);
    g_string_free(t, TRUE);
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
} power_dlg;

static void on_power_response(GtkDialog *d, int response, gpointer data) {
    power_dlg *p = data;
    if (response == GTK_RESPONSE_OK) {
        for (GSList *l = p->radios; l != nullptr; l = l->next) {
            if (!gtk_toggle_button_get_active(l->data))
                continue;
            const char *profile = g_object_get_data(l->data, "profile");
            GError *err = nullptr;
            GVariant *r = g_dbus_proxy_call_sync(
                p->proxy, "org.freedesktop.DBus.Properties.Set",
                g_variant_new("(ssv)", g_dbus_proxy_get_interface_name(p->proxy), "ActiveProfile",
                              g_variant_new_string(profile)),
                G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 10000, nullptr, &err);
            if (r == nullptr) {
                g_dbus_error_strip_remote_error(err);
                xd_message_box(GTK_WINDOW(d), "Power Profiles", XD_MB_ERROR, XD_MB_OK, FALSE,
                               err->message);
                g_error_free(err);
                return;
            }
            g_variant_unref(r);
        }
    }
    g_slist_free(p->radios);
    g_object_unref(p->proxy);
    g_free(p);
    gtk_widget_destroy(GTK_WIDGET(d));
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

static void on_power(GtkButton *b, gpointer data) {
    sysdm *s = data;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.freedesktop.UPower.PowerProfiles",
        "/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles", nullptr,
        nullptr);
    GVariant *profiles = proxy != nullptr ? g_dbus_proxy_get_cached_property(proxy, "Profiles")
                                          : nullptr;
    if (profiles == nullptr) {
        g_clear_object(&proxy);
        xd_message_box(GTK_WINDOW(s->window), "Power Profiles", XD_MB_INFO, XD_MB_OK, FALSE,
                       "Power profiles aren't available. Install and start "
                       "power-profiles-daemon (or tuned-ppd) to use them.");
        gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
        return;
    }
    GVariant *active_v = g_dbus_proxy_get_cached_property(proxy, "ActiveProfile");
    const char *active = active_v != nullptr ? g_variant_get_string(active_v, nullptr) : "";

    power_dlg *p = g_new0(power_dlg, 1);
    p->proxy = proxy;
    GtkWidget *box = sysdm_dialog(s, "Power Profiles", "preferences-system-power-management", TRUE,
                                  &p->dialog);
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
            GtkWidget *r = first == nullptr
                               ? gtk_radio_button_new_with_label(nullptr, profile_label(id))
                               : gtk_radio_button_new_with_label_from_widget(
                                     GTK_RADIO_BUTTON(first), profile_label(id));
            if (first == nullptr)
                first = r;
            g_object_set_data_full(G_OBJECT(r), "profile", g_strdup(id), g_free);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r), strcmp(id, active) == 0);
            gtk_box_pack_start(GTK_BOX(radios), r, FALSE, FALSE, 0);
            p->radios = g_slist_append(p->radios, r);
        }
        g_variant_unref(dict);
    }
    gtk_box_pack_start(GTK_BOX(box), sysdm_group("Power profile", radios), FALSE, FALSE, 0);
    g_signal_connect(p->dialog, "response", G_CALLBACK(on_power_response), p);
    gtk_widget_show_all(p->dialog);
    g_variant_unref(profiles);
    if (active_v != nullptr)
        g_variant_unref(active_v);
}

/* ---- the tab ------------------------------------------------------------------------- */

static void on_devmgr([[maybe_unused]] GtkButton *b, gpointer data) { sysdm_device_manager(data); }

GtkWidget *sysdm_hardware_page(sysdm *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    gtk_box_pack_start(
        GTK_BOX(page),
        sysdm_group_with_icon("Drivers", "application-x-sharedlib",
                              "Linux finds and loads the drivers your hardware needs by itself. "
                              "Kernel Modules lists the drivers loaded now.",
                              sysdm_button_row(sysdm_button("_Kernel Modules...",
                                                            G_CALLBACK(on_modules), s),
                                               nullptr)),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(page),
        sysdm_group_with_icon("Device Manager", "computer",
                              "The Device Manager lists all the hardware devices installed on "
                              "your computer. Use the Device Manager to see the properties of "
                              "any device.",
                              sysdm_button_row(sysdm_button("_Driver Signing...",
                                                            G_CALLBACK(on_signing), s),
                                               sysdm_button("D_evice Manager...",
                                                            G_CALLBACK(on_devmgr), s),
                                               nullptr)),
        FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(page),
        sysdm_group_with_icon("Power Profiles", "preferences-system-power-management",
                              "Power profiles set how fast this computer runs against how much "
                              "power it uses.",
                              sysdm_button_row(sysdm_button("_Power Profiles...",
                                                            G_CALLBACK(on_power), s),
                                               nullptr)),
        FALSE, FALSE, 0);
    return page;
}
