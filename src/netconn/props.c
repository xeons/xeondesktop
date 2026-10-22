/* SPDX-License-Identifier: MIT
 * xd-netconn's Properties dialog, as XP's "Local Area Connection
 * Properties":
 *
 *   General            the adapter ("Connect using", Configure... opens
 *                      NetworkManager's editor), or a VPN's server; the
 *                      items the connection uses: TCP/IP and TCP/IPv6,
 *                      ticked when on. TCP/IP's Properties is XP's
 *                      Internet Protocol (TCP/IP) Properties.
 *   Wireless Networks  (Wi-Fi) View Wireless Networks, and the preferred
 *                      networks: the saved Wi-Fi connections
 *
 * Changes are made to a copy of the connection's settings and saved
 * through NetworkManager on OK. An adapter without settings yet gets a
 * new connection.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "app.h"

enum { ITEM_ON, ITEM_ICON, ITEM_NAME, ITEM_ID, ITEM_COLS };
enum { ITEM_IPV4, ITEM_IPV6 };

typedef struct props_dlg {
    nc_app *app;
    char *key;
    char *name;
    GtkWidget *dialog, *items, *item_props, *description, *host;
    GtkListStore *store, *preferred;
    GtkWidget *preferred_view;
    NMConnection *edit; /* the settings being changed */
    NMRemoteConnection *remote; /* where they go; nullptr: a new connection */
    bool changed;
} props_dlg;

static const char *const descriptions[] = {
    [ITEM_IPV4] = "Transmission Control Protocol/Internet Protocol. The default wide area network "
                  "protocol that provides communication across diverse interconnected networks.",
    [ITEM_IPV6] = "Internet Protocol version 6, the next generation of the Internet Protocol, "
                  "with a much larger address space.",
};

static GtkWidget *label(const char *text) {
    GtkWidget *label_widget = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label_widget), 50);
    return label_widget;
}

static GtkWidget *frame(const char *title, GtkWidget *child) {
    GtkWidget *frame_widget = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame_widget), child);
    return frame_widget;
}

/* A group box whose title is a radio button, as XP's TCP/IP dialog. */
static GtkWidget *radio_frame(GtkWidget *radio, GtkWidget *child) {
    GtkWidget *frame_widget = frame(nullptr, child);
    gtk_frame_set_label_widget(GTK_FRAME(frame_widget), radio);
    return frame_widget;
}

static GtkWidget *button(const char *text) {
    GtkWidget *button_widget = gtk_button_new_with_mnemonic(text);
    gtk_widget_set_size_request(button_widget, 90, -1);
    return button_widget;
}

/* ---- Internet Protocol (TCP/IP) Properties ----------------------------------------- */

typedef struct tcpip {
    GtkWidget *dialog, *auto_ip, *manual_ip, *auto_dns, *manual_dns;
    GtkWidget *address, *netmask, *gateway, *dns1, *dns2;
    GtkWidget *ip_grid, *dns_grid;
} tcpip;

static void tcpip_sensitivity(tcpip *ip_dialog) {
    bool manual_ip = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ip_dialog->manual_ip));
    gtk_widget_set_sensitive(ip_dialog->ip_grid, manual_ip);
    /* A fixed address needs fixed DNS servers, as in XP. */
    if (manual_ip)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ip_dialog->manual_dns), TRUE);
    gtk_widget_set_sensitive(ip_dialog->auto_dns, !manual_ip);
    gtk_widget_set_sensitive(ip_dialog->dns_grid, gtk_toggle_button_get_active(
                                                      GTK_TOGGLE_BUTTON(ip_dialog->manual_dns)));
}

static void on_tcpip_toggled([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    tcpip_sensitivity(data);
}

/* XP fills in the class's subnet mask once the address is typed. */
static gboolean on_address_left([[maybe_unused]] GtkWidget *widget,
                                [[maybe_unused]] GdkEvent *focus_event, gpointer data) {
    tcpip *ip_dialog = data;
    unsigned prefix = xd_nc_default_prefix(gtk_entry_get_text(GTK_ENTRY(ip_dialog->address)));
    if (prefix != 0 && gtk_entry_get_text(GTK_ENTRY(ip_dialog->netmask))[0] == '\0') {
        char *mask = xd_nc_prefix_to_netmask(prefix);
        gtk_entry_set_text(GTK_ENTRY(ip_dialog->netmask), mask);
        g_free(mask);
    }
    return FALSE;
}

static GtkWidget *ip_entry(GtkWidget *grid, int y, const char *name, const char *value) {
    GtkWidget *label_widget = gtk_label_new_with_mnemonic(name);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    gtk_widget_set_hexpand(label_widget, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label_widget, 0, y, 1, 1);
    GtkWidget *entry_field = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry_field), 15);
    gtk_entry_set_width_chars(GTK_ENTRY(entry_field), 16);
    gtk_entry_set_activates_default(GTK_ENTRY(entry_field), TRUE);
    gtk_entry_set_text(GTK_ENTRY(entry_field), value != nullptr ? value : "");
    gtk_label_set_mnemonic_widget(GTK_LABEL(label_widget), entry_field);
    gtk_grid_attach(GTK_GRID(grid), entry_field, 1, y, 1, 1);
    return entry_field;
}

static GtkWidget *small_grid(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 12);
    return grid;
}

static void on_tcpip_advanced([[maybe_unused]] GtkButton *button, gpointer data) {
    props_dlg *properties = data;
    nc_run_editor(GTK_WINDOW(properties->dialog),
                  properties->remote != nullptr
                      ? nm_connection_get_uuid(NM_CONNECTION(properties->remote))
                      : nullptr);
}

/* Writes the form into the connection's ipv4 setting. */
static void tcpip_apply(tcpip *ip_dialog, NMSettingIPConfig *s4) {
    bool manual_ip = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ip_dialog->manual_ip));
    bool manual_dns = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ip_dialog->manual_dns));
    nm_setting_ip_config_clear_addresses(s4);
    nm_setting_ip_config_clear_dns(s4);
    if (manual_ip) {
        unsigned prefix = 24;
        xd_nc_netmask_to_prefix(gtk_entry_get_text(GTK_ENTRY(ip_dialog->netmask)), &prefix);
        NMIPAddress *address = nm_ip_address_new(
            AF_INET, gtk_entry_get_text(GTK_ENTRY(ip_dialog->address)), prefix, nullptr);
        if (address != nullptr) {
            nm_setting_ip_config_add_address(s4, address);
            nm_ip_address_unref(address);
        }
        const char *gw = gtk_entry_get_text(GTK_ENTRY(ip_dialog->gateway));
        g_object_set(s4, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_MANUAL,
                     NM_SETTING_IP_CONFIG_GATEWAY, gw[0] != '\0' ? gw : nullptr,
                     NM_SETTING_IP_CONFIG_IGNORE_AUTO_DNS, FALSE, nullptr);
    } else {
        g_object_set(s4, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
                     NM_SETTING_IP_CONFIG_GATEWAY, nullptr, NM_SETTING_IP_CONFIG_IGNORE_AUTO_DNS,
                     manual_dns, nullptr);
    }
    if (manual_ip || manual_dns) {
        const char *dns[] = {gtk_entry_get_text(GTK_ENTRY(ip_dialog->dns1)),
                             gtk_entry_get_text(GTK_ENTRY(ip_dialog->dns2))};
        for (size_t i = 0; i < XD_ARRAY_LEN(dns); i++) {
            if (dns[i][0] != '\0')
                nm_setting_ip_config_add_dns(s4, dns[i]);
        }
    }
}

/* Returns whether OK was pressed (and the settings changed). */
static bool tcpip_run(props_dlg *properties) {
    NMSettingIPConfig *s4 = nm_connection_get_setting_ip4_config(properties->edit);
    if (s4 == nullptr) {
        s4 = NM_SETTING_IP_CONFIG(nm_setting_ip4_config_new());
        g_object_set(s4, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO, nullptr);
        nm_connection_add_setting(properties->edit, NM_SETTING(s4));
    }
    const char *method = nm_setting_ip_config_get_method(s4);
    bool manual = g_strcmp0(method, NM_SETTING_IP4_CONFIG_METHOD_MANUAL) == 0;
    NMIPAddress *addr = nm_setting_ip_config_get_num_addresses(s4) > 0
                            ? nm_setting_ip_config_get_address(s4, 0)
                            : nullptr;
    char *mask = addr != nullptr ? xd_nc_prefix_to_netmask(nm_ip_address_get_prefix(addr))
                                 : g_strdup("");
    guint ndns = nm_setting_ip_config_get_num_dns(s4);

    tcpip ip_dialog = {0};
    ip_dialog.dialog = gtk_dialog_new_with_buttons(
        "Internet Protocol (TCP/IP) Properties", GTK_WINDOW(properties->dialog),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "OK", GTK_RESPONSE_OK,
        "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(ip_dialog.dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(ip_dialog.dialog), FALSE);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_box_pack_start(GTK_BOX(page),
                       label("You can get IP settings assigned automatically if your network "
                             "supports this capability. Otherwise, you need to ask your network "
                             "administrator for the appropriate IP settings."),
                       FALSE, FALSE, 0);

    ip_dialog.auto_ip =
        gtk_radio_button_new_with_mnemonic(nullptr, "_Obtain an IP address automatically");
    ip_dialog.manual_ip = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(ip_dialog.auto_ip), "U_se the following IP address:");
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(manual ? ip_dialog.manual_ip : ip_dialog.auto_ip), TRUE);
    gtk_box_pack_start(GTK_BOX(page), ip_dialog.auto_ip, FALSE, FALSE, 0);
    ip_dialog.ip_grid = small_grid();
    ip_dialog.address =
        ip_entry(ip_dialog.ip_grid, 0,
                 "_IP address:", manual && addr != nullptr ? nm_ip_address_get_address(addr) : "");
    ip_dialog.netmask = ip_entry(ip_dialog.ip_grid, 1, "S_ubnet mask:", manual ? mask : "");
    ip_dialog.gateway =
        ip_entry(ip_dialog.ip_grid, 2,
                 "_Default gateway:", manual ? nm_setting_ip_config_get_gateway(s4) : "");
    g_signal_connect(ip_dialog.address, "focus-out-event", G_CALLBACK(on_address_left), &ip_dialog);
    gtk_box_pack_start(GTK_BOX(page), radio_frame(ip_dialog.manual_ip, ip_dialog.ip_grid), FALSE,
                       FALSE, 0);

    bool fixed_dns = manual || (ndns > 0 && nm_setting_ip_config_get_ignore_auto_dns(s4));
    ip_dialog.auto_dns =
        gtk_radio_button_new_with_mnemonic(nullptr, "O_btain DNS server address automatically");
    ip_dialog.manual_dns = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(ip_dialog.auto_dns), "Us_e the following DNS server addresses:");
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(fixed_dns ? ip_dialog.manual_dns : ip_dialog.auto_dns), TRUE);
    gtk_box_pack_start(GTK_BOX(page), ip_dialog.auto_dns, FALSE, FALSE, 0);
    ip_dialog.dns_grid = small_grid();
    ip_dialog.dns1 = ip_entry(ip_dialog.dns_grid, 0, "_Preferred DNS server:",
                              fixed_dns && ndns > 0 ? nm_setting_ip_config_get_dns(s4, 0) : "");
    ip_dialog.dns2 = ip_entry(ip_dialog.dns_grid, 1, "_Alternate DNS server:",
                              fixed_dns && ndns > 1 ? nm_setting_ip_config_get_dns(s4, 1) : "");
    gtk_box_pack_start(GTK_BOX(page), radio_frame(ip_dialog.manual_dns, ip_dialog.dns_grid), FALSE,
                       FALSE, 0);

    GtkWidget *adv = button("Ad_vanced...");
    gtk_widget_set_halign(adv, GTK_ALIGN_END);
    gtk_widget_set_sensitive(adv, nc_have_editor() && properties->remote != nullptr);
    g_signal_connect(adv, "clicked", G_CALLBACK(on_tcpip_advanced), properties);
    gtk_box_pack_start(GTK_BOX(page), adv, FALSE, FALSE, 0);

    g_signal_connect(ip_dialog.manual_ip, "toggled", G_CALLBACK(on_tcpip_toggled), &ip_dialog);
    g_signal_connect(ip_dialog.manual_dns, "toggled", G_CALLBACK(on_tcpip_toggled), &ip_dialog);
    tcpip_sensitivity(&ip_dialog);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), page, gtk_label_new_with_mnemonic("_General"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(ip_dialog.dialog))), tabs,
                       TRUE, TRUE, 0);
    gtk_widget_show_all(ip_dialog.dialog);
    /* Closing Properties destroys this box too: keep it alive until we're
     * done with it. */
    g_object_ref(ip_dialog.dialog);
    bool ok = false;
    while (gtk_dialog_run(GTK_DIALOG(ip_dialog.dialog)) == GTK_RESPONSE_OK) {
        xd_nc_ipv4_form form = {
            .manual_ip = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ip_dialog.manual_ip)),
            .manual_dns = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ip_dialog.manual_dns)),
            .address = gtk_entry_get_text(GTK_ENTRY(ip_dialog.address)),
            .netmask = gtk_entry_get_text(GTK_ENTRY(ip_dialog.netmask)),
            .gateway = gtk_entry_get_text(GTK_ENTRY(ip_dialog.gateway)),
            .dns1 = gtk_entry_get_text(GTK_ENTRY(ip_dialog.dns1)),
            .dns2 = gtk_entry_get_text(GTK_ENTRY(ip_dialog.dns2)),
        };
        const char *problem = xd_nc_check_ipv4_form(&form);
        if (problem != nullptr) {
            xd_message_box(GTK_WINDOW(ip_dialog.dialog), "Internet Protocol (TCP/IP) Properties",
                           XD_MB_ERROR, XD_MB_OK, FALSE, problem);
            continue;
        }
        tcpip_apply(&ip_dialog, s4);
        ok = true;
        break;
    }
    gtk_widget_destroy(ip_dialog.dialog);
    g_object_unref(ip_dialog.dialog);
    g_free(mask);
    return ok;
}

/* ---- General -------------------------------------------------------------------------- */

static bool ip_enabled(NMConnection *connection, bool v6) {
    NMSettingIPConfig *setting = v6 ? nm_connection_get_setting_ip6_config(connection)
                                    : nm_connection_get_setting_ip4_config(connection);
    const char *method = setting != nullptr ? nm_setting_ip_config_get_method(setting) : nullptr;
    return method == nullptr || (strcmp(method, "disabled") != 0 && strcmp(method, "ignore") != 0);
}

static void set_ip_enabled(NMConnection *connection, bool v6, bool on) {
    NMSettingIPConfig *setting = v6 ? nm_connection_get_setting_ip6_config(connection)
                                    : nm_connection_get_setting_ip4_config(connection);
    if (setting == nullptr) {
        setting = v6 ? NM_SETTING_IP_CONFIG(nm_setting_ip6_config_new())
                     : NM_SETTING_IP_CONFIG(nm_setting_ip4_config_new());
        nm_connection_add_setting(connection, NM_SETTING(setting));
    }
    if (!on)
        nm_setting_ip_config_clear_addresses(setting);
    g_object_set(setting, NM_SETTING_IP_CONFIG_METHOD, on ? "auto" : "disabled",
                 NM_SETTING_IP_CONFIG_GATEWAY, nullptr, nullptr);
}

static int selected_item(props_dlg *properties) {
    GtkTreeModel *model;
    GtkTreeIter iter;
    int id = -1;
    if (gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->items)), &model, &iter))
        gtk_tree_model_get(model, &iter, ITEM_ID, &id, -1);
    return id;
}

static void on_item_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    props_dlg *properties = data;
    int id = selected_item(properties);
    gtk_label_set_text(GTK_LABEL(properties->description), id >= 0 ? descriptions[id] : "");
    bool on = id >= 0 && ip_enabled(properties->edit, id == ITEM_IPV6);
    gtk_widget_set_sensitive(
        properties->item_props,
        on && (id == ITEM_IPV4 || (nc_have_editor() && properties->remote != nullptr)));
}

static void on_item_toggled([[maybe_unused]] GtkCellRendererToggle *renderer, char *path,
                            gpointer data) {
    props_dlg *properties = data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(properties->store), &iter, path))
        return;
    gboolean on = FALSE;
    int id = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(properties->store), &iter, ITEM_ON, &on, ITEM_ID, &id, -1);
    gtk_list_store_set(properties->store, &iter, ITEM_ON, !on, -1);
    set_ip_enabled(properties->edit, id == ITEM_IPV6, !on);
    properties->changed = true;
    on_item_selected(nullptr, properties);
}

static void on_item_properties([[maybe_unused]] GtkButton *button, gpointer data) {
    props_dlg *properties = data;
    int id = selected_item(properties);
    if (id == ITEM_IPV4) {
        if (tcpip_run(properties))
            properties->changed = true;
    } else if (id == ITEM_IPV6 && properties->remote != nullptr) {
        nc_run_editor(GTK_WINDOW(properties->dialog),
                      nm_connection_get_uuid(NM_CONNECTION(properties->remote)));
    }
}

static void on_item_activated([[maybe_unused]] GtkTreeView *tv, [[maybe_unused]] GtkTreePath *path,
                              [[maybe_unused]] GtkTreeViewColumn *col, gpointer data) {
    props_dlg *properties = data;
    if (gtk_widget_get_sensitive(properties->item_props))
        on_item_properties(nullptr, properties);
}

static void on_configure([[maybe_unused]] GtkButton *button, gpointer data) {
    props_dlg *properties = data;
    nc_run_editor(GTK_WINDOW(properties->dialog),
                  nm_connection_get_uuid(NM_CONNECTION(properties->remote)));
}

static void on_host_changed([[maybe_unused]] GtkEditable *editable, gpointer data) {
    props_dlg *properties = data;
    properties->changed = true;
}

static GtkWidget *items_list(props_dlg *properties) {
    properties->store =
        gtk_list_store_new(ITEM_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    gtk_list_store_insert_with_values(properties->store, nullptr, -1, ITEM_ON,
                                      ip_enabled(properties->edit, false), ITEM_ICON,
                                      "preferences-system-network", ITEM_NAME,
                                      "Internet Protocol (TCP/IP)", ITEM_ID, ITEM_IPV4, -1);
    gtk_list_store_insert_with_values(
        properties->store, nullptr, -1, ITEM_ON, ip_enabled(properties->edit, true), ITEM_ICON,
        "preferences-system-network", ITEM_NAME, "Internet Protocol Version 6 (TCP/IPv6)", ITEM_ID,
        ITEM_IPV6, -1);
    properties->items = gtk_tree_view_new_with_model(GTK_TREE_MODEL(properties->store));
    g_object_unref(properties->store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(properties->items), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *renderer = gtk_cell_renderer_toggle_new();
    g_signal_connect(renderer, "toggled", G_CALLBACK(on_item_toggled), properties);
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    gtk_tree_view_column_set_attributes(col, renderer, "active", ITEM_ON, nullptr);
    renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    gtk_tree_view_column_set_attributes(col, renderer, "icon-name", ITEM_ICON, nullptr);
    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_set_attributes(col, renderer, "text", ITEM_NAME, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(properties->items), col);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->items)), "changed",
                     G_CALLBACK(on_item_selected), properties);
    g_signal_connect(properties->items, "row-activated", G_CALLBACK(on_item_activated), properties);
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sw, -1, 90);
    gtk_container_add(GTK_CONTAINER(sw), properties->items);
    return sw;
}

static GtkWidget *general_page(props_dlg *properties, nc_item *it) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    NMSettingVpn *vpn = nm_connection_get_setting_vpn(properties->edit);
    const char *host_key = vpn != nullptr ? xd_nc_vpn_host_key(nm_setting_vpn_get_service_type(vpn))
                                          : nullptr;
    if (vpn != nullptr) {
        GtkWidget *label_widget =
            label("_Host name or IP address of destination (such as example.com or "
                  "157.54.0.1):");
        gtk_box_pack_start(GTK_BOX(page), label_widget, FALSE, FALSE, 0);
        properties->host = gtk_entry_new();
        const char *host = host_key != nullptr ? nm_setting_vpn_get_data_item(vpn, host_key)
                                               : nullptr;
        gtk_entry_set_text(GTK_ENTRY(properties->host), host != nullptr ? host : "");
        gtk_widget_set_sensitive(properties->host, host_key != nullptr);
        gtk_label_set_mnemonic_widget(GTK_LABEL(label_widget), properties->host);
        g_signal_connect(properties->host, "changed", G_CALLBACK(on_host_changed), properties);
        /* The plugin's own settings (certificates, users, ...): the
         * plugin's editor, inside NetworkManager's. */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(row), properties->host, TRUE, TRUE, 0);
        GtkWidget *conf = button("_Configure...");
        gtk_widget_set_sensitive(conf, nc_have_editor());
        g_signal_connect(conf, "clicked", G_CALLBACK(on_configure), properties);
        gtk_box_pack_start(GTK_BOX(row), conf, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(page), label("Connect using:"), FALSE, FALSE, 0);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *dev = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GIcon *icon = nc_icon(it->icon);
        gtk_box_pack_start(GTK_BOX(dev), gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_MENU), FALSE,
                           FALSE, 0);
        g_object_unref(icon);
        GtkWidget *name = gtk_label_new(it->device_name[0] != '\0' ? it->device_name : it->name);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_box_pack_start(GTK_BOX(dev), name, TRUE, TRUE, 0);
        GtkWidget *devframe = gtk_frame_new(nullptr);
        gtk_frame_set_shadow_type(GTK_FRAME(devframe), GTK_SHADOW_IN);
        gtk_container_set_border_width(GTK_CONTAINER(dev), 3);
        gtk_container_add(GTK_CONTAINER(devframe), dev);
        gtk_box_pack_start(GTK_BOX(row), devframe, TRUE, TRUE, 0);
        GtkWidget *conf = button("_Configure...");
        gtk_widget_set_sensitive(conf, nc_have_editor() && properties->remote != nullptr);
        g_signal_connect(conf, "clicked", G_CALLBACK(on_configure), properties);
        gtk_box_pack_start(GTK_BOX(row), conf, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    }
    GtkWidget *label_widget = label("T_his connection uses the following items:");
    gtk_box_pack_start(GTK_BOX(page), label_widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), items_list(properties), FALSE, FALSE, 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label_widget), properties->items);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *install = button("I_nstall...");
    GtkWidget *uninstall = button("_Uninstall");
    gtk_widget_set_sensitive(install, FALSE);
    gtk_widget_set_sensitive(uninstall, FALSE);
    properties->item_props = button("P_roperties");
    g_signal_connect(properties->item_props, "clicked", G_CALLBACK(on_item_properties), properties);
    gtk_box_pack_start(GTK_BOX(buttons), install, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), uninstall, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), properties->item_props, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    properties->description = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(properties->description), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(properties->description), 46);
    gtk_label_set_xalign(GTK_LABEL(properties->description), 0);
    gtk_label_set_yalign(GTK_LABEL(properties->description), 0);
    gtk_label_set_lines(GTK_LABEL(properties->description), 3);
    gtk_widget_set_size_request(properties->description, -1, 50);
    GtkWidget *desc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(desc), properties->description, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Description", desc), FALSE, FALSE, 0);
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(properties->store), &first))
        gtk_tree_selection_select_iter(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->items)), &first);
    on_item_selected(nullptr, properties);
    return page;
}

/* ---- Wireless Networks ---------------------------------------------------------------- */

static void fill_preferred(props_dlg *properties) {
    gtk_list_store_clear(properties->preferred);
    const GPtrArray *all = nm_client_get_connections(properties->app->client);
    for (guint i = 0; all != nullptr && i < all->len; i++) {
        NMConnection *connection = g_ptr_array_index(all, i);
        if (nm_connection_is_type(connection, NM_SETTING_WIRELESS_SETTING_NAME))
            gtk_list_store_insert_with_values(properties->preferred, nullptr, -1, 0,
                                              nm_connection_get_id(connection), 1,
                                              nm_connection_get_uuid(connection), -1);
    }
}

static void on_view_networks([[maybe_unused]] GtkButton *button, gpointer data) {
    props_dlg *properties = data;
    nc_item *it = nc_find(properties->app, properties->key);
    if (it != nullptr && it->device != nullptr)
        nc_wireless_show(properties->app, it->device);
}

static char *selected_preferred(props_dlg *properties) {
    GtkTreeModel *model;
    GtkTreeIter iter;
    char *uuid = nullptr;
    if (gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(properties->preferred_view)), &model, &iter))
        gtk_tree_model_get(model, &iter, 1, &uuid, -1);
    return uuid;
}

static void removed(GObject *src, GAsyncResult *res, gpointer data) {
    GError *error = nullptr;
    if (!nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(src), res, &error)) {
        /* Over Properties, unless it was closed meanwhile. */
        nc_error(gtk_widget_get_mapped(data) ? GTK_WINDOW(data) : nullptr,
                 "The network could not be removed.", error);
        g_error_free(error);
    }
    g_object_unref(data);
}

static void on_remove_preferred([[maybe_unused]] GtkButton *button, gpointer data) {
    props_dlg *properties = data;
    nc_app *app = properties->app;
    char *uuid = selected_preferred(properties);
    NMRemoteConnection *connection =
        uuid != nullptr ? nm_client_get_connection_by_uuid(app->client, uuid) : nullptr;
    if (connection == nullptr) {
        g_free(uuid);
        return;
    }
    char *question_text =
        g_strdup_printf("Remove '%s' from the preferred networks? Its key is forgotten.",
                        nm_connection_get_id(NM_CONNECTION(connection)));
    GtkWidget *dialog = g_object_ref(properties->dialog);
    int response = xd_message_box(GTK_WINDOW(dialog), "Wireless Networks", XD_MB_QUESTION,
                                  XD_MB_YES_NO, TRUE, question_text);
    g_free(question_text);
    /* The box ran the main loop: the network may be gone by now (and
     * Properties with `properties`). */
    connection = response == GTK_RESPONSE_YES ? nm_client_get_connection_by_uuid(app->client, uuid)
                                              : nullptr;
    g_free(uuid);
    if (connection != nullptr)
        nm_remote_connection_delete_async(connection, nullptr, removed, g_object_ref(dialog));
    g_object_unref(dialog);
}

static void on_preferred_properties([[maybe_unused]] GtkButton *button, gpointer data) {
    props_dlg *properties = data;
    char *uuid = selected_preferred(properties);
    if (uuid != nullptr)
        nc_run_editor(GTK_WINDOW(properties->dialog), uuid);
    g_free(uuid);
}

static void on_connections_changed([[maybe_unused]] NMClient *client,
                                   [[maybe_unused]] NMRemoteConnection *rc, gpointer data) {
    fill_preferred(data);
}

static GtkWidget *wireless_page(props_dlg *properties) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *avail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(avail),
                       label("To connect to, disconnect from, or find out more information about "
                             "wireless networks in range, click the button below."),
                       FALSE, FALSE, 0);
    GtkWidget *view = button("_View Wireless Networks");
    gtk_widget_set_halign(view, GTK_ALIGN_END);
    g_signal_connect(view, "clicked", G_CALLBACK(on_view_networks), properties);
    gtk_box_pack_start(GTK_BOX(avail), view, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Available networks", avail), FALSE, FALSE, 0);

    GtkWidget *pref = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(pref),
                       label("Automatically connect to available networks in the order listed "
                             "below:"),
                       FALSE, FALSE, 0);
    properties->preferred = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    properties->preferred_view =
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(properties->preferred));
    g_object_unref(properties->preferred);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(properties->preferred_view), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
    g_object_set(renderer, "icon-name", "network-wireless", nullptr);
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_set_attributes(col, renderer, "text", 0, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(properties->preferred_view), col);
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sw, -1, 110);
    gtk_container_add(GTK_CONTAINER(sw), properties->preferred_view);
    gtk_box_pack_start(GTK_BOX(pref), sw, TRUE, TRUE, 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *rm = button("Re_move");
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove_preferred), properties);
    GtkWidget *pp = button("Pr_operties");
    gtk_widget_set_sensitive(pp, nc_have_editor());
    g_signal_connect(pp, "clicked", G_CALLBACK(on_preferred_properties), properties);
    gtk_box_pack_end(GTK_BOX(buttons), pp, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), rm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pref), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame("Preferred networks", pref), TRUE, TRUE, 0);
    fill_preferred(properties);
    g_signal_connect(properties->app->client, "connection-added",
                     G_CALLBACK(on_connections_changed), properties);
    g_signal_connect(properties->app->client, "connection-removed",
                     G_CALLBACK(on_connections_changed), properties);
    return page;
}

/* ---- the dialog --------------------------------------------------------------------- */

/* Settings for an adapter that has none yet. */
static NMConnection *new_for_device(nc_item *it) {
    NMConnection *connection = nm_simple_connection_new();
    NMSetting *setting = nm_setting_connection_new();
    char *uuid = nm_utils_uuid_generate();
    g_object_set(setting, NM_SETTING_CONNECTION_ID, it->name, NM_SETTING_CONNECTION_UUID, uuid,
                 NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRED_SETTING_NAME,
                 NM_SETTING_CONNECTION_INTERFACE_NAME, nm_device_get_iface(it->device),
                 NM_SETTING_CONNECTION_AUTOCONNECT, TRUE, nullptr);
    g_free(uuid);
    nm_connection_add_setting(connection, setting);
    nm_connection_add_setting(connection, nm_setting_wired_new());
    setting = nm_setting_ip4_config_new();
    g_object_set(setting, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO, nullptr);
    nm_connection_add_setting(connection, setting);
    setting = nm_setting_ip6_config_new();
    g_object_set(setting, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO, nullptr);
    nm_connection_add_setting(connection, setting);
    return connection;
}

static void save(props_dlg *properties) {
    if (!properties->changed)
        return;
    if (properties->host != nullptr) {
        NMSettingVpn *vpn = nm_connection_get_setting_vpn(properties->edit);
        const char *key = xd_nc_vpn_host_key(nm_setting_vpn_get_service_type(vpn));
        if (key != nullptr)
            nm_setting_vpn_add_data_item(vpn, key, gtk_entry_get_text(GTK_ENTRY(properties->host)));
    }
    char *what = g_strdup_printf("The settings of %s could not be saved.", properties->name);
    if (properties->remote != nullptr)
        nc_commit(properties->app, properties->remote, properties->edit, what);
    else
        nc_add(properties->app, properties->edit, what);
    g_free(what);
    properties->changed = false;
}

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    if (response == GTK_RESPONSE_OK)
        save(data);
    gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    props_dlg *properties = data;
    g_signal_handlers_disconnect_by_data(properties->app->client, properties);
    g_clear_object(&properties->edit);
    g_clear_object(&properties->remote);
    g_free(properties->key);
    g_free(properties->name);
    g_free(properties);
}

void nc_properties_show(nc_app *app, const char *key) {
    nc_item *it = nc_find(app, key);
    if (it == nullptr || it->group == XD_NC_WIZARD)
        return;
    bool wifi = it->device != nullptr && NM_IS_DEVICE_WIFI(it->device);
    props_dlg *properties = g_new0(props_dlg, 1);
    properties->app = app;
    properties->key = g_strdup(key);
    properties->name = g_strdup(it->name);
    if (it->conn != nullptr) {
        properties->remote = g_object_ref(it->conn);
        properties->edit = nm_simple_connection_new_clone(NM_CONNECTION(it->conn));
    } else if (it->device != nullptr && !wifi) {
        properties->edit = new_for_device(it);
    }

    properties->dialog = gtk_dialog_new();
    nc_track(app, properties->dialog);
    char *title = g_strdup_printf("%s Properties", it->name);
    gtk_window_set_title(GTK_WINDOW(properties->dialog), title);
    g_free(title);
    gtk_window_set_icon_name(GTK_WINDOW(properties->dialog), it->icon);
    gtk_window_set_resizable(GTK_WINDOW(properties->dialog), FALSE);
    if (nc_parent(app) != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(properties->dialog), nc_parent(app));
    gtk_dialog_add_button(GTK_DIALOG(properties->dialog), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(properties->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(properties->dialog), GTK_RESPONSE_OK);
    g_signal_connect(properties->dialog, "response", G_CALLBACK(on_response), properties);
    g_signal_connect(properties->dialog, "destroy", G_CALLBACK(on_destroy), properties);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    if (properties->edit != nullptr) {
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(properties, it),
                                 gtk_label_new_with_mnemonic("_General"));
    } else {
        /* A Wi-Fi adapter between networks: its TCP/IP settings belong to
         * each network's connection. */
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(page), 10);
        gtk_box_pack_start(GTK_BOX(page),
                           label("This adapter is not connected to a network. Its TCP/IP "
                                 "settings are kept for each wireless network; connect to one to "
                                 "change them."),
                           FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), page, gtk_label_new_with_mnemonic("_General"));
    }
    if (wifi)
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs), wireless_page(properties),
                                 gtk_label_new_with_mnemonic("_Wireless Networks"));
    gtk_widget_set_size_request(tabs, 380, -1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(properties->dialog))), tabs,
                       TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(properties->dialog, GTK_NOTEBOOK(tabs));
    gtk_widget_show_all(properties->dialog);
}
