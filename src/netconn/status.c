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

static char *lease_time(gint64 t) {
    GDateTime *dt = t > 0 ? g_date_time_new_from_unix_local(t) : nullptr;
    char *s = dt != nullptr ? g_date_time_format(dt, "%x %X") : g_strdup("");
    if (dt != nullptr)
        g_date_time_unref(dt);
    return s;
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
        NMIPAddress *a = g_ptr_array_index(addrs, 0);
        out->address = g_strdup(nm_ip_address_get_address(a));
        out->netmask = xd_nc_prefix_to_netmask(nm_ip_address_get_prefix(a));
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
    NMRemoteConnection *c = active != nullptr ? nm_active_connection_get_connection(active)
                                              : nullptr;
    NMSettingIPConfig *ip4 = c != nullptr ? nm_connection_get_setting_ip4_config(NM_CONNECTION(c))
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

static void show_counter(GtkWidget *l, const char *iface, const char *name) {
    guint64 n = 0;
    if (counter(iface, name, &n)) {
        char *s = xd_nc_format_count(n);
        gtk_label_set_text(GTK_LABEL(l), s);
        g_free(s);
    } else {
        gtk_label_set_text(GTK_LABEL(l), "—");
    }
}

/* ---- the dialog ----------------------------------------------------------------------- */

static void set_text(GtkWidget *l, const char *text) {
    if (l != nullptr)
        gtk_label_set_text(GTK_LABEL(l), text != nullptr && text[0] != '\0' ? text : "");
}

static bool update(status_dlg *d) {
    nc_item *it = nc_find(d->app, d->key);
    /* XP closes Status when the connection goes. */
    if (it == nullptr || !it->connected)
        return false;
    set_text(d->status, it->status);
    char *s = xd_nc_format_duration(nc_up_seconds(d->app, it->active));
    set_text(d->duration, s);
    g_free(s);
    guint64 kbps = 0;
    if (it->device != nullptr && NM_IS_DEVICE_ETHERNET(it->device))
        kbps = (guint64)nm_device_ethernet_get_speed(NM_DEVICE_ETHERNET(it->device)) * 1000;
    else if (it->device != nullptr && NM_IS_DEVICE_WIFI(it->device))
        kbps = nm_device_wifi_get_bitrate(NM_DEVICE_WIFI(it->device));
    s = kbps > 0 ? xd_nc_format_speed(kbps) : g_strdup("—");
    set_text(d->speed, s);
    g_free(s);
    if (d->signal != nullptr && it->device != nullptr && NM_IS_DEVICE_WIFI(it->device)) {
        NMAccessPoint *ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(it->device));
        GBytes *ssid = ap != nullptr ? nm_access_point_get_ssid(ap) : nullptr;
        char *name = ssid != nullptr
                         ? nm_utils_ssid_to_utf8(g_bytes_get_data(ssid, nullptr),
                                                 g_bytes_get_size(ssid))
                         : g_strdup(it->conn != nullptr
                                        ? nm_connection_get_id(NM_CONNECTION(it->conn))
                                        : "");
        set_text(d->network, name);
        g_free(name);
        unsigned strength = ap != nullptr ? nm_access_point_get_strength(ap) : 0;
        s = g_strdup_printf("%s (%u%%)", xd_nc_signal_words(strength), strength);
        set_text(d->signal, s);
        g_free(s);
    }
    const char *iface = it->device != nullptr ? nm_device_get_ip_iface(it->device) : nullptr;
    if (iface == nullptr && it->device != nullptr)
        iface = nm_device_get_iface(it->device);
    show_counter(d->sent[0], iface, "tx_bytes");
    show_counter(d->received[0], iface, "rx_bytes");
    show_counter(d->sent[1], iface, "tx_packets");
    show_counter(d->received[1], iface, "rx_packets");

    nc_ipv4_info info;
    nc_ipv4_info_get(it->device, it->active, &info);
    set_text(d->addr_type, info.dhcp ? "Assigned by DHCP" : "Manually Configured");
    set_text(d->address, info.address);
    set_text(d->netmask, info.netmask);
    set_text(d->gateway, info.gateway);
    nc_ipv4_info_clear(&info);
    return true;
}

static gboolean tick(gpointer data) {
    status_dlg *d = data;
    if (update(d))
        return G_SOURCE_CONTINUE;
    d->timer = 0;
    gtk_widget_destroy(d->dialog);
    return G_SOURCE_REMOVE;
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    status_dlg *d = data;
    if (d->timer != 0)
        g_source_remove(d->timer);
    g_free(d->key);
    g_free(d);
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *f = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *grid(void) {
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    return g;
}

/* "Status:  Connected", the value on the right as in XP. */
static GtkWidget *row(GtkWidget *g, int y, const char *name) {
    GtkWidget *l = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_grid_attach(GTK_GRID(g), l, 0, y, 1, 1);
    GtkWidget *v = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v), 1);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_can_focus(v, FALSE);
    gtk_grid_attach(GTK_GRID(g), v, 1, y, 1, 1);
    return v;
}

static void on_properties(status_dlg *d) { nc_properties_show(d->app, d->key); }

static void on_disable(status_dlg *d) {
    nc_item *it = nc_find(d->app, d->key);
    if (it != nullptr && it->device != nullptr)
        nc_disable(d->app, d->key);
    else
        nc_disconnect(d->app, d->key);
    gtk_widget_destroy(d->dialog);
}

static void on_wireless(status_dlg *d) {
    nc_item *it = nc_find(d->app, d->key);
    if (it != nullptr && it->device != nullptr)
        nc_wireless_show(d->app, it->device);
}

static void on_repair(status_dlg *d) { nc_repair(d->app, d->key); }

static void add_detail(GtkListStore *store, const char *name, const char *value) {
    if (value == nullptr || value[0] == '\0')
        return;
    gtk_list_store_insert_with_values(store, nullptr, -1, 0, name, 1, value, -1);
}

/* XP's Network Connection Details. */
static void on_details(status_dlg *d) {
    nc_item *it = nc_find(d->app, d->key);
    if (it == nullptr)
        return;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Network Connection Details", GTK_WINDOW(d->dialog),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 360, 300);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *l = gtk_label_new_with_mnemonic("Network Connection _Details:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
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
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), tv);
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
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static GtkWidget *button(const char *text, GCallback cb, status_dlg *d) {
    GtkWidget *b = gtk_button_new_with_mnemonic(text);
    gtk_widget_set_size_request(b, 90, -1);
    g_signal_connect_swapped(b, "clicked", cb, d);
    return b;
}

static GtkWidget *general_page(status_dlg *d, nc_item *it) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    bool wifi = it->device != nullptr && NM_IS_DEVICE_WIFI(it->device);
    GtkWidget *g = grid();
    int y = 0;
    d->status = row(g, y++, "Status:");
    if (wifi)
        d->network = row(g, y++, "Network:");
    d->duration = row(g, y++, "Duration:");
    d->speed = row(g, y++, "Speed:");
    if (wifi)
        d->signal = row(g, y++, "Signal Strength:");
    gtk_box_pack_start(GTK_BOX(page), frame("Connection", g), FALSE, FALSE, 0);

    /* Sent — computer — Received, as XP drew it. */
    GtkWidget *a = grid();
    GtkWidget *sent = gtk_label_new("Sent");
    gtk_label_set_xalign(GTK_LABEL(sent), 1);
    gtk_grid_attach(GTK_GRID(a), sent, 1, 0, 1, 1);
    GtkWidget *pc = gtk_image_new_from_icon_name(wifi ? "network-wireless" : "network-wired",
                                                 GTK_ICON_SIZE_DND);
    gtk_widget_set_hexpand(pc, TRUE);
    gtk_grid_attach(GTK_GRID(a), pc, 2, 0, 1, 1);
    GtkWidget *recv = gtk_label_new("Received");
    gtk_label_set_xalign(GTK_LABEL(recv), 0);
    gtk_grid_attach(GTK_GRID(a), recv, 3, 0, 1, 1);
    static const char *const names[] = {"Bytes:", "Packets:"};
    for (int i = 0; i < 2; i++) {
        GtkWidget *n = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(n), 0);
        gtk_grid_attach(GTK_GRID(a), n, 0, i + 1, 1, 1);
        d->sent[i] = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(d->sent[i]), 1);
        gtk_grid_attach(GTK_GRID(a), d->sent[i], 1, i + 1, 1, 1);
        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
        gtk_widget_set_halign(sep, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(a), sep, 2, i + 1, 1, 1);
        d->received[i] = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(d->received[i]), 0);
        gtk_grid_attach(GTK_GRID(a), d->received[i], 3, i + 1, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(page), frame("Activity", a), FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(buttons), button("P_roperties", G_CALLBACK(on_properties), d),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons),
                       button(it->device != nullptr ? "_Disable" : "_Disconnect",
                              G_CALLBACK(on_disable), d),
                       FALSE, FALSE, 0);
    if (wifi)
        gtk_box_pack_start(GTK_BOX(buttons),
                           button("_View Wireless Networks", G_CALLBACK(on_wireless), d), FALSE,
                           FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    return page;
}

static GtkWidget *support_page(status_dlg *d) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("preferences-system-network", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(outer), img, FALSE, FALSE, 0);
    GtkWidget *g = grid();
    d->addr_type = row(g, 0, "Address Type:");
    d->address = row(g, 1, "IP Address:");
    d->netmask = row(g, 2, "Subnet Mask:");
    d->gateway = row(g, 3, "Default Gateway:");
    gtk_box_pack_start(GTK_BOX(outer), g, TRUE, TRUE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(col), outer, FALSE, FALSE, 0);
    GtkWidget *details = button("_Details...", G_CALLBACK(on_details), d);
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
    GtkWidget *b = button("Re_pair", G_CALLBACK(on_repair), d);
    gtk_widget_set_valign(b, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(repair), b, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), repair, FALSE, FALSE, 0);
    return page;
}

void nc_status_show(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || !it->connected)
        return;
    status_dlg *d = g_new0(status_dlg, 1);
    d->app = app;
    d->key = g_strdup(key);
    d->dialog = gtk_dialog_new();
    nc_track(app, d->dialog);
    char *title = g_strdup_printf("%s Status", it->name);
    gtk_window_set_title(GTK_WINDOW(d->dialog), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(d->dialog), it->icon);
    gtk_window_set_resizable(GTK_WINDOW(d->dialog), FALSE);
    if (nc_parent(app) != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(d->dialog), nc_parent(app));
    gtk_dialog_add_button(GTK_DIALOG(d->dialog), "_Close", GTK_RESPONSE_CLOSE);
    gtk_dialog_set_default_response(GTK_DIALOG(d->dialog), GTK_RESPONSE_CLOSE);
    g_signal_connect(d->dialog, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    g_signal_connect(d->dialog, "destroy", G_CALLBACK(on_destroy), d);

    d->notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(d->notebook), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(d->notebook), general_page(d, it),
                             gtk_label_new_with_mnemonic("_General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(d->notebook), support_page(d),
                             gtk_label_new_with_mnemonic("_Support"));
    gtk_widget_set_size_request(d->notebook, 340, -1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d->dialog))), d->notebook,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(d->dialog, GTK_NOTEBOOK(d->notebook));
    update(d);
    d->timer = g_timeout_add_seconds(1, tick, d);
    gtk_widget_show_all(d->dialog);
}
