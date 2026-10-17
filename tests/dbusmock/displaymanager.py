"""A display manager's org.freedesktop.DisplayManager (LightDM's and
SDDM's interface) for python-dbusmock: one seat that can switch to the
greeter, for the Start menu's Switch User. Calls are only logged.

    python3 -m dbusmock --system -t tests/dbusmock/displaymanager.py -l calls.log

Run the session with XDG_SEAT_PATH=/org/freedesktop/DisplayManager/Seat0,
or without it to have the shell look the seat up.
"""

# SPDX-License-Identifier: MIT

import dbus

BUS_NAME = "org.freedesktop.DisplayManager"
MAIN_OBJ = "/org/freedesktop/DisplayManager"
MAIN_IFACE = "org.freedesktop.DisplayManager"
SEAT_IFACE = "org.freedesktop.DisplayManager.Seat"
SEAT_PATH = "/org/freedesktop/DisplayManager/Seat0"
SYSTEM_BUS = True


def load(mock, _parameters):
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {"Seats": dbus.Array([dbus.ObjectPath(SEAT_PATH)], signature="o"),
             "Sessions": dbus.Array([], signature="o")},
            signature="sv",
        ),
    )
    mock.AddObject(
        SEAT_PATH,
        SEAT_IFACE,
        {"CanSwitch": True, "HasGuestAccount": False,
         "Sessions": dbus.Array([], signature="o")},
        [
            ("SwitchToGreeter", "", "", ""),
            ("SwitchToUser", "ss", "", ""),
            ("Lock", "", "", ""),
        ],
    )
