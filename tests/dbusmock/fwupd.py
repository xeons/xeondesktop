"""fwupd (org.freedesktop.fwupd) for python-dbusmock, which has no
template of its own: the parts Automatic Updates (xd-wuaucpl) uses. Nothing
is flashed: the devices and releases are made up, and Install only changes
a device's version here.

    python3 -m dbusmock --system -t tests/dbusmock/fwupd.py \
        -p '{"CabDir": "/scratch/cabs"}'

Parameters:
    CabDir   where the releases' cabinet files are written; their
             Locations are file:// URLs there, with real checksums, so a
             client downloads (copies) and checks them as from the LVFS.
             Without it, the releases have https Locations a test must
             never fetch.
    Deny     true: Install fails as if polkit said no
    Delay    milliseconds between Install's progress steps (default 300)
    Log      a file that gets a line per Install ("install DEVICE-ID SIZE")

GetDevices lists three devices: system firmware (1.2.0, an update to 1.3.0
that needs a restart), an SSD (3.1, an update to 3.2) and a receiver that
can't be updated. GetHostSecurityAttrs gives HSI:1 with a runtime issue.
"""

# SPDX-License-Identifier: MIT

import hashlib
import os

import dbus
import dbus.service
from gi.repository import GLib

BUS_NAME = "org.freedesktop.fwupd"
MAIN_OBJ = "/"
MAIN_IFACE = "org.freedesktop.fwupd"
SYSTEM_BUS = True

# fwupd's FwupdDeviceFlags, FwupdStatus, FwupdReleaseUrgency,
# FwupdSecurityAttrResult and FwupdSecurityAttrFlags.
FLAG_INTERNAL, FLAG_UPDATABLE, FLAG_REQUIRE_AC, FLAG_NEEDS_REBOOT = 1 << 0, 1 << 1, 1 << 3, 1 << 8
STATUS_IDLE, STATUS_DEVICE_WRITE, STATUS_DEVICE_VERIFY, STATUS_WAITING_FOR_AUTH = 1, 5, 6, 11
URGENCY_HIGH, URGENCY_CRITICAL = 3, 4
ENABLED, NOT_ENABLED, ENCRYPTED, FOUND, NOT_FOUND, NOT_SUPPORTED = 1, 2, 7, 11, 12, 14
ATTR_SUCCESS, ATTR_RUNTIME_ISSUE = 1 << 0, 1 << 10

DEVICES = [
    {"DeviceId": "a45df35ac0e948ee180fe216a5f703f32dda163f", "Name": "System Firmware",
     "Vendor": "XD Test Computers", "Version": "1.2.0", "Plugin": "uefi_capsule",
     "Flags": FLAG_INTERNAL | FLAG_UPDATABLE | FLAG_REQUIRE_AC | FLAG_NEEDS_REBOOT,
     "Icon": ["computer"], "Summary": "UEFI ESRT device",
     "release": {"Version": "1.3.0", "Urgency": URGENCY_CRITICAL, "file": "xdtest-bios.cab",
                 "Description": "<p>This release fixes security issues.</p>"
                                "<ul><li>Fixes CVE-2026-1234</li><li>Faster boot</li></ul>"}},
    {"DeviceId": "6b7f3bf6a6b1c30e1c08ab1d34ee54cd5bb4a0c7", "Name": "XD Test SSD 512GB",
     "Vendor": "XD Storage", "Version": "3.1", "Plugin": "nvme",
     "Flags": FLAG_INTERNAL | FLAG_UPDATABLE, "Icon": ["drive-harddisk"],
     "release": {"Version": "3.2", "Urgency": URGENCY_HIGH, "file": "xdtest-ssd.cab",
                 "Description": "<p>Improves the drive's life.</p>"}},
    {"DeviceId": "0f7e38d2e1bf9b1a6c3f3a6e6c1e2a1d49a04f2b", "Name": "Unifying Receiver",
     "Vendor": "XD Input", "Version": "1.0", "Plugin": "logitech_hidpp", "Flags": 0,
     "Icon": ["input-mouse"]},
]

ATTRS = [
    ("org.fwupd.hsi.Uefi.SecureBoot", 1, ENABLED, ATTR_SUCCESS, "UEFI secure boot"),
    ("org.fwupd.hsi.Tpm.Version20", 1, FOUND, ATTR_SUCCESS, "TPM v2.0"),
    ("org.fwupd.hsi.Spi.Bioswe", 1, NOT_ENABLED, ATTR_SUCCESS, "SPI write"),
    ("org.fwupd.hsi.Iommu", 2, NOT_FOUND, 0, "IOMMU"),
    ("org.fwupd.hsi.EncryptedRam", 4, NOT_SUPPORTED, 0, "Encrypted RAM"),
    ("org.fwupd.hsi.Kernel.Lockdown", 0, NOT_ENABLED, ATTR_RUNTIME_ISSUE, "Linux kernel lockdown"),
    ("org.fwupd.hsi.Kernel.Swap", 0, ENCRYPTED, ATTR_SUCCESS, "Linux swap"),
]

state = {"devices": [], "deny": False, "delay": 300, "log": None}


def log(line):
    if state["log"]:
        with open(state["log"], "a", encoding="utf-8") as f:
            f.write(line + "\n")


def cab_bytes(d):
    return b"MSCF\0\0\0\0" + d["Name"].encode() + b" " + d["release"]["Version"].encode()


def device_dict(d):
    return dbus.Dictionary({
        "DeviceId": d["DeviceId"], "Name": d["Name"], "Vendor": d["Vendor"],
        "Version": d["Version"], "Plugin": d["Plugin"], "Flags": dbus.UInt64(d["Flags"]),
        "Icon": dbus.Array(d["Icon"], signature="s"), "Summary": d.get("Summary", ""),
        "Created": dbus.UInt64(1700000000),
    }, signature="sv")  # fmt: skip


def release_dict(d, cab_dir):
    r = d["release"]
    data = cab_bytes(d)
    if cab_dir:
        url = "file://" + os.path.join(cab_dir, r["file"])
    else:
        url = "https://fwupd.invalid/downloads/" + r["file"]
    return dbus.Dictionary({
        "AppstreamId": "org.xeondesktop.test." + r["file"].split(".")[0],
        "Name": d["Name"], "Summary": "Firmware for the " + d["Name"],
        "Version": r["Version"], "Description": r["Description"], "Vendor": d["Vendor"],
        "Size": dbus.UInt64(len(data)), "Urgency": dbus.UInt32(r["Urgency"]),
        "Checksum": hashlib.sha1(data).hexdigest() + "," + hashlib.sha256(data).hexdigest(),
        "Locations": dbus.Array([url], signature="s"), "Uri": url, "RemoteId": "lvfs",
        "Flags": dbus.UInt64(1 << 2), "InstallDuration": dbus.UInt32(60),
    }, signature="sv")  # fmt: skip


def load(mock, parameters):
    state["devices"] = [dict(d) for d in DEVICES]
    state["deny"] = bool(parameters.get("Deny", False))
    state["delay"] = int(parameters.get("Delay", 300))
    state["log"] = parameters.get("Log")
    state["cab_dir"] = parameters.get("CabDir")
    if state["cab_dir"]:
        os.makedirs(state["cab_dir"], exist_ok=True)
        for d in state["devices"]:
            if "release" in d:
                with open(os.path.join(state["cab_dir"], d["release"]["file"]), "wb") as f:
                    f.write(cab_bytes(d))
    mock.AddProperties(MAIN_IFACE, dbus.Dictionary({
        "DaemonVersion": "1.9.30", "HostVendor": "XD Test Computers",
        "HostProduct": "Test Machine", "HostSecurityId": "HSI:1! (v1.9.30)",
        "Tainted": False, "Interactive": False, "OnlyTrusted": True,
        "Status": dbus.UInt32(STATUS_IDLE), "Percentage": dbus.UInt32(0),
    }, signature="sv"))  # fmt: skip


def find(device_id):
    for d in state["devices"]:
        if d["DeviceId"] == device_id:
            return d
    raise dbus.exceptions.DBusException("No such device", name=MAIN_IFACE + ".NotFound")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="aa{sv}")
def GetDevices(self):
    return dbus.Array([device_dict(d) for d in state["devices"]], signature="a{sv}")


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="aa{sv}")
def GetUpgrades(self, device_id):
    d = find(device_id)
    if not d["Flags"] & FLAG_UPDATABLE:
        raise dbus.exceptions.DBusException("Device is not updatable",
                                            name=MAIN_IFACE + ".NotSupported")
    if "release" not in d:
        raise dbus.exceptions.DBusException(f"No upgrades for {d['Name']}",
                                            name=MAIN_IFACE + ".NothingToDo")
    return dbus.Array([release_dict(d, state["cab_dir"])], signature="a{sv}")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="aa{sv}")
def GetHostSecurityAttrs(self):
    out = []
    for appstream_id, level, result, flags, title in ATTRS:
        out.append(dbus.Dictionary({
            "AppstreamId": appstream_id, "HsiLevel": dbus.UInt32(level),
            "HsiResult": dbus.UInt32(result), "Flags": dbus.UInt64(flags), "Title": title,
            "Plugin": "test", "Created": dbus.UInt64(1700000000),
        }, signature="sv"))  # fmt: skip
    return dbus.Array(out, signature="a{sv}")


@dbus.service.method(MAIN_IFACE, in_signature="sha{sv}", out_signature="",
                     async_callbacks=("ok", "fail"))
def Install(self, device_id, handle, options, ok, fail):
    d = find(device_id)
    with os.fdopen(handle.take(), "rb") as f:
        data = f.read()
    log(f"install {device_id} {len(data)}")
    if state["deny"]:
        fail(dbus.exceptions.DBusException("Failed to obtain auth",
                                           name=MAIN_IFACE + ".AuthFailed"))
        return
    if "release" not in d or data != cab_bytes(d):
        fail(dbus.exceptions.DBusException("Firmware is not for this device",
                                           name=MAIN_IFACE + ".InvalidFile"))
        return
    steps = [(STATUS_DEVICE_WRITE, p) for p in (0, 25, 50, 75, 100)]
    steps.append((STATUS_DEVICE_VERIFY, 100))

    def tick():
        if steps:
            status, pct = steps.pop(0)
            self.UpdateProperties(MAIN_IFACE, {"Status": dbus.UInt32(status),
                                               "Percentage": dbus.UInt32(pct)})
            return True
        d["Version"] = d["release"]["Version"]
        del d["release"]
        self.UpdateProperties(MAIN_IFACE, {"Status": dbus.UInt32(STATUS_IDLE),
                                           "Percentage": dbus.UInt32(0)})
        self.EmitSignal(MAIN_IFACE, "DeviceChanged", "a{sv}", [device_dict(d)])
        self.EmitSignal(MAIN_IFACE, "Changed", "", [])
        ok()
        return False

    GLib.timeout_add(state["delay"], tick)
