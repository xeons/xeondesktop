"""PackageKit (org.freedesktop.PackageKit) for python-dbusmock, which has no
template of its own: the daemon and its transactions, as packagekit-glib2
and xd-appwiz use them. Nothing is installed or removed for real; the
packages are a list in this file (or the "packages" parameter).

    python3 -m dbusmock --system -t tests/dbusmock/packagekit.py \
        -p '{"AppsDir": "/tmp/scratch/share/applications"}'

Parameters:
    AppsDir     where installed packages' .desktop files are written (and
                removed again), so programs appear and disappear as on a
                real system; SearchFiles matches these paths
    Delay       milliseconds between the steps of installs and removals
                (default 400), so the progress dialog can be watched
    Deny        true: installs and removals fail as if polkit said no
    packages    a list of packages replacing the built-in ones (see
                PACKAGES)
    Log         a file that gets a line per RefreshCache and UpdatePackages
                ("refresh-cache", "update-packages FLAGS ID..."), so a test
                can tell a download (FLAGS has only-download) from an
                install

Updates: an installed package with an "update" (version, info: low ...
critical, restart: none, application, session or system, text) is listed
by GetUpdates and described by GetUpdateDetail (as UpdateDetails, the
plural signal packagekit-glib2 asks for). UpdatePackages with
only-download only marks it downloaded; without, it installs the new
version (RequireRestart for a "system" restart), or fails with Deny.

InstallFiles takes any file: with the only-trusted flag it fails with a
missing signature unless the file name contains "signed", as for a local
package file; the package is named after the file.
"""

# SPDX-License-Identifier: MIT

import copy
import os
import re

import dbus
from dbusmock import mockobject
from gi.repository import GLib

BUS_NAME = "org.freedesktop.PackageKit"
MAIN_OBJ = "/org/freedesktop/PackageKit"
MAIN_IFACE = "org.freedesktop.PackageKit"
TX_IFACE = "org.freedesktop.PackageKit.Transaction"
SYSTEM_BUS = True

# packagekit-glib2's enums (pk-enum.h).
ROLE = {"get-details": 3, "get-files": 4, "get-packages": 5, "get-update-detail": 8,
        "get-updates": 9, "install-files": 10, "install-packages": 11, "refresh-cache": 13,
        "remove-packages": 14, "resolve": 17, "search-file": 19, "search-group": 20,
        "search-name": 21, "update-packages": 22}
STATUS_WAIT, STATUS_SETUP, STATUS_QUERY = 1, 2, 4
STATUS_REMOVE, STATUS_REFRESH_CACHE, STATUS_DOWNLOAD, STATUS_INSTALL, STATUS_UPDATE = 6, 7, 8, 9, 10
STATUS_DOWNLOAD_REPOSITORY = 20
STATUS_DEP_RESOLVE, STATUS_COMMIT, STATUS_FINISHED, STATUS_WAITING_FOR_AUTH = 13, 16, 18, 31
INFO_INSTALLED, INFO_AVAILABLE, INFO_DOWNLOADING = 1, 2, 10
INFO_INSTALLING, INFO_REMOVING, INFO_FINISHED = 12, 13, 18
INFO_UPDATING = 11
# An update's kind (info) and severity (the high 16 bits, PackageKit 1.2.4).
INFO = {"low": 3, "enhancement": 4, "normal": 5, "bugfix": 6, "important": 7, "security": 8,
        "critical": 26}
RESTART = {"none": 1, "application": 2, "session": 3, "system": 4}
FLAG_ONLY_DOWNLOAD = 1 << 3
EXIT_SUCCESS, EXIT_FAILED, EXIT_CANCELLED, EXIT_NEED_UNTRUSTED = 1, 2, 3, 8
ERROR_PACKAGE_NOT_INSTALLED, ERROR_PACKAGE_NOT_FOUND = 7, 8
ERROR_DEP_RESOLUTION_FAILED, ERROR_TRANSACTION_CANCELLED = 13, 17
ERROR_MISSING_GPG_SIGNATURE, ERROR_NOT_AUTHORIZED = 31, 48
FILTER_INSTALLED, FILTER_NOT_INSTALLED = 1 << 2, 1 << 3
FLAG_ONLY_TRUSTED, FLAG_SIMULATE = 1 << 1, 1 << 2
GROUPS = {"accessories": 2, "games": 11, "graphics": 12, "office": 19, "other": 20,
          "programming": 22, "system": 27}
# What a backend that searches files and simulates would say it can do.
ROLES = sum(1 << r for r in ROLE.values())

# name, version, installed, group, size, summary, apps (desktop id, name),
# requires; url, license and description are made up from these.
PACKAGES = [
    {"name": "xdtest-editor", "version": "2.4.1-1", "installed": True, "group": "accessories",
     "size": 23477862, "summary": "A small text editor",
     "apps": [["xdtest-editor", "XD Test Editor"]], "requires": [],
     "update": {"version": "2.4.2-1", "info": "security", "severity": "important",
                "restart": "application", "text": "Fixes a crash opening long lines.",
                "cves": ["https://example.org/CVE-2026-0001"]}},
    {"name": "xdtest-kernel", "version": "6.1-1", "installed": True, "group": "system",
     "size": 150000000, "summary": "The operating system kernel", "apps": [], "requires": [],
     "update": {"version": "6.2-1", "info": "normal", "restart": "system",
                "text": "A newer kernel with more drivers."}},
    {"name": "xdtest-libpaint", "version": "1.0-1", "installed": True, "group": "system",
     "size": 2000000, "summary": "Drawing library and tools",
     "apps": [["xdtest-painttools", "XD Test Paint Tools"]], "requires": []},
    {"name": "xdtest-paint", "version": "3.0-2", "installed": True, "group": "graphics",
     "size": 56623104, "summary": "Paint program", "apps": [["xdtest-paint", "XD Test Paint"]],
     "requires": ["xdtest-libpaint"]},
    {"name": "xdtest-sketch", "version": "0.9-1", "installed": True, "group": "graphics",
     "size": 8000000, "summary": "Sketching program",
     "apps": [["xdtest-sketch", "XD Test Sketch"]], "requires": ["xdtest-libpaint"]},
    {"name": "xdtest-office", "version": "7.6-1", "installed": True, "group": "office",
     "size": 429916160, "summary": "Office suite",
     "apps": [["xdtest-writer", "XD Test Writer"], ["xdtest-calc", "XD Test Calc"]],
     "requires": []},
    {"name": "xdtest-chess", "version": "1.2-1", "installed": False, "group": "games",
     "size": 3500000, "summary": "Chess against the computer",
     "apps": [["xdtest-chess", "XD Test Chess"]], "requires": ["xdtest-chess-data"]},
    {"name": "xdtest-chess-data", "version": "1.2-1", "installed": False, "group": "games",
     "size": 12000000, "summary": "Chess openings book", "apps": [], "requires": []},
    {"name": "xdtest-solitaire", "version": "1.0-1", "installed": False, "group": "games",
     "size": 900000, "summary": "Card games", "apps": [["xdtest-solitaire", "XD Test Solitaire"]],
     "requires": []},
]

state = {"packages": [], "apps_dir": None, "delay": 400, "deny": False, "next": 1, "log": None}


def log(line):
    if state["log"]:
        with open(state["log"], "a", encoding="utf-8") as f:
            f.write(line + "\n")


def package_id(p):
    data = "installed:extra" if p["installed"] else "extra"
    return f"{p['name']};{p['version']};x86_64;{data}"


def find(name_or_id):
    name = name_or_id.split(";")[0]
    for p in state["packages"]:
        if p["name"] == name:
            return p
    return None


def desktop_path(app_id):
    return os.path.join(state["apps_dir"], app_id + ".desktop") if state["apps_dir"] else None


def write_desktop_files(p):
    for app_id, name in p["apps"]:
        path = desktop_path(app_id)
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write("[Desktop Entry]\nType=Application\n"
                        f"Name={name}\nExec=true\nIcon=package-x-generic\n")


def remove_desktop_files(p):
    for app_id, _ in p["apps"]:
        path = desktop_path(app_id)
        if path and os.path.exists(path):
            os.unlink(path)


def files_of(p):
    files = [f"/usr/bin/{p['name']}"]
    files += [desktop_path(a) or f"/usr/share/applications/{a}.desktop" for a, _ in p["apps"]]
    return files


def matches_filter(p, flt):
    if flt & FILTER_INSTALLED and not p["installed"]:
        return False
    if flt & FILTER_NOT_INSTALLED and p["installed"]:
        return False
    return True


def load(mock, parameters):
    state["packages"] = copy.deepcopy(parameters.get("packages", PACKAGES))
    state["apps_dir"] = parameters.get("AppsDir")
    state["delay"] = int(parameters.get("Delay", 400))
    state["deny"] = bool(parameters.get("Deny", False))
    state["log"] = parameters.get("Log")
    if state["apps_dir"]:
        os.makedirs(state["apps_dir"], exist_ok=True)
        for p in state["packages"]:
            if p["installed"]:
                write_desktop_files(p)
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "VersionMajor": dbus.UInt32(1),
                "VersionMinor": dbus.UInt32(4),
                "VersionMicro": dbus.UInt32(0),
                "BackendName": "mock",
                "BackendDescription": "python-dbusmock",
                "BackendAuthor": "XeonDesktop",
                "Roles": dbus.UInt64(ROLES),
                "Groups": dbus.UInt64(sum(1 << g for g in GROUPS.values())),
                "Filters": dbus.UInt64(FILTER_INSTALLED | FILTER_NOT_INSTALLED),
                "MimeTypes": dbus.Array(["application/x-compressed-tar"], signature="s"),
                "Locked": False,
                "NetworkState": dbus.UInt32(2),
                "DistroId": "mock;1;x86_64",
            },
            signature="sv",
        ),
    )
    mock.AddMethods(
        MAIN_IFACE,
        [
            ("CanAuthorize", "s", "u", "ret = dbus.UInt32(1)"),
            ("GetDaemonState", "", "s", 'ret = "mock"'),
            ("GetTransactionList", "", "ao", "ret = dbus.Array([], signature='o')"),
            ("GetTimeSinceAction", "u", "u", "ret = dbus.UInt32(0)"),
            ("StateHasChanged", "s", "", ""),
            ("SuggestDaemonQuit", "", "", ""),
            ("SetProxy", "ssssss", "", ""),
        ],
    )


# ---- transactions ----------------------------------------------------------


class Run:
    """A transaction's steps, one per timeout, then Finished and Destroy."""

    def __init__(self, tx, role, delay):
        self.tx = tx
        self.steps = []
        self.exit = EXIT_SUCCESS
        self.delay = delay
        tx.cancelled = False
        tx.UpdateProperties(TX_IFACE, {"Role": dbus.UInt32(ROLE[role])})

    def status(self, s, pct=None):
        props = {"Status": dbus.UInt32(s)}
        if pct is not None:
            props["Percentage"] = dbus.UInt32(pct)
        self.steps.append(lambda: self.tx.UpdateProperties(TX_IFACE, props))

    def package(self, info, p):
        self.steps.append(lambda: self.tx.EmitSignal(
            TX_IFACE, "Package", "uss", [dbus.UInt32(info), package_id(p), p["summary"]]))

    def item(self, p, status, pct):
        self.steps.append(lambda: self.tx.EmitSignal(
            TX_IFACE, "ItemProgress", "suu", [package_id(p), dbus.UInt32(status), dbus.UInt32(pct)]))

    def call(self, fn):
        self.steps.append(fn)

    def error(self, code, details, exit_code=EXIT_FAILED):
        self.steps.append(lambda: self.tx.EmitSignal(
            TX_IFACE, "ErrorCode", "us", [dbus.UInt32(code), details]))
        self.exit = exit_code

    def tick(self):
        if self.tx.cancelled:
            self.steps = []
            self.tx.EmitSignal(TX_IFACE, "ErrorCode", "us",
                               [dbus.UInt32(ERROR_TRANSACTION_CANCELLED), "Cancelled by the user"])
            self.exit = EXIT_CANCELLED
        if self.steps:
            self.steps.pop(0)()
            return True
        self.tx.UpdateProperties(TX_IFACE, {"Status": dbus.UInt32(STATUS_FINISHED),
                                            "Percentage": dbus.UInt32(100)})
        self.tx.EmitSignal(TX_IFACE, "Finished", "uu", [dbus.UInt32(self.exit), dbus.UInt32(10)])
        self.tx.EmitSignal(TX_IFACE, "Destroy", "", [])
        path = self.tx.path
        GLib.timeout_add(1000, lambda: (mockobject.objects[MAIN_OBJ].RemoveObject(path), False)[1])
        return False

    def start(self):
        GLib.timeout_add(self.delay, self.tick)


def query(tx, role):
    return Run(tx, role, 5)


def emit_matching(run, flt, pred):
    for p in state["packages"]:
        if matches_filter(p, flt) and pred(p):
            run.package(INFO_INSTALLED if p["installed"] else INFO_AVAILABLE, p)
    run.start()


def tx_search_files(self, flt, values):
    run = query(self, "search-file")
    emit_matching(run, flt, lambda p: any(v in files_of(p) for v in values))


def tx_search_names(self, flt, values):
    run = query(self, "search-name")
    emit_matching(run, flt, lambda p: any(v.lower() in p["name"] for v in values))


def tx_search_groups(self, flt, values):
    run = query(self, "search-group")
    emit_matching(run, flt, lambda p: p["group"] in values)


def tx_resolve(self, flt, values):
    run = query(self, "resolve")
    emit_matching(run, flt, lambda p: p["name"] in values)


def tx_get_packages(self, flt):
    run = query(self, "get-packages")
    emit_matching(run, flt, lambda p: True)


def tx_get_details(self, ids):
    run = query(self, "get-details")
    for i in ids:
        p = find(i)
        if p is None:
            run.error(ERROR_PACKAGE_NOT_FOUND, f"{i} not found")
            break
        details = dbus.Dictionary({
            "package-id": package_id(p),
            "summary": p["summary"],
            "description": p.get("description", f"{p['summary']}, for testing."),
            "url": p.get("url", f"https://example.org/{p['name']}"),
            "license": p.get("license", "MIT"),
            "group": dbus.UInt32(GROUPS.get(p["group"], GROUPS["other"])),
            "size": dbus.UInt64(p["size"]),
        }, signature="sv")
        run.call(lambda d=details: self.EmitSignal(TX_IFACE, "Details", "a{sv}", [d]))
    run.start()


def tx_get_files(self, ids):
    run = query(self, "get-files")
    for i in ids:
        p = find(i)
        if p is not None:
            run.call(lambda p=p: self.EmitSignal(TX_IFACE, "Files", "sas",
                                                 [package_id(p), dbus.Array(files_of(p), signature="s")]))
    run.start()


def needed_by(names):
    """Installed packages that need any of `names`, recursively."""
    out = []
    todo = list(names)
    while todo:
        n = todo.pop()
        for p in state["packages"]:
            if p["installed"] and n in p["requires"] and p["name"] not in out and p["name"] not in names:
                out.append(p["name"])
                todo.append(p["name"])
    return out


def needs(names):
    """Uninstalled packages `names` need, recursively."""
    out = []
    todo = list(names)
    while todo:
        p = find(todo.pop())
        for r in p["requires"] if p else []:
            q = find(r)
            if q and not q["installed"] and r not in out and r not in names:
                out.append(r)
                todo.append(r)
    return out


def change(run, info, status, pkgs, apply):
    """The steps of installing or removing `pkgs`."""
    run.status(STATUS_WAITING_FOR_AUTH)
    if state["deny"]:
        run.error(ERROR_NOT_AUTHORIZED, "Failed to obtain authentication.")
        return
    run.status(STATUS_DEP_RESOLVE, 101)
    if info == INFO_INSTALLING:
        run.status(STATUS_DOWNLOAD, 10)
        for p in pkgs:
            run.package(INFO_DOWNLOADING, p)
            run.item(p, STATUS_DOWNLOAD, 100)
    run.status(status, 30)
    for n, p in enumerate(pkgs):
        run.package(info, p)
        run.item(p, status, 50)
        run.call(lambda p=p: apply(p))
        run.item(p, status, 100)
        run.status(status, 30 + 70 * (n + 1) // len(pkgs))
        run.package(INFO_FINISHED, p)


def do_remove(p):
    p["installed"] = False
    remove_desktop_files(p)


def do_install(p):
    p["installed"] = True
    write_desktop_files(p)


def tx_remove_packages(self, flags, ids, allow_deps, _autoremove):
    run = Run(self, "remove-packages", 5 if flags & FLAG_SIMULATE else state["delay"])
    targets = [find(i) for i in ids]
    if any(p is None or not p["installed"] for p in targets):
        run.error(ERROR_PACKAGE_NOT_INSTALLED, "The package is not installed")
        run.start()
        return
    names = [p["name"] for p in targets]
    others = needed_by(names)
    if others and not allow_deps:
        run.error(ERROR_DEP_RESOLUTION_FAILED, f"{', '.join(others)} need {', '.join(names)}")
    elif flags & FLAG_SIMULATE:
        for p in targets + [find(n) for n in others]:
            run.package(INFO_REMOVING, p)
    else:
        change(run, INFO_REMOVING, STATUS_REMOVE, targets + [find(n) for n in others], do_remove)
    run.start()


def tx_install_packages(self, flags, ids):
    run = Run(self, "install-packages", 5 if flags & FLAG_SIMULATE else state["delay"])
    targets = [find(i) for i in ids]
    if any(p is None for p in targets):
        run.error(ERROR_PACKAGE_NOT_FOUND, "The package was not found")
        run.start()
        return
    pkgs = [find(n) for n in needs([p["name"] for p in targets])] + targets
    if flags & FLAG_SIMULATE:
        for p in pkgs:
            run.package(INFO_INSTALLING, p)
    else:
        change(run, INFO_INSTALLING, STATUS_INSTALL, pkgs, do_install)
    run.start()


def tx_install_files(self, flags, paths):
    run = Run(self, "install-files", 5 if flags & FLAG_SIMULATE else state["delay"])
    if flags & FLAG_ONLY_TRUSTED and not all("signed" in os.path.basename(f) for f in paths):
        run.status(STATUS_INSTALL)
        run.error(ERROR_MISSING_GPG_SIGNATURE, "The package is not signed", EXIT_NEED_UNTRUSTED)
        run.start()
        return
    pkgs = []
    for f in paths:
        base = os.path.basename(f)
        m = re.match(r"(.+?)-(\d[^-]*-\d+)", base)
        name = m.group(1) if m else base.split(".")[0]
        p = find(name)
        if p is None:
            p = {"name": name, "version": m.group(2) if m else "1.0-1", "installed": False,
                 "group": "other", "size": os.path.getsize(f) if os.path.exists(f) else 1000,
                 "summary": f"Installed from {base}", "apps": [[name, name.replace("-", " ").title()]],
                 "requires": []}
            state["packages"].append(p)
        pkgs.append(p)
    if flags & FLAG_SIMULATE:
        for p in pkgs:
            run.package(INFO_INSTALLING, p)
    else:
        change(run, INFO_INSTALLING, STATUS_INSTALL, pkgs, do_install)
    run.start()


def update_id(p):
    return f"{p['name']};{p['update']['version']};x86_64;extra"


def with_updates():
    return [p for p in state["packages"] if p["installed"] and p.get("update")]


def update_info(p):
    u = p["update"]
    return INFO.get(u.get("info", "normal"), 5) | (INFO.get(u.get("severity", ""), 0) << 16)


def emit_package(run, info, pid, summary):
    run.call(lambda: run.tx.EmitSignal(TX_IFACE, "Package", "uss", [dbus.UInt32(info), pid, summary]))


def tx_refresh_cache(self, _force):
    log("refresh-cache")
    run = Run(self, "refresh-cache", 50)
    run.status(STATUS_REFRESH_CACHE, 0)
    run.status(STATUS_DOWNLOAD_REPOSITORY, 50)
    run.status(STATUS_REFRESH_CACHE, 100)
    run.start()


def tx_get_updates(self, _flt):
    run = query(self, "get-updates")
    for p in with_updates():
        emit_package(run, update_info(p), update_id(p), p["summary"])
    run.start()


def tx_get_update_detail(self, ids):
    run = query(self, "get-update-detail")
    details = []
    for i in ids:
        p = find(i)
        if p is None or not p.get("update"):
            continue
        u = p["update"]
        details.append(dbus.Struct(
            (i, dbus.Array([package_id(p)], signature="s"), dbus.Array([], signature="s"),
             dbus.Array([], signature="s"), dbus.Array([], signature="s"),
             dbus.Array(u.get("cves", []), signature="s"),
             dbus.UInt32(RESTART[u.get("restart", "none")]), u.get("text", ""), "",
             dbus.UInt32(1), "2026-10-01T00:00:00Z", ""),
            signature="sasasasasasussuss"))  # fmt: skip
    run.call(lambda: self.EmitSignal(TX_IFACE, "UpdateDetails", "a(sasasasasasussuss)",
                                     [dbus.Array(details, signature="(sasasasasasussuss)")]))
    run.start()


def do_update(run, p):
    restart = p["update"].get("restart", "none")
    p["version"] = p["update"]["version"]
    del p["update"]
    if restart == "system":
        run.tx.EmitSignal(TX_IFACE, "RequireRestart", "us", [dbus.UInt32(RESTART["system"]),
                                                               package_id(p)])


def tx_update_packages(self, flags, ids):
    log(f"update-packages {int(flags)} {' '.join(ids)}")
    simulate = flags & FLAG_SIMULATE
    run = Run(self, "update-packages", 5 if simulate else state["delay"])
    targets = [find(i) for i in ids]
    if any(p is None or not p.get("update") for p in targets):
        run.error(ERROR_PACKAGE_NOT_FOUND, "There is no such update")
        run.start()
        return
    if simulate:
        for p in targets:
            emit_package(run, INFO_UPDATING, update_id(p), p["summary"])
    elif flags & FLAG_ONLY_DOWNLOAD:
        # Into the package cache only: no permission needed, nothing changes.
        run.status(STATUS_DOWNLOAD, 0)
        for n, p in enumerate(targets):
            emit_package(run, INFO_DOWNLOADING, update_id(p), p["summary"])
            run.call(lambda p=p: p["update"].__setitem__("downloaded", True))
            run.status(STATUS_DOWNLOAD, 100 * (n + 1) // len(targets))
    else:
        run.status(STATUS_WAITING_FOR_AUTH)
        if state["deny"]:
            run.error(ERROR_NOT_AUTHORIZED, "Failed to obtain authentication.")
            run.start()
            return
        if not all(p["update"].get("downloaded") for p in targets):
            run.status(STATUS_DOWNLOAD, 10)
        run.status(STATUS_UPDATE, 30)
        for n, p in enumerate(targets):
            new = update_id(p)
            emit_package(run, INFO_UPDATING, new, p["summary"])
            run.call(lambda p=p: do_update(run, p))
            run.status(STATUS_UPDATE, 30 + 70 * (n + 1) // len(targets))
            emit_package(run, INFO_FINISHED, new, p["summary"])
    run.start()


def tx_cancel(self):
    self.cancelled = True


TX_METHODS = [
    ("SetHints", "as", lambda self, hints: None),
    ("Cancel", "", tx_cancel),
    ("SearchFiles", "tas", tx_search_files),
    ("SearchNames", "tas", tx_search_names),
    ("SearchDetails", "tas", tx_search_names),
    ("SearchGroups", "tas", tx_search_groups),
    ("Resolve", "tas", tx_resolve),
    ("GetPackages", "t", tx_get_packages),
    ("GetDetails", "as", tx_get_details),
    ("GetFiles", "as", tx_get_files),
    ("RemovePackages", "tasbb", tx_remove_packages),
    ("InstallPackages", "tas", tx_install_packages),
    ("InstallFiles", "tas", tx_install_files),
    ("RefreshCache", "b", tx_refresh_cache),
    ("GetUpdates", "t", tx_get_updates),
    ("GetUpdateDetail", "as", tx_get_update_detail),
    ("UpdatePackages", "tas", tx_update_packages),
]


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="o")
def CreateTransaction(self):
    path = f"/{state['next']}_mock"
    state["next"] += 1
    self.AddObject(
        path,
        TX_IFACE,
        {
            "Role": dbus.UInt32(0),
            "Status": dbus.UInt32(STATUS_WAIT),
            "LastPackage": "",
            "Uid": dbus.UInt32(os.getuid()),
            "Sender": "",
            "Percentage": dbus.UInt32(101),
            "AllowCancel": True,
            "CallerActive": True,
            "ElapsedTime": dbus.UInt32(0),
            "RemainingTime": dbus.UInt32(0),
            "Speed": dbus.UInt32(0),
            "DownloadSizeRemaining": dbus.UInt64(0),
            "TransactionFlags": dbus.UInt64(0),
        },
        [],
    )
    tx = mockobject.objects[path]
    for name, sig, fn in TX_METHODS:
        tx.AddMethod(TX_IFACE, name, sig, "", fn)
    return dbus.ObjectPath(path)
