/* SPDX-License-Identifier: MIT
 * xd-netconn's Status dialog, as XP's "Local Area Connection Status":
 *
 *   General  status, duration, speed (Wi-Fi: network and signal), bytes
 *            and packets sent and received, updated every second
 *   Support  how the address was assigned, the address, mask and
 *            gateway; Repair, and Details... for the rest
 *
 * Traffic comes from the kernel's counters in sysfs. NetworkManager
 * doesn't say when a connection came up, so the duration counts from
 * when we first saw it up, remembered for the session in
 * $XDG_RUNTIME_DIR/xeondesktop/netconn-up.ini.
 */
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "app.h"

typedef struct status_dlg {
    nc_app *app;
    char *key;
    GtkWidget *dialog, *notebook;
    GtkWidget *status, *duration, *speed, *network, *signal;
    GtkWidget *sent[2], *received[2]; /* bytes, packets */
    GtkWidget *addr_type, *address, *netmask, *gateway;
    guint timer;
} status_dlg;

/* ---- when connections came up ------------------------------------------------------ */

static char *up_file(void) {
    return g_build_filename(g_get_user_runtime_dir(), "xeondesktop", "netconn-up.ini", nullptr);
}

guint64 nc_up_seconds(nc_app *app, NMActiveConnection *active) {
    if (active == nullptr)
        return 0;
    char *file = up_file();
    if (app->up_since == nullptr) {
        app->up_since = g_key_file_new();
        g_key_file_load_from_file(app->up_since, file, G_KEY_FILE_NONE, nullptr);
    }
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    /* By connection; the active connection's path tells this activation
     * from the last. */
    const char *uuid = nm_active_connection_get_uuid(active);
    const char *path = nm_object_get_path(NM_OBJECT(active));
    if (uuid == nullptr || path == nullptr) {
        g_free(file);
        return 0;
    }
    char *seen = g_key_file_get_string(app->up_since, uuid, "Path", nullptr);
    gint64 since = g_key_file_get_int64(app->up_since, uuid, "Since", nullptr);
    if (seen == nullptr || strcmp(seen, path) != 0 || since <= 0 || since > now) {
        since = now;
        g_key_file_set_string(app->up_since, uuid, "Path", path);
        g_key_file_set_int64(app->up_since, uuid, "Since", since);
        char *dir = g_path_get_dirname(file);
        g_mkdir_with_parents(dir, 0700);
        g_key_file_save_to_file(app->up_since, file, nullptr);
        g_free(dir);
    }
    g_free(seen);
    g_free(file);
    return (guint64)(now - since);
}

/* ---- addresses ------------------------------------------------------------------------- */

static char *lease_time(gint64 unix_time) {
    GDateTime *dt = unix_time > 0 ? g_date_time_new_from_unix_local(unix_time) : nullptr;
    char *text = dt != nullptr ? g_date_time_format(dt, "%x %X") : g_strdup("");
    if (dt != nullptr)
        g_date_time_unref(dt);
    return text;
}

void nc_ipv4_info_get(NMDevice *device, NMActiveConnection *active, nc_ipv4_info *out) {
    *out = (nc_ipv4_info){0};
    NMIPConfig *cfg = device != nullptr ? nm_device_get_ip4_config(device) : nullptr;
    if (cfg == nullptr && active != nullptr)
        cfg = nm_active_connection_get_ip4_config(active);
    NMDhcpConfig *dhcp = device != nullptr ? nm_device_get_dhcp4_config(device) : nullptr;
    if (dhcp == nullptr && active != nullptr)
        dhcp = nm_active_connection_get_dhcp4_config(active);
    GPtrArray *addrs = cfg != nullptr ? nm_ip_config_get_addresses(cfg) : nullptr;
    if (addrs != nullptr && addrs->len > 0) {
        NMIPAddress *address = g_ptr_array_index(addrs, 0);
        out->address = g_strdup(nm_ip_address_get_address(address));
        out->netmask = xd_nc_prefix_to_netmask(nm_ip_address_get_prefix(address));
    }
    const char *gw = cfg != nullptr ? nm_ip_config_get_gateway(cfg) : nullptr;
    out->gateway = g_strdup(gw != nullptr ? gw : "");
    const char *const *dns = cfg != nullptr ? nm_ip_config_get_nameservers(cfg) : nullptr;
    out->dns = dns != nullptr ? g_strjoinv(", ", (char **)(uintptr_t)dns) : g_strdup("");
    const char *server = dhcp != nullptr
                             ? nm_dhcp_config_get_one_option(dhcp, "dhcp_server_identifier")
                             : nullptr;
    out->dhcp_server = g_strdup(server != nullptr ? server : "");
    const char *expiry = dhcp != nullptr ? nm_dhcp_config_get_one_option(dhcp, "expiry") : nullptr;
    const char *length =
        dhcp != nullptr ? nm_dhcp_config_get_one_option(dhcp, "dhcp_lease_time") : nullptr;
    gint64 expires = expiry != nullptr ? g_ascii_strtoll(expiry, nullptr, 10) : 0;
    gint64 lease = length != nullptr ? g_ascii_strtoll(length, nullptr, 10) : 0;
    out->lease_expires = lease_time(expires);
    out->lease_obtained = lease_time(expires > 0 && lease > 0 ? expires - lease : 0);
    const char *mac = device != nullptr ? nm_device_get_hw_address(device) : nullptr;
    out->mac = g_strdup(mac != nullptr ? mac : "");
    /* By the connection's settings: there's no lease to show for some
     * DHCP clients. */
    NMRemoteConnection *connection =
        active != nullptr ? nm_active_connection_get_connection(active) : nullptr;
    NMSettingIPConfig *ip4 = connection != nullptr
                                 ? nm_connection_get_setting_ip4_config(NM_CONNECTION(connection))
                                 : nullptr;
    const char *method = ip4 != nullptr ? nm_setting_ip_config_get_method(ip4) : nullptr;
    out->dhcp = dhcp != nullptr || g_strcmp0(method, NM_SETTING_IP4_CONFIG_METHOD_AUTO) == 0;
    if (out->address == nullptr) {
        out->address = g_strdup("");
        out->netmask = g_strdup("");
    }
}

void nc_ipv4_info_clear(nc_ipv4_info *info) {
    g_free(info->address);
    g_free(info->netmask);
    g_free(info->gateway);
    g_free(info->dns);
    g_free(info->dhcp_server);
    g_free(info->mac);
    g_free(info->lease_obtained);
    g_free(info->lease_expires);
    *info = (nc_ipv4_info){0};
}

/* ---- traffic -------------------------------------------------------------------------- */

static bool counter(const char *iface, const char *name, guint64 *out) {
    if (iface == nullptr || strchr(iface, '/') != nullptr)
        return false;
    char *path = g_build_filename("/sys/class/net", iface, "statistics", name, nullptr);
    char *text = nullptr;
    bool ok = g_file_get_contents(path, &text, nullptr, nullptr);
    if (ok)
        ok = g_ascii_string_to_unsigned(g_strstrip(text), 10, 0, G_MAXUINT64, out, nullptr);
    g_free(text);
    g_free(path);
    return ok;
}

static void show_counter(GtkWidget *label_widget, const char *iface, const char *name) {
    guint64 count = 0;
    if (counter(iface, name, &count)) {
        char *text = xd_nc_format_count(count);
        gtk_label_set_text(GTK_LABEL(label_widget), text);
        g_free(text);
    } else {
        gtk_label_set_text(GTK_LABEL(label_widget), "—");
    }
}

/* ---- the dialog ----------------------------------------------------------------------- */

static void set_text(GtkWidget *label_widget, const char *text) {
    if (label_widget != nullptr)
        gtk_label_set_text(GTK_LABEL(label_widget), text != nullptr && text[0] != '\0' ? text : "");
}

static bool update(status_dlg *status_dialog) {
    nc_item *it = nc_find(status_dialog->app, status_dialog->key);
    /* XP closes Status when the connection goes. */
    if (it == nullptr || !it->connected)
        return false;
    set_text(status_dialog->status, it->status);
    char *text = xd_nc_format_duration(nc_up_seconds(status_dialog->app, it->active));
    set_text(status_dialog->duration, text);
    g_free(text);
    guint64 kbps = 0;
    if (it->device != nullptr && NM_IS_DEVICE_ETHERNET(it->device))
        kbps = (guint64)nm_device_ethernet_get_speed(NM_DEVICE_ETHERNET(it->device)) * 1000;
    else if (it->device != nullptr && NM_IS_DEVICE_WIFI(it->device))
        kbps = nm_device_wifi_get_bitrate(NM_DEVICE_WIFI(it->device));
    text = kbps > 0 ? xd_nc_format_speed(kbps) : g_strdup("—");
    set_text(status_dialog->speed, text);
    g_free(text);
    if (status_dialog->signal != nullptr && it->device != nullptr &&
        NM_IS_DEVICE_WIFI(it->device)) {
        NMAccessPoint *ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(it->device));
        GBytes *ssid = ap != nullptr ? nm_access_point_get_ssid(ap) : nullptr;
        char *name = ssid != nullptr
                         ? nm_utils_ssid_to_utf8(g_bytes_get_data(ssid, nullptr),
                                                 g_bytes_get_size(ssid))
                         : g_strdup(it->conn != nullptr
                                        ? nm_connection_get_id(NM_CONNECTION(it->conn))
                                        : "");
        set_text(status_dialog->network, name);
        g_free(name);
        unsigned strength = ap != nullptr ? nm_access_point_get_strength(ap) : 0;
        text = g_strdup_printf("%s (%u%%)", xd_nc_signal_words(strength), strength);
        set_text(status_dialog->signal, text);
        g_free(text);
    }
    const char *iface = it->device != nullptr ? nm_device_get_ip_iface(it->device) : nullptr;
    if (iface == nullptr && it->device != nullptr)
        iface = nm_device_get_iface(it->device);
    show_counter(status_dialog->sent[0], iface, "tx_bytes");
    show_counter(status_dialog->received[0], iface, "rx_bytes");
    show_counter(status_dialog->sent[1], iface, "tx_packets");
    show_counter(status_dialog->received[1], iface, "rx_packets");

    nc_ipv4_info info;
    nc_ipv4_info_get(it->device, it->active, &info);
    set_text(status_dialog->addr_type, info.dhcp ? "Assigned by DHCP" : "Manually Configured");
    set_text(status_dialog->address, info.address);
    set_text(status_dialog->netmask, info.netmask);
    set_text(status_dialog->gateway, info.gateway);
    nc_ipv4_info_clear(&info);
    return true;
}

static gboolean tick(gpointer data) {
    status_dlg *status_dialog = data;
    if (update(status_dialog))
        return G_SOURCE_CONTINUE;
    status_dialog->timer = 0;
    gtk_widget_destroy(status_dialog->dialog);
    return G_SOURCE_REMOVE;
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    status_dlg *status_dialog = data;
    if (status_dialog->timer != 0)
        g_source_remove(status_dialog->timer);
    g_free(status_dialog->key);
    g_free(status_dialog);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *frame_widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame_widget), child);
    return frame_widget;
}

static GtkWidget *grid(void) {
    GtkWidget *grid_widget = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_widget), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid_widget), 12);
    return grid_widget;
}

/* "Status:  Connected", the value on the right as in XP. */
static GtkWidget *row(GtkWidget *grid_widget, int y, const char *name) {
    GtkWidget *caption_label = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(caption_label), 0);
    gtk_widget_set_hexpand(caption_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid_widget), caption_label, 0, y, 1, 1);
    GtkWidget *value_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(value_label), 1);
    gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
    gtk_widget_set_can_focus(value_label, FALSE);
    gtk_grid_attach(GTK_GRID(grid_widget), value_label, 1, y, 1, 1);
    return value_label;
}

static void on_properties(status_dlg *status_dialog) {
    nc_properties_show(status_dialog->app, status_dialog->key);
}

static void on_disable(status_dlg *status_dialog) {
    nc_item *it = nc_find(status_dialog->app, status_dialog->key);
    if (it != nullptr && it->device != nullptr)
        nc_disable(status_dialog->app, status_dialog->key);
    else
        nc_disconnect(status_dialog->app, status_dialog->key);
    gtk_widget_destroy(status_dialog->dialog);
}

static void on_wireless(status_dlg *status_dialog) {
    nc_item *it = nc_find(status_dialog->app, status_dialog->key);
    if (it != nullptr && it->device != nullptr)
        nc_wireless_show(status_dialog->app, it->device);
}

static void on_repair(status_dlg *status_dialog) {
    nc_repair(status_dialog->app, status_dialog->key);
}

static void add_detail(GtkListStore *store, const char *name, const char *value) {
    if (value == nullptr || value[0] == '\0')
        return;
    gtk_list_store_insert_with_values(store, nullptr, -1, 0, name, 1, value, -1);
}

/* XP's Network Connection Details. */
static void on_details(status_dlg *status_dialog) {
    nc_item *it = nc_find(status_dialog->app, status_dialog->key);
    if (it == nullptr)
        return;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Network Connection Details", GTK_WINDOW(status_dialog->dialog),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Close",
        GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 360, 300);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *label_widget = gtk_label_new_with_mnemonic("Network Connection _Details:");
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    gtk_box_pack_start(GTK_BOX(box), label_widget, FALSE, FALSE, 0);
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    nc_ipv4_info info;
    nc_ipv4_info_get(it->device, it->active, &info);
    add_detail(store, "Physical Address", info.mac);
    add_detail(store, "IP Address", info.address);
    add_detail(store, "Subnet Mask", info.netmask);
    add_detail(store, "Default Gateway", info.gateway);
    add_detail(store, "DHCP Server", info.dhcp_server);
    add_detail(store, "Lease Obtained", info.lease_obtained);
    add_detail(store, "Lease Expires", info.lease_expires);
    add_detail(store, "DNS Servers", info.dns);
    nc_ipv4_info_clear(&info);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label_widget), tv);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
                                gtk_tree_view_column_new_with_attributes(
                                    "Property", gtk_cell_renderer_text_new(), "text", 0, nullptr));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
                                gtk_tree_view_column_new_with_attributes(
                                    "Value", gtk_cell_renderer_text_new(), "text", 1, nullptr));
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);
    /* Status closes itself when the connection goes, and this box with it:
     * keep it alive until we're done with it. */
    g_object_ref(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_object_unref(dlg);
}

static GtkWidget *button(const char *text, GCallback cb, status_dlg *status_dialog) {
    GtkWidget *button_widget = gtk_button_new_with_mnemonic(text);
    gtk_widget_set_size_request(button_widget, 90, -1);
    g_signal_connect_swapped(button_widget, "clicked", cb, status_dialog);
    return button_widget;
}

static GtkWidget *general_page(status_dlg *status_dialog, nc_item *it) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    bool wifi = it->device != nullptr && NM_IS_DEVICE_WIFI(it->device);
    GtkWidget *connection_grid = grid();
    int y = 0;
    status_dialog->status = row(connection_grid, y++, "Status:");
    if (wifi)
        status_dialog->network = row(connection_grid, y++, "Network:");
    status_dialog->duration = row(connection_grid, y++, "Duration:");
    status_dialog->speed = row(connection_grid, y++, "Speed:");
    if (wifi)
        status_dialog->signal = row(connection_grid, y++, "Signal Strength:");
    gtk_box_pack_start(GTK_BOX(page), frame("Connection", connection_grid), FALSE, FALSE, 0);

    /* Sent — computer — Received, as XP drew it. */
    GtkWidget *activity_grid = grid();
    GtkWidget *sent = gtk_label_new("Sent");
    gtk_label_set_xalign(GTK_LABEL(sent), 1);
    gtk_grid_attach(GTK_GRID(activity_grid), sent, 1, 0, 1, 1);
    GtkWidget *pc = gtk_image_new_from_icon_name(wifi ? "network-wireless" : "network-wired",
                                                 GTK_ICON_SIZE_DND);
    gtk_widget_set_hexpand(pc, TRUE);
    gtk_grid_attach(GTK_GRID(activity_grid), pc, 2, 0, 1, 1);
    GtkWidget *recv = gtk_label_new("Received");
    gtk_label_set_xalign(GTK_LABEL(recv), 0);
    gtk_grid_attach(GTK_GRID(activity_grid), recv, 3, 0, 1, 1);
    static const char *const names[] = {"Bytes:", "Packets:"};
    for (int i = 0; i < 2; i++) {
        GtkWidget *caption_label = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(caption_label), 0);
        gtk_grid_attach(GTK_GRID(activity_grid), caption_label, 0, i + 1, 1, 1);
        status_dialog->sent[i] = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(status_dialog->sent[i]), 1);
        gtk_grid_attach(GTK_GRID(activity_grid), status_dialog->sent[i], 1, i + 1, 1, 1);
        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
        gtk_widget_set_halign(sep, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(activity_grid), sep, 2, i + 1, 1, 1);
        status_dialog->received[i] = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(status_dialog->received[i]), 0);
        gtk_grid_attach(GTK_GRID(activity_grid), status_dialog->received[i], 3, i + 1, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(page), frame("Activity", activity_grid), FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(buttons),
                       button("P_roperties", G_CALLBACK(on_properties), status_dialog), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons),
                       button(it->device != nullptr ? "_Disable" : "_Disconnect",
                              G_CALLBACK(on_disable), status_dialog),
                       FALSE, FALSE, 0);
    if (wifi)
        gtk_box_pack_start(
            GTK_BOX(buttons),
            button("_View Wireless Networks", G_CALLBACK(on_wireless), status_dialog), FALSE, FALSE,
            0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    return page;
}

static GtkWidget *support_page(status_dlg *status_dialog) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("preferences-system-network", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(outer), img, FALSE, FALSE, 0);
    GtkWidget *address_grid = grid();
    status_dialog->addr_type = row(address_grid, 0, "Address Type:");
    status_dialog->address = row(address_grid, 1, "IP Address:");
    status_dialog->netmask = row(address_grid, 2, "Subnet Mask:");
    status_dialog->gateway = row(address_grid, 3, "Default Gateway:");
    gtk_box_pack_start(GTK_BOX(outer), address_grid, TRUE, TRUE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(col), outer, FALSE, FALSE, 0);
    GtkWidget *details = button("_Details...", G_CALLBACK(on_details), status_dialog);
    gtk_widget_set_halign(details, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(col), details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Connection status", col), FALSE, FALSE, 0);

    GtkWidget *repair = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *text = gtk_label_new("No problems were detected with this connection. If you "
                                    "cannot connect, click Repair.");
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 36);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_box_pack_start(GTK_BOX(repair), text, TRUE, TRUE, 0);
    GtkWidget *repair_button = button("Re_pair", G_CALLBACK(on_repair), status_dialog);
    gtk_widget_set_valign(repair_button, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(repair), repair_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), repair, FALSE, FALSE, 0);
    return page;
}

void nc_status_show(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || !it->connected)
        return;
    status_dlg *status_dialog = g_new0(status_dlg, 1);
    status_dialog->app = app;
    status_dialog->key = g_strdup(key);
    status_dialog->dialog = gtk_dialog_new();
    nc_track(app, status_dialog->dialog);
    char *title = g_strdup_printf("%s Status", it->name);
    gtk_window_set_title(GTK_WINDOW(status_dialog->dialog), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(status_dialog->dialog), it->icon);
    gtk_window_set_resizable(GTK_WINDOW(status_dialog->dialog), FALSE);
    if (nc_parent(app) != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(status_dialog->dialog), nc_parent(app));
    gtk_dialog_add_button(GTK_DIALOG(status_dialog->dialog), "_Close", GTK_RESPONSE_CLOSE);
    gtk_dialog_set_default_response(GTK_DIALOG(status_dialog->dialog), GTK_RESPONSE_CLOSE);
    g_signal_connect(status_dialog->dialog, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    g_signal_connect(status_dialog->dialog, "destroy", G_CALLBACK(on_destroy), status_dialog);

    status_dialog->notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(status_dialog->notebook), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(status_dialog->notebook), general_page(status_dialog, it),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(status_dialog->notebook), support_page(status_dialog),
                             gtk_label_new_with_mnemonic("_Support"));
    gtk_widget_set_size_request(status_dialog->notebook, 340, -1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(status_dialog->dialog))),
                       status_dialog->notebook, TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(status_dialog->dialog, GTK_NOTEBOOK(status_dialog->notebook));
    update(status_dialog);
    status_dialog->timer = g_timeout_add_seconds(1, tick, status_dialog);
    gtk_widget_show_all(status_dialog->dialog);
}
