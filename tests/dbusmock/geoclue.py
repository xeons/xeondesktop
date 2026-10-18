"""GeoClue 2 (org.freedesktop.GeoClue2) for python-dbusmock, with what
Display Properties' Night Light uses: Manager.GetClient, a client's
DesktopId and RequestedAccuracyLevel, Start and Stop, and the
LocationUpdated signal with a Location object. dbusmock has no GeoClue
template of its own.

    python3 -m dbusmock --system -t tests/dbusmock/geoclue.py \\
        -p '{"latitude": 51.5074, "longitude": -0.1278}'

With {"deny": true}, Start fails as GeoClue does for a program it hasn't
authorised. For Privacy's Location tab, the Manager's InUse and
AvailableAccuracyLevel come from the parameters of those names:

    python3 -m dbusmock --system -t tests/dbusmock/geoclue.py -p '{"InUse": true}'
"""

# SPDX-License-Identifier: MIT

import dbus
from gi.repository import GLib

from dbusmock import mockobject

BUS_NAME = "org.freedesktop.GeoClue2"
MAIN_OBJ = "/org/freedesktop/GeoClue2/Manager"
MAIN_IFACE = "org.freedesktop.GeoClue2.Manager"
CLIENT_IFACE = "org.freedesktop.GeoClue2.Client"
LOCATION_IFACE = "org.freedesktop.GeoClue2.Location"
SYSTEM_BUS = True


def load(mock, parameters):
    mock.latitude = float(parameters.get("latitude", 51.5074))
    mock.longitude = float(parameters.get("longitude", -0.1278))
    mock.deny = bool(parameters.get("deny", False))
    mock.next_client = 1
    mock.next_location = 1
    mock.start = lambda client: start(mock, client)
    mock.AddProperties(MAIN_IFACE, {
        "InUse": dbus.Boolean(parameters.get("InUse", False)),
        # GClueAccuracyLevel: 8 is EXACT.
        "AvailableAccuracyLevel": dbus.UInt32(parameters.get("AvailableAccuracyLevel", 8)),
    })


def start(mock, client):
    if mock.deny or client.Get(CLIENT_IFACE, "DesktopId") == "":
        raise dbus.exceptions.DBusException("'xd-desk' disallowed, no agent for UID 1000",
                                            name="org.freedesktop.DBus.Error.AccessDenied")
    client.UpdateProperties(CLIENT_IFACE, {"Active": dbus.Boolean(True)})

    def fix():
        old = client.Get(CLIENT_IFACE, "Location")
        path = f"/org/freedesktop/GeoClue2/Location/{mock.next_location}"
        mock.next_location += 1
        mock.AddObject(path, LOCATION_IFACE, {
            "Latitude": dbus.Double(mock.latitude),
            "Longitude": dbus.Double(mock.longitude),
            "Accuracy": dbus.Double(5000.0),
            "Altitude": dbus.Double(-1.7976931348623157e308),
            "Speed": dbus.Double(-1.0),
            "Heading": dbus.Double(-1.0),
            "Description": "Mock location",
            "Timestamp": dbus.Struct((dbus.UInt64(0), dbus.UInt64(0)), signature="tt"),
        }, [])
        client.Set(CLIENT_IFACE, "Location", dbus.ObjectPath(path))
        client.EmitSignal(CLIENT_IFACE, "LocationUpdated", "oo",
                          [dbus.ObjectPath(old), dbus.ObjectPath(path)])
        return False

    # A moment later, as a real fix takes.
    GLib.timeout_add(300, fix)


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="o")
def GetClient(self):
    path = f"/org/freedesktop/GeoClue2/Client/{self.next_client}"
    self.next_client += 1
    self.AddObject(path, CLIENT_IFACE, {
        "Location": dbus.ObjectPath("/"),
        "DistanceThreshold": dbus.UInt32(0),
        "TimeThreshold": dbus.UInt32(0),
        "DesktopId": "",
        "RequestedAccuracyLevel": dbus.UInt32(0),
        "Active": False,
    }, [
        ("Start", "", "", "objects['%s'].start(self)" % MAIN_OBJ),
        ("Stop", "", "",
         "self.UpdateProperties('%s', {'Active': dbus.Boolean(False)})" % CLIENT_IFACE),
    ])
    return dbus.ObjectPath(path)


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="o")
def CreateClient(self):
    return GetClient(self)


@dbus.service.method(MAIN_IFACE, in_signature="o", out_signature="")
def DeleteClient(self, path):
    if path in mockobject.objects:
        self.RemoveObject(path)


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="")
def AddAgent(self, desktop_id):
    pass
