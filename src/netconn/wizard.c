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
static void on_response(GtkDialog *d, int response, [[maybe_unused]] gpointer data) {
    if (response != GTK_RESPONSE_NONE)
        gtk_widget_destroy(GTK_WIDGET(d));
}

typedef enum text_style { PLAIN, BOLD, HEADING, TITLE } text_style;

static GtkWidget *label(const char *text, text_style style) {
    GtkWidget *l = gtk_label_new(nullptr);
    char *m = nullptr;
    switch (style) {
    case BOLD:
        m = g_markup_printf_escaped("<b>%s</b>", text);
        break;
    case HEADING:
        m = g_markup_printf_escaped("<span size='large' weight='bold'>%s</span>", text);
        break;
    case TITLE:
        m = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", text);
        break;
    case PLAIN:
        break;
    }
    if (m != nullptr)
        gtk_label_set_markup(GTK_LABEL(l), m);
    else
        gtk_label_set_text_with_mnemonic(GTK_LABEL(l), text);
    g_free(m);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
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
    NMActiveConnection *a = nm_client_activate_connection_finish(NM_CLIENT(src), res, &error);
    g_clear_object(&a);
    if (error != nullptr) {
        nc_error(nc_parent(app), "The connection was created, but could not connect.", error);
        g_error_free(error);
    }
    nc_release(app);
}

static void added(GObject *src, GAsyncResult *res, gpointer data) {
    add_job *j = data;
    GError *error = nullptr;
    NMRemoteConnection *c = nm_client_add_connection_finish(NM_CLIENT(src), res, &error);
    if (c == nullptr) {
        nc_error(nc_parent(j->app), "The connection could not be created.", error);
        g_error_free(error);
    } else if (j->connect) {
        nc_hold(j->app);
        nm_client_activate_connection_async(j->app->client, NM_CONNECTION(c), j->device, nullptr,
                                            nullptr, activated, j->app);
    }
    nc_release(j->app);
    g_clear_object(&c);
    g_clear_object(&j->device);
    g_free(j);
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

static kind chosen(wizard *w) {
    for (int i = 0; i < 3; i++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->kind[i])))
            return (kind)i;
    }
    return KIND_WIRED;
}

static const char *detail_page(wizard *w) {
    switch (chosen(w)) {
    case KIND_WIFI:
        return "wifi";
    case KIND_VPN:
        return "vpn";
    case KIND_WIRED:
        break;
    }
    return "wired";
}

static const char *security_key_mgmt(wizard *w) {
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(w->security));
    return i >= 0 && (size_t)i < XD_ARRAY_LEN(securities) ? securities[i].key_mgmt : nullptr;
}

static NMVpnPluginInfo *chosen_plugin(wizard *w) {
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(w->vpn_type));
    return i >= 0 ? g_slist_nth_data(w->plugins, (guint)i) : nullptr;
}

static const char *text(GtkWidget *e) { return gtk_entry_get_text(GTK_ENTRY(e)); }

static bool blank(GtkWidget *e) {
    char *t = g_strstrip(g_strdup(text(e)));
    bool b = t[0] == '\0';
    g_free(t);
    return b;
}

/* Whether the current page is filled in enough for Next. */
static bool page_ok(wizard *w) {
    if (strcmp(w->page, "wired") == 0)
        return !blank(w->wired_name);
    if (strcmp(w->page, "wifi") == 0) {
        size_t n = strlen(text(w->ssid));
        const char *km = security_key_mgmt(w);
        if (n == 0 || n > 32)
            return false;
        if (km == nullptr)
            return true;
        return xd_nc_valid_wifi_key(km, text(w->key)) && strcmp(text(w->key), text(w->confirm)) == 0;
    }
    if (strcmp(w->page, "vpn") == 0)
        return !blank(w->vpn_name) && chosen_plugin(w) != nullptr;
    return true;
}

static void show_page(wizard *w, const char *page) {
    w->page = page;
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), page);
    bool last = strcmp(page, "finish") == 0;
    gtk_widget_set_sensitive(w->back, strcmp(page, "welcome") != 0);
    gtk_widget_set_visible(w->next, !last);
    gtk_widget_set_visible(w->finish, last);
    gtk_widget_set_sensitive(w->next, page_ok(w));
    gtk_widget_grab_default(last ? w->finish : w->next);
}

static void on_input([[maybe_unused]] GtkWidget *x, gpointer data) {
    wizard *w = data;
    if (w->page == nullptr)
        return; /* still being built */
    bool keyed = security_key_mgmt(w) != nullptr;
    gtk_widget_set_sensitive(w->key, keyed);
    gtk_widget_set_sensitive(w->confirm, keyed);
    NMVpnPluginInfo *pi = chosen_plugin(w);
    gtk_widget_set_sensitive(
        w->vpn_host, pi != nullptr && xd_nc_vpn_host_key(nm_vpn_plugin_info_get_service(pi)) != nullptr);
    gtk_widget_set_sensitive(w->next, page_ok(w));
}

static char *connection_name(wizard *w) {
    switch (chosen(w)) {
    case KIND_WIFI:
        return g_strdup(text(w->ssid));
    case KIND_VPN:
        return g_strstrip(g_strdup(text(w->vpn_name)));
    case KIND_WIRED:
        break;
    }
    return g_strstrip(g_strdup(text(w->wired_name)));
}

static void on_next([[maybe_unused]] GtkButton *b, gpointer data) {
    wizard *w = data;
    if (!page_ok(w))
        return;
    if (strcmp(w->page, "welcome") == 0) {
        show_page(w, "type");
    } else if (strcmp(w->page, "type") == 0) {
        show_page(w, detail_page(w));
    } else {
        char *name = connection_name(w);
        char *m = g_markup_printf_escaped(
            "You have successfully completed the steps needed to create the following "
            "connection:\n\n<b>%s</b>\n\nThe connection will be saved in Network Connections.",
            name);
        gtk_label_set_markup(GTK_LABEL(w->summary), m);
        g_free(m);
        g_free(name);
        show_page(w, "finish");
    }
}

static void on_back([[maybe_unused]] GtkButton *b, gpointer data) {
    wizard *w = data;
    if (strcmp(w->page, "finish") == 0)
        show_page(w, detail_page(w));
    else if (strcmp(w->page, "type") == 0)
        show_page(w, "welcome");
    else
        show_page(w, "type");
}

static NMConnection *build(wizard *w, NMDevice **device) {
    *device = nullptr;
    kind k = chosen(w);
    NMConnection *c = nm_simple_connection_new();
    NMSetting *s = nm_setting_connection_new();
    char *uuid = nm_utils_uuid_generate();
    char *name = connection_name(w);
    g_object_set(s, NM_SETTING_CONNECTION_ID, name, NM_SETTING_CONNECTION_UUID, uuid,
                 NM_SETTING_CONNECTION_AUTOCONNECT, k != KIND_VPN, nullptr);
    g_free(name);
    g_free(uuid);
    nm_connection_add_setting(c, s);
    if (k == KIND_WIRED) {
        g_object_set(s, NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRED_SETTING_NAME, nullptr);
        const char *iface = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w->wired_device));
        if (iface != nullptr && iface[0] != '\0') {
            g_object_set(s, NM_SETTING_CONNECTION_INTERFACE_NAME, iface, nullptr);
            *device = nm_client_get_device_by_iface(w->app->client, iface);
        }
        nm_connection_add_setting(c, nm_setting_wired_new());
    } else if (k == KIND_WIFI) {
        g_object_set(s, NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME, nullptr);
        NMSetting *wifi = nm_setting_wireless_new();
        const char *ssid = text(w->ssid);
        GBytes *bytes = g_bytes_new(ssid, strlen(ssid));
        g_object_set(wifi, NM_SETTING_WIRELESS_SSID, bytes, NM_SETTING_WIRELESS_MODE,
                     NM_SETTING_WIRELESS_MODE_INFRA, nullptr);
        g_bytes_unref(bytes);
        nm_connection_add_setting(c, wifi);
        const char *km = security_key_mgmt(w);
        if (km != nullptr) {
            NMSetting *sec = nm_setting_wireless_security_new();
            g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, km, nullptr);
            if (strcmp(km, "none") == 0)
                g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_WEP_KEY0, text(w->key),
                             NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE, NM_WEP_KEY_TYPE_KEY,
                             nullptr);
            else
                g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_PSK, text(w->key), nullptr);
            nm_connection_add_setting(c, sec);
        }
    } else {
        g_object_set(s, NM_SETTING_CONNECTION_TYPE, NM_SETTING_VPN_SETTING_NAME, nullptr);
        NMVpnPluginInfo *pi = chosen_plugin(w);
        const char *service = nm_vpn_plugin_info_get_service(pi);
        NMSettingVpn *vpn = NM_SETTING_VPN(nm_setting_vpn_new());
        g_object_set(vpn, NM_SETTING_VPN_SERVICE_TYPE, service, nullptr);
        const char *host_key = xd_nc_vpn_host_key(service);
        char *host = g_strstrip(g_strdup(text(w->vpn_host)));
        if (host_key != nullptr && host[0] != '\0')
            nm_setting_vpn_add_data_item(vpn, host_key, host);
        g_free(host);
        nm_connection_add_setting(c, NM_SETTING(vpn));
    }
    s = nm_setting_ip4_config_new();
    g_object_set(s, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO, nullptr);
    nm_connection_add_setting(c, s);
    s = nm_setting_ip6_config_new();
    g_object_set(s, NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO, nullptr);
    nm_connection_add_setting(c, s);
    return c;
}

static void on_finish([[maybe_unused]] GtkButton *b, gpointer data) {
    wizard *w = data;
    NMDevice *device = nullptr;
    NMConnection *c = build(w, &device);
    add_job *j = g_new0(add_job, 1);
    j->app = w->app;
    j->device = device != nullptr ? g_object_ref(device) : nullptr;
    j->connect = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->connect_now));
    nc_hold(w->app);
    nm_client_add_connection_async(w->app->client, c, TRUE, nullptr, added, j);
    g_object_unref(c);
    gtk_widget_destroy(w->dialog);
}

static void on_wizard_destroy([[maybe_unused]] GtkWidget *x, gpointer data) {
    wizard *w = data;
    g_slist_free_full(w->plugins, g_object_unref);
    g_free(w);
}

static GtkWidget *page_box(const char *title, const char *subtitle) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_pack_start(GTK_BOX(box), label(title, BOLD), FALSE, FALSE, 0);
    if (subtitle != nullptr) {
        GtkWidget *s = label(subtitle, PLAIN);
        gtk_widget_set_margin_start(s, 14);
        gtk_box_pack_start(GTK_BOX(box), s, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE,
                       0);
    return box;
}

static GtkWidget *form_entry(GtkWidget *grid, int y, const char *name, GtkWidget *field) {
    GtkWidget *l = label(name, PLAIN);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), field);
    gtk_grid_attach(GTK_GRID(grid), l, 0, y, 1, 1);
    gtk_widget_set_hexpand(field, TRUE);
    gtk_grid_attach(GTK_GRID(grid), field, 1, y, 1, 1);
    return field;
}

static GtkWidget *entry(wizard *w, bool secret) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    if (secret) {
        gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
    }
    g_signal_connect(e, "changed", G_CALLBACK(on_input), w);
    return e;
}

static GtkWidget *form(void) {
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 8);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    gtk_widget_set_margin_start(g, 14);
    return g;
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

static GtkWidget *type_page(wizard *w) {
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
        w->kind[i] = i == 0 ? gtk_radio_button_new_with_mnemonic(nullptr, kinds[i][0])
                            : gtk_radio_button_new_with_mnemonic_from_widget(
                                  GTK_RADIO_BUTTON(w->kind[0]), kinds[i][0]);
        gtk_box_pack_start(GTK_BOX(box), w->kind[i], FALSE, FALSE, 0);
        GtkWidget *d = label(kinds[i][1], PLAIN);
        gtk_widget_set_margin_start(d, 26);
        gtk_box_pack_start(GTK_BOX(box), d, FALSE, FALSE, 0);
    }
    return box;
}

static GtkWidget *wired_page(wizard *w) {
    GtkWidget *box = page_box("Wired Connection", "Name the connection and pick the adapter it uses.");
    GtkWidget *g = form();
    w->wired_name = form_entry(g, 0, "Connection _name:", entry(w, false));
    /* "Local Area Connection N", the next free number. */
    unsigned n = 1;
    for (char *name = xd_nc_numbered_name("Local Area Connection", n);;
         name = xd_nc_numbered_name("Local Area Connection", ++n)) {
        bool taken = false;
        const GPtrArray *all = nm_client_get_connections(w->app->client);
        for (guint i = 0; all != nullptr && i < all->len && !taken; i++)
            taken = strcmp(nm_connection_get_id(g_ptr_array_index(all, i)), name) == 0;
        if (!taken) {
            gtk_entry_set_text(GTK_ENTRY(w->wired_name), name);
            g_free(name);
            break;
        }
        g_free(name);
    }
    w->wired_device = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->wired_device), "", "Any wired adapter");
    const GPtrArray *devs = nm_client_get_devices(w->app->client);
    for (guint i = 0; devs != nullptr && i < devs->len; i++) {
        NMDevice *d = g_ptr_array_index(devs, i);
        if (!NM_IS_DEVICE_ETHERNET(d))
            continue;
        const char *desc = nm_device_get_description(d);
        const char *iface = nm_device_get_iface(d);
        char *t = desc != nullptr && strcmp(desc, iface) != 0
                      ? g_strdup_printf("%s (%s)", desc, iface)
                      : g_strdup(iface);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->wired_device), nm_device_get_iface(d), t);
        g_free(t);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->wired_device), 0);
    form_entry(g, 1, "Connect _using:", w->wired_device);
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *wifi_page(wizard *w) {
    GtkWidget *box = page_box("Wireless Network",
                              "Type the network's name and how it is secured. To pick a network "
                              "in range instead, use View Available Wireless Networks.");
    GtkWidget *g = form();
    w->ssid = form_entry(g, 0, "Network _name (SSID):", entry(w, false));
    gtk_entry_set_max_length(GTK_ENTRY(w->ssid), 32);
    w->security = gtk_combo_box_text_new();
    for (size_t i = 0; i < XD_ARRAY_LEN(securities); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->security), securities[i].label);
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->security), 0);
    g_signal_connect(w->security, "changed", G_CALLBACK(on_input), w);
    form_entry(g, 1, "_Security:", w->security);
    w->key = form_entry(g, 2, "Network _key:", entry(w, true));
    w->confirm = form_entry(g, 3, "C_onfirm network key:", entry(w, true));
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *vpn_page(wizard *w) {
    GtkWidget *box = page_box("VPN Connection",
                              "Name the connection, pick the kind of VPN and type its server.");
    GtkWidget *g = form();
    w->vpn_name = form_entry(g, 0, "Company N_ame:", entry(w, false));
    w->vpn_type = gtk_combo_box_text_new();
    w->plugins = nm_vpn_plugin_info_list_load();
    for (GSList *l = w->plugins; l != nullptr; l = l->next)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->vpn_type),
                                       nm_vpn_plugin_info_get_name(l->data));
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->vpn_type), w->plugins != nullptr ? 0 : -1);
    g_signal_connect(w->vpn_type, "changed", G_CALLBACK(on_input), w);
    form_entry(g, 1, "VPN _type:", w->vpn_type);
    w->vpn_host = form_entry(g, 2, "_Host name or IP address:", entry(w, false));
    gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
    if (w->plugins == nullptr)
        gtk_box_pack_start(GTK_BOX(box),
                           label("No VPN plugins for NetworkManager are installed. Install one "
                                 "(such as NetworkManager-openvpn) to create a VPN connection.",
                                 PLAIN),
                           FALSE, FALSE, 0);
    return box;
}

static GtkWidget *finish_page(wizard *w) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_pack_start(GTK_BOX(box),
                       label("Completing the New Connection Wizard",
                             TITLE),
                       FALSE, FALSE, 0);
    w->summary = label("", PLAIN);
    gtk_box_pack_start(GTK_BOX(box), w->summary, FALSE, FALSE, 0);
    w->connect_now = gtk_check_button_new_with_mnemonic("_Connect now");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->connect_now), TRUE);
    gtk_box_pack_start(GTK_BOX(box), w->connect_now, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), label("To create the connection and close this wizard, click "
                                         "Finish.",
                                         PLAIN),
                     FALSE, FALSE, 0);
    return box;
}

void nc_wizard_show(nc_app *app) {
    if (app->client == nullptr || !nm_client_get_nm_running(app->client))
        return;
    wizard *w = g_new0(wizard, 1);
    w->app = app;
    w->dialog = gtk_dialog_new();
    nc_track(app, w->dialog);
    gtk_window_set_title(GTK_WINDOW(w->dialog), "New Connection Wizard");
    gtk_window_set_icon_name(GTK_WINDOW(w->dialog), "tools-wizard");
    gtk_window_set_resizable(GTK_WINDOW(w->dialog), FALSE);
    if (nc_parent(app) != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(w->dialog), nc_parent(app));
    w->back = gtk_dialog_add_button(GTK_DIALOG(w->dialog), "< _Back", GTK_RESPONSE_NONE);
    w->next = gtk_dialog_add_button(GTK_DIALOG(w->dialog), "_Next >", GTK_RESPONSE_NONE);
    w->finish = gtk_dialog_add_button(GTK_DIALOG(w->dialog), "Finish", GTK_RESPONSE_NONE);
    gtk_dialog_add_button(GTK_DIALOG(w->dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_widget_set_can_default(w->next, TRUE);
    gtk_widget_set_can_default(w->finish, TRUE);
    g_signal_connect(w->back, "clicked", G_CALLBACK(on_back), w);
    g_signal_connect(w->next, "clicked", G_CALLBACK(on_next), w);
    g_signal_connect(w->finish, "clicked", G_CALLBACK(on_finish), w);
    g_signal_connect(w->dialog, "response", G_CALLBACK(on_response), nullptr);
    g_signal_connect(w->dialog, "destroy", G_CALLBACK(on_wizard_destroy), w);

    w->stack = gtk_stack_new();
    gtk_widget_set_size_request(w->stack, 500, 320);
    gtk_stack_add_named(GTK_STACK(w->stack), welcome_page(), "welcome");
    gtk_stack_add_named(GTK_STACK(w->stack), type_page(w), "type");
    gtk_stack_add_named(GTK_STACK(w->stack), wired_page(w), "wired");
    gtk_stack_add_named(GTK_STACK(w->stack), wifi_page(w), "wifi");
    gtk_stack_add_named(GTK_STACK(w->stack), vpn_page(w), "vpn");
    gtk_stack_add_named(GTK_STACK(w->stack), finish_page(w), "finish");
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(w->dialog));
    gtk_box_pack_start(GTK_BOX(content), w->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    gtk_widget_show_all(w->dialog);
    show_page(w, "welcome");
    on_input(nullptr, w);
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
    GBytes *b = nm_access_point_get_ssid(ap);
    return b != nullptr && g_bytes_get_size(b) > 0
               ? nm_utils_ssid_to_utf8(g_bytes_get_data(b, nullptr), g_bytes_get_size(b))
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
    NMRemoteConnection *c = active != nullptr ? nm_active_connection_get_connection(active) : nullptr;
    NMSettingWireless *s = c != nullptr ? nm_connection_get_setting_wireless(NM_CONNECTION(c))
                                        : nullptr;
    GBytes *b = s != nullptr ? nm_setting_wireless_get_ssid(s) : nullptr;
    if (ssid == nullptr && b != nullptr)
        ssid = nm_utils_ssid_to_utf8(g_bytes_get_data(b, nullptr), g_bytes_get_size(b));
    return ssid;
}

/* A saved connection for the network, if any. */
static NMRemoteConnection *saved_for(chooser *c, NMAccessPoint *ap) {
    const GPtrArray *all = nm_client_get_connections(c->app->client);
    GPtrArray *mine = g_ptr_array_new();
    for (guint i = 0; all != nullptr && i < all->len; i++) {
        NMConnection *conn = g_ptr_array_index(all, i);
        if (nm_connection_is_type(conn, NM_SETTING_WIRELESS_SETTING_NAME))
            g_ptr_array_add(mine, conn);
    }
    GPtrArray *match = nm_access_point_filter_connections(ap, mine);
    NMRemoteConnection *r = match != nullptr && match->len > 0 ? g_ptr_array_index(match, 0)
                                                               : nullptr;
    if (match != nullptr)
        g_ptr_array_unref(match);
    g_ptr_array_unref(mine);
    return r;
}

static void fill_networks(chooser *c) {
    char *keep = nullptr;
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
    if (gtk_tree_selection_get_selected(sel, &model, &iter))
        gtk_tree_model_get(model, &iter, NET_AP, &keep, -1);
    gtk_list_store_clear(c->store);
    NMDeviceWifi *wifi = NM_DEVICE_WIFI(c->device);
    char *active_ssid = connected_ssid(c->device);
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
        bool current = active_ssid != nullptr && strcmp(ssid, active_ssid) == 0;
        bool saved = saved_for(c, ap) != nullptr;
        unsigned strength = nm_access_point_get_strength(ap);
        char *markup = g_markup_printf_escaped(
            "<b>%s</b>%s\n<small>%s</small>", ssid,
            current ? "  —  Connected" : saved ? "  —  Automatic" : "",
            security_text(ap));
        char *signal = g_strdup_printf("%s (%u%%)", xd_nc_signal_words(strength), strength);
        gtk_list_store_insert_with_values(c->store, &iter, -1, NET_ICON, signal_icon(strength),
                                          NET_TEXT, markup, NET_SIGNAL, signal, NET_AP,
                                          nm_object_get_path(NM_OBJECT(ap)), NET_STRENGTH,
                                          strength, NET_CURRENT, current, -1);
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

static NMAccessPoint *selected_ap(chooser *c, bool *current) {
    GtkTreeModel *model;
    GtkTreeIter iter;
    char *path = nullptr;
    gboolean cur = FALSE;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)),
                                         &model, &iter))
        return nullptr;
    gtk_tree_model_get(model, &iter, NET_AP, &path, NET_CURRENT, &cur, -1);
    NMAccessPoint *found = nullptr;
    const GPtrArray *aps = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(c->device));
    for (guint i = 0; aps != nullptr && i < aps->len && found == nullptr; i++) {
        if (strcmp(nm_object_get_path(NM_OBJECT(g_ptr_array_index(aps, i))), path) == 0)
            found = g_ptr_array_index(aps, i);
    }
    g_free(path);
    if (current != nullptr)
        *current = cur;
    return found;
}

static void on_network_selected([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    chooser *c = data;
    bool current = false;
    NMAccessPoint *ap = selected_ap(c, &current);
    gtk_button_set_label(GTK_BUTTON(c->connect), current ? "_Disconnect" : "C_onnect");
    gtk_widget_set_sensitive(c->connect, ap != nullptr);
}

static void on_aps_changed([[maybe_unused]] GObject *o, [[maybe_unused]] GParamSpec *ps,
                           gpointer data) {
    fill_networks(data);
    on_network_selected(nullptr, data);
}

typedef struct key_job {
    nc_app *app;
    NMDevice *device;
    char *ap_path, *key_mgmt;
} key_job;

static void key_job_free(key_job *k) {
    g_clear_object(&k->device);
    g_free(k->ap_path);
    g_free(k->key_mgmt);
    g_free(k);
}

static void joined(GObject *src, GAsyncResult *res, gpointer data) {
    nc_app *app = data;
    GError *error = nullptr;
    NMActiveConnection *a = nm_client_add_and_activate_connection_finish(NM_CLIENT(src), res,
                                                                         &error);
    g_clear_object(&a);
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
    NMConnection *c = nm_simple_connection_new();
    NMSetting *sec = nm_setting_wireless_security_new();
    g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, key_mgmt, nullptr);
    if (strcmp(key_mgmt, "none") == 0)
        g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_WEP_KEY0, key,
                     NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE, NM_WEP_KEY_TYPE_KEY, nullptr);
    else
        g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_PSK, key, nullptr);
    nm_connection_add_setting(c, sec);
    return c;
}

static void join(nc_app *app, NMDevice *device, const char *ap_path, const char *key_mgmt,
                 const char *key) {
    NMConnection *c = partial(key_mgmt, key);
    nc_hold(app);
    nm_client_add_and_activate_connection_async(app->client, c, device, ap_path, nullptr, joined,
                                                app);
    g_clear_object(&c);
}

static void got_key(GObject *src, GAsyncResult *res, gpointer data) {
    key_job *k = data;
    char *out = nullptr;
    GError *error = nullptr;
    /* No output means Cancel (its exit status may be a sanitizer's). */
    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, nullptr, &error) &&
        out != nullptr) {
        g_strchomp(out);
        if (out[0] != '\0')
            join(k->app, k->device, k->ap_path, k->key_mgmt, out);
    }
    if (out != nullptr) {
        explicit_bzero(out, strlen(out));
        g_free(out);
    }
    g_clear_error(&error);
    nc_release(k->app);
    key_job_free(k);
}

/* Asks for the key with xd-wifi-key, which prints it (as the shell's
 * network icon does). */
static void ask_key(chooser *c, NMAccessPoint *ap, const char *ssid, const char *key_mgmt) {
    char *prog = nc_own_program("xd-wifi-key");
    GError *error = nullptr;
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, prog, ssid,
                                      nullptr);
    g_free(prog);
    if (p == nullptr) {
        nc_error(GTK_WINDOW(c->dialog), "The network key prompt could not be started.", error);
        g_error_free(error);
        return;
    }
    key_job *k = g_new0(key_job, 1);
    k->app = c->app;
    nc_hold(c->app);
    k->device = g_object_ref(c->device);
    k->ap_path = g_strdup(nm_object_get_path(NM_OBJECT(ap)));
    k->key_mgmt = g_strdup(key_mgmt);
    g_subprocess_communicate_utf8_async(p, nullptr, nullptr, got_key, k);
    g_object_unref(p);
}

static void on_connect([[maybe_unused]] GtkButton *b, gpointer data) {
    chooser *c = data;
    bool current = false;
    NMAccessPoint *ap = selected_ap(c, &current);
    if (ap == nullptr)
        return;
    if (current) {
        char *key = g_strdup_printf("dev:%s", nm_object_get_path(NM_OBJECT(c->device)));
        nc_disable(c->app, key);
        g_free(key);
        return;
    }
    const char *ap_path = nm_object_get_path(NM_OBJECT(ap));
    NMRemoteConnection *saved = saved_for(c, ap);
    if (saved != nullptr) {
        nc_hold(c->app);
        nm_client_activate_connection_async(c->app->client, NM_CONNECTION(saved), c->device,
                                            ap_path, nullptr, activated, c->app);
        return;
    }
    const char *km = xd_nc_wifi_key_mgmt(nm_access_point_get_flags(ap),
                                         nm_access_point_get_wpa_flags(ap),
                                         nm_access_point_get_rsn_flags(ap));
    if (km == nullptr) {
        join(c->app, c->device, ap_path, nullptr, nullptr);
    } else if (strcmp(km, "wpa-eap") == 0) {
        /* 802.1X needs certificates and more: NetworkManager's editor. */
        if (nc_have_editor())
            nc_run_editor(GTK_WINDOW(c->dialog), nullptr);
        else
            xd_message_box(GTK_WINDOW(c->dialog), "Wireless Network Connection", XD_MB_INFO,
                           XD_MB_OK, FALSE,
                           "This network uses 802.1X authentication. Set it up with "
                           "NetworkManager's connection editor (nm-connection-editor).");
    } else {
        char *ssid = ap_ssid(ap);
        ask_key(c, ap, ssid != nullptr ? ssid : "", km);
        g_free(ssid);
    }
}

static void on_refresh([[maybe_unused]] GtkButton *b, gpointer data) {
    chooser *c = data;
    nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(c->device), nullptr, nullptr, nullptr);
    fill_networks(c);
}

static void on_network_activated([[maybe_unused]] GtkTreeView *tv,
                                 [[maybe_unused]] GtkTreePath *path,
                                 [[maybe_unused]] GtkTreeViewColumn *col, gpointer data) {
    on_connect(nullptr, data);
}

static void on_chooser_destroy([[maybe_unused]] GtkWidget *x, gpointer data) {
    chooser *c = data;
    g_signal_handlers_disconnect_by_data(c->device, c);
    g_object_unref(c->device);
    g_free(c);
}

void nc_wireless_show(nc_app *app, NMDevice *device) {
    if (device == nullptr || !NM_IS_DEVICE_WIFI(device))
        return;
    chooser *c = g_new0(chooser, 1);
    c->app = app;
    c->device = g_object_ref(device);
    c->dialog = gtk_dialog_new();
    nc_track(app, c->dialog);
    gtk_window_set_title(GTK_WINDOW(c->dialog), "Wireless Network Connection");
    gtk_window_set_icon_name(GTK_WINDOW(c->dialog), "network-wireless");
    gtk_window_set_default_size(GTK_WINDOW(c->dialog), 520, 420);
    if (nc_parent(app) != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(c->dialog), nc_parent(app));
    GtkWidget *refresh = gtk_dialog_add_button(GTK_DIALOG(c->dialog), "_Refresh network list",
                                               GTK_RESPONSE_NONE);
    c->connect = gtk_dialog_add_button(GTK_DIALOG(c->dialog), "C_onnect", GTK_RESPONSE_NONE);
    gtk_dialog_add_button(GTK_DIALOG(c->dialog), "_Close", GTK_RESPONSE_CLOSE);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), c);
    g_signal_connect(c->connect, "clicked", G_CALLBACK(on_connect), c);
    g_signal_connect(c->dialog, "response", G_CALLBACK(on_response), nullptr);
    g_signal_connect(c->dialog, "destroy", G_CALLBACK(on_chooser_destroy), c);

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
    c->store = gtk_list_store_new(NET_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_UINT, G_TYPE_BOOLEAN);
    GtkTreeSortable *sort = GTK_TREE_SORTABLE(c->store);
    gtk_tree_sortable_set_sort_column_id(sort, NET_STRENGTH, GTK_SORT_DESCENDING);
    c->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c->store));
    g_object_unref(c->store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(c->view), FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_pixbuf_new();
    g_object_set(r, "stock-size", GTK_ICON_SIZE_DND, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(c->view), gtk_tree_view_column_new_with_attributes(
                                                            "", r, "icon-name", NET_ICON, nullptr));
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        "Network", gtk_cell_renderer_text_new(), "markup", NET_TEXT, nullptr);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(c->view), col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(c->view),
                                gtk_tree_view_column_new_with_attributes(
                                    "Signal", gtk_cell_renderer_text_new(), "text", NET_SIGNAL,
                                    nullptr));
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view)), "changed",
                     G_CALLBACK(on_network_selected), c);
    g_signal_connect(c->view, "row-activated", G_CALLBACK(on_network_activated), c);
    GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), c->view);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(c->dialog))), box, TRUE,
                       TRUE, 0);
    g_signal_connect(c->device, "notify::access-points", G_CALLBACK(on_aps_changed), c);
    g_signal_connect(c->device, "notify::active-access-point", G_CALLBACK(on_aps_changed), c);
    g_signal_connect(c->device, "notify::state", G_CALLBACK(on_aps_changed), c);
    fill_networks(c);
    on_network_selected(nullptr, c);
    nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(device), nullptr, nullptr, nullptr);
    gtk_widget_show_all(c->dialog);
}
