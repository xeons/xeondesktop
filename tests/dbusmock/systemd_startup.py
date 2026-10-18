"""The user's systemd (org.freedesktop.systemd1 on the session bus) for
python-dbusmock, as the System Configuration Utility (xd-msconfig) sees
it: unit files, their state and install links, enabling and disabling.
Nothing is linked on disk and nothing ever runs. (systemd_user.py is the
one for Scheduled Tasks' timers.)

    python3 -m dbusmock -t tests/dbusmock/systemd_startup.py

Parameters: "units", {name: {"Description", "WantedBy", "enabled",
"global", "active"}} (by default: a sync client you enabled, PipeWire
enabled for every user, a podman service that's off, a timer's service);
"user_unit_dir", where your links are said to be
($XDG_CONFIG_HOME/systemd/user).
"""

# SPDX-License-Identifier: MIT

import fnmatch
import os

import dbus

BUS_NAME = "org.freedesktop.systemd1"
MAIN_OBJ = "/org/freedesktop/systemd1"
MAIN_IFACE = "org.freedesktop.systemd1.Manager"
UNIT_IFACE = "org.freedesktop.systemd1.Unit"
SYSTEM_BUS = False

DEFAULT_UNITS = {
    "syncthing.service": {"Description": "Syncthing - Open Source Continuous File Synchronization",
                          "WantedBy": "default.target", "enabled": True, "active": True},
    "pipewire.service": {"Description": "PipeWire Multimedia Service",
                         "WantedBy": "default.target", "global": True, "active": True},
    "podman-auto-update.service": {"Description": "Podman auto-update service",
                                   "WantedBy": "default.target"},
    "backup.service": {"Description": "Nightly backup", "WantedBy": "timers.target",
                       "enabled": True},
}  # fmt: skip


def unit_path(name):
    escaped = "".join(c if c.isalnum() and c.isascii() else "_%02x" % ord(c) for c in name)
    return f"{MAIN_OBJ}/unit/{escaped}"


def load(mock, parameters):
    mock.unit_dir = parameters.get(
        "user_unit_dir",
        os.path.join(
            os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "systemd", "user"
        ),
    )
    mock.unit_files = {}
    mock.enabled = set()
    for name, u in parameters.get("units", DEFAULT_UNITS).items():
        mock.unit_files[name] = dict(u)
        if u.get("enabled"):
            mock.enabled.add(name)
        mock.AddObject(
            unit_path(name),
            UNIT_IFACE,
            {
                "Id": name,
                "Description": u.get("Description", name),
                "LoadState": "loaded",
                "ActiveState": "active" if u.get("active") else "inactive",
                "SubState": "running" if u.get("active") else "dead",
            },
            [],
        )
    mock.AddProperties(MAIN_IFACE, {"Version": "257-mock"})


def state(mock, name):
    u = mock.unit_files[name]
    return "enabled" if u.get("global") or name in mock.enabled else "disabled"


def links(mock, name):
    u = mock.unit_files[name]
    wants = u.get("WantedBy", "default.target") + ".wants"
    out = []
    if u.get("global"):
        out.append(f"/etc/systemd/user/{wants}/{name}")
    if name in mock.enabled:
        out.append(f"{mock.unit_dir}/{wants}/{name}")
    return out


def known(mock, name):
    if name not in mock.unit_files:
        raise dbus.exceptions.DBusException(
            f"Unit file {name} does not exist.", name="org.freedesktop.DBus.Error.FileNotFound"
        )


@dbus.service.method(MAIN_IFACE, in_signature="asas", out_signature="a(ss)")
def ListUnitFilesByPatterns(self, states, patterns):
    out = []
    for name in sorted(self.unit_files):
        if states and state(self, name) not in states:
            continue
        if patterns and not any(fnmatch.fnmatch(name, p) for p in patterns):
            continue
        out.append((f"/usr/lib/systemd/user/{name}", state(self, name)))
    return dbus.Array(out, signature="(ss)")


@dbus.service.method(MAIN_IFACE, in_signature="sb", out_signature="as")
def GetUnitFileLinks(self, name, _runtime):
    known(self, str(name))
    return dbus.Array(links(self, str(name)), signature="s")


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def LoadUnit(self, name):
    known(self, str(name))
    return dbus.ObjectPath(unit_path(str(name)))


@dbus.service.method(MAIN_IFACE, in_signature="asbb", out_signature="ba(sss)")
def EnableUnitFiles(self, files, _runtime, _force):
    changes = []
    for f in files:
        name = os.path.basename(str(f))
        known(self, name)
        if name not in self.enabled and not self.unit_files[name].get("global"):
            self.enabled.add(name)
            changes.append(("symlink", links(self, name)[-1], f"/usr/lib/systemd/user/{name}"))
    return (True, dbus.Array(changes, signature="(sss)"))


@dbus.service.method(MAIN_IFACE, in_signature="asb", out_signature="a(sss)")
def DisableUnitFiles(self, files, _runtime):
    changes = []
    for f in files:
        name = os.path.basename(str(f))
        known(self, name)
        if name in self.enabled:
            link = links(self, name)[-1]
            self.enabled.discard(name)
            changes.append(("unlink", link, ""))
    return dbus.Array(changes, signature="(sss)")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="")
def Reload(self):
    pass

