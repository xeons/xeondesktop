"""NetworkManager for python-dbusmock with a scene for xd-netconn: the
stock networkmanager template plus a connected wired adapter (with IPv4
settings), one with no cable, a Wi-Fi adapter with two networks in range
and a saved one, and a VPN. It adds the calls libnm makes that the
template lacks (Update2, AddConnection2, Device.Disconnect).

    python3 -m dbusmock --system -t tests/dbusmock/netconn.py
"""

# SPDX-License-Identifier: MIT

import uuid

import dbus

import dbusmock
from dbusmock.templates import networkmanager as nm
from dbusmock.templates.networkmanager import *  # noqa: F401,F403 (the template's mock methods)
from dbusmock.templates.networkmanager import (
    CSETTINGS_IFACE,
    DEVICE_IFACE,
    MANAGER_IFACE,
    MANAGER_OBJ,
    SETTINGS_IFACE,
    SETTINGS_OBJ,
    DeviceState,
    NMActiveConnectionState,
)

BUS_NAME = nm.BUS_NAME
MAIN_OBJ = nm.MAIN_OBJ
SYSTEM_BUS = True
IS_OBJECT_MANAGER = True

IP4_IFACE = "org.freedesktop.NetworkManager.IP4Config"


def connection_update2(conn, settings):
    # NetworkManager replaces the settings (keeping secrets); the
    # template's Update merges them.
    conn.settings = settings
    conn.EmitSignal(CSETTINGS_IFACE, "Updated", "", [])


@dbus.service.method(SETTINGS_IFACE, in_signature="a{sa{sv}}", out_signature="o")
def SettingsAddConnection(self, connection_settings):
    path = nm.SettingsAddConnection(self, connection_settings)
    obj = dbusmock.get_object(path)
    obj.connection_update2 = connection_update2
    obj.AddMethod(CSETTINGS_IFACE, "Update2", "a{sa{sv}}ua{sv}", "a{sv}", "self.connection_update2(self, args[0]); ret = {}")
    return path


def device_disconnect(dev):
    main = dbusmock.get_object(MAIN_OBJ)
    path = dev.__dbus_object_path__
    active = dev.Get(DEVICE_IFACE, "ActiveConnection")
    if active != "/":
        main.RemoveActiveConnection(path, active)
    else:
        main.SetDeviceDisconnected(path)
    dev.Set(DEVICE_IFACE, "State", dbus.UInt32(DeviceState.DISCONNECTED))
    dev.Set(DEVICE_IFACE, "Autoconnect", False)


def add_connection(mock, settings):
    return dbus.ObjectPath(mock.SettingsAddConnection(dbus.Dictionary(settings, signature="sa{sv}")))


def connection(cid, ctype, extra, iface=None):
    c = {
        "connection": {
            "id": cid,
            "uuid": str(uuid.uuid4()),
            "type": ctype,
            "autoconnect": False,  # the template would activate it
            "timestamp": dbus.UInt64(1760000000),
        },
        "ipv4": {"method": "auto"},
        "ipv6": {"method": "auto"},
    }
    if iface is not None:
        c["connection"]["interface-name"] = iface
    c.update(extra)
    return c


def set_device(path, props):
    obj = dbusmock.get_object(path)
    for k, v in props.items():
        if k in obj.props.get(DEVICE_IFACE, {}):
            obj.Set(DEVICE_IFACE, k, v)
        else:
            obj.AddProperty(DEVICE_IFACE, k, v)
    obj.device_disconnect = device_disconnect
    obj.AddMethod(DEVICE_IFACE, "Disconnect", "", "", "self.device_disconnect(self)")


def load(mock, parameters):
    nm.load(mock, parameters)
    dbusmock.get_object(MANAGER_OBJ).Set(MANAGER_IFACE, "Version", "1.58.0")
    dbusmock.get_object(SETTINGS_OBJ).AddMethod(
        SETTINGS_IFACE, "AddConnection2", "a{sa{sv}}ua{sv}", "oa{sv}", "ret = (self.SettingsAddConnection(args[0]), {})"
    )

    # Wired, connected: 192.168.1.20/24 by DHCP, gateway .1. Its traffic
    # counters come from sysfs, so it borrows loopback's.
    eth0 = mock.AddEthernetDevice("mock_eth0", "eth0", DeviceState.ACTIVATED)
    dbusmock.get_object(eth0).Set("org.freedesktop.NetworkManager.Device.Wired", "Carrier", True)
    dbusmock.get_object(eth0).Set("org.freedesktop.NetworkManager.Device.Wired", "Speed", dbus.UInt32(1000))
    wired = add_connection(mock, connection("Local Area Connection", "802-3-ethernet", {"802-3-ethernet": {}}, "eth0"))
    ip4 = "/org/freedesktop/NetworkManager/IP4Config/1"
    mock.AddObject(
        ip4,
        IP4_IFACE,
        {
            "AddressData": dbus.Array(
                [dbus.Dictionary({"address": "192.168.1.20", "prefix": dbus.UInt32(24)}, signature="sv")],
                signature="a{sv}",
            ),
            "Gateway": "192.168.1.1",
            "NameserverData": dbus.Array(
                [dbus.Dictionary({"address": "192.168.1.1"}, signature="sv")], signature="a{sv}"
            ),
            "Domains": dbus.Array(["home.lan"], signature="s"),
            "Searches": dbus.Array([], signature="s"),
            "RouteData": dbus.Array([], signature="a{sv}"),
        },
        [],
    )
    mock.object_manager_emit_added(ip4)
    set_device(
        eth0,
        {
            "AvailableConnections": dbus.Array([wired], signature="o"),
            "Autoconnect": True,
            "Ip4Config": dbus.ObjectPath(ip4),
            "Ip4Connectivity": dbus.UInt32(4),
            "IpInterface": "lo",
        },
    )
    mock.AddActiveConnection([eth0], wired, "/", "1", dbus.UInt32(NMActiveConnectionState.NM_ACTIVE_CONNECTION_STATE_ACTIVATED))

    # Wired, no cable.
    eth1 = mock.AddEthernetDevice("mock_eth1", "eth1", DeviceState.UNAVAILABLE)
    set_device(eth1, {"Autoconnect": True})

    # Wi-Fi, not connected; two networks in range, one saved.
    wlan = mock.AddWiFiDevice("mock_wlan0", "wlan0", DeviceState.DISCONNECTED)
    mock.AddAccessPoint(wlan, "ap_home", "HomeNet", "00:11:22:33:44:55", 2, 2437, 54000, 82, 0x188)
    mock.AddAccessPoint(wlan, "ap_cafe", "CoffeeShop", "00:11:22:33:44:66", 2, 5180, 54000, 41, 0)
    home = add_connection(
        mock,
        connection(
            "HomeNet",
            "802-11-wireless",
            {
                "802-11-wireless": {"ssid": dbus.ByteArray(b"HomeNet"), "mode": "infrastructure"},
                "802-11-wireless-security": {"key-mgmt": "wpa-psk"},
            },
        ),
    )
    set_device(wlan, {"AvailableConnections": dbus.Array([home], signature="o"), "Autoconnect": True})

    # A VPN, not connected.
    add_connection(
        mock,
        connection(
            "Office VPN",
            "vpn",
            {
                "vpn": {
                    "service-type": "org.freedesktop.NetworkManager.openvpn",
                    "data": dbus.Dictionary({"remote": "vpn.example.com"}, signature="ss"),
                }
            },
        ),
    )
