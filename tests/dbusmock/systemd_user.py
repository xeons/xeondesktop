"""The user's systemd (org.freedesktop.systemd1 on the session bus) for
python-dbusmock, whose systemd template is the system manager's and has
none of this: what xd-schedtasks uses. Units come from the unit folder
($XDG_CONFIG_HOME/systemd/user, or the unit_dir parameter) at each
Reload; calendar expressions are normalised, and their next elapse
worked out, by `systemd-analyze calendar` (which only parses). Nothing is
ever run: starting a service marks it running for a few seconds, then it
"exits" with the exit_status parameter (0).

    python3 -m dbusmock -t tests/dbusmock/systemd_user.py

Other programs' timers: the others parameter, {"name.timer": "calendar"}
(by default one, mail-sync.timer). The mock interface's Trigger(timer)
elapses a timer now.
"""

# SPDX-License-Identifier: MIT

import configparser
import datetime
import fnmatch
import os
import shlex
import subprocess
import time

import dbus
from gi.repository import GLib

from dbusmock import MOCK_IFACE, mockobject

BUS_NAME = "org.freedesktop.systemd1"
MAIN_OBJ = "/org/freedesktop/systemd1"
MAIN_IFACE = "org.freedesktop.systemd1.Manager"
UNIT_IFACE = "org.freedesktop.systemd1.Unit"
TIMER_IFACE = "org.freedesktop.systemd1.Timer"
SERVICE_IFACE = "org.freedesktop.systemd1.Service"
SYSTEM_BUS = False

INFINITY = dbus.UInt64(2**64 - 1)
RUN_SECONDS = 2


def unit_path(name):
    # systemd's bus path escaping: anything but [A-Za-z0-9] as _xx.
    escaped = "".join(c if c.isalnum() and c.isascii() else "_%02x" % ord(c) for c in name)
    return f"{MAIN_OBJ}/unit/{escaped}"


def now_usec():
    return dbus.UInt64(int(time.time() * 1_000_000))


def calendar(spec):
    """(normalised form, next elapse in realtime usec or 0) from systemd."""
    try:
        out = subprocess.run(
            ["systemd-analyze", "calendar", spec], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return spec, 0
    normal, nxt = spec, 0
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("Normalized form:"):
            normal = line.split(":", 1)[1].strip()
        elif line.startswith("(in UTC):"):
            stamp = line.split(":", 1)[1].strip()
            when = datetime.datetime.strptime(stamp, "%a %Y-%m-%d %H:%M:%S UTC")
            nxt = int(when.replace(tzinfo=datetime.timezone.utc).timestamp() * 1_000_000)
    return normal, nxt


def unspecify(value):
    # %% is a percent sign; we write no other specifiers.
    return value.replace("%%", "%")


def read_unit(path):
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.optionxform = str
    parser.read(path, encoding="utf-8")
    return parser


def load(mock, parameters):
    mock.unit_dir = parameters.get(
        "unit_dir",
        os.path.join(
            os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "systemd", "user"
        ),
    )
    mock.exit_status = int(parameters.get("exit_status", 0))
    mock.units = {}
    mock.enabled = set()
    mock.next_job = 1
    mock.AddProperties(MAIN_IFACE, {"Version": "257-mock"})
    for name, spec in parameters.get("others", {"mail-sync.timer": "*-*-* *:00/15:00"}).items():
        add_timer(mock, name, f"Sync mail ({name})", calendar_spec=spec, file_state="static")
        add_service(mock, name[: -len(".timer")] + ".service", "Sync mail", ["/usr/bin/true"])
    reload_units(mock)


def set_props(obj, iface, props):
    """Adds the properties that are new, updates the others."""
    have = obj.props.get(iface, {})
    new = {k: v for k, v in props.items() if k not in have}
    old = {k: v for k, v in props.items() if k in have}
    if new:
        obj.AddProperties(iface, new)
    if old:
        obj.UpdateProperties(iface, old)


def unit_object(mock, name):
    path = unit_path(name)
    if path not in mockobject.objects:
        mock.AddObject(
            path,
            UNIT_IFACE,
            {
                "Id": name,
                "Names": [name],
                "Description": name,
                "LoadState": "loaded",
                "ActiveState": "inactive",
                "SubState": "dead",
                "UnitFileState": "disabled",
                "FragmentPath": "",
            },
            [],
        )
    mock.units[name] = path
    return mockobject.objects[path]


def add_timer(mock, name, description, calendar_spec=None, mono=None, file_state=None):
    obj = unit_object(mock, name)
    cal, nxt = [], 0
    if calendar_spec is not None:
        normal, nxt = calendar(calendar_spec)
        cal = [("OnCalendar", normal, dbus.UInt64(nxt))]
    obj.UpdateProperties(
        UNIT_IFACE,
        {
            "Description": description,
            "LoadState": "loaded",
            "UnitFileState": file_state
            or ("enabled" if name in mock.enabled else "disabled"),
        },
    )
    active = obj.Get(UNIT_IFACE, "ActiveState") == "active"
    set_props(
        obj,
        TIMER_IFACE,
        {
            "Unit": name[: -len(".timer")] + ".service",
            "TimersCalendar": dbus.Array(cal, signature="(sst)"),
            "TimersMonotonic": dbus.Array(mono or [], signature="(stt)"),
            "NextElapseUSecRealtime": dbus.UInt64(nxt if active or file_state else 0),
            "NextElapseUSecMonotonic": dbus.UInt64(0),
            "LastTriggerUSec": obj.props.get(TIMER_IFACE, {}).get(
                "LastTriggerUSec", dbus.UInt64(0)
            ),
        },
    )
    if file_state == "static":
        obj.UpdateProperties(UNIT_IFACE, {"ActiveState": "active", "SubState": "waiting"})


def add_service(mock, name, description, argv, workdir="", max_usec=INFINITY):
    obj = unit_object(mock, name)
    obj.UpdateProperties(UNIT_IFACE, {"Description": description, "LoadState": "loaded"})
    old = obj.props.get(SERVICE_IFACE, {})
    set_props(
        obj,
        SERVICE_IFACE,
        {
            "ExecStart": dbus.Array(
                [(argv[0], dbus.Array(argv, signature="s"), False, 0, 0, 0, 0, 0, 0, 0)],
                signature="(sasbttttuii)",
            ),
            "WorkingDirectory": workdir,
            "RuntimeMaxUSec": dbus.UInt64(max_usec),
            "ExecMainStatus": old.get("ExecMainStatus", dbus.Int32(0)),
            "ExecMainCode": old.get("ExecMainCode", dbus.Int32(0)),
            "ExecMainStartTimestamp": old.get("ExecMainStartTimestamp", dbus.UInt64(0)),
            "ExecMainExitTimestamp": old.get("ExecMainExitTimestamp", dbus.UInt64(0)),
            "Result": old.get("Result", "success"),
        },
    )


MONOTONIC = {"OnStartupSec": "OnStartupUSec", "OnActiveSec": "OnActiveUSec",
             "OnBootSec": "OnBootUSec"}


def reload_units(mock):
    files = set(os.listdir(mock.unit_dir)) if os.path.isdir(mock.unit_dir) else set()
    for name in list(mock.units):
        if name.startswith("xd-task-") and name not in files:
            mock.RemoveObject(mock.units.pop(name))
    for f in sorted(files):
        path = os.path.join(mock.unit_dir, f)
        unit = read_unit(path)
        desc = unspecify(unit.get("Unit", "Description", fallback=f))
        if f.endswith(".timer"):
            spec = unit.get("Timer", "OnCalendar", fallback=None)
            mono = [
                (MONOTONIC[k], dbus.UInt64(int(v) * 1_000_000), dbus.UInt64(0))
                for k, v in (unit["Timer"].items() if unit.has_section("Timer") else [])
                if k in MONOTONIC
            ]
            add_timer(mock, f, desc, calendar_spec=spec, mono=mono)
        elif f.endswith(".service"):
            exec_start = unit.get("Service", "ExecStart", fallback="")
            argv = [unspecify(w).replace("$$", "$") for w in shlex.split(exec_start)]
            runtime = unit.get("Service", "RuntimeMaxSec", fallback=None)
            add_service(
                mock,
                f,
                desc,
                argv or [""],
                workdir=unspecify(unit.get("Service", "WorkingDirectory", fallback="")),
                max_usec=int(runtime) * 1_000_000 if runtime else INFINITY,
            )


def job(mock, name):
    job_id = mock.next_job
    mock.next_job += 1
    path = f"{MAIN_OBJ}/job/{job_id}"
    GLib.idle_add(
        lambda: mock.EmitSignal(MAIN_IFACE, "JobRemoved", "uoss", [job_id, path, name, "done"])
    )
    return dbus.ObjectPath(path)


def unit(mock, name):
    if str(name) not in mock.units:
        raise dbus.exceptions.DBusException(
            f"Unit {name} not loaded.", name="org.freedesktop.systemd1.NoSuchUnit"
        )
    return mockobject.objects[mock.units[str(name)]]


def finish_service(mock, name):
    if str(name) not in mock.units:
        return False
    obj = unit(mock, name)
    if obj.Get(UNIT_IFACE, "ActiveState") == "active":
        obj.UpdateProperties(UNIT_IFACE, {"ActiveState": "inactive", "SubState": "dead"})
        obj.UpdateProperties(
            SERVICE_IFACE,
            {
                "ExecMainExitTimestamp": now_usec(),
                "ExecMainCode": dbus.Int32(1),  # CLD_EXITED
                "ExecMainStatus": dbus.Int32(mock.exit_status),
                "Result": "success" if mock.exit_status == 0 else "exit-code",
            },
        )
    return False


def start(mock, name):
    obj = unit(mock, name)
    if name.endswith(".service"):
        obj.UpdateProperties(UNIT_IFACE, {"ActiveState": "active", "SubState": "running"})
        obj.UpdateProperties(SERVICE_IFACE, {"ExecMainStartTimestamp": now_usec()})
        GLib.timeout_add_seconds(RUN_SECONDS, finish_service, mock, name)
    else:
        obj.UpdateProperties(UNIT_IFACE, {"ActiveState": "active", "SubState": "waiting"})
        cal = obj.Get(TIMER_IFACE, "TimersCalendar")
        nxt = calendar(cal[0][1])[1] if cal else 0
        obj.UpdateProperties(TIMER_IFACE, {"NextElapseUSecRealtime": dbus.UInt64(nxt)})


def stop(mock, name):
    obj = unit(mock, name)
    was_active = obj.Get(UNIT_IFACE, "ActiveState") == "active"
    obj.UpdateProperties(UNIT_IFACE, {"ActiveState": "inactive", "SubState": "dead"})
    if name.endswith(".service") and was_active:
        # Ended on request: killed by SIGTERM, which systemd counts as success.
        obj.UpdateProperties(
            SERVICE_IFACE,
            {
                "ExecMainExitTimestamp": now_usec(),
                "ExecMainCode": dbus.Int32(2),  # CLD_KILLED
                "ExecMainStatus": dbus.Int32(15),
                "Result": "success",
            },
        )
    elif name.endswith(".timer"):
        obj.UpdateProperties(TIMER_IFACE, {"NextElapseUSecRealtime": dbus.UInt64(0)})


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="")
def Reload(self):
    reload_units(self)


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def LoadUnit(self, name):
    if str(name) not in self.units:
        reload_units(self)
    if str(name) not in self.units:
        obj = unit_object(self, str(name))
        obj.UpdateProperties(UNIT_IFACE, {"LoadState": "not-found"})
    return dbus.ObjectPath(self.units[str(name)])


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def GetUnit(self, name):
    unit(self, name)
    return dbus.ObjectPath(self.units[str(name)])


@dbus.service.method(MAIN_IFACE, in_signature="asas", out_signature="a(ssssssouso)")
def ListUnitsByPatterns(self, states, patterns):
    result = []
    for name, path in sorted(self.units.items()):
        obj = mockobject.objects[path]
        p = obj.props[UNIT_IFACE]
        if p["LoadState"] != "loaded":
            continue
        if states and p["ActiveState"] not in states:
            continue
        if patterns and not any(fnmatch.fnmatch(name, pat) for pat in patterns):
            continue
        result.append(
            (name, p["Description"], p["LoadState"], p["ActiveState"], p["SubState"], "",
             dbus.ObjectPath(path), dbus.UInt32(0), "", dbus.ObjectPath("/"))
        )
    return dbus.Array(result, signature="(ssssssouso)")


@dbus.service.method(MAIN_IFACE, in_signature="asbb", out_signature="ba(sss)")
def EnableUnitFiles(self, files, _runtime, _force):
    changes = []
    for f in files:
        name = os.path.basename(str(f))
        if not os.path.exists(os.path.join(self.unit_dir, name)):
            raise dbus.exceptions.DBusException(
                f"Unit file {name} does not exist.", name="org.freedesktop.DBus.Error.FileNotFound"
            )
        self.enabled.add(name)
        unit(self, name).UpdateProperties(UNIT_IFACE, {"UnitFileState": "enabled"})
        changes.append(("symlink", f"{self.unit_dir}/timers.target.wants/{name}", name))
    return (True, dbus.Array(changes, signature="(sss)"))


@dbus.service.method(MAIN_IFACE, in_signature="asb", out_signature="a(sss)")
def DisableUnitFiles(self, files, _runtime):
    changes = []
    for f in files:
        name = os.path.basename(str(f))
        if name in self.enabled:
            self.enabled.discard(name)
            changes.append(("unlink", f"{self.unit_dir}/timers.target.wants/{name}", ""))
        if name in self.units:
            unit(self, name).UpdateProperties(UNIT_IFACE, {"UnitFileState": "disabled"})
    return dbus.Array(changes, signature="(sss)")


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="o")
def StartUnit(self, name, _mode):
    start(self, str(name))
    return job(self, name)


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="o")
def StopUnit(self, name, _mode):
    stop(self, str(name))
    return job(self, name)


@dbus.service.method(MAIN_IFACE, in_signature="ss", out_signature="o")
def RestartUnit(self, name, _mode):
    stop(self, str(name))
    start(self, str(name))
    return job(self, name)


@dbus.service.method(MOCK_IFACE, in_signature="s", out_signature="")
def Trigger(self, timer):
    obj = unit(self, timer)
    obj.UpdateProperties(TIMER_IFACE, {"LastTriggerUSec": now_usec()})
    start(self, obj.Get(TIMER_IFACE, "Unit"))
