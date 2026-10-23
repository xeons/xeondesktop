"""Snapper (org.opensuse.Snapper) for python-dbusmock, which has no
template of its own: the configs and snapshots Previous Versions in
xd-files asks for (ListConfigs, ListSnapshots). Nothing here touches a
real snapshot.

    python3 -m dbusmock --system -t tests/dbusmock/snapper.py \\
        -p '{"configs": {"home": "/scratch/vol"},
             "snapshots": "{\\"home\\": [[1, 1700000000, \\"timeline\\"]]}"}'

Parameters: "configs", each config's name and SUBVOLUME; "snapshots",
as JSON (D-Bus can't carry its mixed lists), each config's snapshots as
[number, date (seconds since the epoch), description], with snapshot 0
(the file system as it is now) first as Snapper's lists have it;
"denied", configs whose ListSnapshots is refused, as for a user that
Snapper's ALLOW_USERS doesn't name.
"""

# SPDX-License-Identifier: MIT

import json

import dbus

BUS_NAME = "org.opensuse.Snapper"
MAIN_OBJ = "/org/opensuse/Snapper"
MAIN_IFACE = "org.opensuse.Snapper"
SYSTEM_BUS = True

# (uquxussa{ss}): number, type (0 single, 1 pre, 2 post), pre number,
# date, uid, description, cleanup, user data.
SNAPSHOT = "(uquxussa{ss})"


def load(mock, parameters):
    mock.snapper_configs = dict(parameters.get("configs", {}))
    mock.snapper_snapshots = json.loads(parameters.get("snapshots", "{}"))
    mock.snapper_denied = list(parameters.get("denied", []))
    mock.AddMethods(MAIN_IFACE, [
        ("ListConfigs", "", "a(ssa{ss})", "ret = self.snapper_list_configs()"),
        ("ListSnapshots", "s", "a" + SNAPSHOT, "ret = self.snapper_list_snapshots(args[0])"),
    ])  # fmt: skip
    mock.snapper_list_configs = lambda: list_configs(mock)
    mock.snapper_list_snapshots = lambda name: list_snapshots(mock, name)


def list_configs(mock):
    return dbus.Array(
        [
            dbus.Struct((name, subvolume, dbus.Dictionary({"SUBVOLUME": subvolume, "FSTYPE": "btrfs"},
                                                          signature="ss")),
                        signature="ssa{ss}")
            for name, subvolume in mock.snapper_configs.items()
        ],
        signature="(ssa{ss})",
    )  # fmt: skip


def list_snapshots(mock, name):
    if name not in mock.snapper_configs:
        raise dbus.exceptions.DBusException("unknown config", name="error.unknown_config")
    if name in mock.snapper_denied:
        raise dbus.exceptions.DBusException("no permissions", name="error.no_permissions")
    current = dbus.Struct((dbus.UInt32(0), dbus.UInt16(0), dbus.UInt32(0), dbus.Int64(0),
                           dbus.UInt32(0), "current", "", dbus.Dictionary({}, signature="ss")),
                          signature=SNAPSHOT[1:-1])  # fmt: skip
    snapshots = [current]
    for number, date, description in mock.snapper_snapshots.get(name, []):
        snapshots.append(dbus.Struct(
            (dbus.UInt32(number), dbus.UInt16(0), dbus.UInt32(0), dbus.Int64(date),
             dbus.UInt32(0), description, "timeline", dbus.Dictionary({}, signature="ss")),
            signature=SNAPSHOT[1:-1]))  # fmt: skip
    return dbus.Array(snapshots, signature=SNAPSHOT)
