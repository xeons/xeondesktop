"""colord (org.freedesktop.ColorManager) for python-dbusmock, with what
xd-color uses: listing devices and profiles, finding them by id or file,
creating devices (monitors) and profiles (from a file descriptor, as for
the user's own profiles), associating, removing and making defaults.
dbusmock has no colord template of its own.

    python3 -m dbusmock --system -t tests/dbusmock/colord.py

It starts with a printer and a scanner, colord's sRGB and Adobe RGB
profiles, and a printer profile associated with the printer. As in
colord, a device's Profiles lists the default first.
"""

# SPDX-License-Identifier: MIT

import os
import re
import time

import dbus

from dbusmock import mockobject

BUS_NAME = "org.freedesktop.ColorManager"
MAIN_OBJ = "/org/freedesktop/ColorManager"
MAIN_IFACE = "org.freedesktop.ColorManager"
DEVICE_IFACE = "org.freedesktop.ColorManager.Device"
PROFILE_IFACE = "org.freedesktop.ColorManager.Profile"
SYSTEM_BUS = True

SYSTEM_PROFILES = [
    # id, title, file, kind
    ("icc-srgb", "sRGB IEC61966-2.1", "/usr/share/color/icc/colord/sRGB.icc",
     "colorspace-conversion"),
    ("icc-adobe", "Adobe RGB (1998)", "/usr/share/color/icc/colord/AdobeRGB1998.icc",
     "colorspace-conversion"),
    ("icc-hp-plain", "HP LaserJet Plain Paper", "/usr/share/color/icc/HP-LaserJet-Plain.icc",
     "output-device"),
]
DEVICES = [
    # id, kind, vendor, model, profiles
    ("cups-HP_LaserJet_4", "printer", "HP", "HP LaserJet 4", ["icc-hp-plain"]),
    ("sane-epson2:libusb:001:004", "scanner", "Epson", "Perfection V39", []),
]


def sanitize(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def error(name, text):
    return dbus.exceptions.DBusException(text, name=f"org.freedesktop.ColorManager.{name}")


def load(mock, parameters):
    # For the devices' methods, which dbusmock runs with `objects`.
    mock.dev_add = lambda dev, p: dev_add(mock, dev, p)
    mock.dev_remove = lambda dev, p: dev_remove(mock, dev, p)
    mock.dev_default = lambda dev, p: dev_default(mock, dev, p)
    mock.devices = {}  # path -> id
    mock.profiles = {}  # path -> {"id", "file"}
    for pid, title, path, kind in SYSTEM_PROFILES:
        add_profile(mock, pid, {"Title": title, "Filename": path, "Kind": kind}, system=True)
    for did, kind, vendor, model, profiles in DEVICES:
        dev = add_device(mock, did, {"Kind": kind, "Vendor": vendor, "Model": model})
        for pid in profiles:
            dev_add(mock, dev, profile_by_id(mock, pid))
    mock.AddProperties(MAIN_IFACE, {"DaemonVersion": "1.4.8", "SystemVendor": "Mock",
                                    "SystemModel": "colord"})


def profile_by_id(mock, pid):
    for path, p in mock.profiles.items():
        if p["id"] == pid:
            return path
    raise error("NotFound", f"profile id '{pid}' does not exist")


def add_profile(mock, pid, props, system=False):
    path = f"{MAIN_OBJ}/profiles/{sanitize(pid)}"
    if path in mock.profiles:
        raise error("AlreadyExists", f"profile id '{pid}' already exists")
    filename = str(props.get("Filename", ""))
    title = str(props.get("Title", "")) or os.path.splitext(os.path.basename(filename))[0]
    mock.AddObject(path, PROFILE_IFACE, {
        "ProfileId": pid,
        "Title": title,
        "Filename": filename,
        "Kind": str(props.get("Kind", "display-device")),
        "Colorspace": "rgb",
        "Format": "ColorSpace..",
        "Qualifier": "",
        "Created": dbus.Int64(int(time.time())),
        "Scope": "normal",
        "HasVcgt": False,
        "IsSystemWide": system,
        "Metadata": dbus.Dictionary({k: str(v) for k, v in props.items()}, signature="ss"),
    }, [])
    mock.profiles[path] = {"id": pid, "file": filename}
    mock.EmitSignal(MAIN_IFACE, "ProfileAdded", "o", [dbus.ObjectPath(path)])
    return path


def add_device(mock, did, props):
    path = f"{MAIN_OBJ}/devices/{sanitize(did)}"
    if path in mock.devices:
        raise error("AlreadyExists", f"device id '{did}' already exists")
    mock.AddObject(path, DEVICE_IFACE, {
        "DeviceId": did,
        "Kind": str(props.get("Kind", "display")),
        "Vendor": str(props.get("Vendor", "")),
        "Model": str(props.get("Model", "")),
        "Serial": str(props.get("Serial", "")),
        "Mode": str(props.get("Mode", "physical")),
        "Colorspace": str(props.get("Colorspace", "rgb")),
        "Scope": "normal",
        "Profiles": dbus.Array([], signature="o"),
        "Created": dbus.UInt64(int(time.time())),
        "Modified": dbus.UInt64(int(time.time())),
        "Metadata": dbus.Dictionary({k: str(v) for k, v in props.items()}, signature="ss"),
    }, [
        ("AddProfile", "so", "", "objects['%s'].dev_add(self, args[1])" % MAIN_OBJ),
        ("RemoveProfile", "o", "", "objects['%s'].dev_remove(self, args[0])" % MAIN_OBJ),
        ("MakeProfileDefault", "o", "", "objects['%s'].dev_default(self, args[0])" % MAIN_OBJ),
    ])
    mock.devices[path] = did
    mock.EmitSignal(MAIN_IFACE, "DeviceAdded", "o", [dbus.ObjectPath(path)])
    return path


def profiles_of(dev):
    return list(dev.Get(DEVICE_IFACE, "Profiles"))


def set_profiles(mock, dev, profiles):
    dev.UpdateProperties(DEVICE_IFACE, {"Profiles": dbus.Array(profiles, signature="o")})
    mock.EmitSignal(MAIN_IFACE, "DeviceChanged", "o", [dbus.ObjectPath(dev.__dbus_object_path__)])


def check_profile(mock, profile):
    if str(profile) not in mock.profiles:
        raise error("NotFound", f"profile '{profile}' does not exist")


def dev_add(mock, dev, profile):
    if isinstance(dev, str):
        dev = mockobject.objects[dev]
    check_profile(mock, profile)
    profiles = profiles_of(dev)
    if dbus.ObjectPath(profile) in profiles:
        raise error("AlreadyExists", "profile is already associated")
    set_profiles(mock, dev, profiles + [dbus.ObjectPath(profile)])


def dev_remove(mock, dev, profile):
    profiles = profiles_of(dev)
    if dbus.ObjectPath(profile) not in profiles:
        raise error("NotFound", "profile is not associated with the device")
    set_profiles(mock, dev, [p for p in profiles if p != profile])


def dev_default(mock, dev, profile):
    check_profile(mock, profile)
    profiles = [p for p in profiles_of(dev) if p != profile]
    set_profiles(mock, dev, [dbus.ObjectPath(profile)] + profiles)


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="ao")
def GetDevices(self):
    return dbus.Array(sorted(self.devices), signature="o")


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="ao")
def GetDevicesByKind(self, kind):
    return dbus.Array([p for p in sorted(self.devices)
                       if mockobject.objects[p].Get(DEVICE_IFACE, "Kind") == kind], signature="o")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="ao")
def GetProfiles(self):
    return dbus.Array(sorted(self.profiles), signature="o")


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def FindDeviceById(self, did):
    for path, d in self.devices.items():
        if d == did:
            return dbus.ObjectPath(path)
    raise error("NotFound", f"device id '{did}' does not exist")


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def FindProfileById(self, pid):
    return dbus.ObjectPath(profile_by_id(self, pid))


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def FindProfileByFilename(self, filename):
    for path, p in self.profiles.items():
        if p["file"] == filename or os.path.basename(p["file"]) == filename:
            return dbus.ObjectPath(path)
    raise error("NotFound", f"profile filename '{filename}' does not exist")


@dbus.service.method(MAIN_IFACE, in_signature="ssa{ss}", out_signature="o")
def CreateDevice(self, did, _scope, props):
    return dbus.ObjectPath(add_device(self, str(did), props))


@dbus.service.method(MAIN_IFACE, in_signature="ssa{ss}", out_signature="o")
def CreateProfile(self, pid, _scope, props):
    return dbus.ObjectPath(add_profile(self, str(pid), props))


@dbus.service.method(MAIN_IFACE, in_signature="ssha{ss}", out_signature="o")
def CreateProfileWithFd(self, pid, _scope, fd, props):
    # colord reads the profile through the descriptor (it can't open the
    # user's files); here it only has to be readable and look like ICC.
    with os.fdopen(fd.take(), "rb") as f:
        head = f.read(128)
    if len(head) < 128 or head[36:40] != b"acsp":
        raise error("FileInvalid", "failed to parse the ICC profile")
    return dbus.ObjectPath(add_profile(self, str(pid), props))


@dbus.service.method(MAIN_IFACE, in_signature="o", out_signature="")
def DeleteDevice(self, path):
    if str(path) not in self.devices:
        raise error("NotFound", f"device '{path}' does not exist")
    del self.devices[str(path)]
    self.RemoveObject(path)
    self.EmitSignal(MAIN_IFACE, "DeviceRemoved", "o", [path])


@dbus.service.method(MAIN_IFACE, in_signature="o", out_signature="")
def DeleteProfile(self, path):
    check_profile(self, path)
    del self.profiles[str(path)]
    for dev in self.devices:
        obj = mockobject.objects[dev]
        profiles = profiles_of(obj)
        if path in profiles:
            set_profiles(self, obj, [p for p in profiles if p != path])
    self.RemoveObject(path)
    self.EmitSignal(MAIN_IFACE, "ProfileRemoved", "o", [path])
