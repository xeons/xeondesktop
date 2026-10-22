/* SPDX-License-Identifier: MIT
 * The parts of Network Connections (xd-netconn) that don't need a window
 * or NetworkManager: XP's groups and status texts for NetworkManager's
 * states, the Status dialog's numbers, and checking the TCP/IP form.
 */
#ifndef XD_NETCONN_NETCONN_H
#define XD_NETCONN_NETCONN_H

#include <glib.h>

/* XP's groups, in the order the window shows them. */
typedef enum xd_nc_group {
    XD_NC_LAN,       /* LAN or High-Speed Internet: Ethernet, bridges, bonds, VLANs */
    XD_NC_WIRELESS,  /* Wi-Fi */
    XD_NC_BROADBAND, /* mobile broadband, DSL (PPPoE) */
    XD_NC_VPN,       /* VPN plugins, WireGuard */
    XD_NC_PAN,       /* Bluetooth */
    XD_NC_OTHER,
    XD_NC_WIZARD, /* New Connection Wizard */
    XD_NC_GROUP_COUNT,
} xd_nc_group;

const char *xd_nc_group_title(xd_nc_group connection_group);
/* The group for a NetworkManager connection type ("802-3-ethernet",
 * "vpn", ...; the setting names, which libnm's device types map to). */
xd_nc_group xd_nc_group_for_type(const char *type);

/* What a connection's status text depends on. The numbers are
 * NetworkManager's D-Bus enums (NMDeviceState, NMActiveConnectionState,
 * NMConnectivityState), which are stable API. */
typedef struct xd_nc_state {
    bool has_device;
    unsigned device_state;
    bool managed, autoconnect;
    bool carrier_known, carrier; /* Ethernet's cable */
    bool radio_off;              /* Wi-Fi switched off */
    unsigned active_state;       /* the connection's, for those without a device (VPN) */
    unsigned connectivity;       /* the device's IPv4 connectivity */
} xd_nc_state;

/* "Connected", "Disabled", "Network cable unplugged", "Not connected",
 * "Acquiring network address", ... */
const char *xd_nc_status_text(const xd_nc_state *state);
/* Whether the connection is up (for Status, Disconnect, the icon). */
bool xd_nc_state_connected(const xd_nc_state *state);
/* Whether XP would call it disabled (its icon is greyed, Enable shows). */
bool xd_nc_state_disabled(const xd_nc_state *state);

/* "Local Area Connection", "Local Area Connection 2", ... (n from 1). */
char *xd_nc_numbered_name(const char *base, unsigned number);

/* The Status dialog: "00:12:34", "1 day 02:03:04"; "100.0 Mbps" from
 * kbit/s; "1,234,567". g_free them. */
char *xd_nc_format_duration(guint64 seconds);
char *xd_nc_format_speed(guint64 kbps);
char *xd_nc_format_count(guint64 count);

/* IPv4 for the TCP/IP dialog. A subnet mask ("255.255.255.0") as a
 * prefix length; false if it isn't a mask. */
bool xd_nc_netmask_to_prefix(const char *mask, unsigned *prefix);
char *xd_nc_prefix_to_netmask(unsigned prefix);
bool xd_nc_valid_ipv4(const char *text);
/* The mask XP fills in for an address: its class's (8, 16 or 24), or 0. */
unsigned xd_nc_default_prefix(const char *ip);

/* The TCP/IP form, as XP checks it on OK: nullptr if it's fine, else
 * what's wrong. Empty strings are blank fields. */
typedef struct xd_nc_ipv4_form {
    bool manual_ip, manual_dns;
    const char *address, *netmask, *gateway, *dns1, *dns2;
} xd_nc_ipv4_form;
const char *xd_nc_check_ipv4_form(const xd_nc_ipv4_form *form);

/* The vpn.data key holding the server, for NetworkManager's VPN plugins
 * (by service type, "org.freedesktop.NetworkManager.openvpn"); nullptr if
 * we don't know the plugin. */
const char *xd_nc_vpn_host_key(const char *service_type);

/* The key management a Wi-Fi network needs, from its access point's
 * flags (NM80211ApFlags, NM80211ApSecurityFlags): nullptr for an open
 * network, "none" (WEP), "wpa-psk", "sae" or "wpa-eap". */
const char *xd_nc_wifi_key_mgmt(unsigned flags, unsigned wpa_flags, unsigned rsn_flags);
/* Whether `key` fits the key management: WEP keys are 5 or 13
 * characters or 10 or 26 hex digits; WPA and SAE passphrases 8-63
 * characters, or 64 hex digits for WPA. */
bool xd_nc_valid_wifi_key(const char *key_mgmt, const char *key);
/* XP's words for a signal strength (percent): "Excellent" ... "Very low". */
const char *xd_nc_signal_words(unsigned strength);

#endif
