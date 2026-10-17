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

Like UDisks2, it refuses to format, delete or shrink what's mounted,
partitions that overlap or leave their disk, a partition shrunk below
its filesystem, and formatting with mkntfs (missing here). "MockCalls"
on /org/freedesktop/UDisks2 lists what was called.
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
SWAP = U + ".Swapspace"
OM = "org.freedesktop.DBus.ObjectManager"
ERR = U + ".Error"

MIB = 1024 * 1024
GIB = 1024 * MIB

# libblockdev's resize modes: offline shrink 2, offline grow 4, online
# shrink 8, online grow 16.
RESIZE = {"ext4": 2 | 4 | 16, "ext3": 2 | 4 | 16, "xfs": 4 | 16, "btrfs": 8 | 16,
          "vfat": 2 | 4, "ntfs": 2 | 4}
FORMATS = {"ext4": True, "btrfs": True, "xfs": True, "vfat": True, "exfat": True,
           "ntfs": False, "swap": True}
NATURAL = {"gpt": {"vfat": "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7",
                   "ntfs": "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7",
                   "exfat": "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7",
                   "swap": "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"},
           "dos": {"vfat": "0x0c", "ntfs": "0x07", "exfat": "0x07", "swap": "0x82"}}
LINUX = {"gpt": "0fc63daf-8483-4772-8e79-3d69d8477de4", "dos": "0x83"}

state = types.SimpleNamespace(main=None, fs_size={}, calls=[])


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
            ("AddConfigurationItem", "(sa{sv})a{sv}", "", "")]


def fs_props(points):
    return {"MountPoints": dbus.Array([ay(p) for p in points], signature="ay"),
            "Size": dbus.UInt64(0)}


def fs_methods():
    return [("Mount", "a{sv}", "s", "ret = xd_udisks.mount(self, args)"),
            ("Unmount", "a{sv}", "", "xd_udisks.unmount(self, args)"),
            ("SetLabel", "sa{sv}", "", "xd_udisks.set_label(self, args)"),
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


def load(mock, _parameters):
    state.main = mock
    state.fs_size = {}
    state.calls = []
    mockobject.xd_udisks = types.SimpleNamespace(
        format=do_format, mount=do_mount, unmount=do_unmount, set_label=do_set_label,
        fs_resize=do_fs_resize, delete=do_delete, part_resize=do_part_resize,
        set_part=do_set_part, create=do_create, eject=do_eject)
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
        ("CanResize", "s", "(bts)",
         "ret = (args[0] in xd_udisks_resize, dbus.UInt64(xd_udisks_resize.get(args[0], 0)), '')"),
    ])
    mockobject.xd_udisks_formats = FORMATS
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
    log(f"Format {self.path} {fstype} label={opts.get('label', '')} "
        f"tear-down={bool(opts.get('tear-down', False))}")
    if mounted(self.path):
        fail("DeviceBusy", "Error formatting: the device is mounted")
    if SWAP in self.props and self.props[SWAP]["Active"]:
        fail("DeviceBusy", "Error formatting: the swap space is in use")
    if not FORMATS.get(fstype, fstype in ("gpt", "dos", "empty")):
        fail("Failed", f"Error formatting: mk{fstype} is not installed")
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
    point = f"/run/media/xeon/{label}"
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


def do_eject(self, _args):
    log(f"Eject {self.path}")
    for o in list(mockobject.objects.values()):
        if BLOCK in o.props and str(o.props[BLOCK]["Drive"]) == self.path and mounted(o.path):
            fail("DeviceBusy", f"{o.path} is mounted")
    self.UpdateProperties(DRIVE, {"MediaAvailable": dbus.Boolean(False)})
