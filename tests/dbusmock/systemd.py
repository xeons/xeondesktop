"""systemd's manager (org.freedesktop.systemd1) for python-dbusmock, with
what xd-services uses: listing units and unit files, loading units with
their Unit and Service properties, start/stop/restart jobs (JobRemoved),
enabling, disabling and masking. dbusmock's own systemd template has
too little of it.

    python3 -m dbusmock --system -t tests/dbusmock/systemd.py
    python3 -m dbusmock --session -t tests/dbusmock/systemd.py -p '{"user": true}'

A unit named "broken.service" fails to start (its job ends "failed").
With -p '{"display_manager": "sddm.service"}', that unit runs and
display-manager.service is its alias.
"""

# SPDX-License-Identifier: MIT

import dbus
from gi.repository import GLib

from dbusmock import MOCK_IFACE, mockobject

BUS_NAME = "org.freedesktop.systemd1"
MAIN_OBJ = "/org/freedesktop/systemd1"
MAIN_IFACE = "org.freedesktop.systemd1.Manager"
UNIT_IFACE = "org.freedesktop.systemd1.Unit"
SERVICE_IFACE = "org.freedesktop.systemd1.Service"
SYSTEM_BUS = True

# id: (description, active, unit file state, loaded, User=, ExecStart argv)
SYSTEM_UNITS = {
    "sshd.service": ("OpenSSH Daemon", "active", "enabled", True, "", ["/usr/bin/sshd", "-D"]),
    "cups.service": ("CUPS Scheduler", "inactive", "disabled", False, "", ["/usr/bin/cupsd", "-l"]),
    "systemd-journald.service": ("Journal Service", "active", "static", True, "",
                                 ["/usr/lib/systemd/systemd-journald"]),
    "nginx.service": ("A high performance web server", "inactive", "masked", False, "http",
                      ["/usr/bin/nginx", "-g", "pid /run/nginx.pid; daemon on;"]),
    "broken.service": ("Always Fails", "failed", "enabled", True, "nobody", ["/usr/bin/false"]),
    "getty@tty1.service": ("Getty on tty1", "active", "enabled", True, "",
                           ["/sbin/agetty", "-o", "-- \\u", "--noreset", "-", "$TERM"]),
}
USER_UNITS = {
    "pipewire.service": ("PipeWire Multimedia Service", "active", "enabled", True, "",
                         ["/usr/bin/pipewire"]),
    "syncthing.service": ("Syncthing - Open Source Continuous File Synchronization", "inactive",
                          "disabled", False, "", ["/usr/bin/syncthing", "serve"]),
}


def escape(name):
    out = ""
    for c in name:
        out += c if c.isalnum() else "_%02x" % ord(c)
    return out


def unit_path(name):
    return f"{MAIN_OBJ}/unit/{escape(name)}"


def load(mock, parameters):
    mock.next_job = 1
    mock.units = {}  # id -> dict
    units = dict(USER_UNITS if parameters.get("user", False) else SYSTEM_UNITS)
    # "display_manager": "sddm.service" runs that, as display-manager.service.
    mock.aliases = {}
    dm = parameters.get("display_manager")
    if dm:
        units[dm] = ("Display Manager", "active", "enabled", True, "", [f"/usr/bin/{dm[:-8]}"])
        mock.aliases["display-manager.service"] = dm
    for uid, (desc, active, state, loaded, user, argv) in units.items():
        mock.units[uid] = {
            "desc": desc, "active": active, "state": state, "loaded": loaded,
            "user": user, "argv": argv,
        }
        if loaded:
            add_unit_object(mock, uid)
    mock.AddProperties(MAIN_IFACE, {"Version": "258"})


def add_unit_object(mock, uid):
    u = mock.units[uid]
    path = unit_path(uid)
    if path in mockobject.objects:
        return path
    u["loaded"] = True
    mock.AddObject(path, UNIT_IFACE, {
        "Id": uid,
        "Names": dbus.Array([uid], signature="s"),
        "Description": u["desc"],
        "LoadState": "loaded",
        "ActiveState": u["active"],
        "SubState": "running" if u["active"] == "active" else "dead",
        "UnitFileState": u["state"],
        "FragmentPath": f"/usr/lib/systemd/system/{uid}",
        "Requires": dbus.Array(["system.slice", "sysinit.target"], signature="s"),
        "Wants": dbus.Array(["network-online.target"], signature="s"),
        "After": dbus.Array(["network.target", "basic.target"], signature="s"),
        "Before": dbus.Array(["shutdown.target"], signature="s"),
        "RequiredBy": dbus.Array([], signature="s"),
        "WantedBy": dbus.Array(["multi-user.target"], signature="s"),
    }, [])
    exec_start = dbus.Array([dbus.Struct(
        (u["argv"][0], dbus.Array(u["argv"], signature="s"), False,
         dbus.UInt64(0), dbus.UInt64(0), dbus.UInt64(0), dbus.UInt64(0),
         dbus.UInt32(0), dbus.Int32(0), dbus.Int32(0)),
        signature="sasbttttuii")], signature="(sasbttttuii)")
    mockobject.objects[path].AddProperties(SERVICE_IFACE, {
        "User": u["user"],
        "ExecStart": exec_start,
        "MainPID": dbus.UInt32(1234 if u["active"] == "active" else 0),
    })
    return path


def set_active(mock, uid, active):
    mock.units[uid]["active"] = active
    path = add_unit_object(mock, uid)
    mockobject.objects[path].UpdateProperties(UNIT_IFACE, {"ActiveState": active})


def unit(mock, name):
    name = mock.aliases.get(str(name), str(name))
    if name not in mock.units:
        raise dbus.exceptions.DBusException(
            f"Unit {name} not found.", name="org.freedesktop.systemd1.NoSuchUnit")
    return name


def job(mock, name, after):
    jid = mock.next_job
    mock.next_job += 1
    path = f"{MAIN_OBJ}/job/{jid}"

    def finish():
        result = after()
        mock.EmitSignal(MAIN_IFACE, "JobRemoved", "uoss", [dbus.UInt32(jid), path, name, result])
        return False

    mock.EmitSignal(MAIN_IFACE, "JobNew", "uos", [dbus.UInt32(jid), path, name])
    GLib.timeout_add(300, finish)
    return dbus.ObjectPath(path)


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="a(ssssssouso)")
def ListUnits(self):
    out = []
    for uid, u in self.units.items():
        if not u["loaded"]:
            continue
        out.append((uid, u["desc"], "loaded", u["active"],
                    "running" if u["active"] == "active" else "dead", "",
                    dbus.ObjectPath(unit_path(uid)), dbus.UInt32(0), "", dbus.ObjectPath("/")))
    return dbus.Array(out, signature="(ssssssouso)")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="a(ss)")
def ListUnitFiles(self):
    out = [(f"/usr/lib/systemd/system/{uid}", u["state"]) for uid, u in self.units.items()
           if "@" not in uid]
    out.append(("/usr/lib/systemd/system/getty@.service", "enabled"))
    return dbus.Array(out, signature="(ss)")


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def LoadUnit(self, name):
    return dbus.ObjectPath(add_unit_object(self, unit(self, name)))


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def GetUnit(self, name):
    uid = unit(self, name)
    if not self.units[uid]["loaded"]:
        raise dbus.exceptions.DBusException(
            f"Unit {uid} not loaded.", name="org.freedesktop.systemd1.NoSuchUnit")
    return dbus.ObjectPath(unit_path(uid))


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="o")
def StartUnit(self, name, _mode):
    uid = unit(self, name)
    if self.units[uid]["state"] == "masked":
        raise dbus.exceptions.DBusException(
            f"Unit {uid} is masked.", name="org.freedesktop.systemd1.UnitMasked")
    set_active(self, uid, "activating")

    def after():
        ok = uid != "broken.service"
        set_active(self, uid, "active" if ok else "failed")
        return "done" if ok else "failed"

    return job(self, uid, after)


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="o")
def StopUnit(self, name, _mode):
    uid = unit(self, name)
    set_active(self, uid, "deactivating")

    def after():
        set_active(self, uid, "inactive")
        return "done"

    return job(self, uid, after)


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="o")
def RestartUnit(self, name, mode):
    return StartUnit(self, name, mode)


def change_state(mock, names, state, kind):
    changes = []
    for n in names:
        uid = unit(mock, n)
        mock.units[uid]["state"] = state
        path = unit_path(uid)
        if path in mockobject.objects:
            mockobject.objects[path].UpdateProperties(UNIT_IFACE, {"UnitFileState": state})
        changes.append((kind, f"/etc/systemd/system/multi-user.target.wants/{uid}",
                        f"/usr/lib/systemd/system/{uid}"))
    mock.EmitSignal(MAIN_IFACE, "UnitFilesChanged", "", [])
    return dbus.Array(changes, signature="(sss)")


@dbus.service.method(MAIN_IFACE, in_signature="asbb", out_signature="ba(sss)")
def EnableUnitFiles(self, names, _runtime, _force):
    return (True, change_state(self, names, "enabled", "symlink"))


@dbus.service.method(MAIN_IFACE, in_signature="asb", out_signature="a(sss)")
def DisableUnitFiles(self, names, _runtime):
    return change_state(self, names, "disabled", "unlink")


@dbus.service.method(MAIN_IFACE, in_signature="asbb", out_signature="a(sss)")
def MaskUnitFiles(self, names, _runtime, _force):
    return change_state(self, names, "masked", "symlink")


@dbus.service.method(MAIN_IFACE, in_signature="asb", out_signature="a(sss)")
def UnmaskUnitFiles(self, names, _runtime):
    return change_state(self, names, "disabled", "unlink")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="")
def Reload(self):
    pass


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="")
def Subscribe(self):
    pass


@dbus.service.method(MOCK_IFACE, in_signature="ss", out_signature="")
def SetUnitActive(self, name, active):
    """Changes a unit's state as if from outside."""
    set_active(self, unit(self, name), str(active))
