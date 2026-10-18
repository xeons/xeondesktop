/* SPDX-License-Identifier: MIT
 * Firmware and Device Security: fwupd over D-Bus (its client library
 * isn't needed for the little we ask). GetDevices and each updatable
 * device's GetUpgrades (from the LVFS metadata fwupd keeps); an update is
 * downloaded here (libsoup, or a file:// location of a local remote),
 * checked against the release's checksum and handed to Install as a file
 * descriptor; fwupd asks polkit. The Host Security ID and its attributes
 * come from HostSecurityId and GetHostSecurityAttrs.
 */
#include <gio/gunixfdlist.h>
#include <errno.h>
#include <libsoup/soup.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../appwiz/programs.h"
#include "../gtkapp/gtkapp.h"
#include "updlib.h"
#include "wuaucpl.h"

/* fwupd's FwupdDeviceFlags, FwupdStatus and FwupdSecurityAttrFlags. */
static constexpr guint64 DEVICE_UPDATABLE = 1u << 1;
static constexpr guint64 DEVICE_REQUIRE_AC = 1u << 3;
static constexpr guint64 DEVICE_NEEDS_REBOOT = 1u << 8;
static constexpr guint64 DEVICE_NEEDS_SHUTDOWN = 1u << 17;
static constexpr guint64 ATTR_SUCCESS = 1u << 0;
static constexpr guint64 ATTR_OBSOLETED = 1u << 1;
static constexpr guint64 ATTR_RUNTIME_ISSUE = 1u << 10;

/* No firmware file is this big; a server that sends more is refused. */
static constexpr guint64 MAX_DOWNLOAD = 512u * 1024 * 1024;

enum {
    FW_ICON,
    FW_NAME,
    FW_VENDOR,
    FW_VERSION,
    FW_AVAILABLE, /* what the Update column says */
    FW_DEVICE_ID,
    FW_FLAGS,
    FW_NEW_VERSION, /* "" without an update */
    FW_SUMMARY,
    FW_DESCRIPTION, /* plain text */
    FW_URGENCY,
    FW_SIZE,
    FW_CHECKSUM,
    FW_LOCATION,
    FW_COUNT
};

static const char *vstr(GVariantDict *d, const char *key) {
    const char *s = nullptr;
    return g_variant_dict_lookup(d, key, "&s", &s) ? s : nullptr;
}

static guint64 vu64(GVariantDict *d, const char *key) {
    guint64 v = 0;
    return g_variant_dict_lookup(d, key, "t", &v) ? v : 0;
}

static unsigned vu32(GVariantDict *d, const char *key) {
    guint32 v = 0;
    return g_variant_dict_lookup(d, key, "u", &v) ? v : 0;
}

/* ---- the scheduled check ------------------------------------------------------------ */

int xd_wuau_firmware_check(GDBusConnection *bus, int *important, GPtrArray *ids) {
    if (bus == nullptr)
        return -1;
    GVariant *r = g_dbus_connection_call_sync(bus, FWUPD_NAME, FWUPD_PATH, FWUPD_IFACE,
                                              "GetDevices", nullptr, G_VARIANT_TYPE("(aa{sv})"),
                                              G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, nullptr);
    if (r == nullptr)
        return -1;
    int n = 0;
    GVariant *devices = g_variant_get_child_value(r, 0);
    for (gsize i = 0; i < g_variant_n_children(devices); i++) {
        GVariant *dev = g_variant_get_child_value(devices, i);
        GVariantDict d;
        g_variant_dict_init(&d, dev);
        const char *id = vstr(&d, "DeviceId");
        if (id != nullptr && (vu64(&d, "Flags") & DEVICE_UPDATABLE) != 0) {
            GVariant *u = g_dbus_connection_call_sync(
                bus, FWUPD_NAME, FWUPD_PATH, FWUPD_IFACE, "GetUpgrades", g_variant_new("(s)", id),
                G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, nullptr);
            GVariant *releases = u != nullptr ? g_variant_get_child_value(u, 0) : nullptr;
            if (releases != nullptr && g_variant_n_children(releases) > 0) {
                GVariant *rel = g_variant_get_child_value(releases, 0);
                GVariantDict rd;
                g_variant_dict_init(&rd, rel);
                n++;
                if (xd_update_rank_important(xd_update_rank_of_urgency(vu32(&rd, "Urgency"))))
                    ++*important;
                g_ptr_array_add(ids, g_strdup_printf("fwupd/%s/%s", id,
                                                     vstr(&rd, "Version") != nullptr
                                                         ? vstr(&rd, "Version")
                                                         : ""));
                g_variant_dict_clear(&rd);
                g_variant_unref(rel);
            }
            if (releases != nullptr)
                g_variant_unref(releases);
            if (u != nullptr)
                g_variant_unref(u);
        }
        g_variant_dict_clear(&d);
        g_variant_unref(dev);
    }
    g_variant_unref(devices);
    g_variant_unref(r);
    return n;
}

/* ---- Firmware ---------------------------------------------------------------------- */

typedef struct fw_ctx {
    wuau *w;
    char *device_id;
} fw_ctx;

static int fw_pending;

static void fw_status(wuau *w, const char *text) {
    gtk_label_set_text(GTK_LABEL(w->fw_status), text);
}

static bool fw_find(wuau *w, const char *id, GtkTreeIter *out) {
    GtkTreeModel *m = GTK_TREE_MODEL(w->fw_store);
    for (bool ok = gtk_tree_model_get_iter_first(m, out); ok; ok = gtk_tree_model_iter_next(m, out)) {
        char *row = nullptr;
        gtk_tree_model_get(m, out, FW_DEVICE_ID, &row, -1);
        bool same = g_strcmp0(row, id) == 0;
        g_free(row);
        if (same)
            return true;
    }
    return false;
}

void xd_wuau_firmware_ids(wuau *w, int *important, GPtrArray *ids) {
    GtkTreeIter it;
    GtkTreeModel *m = GTK_TREE_MODEL(w->fw_store);
    for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        char *id = nullptr, *version = nullptr;
        unsigned urgency = 0;
        gtk_tree_model_get(m, &it, FW_DEVICE_ID, &id, FW_NEW_VERSION, &version, FW_URGENCY,
                           &urgency, -1);
        if (version != nullptr && version[0] != '\0') {
            if (xd_update_rank_important(xd_update_rank_of_urgency(urgency)))
                ++*important;
            g_ptr_array_add(ids, g_strdup_printf("fwupd/%s/%s", id, version));
        }
        g_free(id);
        g_free(version);
    }
}

static void fw_show_details(wuau *w) {
    GtkTreeModel *m;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->fw_view));
    bool can = false;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) {
        gtk_label_set_text(GTK_LABEL(w->fw_details), "Select a device to see its firmware.");
        gtk_widget_set_sensitive(w->fw_install, FALSE);
        return;
    }
    char *name = nullptr, *vendor = nullptr, *version = nullptr, *next = nullptr;
    char *summary = nullptr, *description = nullptr;
    guint64 flags = 0, size = 0;
    unsigned urgency = 0;
    gtk_tree_model_get(m, &it, FW_NAME, &name, FW_VENDOR, &vendor, FW_VERSION, &version,
                       FW_NEW_VERSION, &next, FW_SUMMARY, &summary, FW_DESCRIPTION, &description,
                       FW_FLAGS, &flags, FW_SIZE, &size, FW_URGENCY, &urgency, -1);
    GString *s = g_string_new(nullptr);
    char *line = g_markup_printf_escaped("<b>%s</b>%s%s\nFirmware version %s.", name,
                                         vendor != nullptr && vendor[0] != '\0' ? ", by " : "",
                                         vendor != nullptr ? vendor : "",
                                         version != nullptr && version[0] != '\0' ? version
                                                                                  : "unknown");
    g_string_append(s, line);
    g_free(line);
    if ((flags & DEVICE_UPDATABLE) == 0) {
        g_string_append(s, " This device's firmware can't be updated from here.");
    } else if (next != nullptr && next[0] != '\0') {
        can = true;
        char *mb = xd_appwiz_format_size(size);
        const char *rank = xd_update_rank_name(xd_update_rank_of_urgency(urgency));
        line = g_markup_printf_escaped("\n\nVersion %s is available%s%s%s (%s). %s", next,
                                       rank[0] != '\0' ? " (" : "", rank,
                                       rank[0] != '\0' ? ")" : "", mb,
                                       summary != nullptr ? summary : "");
        g_string_append(s, line);
        g_free(line);
        g_free(mb);
        if (description != nullptr && description[0] != '\0') {
            line = g_markup_printf_escaped("\n\n%s", description);
            g_string_append(s, line);
            g_free(line);
        }
        if (flags & DEVICE_REQUIRE_AC)
            g_string_append(s, "\n\nPlug the computer in first: this update needs mains "
                               "power.");
        if (flags & (DEVICE_NEEDS_REBOOT | DEVICE_NEEDS_SHUTDOWN))
            g_string_append(s, "\n\nThe update finishes when the computer restarts.");
    } else {
        g_string_append(s, " It's up to date.");
    }
    gtk_label_set_markup(GTK_LABEL(w->fw_details), s->str);
    g_string_free(s, TRUE);
    gtk_widget_set_sensitive(w->fw_install, can && !w->busy);
    g_free(name);
    g_free(vendor);
    g_free(version);
    g_free(next);
    g_free(summary);
    g_free(description);
}

static void fw_done(wuau *w) {
    if (--fw_pending > 0)
        return;
    int n = 0, important = 0;
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    xd_wuau_firmware_ids(w, &important, ids);
    n = (int)ids->len;
    g_ptr_array_free(ids, TRUE);
    w->fw_updates = n;
    char *text = n == 0 ? g_strdup("The firmware of every device that can be updated is up to "
                                   "date.")
                        : g_strdup_printf("Firmware updates are available for %d %s. Select a "
                                          "device, then click Update.",
                                          n, n == 1 ? "device" : "devices");
    fw_status(w, text);
    g_free(text);
    xd_wuau_save_state(w, false);
    fw_show_details(w);
}

static void on_upgrades(GObject *src, GAsyncResult *res, gpointer data) {
    fw_ctx *c = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &error);
    wuau *w = c->w;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        g_free(c->device_id);
        g_free(c);
        return;
    }
    GtkTreeIter it;
    if (fw_find(w, c->device_id, &it)) {
        GVariant *releases = r != nullptr ? g_variant_get_child_value(r, 0) : nullptr;
        if (releases != nullptr && g_variant_n_children(releases) > 0) {
            GVariant *rel = g_variant_get_child_value(releases, 0);
            GVariantDict d;
            g_variant_dict_init(&d, rel);
            const char *version = vstr(&d, "Version");
            const char **locations = nullptr;
            g_variant_dict_lookup(&d, "Locations", "^a&s", &locations);
            const char *location = xd_fwupd_pick_location(locations, vstr(&d, "Uri"));
            char *text = xd_fwupd_markup_to_text(vstr(&d, "Description"));
            gtk_list_store_set(w->fw_store, &it, FW_AVAILABLE, version, FW_NEW_VERSION, version,
                               FW_SUMMARY, vstr(&d, "Summary"), FW_DESCRIPTION, text, FW_URGENCY,
                               vu32(&d, "Urgency"), FW_SIZE, vu64(&d, "Size"), FW_CHECKSUM,
                               vstr(&d, "Checksum"), FW_LOCATION, location, -1);
            g_free(text);
            g_free(locations);
            g_variant_dict_clear(&d);
            g_variant_unref(rel);
        } else {
            /* NothingToDo: fwupd's way of saying it's up to date. */
            bool nothing = error == nullptr ||
                           (g_dbus_error_is_remote_error(error) &&
                            g_str_has_suffix(g_dbus_error_get_remote_error(error) != nullptr
                                                 ? g_dbus_error_get_remote_error(error)
                                                 : "",
                                             ".NothingToDo"));
            gtk_list_store_set(w->fw_store, &it, FW_AVAILABLE,
                               nothing ? "Up to date" : "Unknown", -1);
        }
        if (releases != nullptr)
            g_variant_unref(releases);
    }
    g_clear_error(&error);
    if (r != nullptr)
        g_variant_unref(r);
    g_free(c->device_id);
    g_free(c);
    fw_done(w);
}

static void on_devices(GObject *src, GAsyncResult *res, gpointer data) {
    wuau *w = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &error);
    if (r == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            bool missing = g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
                           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER);
            g_dbus_error_strip_remote_error(error);
            char *text = missing ? g_strdup("Firmware can't be checked: fwupd, the firmware "
                                            "update service, isn't installed.")
                                 : g_strdup_printf("fwupd couldn't list the devices: %s",
                                                   error->message);
            fw_status(w, text);
            g_free(text);
        }
        g_error_free(error);
        return;
    }
    gtk_list_store_clear(w->fw_store);
    GVariant *devices = g_variant_get_child_value(r, 0);
    fw_pending = 1;
    for (gsize i = 0; i < g_variant_n_children(devices); i++) {
        GVariant *dev = g_variant_get_child_value(devices, i);
        GVariantDict d;
        g_variant_dict_init(&d, dev);
        const char *id = vstr(&d, "DeviceId");
        guint64 flags = vu64(&d, "Flags");
        const char **icons = nullptr;
        g_variant_dict_lookup(&d, "Icon", "^a&s", &icons);
        if (id != nullptr) {
            bool updatable = (flags & DEVICE_UPDATABLE) != 0;
            gtk_list_store_insert_with_values(
                w->fw_store, nullptr, -1, FW_ICON,
                icons != nullptr && icons[0] != nullptr ? icons[0] : "application-x-firmware",
                FW_NAME, vstr(&d, "Name"), FW_VENDOR, vstr(&d, "Vendor"), FW_VERSION,
                vstr(&d, "Version"), FW_AVAILABLE, updatable ? "Checking..." : "Not updatable",
                FW_DEVICE_ID, id, FW_FLAGS, flags, FW_NEW_VERSION, "", -1);
            if (updatable) {
                fw_ctx *c = g_new0(fw_ctx, 1);
                *c = (fw_ctx){w, g_strdup(id)};
                fw_pending++;
                g_dbus_proxy_call(w->fwupd, "GetUpgrades", g_variant_new("(s)", id),
                                  G_DBUS_CALL_FLAGS_NONE, -1, w->quit, on_upgrades, c);
            }
        }
        g_free(icons);
        g_variant_dict_clear(&d);
        g_variant_unref(dev);
    }
    g_variant_unref(devices);
    g_variant_unref(r);
    fw_done(w);
}

/* ---- Device Security ---------------------------------------------------------------- */

static const char *const level_names[] = {
    "Runtime", "Level 1: critical", "Level 2: important", "Level 3: theoretical",
    "Level 4: system protection", "Level 5: advanced",
};

static void on_attrs(GObject *src, GAsyncResult *res, gpointer data) {
    wuau *w = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &error);
    if (r == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_dbus_error_strip_remote_error(error);
            char *text = g_strdup_printf("fwupd couldn't report on this computer's security: %s",
                                         error->message);
            gtk_label_set_text(GTK_LABEL(w->hsi_text), text);
            g_free(text);
        }
        g_error_free(error);
        return;
    }
    gtk_tree_store_clear(w->hsi_store);
    GtkTreeIter groups[XD_ARRAY_LEN(level_names)];
    bool have[XD_ARRAY_LEN(level_names)] = {};
    GVariant *attrs = g_variant_get_child_value(r, 0);
    /* Levels 1 to 5, then what's found at run time, as fwupdmgr lists them. */
    for (size_t pass = 1; pass <= XD_ARRAY_LEN(level_names); pass++) {
        size_t level = pass % XD_ARRAY_LEN(level_names);
        for (gsize i = 0; i < g_variant_n_children(attrs); i++) {
            GVariant *attr = g_variant_get_child_value(attrs, i);
            GVariantDict d;
            g_variant_dict_init(&d, attr);
            guint64 flags = vu64(&d, "Flags");
            unsigned hsi = vu32(&d, "HsiLevel");
            if (flags & ATTR_RUNTIME_ISSUE)
                hsi = 0;
            if (hsi == level && (flags & ATTR_OBSOLETED) == 0) {
                if (!have[level]) {
                    gtk_tree_store_insert_with_values(w->hsi_store, &groups[level], nullptr, -1, 1,
                                                      level_names[level], -1);
                    have[level] = true;
                }
                const char *title = xd_fwupd_attr_title(vstr(&d, "AppstreamId"),
                                                        vstr(&d, "Title"), vstr(&d, "Name"));
                bool ok = (flags & ATTR_SUCCESS) != 0;
                gtk_tree_store_insert_with_values(
                    w->hsi_store, nullptr, &groups[level], -1, 0,
                    ok ? "security-high" : "security-low", 1, title, 2,
                    xd_fwupd_hsi_result_text(vu32(&d, "HsiResult")), -1);
            }
            g_variant_dict_clear(&d);
            g_variant_unref(attr);
        }
    }
    g_variant_unref(attrs);
    g_variant_unref(r);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(w->hsi_view));
}

static void load_security(wuau *w) {
    GVariant *v = g_dbus_proxy_get_cached_property(w->fwupd, "HostSecurityId");
    const char *hsi = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                          ? g_variant_get_string(v, nullptr)
                          : nullptr;
    bool runtime = false;
    int level = xd_fwupd_hsi_level(hsi, &runtime);
    char *head = level >= 0 ? g_markup_printf_escaped("<b>Host Security ID: HSI:%d%s</b>", level,
                                                      runtime ? "!" : "")
                            : g_strdup("<b>Host Security ID: unknown</b>");
    gtk_label_set_markup(GTK_LABEL(w->hsi_level), head);
    g_free(head);
    char *text = g_strdup_printf("%s%s", xd_fwupd_hsi_level_text(level),
                                 runtime ? " Something running on this computer weakens it, "
                                           "though: see Runtime below."
                                         : "");
    gtk_label_set_text(GTK_LABEL(w->hsi_text), text);
    g_free(text);
    if (v != nullptr)
        g_variant_unref(v);
    g_dbus_proxy_call(w->fwupd, "GetHostSecurityAttrs", nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                      w->quit, on_attrs, w);
}

static void list_devices(wuau *w) {
    fw_status(w, "Looking for devices...");
    gtk_widget_set_sensitive(w->fw_install, FALSE);
    g_dbus_proxy_call(w->fwupd, "GetDevices", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, w->quit,
                      on_devices, w);
}

static void on_proxy([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    wuau *w = data;
    GError *error = nullptr;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (proxy == nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            fw_status(w, "Firmware can't be checked: there's no system bus.");
            gtk_label_set_text(GTK_LABEL(w->hsi_text), "There's no system bus to ask fwupd.");
        }
        g_error_free(error);
        return;
    }
    w->fwupd = proxy;
    list_devices(w);
    load_security(w);
}

void xd_wuau_firmware_load(wuau *w) {
    if (w->fwupd != nullptr) {
        list_devices(w);
        return;
    }
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, FWUPD_NAME,
                             FWUPD_PATH, FWUPD_IFACE, w->quit, on_proxy, w);
}

/* ---- installing ---------------------------------------------------------------------- */

typedef struct flash {
    wuau *w;
    char *device_id, *name, *checksum, *location;
    guint64 flags;
    GCancellable *cancel;
    xd_wuau_progress *progress;
    gulong props_handler;
    SoupSession *session;
    SoupMessage *message;
} flash;

static void flash_free(flash *f) {
    wuau *w = f->w;
    if (f->props_handler != 0)
        g_signal_handler_disconnect(w->fwupd, f->props_handler);
    xd_wuau_progress_free(f->progress);
    g_clear_object(&f->message);
    g_clear_object(&f->session);
    g_object_unref(f->cancel);
    g_free(f->device_id);
    g_free(f->name);
    g_free(f->checksum);
    g_free(f->location);
    g_free(f);
    w->busy = false;
}

static void flash_failed(flash *f, const char *what, const char *detail) {
    wuau *w = f->w;
    bool cancelled = g_cancellable_is_cancelled(f->cancel);
    flash_free(f);
    if (!cancelled)
        xd_wuau_error(w, what, detail);
    fw_show_details(w);
}

static void on_fwupd_props([[maybe_unused]] GDBusProxy *p, [[maybe_unused]] GVariant *changed,
                           [[maybe_unused]] const char *const *invalidated, gpointer data) {
    flash *f = data;
    GVariant *s = g_dbus_proxy_get_cached_property(f->w->fwupd, "Status");
    GVariant *pc = g_dbus_proxy_get_cached_property(f->w->fwupd, "Percentage");
    unsigned status = s != nullptr ? g_variant_get_uint32(s) : 0;
    unsigned pct = pc != nullptr ? g_variant_get_uint32(pc) : 0;
    static const char *const texts[] = {
        [2] = "Loading...", [3] = "Unpacking the firmware...", [4] = "Restarting the device...",
        [5] = "Writing the firmware...", [6] = "Checking what was written...",
        [7] = "Scheduling the update...", [8] = "Downloading...", [9] = "Reading the device...",
        [10] = "Erasing the device...", [11] = "Waiting for permission...",
        [12] = "Waiting for the device...",
    };
    const char *text = status < XD_ARRAY_LEN(texts) ? texts[status] : nullptr;
    if (status != 1)
        xd_wuau_progress_set(f->progress, pct > 0 && pct <= 100 ? pct / 100.0 : -1.0, text);
    if (s != nullptr)
        g_variant_unref(s);
    if (pc != nullptr)
        g_variant_unref(pc);
}

static void on_installed(GObject *src, GAsyncResult *res, gpointer data) {
    flash *f = data;
    GError *error = nullptr;
    GVariant *r = g_dbus_proxy_call_with_unix_fd_list_finish(G_DBUS_PROXY(src), nullptr, res,
                                                             &error);
    if (r == nullptr) {
        char *remote = g_dbus_error_get_remote_error(error);
        bool denied = remote != nullptr && g_str_has_suffix(remote, ".AuthFailed");
        g_free(remote);
        g_dbus_error_strip_remote_error(error);
        flash_failed(f,
                     denied ? "You don't have permission to update firmware on this computer."
                            : "The firmware could not be updated.",
                     error->message);
        g_error_free(error);
        return;
    }
    g_variant_unref(r);
    wuau *w = f->w;
    bool restart = (f->flags & (DEVICE_NEEDS_REBOOT | DEVICE_NEEDS_SHUTDOWN)) != 0;
    char *name = g_strdup(f->name);
    flash_free(f);
    xd_wuau_firmware_load(w);
    if (restart) {
        xd_wuau_ask_restart(w, "The firmware update is ready.");
    } else {
        char *text = g_strdup_printf("The firmware of %s was updated successfully.", name);
        xd_message_box(GTK_WINDOW(w->window), XD_WUAU_TITLE, XD_MB_INFO, XD_MB_OK, FALSE, text);
        g_free(text);
    }
    g_free(name);
}

/* The cabinet file is checked; hands it to fwupd. */
static void install_bytes(flash *f, GBytes *data) {
    if (!xd_fwupd_checksum_ok(f->checksum, data)) {
        flash_failed(f, "The firmware wasn't installed: the file downloaded isn't the one the "
                        "update describes (its checksum doesn't match).",
                     nullptr);
        return;
    }
    /* In memory, not on disk: fwupd reads it from the descriptor. */
    int fd = memfd_create("firmware.cab", MFD_CLOEXEC);
    gsize len = 0;
    const char *bytes = g_bytes_get_data(data, &len);
    bool ok = fd >= 0;
    for (gsize done = 0; ok && done < len;) {
        ssize_t n = write(fd, bytes + done, len - done);
        if (n < 0 && errno == EINTR)
            continue;
        ok = n > 0;
        done += ok ? (gsize)n : 0;
    }
    ok = ok && lseek(fd, 0, SEEK_SET) == 0;
    if (!ok) {
        if (fd >= 0)
            close(fd);
        flash_failed(f, "The firmware could not be updated.", g_strerror(errno));
        return;
    }
    GUnixFDList *fds = g_unix_fd_list_new();
    int index = g_unix_fd_list_append(fds, fd, nullptr);
    close(fd);
    /* Flashing can't be stopped half way. */
    xd_wuau_progress_cancellable(f->progress, false);
    xd_wuau_progress_set(f->progress, -1, "Waiting for permission...");
    f->props_handler = g_signal_connect(f->w->fwupd, "g-properties-changed",
                                        G_CALLBACK(on_fwupd_props), f);
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_dbus_proxy_call_with_unix_fd_list(
        f->w->fwupd, "Install",
        g_variant_new("(sha{sv})", f->device_id, index, &options),
        G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, fds, nullptr, on_installed,
        f);
    g_object_unref(fds);
}

static void on_file_loaded(GObject *src, GAsyncResult *res, gpointer data) {
    flash *f = data;
    GError *error = nullptr;
    char *contents = nullptr;
    gsize len = 0;
    if (!g_file_load_contents_finish(G_FILE(src), res, &contents, &len, nullptr, &error)) {
        flash_failed(f, "The firmware could not be downloaded.", error->message);
        g_error_free(error);
        return;
    }
    GBytes *bytes = g_bytes_new_take(contents, len);
    install_bytes(f, bytes);
    g_bytes_unref(bytes);
}

static void on_downloaded(GObject *src, GAsyncResult *res, gpointer data) {
    flash *f = data;
    GError *error = nullptr;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &error);
    unsigned status = soup_message_get_status(f->message);
    if (bytes == nullptr || status != SOUP_STATUS_OK ||
        g_bytes_get_size(bytes) > MAX_DOWNLOAD) {
        char *why = error != nullptr ? g_strdup(error->message)
                                     : g_strdup_printf("The server answered %u %s.", status,
                                                       soup_message_get_reason_phrase(f->message));
        flash_failed(f, "The firmware could not be downloaded.", why);
        g_free(why);
        g_clear_error(&error);
        if (bytes != nullptr)
            g_bytes_unref(bytes);
        return;
    }
    install_bytes(f, bytes);
    g_bytes_unref(bytes);
}

static void on_fw_install([[maybe_unused]] GtkButton *b, gpointer data) {
    wuau *w = data;
    GtkTreeModel *m;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->fw_view));
    if (w->busy || w->fwupd == nullptr || !gtk_tree_selection_get_selected(sel, &m, &it))
        return;
    flash *f = g_new0(flash, 1);
    f->w = w;
    char *version = nullptr, *next = nullptr;
    gtk_tree_model_get(m, &it, FW_DEVICE_ID, &f->device_id, FW_NAME, &f->name, FW_VERSION,
                       &version, FW_NEW_VERSION, &next, FW_CHECKSUM, &f->checksum, FW_LOCATION,
                       &f->location, FW_FLAGS, &f->flags, -1);
    f->cancel = g_cancellable_new();
    char *q = g_strdup_printf("Do you want to update the firmware of %s from version %s to %s?"
                              "\n\nDon't turn off or unplug the computer while its firmware is "
                              "being updated.",
                              f->name, version != nullptr ? version : "", next);
    int yes = xd_message_box(GTK_WINDOW(w->window), XD_WUAU_TITLE, XD_MB_QUESTION, XD_MB_YES_NO,
                             FALSE, q);
    g_free(q);
    g_free(version);
    g_free(next);
    w->busy = true;
    if (yes != GTK_RESPONSE_YES) {
        flash_free(f);
        return;
    }
    if (f->location == nullptr) {
        flash_failed(f, "The firmware could not be downloaded: fwupd doesn't say where from.",
                     nullptr);
        return;
    }
    gtk_widget_set_sensitive(w->fw_install, FALSE);
    char *text = g_strdup_printf("Please wait while the firmware of %s is updated.", f->name);
    f->progress = xd_wuau_progress_new(w, text, f->cancel);
    g_free(text);
    xd_wuau_progress_set(f->progress, -1, "Downloading...");
    if (g_str_has_prefix(f->location, "file://")) {
        GFile *file = g_file_new_for_uri(f->location);
        g_file_load_contents_async(file, f->cancel, on_file_loaded, f);
        g_object_unref(file);
        return;
    }
    GVariant *v = g_dbus_proxy_get_cached_property(w->fwupd, "DaemonVersion");
    char *agent = g_strdup_printf("xd-wuaucpl fwupd/%s",
                                  v != nullptr ? g_variant_get_string(v, nullptr) : "unknown");
    if (v != nullptr)
        g_variant_unref(v);
    f->session = soup_session_new_with_options("user-agent", agent, nullptr);
    g_free(agent);
    f->message = soup_message_new("GET", f->location);
    if (f->message == nullptr) {
        flash_failed(f, "The firmware could not be downloaded: its address isn't valid.",
                     f->location);
        return;
    }
    soup_session_send_and_read_async(f->session, f->message, G_PRIORITY_DEFAULT, f->cancel,
                                     on_downloaded, f);
}

/* ---- the pages ---------------------------------------------------------------------- */

static void on_fw_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    fw_show_details(data);
}

static void column(GtkWidget *view, const char *title, int col, bool expand) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    if (expand)
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col,
                                                                    nullptr);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_expand(c, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
}

static GtkWidget *scrolled(GtkWidget *child, int height) {
    GtkWidget *s = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(s), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(s, -1, height);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

GtkWidget *xd_wuau_firmware_page(wuau *w) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       xd_wuau_label("Firmware is the software built into the computer and its "
                                     "devices. Updates come from their makers through the Linux "
                                     "Vendor Firmware Service."),
                       FALSE, FALSE, 0);
    w->fw_status = xd_wuau_label("Looking for devices...");
    gtk_box_pack_start(GTK_BOX(page), w->fw_status, FALSE, FALSE, 0);
    w->fw_store = gtk_list_store_new(FW_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT,
                                     G_TYPE_UINT64, G_TYPE_STRING, G_TYPE_STRING);
    w->fw_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(w->fw_store));
    GtkTreeViewColumn *device = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(device, "Device");
    gtk_tree_view_column_set_expand(device, TRUE);
    gtk_tree_view_column_set_resizable(device, TRUE);
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(device, icon, FALSE);
    gtk_tree_view_column_add_attribute(device, icon, "icon-name", FW_ICON);
    GtkCellRenderer *name = gtk_cell_renderer_text_new();
    g_object_set(name, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_column_pack_start(device, name, TRUE);
    gtk_tree_view_column_add_attribute(device, name, "text", FW_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(w->fw_view), device);
    column(w->fw_view, "Version", FW_VERSION, false);
    column(w->fw_view, "Update", FW_AVAILABLE, false);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w->fw_view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_fw_selection), w);
    gtk_box_pack_start(GTK_BOX(page), scrolled(w->fw_view, 130), TRUE, TRUE, 0);

    w->fw_details = xd_wuau_label("Select a device to see its firmware.");
    gtk_label_set_yalign(GTK_LABEL(w->fw_details), 0);
    gtk_label_set_selectable(GTK_LABEL(w->fw_details), TRUE);
    GtkWidget *details = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(details), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(details, -1, 110);
    gtk_container_add(GTK_CONTAINER(details), w->fw_details);
    gtk_box_pack_start(GTK_BOX(page), xd_wuau_frame("Details", details), FALSE, FALSE, 0);

    w->fw_install = gtk_button_new_with_mnemonic("U_pdate...");
    gtk_widget_set_size_request(w->fw_install, 80, -1);
    gtk_widget_set_halign(w->fw_install, GTK_ALIGN_END);
    gtk_widget_set_sensitive(w->fw_install, FALSE);
    g_signal_connect(w->fw_install, "clicked", G_CALLBACK(on_fw_install), w);
    gtk_box_pack_start(GTK_BOX(page), w->fw_install, FALSE, FALSE, 0);
    return page;
}

GtkWidget *xd_wuau_security_page(wuau *w) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("security-medium", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(head), img, FALSE, FALSE, 0);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    w->hsi_level = xd_wuau_label("");
    gtk_label_set_markup(GTK_LABEL(w->hsi_level), "<b>Host Security ID</b>");
    gtk_box_pack_start(GTK_BOX(texts), w->hsi_level, FALSE, FALSE, 0);
    w->hsi_text = xd_wuau_label("Asking fwupd...");
    gtk_box_pack_start(GTK_BOX(texts), w->hsi_text, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), texts, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       xd_wuau_label("The Host Security ID says how well the computer's "
                                     "hardware and firmware protect it, from level 1 (the "
                                     "critical protections) up. Each protection below either "
                                     "holds or doesn't; a computer reaches a level when every "
                                     "protection of that level and those below it holds."),
                       FALSE, FALSE, 0);

    w->hsi_store = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    w->hsi_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(w->hsi_store));
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Protection");
    gtk_tree_view_column_set_expand(c, TRUE);
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(c, icon, FALSE);
    gtk_tree_view_column_add_attribute(c, icon, "icon-name", 0);
    GtkCellRenderer *title = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(c, title, TRUE);
    gtk_tree_view_column_add_attribute(c, title, "text", 1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(w->hsi_view), c);
    column(w->hsi_view, "Result", 2, false);
    gtk_box_pack_start(GTK_BOX(page), scrolled(w->hsi_view, 200), TRUE, TRUE, 0);
    return page;
}

void xd_wuau_firmware_finish(wuau *w) {
    g_clear_object(&w->fwupd);
    g_clear_object(&w->fw_store);
    g_clear_object(&w->hsi_store);
}
