"""UDisks2 (org.freedesktop.UDisks2) for python-dbusmock, which has no
template of its own: fake disks for xd-diskmgmt, with the methods it
calls. Nothing here touches a real device.

    python3 -m dbusmock --system -t tests/dbusmock/udisks2.py

The disks:
  sda  64 GiB GPT: EFI (vfat, /boot/efi), root (ext4, /), swap (active),
       about 19 GiB unallocated
  sdb  8 GiB MBR: Data (NTFS, active), an extended partition with Stuff
       (ext4, mounted under /run/media) and free space, 2 GiB unallocated
  sdc  4 GiB, empty (not initialized)
  sdd  16 GB USB stick (removable, ejectable): USBSTICK (FAT32)
  sr0  DVD drive without a disc

/ and /boot/efi are in /etc/fstab (Block.Configuration); Add-, Update-
and RemoveConfigurationItem change those entries (checking their types,
and that the one updated or removed is there), and Mount mounts where
the volume's entry says.

Like UDisks2, it refuses to format, delete or shrink what's mounted,
partitions that overlap or leave their disk, a partition shrunk below
its filesystem, and formatting with mkntfs (missing here); and to check
or repair a mounted filesystem. USBSTICK has errors: Check finds them,
Repair fixes them. "MockCalls" on /org/freedesktop/UDisks2 lists what
was called.

Parameters: "media", the folder Mount mounts in (default
/run/media/xeon), so a test can make the folders; "smart": true gives the
drives S.M.A.R.T. data, for xd-diskbench's Health tab: sda (Drive.Ata) is
healthy, sdb (Drive.Ata) has 8 pending sectors, the USB stick has none,
and it adds

  nvme0  1 TB NVMe drive (NVMe.Controller) with its health log

SmartUpdate is logged in MockCalls, and refused as not authorised with
"smart_denied": true. "encrypted": true adds

  sde  8 GB USB stick: an encrypted (LUKS2) partition, locked, whose
       passphrase is "correct horse"; unlocked, it holds Private (ext4)

with the Encrypted interface's Unlock (a wrong passphrase is refused),
Lock (refused while the cleartext is mounted) and ChangePassphrase.
Format's "encrypt.passphrase" makes any volume encrypted, its file
system inside, unlocked. Add- and RemoveConfigurationItem take
/etc/crypttab entries too ("crypttab": name, device, passphrase-path,
options and the optional passphrase-contents, all bytestrings); the
cleartext's /etc/fstab entries stay in Encrypted.ChildConfiguration
while it's locked.
"""

# SPDX-License-Identifier: MIT

import types

import dbus

from dbusmock import MOCK_IFACE, mockobject

BUS_NAME = "org.freedesktop.UDisks2"
MAIN_OBJ = "/org/freedesktop/UDisks2"
MAIN_IFACE = "org.freedesktop.UDisks2"
SYSTEM_BUS = True
IS_OBJECT_MANAGER = True

U = "org.freedesktop.UDisks2"
MANAGER = U + ".Manager"
DRIVE = U + ".Drive"
BLOCK = U + ".Block"
PTABLE = U + ".PartitionTable"
PART = U + ".Partition"
FS = U + ".Filesystem"
ENC = U + ".Encrypted"
SWAP = U + ".Swapspace"
OM = "org.freedesktop.DBus.ObjectManager"
ERR = U + ".Error"

MIB = 1024 * 1024
GIB = 1024 * MIB

# libblockdev's resize modes: offline shrink 2, offline grow 4, online
# shrink 8, online grow 16.
RESIZE = {"ext4": 2 | 4 | 16, "ext3": 2 | 4 | 16, "xfs": 4 | 16, "btrfs": 8 | 16,
          "vfat": 2 | 4, "ntfs": 2 | 4}
# Manager.CanCheck / CanRepair: the fsck programs "installed" here.
CHECKS = {"ext4", "vfat", "ntfs"}
FORMATS = {"ext4": True, "btrfs": True, "xfs": True, "vfat": True, "exfat": True,
           "ntfs": False, "swap": True}
NATURAL = {"gpt": {"vfat": "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7",
                   "ntfs": "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7",
                   "exfat": "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7",
                   "swap": "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"},
           "dos": {"vfat": "0x0c", "ntfs": "0x07", "exfat": "0x07", "swap": "0x82"}}
LINUX = {"gpt": "0fc63daf-8483-4772-8e79-3d69d8477de4", "dos": "0x83"}

state = types.SimpleNamespace(main=None, fs_size={}, calls=[], damaged=set(),
                              media="/run/media/xeon", passphrase={}, inner={}, child_config={},
                              keyfiles={})


def ay(s):
    return dbus.ByteArray(s.encode() + b"\0")


def path_of(name):
    return f"{MAIN_OBJ}/block_devices/{name}"


def obj(path):
    return mockobject.objects[str(path)]


def fail(name, msg):
    raise dbus.exceptions.DBusException(msg, name=f"{ERR}.{name}")


def log(what):
    state.calls.append(what)


def emit_added(path, ifaces):
    o = obj(path)
    state.main.EmitSignal(OM, "InterfacesAdded", "oa{sa{sv}}",
                          [dbus.ObjectPath(path),
                           dbus.Dictionary({i: o.props[i] for i in ifaces}, signature="sa{sv}")])


def emit_removed(path, ifaces):
    state.main.EmitSignal(OM, "InterfacesRemoved", "oas",
                          [dbus.ObjectPath(path), dbus.Array(ifaces, signature="s")])


def add_iface(path, iface, props, methods):
    o = obj(path)
    o.AddProperties(iface, dbus.Dictionary(props, signature="sv"))
    o.AddMethods(iface, methods)


def remove_iface(path, iface):
    o = obj(path)
    if iface in o.props:
        del o.props[iface]
        emit_removed(path, [iface])


def mounted(path):
    o = obj(path)
    return FS in o.props and len(o.props[FS]["MountPoints"]) > 0


def block_methods():
    return [("Format", "sa{sv}", "", "xd_udisks.format(self, args)"),
            ("AddConfigurationItem", "(sa{sv})a{sv}", "", "xd_udisks.config(self, None, args[0])"),
            ("UpdateConfigurationItem", "(sa{sv})(sa{sv})a{sv}", "",
             "xd_udisks.config(self, args[0], args[1])"),
            ("RemoveConfigurationItem", "(sa{sv})a{sv}", "", "xd_udisks.config(self, args[0], None)")]


def fs_props(points):
    return {"MountPoints": dbus.Array([ay(p) for p in points], signature="ay"),
            "Size": dbus.UInt64(0)}


def fs_methods():
    return [("Mount", "a{sv}", "s", "ret = xd_udisks.mount(self, args)"),
            ("Unmount", "a{sv}", "", "xd_udisks.unmount(self, args)"),
            ("SetLabel", "sa{sv}", "", "xd_udisks.set_label(self, args)"),
            ("Check", "a{sv}", "b", "ret = xd_udisks.check(self, False)"),
            ("Repair", "a{sv}", "b", "ret = xd_udisks.check(self, True)"),
            ("Resize", "ta{sv}", "", "xd_udisks.fs_resize(self, args)")]


def part_methods():
    return [("Delete", "a{sv}", "", "xd_udisks.delete(self, args)"),
            ("Resize", "ta{sv}", "", "xd_udisks.part_resize(self, args)"),
            ("SetFlags", "ta{sv}", "", "xd_udisks.set_part(self, 'Flags', dbus.UInt64(args[0]))"),
            ("SetType", "sa{sv}", "", "xd_udisks.set_part(self, 'Type', args[0])"),
            ("SetName", "sa{sv}", "", "xd_udisks.set_part(self, 'Name', args[0])")]


def table_methods():
    return [("CreatePartition", "ttssa{sv}", "o",
             "ret = xd_udisks.create(self, args[0], args[1], args[2], args[3], args[4], None, None)"),
            ("CreatePartitionAndFormat", "ttssa{sv}sa{sv}", "o",
             "ret = xd_udisks.create(self, args[0], args[1], args[2], args[3], args[4], "
             "args[5], args[6])")]


def add_block(mock, name, size, drive, usage="", fstype="", version="", label="",
              mounts=None, swap_active=None):
    path = path_of(name)
    mock.AddObject(path, BLOCK, {
        "Device": ay(f"/dev/{name}"),
        "PreferredDevice": ay(f"/dev/{name}"),
        "Symlinks": dbus.Array([], signature="ay"),
        "Id": f"by-id-{name}",
        "Size": dbus.UInt64(size),
        "ReadOnly": False,
        "Drive": dbus.ObjectPath(drive),
        "IdUsage": usage,
        "IdType": fstype,
        "IdVersion": version,
        "IdLabel": label,
        "IdUUID": f"uuid-{name}" if fstype else "",
        "Configuration": dbus.Array([], signature="(sa{sv})"),
        "CryptoBackingDevice": dbus.ObjectPath("/"),
        "HintPartitionable": True,
        "HintSystem": drive.endswith("sda"),
        "HintIgnore": False,
        "HintAuto": False,
        "HintName": "",
        "HintIconName": "",
    }, block_methods())
    if usage == "filesystem":
        add_iface(path, FS, fs_props(mounts or []), fs_methods())
        state.fs_size[path] = size
    if swap_active is not None:
        add_iface(path, SWAP, {"Active": swap_active}, [])
    return path


def add_partition(mock, disk, name, number, offset, size, ptype, flags=0, container=False,
                  contained=False, **kw):
    drive = obj(path_of(disk)).props[BLOCK]["Drive"]
    path = add_block(mock, name, size, drive, **kw)
    add_iface(path, PART, {
        "Number": dbus.UInt32(number),
        "Type": ptype,
        "Flags": dbus.UInt64(flags),
        "Offset": dbus.UInt64(offset),
        "Size": dbus.UInt64(size),
        "Name": "",
        "UUID": f"part-{name}",
        "Table": dbus.ObjectPath(path_of(disk)),
        "IsContainer": container,
        "IsContained": contained,
    }, part_methods())
    table = obj(path_of(disk))
    parts = list(table.props[PTABLE]["Partitions"]) + [dbus.ObjectPath(path)]
    table.UpdateProperties(PTABLE, {"Partitions": dbus.Array(parts, signature="o")})
    return path


def add_drive(mock, name, model, size, removable=False, ejectable=False, optical=False,
              media=True, bus="", sortkey="01"):
    path = f"{MAIN_OBJ}/drives/{name}"
    mock.AddObject(path, DRIVE, {
        "Vendor": "", "Model": model, "Revision": "1.0", "Serial": f"SN-{name}",
        "WWN": "", "Id": f"{model}-{name}".replace(" ", "-"),
        "Media": "optical_dvd" if optical and media else "",
        "MediaCompatibility": dbus.Array(["optical_dvd"] if optical else [], signature="s"),
        "MediaRemovable": removable or optical, "MediaAvailable": media,
        "MediaChangeDetected": True, "Size": dbus.UInt64(size),
        "TimeDetected": dbus.UInt64(0), "TimeMediaDetected": dbus.UInt64(0),
        "Optical": optical, "OpticalBlank": False, "OpticalNumTracks": dbus.UInt32(0),
        "OpticalNumAudioTracks": dbus.UInt32(0), "OpticalNumDataTracks": dbus.UInt32(0),
        "OpticalNumSessions": dbus.UInt32(0), "RotationRate": -1,
        "ConnectionBus": bus, "Seat": "seat0", "Removable": removable,
        "Ejectable": ejectable, "SortKey": sortkey, "CanPowerOff": removable,
        "SiblingId": "",
    }, [("Eject", "a{sv}", "", "xd_udisks.eject(self, args)"),
        ("PowerOff", "a{sv}", "", "")])
    return path


def add_table(path, ttype):
    o = obj(path)
    if PTABLE in o.props:
        remove_iface(path, PTABLE)
    add_iface(path, PTABLE, {"Type": ttype, "Partitions": dbus.Array([], signature="o")},
              table_methods())


def load(mock, parameters):
    state.main = mock
    state.media = str(parameters.get("media", "/run/media/xeon"))
    state.passphrase = {}
    state.inner = {}
    state.child_config = {}
    state.keyfiles = {}
    state.fs_size = {}
    state.calls = []
    state.damaged = {path_of("sdd1")}
    mockobject.xd_udisks = types.SimpleNamespace(
        format=do_format, mount=do_mount, unmount=do_unmount, set_label=do_set_label,
        check=do_check,
        fs_resize=do_fs_resize, delete=do_delete, part_resize=do_part_resize,
        set_part=do_set_part, create=do_create, eject=do_eject, config=do_config,
        unlock=do_unlock, lock=do_lock, change_passphrase=do_change_passphrase)
    mock.AddMethod(MOCK_IFACE, "MockCalls", "", "as", "ret = xd_udisks.calls()")
    mockobject.xd_udisks.calls = lambda: dbus.Array(state.calls, signature="s")

    mock.AddObject(f"{MAIN_OBJ}/Manager", MANAGER, {
        "Version": "2.11.0",
        "SupportedFilesystems": dbus.Array(list(FORMATS), signature="s"),
        "SupportedEncryptionTypes": dbus.Array(["luks1", "luks2"], signature="s"),
        "DefaultEncryptionType": "luks2",
    }, [
        ("CanFormat", "s", "(bs)",
         "ret = (xd_udisks_formats.get(args[0], False), "
         "'' if xd_udisks_formats.get(args[0], False) else 'mk' + args[0])"),
        ("CanCheck", "s", "(bs)",
         "ret = (args[0] in xd_udisks_checks, '' if args[0] in xd_udisks_checks else 'fsck')"),
        ("CanRepair", "s", "(bs)",
         "ret = (args[0] in xd_udisks_checks, '' if args[0] in xd_udisks_checks else 'fsck')"),
        ("CanResize", "s", "(bts)",
         "ret = (args[0] in xd_udisks_resize, dbus.UInt64(xd_udisks_resize.get(args[0], 0)), '')"),
    ])
    mockobject.xd_udisks_formats = FORMATS
    mockobject.xd_udisks_checks = CHECKS
    mockobject.xd_udisks_resize = RESIZE

    # sda: the system disk, GPT
    d = add_drive(mock, "VBOX_HARDDISK_sda", "VBOX HARDDISK", 64 * GIB, sortkey="00sda")
    add_block(mock, "sda", 64 * GIB, d)
    add_table(path_of("sda"), "gpt")
    add_partition(mock, "sda", "sda1", 1, MIB, 512 * MIB, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b",
                  usage="filesystem", fstype="vfat", version="FAT32", mounts=["/boot/efi"])
    add_partition(mock, "sda", "sda2", 2, 513 * MIB, 40 * GIB, LINUX["gpt"],
                  usage="filesystem", fstype="ext4", version="1.0", label="root", mounts=["/"])
    add_partition(mock, "sda", "sda3", 3, 513 * MIB + 40 * GIB, 4 * GIB,
                  "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", usage="other", fstype="swap",
                  swap_active=True)

    add_fstab(path_of("sda1"), "UUID=uuid-sda1", "/boot/efi", "vfat", "umask=0077", 0, 2)
    add_fstab(path_of("sda2"), "UUID=uuid-sda2", "/", "ext4", "rw,relatime", 0, 1)

    # sdb: MBR with an extended partition
    d = add_drive(mock, "QEMU_HARDDISK_sdb", "QEMU HARDDISK", 8 * GIB, sortkey="01sdb")
    add_block(mock, "sdb", 8 * GIB, d)
    add_table(path_of("sdb"), "dos")
    add_partition(mock, "sdb", "sdb1", 1, MIB, 2 * GIB, "0x07", flags=0x80,
                  usage="filesystem", fstype="ntfs", version="", label="Data")
    add_partition(mock, "sdb", "sdb2", 2, 2 * GIB + MIB, 4 * GIB, "0x05", container=True)
    add_partition(mock, "sdb", "sdb5", 5, 2 * GIB + 2 * MIB, GIB, "0x83", contained=True,
                  usage="filesystem", fstype="ext4", version="1.0", label="Stuff",
                  mounts=["/run/media/xeon/Stuff"])

    # sdc: blank
    d = add_drive(mock, "Blank_Disk_sdc", "Blank Disk", 4 * GIB, sortkey="02sdc")
    add_block(mock, "sdc", 4 * GIB, d)

    # sdd: a USB stick
    d = add_drive(mock, "SanDisk_Cruzer_sdd", "SanDisk Cruzer", 16_000_000_000,
                  removable=True, ejectable=True, bus="usb", sortkey="03sdd")
    add_block(mock, "sdd", 16_000_000_000, d)
    add_table(path_of("sdd"), "dos")
    add_partition(mock, "sdd", "sdd1", 1, MIB, 16_000_000_000 - MIB, "0x0c",
                  usage="filesystem", fstype="vfat", version="FAT32", label="USBSTICK")

    # sr0: a DVD drive without a disc
    d = add_drive(mock, "DVD_ROM_sr0", "DVD-ROM", 0, optical=True, ejectable=True,
                  media=False, sortkey="04sr0")
    add_block(mock, "sr0", 0, d)

    if parameters.get("encrypted", False):
        # sde: a USB stick with an encrypted partition, locked
        d = add_drive(mock, "Kingston_DataTraveler_sde", "DataTraveler 3.0", 8_000_000_000,
                      removable=True, ejectable=True, bus="usb", sortkey="05sde")
        add_block(mock, "sde", 8_000_000_000, d)
        add_table(path_of("sde"), "dos")
        p = add_partition(mock, "sde", "sde1", 1, MIB, 8_000_000_000 - MIB, "0x83",
                          usage="crypto", fstype="crypto_LUKS", version="2")
        make_encrypted(p, "correct horse", ("ext4", "Private", "uuid-private"))

    if parameters.get("smart", False):
        add_smart(mock, bool(parameters.get("smart_denied", False)))


# ---- methods ------------------------------------------------------------------------------


def disk_of(part_path):
    return str(obj(part_path).props[PART]["Table"])


def partitions(disk_path):
    return [str(p) for p in obj(disk_path).props[PTABLE]["Partitions"]]


def check_layout(disk_path, skip, offset, size, logical):
    """A partition [offset, offset+size) must fit the disk and not overlap
    its neighbours; a logical one must be inside the extended one."""
    disk = obj(disk_path)
    dsize = int(disk.props[BLOCK]["Size"])
    if offset < MIB or offset + size > dsize:
        fail("Failed", f"partition {offset}+{size} doesn't fit the disk ({dsize})")
    ext = None
    for p in partitions(disk_path):
        if p == skip:
            continue
        pp = obj(p).props[PART]
        po, ps = int(pp["Offset"]), int(pp["Size"])
        if pp["IsContainer"]:
            ext = (po, ps)
            if logical:
                continue
        elif logical != bool(pp["IsContained"]):
            continue
        if offset < po + ps and po < offset + size:
            fail("Failed", f"partition {offset}+{size} overlaps {p}")
    if logical and (ext is None or offset < ext[0] or offset + size > ext[0] + ext[1]):
        fail("Failed", "a logical partition must be inside the extended partition")


def set_fs(path, fstype, label, version=""):
    o = obj(path)
    remove_iface(path, FS)
    remove_iface(path, SWAP)
    usage = "filesystem" if fstype not in ("", "swap", "empty", "dos", "gpt") else (
        "other" if fstype == "swap" else "")
    if fstype in ("empty", "dos", "gpt"):
        fstype = ""
    o.UpdateProperties(BLOCK, {"IdType": fstype, "IdUsage": usage, "IdLabel": label,
                               "IdVersion": version,
                               "IdUUID": f"new-uuid-{path.rsplit('/', 1)[1]}" if fstype else ""})
    if usage == "filesystem":
        add_iface(path, FS, fs_props([]), fs_methods())
        emit_added(path, [FS])
        state.fs_size[path] = int(o.props[BLOCK]["Size"])
    elif fstype == "swap":
        add_iface(path, SWAP, {"Active": False}, [])
        emit_added(path, [SWAP])


def do_format(self, args):
    fstype, opts = str(args[0]), args[1]
    extra = ""
    if "erase" in opts:
        extra += f" erase={opts['erase']}"
    if "mkfs-args" in opts:
        extra += f" mkfs-args={','.join(str(a) for a in opts['mkfs-args'])}"
    encrypt = opts.get("encrypt.passphrase")
    if encrypt is not None:
        encrypt = text(encrypt) if isinstance(encrypt, dbus.Array) else str(encrypt)
        extra += f" encrypt.type={opts.get('encrypt.type', '')} encrypt.passphrase={encrypt}"
    log(f"Format {self.path} {fstype} label={opts.get('label', '')} "
        f"tear-down={bool(opts.get('tear-down', False))}{extra}")
    if mounted(self.path):
        fail("DeviceBusy", "Error formatting: the device is mounted")
    if SWAP in self.props and self.props[SWAP]["Active"]:
        fail("DeviceBusy", "Error formatting: the swap space is in use")
    if not FORMATS.get(fstype, fstype in ("gpt", "dos", "empty")):
        fail("Failed", f"Error formatting: mk{fstype} is not installed")
    if opts.get("erase", "zero") != "zero":
        fail("Failed", f"Error formatting: erase {opts['erase']} isn't supported here")
    if ENC in self.props and cleartext_of(self.path) is not None:
        if not opts.get("tear-down", False):
            fail("DeviceBusy", "Error formatting: the encrypted device is unlocked")
        lock(self.path)
    if opts.get("tear-down", False):
        # The old filesystem's /etc/fstab (and crypttab) entries go with it.
        self.UpdateProperties(BLOCK, {"Configuration": dbus.Array([], signature="(sa{sv})")})
        state.child_config.pop(self.path, None)
    if ENC in self.props:
        remove_iface(self.path, ENC)
        state.inner.pop(self.path, None)
    if fstype in ("gpt", "dos"):
        if PART in self.props:
            fail("Failed", "Error formatting: a partition table can't go in a partition")
        if PTABLE in self.props:
            for p in partitions(self.path):
                if mounted(p):
                    fail("DeviceBusy", f"Error formatting: {p} is mounted")
            for p in partitions(self.path):
                emit_removed(p, list(obj(p).props))
                state.main.RemoveObject(p)
        set_fs(self.path, fstype, "")
        add_table(self.path, fstype)
        emit_added(self.path, [PTABLE])
        return
    if PTABLE in self.props:
        fail("Failed", "Error formatting: the disk has a partition table")
    version = "FAT32" if fstype == "vfat" else ""
    if encrypt is not None:
        # LUKS here, the file system inside, unlocked (as UDisks2 leaves it).
        luks = str(opts.get("encrypt.type", "luks2"))
        set_fs(self.path, "", "")
        self.UpdateProperties(BLOCK, {"IdType": "crypto_LUKS", "IdUsage": "crypto",
                                      "IdVersion": luks[-1],
                                      "IdUUID": f"new-luks-{self.path.rsplit('/', 1)[1]}"})
        name = self.path.rsplit("/", 1)[1]
        make_encrypted(self.path, encrypt,
                       (fstype, str(opts.get("label", "")), f"new-uuid-{name}-inside"))
        emit_added(self.path, [ENC])
        unlock(self.path)
        return
    set_fs(self.path, fstype, str(opts.get("label", "")), version)
    if opts.get("update-partition-type", False) and PART in self.props:
        table = str(obj(disk_of(self.path)).props[PTABLE]["Type"])
        ptype = NATURAL[table].get(fstype, LINUX[table])
        self.UpdateProperties(PART, {"Type": ptype})


def do_mount(self, _args):
    log(f"Mount {self.path}")
    if mounted(self.path):
        fail("AlreadyMounted", "Device is already mounted")
    label = str(self.props[BLOCK]["IdLabel"]) or str(self.props[BLOCK]["IdUUID"])
    point = f"{state.media}/{label}"
    # As UDisks2 does, where /etc/fstab says.
    for kind, details in self.props[BLOCK]["Configuration"]:
        if kind == "fstab":
            point = text(details["dir"])
    self.UpdateProperties(FS, {"MountPoints": dbus.Array([ay(point)], signature="ay")})
    return point


def do_unmount(self, _args):
    log(f"Unmount {self.path}")
    if not mounted(self.path):
        fail("NotMounted", "Device is not mounted")
    point = bytes(self.props[FS]["MountPoints"][0]).rstrip(b"\0").decode()
    if point in ("/", "/boot/efi", "/usr", "/home"):
        fail("DeviceBusy", f"Error unmounting: target {point} is busy")
    self.UpdateProperties(FS, {"MountPoints": dbus.Array([], signature="ay")})


def do_set_label(self, args):
    log(f"SetLabel {self.path} {args[0]}")
    self.UpdateProperties(BLOCK, {"IdLabel": str(args[0])})


def do_check(self, repair):
    log(f"{'Repair' if repair else 'Check'} {self.path}")
    if mounted(self.path):
        device = bytes(self.props[BLOCK]["Device"]).rstrip(b"\0").decode()
        fail("DeviceBusy", f"Cannot {'repair' if repair else 'check'} {device}: it is mounted")
    if str(self.props[BLOCK]["IdType"]) not in CHECKS:
        fail("NotSupported", "Checking this filesystem is not supported")
    if repair:
        state.damaged.discard(self.path)
        return True
    return self.path not in state.damaged


def do_fs_resize(self, args):
    size = int(args[0])
    log(f"Filesystem.Resize {self.path} {size}")
    fstype = str(self.props[BLOCK]["IdType"])
    psize = int(self.props[BLOCK]["Size"])
    target = psize if size == 0 else size
    if target > psize:
        fail("Failed", "Error resizing: the filesystem can't be bigger than its partition")
    shrink = target < state.fs_size.get(self.path, psize)
    modes = RESIZE.get(fstype, 0)
    need = (8 if shrink else 16) if mounted(self.path) else (2 if shrink else 4)
    if not modes & need:
        fail("Failed", f"Error resizing: {fstype} can't be {'shrunk' if shrink else 'grown'} "
             f"{'while mounted' if mounted(self.path) else 'while unmounted'}")
    state.fs_size[self.path] = target


def do_delete(self, args):
    log(f"Delete {self.path} tear-down={bool(args[0].get('tear-down', False))}")
    if mounted(self.path):
        fail("DeviceBusy", "Error deleting partition: the device is mounted")
    if SWAP in self.props and self.props[SWAP]["Active"]:
        fail("DeviceBusy", "Error deleting partition: the swap space is in use")
    disk = disk_of(self.path)
    if self.props[PART]["IsContainer"]:
        for p in partitions(disk):
            if obj(p).props[PART]["IsContained"]:
                fail("Failed", "Error deleting partition: the extended partition isn't empty")
    path = self.path
    table = obj(disk)
    parts = [p for p in partitions(disk) if p != path]
    table.UpdateProperties(PTABLE, {"Partitions": dbus.Array(parts, signature="o")})
    emit_removed(path, list(self.props))
    state.main.RemoveObject(path)


def do_part_resize(self, args):
    size = int(args[0])
    log(f"Partition.Resize {self.path} {size}")
    pp = self.props[PART]
    disk = disk_of(self.path)
    if size == 0:
        fail("Failed", "the mock wants an exact size")
    size = (size + MIB - 1) // MIB * MIB
    check_layout(disk, self.path, int(pp["Offset"]), size, bool(pp["IsContained"]))
    if FS in self.props and size < state.fs_size.get(self.path, 0):
        fail("Failed", "Error resizing partition: it would cut off the end of its filesystem")
    self.UpdateProperties(PART, {"Size": dbus.UInt64(size)})
    self.UpdateProperties(BLOCK, {"Size": dbus.UInt64(size)})


def do_set_part(self, prop, value):
    log(f"Partition.Set{prop} {self.path} {value}")
    self.UpdateProperties(PART, {prop: value})


def do_create(self, offset, size, ptype, name, opts, fstype, fsopts):
    offset, size = int(offset), int(size)
    kind = str(opts.get("partition-type", ""))
    log(f"CreatePartition{'AndFormat' if fstype else ''} {self.path} {offset} {size} "
        f"type='{ptype}' kind={kind} fs={fstype}")
    ttype = str(self.props[PTABLE]["Type"])
    logical = kind == "logical"
    if ttype == "dos":
        prims = [p for p in partitions(self.path) if not obj(p).props[PART]["IsContained"]]
        if not logical and len(prims) >= 4:
            fail("Failed", "no free primary partition slot")
        if kind == "extended" and any(obj(p).props[PART]["IsContainer"] for p in prims):
            fail("Failed", "there is already an extended partition")
    offset = (offset + MIB - 1) // MIB * MIB
    if size == 0:
        fail("Failed", "the mock wants an exact size")
    check_layout(self.path, None, offset, size, logical)
    used = {int(obj(p).props[PART]["Number"]) for p in partitions(self.path)}
    number = 5 if logical else 1
    while number in used:
        number += 1
    disk = self.path.rsplit("/", 1)[1]
    pname = f"{disk}{number}"
    ext = kind == "extended"
    path = add_partition(state.main, disk, pname, number, offset, size,
                         "0x05" if ext else (ptype or LINUX[ttype]),
                         container=ext, contained=logical)
    emit_added(path, list(obj(path).props))
    if fstype:
        do_format(obj(path), [fstype, fsopts])
    return dbus.ObjectPath(path)


# ---- /etc/fstab ----------------------------------------------------------------------------

FSTAB_STR = ("fsname", "dir", "type", "opts")
FSTAB_INT = ("freq", "passno")


def text(ay):
    return bytes(bytearray(int(b) for b in ay)).rstrip(b"\0").decode()


def fstab_item(fsname, dir_, fstype, opts, freq, passno):
    return dbus.Struct(("fstab", dbus.Dictionary({
        "fsname": ay(fsname), "dir": ay(dir_), "type": ay(fstype), "opts": ay(opts),
        "freq": dbus.Int32(freq), "passno": dbus.Int32(passno)}, signature="sv")),
        signature="sa{sv}")


def add_fstab(path, *fields):
    o = obj(path)
    items = list(o.props[BLOCK]["Configuration"]) + [fstab_item(*fields)]
    o.props[BLOCK]["Configuration"] = dbus.Array(items, signature="(sa{sv})")


CRYPTTAB_STR = ("name", "device", "passphrase-path", "options")


def crypttab_item(f):
    return dbus.Struct(("crypttab", dbus.Dictionary({k: ay(f[k]) for k in CRYPTTAB_STR},
                                                    signature="sv")), signature="sa{sv}")


def is_bytes(v):
    return isinstance(v, dbus.ByteArray) or (isinstance(v, dbus.Array) and v.signature == "y")


def config_fields(item):
    """A configuration item as UDisks2 reads it: (kind, fields). An
    fstab entry's strings are 'ay', its numbers 'i', all six there; a
    crypttab entry's four are 'ay', and passphrase-contents (what UDisks2
    writes to passphrase-path) may come too."""
    kind = str(item[0])
    if kind == "fstab":
        return kind, fstab_fields(item)
    if kind != "crypttab":
        fail("Failed", f"only fstab and crypttab entries here, not {kind}")
    details = item[1]
    out = {}
    for key in CRYPTTAB_STR:
        if not is_bytes(details.get(key)):
            fail("Failed", f"crypttab {key} must be a bytestring (ay)")
        out[key] = text(details[key])
    if "passphrase-contents" in details:
        if not is_bytes(details["passphrase-contents"]):
            fail("Failed", "crypttab passphrase-contents must be a bytestring (ay)")
        out["passphrase-contents"] = text(details["passphrase-contents"])
    return kind, out


def fstab_fields(item):
    """An fstab item as UDisks2 reads it: the strings as 'ay', the numbers
    as 'i', all six there."""
    kind, details = item
    if kind != "fstab":
        fail("Failed", f"only fstab entries here, not {kind}")
    out = {}
    for key in FSTAB_STR:
        v = details.get(key)
        if not isinstance(v, dbus.ByteArray) and not (isinstance(v, dbus.Array)
                                                      and v.signature == "y"):
            fail("Failed", f"fstab {key} must be a bytestring (ay)")
        out[key] = text(v)
    for key in FSTAB_INT:
        v = details.get(key)
        if not isinstance(v, dbus.Int32):
            fail("Failed", f"fstab {key} must be an int32 (i)")
        out[key] = int(v)
    return out


def describe(kf):
    kind, f = kf
    if kind == "crypttab":
        return "crypttab " + " ".join(f"{k}={f[k]}" for k in f)
    return " ".join(f"{k}={f[k]}" for k in FSTAB_STR + FSTAB_INT)


def kept(kf):
    """An entry as Configuration shows it: never a crypttab entry's
    passphrase-contents (only GetSecretConfiguration would)."""
    kind, f = kf
    return kind, {k: v for k, v in f.items() if k != "passphrase-contents"}


def do_config(self, old, new):
    """Add (old None), Update or Remove (new None) an /etc/fstab or
    /etc/crypttab entry."""
    old_f = config_fields(old) if old is not None else None
    new_f = config_fields(new) if new is not None else None
    if new_f is None:
        log(f"RemoveConfigurationItem {self.path} {describe(old_f)}")
    elif old_f is None:
        log(f"AddConfigurationItem {self.path} {describe(new_f)}")
    else:
        log(f"UpdateConfigurationItem {self.path} {describe(old_f)} -> {describe(new_f)}")
    items = list(self.props[BLOCK]["Configuration"])
    current = [config_fields(i) for i in items]
    if old_f is not None:
        if kept(old_f) not in current:
            fail("Failed", f"Didn't find the entry to remove in /etc/{old_f[0]}")
        del items[current.index(kept(old_f))]
    if new_f is not None:
        kind, f = new_f
        if kind == "fstab":
            if not f["dir"].startswith("/"):
                fail("Failed", "the mount point must be an absolute path")
            items.append(fstab_item(*(f[k] for k in FSTAB_STR + FSTAB_INT)))
        else:
            if ENC not in self.props:
                fail("Failed", "a crypttab entry is for an encrypted device")
            # As UDisks2 2.11: contents are required, and a key file can
            # only go in /etc/luks-keys.
            if "passphrase-contents" not in f:
                fail("Failed", "Missing options or passphrase-contents parameter in entry to add")
            if f["passphrase-contents"]:
                if not f["passphrase-path"].startswith("/etc/luks-keys/"):
                    fail("Failed", "Crypttab passphrase file can only be created in the "
                         "/etc/luks-keys directory")
                state.keyfiles[f["passphrase-path"]] = f["passphrase-contents"]
            items.append(crypttab_item(f))
    if old_f is not None and old_f[0] == "crypttab" and new_f is None:
        state.keyfiles.pop(old_f[1]["passphrase-path"], None)
    items = dbus.Array(items, signature="(sa{sv})")
    self.UpdateProperties(BLOCK, {"Configuration": items})
    # A cleartext's entries are its encrypted volume's ChildConfiguration.
    backing = str(self.props[BLOCK]["CryptoBackingDevice"])
    if backing != "/":
        state.child_config[backing] = items
        obj(backing).UpdateProperties(ENC, {"ChildConfiguration": items})


# ---- encryption ----------------------------------------------------------------------------


def make_encrypted(path, passphrase, inside):
    """The Encrypted interface on `path`, locked; `inside` is (type, label,
    uuid) of the file system it holds."""
    state.passphrase[path] = passphrase
    state.inner[path] = inside
    add_iface(path, ENC, {
        "ChildConfiguration": dbus.Array([], signature="(sa{sv})"),
        "CleartextDevice": dbus.ObjectPath("/"),
        "HintEncryptionType": "luks2",
        "MetadataSize": dbus.UInt64(16 * MIB),
    }, [("Unlock", "sa{sv}", "o", "ret = xd_udisks.unlock(self, args)"),
        ("Lock", "a{sv}", "", "xd_udisks.lock(self, args)"),
        ("ChangePassphrase", "ssa{sv}", "", "xd_udisks.change_passphrase(self, args)")])


def cleartext_of(path):
    o = obj(path)
    c = str(o.props[ENC]["CleartextDevice"]) if ENC in o.props else "/"
    return None if c == "/" else c


def device_of(path):
    return text(obj(path).props[BLOCK]["Device"])


def unlock(path):
    """The cleartext device appears (dm-N), with the file system inside."""
    o = obj(path)
    n = 0
    while f"{MAIN_OBJ}/block_devices/dm_2d{n}" in mockobject.objects:
        n += 1
    name = f"dm_2d{n}"
    fstype, label, uuid = state.inner[path]
    luks_uuid = str(o.props[BLOCK]["IdUUID"])
    clear = add_block(state.main, name, int(o.props[BLOCK]["Size"]) - 16 * MIB,
                      o.props[BLOCK]["Drive"], usage="filesystem", fstype=fstype,
                      version="FAT32" if fstype == "vfat" else "1.0", label=label)
    c = obj(clear)
    # New, so no PropertiesChanged: InterfacesAdded has them.
    c.props[BLOCK].update({
        "Device": ay(f"/dev/dm-{n}"), "PreferredDevice": ay(f"/dev/mapper/luks-{luks_uuid}"),
        "IdUUID": uuid, "CryptoBackingDevice": dbus.ObjectPath(path), "HintPartitionable": False,
        "Configuration": state.child_config.get(path, dbus.Array([], signature="(sa{sv})"))})
    emit_added(clear, list(c.props))
    set_cleartext(path, clear)
    return dbus.ObjectPath(clear)


def set_cleartext(path, clear):
    """CleartextDevice, signalled by hand: dbusmock's UpdateProperties
    can't wrap an object path."""
    o = obj(path)
    o.props[ENC]["CleartextDevice"] = dbus.ObjectPath(clear)
    o.EmitSignal(dbus.PROPERTIES_IFACE, "PropertiesChanged", "sa{sv}as",
                 [ENC, dbus.Dictionary({"CleartextDevice": dbus.ObjectPath(clear, variant_level=1)},
                                       signature="sv"), dbus.Array([], signature="s")])


def lock(path):
    clear = cleartext_of(path)
    if clear is None:
        fail("NotUnlocked", f"Device {device_of(path)} is not unlocked")
    if mounted(clear):
        fail("DeviceBusy", f"Error locking {device_of(clear)} ({device_of(path)}): "
             f"Device {device_of(clear)} is mounted")
    emit_removed(clear, list(obj(clear).props))
    state.main.RemoveObject(clear)
    state.fs_size.pop(clear, None)
    set_cleartext(path, "/")


def do_unlock(self, args):
    passphrase = str(args[0])
    right = passphrase == state.passphrase.get(self.path)
    log(f"Unlock {self.path} {'right' if right else 'wrong'}")
    if cleartext_of(self.path) is not None:
        fail("AlreadyUnlocked", f"Device {device_of(self.path)} is already unlocked as "
             f"{device_of(cleartext_of(self.path))}")
    if not right:
        fail("Failed", f"Error unlocking {device_of(self.path)}: Failed to activate device: "
             "Incorrect passphrase.")
    return unlock(self.path)


def do_lock(self, _args):
    log(f"Lock {self.path}")
    lock(self.path)


def do_change_passphrase(self, args):
    old, new = str(args[0]), str(args[1])
    right = old == state.passphrase.get(self.path)
    log(f"ChangePassphrase {self.path} {'right' if right else 'wrong'} -> {new}")
    if not right:
        fail("Failed", f"Error changing passphrase on device {device_of(self.path)}: Failed to "
             "change passphrase: No key available with this passphrase.")
    state.passphrase[self.path] = new


def do_eject(self, _args):
    log(f"Eject {self.path}")
    for o in list(mockobject.objects.values()):
        if BLOCK in o.props and str(o.props[BLOCK]["Drive"]) == self.path and mounted(o.path):
            fail("DeviceBusy", f"{o.path} is mounted")
    self.UpdateProperties(DRIVE, {"MediaAvailable": dbus.Boolean(False)})


# ---- S.M.A.R.T. ---------------------------------------------------------------------------

ATA = U + ".Drive.Ata"
NVME = U + ".NVMe.Controller"

# (id, name, flags, value, worst, threshold, pretty, pretty_unit): pretty
# units are 1 a count, 2 milliseconds, 3 sectors, 4 millikelvin.
ATA_GOOD = [
    (0x01, "raw-read-error-rate", 0x000F, 117, 99, 6, 0, 1),
    (0x05, "reallocated-sector-count", 0x0033, 100, 100, 36, 0, 3),
    (0x09, "power-on-hours", 0x0032, 92, 92, 0, 7321 * 3600 * 1000, 2),
    (0x0C, "power-cycle-count", 0x0032, 99, 99, 20, 1043, 1),
    (0xC2, "temperature-celsius-2", 0x0022, 36, 45, 0, 309150, 4),
    (0xC5, "current-pending-sector", 0x0012, 100, 100, 0, 0, 3),
    (0xC6, "offline-uncorrectable", 0x0010, 100, 100, 0, 0, 3),
    (0xC7, "udma-crc-error-count", 0x003E, 200, 200, 0, 0, 1),
    (0xF1, "total-lbas-written", 0x0000, 100, 253, 0, 9765625000, 3),
    (0xF2, "total-lbas-read", 0x0000, 100, 253, 0, 15625000000, 3),
]


def ata_props(temperature, hours, failing=False):
    return {
        "SmartSupported": True, "SmartEnabled": True, "SmartUpdated": dbus.UInt64(1),
        "SmartFailing": failing, "SmartPowerOnSeconds": dbus.UInt64(hours * 3600),
        "SmartTemperature": dbus.Double(temperature + 273.15),
        "SmartNumAttributesFailing": dbus.Int32(0),
        "SmartNumAttributesFailedInThePast": dbus.Int32(0),
        "SmartNumBadSectors": dbus.Int64(0), "SmartSelftestStatus": "success",
        "SmartSelftestPercentRemaining": dbus.Int32(-1),
        "PmSupported": True, "PmEnabled": True, "ApmSupported": True, "ApmEnabled": False,
        "AamSupported": False, "AamEnabled": False, "AamVendorRecommendedValue": dbus.Int32(0),
        "WriteCacheSupported": True, "WriteCacheEnabled": True,
        "ReadLookaheadSupported": True, "ReadLookaheadEnabled": True,
        "SecurityEraseUnitMinutes": dbus.Int32(0),
        "SecurityEnhancedEraseUnitMinutes": dbus.Int32(0), "SecurityFrozen": True,
    }


def smart_methods(attributes_out):
    return [("SmartUpdate", "a{sv}", "", "xd_udisks.smart_update(self)"),
            ("SmartGetAttributes", "a{sv}", attributes_out,
             "ret = xd_udisks.smart_attributes(self)")]


def add_smart(mock, denied):
    state.smart = {}
    state.smart_denied = denied
    mockobject.xd_udisks.smart_update = do_smart_update
    mockobject.xd_udisks.smart_attributes = do_smart_attributes

    sda = f"{MAIN_OBJ}/drives/VBOX_HARDDISK_sda"
    obj(sda).props[DRIVE]["RotationRate"] = dbus.Int32(7200)
    add_iface(sda, ATA, ata_props(36, 7321), smart_methods("a(ysqiiixia{sv})"))
    state.smart[sda] = ATA_GOOD

    # sdb: 8 sectors waiting to be reallocated.
    sdb = f"{MAIN_OBJ}/drives/QEMU_HARDDISK_sdb"
    obj(sdb).props[DRIVE]["RotationRate"] = dbus.Int32(5400)
    add_iface(sdb, ATA, ata_props(41, 21877), smart_methods("a(ysqiiixia{sv})"))
    state.smart[sdb] = [a if a[0] != 0xC5 else a[:6] + (8,) + a[7:] for a in ATA_GOOD]

    # nvme0: an NVMe drive with its health log.
    d = add_drive(mock, "Samsung_SSD_980_PRO_1TB_nvme0", "Samsung SSD 980 PRO 1TB",
                  1_000_204_886_016, sortkey="06nvme0")
    obj(d).props[DRIVE]["RotationRate"] = dbus.Int32(0)
    obj(d).props[DRIVE]["Revision"] = "5B2QGXA7"
    add_block(mock, "nvme0n1", 1_000_204_886_016, d)
    add_iface(d, NVME, {
        "State": "live", "ControllerID": dbus.UInt16(6),
        "SubsystemNQN": ay("nqn.2014.08.org.nvmexpress:mock"), "FGUID": "",
        "NVMeRevision": "1.3", "UnallocatedCapacity": dbus.UInt64(0),
        "SmartUpdated": dbus.UInt64(1),
        "SmartCriticalWarning": dbus.Array([], signature="s"),
        "SmartPowerOnHours": dbus.UInt64(2210), "SmartTemperature": dbus.UInt16(311),
        "SmartSelftestStatus": "", "SmartSelftestPercentRemaining": dbus.Int32(-1),
        "SanitizeStatus": "", "SanitizePercentRemaining": dbus.Int32(-1),
    }, smart_methods("a{sv}"))
    state.smart[d] = {
        "avail_spare": dbus.Byte(100), "spare_thresh": dbus.Byte(10),
        "percent_used": dbus.Byte(2),
        "total_data_read": dbus.UInt64(48_213_000_000_000),
        "total_data_written": dbus.UInt64(31_904_000_000_000),
        "ctrl_busy_time": dbus.UInt64(1312), "power_cycles": dbus.UInt64(1187),
        "unsafe_shutdowns": dbus.UInt64(41), "media_errors": dbus.UInt64(0),
        "num_err_log_entries": dbus.UInt64(3),
        "temp_sensors": dbus.Array([dbus.UInt16(311), dbus.UInt16(318)], signature="q"),
        "wctemp": dbus.UInt16(355), "cctemp": dbus.UInt16(358),
        "warning_temp_time": dbus.UInt32(0), "critical_temp_time": dbus.UInt32(0),
    }


def do_smart_update(self):
    log(f"SmartUpdate {self.path}")
    if state.smart_denied:
        fail("NotAuthorizedCanObtain", "Not authorized to perform operation")


def do_smart_attributes(self):
    data = state.smart[self.path]
    if isinstance(data, dict):
        return dbus.Dictionary(data, signature="sv")
    return dbus.Array([dbus.Struct((dbus.Byte(i), n, dbus.UInt16(f), dbus.Int32(v),
                                    dbus.Int32(w), dbus.Int32(t), dbus.Int64(p), dbus.Int32(u),
                                    dbus.Dictionary({}, signature="sv")))
                       for (i, n, f, v, w, t, p, u) in data], signature="(ysqiiixia{sv})")
