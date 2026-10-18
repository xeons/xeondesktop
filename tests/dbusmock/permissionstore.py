"""xdg-desktop-portal's permission store (org.freedesktop.impl.portal.
PermissionStore, on the session bus) for python-dbusmock, which has no
template of its own: Lookup, List, Set, SetPermission, Delete and
DeletePermission on tables kept in memory, with the Changed signal.

    python3 -m dbusmock -t tests/dbusmock/permissionstore.py \
        -p '{"tables": {"devices": {"camera": {"org.example.Cam": ["yes"]}}}}'

Parameters: "tables", {table: {id: {app: [values]}}}.
"""

# SPDX-License-Identifier: MIT

import dbus
import dbus.service

BUS_NAME = "org.freedesktop.impl.portal.PermissionStore"
MAIN_OBJ = "/org/freedesktop/impl/portal/PermissionStore"
MAIN_IFACE = "org.freedesktop.impl.portal.PermissionStore"
SYSTEM_BUS = False

NOT_FOUND = "org.freedesktop.portal.Error.NotFound"


def load(mock, parameters):
    # table -> id -> {app: [values]}; no per-id data is kept.
    mock.tables = {
        table: {i: {app: list(values) for app, values in apps.items()} for i, apps in ids.items()}
        for table, ids in parameters.get("tables", {}).items()
    }
    mock.AddProperties(MAIN_IFACE, {"version": dbus.UInt32(2)})


def _perms(apps):
    return dbus.Dictionary(
        {app: dbus.Array(values, signature="s") for app, values in apps.items()}, signature="sas"
    )


def _changed(self, table, entry, deleted):
    apps = self.tables.get(table, {}).get(entry, {})
    self.EmitSignal(
        MAIN_IFACE,
        "Changed",
        "ssbva{sas}",
        [table, entry, deleted, dbus.Byte(0, variant_level=1), _perms(apps)],
    )


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="a{sas}v")
def Lookup(self, table, entry):
    apps = self.tables.get(table, {}).get(entry)
    if apps is None:
        raise dbus.exceptions.DBusException(f"No entry for {entry}", name=NOT_FOUND)
    return _perms(apps), dbus.Byte(0, variant_level=1)


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="as")
def List(self, table):
    return dbus.Array(sorted(self.tables.get(table, {})), signature="s")


@dbus.service.method(MAIN_IFACE, in_signature="sbsa{sas}v", out_signature="")
def Set(self, table, create, entry, app_permissions, _data):
    if table not in self.tables and not create:
        raise dbus.exceptions.DBusException(f"No table {table}", name=NOT_FOUND)
    self.tables.setdefault(table, {})[entry] = {
        str(app): [str(v) for v in values] for app, values in app_permissions.items()
    }
    _changed(self, table, entry, False)


@dbus.service.method(MAIN_IFACE, in_signature="sbssas", out_signature="")
def SetPermission(self, table, create, entry, app, permissions):
    if table not in self.tables and not create:
        raise dbus.exceptions.DBusException(f"No table {table}", name=NOT_FOUND)
    self.tables.setdefault(table, {}).setdefault(entry, {})[str(app)] = [
        str(p) for p in permissions
    ]
    _changed(self, table, entry, False)


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="")
def Delete(self, table, entry):
    if entry not in self.tables.get(table, {}):
        raise dbus.exceptions.DBusException(f"No entry for {entry}", name=NOT_FOUND)
    del self.tables[table][entry]
    _changed(self, table, entry, True)


@dbus.service.method(MAIN_IFACE, in_signature="sss", out_signature="")
def DeletePermission(self, table, entry, app):
    apps = self.tables.get(table, {}).get(entry)
    if apps is None or app not in apps:
        raise dbus.exceptions.DBusException(f"No permission for {app}", name=NOT_FOUND)
    del apps[app]
    _changed(self, table, entry, False)
