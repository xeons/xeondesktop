/* SPDX-License-Identifier: MIT
 * Network Connections without a window: see netconn.h.
 */
#include "netconn.h"

#include <gio/gio.h>
#include <stdbit.h>

#include "../core/common.h"

/* NetworkManager's D-Bus enums (nm-dbus-interface.h), the values we use. */
enum {
    DEV_UNMANAGED = 10,
    DEV_UNAVAILABLE = 20,
    DEV_DISCONNECTED = 30,
    DEV_PREPARE = 40,
    DEV_CONFIG = 50,
    DEV_NEED_AUTH = 60,
    DEV_IP_CONFIG = 70,
    DEV_IP_CHECK = 80,
    DEV_SECONDARIES = 90,
    DEV_ACTIVATED = 100,
    DEV_DEACTIVATING = 110,
    DEV_FAILED = 120,
};
enum { ACTIVE_ACTIVATING = 1, ACTIVE_ACTIVATED = 2, ACTIVE_DEACTIVATING = 3 };
enum { CONNECTIVITY_NONE = 1, CONNECTIVITY_LIMITED = 3 };
enum {
    AP_PRIVACY = 0x1,
    KEY_MGMT_PSK = 0x100,
    KEY_MGMT_8021X = 0x200,
    KEY_MGMT_SAE = 0x400,
    KEY_MGMT_OWE = 0x800,
    KEY_MGMT_EAP_SUITE_B = 0x2000,
};

const char *xd_nc_group_title(xd_nc_group connection_group) {
    switch (connection_group) {
    case XD_NC_LAN:
        return "LAN or High-Speed Internet";
    case XD_NC_WIRELESS:
        return "Wireless";
    case XD_NC_BROADBAND:
        return "Broadband";
    case XD_NC_VPN:
        return "Virtual Private Network";
    case XD_NC_PAN:
        return "Personal Area Network";
    case XD_NC_WIZARD:
        return "Wizard";
    case XD_NC_OTHER:
    case XD_NC_GROUP_COUNT:
        break;
    }
    return "Other";
}

xd_nc_group xd_nc_group_for_type(const char *type) {
    static const struct {
        const char *type;
        xd_nc_group group;
    } map[] = {
        {"802-3-ethernet", XD_NC_LAN},  {"bond", XD_NC_LAN},          {"bridge", XD_NC_LAN},
        {"team", XD_NC_LAN},            {"vlan", XD_NC_LAN},          {"macvlan", XD_NC_LAN},
        {"infiniband", XD_NC_LAN},      {"veth", XD_NC_LAN},          {"802-11-wireless", XD_NC_WIRELESS},
        {"wifi-p2p", XD_NC_WIRELESS},   {"gsm", XD_NC_BROADBAND},     {"cdma", XD_NC_BROADBAND},
        {"pppoe", XD_NC_BROADBAND},     {"adsl", XD_NC_BROADBAND},    {"vpn", XD_NC_VPN},
        {"wireguard", XD_NC_VPN},       {"bluetooth", XD_NC_PAN},
    };
    for (size_t i = 0; type != nullptr && i < XD_ARRAY_LEN(map); i++) {
        if (strcmp(type, map[i].type) == 0)
            return map[i].group;
    }
    return XD_NC_OTHER;
}

bool xd_nc_state_disabled(const xd_nc_state *state) {
    if (!state->has_device)
        return false;
    if (!state->managed || state->device_state == DEV_UNMANAGED || state->radio_off)
        return true;
    /* NetworkManager stops connecting a device by itself once it's been
     * disconnected by hand: that is XP's Disable. */
    return state->device_state == DEV_DISCONNECTED && !state->autoconnect;
}

bool xd_nc_state_connected(const xd_nc_state *state) {
    if (!state->has_device)
        return state->active_state == ACTIVE_ACTIVATED;
    return state->managed && state->device_state == DEV_ACTIVATED;
}

const char *xd_nc_status_text(const xd_nc_state *state) {
    if (!state->has_device) {
        switch (state->active_state) {
        case ACTIVE_ACTIVATING:
            return "Connecting";
        case ACTIVE_ACTIVATED:
            return "Connected";
        case ACTIVE_DEACTIVATING:
            return "Disconnecting";
        default:
            return "Disconnected";
        }
    }
    if (xd_nc_state_disabled(state))
        return "Disabled";
    if (state->carrier_known && !state->carrier &&
        (state->device_state == DEV_UNAVAILABLE || state->device_state == DEV_DISCONNECTED))
        return "Network cable unplugged";
    switch (state->device_state) {
    case DEV_UNAVAILABLE:
        return "Not connected";
    case DEV_PREPARE:
        return "Connecting";
    case DEV_CONFIG:
    case DEV_NEED_AUTH:
        return "Authenticating";
    case DEV_IP_CONFIG:
    case DEV_IP_CHECK:
    case DEV_SECONDARIES:
        return "Acquiring network address";
    case DEV_ACTIVATED:
        return state->connectivity >= CONNECTIVITY_NONE &&
                       state->connectivity <= CONNECTIVITY_LIMITED
                   ? "Limited or no connectivity"
                   : "Connected";
    case DEV_DEACTIVATING:
        return "Disconnecting";
    case DEV_DISCONNECTED:
    case DEV_FAILED:
    default:
        return "Not connected";
    }
}

char *xd_nc_numbered_name(const char *base, unsigned number) {
    return number <= 1 ? g_strdup(base) : g_strdup_printf("%s %u", base, number);
}

/* ---- the Status dialog's numbers ----------------------------------------------- */

char *xd_nc_format_duration(guint64 seconds) {
    guint64 days = seconds / 86400;
    unsigned hour_part = (unsigned)(seconds / 3600 % 24),
             minute_part = (unsigned)(seconds / 60 % 60), second_part = (unsigned)(seconds % 60);
    if (days == 0)
        return g_strdup_printf("%02u:%02u:%02u", hour_part, minute_part, second_part);
    return g_strdup_printf("%" G_GUINT64_FORMAT " day%s %02u:%02u:%02u", days, days == 1 ? "" : "s",
                           hour_part, minute_part, second_part);
}

char *xd_nc_format_speed(guint64 kbps) {
    /* One decimal, as XP: 10.0 Mbps, 54.0 Mbps, 1.0 Gbps. */
    if (kbps >= 1000000)
        return g_strdup_printf("%" G_GUINT64_FORMAT ".%" G_GUINT64_FORMAT " Gbps", kbps / 1000000,
                               kbps / 100000 % 10);
    if (kbps >= 1000)
        return g_strdup_printf("%" G_GUINT64_FORMAT ".%" G_GUINT64_FORMAT " Mbps", kbps / 1000,
                               kbps / 100 % 10);
    return g_strdup_printf("%" G_GUINT64_FORMAT ".0 Kbps", kbps);
}

char *xd_nc_format_count(guint64 count) {
    char digits[32];
    g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, count);
    size_t len = strlen(digits);
    GString *out = g_string_sized_new(len + len / 3);
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            g_string_append_c(out, ',');
        g_string_append_c(out, digits[i]);
    }
    return g_string_free(out, FALSE);
}

/* ---- IPv4 ------------------------------------------------------------------------- */

/* A dotted quad as a number (host order); false if it isn't one. GIO
 * parses it (inet_pton rules: four decimal parts). */
static bool parse_ipv4(const char *text, guint32 *out) {
    if (text == nullptr || text[0] == '\0')
        return false;
    GInetAddress *address = g_inet_address_new_from_string(text);
    if (address == nullptr)
        return false;
    bool ok = g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4;
    if (ok) {
        const guint8 *bytes = g_inet_address_to_bytes(address);
        *out =
            (guint32)bytes[0] << 24 | (guint32)bytes[1] << 16 | (guint32)bytes[2] << 8 | bytes[3];
    }
    g_object_unref(address);
    return ok;
}

bool xd_nc_valid_ipv4(const char *text) {
    guint32 parsed;
    return parse_ipv4(text, &parsed);
}

bool xd_nc_netmask_to_prefix(const char *mask, unsigned *prefix) {
    guint32 mask_bits;
    if (!parse_ipv4(mask, &mask_bits))
        return false;
    /* Ones, then only zeros. */
    guint32 inverted = ~mask_bits;
    if ((inverted & (inverted + 1)) != 0)
        return false;
    *prefix = stdc_count_ones_ui(mask_bits);
    return true;
}

char *xd_nc_prefix_to_netmask(unsigned prefix) {
    guint32 mask_bits = prefix == 0 ? 0 : prefix >= 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1);
    return g_strdup_printf("%u.%u.%u.%u", mask_bits >> 24, mask_bits >> 16 & 0xff,
                           mask_bits >> 8 & 0xff, mask_bits & 0xff);
}

unsigned xd_nc_default_prefix(const char *ip) {
    guint32 address;
    if (!parse_ipv4(ip, &address))
        return 0;
    unsigned first = address >> 24;
    return first < 128 ? 8 : first < 192 ? 16 : first < 224 ? 24 : 0;
}

static bool blank(const char *text) { return text == nullptr || text[0] == '\0'; }

const char *xd_nc_check_ipv4_form(const xd_nc_ipv4_form *form) {
    guint32 ip = 0, gw = 0, dns = 0;
    unsigned prefix = 0;
    if (form->manual_ip) {
        if (blank(form->address))
            return "You must type an IP address.";
        if (!parse_ipv4(form->address, &ip))
            return "The IP address you typed is not valid.";
        if (blank(form->netmask))
            return "You must type a subnet mask.";
        if (!xd_nc_netmask_to_prefix(form->netmask, &prefix) || prefix == 0)
            return "The subnet mask you typed is not valid.";
        guint32 host = prefix >= 32 ? 0 : ~0u >> prefix;
        if (prefix < 31 && ((ip & host) == 0 || (ip & host) == host))
            return "The combination of IP address and subnet mask is invalid. All of the bits "
                   "in the host address portion of the IP address are set to 0 or 1.";
        if (ip >> 24 == 0 || ip >> 24 == 127 || ip >> 24 >= 224)
            return "The IP address you typed can't be used by a network adapter.";
        if (!blank(form->gateway)) {
            if (!parse_ipv4(form->gateway, &gw))
                return "The default gateway you typed is not valid.";
            guint32 net = prefix >= 32 ? ~0u : ~(~0u >> prefix);
            if ((gw & net) != (ip & net))
                return "The default gateway is not on the same network segment (subnet) that "
                       "is defined by the IP address and subnet mask.";
        }
    }
    if (form->manual_ip || form->manual_dns) {
        if (!blank(form->dns1) && !parse_ipv4(form->dns1, &dns))
            return "The preferred DNS server you typed is not valid.";
        if (!blank(form->dns2) && !parse_ipv4(form->dns2, &dns))
            return "The alternate DNS server you typed is not valid.";
    }
    return nullptr;
}

/* ---- VPN and Wi-Fi ------------------------------------------------------------------ */

const char *xd_nc_vpn_host_key(const char *service_type) {
    static const struct {
        const char *plugin, *key;
    } map[] = {
        {"openvpn", "remote"},   {"pptp", "gateway"},       {"l2tp", "gateway"},
        {"openconnect", "gateway"}, {"sstp", "gateway"},    {"fortisslvpn", "gateway"},
        {"vpnc", "IPSec gateway"}, {"strongswan", "address"}, {"libreswan", "right"},
    };
    const char *dot = service_type != nullptr ? strrchr(service_type, '.') : nullptr;
    const char *plugin = dot != nullptr ? dot + 1 : service_type;
    for (size_t i = 0; plugin != nullptr && i < XD_ARRAY_LEN(map); i++) {
        if (strcmp(plugin, map[i].plugin) == 0)
            return map[i].key;
    }
    return nullptr;
}

const char *xd_nc_wifi_key_mgmt(unsigned flags, unsigned wpa_flags, unsigned rsn_flags) {
    unsigned all = wpa_flags | rsn_flags;
    if (all & (KEY_MGMT_8021X | KEY_MGMT_EAP_SUITE_B))
        return "wpa-eap";
    if (all & KEY_MGMT_PSK)
        return "wpa-psk";
    if (all & KEY_MGMT_SAE)
        return "sae";
    if (all & KEY_MGMT_OWE)
        return nullptr; /* Enhanced Open: no key */
    if (flags & AP_PRIVACY)
        return "none"; /* WEP */
    return nullptr;
}

static bool all_hex(const char *text) {
    for (; *text != '\0'; text++) {
        if (!g_ascii_isxdigit(*text))
            return false;
    }
    return true;
}

bool xd_nc_valid_wifi_key(const char *key_mgmt, const char *key) {
    if (key == nullptr)
        return false;
    size_t length = strlen(key);
    if (g_strcmp0(key_mgmt, "none") == 0)
        return length == 5 || length == 13 || ((length == 10 || length == 26) && all_hex(key));
    if (g_strcmp0(key_mgmt, "wpa-psk") == 0)
        return (length >= 8 && length <= 63) || (length == 64 && all_hex(key));
    if (g_strcmp0(key_mgmt, "sae") == 0)
        return length >= 8 && length <= 63;
    return length == 0;
}

const char *xd_nc_signal_words(unsigned strength) {
    return strength >= 80   ? "Excellent"
           : strength >= 60 ? "Very good"
           : strength >= 40 ? "Good"
           : strength >= 20 ? "Low"
                            : "Very low";
}
