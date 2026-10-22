/* SPDX-License-Identifier: MIT
 * xd-netconn's New Connection Wizard and XP SP2's "Choose a wireless
 * network".
 *
 *   wizard    Welcome, then the kind of connection: wired, wireless or
 *             VPN (with NetworkManager's installed VPN plugins), its
 *             details, and Completing; Finish adds the connection
 *   wireless  the networks in range, strongest first; Connect uses a
 *             saved connection, or adds one, asking for the key with
 *             xd-wifi-key as the notification area's icon does
 */
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "app.h"

/* Buttons that only act (Next, Connect) answer GTK_RESPONSE_NONE. */
static void on_response(GtkDialog *dialog_widget, int response, [[maybe_unused]] gpointer data) {
    if (response != GTK_RESPONSE_NONE)
        gtk_widget_destroy(GTK_WIDGET(dialog_widget));
}

typedef enum text_style { PLAIN, BOLD, HEADING, TITLE } text_style;

static GtkWidget *label(const char *text, text_style style) {
    GtkWidget *label_widget = gtk_label_new(nullptr);
    char *label_markup = nullptr;
    switch (style) {
    case BOLD:
        label_markup = g_markup_printf_escaped("<b>%s</b>", text);
        break;
    case HEADING:
        label_markup = g_markup_printf_escaped("<span size='large' weight='bold'>%s</span>", text);
        break;
    case TITLE:
        label_markup =
            g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", text);
        break;
    case PLAIN:
        break;
    }
    if (label_markup != nullptr)
        gtk_label_set_markup(GTK_LABEL(label_widget), label_markup);
    else
        gtk_label_set_text_with_mnemonic(GTK_LABEL(label_widget), text);
    g_free(label_markup);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label_widget), 56);
    return label_widget;
}

/* ---- adding, then connecting -------------------------------------------------------- */

typedef struct add_job {
    nc_app *app;
    NMDevice *device;
    bool connect;
} add_job;

static void activated(GObject *src, GAsyncResult *res, gpointer data) {
    nc_app *app = data;
    GError *error = nullptr;
    NMActiveConnection *active_connection =
        nm_client_activate_connection_finish(NM_CLIENT(src), res, &error);
    g_clear_object(&active_connection);
    if (error != nullptr) {
        nc_error(nc_parent(app), "The connection was created, but could not connect.", error);
        g_error_free(error);
    }
    nc_release(app);
}

/* A saved network, chosen in View Available Wireless Networks. */
static void saved_activated(GObject *src, GAsyncResult *res, gpointer data) {
    nc_app *app = data;
    GError *error = nullptr;
    NMActiveConnection *active_connection =
        nm_client_activate_connection_finish(NM_CLIENT(src), res, &error);
    g_clear_object(&active_connection);
    if (error != nullptr) {
        nc_error(nc_parent(app), "Could not connect to the wireless network.", error);
        g_error_free(error);
    }
    nc_release(app);
}

static void added(GObject *src, GAsyncResult *res, gpointer data) {
    add_job *add_request = data;
    GError *error = nullptr;
    NMRemoteConnection *connection = nm_client_add_connection_finish(NM_CLIENT(src), res, &error);
    if (connection == nullptr) {
        nc_error(nc_parent(add_request->app), "The connection could not be created.", error);
        g_error_free(error);
    } else if (add_request->connect) {
        nc_hold(add_request->app);
        nm_client_activate_connection_async(add_request->app->client, NM_CONNECTION(connection),
                                            add_request->device, nullptr, nullptr, activated,
                                            add_request->app);
    }
    nc_release(add_request->app);
    g_clear_object(&connection);
    g_clear_object(&add_request->device);
    g_free(add_request);
}

/* ---- the wizard ---------------------------------------------------------------------- */

typedef enum kind { KIND_WIRED, KIND_WIFI, KIND_VPN } kind;

/* Wi-Fi security choices: label, key management (nullptr: open). */
static const struct {
    const char *label, *key_mgmt;
} securities[] = {
    {"WPA2/WPA3 Personal", "wpa-psk"},
    {"WPA3 Personal only", "sae"},
    {"WEP", "none"},
    {"None (open network)", nullptr},
};

typedef struct wizard {
    nc_app *app;
    GtkWidget *dialog, *stack, *back, *next, *finish;
    GtkWidget *kind[3];
    /* wired */
    GtkWidget *wired_name, *wired_device;
    /* wifi */
    GtkWidget *ssid, *security, *key, *confirm;
    /* vpn */
    GtkWidget *vpn_name, *vpn_type, *vpn_host;
    GSList *plugins; /* NMVpnPluginInfo */
    /* finish */
    GtkWidget *summary, *connect_now;
    const char *page;
} wizard;

static kind chosen(wizard *connection_wizard) {
    for (int i = 0; i < 3; i++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(connection_wizard->kind[i])))
            return (kind)i;
    }
    return KIND_WIRED;
}

static const char *detail_page(wizard *connection_wizard) {
    switch (chosen(connection_wizard)) {
    case KIND_WIFI:
        return "wifi";
    case KIND_VPN:
        return "vpn";
    case KIND_WIRED:
        break;
    }
    return "wired";
}

static const char *security_key_mgmt(wizard *connection_wizard) {
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(connection_wizard->security));
    return i >= 0 && (size_t)i < XD_ARRAY_LEN(securities) ? securities[i].key_mgmt : nullptr;
}

static NMVpnPluginInfo *chosen_plugin(wizard *connection_wizard) {
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(connection_wizard->vpn_type));
    return i >= 0 ? g_slist_nth_data(connection_wizard->plugins, (guint)i) : nullptr;
}

static const char *text(GtkWidget *field) { return gtk_entry_get_text(GTK_ENTRY(field)); }

static bool blank(GtkWidget *field) {
    char *stripped = g_strstrip(g_strdup(text(field)));
    bool is_blank = stripped[0] == '\0';
    g_free(stripped);
    return is_blank;
}

/* Whether the current page is filled in enough for Next. */
static bool page_ok(wizard *connection_wizard) {
    if (strcmp(connection_wizard->page, "wired") == 0)
        return !blank(connection_wizard->wired_name);
    if (strcmp(connection_wizard->page, "wifi") == 0) {
        size_t length = strlen(text(connection_wizard->ssid));
        const char *km = security_key_mgmt(connection_wizard);
        if (length == 0 || length > 32)
            return false;
        if (km == nullptr)
            return true;
        return xd_nc_valid_wifi_key(km, text(connection_wizard->key)) &&
               strcmp(text(connection_wizard->key), text(connection_wizard->confirm)) == 0;
    }
    if (strcmp(connection_wizard->page, "vpn") == 0)
        return !blank(connection_wizard->vpn_name) && chosen_plugin(connection_wizard) != nullptr;
    return true;
}

static void show_page(wizard *connection_wizard, const char *page) {
    connection_wizard->page = page;
    gtk_stack_set_visible_child_name(GTK_STACK(connection_wizard->stack), page);
    bool last = strcmp(page, "finish") == 0;
    gtk_widget_set_sensitive(connection_wizard->back, strcmp(page, "welcome") != 0);
    gtk_widget_set_visible(connection_wizard->next, !last);
    gtk_widget_set_visible(connection_wizard->finish, last);
    gtk_widget_set_sensitive(connection_wizard->next, page_ok(connection_wizard));
    gtk_widget_grab_default(last ? connection_wizard->finish : connection_wizard->next);
}

static void on_input([[maybe_unused]] GtkWidget *widget, gpointer data) {
    wizard *connection_wizard = data;
    if (connection_wizard->page == nullptr)
        return; /* still being built */
    bool keyed = security_key_mgmt(connection_wizard) != nullptr;
    gtk_widget_set_sensitive(connection_wizard->key, keyed);
    gtk_widget_set_sensitive(connection_wizard->confirm, keyed);
    NMVpnPluginInfo *pi = chosen_plugin(connection_wizard);
    gtk_widget_set_sensitive(connection_wizard->vpn_host,
                             pi != nullptr &&
                                 xd_nc_vpn_host_key(nm_vpn_plugin_info_get_service(pi)) != nullptr);
    gtk_widget_set_sensitive(connection_wizard->next, page_ok(connection_wizard));
}

static char *connection_name(wizard *connection_wizard) {
    switch (chosen(connection_wizard)) {
    case KIND_WIFI:
        return g_strdup(text(connection_wizard->ssid));
    case KIND_VPN:
        return g_strstrip(g_strdup(text(connection_wizard->vpn_name)));
    case KIND_WIRED:
        break;
    }
    return g_strstrip(g_strdup(text(connection_wizard->wired_name)));
}

static void on_next([[maybe_unused]] GtkButton *button, gpointer data) {
    wizard *connection_wizard = data;
    if (!page_ok(connection_wizard))
        return;
    if (strcmp(connection_wizard->page, "welcome") == 0) {
        show_page(connection_wizard, "type");
    } else if (strcmp(connection_wizard->page, "type") == 0) {
        show_page(connection_wizard, detail_page(connection_wizard));
    } else {
        char *name = connection_name(connection_wizard);
        char *summary_markup = g_markup_printf_escaped(
            "You have successfully completed the steps needed to create the following "
            "connection:\n\n<b>%s</b>\n\nThe connection will be saved in Network Connections.",
            name);
        gtk_label_set_markup(GTK_LABEL(connection_wizard->summary), summary_markup);
        g_free(summary_markup);
        g_free(name);
        show_page(connection_wizard, "finish");
    }
}

static void on_back([[maybe_unused]] GtkButton *button, gpointer data) {
    wizard *connection_wizard = data;
    if (strcmp(connection_wizard->page, "finish") == 0)
        show_page(connection_wizard, detail_page(connection_wizard));
    else if (strcmp(connection_wizard->page, "type") == 0)
        show_page(connection_wizard, "welcome");
    else
        show_page(connection_wizard, "type");
}

static NMConnection *build(wizard *connection_wizard, NMDevice **device) {
    *device = nullptr;
    kind chosen_kind = chosen(connection_wizard);
    NMConnection *connection = nm_simple_connection_new();
    NMSetting *setting = nm_setting_connection_new();
    char *uuid = nm_utils_uuid_generate();
    char *name = connection_name(connection_wizard);
    g_object_set(setting, NM_SETTING_CONNECTION_ID, name, NM_SETTING_CONNECTION_UUID, uuid,
                 NM_SETTING_CONNECTION_AUTOCONNECT, chosen_kind != KIND_VPN, nullptr);
    g_free(name);
    g_free(uuid);
    nm_connection_add_setting(connection, setting);
    if (chosen_kind == KIND_WIRED) {
        g_object_set(setting, NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRED_SETTING_NAME, nullptr);
        const char *iface =
            gtk_combo_box_get_active_id(GTK_COMBO_BOX(connection_wizard->wired_device));
        if (iface != nullptr && iface[0] != '\0') {
            g_object_set(setting, NM_SETTING_CONNECTION_INTERFACE_NAME, iface, nullptr);
            *device = nm_client_get_device_by_iface(connection_wizard->app->client, iface);
        }
        nm_connection_add_setting(connection, nm_setting_wired_new());
    } else if (chosen_kind == KIND_WIFI) {
        g_object_set(setting, NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
                     nullptr);
        NMSetting *wifi = nm_setting_wireless_new();
        const char *ssid = text(connection_wizard->ssid);
        GBytes *bytes = g_bytes_new(ssid, strlen(ssid));
        g_object_set(wifi, NM_SETTING_WIRELESS_SSID, bytes, NM_SETTING_WIRELESS_MODE,
                     NM_SETTING_WIRELESS_MODE_INFRA, nullptr);
        g_bytes_unref(bytes);
        nm_connection_add_setting(connection, wifi);
        const char *km = security_key_mgmt(connection_wizard);
        if (km != nullptr) {
            NMSetting *sec = nm_setting_wireless_security_new();
            g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, km, nullptr);
            if (strcmp(km, "none") == 0)
                g_object_set(
                    sec, NM_SETTING_WIRELESS_SECURITY_WEP_KEY0, text(connection_wizard->key),
                    NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE, NM_WEP_KEY_TYPE_KEY, nullptr);
            else
                g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_PSK, text(connection_wizard->key),
                             nullptr);
            nm_connection_add_setting(connection, sec);
        }
    } else {
        g_object_set(setting, NM_SETTING_CONNECTION_TYPE, NM_SETTING_VPN_SETTING_NAME, nullptr);
        NMVpnPluginInfo *pi = chosen_plugin(connection_wizard);
        const char *service = nm_vpn_plugin_info_get_service(pi);
        NMSettingVpn *vpn = NM_SETTING_VPN(nm_setting_vpn_new());
        g_object_set(vpn, NM_SETTING_VPN_SERVICE_TYPE, service, nullptr);
        const char *host_key = xd_nc_vpn_host_key(service);
        char *host = g_strstrip(g_strdup(text(connection_wizard->vpn_host)));
        if (host_key != nullptr && host[0] != '\0')
            nm_setting_vpn_add_data_item(vpn, host_key, host);
        g_free(host);
        nm_connection_add_setting(connection, NM_SETTING(vpn));
    }
    setting = nm_setting_ip4_config_new();
    g_object_set(setting, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO, nullptr);
    nm_connection_add_setting(connection, setting);
    setting = nm_setting_ip6_config_new();
    g_object_set(setting, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO, nullptr);
    nm_connection_add_setting(connection, setting);
    return connection;
}

static void on_finish([[maybe_unused]] GtkButton *button, gpointer data) {
    wizard *connection_wizard = data;
    NMDevice *device = nullptr;
    NMConnection *connection = build(connection_wizard, &device);
    add_job *add_request = g_new0(add_job, 1);
    add_request->app = connection_wizard->app;
    add_request->device = device != nullptr ? g_object_ref(device) : nullptr;
    add_request->connect =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(connection_wizard->connect_now));
    nc_hold(connection_wizard->app);
    nm_client_add_connection_async(connection_wizard->app->client, connection, TRUE, nullptr, added,
                                   add_request);
    g_object_unref(connection);
    gtk_widget_destroy(connection_wizard->dialog);
}

static void on_wizard_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    wizard *connection_wizard = data;
    g_slist_free_full(connection_wizard->plugins, g_object_unref);
    g_free(connection_wizard);
}

static GtkWidget *page_box(const char *title, const char *subtitle) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_pack_start(GTK_BOX(box), label(title, BOLD), FALSE, FALSE, 0);
    if (subtitle != nullptr) {
        GtkWidget *subtitle_label = label(subtitle, PLAIN);
        gtk_widget_set_margin_start(subtitle_label, 14);
        gtk_box_pack_start(GTK_BOX(box), subtitle_label, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    return box;
}

static GtkWidget *form_entry(GtkWidget *grid, int y, const char *name, GtkWidget *field) {
    GtkWidget *label_widget = label(name, PLAIN);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label_widget), field);
    gtk_grid_attach(GTK_GRID(grid), label_widget, 0, y, 1, 1);
    gtk_widget_set_hexpand(field, TRUE);
    gtk_grid_attach(GTK_GRID(grid), field, 1, y, 1, 1);
    return field;
}

static GtkWidget *entry(wizard *connection_wizard, bool secret) {
    GtkWidget *entry_widget = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry_widget), TRUE);
    if (secret) {
        gtk_entry_set_visibility(GTK_ENTRY(entry_widget), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(entry_widget), GTK_INPUT_PURPOSE_PASSWORD);
    }
    g_signal_connect(entry_widget, "changed", G_CALLBACK(on_input), connection_wizard);
    return entry_widget;
}

static GtkWidget *form(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 14);
    return grid;
}

static GtkWidget *welcome_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    GtkWidget *img = gtk_image_new_from_icon_name("tools-wizard", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 64);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *title = label("Welcome to the New Connection Wizard",
                             TITLE);
    gtk_box_pack_start(GTK_BOX(col), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), label("This wizard helps you:", PLAIN), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), label("• Connect to a wired network.", PLAIN), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), label("• Connect to a wireless network.", PLAIN),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col),
                       label("• Connect to a private network, such as your workplace "
                             "network, using a virtual private network (VPN).",
                             PLAIN),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), label("To continue, click Next.", PLAIN), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), col, TRUE, TRUE, 0);
    return box;
}

static GtkWidget *type_page(wizard *connection_wizard) {
    GtkWidget *box = page_box("Network Connection Type", "What do you want to do?");
    static const char *const kinds[][2] = {
        {"Connect to a _wired network",
         "Connect to a network or the Internet with a network cable (Ethernet)."},
        {"Connect to a w_ireless network",
         "Connect to a wireless network by its name (SSID), even one that isn't in range."},
        {"Connect to the network at my _workplace",
         "Connect to a business network using a virtual private network (VPN) connection."},
    };
    for (int i = 0; i < 3; i++) {
        connection_wizard->kind[i] =
            i == 0 ? gtk_radio_button_new_with_mnemonic(nullptr, kinds[i][0])
                   : gtk_radio_button_new_with_mnemonic_from_widget(
                         GTK_RADIO_BUTTON(connection_wizard->kind[0]), kinds[i][0]);
        gtk_box_pack_start(GTK_BOX(box), connection_wizard->kind[i], FALSE, FALSE, 0);
        GtkWidget *description = label(kinds[i][1], PLAIN);
        gtk_widget_set_margin_start(description, 26);
        gtk_box_pack_start(GTK_BOX(box), description, FALSE, FALSE, 0);
    }
    return box;
}

static GtkWidget *wired_page(wizard *connection_wizard) {
    GtkWidget *box = page_box("Wired Connection", "Name the connection and pick the adapter it uses.");
    GtkWidget *grid = form();
    connection_wizard->wired_name =
        form_entry(grid, 0, "Connection _name:", entry(connection_wizard, false));
    /* "Local Area Connection N", the next free number. */
    unsigned number = 1;
    for (char *name = xd_nc_numbered_name("Local Area Connection", number);;
         name = xd_nc_numbered_name("Local Area Connection", ++number)) {
        bool taken = false;
        const GPtrArray *all = nm_client_get_connections(connection_wizard->app->client);
        for (guint i = 0; all != nullptr && i < all->len && !taken; i++)
            taken = strcmp(nm_connection_get_id(g_ptr_array_index(all, i)), name) == 0;
        if (!taken) {
            gtk_entry_set_text(GTK_ENTRY(connection_wizard->wired_name), name);
            g_free(name);
            break;
        }
        g_free(name);
    }
    connection_wizard->wired_device = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(connection_wizard->wired_device), "",
                              "Any wired adapter");
    const GPtrArray *devs = nm_client_get_devices(connection_wizard->app->client);
    for (guint i = 0; devs != nullptr && i < devs->len; i++) {
        NMDevice *device = g_ptr_array_index(devs, i);
        if (!NM_IS_DEVICE_ETHERNET(device))
            continue;
        const char *desc = nm_device_get_description(device);
        const char *iface = nm_device_get_iface(device);
        char *text = desc != nullptr && strcmp(desc, iface) != 0
                         ? g_strdup_printf("%s (%s)", desc, iface)
                         : g_strdup(iface);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(connection_wizard->wired_device),
                                  nm_device_get_iface(device), text);
        g_free(text);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(connection_wizard->wired_device), 0);
    form_entry(grid, 1, "Connect _using:", connection_wizard->wired_device);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *wifi_page(wizard *connection_wizard) {
    GtkWidget *box = page_box("Wireless Network",
                              "Type the network's name and how it is secured. To pick a network "
                              "in range instead, use View Available Wireless Networks.");
    GtkWidget *grid = form();
    connection_wizard->ssid =
        form_entry(grid, 0, "Network _name (SSID):", entry(connection_wizard, false));
    gtk_entry_set_max_length(GTK_ENTRY(connection_wizard->ssid), 32);
    connection_wizard->security = gtk_combo_box_text_new();
    for (size_t i = 0; i < XD_ARRAY_LEN(securities); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(connection_wizard->security),
                                       securities[i].label);
    gtk_combo_box_set_active(GTK_COMBO_BOX(connection_wizard->security), 0);
    g_signal_connect(connection_wizard->security, "changed", G_CALLBACK(on_input),
                     connection_wizard);
    form_entry(grid, 1, "_Security:", connection_wizard->security);
    connection_wizard->key = form_entry(grid, 2, "Network _key:", entry(connection_wizard, true));
    connection_wizard->confirm =
        form_entry(grid, 3, "C_onfirm network key:", entry(connection_wizard, true));
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *vpn_page(wizard *connection_wizard) {
    GtkWidget *box = page_box("VPN Connection",
                              "Name the connection, pick the kind of VPN and type its server.");
    GtkWidget *grid = form();
    connection_wizard->vpn_name =
        form_entry(grid, 0, "Company N_ame:", entry(connection_wizard, false));
    connection_wizard->vpn_type = gtk_combo_box_text_new();
    connection_wizard->plugins = nm_vpn_plugin_info_list_load();
    for (GSList *link = connection_wizard->plugins; link != nullptr; link = link->next)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(connection_wizard->vpn_type),
                                       nm_vpn_plugin_info_get_name(link->data));
    gtk_combo_box_set_active(GTK_COMBO_BOX(connection_wizard->vpn_type),
                             connection_wizard->plugins != nullptr ? 0 : -1);
    g_signal_connect(connection_wizard->vpn_type, "changed", G_CALLBACK(on_input),
                     connection_wizard);
    form_entry(grid, 1, "VPN _type:", connection_wizard->vpn_type);
    connection_wizard->vpn_host =
        form_entry(grid, 2, "_Host name or IP address:", entry(connection_wizard, false));
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    if (connection_wizard->plugins == nullptr)
        gtk_box_pack_start(GTK_BOX(box),
                           label("No VPN plugins for NetworkManager are installed. Install one "
                                 "(such as NetworkManager-openvpn) to create a VPN connection.",
                                 PLAIN),
                           FALSE, FALSE, 0);
    return box;
}

static GtkWidget *finish_page(wizard *connection_wizard) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_pack_start(GTK_BOX(box),
                       label("Completing the New Connection Wizard",
                             TITLE),
                       FALSE, FALSE, 0);
    connection_wizard->summary = label("", PLAIN);
    gtk_box_pack_start(GTK_BOX(box), connection_wizard->summary, FALSE, FALSE, 0);
    connection_wizard->connect_now = gtk_check_button_new_with_mnemonic("_Connect now");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(connection_wizard->connect_now), TRUE);
    gtk_box_pack_start(GTK_BOX(box), connection_wizard->connect_now, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), label("To create the connection and close this wizard, click "
                                         "Finish.",
                                         PLAIN),
                     FALSE, FALSE, 0);
    return box;
}

void nc_wizard_show(nc_app *app) {
    if (app->client == nullptr || !nm_client_get_nm_running(app->client))
        return;
    wizard *connection_wizard = g_new0(wizard, 1);
    connection_wizard->app = app;
    connection_wizard->dialog = gtk_dialog_new();
    nc_track(app, connection_wizard->dialog);
    gtk_window_set_title(GTK_WINDOW(connection_wizard->dialog), "New Connection Wizard");
    gtk_window_set_icon_name(GTK_WINDOW(connection_wizard->dialog), "tools-wizard");
    gtk_window_set_resizable(GTK_WINDOW(connection_wizard->dialog), FALSE);
    if (nc_parent(app) != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(connection_wizard->dialog), nc_parent(app));
    connection_wizard->back =
        gtk_dialog_add_button(GTK_DIALOG(connection_wizard->dialog), "< _Back", GTK_RESPONSE_NONE);
    connection_wizard->next =
        gtk_dialog_add_button(GTK_DIALOG(connection_wizard->dialog), "_Next >", GTK_RESPONSE_NONE);
    connection_wizard->finish =
        gtk_dialog_add_button(GTK_DIALOG(connection_wizard->dialog), "Finish", GTK_RESPONSE_NONE);
    gtk_dialog_add_button(GTK_DIALOG(connection_wizard->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_widget_set_can_default(connection_wizard->next, TRUE);
    gtk_widget_set_can_default(connection_wizard->finish, TRUE);
    g_signal_connect(connection_wizard->back, "clicked", G_CALLBACK(on_back), connection_wizard);
    g_signal_connect(connection_wizard->next, "clicked", G_CALLBACK(on_next), connection_wizard);
    g_signal_connect(connection_wizard->finish, "clicked", G_CALLBACK(on_finish),
                     connection_wizard);
    g_signal_connect(connection_wizard->dialog, "response", G_CALLBACK(on_response), nullptr);
    g_signal_connect(connection_wizard->dialog, "destroy", G_CALLBACK(on_wizard_destroy),
                     connection_wizard);

    connection_wizard->stack = gtk_stack_new();
    gtk_widget_set_size_request(connection_wizard->stack, 500, 320);
    gtk_stack_add_named(GTK_STACK(connection_wizard->stack), welcome_page(), "welcome");
    gtk_stack_add_named(GTK_STACK(connection_wizard->stack), type_page(connection_wizard), "type");
    gtk_stack_add_named(GTK_STACK(connection_wizard->stack), wired_page(connection_wizard),
                        "wired");
    gtk_stack_add_named(GTK_STACK(connection_wizard->stack), wifi_page(connection_wizard), "wifi");
    gtk_stack_add_named(GTK_STACK(connection_wizard->stack), vpn_page(connection_wizard), "vpn");
    gtk_stack_add_named(GTK_STACK(connection_wizard->stack), finish_page(connection_wizard),
                        "finish");
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(connection_wizard->dialog));
    gtk_box_pack_start(GTK_BOX(content), connection_wizard->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    gtk_widget_show_all(connection_wizard->dialog);
    show_page(connection_wizard, "welcome");
    on_input(nullptr, connection_wizard);
}

/* ---- View Available Wireless Networks ---------------------------------------------------- */

enum { NET_ICON, NET_TEXT, NET_SIGNAL, NET_AP, NET_STRENGTH, NET_CURRENT, NET_COLS };

typedef struct chooser {
    nc_app *app;
    NMDevice *device;
    GtkWidget *dialog, *view, *connect;
    GtkListStore *store;
} chooser;

static const char *signal_icon(unsigned strength) {
    return strength >= 80   ? "network-wireless-signal-excellent"
           : strength >= 55 ? "network-wireless-signal-good"
           : strength >= 30 ? "network-wireless-signal-ok"
           : strength >= 5  ? "network-wireless-signal-weak"
                            : "network-wireless-signal-none";
}

static char *ap_ssid(NMAccessPoint *ap) {
    GBytes *bytes = nm_access_point_get_ssid(ap);
    return bytes != nullptr && g_bytes_get_size(bytes) > 0
               ? nm_utils_ssid_to_utf8(g_bytes_get_data(bytes, nullptr), g_bytes_get_size(bytes))
               : nullptr;
}

static const char *security_text(NMAccessPoint *ap) {
    unsigned flags = nm_access_point_get_flags(ap), wpa = nm_access_point_get_wpa_flags(ap),
             rsn = nm_access_point_get_rsn_flags(ap);
    const char *km = xd_nc_wifi_key_mgmt(flags, wpa, rsn);
    if (km == nullptr)
        return "Unsecured wireless network";
    if (strcmp(km, "none") == 0)
        return "Security-enabled wireless network (WEP)";
    if (strcmp(km, "wpa-eap") == 0)
        return "Security-enabled wireless network (802.1X)";
    if (strcmp(km, "sae") == 0)
        return "Security-enabled wireless network (WPA3)";
    return rsn != 0 ? "Security-enabled wireless network (WPA2)"
                    : "Security-enabled wireless network (WPA)";
}

/* The network the adapter is connected to: its access point's, or its
 * connection's while the access point isn't known yet. */
static char *connected_ssid(NMDevice *device) {
    if (nm_device_get_state(device) != NM_DEVICE_STATE_ACTIVATED)
        return nullptr;
    NMAccessPoint *ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(device));
    char *ssid = ap != nullptr ? ap_ssid(ap) : nullptr;
    NMActiveConnection *active = nm_device_get_active_connection(device);
    NMRemoteConnection *connection =
        active != nullptr ? nm_active_connection_get_connection(active) : nullptr;
    NMSettingWireless *setting = connection != nullptr
                                     ? nm_connection_get_setting_wireless(NM_CONNECTION(connection))
                                     : nullptr;
    GBytes *bytes = setting != nullptr ? nm_setting_wireless_get_ssid(setting) : nullptr;
    if (ssid == nullptr && bytes != nullptr)
        ssid = nm_utils_ssid_to_utf8(g_bytes_get_data(bytes, nullptr), g_bytes_get_size(bytes));
    return ssid;
}

/* A saved connection for the network, if any. */
static NMRemoteConnection *saved_for(chooser *network_chooser, NMAccessPoint *ap) {
    const GPtrArray *all = nm_client_get_connections(network_chooser->app->client);
    GPtrArray *mine = g_ptr_array_new();
    for (guint i = 0; all != nullptr && i < all->len; i++) {
        NMConnection *conn = g_ptr_array_index(all, i);
        if (nm_connection_is_type(conn, NM_SETTING_WIRELESS_SETTING_NAME))
            g_ptr_array_add(mine, conn);
    }
    GPtrArray *match = nm_access_point_filter_connections(ap, mine);
    NMRemoteConnection *saved =
        match != nullptr && match->len > 0 ? g_ptr_array_index(match, 0) : nullptr;
    if (match != nullptr)
        g_ptr_array_unref(match);
    g_ptr_array_unref(mine);
    return saved;
}

static void fill_networks(chooser *network_chooser) {
    char *keep = nullptr;
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(network_chooser->view));
    if (gtk_tree_selection_get_selected(sel, &model, &iter))
        gtk_tree_model_get(model, &iter, NET_AP, &keep, -1);
    gtk_list_store_clear(network_chooser->store);
    NMDeviceWifi *wifi = NM_DEVICE_WIFI(network_chooser->device);
    char *active_ssid = connected_ssid(network_chooser->device);
    const GPtrArray *aps = nm_device_wifi_get_access_points(wifi);
    /* One row per network: its strongest access point. */
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    GPtrArray *best = g_ptr_array_new();
    for (guint i = 0; aps != nullptr && i < aps->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(aps, i);
        char *ssid = ap_ssid(ap);
        if (ssid == nullptr)
            continue; /* hidden */
        gpointer idx;
        if (g_hash_table_lookup_extended(seen, ssid, nullptr, &idx)) {
            guint at = GPOINTER_TO_UINT(idx);
            NMAccessPoint *other = g_ptr_array_index(best, at);
            if (nm_access_point_get_strength(ap) > nm_access_point_get_strength(other))
                g_ptr_array_index(best, at) = ap;
            g_free(ssid);
        } else {
            g_hash_table_insert(seen, ssid, GUINT_TO_POINTER(best->len));
            g_ptr_array_add(best, ap);
        }
    }
    for (guint i = 0; i < best->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(best, i);
        char *ssid = ap_ssid(ap);
        if (ssid == nullptr)
            continue; /* not kept above; the SSID can change in between */
        bool current = active_ssid != nullptr && strcmp(ssid, active_ssid) == 0;
        bool saved = saved_for(network_chooser, ap) != nullptr;
        unsigned strength = nm_access_point_get_strength(ap);
        char *markup = g_markup_printf_escaped(
            "<b>%s</b>%s\n<small>%s</small>", ssid,
            current ? "  —  Connected" : saved ? "  —  Automatic" : "",
            security_text(ap));
        char *signal = g_strdup_printf("%s (%u%%)", xd_nc_signal_words(strength), strength);
        gtk_list_store_insert_with_values(network_chooser->store, &iter, -1, NET_ICON,
                                          signal_icon(strength), NET_TEXT, markup, NET_SIGNAL,
                                          signal, NET_AP, nm_object_get_path(NM_OBJECT(ap)),
                                          NET_STRENGTH, strength, NET_CURRENT, current, -1);
        if (keep != nullptr && strcmp(keep, nm_object_get_path(NM_OBJECT(ap))) == 0)
            gtk_tree_selection_select_iter(sel, &iter);
        g_free(signal);
        g_free(markup);
        g_free(ssid);
    }
    g_ptr_array_free(best, TRUE);
    g_hash_table_destroy(seen);
    g_free(active_ssid);
    g_free(keep);
}

static NMAccessPoint *selected_ap(chooser *network_chooser, bool *current) {
    GtkTreeModel *model;
    GtkTreeIter iter;
    char *path = nullptr;
    gboolean cur = FALSE;
    if (!gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(network_chooser->view)), &model, &iter))
        return nullptr;
    gtk_tree_model_get(model, &iter, NET_AP, &path, NET_CURRENT, &cur, -1);
    NMAccessPoint *found = nullptr;
    const GPtrArray *aps =
        nm_device_wifi_get_access_points(NM_DEVICE_WIFI(network_chooser->device));
    for (guint i = 0; aps != nullptr && i < aps->len && found == nullptr; i++) {
        if (strcmp(nm_object_get_path(NM_OBJECT(g_ptr_array_index(aps, i))), path) == 0)
            found = g_ptr_array_index(aps, i);
    }
    g_free(path);
    if (current != nullptr)
        *current = cur;
    return found;
}

static void on_network_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    chooser *network_chooser = data;
    bool current = false;
    NMAccessPoint *ap = selected_ap(network_chooser, &current);
    gtk_button_set_label(GTK_BUTTON(network_chooser->connect),
                         current ? "_Disconnect" : "C_onnect");
    gtk_widget_set_sensitive(network_chooser->connect, ap != nullptr);
}

static void on_aps_changed([[maybe_unused]] GObject *object, [[maybe_unused]] GParamSpec *ps,
                           gpointer data) {
    fill_networks(data);
    on_network_selected(nullptr, data);
}

typedef struct key_job {
    nc_app *app;
    NMDevice *device;
    char *ap_path, *key_mgmt;
} key_job;

static void key_job_free(key_job *key_request) {
    g_clear_object(&key_request->device);
    g_free(key_request->ap_path);
    g_free(key_request->key_mgmt);
    g_free(key_request);
}

static void joined(GObject *src, GAsyncResult *res, gpointer data) {
    nc_app *app = data;
    GError *error = nullptr;
    NMActiveConnection *nmactive_connection =
        nm_client_add_and_activate_connection_finish(NM_CLIENT(src), res, &error);
    g_clear_object(&nmactive_connection);
    if (error != nullptr) {
        nc_error(nc_parent(app), "Could not connect to the wireless network.", error);
        g_error_free(error);
    }
    nc_release(app);
}

/* The settings NetworkManager can't guess from the access point: the
 * key. */
static NMConnection *partial(const char *key_mgmt, const char *key) {
    if (key_mgmt == nullptr)
        return nullptr;
    NMConnection *connection = nm_simple_connection_new();
    NMSetting *sec = nm_setting_wireless_security_new();
    g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, key_mgmt, nullptr);
    if (strcmp(key_mgmt, "none") == 0)
        g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_WEP_KEY0, key,
                     NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE, NM_WEP_KEY_TYPE_KEY, nullptr);
    else
        g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_PSK, key, nullptr);
    nm_connection_add_setting(connection, sec);
    return connection;
}

static void join(nc_app *app, NMDevice *device, const char *ap_path, const char *key_mgmt,
                 const char *key) {
    NMConnection *connection = partial(key_mgmt, key);
    nc_hold(app);
    nm_client_add_and_activate_connection_async(app->client, connection, device, ap_path, nullptr,
                                                joined, app);
    g_clear_object(&connection);
}

static void got_key(GObject *src, GAsyncResult *res, gpointer data) {
    key_job *key_request = data;
    char *out = nullptr;
    GError *error = nullptr;
    /* No output means Cancel (its exit status may be a sanitizer's). */
    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, nullptr, &error) &&
        out != nullptr) {
        g_strchomp(out);
        if (out[0] != '\0')
            join(key_request->app, key_request->device, key_request->ap_path, key_request->key_mgmt,
                 out);
    }
    if (out != nullptr) {
        explicit_bzero(out, strlen(out));
        g_free(out);
    }
    g_clear_error(&error);
    nc_release(key_request->app);
    key_job_free(key_request);
}

/* Asks for the key with xd-wifi-key, which prints it (as the shell's
 * network icon does). */
static void ask_key(chooser *network_chooser, NMAccessPoint *ap, const char *ssid,
                    const char *key_mgmt) {
    char *prog = nc_own_program("xd-wifi-key");
    GError *error = nullptr;
    GSubprocess *process =
        g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, prog, ssid, nullptr);
    g_free(prog);
    if (process == nullptr) {
        nc_error(GTK_WINDOW(network_chooser->dialog),
                 "The network key prompt could not be started.", error);
        g_error_free(error);
        return;
    }
    key_job *key_request = g_new0(key_job, 1);
    key_request->app = network_chooser->app;
    nc_hold(network_chooser->app);
    key_request->device = g_object_ref(network_chooser->device);
    key_request->ap_path = g_strdup(nm_object_get_path(NM_OBJECT(ap)));
    key_request->key_mgmt = g_strdup(key_mgmt);
    g_subprocess_communicate_utf8_async(process, nullptr, nullptr, got_key, key_request);
    g_object_unref(process);
}

static void on_connect([[maybe_unused]] GtkButton *button, gpointer data) {
    chooser *network_chooser = data;
    bool current = false;
    NMAccessPoint *ap = selected_ap(network_chooser, &current);
    if (ap == nullptr)
        return;
    if (current) {
        char *key =
            g_strdup_printf("dev:%s", nm_object_get_path(NM_OBJECT(network_chooser->device)));
        nc_disable(network_chooser->app, key);
        g_free(key);
        return;
    }
    const char *ap_path = nm_object_get_path(NM_OBJECT(ap));
    NMRemoteConnection *saved = saved_for(network_chooser, ap);
    if (saved != nullptr) {
        nc_hold(network_chooser->app);
        nm_client_activate_connection_async(network_chooser->app->client, NM_CONNECTION(saved),
                                            network_chooser->device, ap_path, nullptr,
                                            saved_activated, network_chooser->app);
        return;
    }
    const char *km = xd_nc_wifi_key_mgmt(nm_access_point_get_flags(ap),
                                         nm_access_point_get_wpa_flags(ap),
                                         nm_access_point_get_rsn_flags(ap));
    if (km == nullptr) {
        join(network_chooser->app, network_chooser->device, ap_path, nullptr, nullptr);
    } else if (strcmp(km, "wpa-eap") == 0) {
        /* 802.1X needs certificates and more: NetworkManager's editor. */
        if (nc_have_editor())
            nc_run_editor(GTK_WINDOW(network_chooser->dialog), nullptr);
        else
            xd_message_box(GTK_WINDOW(network_chooser->dialog), "Wireless Network Connection",
                           XD_MB_INFO, XD_MB_OK, FALSE,
                           "This network uses 802.1X authentication. Set it up with "
                           "NetworkManager's connection editor (nm-connection-editor).");
    } else {
        char *ssid = ap_ssid(ap);
        ask_key(network_chooser, ap, ssid != nullptr ? ssid : "", km);
        g_free(ssid);
    }
}

static void on_refresh([[maybe_unused]] GtkButton *button, gpointer data) {
    chooser *network_chooser = data;
    nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(network_chooser->device), nullptr, nullptr,
                                      nullptr);
    fill_networks(network_chooser);
}

static void on_network_activated([[maybe_unused]] GtkTreeView *tv,
                                 [[maybe_unused]] GtkTreePath *path,
                                 [[maybe_unused]] GtkTreeViewColumn *col, gpointer data) {
    on_connect(nullptr, data);
}

static void on_chooser_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    chooser *network_chooser = data;
    g_signal_handlers_disconnect_by_data(network_chooser->device, network_chooser);
    g_object_unref(network_chooser->device);
    g_free(network_chooser);
}

void nc_wireless_show(nc_app *app, NMDevice *device) {
    if (device == nullptr || !NM_IS_DEVICE_WIFI(device))
        return;
    chooser *network_chooser = g_new0(chooser, 1);
    network_chooser->app = app;
    network_chooser->device = g_object_ref(device);
    network_chooser->dialog = gtk_dialog_new();
    nc_track(app, network_chooser->dialog);
    gtk_window_set_title(GTK_WINDOW(network_chooser->dialog), "Wireless Network Connection");
    gtk_window_set_icon_name(GTK_WINDOW(network_chooser->dialog), "network-wireless");
    gtk_window_set_default_size(GTK_WINDOW(network_chooser->dialog), 520, 420);
    if (nc_parent(app) != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(network_chooser->dialog), nc_parent(app));
    GtkWidget *refresh = gtk_dialog_add_button(GTK_DIALOG(network_chooser->dialog),
                                               "_Refresh network list", GTK_RESPONSE_NONE);
    network_chooser->connect =
        gtk_dialog_add_button(GTK_DIALOG(network_chooser->dialog), "C_onnect", GTK_RESPONSE_NONE);
    gtk_dialog_add_button(GTK_DIALOG(network_chooser->dialog), "_Close", GTK_RESPONSE_CLOSE);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), network_chooser);
    g_signal_connect(network_chooser->connect, "clicked", G_CALLBACK(on_connect), network_chooser);
    g_signal_connect(network_chooser->dialog, "response", G_CALLBACK(on_response), nullptr);
    g_signal_connect(network_chooser->dialog, "destroy", G_CALLBACK(on_chooser_destroy),
                     network_chooser);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(box),
                       label("Choose a wireless network", HEADING),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       label("Click an item in the list below to connect to a wireless network "
                             "in range or to get more information.",
                             PLAIN),
                       FALSE, FALSE, 0);
    network_chooser->store =
        gtk_list_store_new(NET_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                           G_TYPE_UINT, G_TYPE_BOOLEAN);
    GtkTreeSortable *sort = GTK_TREE_SORTABLE(network_chooser->store);
    gtk_tree_sortable_set_sort_column_id(sort, NET_STRENGTH, GTK_SORT_DESCENDING);
    network_chooser->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(network_chooser->store));
    g_object_unref(network_chooser->store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(network_chooser->view), FALSE);
    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
    g_object_set(renderer, "stock-size", GTK_ICON_SIZE_DND, nullptr);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(network_chooser->view),
        gtk_tree_view_column_new_with_attributes("", renderer, "icon-name", NET_ICON, nullptr));
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Network", gtk_cell_renderer_text_new(), "markup", NET_TEXT, nullptr);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(network_chooser->view), col);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(network_chooser->view),
        gtk_tree_view_column_new_with_attributes("Signal", gtk_cell_renderer_text_new(), "text",
                                                 NET_SIGNAL, nullptr));
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(network_chooser->view)), "changed",
                     G_CALLBACK(on_network_selected), network_chooser);
    g_signal_connect(network_chooser->view, "row-activated", G_CALLBACK(on_network_activated),
                     network_chooser);
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), network_chooser->view);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(network_chooser->dialog))),
                       box, TRUE, TRUE, 0);
    g_signal_connect(network_chooser->device, "notify::access-points", G_CALLBACK(on_aps_changed),
                     network_chooser);
    g_signal_connect(network_chooser->device, "notify::active-access-point",
                     G_CALLBACK(on_aps_changed), network_chooser);
    g_signal_connect(network_chooser->device, "notify::state", G_CALLBACK(on_aps_changed),
                     network_chooser);
    fill_networks(network_chooser);
    on_network_selected(nullptr, network_chooser);
    nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(device), nullptr, nullptr, nullptr);
    gtk_widget_show_all(network_chooser->dialog);
}
