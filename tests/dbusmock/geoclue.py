"""GeoClue 2 (org.freedesktop.GeoClue2, on the system bus) for
python-dbusmock, which has no template of its own: the Manager's
properties xd-privacy shows (InUse, AvailableAccuracyLevel) and its
methods, which do nothing here.

    python3 -m dbusmock --system -t tests/dbusmock/geoclue.py -p '{"InUse": true}'
"""

# SPDX-License-Identifier: MIT

import dbus

BUS_NAME = "org.freedesktop.GeoClue2"
MAIN_OBJ = "/org/freedesktop/GeoClue2/Manager"
MAIN_IFACE = "org.freedesktop.GeoClue2.Manager"
SYSTEM_BUS = True

# GClueAccuracyLevel: EXACT.
ACCURACY_EXACT = 8


def load(mock, parameters):
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "InUse": dbus.Boolean(parameters.get("InUse", False)),
                "AvailableAccuracyLevel": dbus.UInt32(
                    parameters.get("AvailableAccuracyLevel", ACCURACY_EXACT)
                ),
            },
            signature="sv",
        ),
    )
    mock.AddMethods(
        MAIN_IFACE,
        [
            ("GetClient", "", "o", 'ret = dbus.ObjectPath("/org/freedesktop/GeoClue2/Client/1")'),
            ("CreateClient", "", "o", 'ret = dbus.ObjectPath("/org/freedesktop/GeoClue2/Client/1")'),
            ("DeleteClient", "o", "", ""),
            ("AddAgent", "s", "", ""),
        ],
    )
