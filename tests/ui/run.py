#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""UI tests: drives a headless XeonDesktop session with xd-inject and checks
what happened (windows, files, settings, screenshots).

    tests/ui/run.py --build-dir build [SCENARIO...]
    tests/ui/run.py --list

Each scenario gets a session of its own, set up as the hand-run tests were:
a private mount namespace (unshare) in which a scratch home stands in for
the real one, scratch XDG directories and runtime dir, dbus-run-session for
the session bus, the real system bus cut off, and xd-compositor on the
headless wlroots backend with the pixman renderer and --allow-virtual-input.
Nothing reaches the user's own desktop, files or system services.

Every process of a session carries its runtime dir in XDG_RUNTIME_DIR, and
teardown ends exactly those processes (never anything found by name), so
two runs at once can't touch each other and a failed or interrupted run
leaves nothing behind. (A runner killed outright takes its session down
with it; the next run removes the scratch tree.) --keep DIR saves a failed
scenario's screenshots and log there first.

Checks are deliberately loose: a window with a title exists or is focused,
a file appeared, a settings key changed, part of the screen changed or has
a colour. No stored reference images.

Exit status: 0 passed, 1 failed, 77 skipped (no unshare, grim, headless
backend or python3-gobject), as meson expects.
"""

import argparse
import configparser
import ctypes
import importlib.util
import os
import pwd
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback

SKIP = 77
SOURCE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Longest a whole scenario may take; meson's own timeout (ui suite) is above
# it, so the runner always gets to clean up itself.
SCENARIO_LIMIT = 150
WIDTH, HEIGHT = 1024, 768

CLASSIC_INI = "[Appearance]\nStyle=classic\n\n[StartMenu]\nStyle=classic\n"
AERO_INI = "[Appearance]\nStyle=aero\n\n[StartMenu]\nStyle=win7\n"
LUNA_INI = "[Appearance]\nStyle=luna\n\n[StartMenu]\nStyle=xp\n"

SCENARIOS = {}


def scenario(desktop_ini=CLASSIC_INI, requires=()):
    """Registers a scenario: a function of the session (Session), run with
    that desktop.ini; it's skipped without the Python modules it requires."""

    def register(fn):
        SCENARIOS[fn.__name__] = (fn, desktop_ini, requires)
        return fn

    return register


class Failure(Exception):
    pass


# ---- outer side: the session's lifetime -------------------------------------------


def unusable(build):
    """Why the suite can't run here, or None."""
    for tool in ("unshare", "grim", "dbus-run-session", "timeout"):
        if shutil.which(tool) is None:
            return f"{tool} is not installed"
    for exe in ("xd-compositor", "xd-shell", "xd-inject", "xd-windows"):
        if not os.access(os.path.join(build, exe), os.X_OK):
            return f"{exe} is not built in {build}"
    try:
        import gi

        gi.require_version("GdkPixbuf", "2.0")
        from gi.repository import GdkPixbuf  # noqa: F401
    except (ImportError, ValueError) as e:
        return f"python-gobject with GdkPixbuf is missing ({e})"
    probe = subprocess.run(
        ["unshare", "-r", "-m", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )
    if probe.returncode != 0:
        return "unprivileged user and mount namespaces aren't available: " + probe.stderr.decode(
            errors="replace"
        ).strip()
    return None


def session_pids(runtime):
    """Our session's processes: ours, with XDG_RUNTIME_DIR=runtime exactly."""
    want = b"XDG_RUNTIME_DIR=" + runtime.encode()
    uid = os.getuid()
    me = os.getpid()
    pids = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit() or int(entry) == me:
            continue
        try:
            if os.stat(f"/proc/{entry}").st_uid != uid:
                continue
            with open(f"/proc/{entry}/environ", "rb") as f:
                if want in f.read().split(b"\0"):
                    pids.append(int(entry))
        except OSError:
            pass  # gone, or not ours to read
    return pids


def end_session(runtime, grace):
    """Ends every process of the session, politely first if grace > 0."""
    for sig, wait in ((signal.SIGTERM, grace), (signal.SIGKILL, 2.0)):
        if wait <= 0:
            continue
        pids = session_pids(runtime)
        for pid in pids:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + wait
        while pids and time.monotonic() < deadline:
            time.sleep(0.1)
            pids = session_pids(runtime)
    return session_pids(runtime)


def remove_tree(base):
    """Deletes the scratch tree, unless something is still mounted in it (the
    mounts are only in the session's own namespace, but make sure)."""
    with open("/proc/self/mounts") as f:
        for line in f:
            target = line.split()[1].encode().decode("unicode_escape")
            if target == base or target.startswith(base + "/"):
                print(f"run.py: {target} is still mounted; leaving {base}", file=sys.stderr)
                return
    shutil.rmtree(base, ignore_errors=True)


SCRATCH_PREFIX = "xd-ui-"


def scratch_parent(home):
    """Where scratch trees go: the temp dir, unless that's inside the home the
    session hides (then /tmp)."""
    tmp = os.path.realpath(tempfile.gettempdir())
    if tmp == home or tmp.startswith(home + "/"):
        tmp = "/tmp"
    return tmp


def reap_stale(parent):
    """Removes what a run killed outright (SIGKILL) left: our scratch trees
    older than any run can last, once nothing runs in them."""
    for name in os.listdir(parent):
        base = os.path.join(parent, name)
        try:
            st = os.lstat(base)
        except OSError:
            continue
        if (
            name.startswith(SCRATCH_PREFIX)
            and st.st_uid == os.getuid()
            and os.path.isdir(base)
            and not os.path.islink(base)
            and time.time() - st.st_mtime > SCENARIO_LIMIT + 60
            and not session_pids(os.path.join(base, "run"))
        ):
            remove_tree(base)


# The XDG directories, inside the scratch home: the Recycle Bin must be on
# the same mount as the files sent to it.
XDG_DIRS = {
    "XDG_CONFIG_HOME": ".config",
    "XDG_DATA_HOME": ".local/share",
    "XDG_STATE_HOME": ".local/state",
    "XDG_CACHE_HOME": ".cache",
}


def populate(base, home_src, desktop_ini):
    for d in XDG_DIRS.values():
        os.makedirs(os.path.join(home_src, d))
    cfg = os.path.join(home_src, XDG_DIRS["XDG_CONFIG_HOME"], "xeondesktop")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "desktop.ini"), "w") as f:
        f.write(desktop_ini)
    with open(os.path.join(cfg, "monitors.ini"), "w") as f:
        f.write(f"[HEADLESS-1]\nEnabled=true\nWidth={WIDTH}\nHeight={HEIGHT}\nScale=1\n")
    apps = os.path.join(home_src, XDG_DIRS["XDG_DATA_HOME"], "applications")
    os.makedirs(apps)
    src_apps = os.path.join(SOURCE_ROOT, "data", "applications")
    for name in os.listdir(src_apps):
        if name.endswith(".desktop"):
            shutil.copy(os.path.join(src_apps, name), apps)
    os.makedirs(os.path.join(base, "shots"))
    for d in ("Desktop", "Documents/Letters", "Pictures", "Music", "Downloads"):
        os.makedirs(os.path.join(home_src, d))
    with open(os.path.join(home_src, "Documents", "Notes.txt"), "w") as f:
        f.write("Shopping list\n")


def run_scenario(name, build, keep, keep_all=False):
    """Runs one scenario in a session of its own; True if it passed."""
    desktop_ini = SCENARIOS[name][1]
    home = os.path.realpath(pwd.getpwuid(os.getuid()).pw_dir)
    base = tempfile.mkdtemp(prefix=SCRATCH_PREFIX, dir=scratch_parent(home))
    runtime = os.path.join(base, "run")
    os.mkdir(runtime, 0o700)
    home_src = os.path.join(base, "home")
    populate(base, home_src, desktop_ini)

    env = {
        k: v
        for k, v in os.environ.items()
        if k
        not in (
            "WAYLAND_DISPLAY",
            "DISPLAY",
            "DBUS_SESSION_BUS_ADDRESS",
            "XDG_SESSION_ID",
            "XDG_SEAT",
            "XDG_VTNR",
            "SWAYSOCK",
            "HYPRLAND_INSTANCE_SIGNATURE",
            "PULSE_SERVER",
        )
    }
    env.update(
        {
            "HOME": home,
            "XDG_RUNTIME_DIR": runtime,
            **{k: os.path.join(home, d) for k, d in XDG_DIRS.items()},
            "GIO_USE_VFS": "local",
            "NO_AT_BRIDGE": "1",
            "ASAN_OPTIONS": "detect_leaks=0",
            # Never the real system bus (polkit, UDisks, NetworkManager...):
            # a scenario that needs one starts a dbusmock bus of its own.
            "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=" + os.path.join(runtime, "no-system-bus"),
            # Never the real speakers: sounds are logged, not played.
            "XD_SOUND_TRACE": os.path.join(runtime, "sounds.log"),
            "WLR_BACKENDS": "headless",
            "WLR_RENDERER": "pixman",
            "WLR_HEADLESS_OUTPUTS": "1",
            "PATH": build + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"),
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
        }
    )

    # In the new namespace (root there only to mount), the scratch home is
    # bound over the real one, with the source and build trees bound back
    # in where they lie inside it; then back to our own uid.
    binds = []
    for path in sorted({SOURCE_ROOT, build}):
        path = os.path.realpath(path)
        if path.startswith(home + "/"):
            rel = os.path.relpath(path, home)
            inside = os.path.join(home_src, rel)
            binds.append(f"mkdir -p {shlex.quote(inside)}")
            binds.append(f"mount --bind {shlex.quote(path)} {shlex.quote(inside)}")
    binds.append(f"mount --rbind {shlex.quote(home_src)} {shlex.quote(home)}")
    binds.append(
        f'exec unshare --user --map-user={os.getuid()} --map-group={os.getgid()} "$0" "$@"'
    )
    script = "set -e\n" + "\n".join(binds)
    cmd = [
        "unshare", "-r", "-m", "sh", "-c", script,
        "timeout", "-k", "5", str(SCENARIO_LIMIT),
        "dbus-run-session", "--",
        sys.executable, os.path.abspath(__file__), "--inner", name,
        "--build-dir", build, "--base", base, "--outer", str(os.getpid()),
    ]  # fmt: skip

    log_path = os.path.join(base, "log")
    passed = False
    proc = None
    try:
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                rc = proc.wait(timeout=SCENARIO_LIMIT + 15)
            except subprocess.TimeoutExpired:
                rc = None
        passed = rc == 0
        with open(log_path, "rb") as f:
            out = f.read().decode(errors="replace")
        if not passed:
            sys.stdout.write(out)
            if rc is None:
                print(f"{name}: timed out")
        else:
            sys.stdout.write("".join(l + "\n" for l in out.splitlines() if l.startswith("ui: ")))
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        left = end_session(runtime, grace=3.0 if passed else 1.0)
        if left:
            print(f"run.py: processes {left} wouldn't end; leaving {base}", file=sys.stderr)
        else:
            if keep and (not passed or keep_all):
                dest = os.path.join(keep, f"{name}-{os.getpid()}")
                shutil.rmtree(dest, ignore_errors=True)
                shutil.copytree(os.path.join(base, "shots"), dest)
                shutil.copy(log_path, dest)
                print(f"{name}: screenshots and log are in {dest}")
            remove_tree(base)
    return passed


def outer(args):
    build = os.path.realpath(args.build_dir)
    names = args.scenarios or list(SCENARIOS)
    for n in names:
        if n not in SCENARIOS:
            print(f"run.py: no scenario {n} (try --list)", file=sys.stderr)
            return 2
    why = unusable(build)
    if why is not None:
        print(f"skipped: {why}")
        return SKIP
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
    reap_stale(scratch_parent(os.path.realpath(pwd.getpwuid(os.getuid()).pw_dir)))

    # meson ends a test with SIGTERM: turn it into an exception so the
    # finally blocks above end the session.
    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    failed = []
    skipped = []
    try:
        for n in names:
            missing = [m for m in SCENARIOS[n][2] if importlib.util.find_spec(m) is None]
            if missing:
                print(f"SKIP {n} (needs the Python module {', '.join(missing)})", flush=True)
                skipped.append(n)
                continue
            start = time.monotonic()
            ok = run_scenario(n, build, args.keep, args.keep_all)
            print(f"{'PASS' if ok else 'FAIL'} {n} ({time.monotonic() - start:.1f}s)", flush=True)
            if not ok:
                failed.append(n)
    except KeyboardInterrupt:
        print("interrupted")
        return 1
    if failed:
        print("failed: " + " ".join(failed))
        return 1
    return SKIP if len(skipped) == len(names) else 0


# ---- inner side: inside the session ---------------------------------------------

PR_SET_PDEATHSIG = 1


def die_with_parent():
    """For Popen: the child gets SIGKILL if this runner dies, whatever kills it."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)


class Shot:
    """A screenshot: pixels by logical coordinates (scale 1)."""

    def __init__(self, path):
        import gi

        gi.require_version("GdkPixbuf", "2.0")
        from gi.repository import GdkPixbuf

        self.path = path
        pb = GdkPixbuf.Pixbuf.new_from_file(path)
        self.width, self.height = pb.get_width(), pb.get_height()
        self.stride = pb.get_rowstride()
        self.channels = pb.get_n_channels()
        self.data = pb.get_pixels()

    def pixel(self, x, y):
        i = y * self.stride + x * self.channels
        return tuple(self.data[i : i + 3])

    def differs(self, other, rect, step=4, tolerance=8):
        """The share (0..1) of sampled pixels in rect that differ from other."""
        x0, y0, w, h = rect
        total = changed = 0
        for y in range(y0, y0 + h, step):
            for x in range(x0, x0 + w, step):
                total += 1
                a, b = self.pixel(x, y), other.pixel(x, y)
                if max(abs(p - q) for p, q in zip(a, b)) > tolerance:
                    changed += 1
        return changed / max(total, 1)

    def changed_box(self, other, rect=(0, 0, WIDTH, HEIGHT), step=2, tolerance=8):
        """The bounding box (x, y, w, h) of what differs from other within
        rect, or None."""
        x0, y0, w, h = rect
        xs, ys = [], []
        for y in range(y0, y0 + h, step):
            for x in range(x0, x0 + w, step):
                a, b = self.pixel(x, y), other.pixel(x, y)
                if max(abs(p - q) for p, q in zip(a, b)) > tolerance:
                    xs.append(x)
                    ys.append(y)
        if not xs:
            return None
        return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)


class Session:
    """What a scenario works with, inside the session."""

    def __init__(self, build, base):
        self.build = build
        self.base = base
        self.home = os.environ["HOME"]
        self.config = os.path.join(os.environ["XDG_CONFIG_HOME"], "xeondesktop")
        self.data = os.environ["XDG_DATA_HOME"]
        self.shots = 0
        self.children = []

    def log(self, msg):
        print(f"ui: {msg}", flush=True)

    def tool(self, name):
        return os.path.join(self.build, name)

    def launch(self, *cmd, env=None):
        """Starts a program in the session, in the background."""
        e = dict(os.environ)
        if env:
            e.update(env)
        p = subprocess.Popen(list(cmd), env=e, preexec_fn=die_with_parent)
        self.children.append(p)
        return p

    def inject(self, *steps):
        steps = [str(s) for s in steps]
        r = subprocess.run([self.tool("xd-inject"), *steps], stderr=subprocess.PIPE, timeout=60)
        if r.returncode != 0:
            raise Failure(f"xd-inject {' '.join(steps)}: {r.stderr.decode().strip()}")

    def windows(self):
        """The session's windows: dicts of app_id, title and states (a set)."""
        r = subprocess.run(
            [self.tool("xd-windows")], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10
        )
        if r.returncode != 0:
            raise Failure("xd-windows: " + r.stderr.decode().strip())
        out = []
        for line in r.stdout.decode(errors="replace").splitlines():
            app_id, title, states = line.split("\t")
            out.append(
                {"app_id": app_id, "title": title, "states": set(states.split(",")) - {"-"}}
            )
        return out

    def window(self, title=None, app_id=None):
        """The first window matching title and/or app_id, or None."""
        for w in self.windows():
            if (title is None or w["title"] == title) and (app_id is None or w["app_id"] == app_id):
                return w
        return None

    def wait(self, check, what, timeout=20.0):
        """Polls check() until it returns something true; fails after timeout."""
        deadline = time.monotonic() + timeout
        while True:
            v = check()
            if v:
                return v
            if time.monotonic() > deadline:
                windows = "?"
                if "WAYLAND_DISPLAY" in os.environ:
                    self.shot("timeout")
                    windows = self.windows()
                raise Failure(f"timed out waiting for {what}; windows: {windows}")
            time.sleep(0.2)

    def wait_window(self, title=None, app_id=None, state=None, gone=False, timeout=20.0):
        def check():
            w = self.window(title, app_id)
            if gone:
                return w is None
            return w if w is not None and (state is None or state in w["states"]) else None

        what = f"window {title or app_id!r}" + (f" {state}" if state else "")
        return self.wait(check, what + (" to close" if gone else ""), timeout)

    def shot(self, label):
        self.shots += 1
        path = os.path.join(self.base, "shots", f"{self.shots:02d}-{label}.png")
        r = subprocess.run(["grim", path], stderr=subprocess.PIPE, timeout=30)
        if r.returncode != 0:
            raise Failure("grim: " + r.stderr.decode().strip())
        return Shot(path)

    def settle(self, label="settled", timeout=10.0):
        """Waits until two screenshots in a row match (animations, fades and
        redraws over) and returns the last."""
        prev = self.shot(label)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            time.sleep(0.3)
            cur = self.shot(label)
            if cur.differs(prev, (0, 0, WIDTH, HEIGHT), step=8, tolerance=0) == 0:
                return cur
            prev = cur
        return prev

    def ini(self, name="desktop.ini"):
        c = configparser.ConfigParser(interpolation=None, strict=False)
        c.optionxform = str
        c.read(os.path.join(self.config, name))
        return c

    def check(self, cond, msg):
        if not cond:
            self.shot("failed")
            raise Failure(msg)
        self.log("ok: " + msg)


def inner(args):
    build = os.path.realpath(args.build_dir)
    fn = SCENARIOS[args.inner][0]
    runtime = os.environ["XDG_RUNTIME_DIR"]
    s = Session(build, args.base)

    # If the runner outside was killed outright, stop: what we started dies
    # with us (die_with_parent), and its next run removes the scratch tree.
    def watchdog():
        while os.path.exists(f"/proc/{args.outer}"):
            time.sleep(0.5)
        os._exit(1)

    if args.outer:
        threading.Thread(target=watchdog, daemon=True).start()
    comp = subprocess.Popen(
        [os.path.join(build, "xd-compositor"), "--allow-virtual-input"],
        preexec_fn=die_with_parent,
    )
    rc = 1
    try:
        socket = os.path.join(runtime, "wayland-0")
        s.wait(lambda: os.path.exists(socket) or comp.poll() is not None, "the compositor", 30)
        if comp.poll() is not None:
            raise Failure(f"xd-compositor exited ({comp.returncode})")
        os.environ["WAYLAND_DISPLAY"] = "wayland-0"
        # The shell is up once the taskbar is drawn: the bottom row isn't the
        # black of an empty output any more.
        s.wait(lambda: s.shot("startup").pixel(WIDTH // 2, HEIGHT - 4) != (0, 0, 0),
               "the taskbar", 30)  # fmt: skip
        s.settle("startup")
        fn(s)
        s.log("passed")
        rc = 0
    except Failure as e:
        print(f"FAILED: {e}", flush=True)
    except Exception:
        traceback.print_exc()
    finally:
        for p in s.children + [comp]:
            if p.poll() is None:
                p.terminate()
        for p in s.children + [comp]:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    return rc


# ---- scenarios -------------------------------------------------------------------


@scenario()
def start_menu_run(s):
    """Ctrl+Esc opens the Start menu; its Run... opens the Run dialog, which
    starts a program."""
    before = s.settle("desktop")
    s.inject("key", "ctrl+Escape")
    menu = s.settle("start-menu")
    # The classic Start menu rises from the Start button, bottom left.
    s.check(menu.differs(before, (0, 400, 160, 320)) > 0.2, "Ctrl+Esc opened the Start menu")
    # From nothing selected, Up goes to Shut Down, then Log Off, then Run.
    s.inject("key", "Up", "key", "Up", "key", "Up", "key", "Return")
    s.wait_window(title="Run", state="activated")
    s.log("ok: Run... opened the Run dialog")
    s.inject("type", "xd-control", "key", "Return")
    s.wait_window(title="Run", gone=True)
    s.wait_window(title="Control Panel", state="activated")
    s.log("ok: Run started Control Panel")


def two_windows(s):
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    s.launch("xd-run")
    s.wait_window(title="Run", state="activated")


@scenario()
def alt_tab(s):
    """Alt+Tab switches to the window used before, and back."""
    two_windows(s)
    s.inject("key", "alt+Tab")
    s.wait_window(title="Control Panel", state="activated")
    s.check("activated" not in s.window(title="Run")["states"], "Alt+Tab switched to Control Panel")
    s.inject("key", "alt+Tab")
    s.wait_window(title="Run", state="activated")
    s.log("ok: Alt+Tab switched back to Run")


@scenario()
def maximize_minimize(s):
    """Win+Up maximizes; Win+Down restores, then minimizes."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    s.settle("restored")
    s.inject("key", "super+Up")
    s.wait_window(title="Control Panel", state="maximized")
    s.log("ok: Win+Up maximized")
    s.inject("key", "super+Down")
    s.wait(lambda: "maximized" not in s.window(title="Control Panel")["states"], "restore")
    s.log("ok: Win+Down restored")
    s.inject("key", "super+Down")
    s.wait_window(title="Control Panel", state="minimized")
    s.log("ok: Win+Down again minimized")


@scenario()
def alt_f4(s):
    """Alt+F4 closes the focused window."""
    two_windows(s)
    s.inject("key", "alt+F4")
    s.wait_window(title="Run", gone=True)
    s.check(s.window(title="Control Panel") is not None, "Alt+F4 closed only the focused window")


@scenario(AERO_INI)
def aero_snap(s):
    """Win+Left and Win+Right snap a window to half the screen (Aero)."""
    empty = s.settle("desktop")
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    work = HEIGHT - 60  # above the taskbar
    left = (8, 40, WIDTH // 2 - 40, work - 80)
    right = (WIDTH // 2 + 32, 40, WIDTH // 2 - 40, work - 80)

    s.inject("key", "super+Left")
    shot = s.settle("snapped-left")
    s.check(shot.differs(empty, left) > 0.9, "Win+Left: the window covers the left half")
    s.check(shot.differs(empty, right) < 0.02, "Win+Left: the right half shows the desktop")

    s.inject("key", "super+Right")
    s.inject("key", "super+Right")  # the first goes back to where it was
    shot = s.settle("snapped-right")
    s.check(shot.differs(empty, right) > 0.9, "Win+Right: the window covers the right half")
    s.check(shot.differs(empty, left) < 0.02, "Win+Right: the left half shows the desktop")
    s.check("maximized" not in s.window(title="Control Panel")["states"], "snapped, not maximized")


@scenario()
def file_manager(s):
    """Explorer: File > New > Folder, rename in place, Delete to the Recycle
    Bin."""
    docs = os.path.join(s.home, "Documents")
    s.launch("xd-files", docs)
    win = s.wait_window(app_id="org.xeondesktop.Files", state="activated")
    s.log(f"Explorer is up: {win['title']}")
    s.settle("explorer")
    new = os.path.join(docs, "New Folder")
    s.inject("key", "alt+f", "sleep", 300, "key", "w", "sleep", 300, "key", "f")
    s.wait(lambda: os.path.isdir(new), "File > New > Folder to make New Folder")
    s.log("ok: File > New > Folder made New Folder")
    # The new folder's name is being edited, all selected.
    s.settle("renaming")
    s.inject("type", "Reports", "key", "Return")
    renamed = os.path.join(docs, "Reports")
    s.wait(lambda: os.path.isdir(renamed) and not os.path.exists(new), "the rename")
    s.log("ok: renamed to Reports")
    s.settle("renamed")
    s.inject("key", "Delete")
    s.settle("confirm")
    s.inject("key", "Return")  # Yes, send it to the Recycle Bin
    trash = os.path.join(s.data, "Trash", "files", "Reports")
    s.wait(lambda: not os.path.exists(renamed) and os.path.isdir(trash), "the Recycle Bin")
    info = os.path.join(s.data, "Trash", "info", "Reports.trashinfo")
    s.check(os.path.exists(info), "Delete sent Reports to the Recycle Bin")


@scenario()
def thumbnails(s):
    """Thumbnails view: pictures with no thumbnail in the cache get one from
    xd-thumbnailer (sandboxed), written to the freedesktop cache and drawn
    as each arrives."""
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    pics = os.path.join(s.home, "Pictures")
    colours = {"Red": (0xC8, 0x20, 0x28), "Green": (0x20, 0xA8, 0x40), "Blue": (0x28, 0x40, 0xC8)}
    for name, (r, g, b) in colours.items():
        pb = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, False, 8, 400, 300)
        pb.fill((r << 24) | (g << 16) | (b << 8) | 0xFF)
        pb.savev(os.path.join(pics, name + ".png"), "png", [], [])
    s.launch("xd-files", "--view=thumbnails", pics)
    s.wait_window(app_id="org.xeondesktop.Files", state="activated")
    # The default thumbnail size (96) is the spec's "normal" (128).
    cache = os.path.join(os.environ["XDG_CACHE_HOME"], "thumbnails", "normal")
    s.wait(lambda: os.path.isdir(cache) and len(os.listdir(cache)) == len(colours),
           "xd-thumbnailer to fill the cache")  # fmt: skip
    s.log("ok: three thumbnails in the cache")
    shot = s.settle("thumbnails")

    def area(rgb):
        n = 0
        for y in range(0, HEIGHT, 2):
            for x in range(0, WIDTH, 2):
                if max(abs(p - q) for p, q in zip(shot.pixel(x, y), rgb)) <= 6:
                    n += 1
        return n

    # A thumbnail 88 px across, scaled from 4:3: about 88 x 66 px, sampled
    # every other pixel both ways.
    for name, rgb in colours.items():
        n = area(rgb)
        s.check(n > 1000, f"{name}.png is drawn as its thumbnail ({n} samples)")


@scenario()
def move_after_left_resize(s):
    """A window resized from its left edge and then moved stays where it's
    dropped when it next redraws (the resize's anchor used to pull it back
    at every commit, and fought the drag)."""
    empty = s.settle("desktop")
    s.launch("xd-files", os.path.join(s.home, "Documents"))
    s.wait_window(app_id="org.xeondesktop.Files", state="activated")
    area = (0, 0, WIDTH, HEIGHT - 40)  # above the taskbar
    x, y, w, h = s.settle("explorer").changed_box(empty, area)
    mid = y + h // 2
    s.inject("move", x + 1, mid, "down", "sleep", 100, "move", x + 40, mid, "sleep", 100,
             "move", x + 80, mid, "sleep", 300, "up", "sleep", 500)
    rx, ry, rw, rh = s.settle("resized").changed_box(empty, area)
    s.check(abs(rx - (x + 80)) <= 4 and abs(rx + rw - (x + w)) <= 4,
            f"the left edge followed, the right edge stayed ({rx},{ry} {rw}x{rh})")
    cx, cy = rx + rw // 2, ry + 10  # on the caption
    s.inject("move", cx, cy, "down", "sleep", 100, "move", cx + 50, cy + 30, "sleep", 100,
             "move", cx + 100, cy + 60, "sleep", 300, "up", "sleep", 500)
    # Make it redraw: each selection change is a commit.
    s.inject("key", "Down", "sleep", 200, "key", "Up", "sleep", 200, "key", "Down")
    mx, my, _, _ = s.settle("moved").changed_box(empty, area)
    s.check(abs(mx - (rx + 100)) <= 4 and abs(my - (ry + 60)) <= 4,
            f"the window stayed where it was dropped ({mx},{my}, expected {rx + 100},{ry + 60})")


@scenario()
def display_style(s):
    """Display Properties: picking another style and Apply writes it."""
    s.check(s.ini()["Appearance"]["Style"] == "classic", "the session starts in the classic style")
    s.launch("xd-desk", "--page", "2")
    s.wait_window(title="Display Properties", state="activated")
    s.settle("appearance")
    # Focus starts on the tabs; the first control after them is the style list.
    s.inject("key", "Tab", "sleep", 200, "key", "Down", "sleep", 300)
    s.settle("style-changed")
    s.inject("key", "Return")  # OK: apply and close
    s.wait_window(title="Display Properties", gone=True)

    def style():
        c = s.ini()
        return c.get("Appearance", "Style", fallback="") not in ("", "classic")

    s.wait(style, "desktop.ini to change")
    s.log(f"ok: Style is now {s.ini()['Appearance']['Style']}")

    # The shell follows: the classic grey taskbar turns Luna blue.
    def blue_taskbar():
        r, g, b = s.shot("taskbar").pixel(WIDTH // 2, HEIGHT - 10)
        return b > r + 60

    s.wait(blue_taskbar, "the taskbar to turn blue")
    s.log("ok: the taskbar took the new style")


@scenario()
def desktop_menu(s):
    """A right-click on the desktop opens its menu; Esc closes it."""
    before = s.settle("desktop")
    s.inject("move", 600, 300, "click", "right")
    menu = s.settle("menu")
    s.check(menu.differs(before, (600, 300, 120, 150)) > 0.3, "right-click opened the desktop menu")
    s.inject("key", "Escape")
    after = s.settle("closed")
    s.check(after.differs(before, (600, 300, 120, 150)) < 0.02, "Esc closed it")


def start_menu_closes_on_desktop(s, region):
    """Ctrl+Esc opens the Start menu; a click on the desktop closes it (the
    desktop is the shell's own surface, so the compositor's popup grab
    doesn't end it)."""
    before = s.settle("desktop")
    s.inject("key", "ctrl+Escape")
    menu = s.settle("start-menu")
    s.check(menu.differs(before, region) > 0.2, "Ctrl+Esc opened the Start menu")
    s.inject("move", 850, 120, "click")
    after = s.settle("closed")
    s.check(after.differs(before, region) < 0.02, "a click on the desktop closed it")


@scenario()
def classic_start_closes_on_desktop(s):
    """The Windows 2000 Start menu closes on a click on the desktop."""
    start_menu_closes_on_desktop(s, (0, 400, 160, 320))


@scenario(LUNA_INI)
def xp_start_closes_on_desktop(s):
    """The Windows XP Start menu closes on a click on the desktop."""
    start_menu_closes_on_desktop(s, (0, 300, 400, 420))


@scenario(LUNA_INI)
def combo_closes_on_tab(s):
    """An open drop-down list closes when the dialog is clicked elsewhere
    (here, another tab); it used to stay up over the other tab."""
    empty = s.settle("desktop")
    s.launch("xd-taskbar")
    s.wait_window(title="Taskbar and Start Menu Properties", state="activated")
    x, y, _, _ = s.settle("dialog").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.inject("move", x + 97, y + 48, "click", "sleep", 400)  # the Start Menu tab
    tab = s.settle("start-menu-tab")
    s.inject("move", x + 233, y + 101, "click", "sleep", 400)  # the style list's arrow
    listed = s.settle("list-open")
    below = (x + 24, y + 112, 200, 50)  # where the list drops down
    s.check(listed.differs(tab, below) > 0.2, "the drop-down list opened")
    s.inject("move", x + 38, y + 48, "click", "sleep", 400)  # the Taskbar tab
    closed = s.settle("list-closed")
    s.check(closed.differs(tab, below) < 0.02, "a click elsewhere in the dialog closed the list")


@scenario(LUNA_INI)
def taskbar_only(s):
    """xd-shell --taskbar-only, as under another compositor: no desktop
    (wallpaper, icons), and `xd-shell --toggle-start-menu` (a key binding's)
    opens and closes the Start menu."""
    import gi

    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib

    with_desktop = s.settle("desktop")
    # The session's shell, by the name it owns, makes way.
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    pid = bus.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "GetConnectionUnixProcessID", GLib.Variant("(s)", ("org.xeondesktop.Shell",)),
        GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, 5000, None,
    ).unpack()[0]  # fmt: skip
    os.kill(pid, signal.SIGTERM)
    s.wait(lambda: not os.path.exists(f"/proc/{pid}"), "the session's shell to exit")
    # A XEONDESKTOP_PID that isn't the compositor's: as if left over in the
    # user manager from an earlier XeonDesktop session.
    s.launch(s.tool("xd-shell"), "--taskbar-only", env={"XEONDESKTOP_PID": "1"})
    s.wait(lambda: s.shot("taskbar-only").pixel(WIDTH // 2, HEIGHT - 4) != (0, 0, 0), "the taskbar")
    bare = s.settle("taskbar-only")
    # The desktop icons, top left, are gone: what's there is the
    # compositor's own background, the same everywhere.
    icons = (0, 0, 80, 210)
    s.check(bare.differs(with_desktop, icons) > 0.05, "no desktop icons")
    s.check(bare.pixel(38, 170) == bare.pixel(WIDTH // 2, HEIGHT // 2), "nothing drawn over the output")
    s.check(bare.differs(with_desktop, (0, HEIGHT - 30, 80, 30)) < 0.05, "the Start button is there")

    def toggle():
        r = subprocess.run([s.tool("xd-shell"), "--toggle-start-menu"], timeout=10)
        if r.returncode != 0:
            raise Failure(f"xd-shell --toggle-start-menu exited {r.returncode}")

    region = (0, 300, 400, 420)
    toggle()
    menu = s.settle("start-menu")
    s.check(menu.differs(bare, region) > 0.2, "--toggle-start-menu opened the Start menu")
    toggle()
    s.check(s.settle("closed").differs(bare, region) < 0.02, "and again closed it")
    toggle()
    s.check(s.settle("reopened").differs(bare, region) > 0.2, "it opens again")
    s.inject("key", "Escape")
    s.check(s.settle("escaped").differs(bare, region) < 0.02, "Esc closed it: it had the keyboard")


@scenario()
def combo_list_scrolls(s):
    """A drop-down list longer than its eight rows has a scroll bar: its
    arrow scrolls the list, its thumb drags to the end, and the last item
    (Display Properties' 23rd colour scheme) can be picked; Home, End and
    the page keys move through it too."""
    s.launch("xd-desk", "--page", "2")
    s.wait_window(title="Display Properties", state="activated")
    # Focus starts on the tabs; Tab goes to the style list, then the colour
    # schemes.
    s.inject("key", "Tab", "sleep", 200, "key", "Tab", "sleep", 200)
    focused = s.settle("scheme-focused")
    s.inject("key", "alt+Down")
    listed = s.settle("list-open")
    box = listed.changed_box(focused, (0, 0, WIDTH, HEIGHT - 40), step=1)
    s.check(box is not None and box[3] > 138, f"Alt+Down opened the list ({box})")
    # The list is as wide as the combo box (the arrow button changed too),
    # 8 rows of 17px and a border, at the bottom of what changed.
    lx, lw = box[0], box[2]
    ly = box[1] + box[3] - 138
    bar_x = lx + lw - 9  # the middle of the scroll bar

    # The scroll bar's down arrow scrolls the list by one: the rows move.
    s.inject("move", bar_x, ly + 138 - 8, "click", "sleep", 300)
    scrolled = s.settle("arrow-clicked")
    rows = (lx + 4, ly + 2, lw - 30, 134)
    s.check(scrolled.differs(listed, rows) > 0.02, "the down arrow scrolled the list")
    s.check(scrolled.differs(listed, (bar_x - 6, ly + 20, 12, 100)) > 0.0,
            "the scroll bar's thumb moved")

    # Drag the thumb from near the top to the bottom (past the list, still
    # over the dialog), then pick the last row.
    s.inject("move", bar_x, ly + 22, "down", "sleep", 100, "move", bar_x, ly + 80, "sleep", 100,
             "move", bar_x - 40, ly - 30, "sleep", 100,  # back up, over the dialog
             "move", bar_x, ly + 136, "sleep", 300, "up", "sleep", 300)
    s.settle("thumb-dragged")
    s.inject("move", lx + 40, ly + 138 - 9, "sleep", 200, "click", "sleep", 400)
    picked = s.settle("last-picked")
    combo_text = (lx + 4, ly - 18, lw - 30, 14)  # the combo box, just above the list
    s.check(picked.differs(focused, combo_text) > 0.02, "a click picked the last row")
    s.check(picked.differs(focused, rows) < 0.02, "and closed the list")

    # Keyboard: Home, then PageDown twice and End, then back to the last.
    s.inject("key", "alt+Down", "sleep", 300, "key", "Home", "sleep", 200)
    top = s.settle("home")
    s.inject("key", "Next", "sleep", 200, "key", "Next", "sleep", 200)
    paged = s.settle("paged")
    s.check(paged.differs(top, rows) > 0.02, "PageDown moved through the list")
    s.inject("key", "End", "sleep", 200, "key", "Return", "sleep", 300)
    s.inject("key", "Return")  # OK: apply and close
    s.wait_window(title="Display Properties", gone=True)
    s.wait(lambda: s.ini().get("Appearance", "Scheme", fallback="") == "windows-standard",
           "desktop.ini to name the last scheme")
    s.log("ok: the last scheme, Windows Standard, was picked and applied")


@scenario()
def desktop_submenu_over_window(s):
    """The desktop menu and its submenus draw over windows (a submenu, Arrange
    Icons By, once drew behind them)."""
    empty = s.settle("desktop")
    s.launch("xd-files", os.path.join(s.home, "Documents"))
    s.wait_window(app_id="org.xeondesktop.Files", state="activated")
    area = (0, 0, WIDTH, HEIGHT - 40)  # above the taskbar
    window = s.settle("explorer")
    x, y, w, h = window.changed_box(empty, area)
    s.log(f"Explorer at {x},{y} {w}x{h}")
    s.check(WIDTH - (x + w) > 150, "Explorer leaves room on its right for the menu")
    # Right-click on the desktop just right of the window: the menu fits
    # there, and its submenu, with no room on the right, flips left over
    # the window.
    s.inject("move", x + w + 10, y + 30, "click", "right")
    root = s.settle("menu")
    # (The window goes inactive too, so look only right of it.)
    box = root.changed_box(window, (x + w, 0, WIDTH - x - w, HEIGHT - 40))
    s.log(f"the menu drew at {box}")
    s.check(box is not None and box[3] > 100, "the menu opened")
    # Down selects Arrange Icons By, Right opens it.
    s.inject("key", "Down", "sleep", 200, "key", "Right")
    sub = s.settle("submenu")
    sbox = sub.changed_box(root, area)
    s.log(f"the submenu drew at {sbox}")
    s.check(sbox is not None and sbox[0] < x + w - 60 and sbox[3] > 60,
            "the submenu draws over the window")
    s.inject("key", "Escape", "sleep", 200, "key", "Escape")
    closed = s.settle("closed")
    s.check(closed.changed_box(empty, (x + w, 0, WIDTH - x - w, HEIGHT - 40)) is None,
            "Esc closed both")


@scenario(requires=("dbusmock",))
def disk_management_menu(s):
    """Disk Management (on a dbusmock UDisks2): the Action menu opens with
    its items, and the window still responds after (it once froze)."""
    from dbusmock import DBusTestCase

    # A private system bus for this session's programs from here on.
    DBusTestCase.start_system_bus()
    try:
        mock = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "udisks2.py")
        proc, _ = DBusTestCase.spawn_server_template(
            mock, {}, stdout=subprocess.DEVNULL, system_bus=True
        )
        s.children.append(proc)
        s.launch("xd-diskmgmt")
        s.wait_window(title="Disk Management", state="activated")
        before = s.settle("disk-management")

        def action_menu(label):
            # F10 selects the menu bar and A opens Action, as in Windows.
            # (Alt+A opens File instead here; see the report.)
            s.inject("key", "F10", "sleep", 200, "key", "a")
            shot = s.settle(label)
            box = shot.changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
            # A menu of the selected volume's actions and Refresh: well over
            # a few items tall (it once came up empty).
            s.check(box is not None and box[3] > 100, f"the Action menu opened with items ({box})")
            s.inject("key", "Escape", "sleep", 200, "key", "Escape")
            closed = s.settle(label + "-closed")
            s.check(closed.changed_box(before, (0, 0, WIDTH, HEIGHT - 40)) is None, "Esc closed it")

        action_menu("action-menu")
        # Frozen, the window ignored all input: the menu wouldn't open again
        # and Alt+F4 wouldn't close it.
        action_menu("action-menu-again")
        s.inject("key", "alt+F4")
        s.wait_window(title="Disk Management", gone=True, timeout=10)
        s.log("ok: Disk Management still responds and closes")
    finally:
        DBusTestCase.tearDownClass()


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--build-dir", default=os.path.join(SOURCE_ROOT, "build"))
    p.add_argument("--keep", metavar="DIR", help="save failed scenarios' screenshots and log")
    p.add_argument("--keep-all", action="store_true", help="with --keep, passed ones' too")
    p.add_argument("--list", action="store_true", help="list the scenarios")
    p.add_argument("--names", action="store_true", help=argparse.SUPPRESS)  # for meson
    p.add_argument("--inner", help=argparse.SUPPRESS)
    p.add_argument("--base", help=argparse.SUPPRESS)
    p.add_argument("--outer", type=int, help=argparse.SUPPRESS)
    p.add_argument("scenarios", nargs="*")
    args = p.parse_args()
    if args.names:
        print("\n".join(SCENARIOS))
        return 0
    if args.list:
        for name, (fn, _, _) in SCENARIOS.items():
            print(f"{name}: {' '.join(fn.__doc__.split())}")
        return 0
    if args.inner:
        return inner(args)
    return outer(args)


if __name__ == "__main__":
    sys.exit(main())
