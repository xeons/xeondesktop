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
import urllib.parse

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
# Per scenario: "outputs", the monitors as (width, height, scale) side by
# side from (0, 0) (default: one WIDTH x HEIGHT at scale 1); "setup", a
# function of the session run before the compositor starts (a system bus
# for the programs it starts).
SCENARIO_OPTIONS = {}


def scenario(desktop_ini=CLASSIC_INI, requires=(), outputs=None, setup=None):
    """Registers a scenario: a function of the session (Session), run with
    that desktop.ini; it's skipped without the Python modules it requires."""

    def register(fn):
        SCENARIOS[fn.__name__] = (fn, desktop_ini, requires)
        SCENARIO_OPTIONS[fn.__name__] = {
            "outputs": outputs or [(WIDTH, HEIGHT, 1)],
            "setup": setup,
        }
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


def populate(base, home_src, desktop_ini, outputs):
    for d in XDG_DIRS.values():
        os.makedirs(os.path.join(home_src, d))
    cfg = os.path.join(home_src, XDG_DIRS["XDG_CONFIG_HOME"], "xeondesktop")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "desktop.ini"), "w") as f:
        f.write(desktop_ini)
    with open(os.path.join(cfg, "monitors.ini"), "w") as f:
        x = 0
        for i, (w, h, scale) in enumerate(outputs, 1):
            f.write(f"[HEADLESS-{i}]\nEnabled=true\nWidth={w}\nHeight={h}\nScale={scale}\n"
                    f"X={x}\nY=0\n\n")
            x += round(w / scale)
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
    outputs = SCENARIO_OPTIONS[name]["outputs"]
    populate(base, home_src, desktop_ini, outputs)

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
            # The taskbar clock holds still, so screenshots don't change
            # when a minute ends mid-scenario.
            "XD_CLOCK_FIXED": "12:00 PM",
            "WLR_BACKENDS": "headless",
            "WLR_RENDERER": "pixman",
            "WLR_HEADLESS_OUTPUTS": str(len(outputs)),
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
        # At scale 1, so pixels are logical coordinates on every monitor.
        r = subprocess.run(["grim", "-s", "1", path], stderr=subprocess.PIPE, timeout=30)
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
    setup = SCENARIO_OPTIONS[args.inner]["setup"]
    if setup is not None:
        setup(s)
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


@scenario(AERO_INI)
def superbar_thumbnails(s):
    """Aero: hovering a running program's button shows its windows'
    thumbnails above it; hovering one peeks at its window, clicking one
    brings it forward; the jump list pins a file and lists it under
    Pinned."""
    bar = (0, HEIGHT - 40, WIDTH, 40)
    before = s.settle("taskbar")
    for folder in ("Documents", "Pictures"):
        s.launch("xd-files", os.path.join(s.home, folder))
        s.wait_window(title=folder, state="activated")
    after = s.settle("two-windows")
    button = after.changed_box(before, bar)
    s.check(button is not None, f"the Files button lights up ({button})")
    cx, cy = button[0] + button[2] // 2, HEIGHT - 20

    s.inject("move", cx, cy, "sleep", 1000)
    shown = s.settle("thumbnails")
    above = (0, HEIGHT - 240, WIDTH, 200)
    popup = shown.changed_box(after, above)
    s.check(popup is not None and popup[2] > 300,
            f"two thumbnails show above the button ({popup})")  # fmt: skip

    # The first is Documents (the older window): hovered, it peeks at it.
    tx, ty = popup[0] + popup[2] // 4, popup[1] + popup[3] // 2
    s.inject("move", cx, popup[1] + popup[3] - 4, "move", tx, ty, "sleep", 1500)
    peek = s.settle("peek")
    s.check(peek.differs(shown, (0, 0, WIDTH, popup[1] - 8)) > 0.05,
            "hovering a thumbnail peeks at its window")  # fmt: skip
    s.inject("click")
    s.wait_window(title="Documents", state="activated")
    s.log("ok: clicking the thumbnail brought Documents forward")

    # The jump list: pin a recent file, which then shows under Pinned.
    notes = "file://" + os.path.join(s.home, "Documents", "Notes.txt")
    recent = os.path.join(s.data, "recently-used.xbel")
    with open(recent, "w") as f:
        f.write(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<xbel version="1.0" xmlns:bookmark="http://www.freedesktop.org/standards/'
            'desktop-bookmarks" xmlns:mime="http://www.freedesktop.org/standards/'
            'shared-mime-info">\n'
            f'<bookmark href="{notes}" added="2026-01-01T00:00:00Z" '
            'modified="2026-01-01T00:00:00Z" visited="2026-01-01T00:00:00Z"><info>'
            '<metadata owner="http://freedesktop.org"><mime:mime-type type="text/plain"/>'
            '<bookmark:applications><bookmark:application name="org.xeondesktop.Files" '
            "exec=\"'xd-files %u'\" modified=\"2026-01-01T00:00:00Z\" count=\"1\"/>"
            "</bookmark:applications></metadata></info></bookmark>\n</xbel>\n"
        )
    s.inject("move", WIDTH // 2, HEIGHT // 2, "sleep", 600)
    closed = s.settle("thumbnails-gone")
    s.inject("move", cx, cy, "sleep", 200, "click", "right", "sleep", 800)
    menu = s.settle("jump-list")
    jl = menu.changed_box(closed, above)
    s.check(jl is not None, f"the jump list opened ({jl})")
    # Its first item is the recent file; the pin button is at its right end.
    item_y = jl[1] + 6 + 24 + 13
    s.inject("move", jl[0] + 60, item_y, "move", jl[0] + jl[2] - 14, item_y, "sleep", 300,
             "click", "sleep", 800)  # fmt: skip
    pins = os.path.join(s.config, "jumplist-pinned")
    s.wait(lambda: os.path.exists(pins) and notes in open(pins).read(), "the pin to be saved")
    s.log("ok: the pin button pinned Notes.txt to the Files jump list")
    s.inject("key", "Escape")


def write_recent(s, uses):
    """recently-used.xbel saying xd-files opened each (uri, ISO time)."""
    marks = []
    for uri, when in uses:
        marks.append(
            f'<bookmark href="{uri}" added="{when}" modified="{when}" visited="{when}"><info>'
            '<metadata owner="http://freedesktop.org"><mime:mime-type type="text/plain"/>'
            '<bookmark:applications><bookmark:application name="org.xeondesktop.Files" '
            f"exec=\"'xd-files %u'\" modified=\"{when}\" count=\"1\"/>"
            "</bookmark:applications></metadata></info></bookmark>\n"
        )
    with open(os.path.join(s.data, "recently-used.xbel"), "w") as f:
        f.write(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<xbel version="1.0" xmlns:bookmark="http://www.freedesktop.org/standards/'
            'desktop-bookmarks" xmlns:mime="http://www.freedesktop.org/standards/'
            'shared-mime-info">\n' + "".join(marks) + "</xbel>\n"
        )


def files_button(s):
    """Opens one Explorer window on Documents; its taskbar button's centre."""
    bar = (0, HEIGHT - 40, WIDTH, 40)
    before = s.settle("taskbar")
    s.launch("xd-files", os.path.join(s.home, "Documents"))
    s.wait_window(title="Documents", state="activated")
    after = s.settle("files-window")
    button = after.changed_box(before, bar)
    s.check(button is not None, f"the Files button lights up ({button})")
    return button[0] + button[2] // 2, HEIGHT - 20


@scenario(AERO_INI)
def superbar_jumplist_items(s):
    """Aero: a jump list item's right-click menu pins it (the list stays
    open, the item under Pinned) and removes a recent one from the list;
    dragging a pinned item puts it above another."""
    docs = os.path.join(s.home, "Documents")
    for name in ("Todo.txt", "Plan.txt"):
        with open(os.path.join(docs, name), "w") as f:
            f.write(name + "\n")
    uri = {n: "file://" + os.path.join(docs, n) for n in ("Todo.txt", "Notes.txt", "Plan.txt")}
    write_recent(s, [(uri["Todo.txt"], "2026-01-03T00:00:00Z"),
                     (uri["Notes.txt"], "2026-01-02T00:00:00Z"),
                     (uri["Plan.txt"], "2026-01-01T00:00:00Z")])  # fmt: skip
    cx, cy = files_button(s)
    s.inject("move", WIDTH // 2, HEIGHT // 2, "sleep", 600)
    closed = s.settle("no-jump-list")
    above = (0, 0, WIDTH, HEIGHT - 40)
    pins = os.path.join(s.config, "jumplist-pinned")

    def pinned():
        if not os.path.exists(pins):
            return []
        c = configparser.ConfigParser(interpolation=None, strict=False)
        c.optionxform = str
        c.read(pins)
        if not c.has_section("org.xeondesktop.Files.desktop"):
            return []
        return [u for u in c["org.xeondesktop.Files.desktop"].get("Pinned", "").split(";") if u]

    def jump_list(label):
        shot = s.settle(label)
        box = shot.changed_box(closed, above)
        s.check(box is not None, f"the jump list is open ({label}: {box})")
        return shot, box

    def item_menu(box, y):
        """Right-clicks the item centred `y` below the list's top."""
        s.inject("move", box[0] + 60, box[1] + y, "sleep", 200, "click", "right", "sleep", 600)
        return s.settle("item-menu")

    # Recent: Todo, Notes, Plan (newest first).
    s.inject("move", cx, cy, "sleep", 200, "click", "right", "sleep", 800)
    shot, jl = jump_list("jump-list")
    menu = item_menu(jl, 6 + 24 + 13)
    ctx = menu.changed_box(shot, above)
    s.check(ctx is not None and ctx[3] > 60, f"Todo.txt's right-click menu opened ({ctx})")
    # Open, Pin to this list, Remove from this list, Properties.
    s.inject("key", "Down", "key", "Down", "key", "Return", "sleep", 600)
    s.wait(lambda: pinned() == [uri["Todo.txt"]], "Pin to this list to pin Todo.txt")
    s.log("ok: Pin to this list pinned Todo.txt")
    # The list stayed open: Pinned (Todo), Recent (Notes, Plan).
    shot, jl = jump_list("pinned-todo")
    item_menu(jl, 6 + 24 + 26 + 6 + 24 + 26 + 13)
    s.inject("key", "Down", "key", "Down", "key", "Return", "sleep", 600)
    s.wait(lambda: pinned() == [uri["Todo.txt"], uri["Plan.txt"]], "Plan.txt to be pinned")
    s.log("ok: Plan.txt pinned after Todo.txt")

    # Pinned (Todo, Plan), Recent (Notes): Remove from this list.
    shot, jl = jump_list("pinned-two")
    item_menu(jl, 6 + 24 + 2 * 26 + 6 + 24 + 13)
    s.inject("key", "Down", "key", "Down", "key", "Down", "key", "Return", "sleep", 600)

    def removed():
        import gi

        gi.require_version("GLib", "2.0")
        from gi.repository import GLib

        bf = GLib.BookmarkFile()
        bf.load_from_file(os.path.join(s.data, "recently-used.xbel"))
        # Used by no other program, the entry goes altogether.
        if not bf.has_item(uri["Notes.txt"]):
            return bf.has_item(uri["Todo.txt"])
        return "org.xeondesktop.Files" not in bf.get_applications(uri["Notes.txt"])

    s.wait(removed, "Remove from this list to forget Notes.txt for Explorer")
    s.log("ok: Remove from this list took Notes.txt off Explorer's recent files")
    shot, jl = jump_list("removed")
    s.check(jl[3] < 6 + 24 + 2 * 26 + 6 + 24 + 26 + 40,
            f"the list is shorter without Recent ({jl})")  # fmt: skip

    # Plan dragged above Todo.
    x = jl[0] + 60
    s.inject("move", x, jl[1] + 6 + 24 + 26 + 13, "down", "sleep", 100,
             "move", x, jl[1] + 6 + 24 + 26 + 4, "sleep", 100,
             "move", x, jl[1] + 6 + 24 + 13, "sleep", 200, "up", "sleep", 400)  # fmt: skip
    s.wait(lambda: pinned() == [uri["Plan.txt"], uri["Todo.txt"]], "the drag to reorder")
    s.log("ok: dragging Plan.txt onto Todo.txt put it first")
    s.check(s.window(title="Plan.txt") is None and s.window(title="Todo.txt") is None,
            "the drag opened nothing")  # fmt: skip
    s.inject("key", "Escape")


@scenario(AERO_INI + "\n[Taskbar]\nEdge=left\n")
def superbar_side_thumbnails(s):
    """Aero with the taskbar at the left: a program's thumbnails show to the
    right of its button, stacked down."""
    bar = (0, 0, 80, HEIGHT)
    before = s.settle("taskbar")
    for folder in ("Documents", "Pictures"):
        s.launch("xd-files", os.path.join(s.home, folder))
        s.wait_window(title=folder, state="activated")
    after = s.settle("two-windows")
    button = after.changed_box(before, bar)
    s.check(button is not None, f"the Files button lights up ({button})")
    cx, cy = button[0] + button[2] // 2, button[1] + button[3] // 2
    s.inject("move", cx, cy, "sleep", 1000)
    shown = s.settle("thumbnails")
    # The bar is wider than the button's lit part: from past it.
    beside = (button[0] + button[2] + 80, 0, 400, HEIGHT)
    popup = shown.changed_box(after, beside)
    s.check(popup is not None and popup[3] > 300 and popup[2] < 260 and popup[1] < cy,
            f"two thumbnails stacked beside the button ({popup})")  # fmt: skip
    # The upper one is Documents (the older window): clicking it brings it
    # forward.
    s.inject("move", popup[0] + 20, cy, "move", popup[0] + popup[2] // 2, popup[1] + 6 + 77,
             "sleep", 300, "click")  # fmt: skip
    s.wait_window(title="Documents", state="activated")
    s.log("ok: clicking the upper thumbnail brought Documents forward")


# A media player on the session bus (MPRIS) naming Explorer's desktop
# file: its method calls go to the file named on the command line.
MPRIS_PLAYER = r"""
import sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

XML = '''<node>
<interface name="org.mpris.MediaPlayer2">
  <method name="Raise"/><method name="Quit"/>
  <property name="DesktopEntry" type="s" access="read"/>
  <property name="Identity" type="s" access="read"/>
</interface>
<interface name="org.mpris.MediaPlayer2.Player">
  <method name="Previous"/><method name="Next"/><method name="PlayPause"/>
  <property name="PlaybackStatus" type="s" access="read"/>
  <property name="CanControl" type="b" access="read"/>
  <property name="CanGoPrevious" type="b" access="read"/>
  <property name="CanGoNext" type="b" access="read"/>
  <property name="CanPlay" type="b" access="read"/>
  <property name="CanPause" type="b" access="read"/>
</interface>
</node>'''
log = sys.argv[1]
state = {"status": "Paused"}
node = Gio.DBusNodeInfo.new_for_xml(XML)

def get_property(conn, sender, path, iface, prop):
    if prop == "DesktopEntry":
        return GLib.Variant("s", "org.xeondesktop.Files")
    if prop == "Identity":
        return GLib.Variant("s", "Test Player")
    if prop == "PlaybackStatus":
        return GLib.Variant("s", state["status"])
    return GLib.Variant("b", True)

def method_call(conn, sender, path, iface, method, params, invocation):
    with open(log, "a") as f:
        f.write(method + "\n")
    if method == "PlayPause":
        state["status"] = "Playing" if state["status"] != "Playing" else "Paused"
        conn.emit_signal(None, path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         GLib.Variant("(sa{sv}as)", ("org.mpris.MediaPlayer2.Player",
                                      {"PlaybackStatus": GLib.Variant("s", state["status"])}, [])))
    invocation.return_value(None)

bus = Gio.bus_get_sync(Gio.BusType.SESSION)
for iface in node.interfaces:
    bus.register_object("/org/mpris/MediaPlayer2", iface, method_call, get_property)
Gio.bus_own_name_on_connection(bus, "org.mpris.MediaPlayer2.xdtest", 0, None)
GLib.MainLoop().run()
"""


@scenario(AERO_INI)
def superbar_media_buttons(s):
    """Aero: a media player (MPRIS) of a program with one window puts
    Previous, Play/Pause and Next under its thumbnail; they reach the
    player, and the middle one turns to Pause while it plays."""
    calls = os.path.join(s.base, "mpris-calls")
    s.launch(sys.executable, "-c", MPRIS_PLAYER, calls)
    cx, cy = files_button(s)
    after = s.settle("one-window")
    s.inject("move", cx, cy, "sleep", 1000)
    shown = s.settle("thumbnail")
    above = (0, HEIGHT - 300, WIDTH, 260)
    popup = shown.changed_box(after, above)
    # A tile with the buttons is taller than the 166 of one without.
    s.check(popup is not None and popup[3] > 180, f"the thumbnail has a button row ({popup})")
    x0 = popup[0] + 6 + (208 - 94) // 2
    by = popup[1] + 6 + 22 + 4 + 120 + 4 + 3 + 12

    def called():
        return open(calls).read().split() if os.path.exists(calls) else []

    s.inject("move", cx, popup[1] + popup[3] - 4, "move", x0 + 64 + 15, by, "sleep", 300, "click")
    s.wait(lambda: called() == ["Next"], "Next to reach the player")
    s.log("ok: the Next button called the player's Next")
    paused = s.settle("paused")
    s.inject("move", x0 + 32 + 15, by, "sleep", 300, "click", "sleep", 500)
    s.wait(lambda: called() == ["Next", "PlayPause"], "Play to reach the player")
    s.log("ok: the Play button called PlayPause")
    s.inject("move", x0 + 15, by, "sleep", 500)
    playing = s.settle("playing")
    s.check(playing.differs(paused, (x0 + 32, by - 10, 30, 20), step=1) > 0.05,
            "the middle button shows Pause while it plays")  # fmt: skip
    s.check(s.window(title="Documents") is not None, "the window is still there")


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
def favorites(s):
    """Explorer's Favorites > Add to Favorites...: the folder goes into GTK's
    bookmarks under the name given, after the lines already there, and the
    menu lists it."""
    docs = os.path.join(s.home, "Documents")
    gtk = os.path.join(os.environ["XDG_CONFIG_HOME"], "gtk-3.0")
    os.makedirs(gtk, exist_ok=True)
    bookmarks = os.path.join(gtk, "bookmarks")
    with open(bookmarks, "w") as f:
        f.write("# kept\nfile:///tmp Scratch\n")
    empty = s.settle("desktop")
    s.launch("xd-files", docs)
    s.wait_window(app_id="org.xeondesktop.Files", state="activated")
    x, y, _, _ = s.settle("explorer").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    # The menu bar's Favorites, then its first item, Add to Favorites...
    s.inject("move", x + 171, y + 32, "click", "sleep", 500)
    s.settle("favorites menu")
    s.inject("move", x + 210, y + 55, "click")
    # The box is a dialog of the window (not in the window list): the
    # window loses the focus to it.
    s.wait(lambda: "activated" not in s.window(app_id="org.xeondesktop.Files")["states"],
           "the Add Favorite box to take the focus")
    s.settle("add favorite")
    s.inject("key", "ctrl+a", "type", "Paperwork", "key", "Return")
    want = "# kept\nfile:///tmp Scratch\n" + "file://" + urllib.parse.quote(docs) + " Paperwork\n"
    s.wait(lambda: open(bookmarks).read() == want, "the bookmark to be added")
    s.log("ok: Add to Favorites wrote the bookmark, the other lines kept")
    s.inject("move", x + 171, y + 32, "click", "sleep", 500)
    s.settle("favorites menu with Paperwork")
    s.inject("key", "Escape")


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


def is_checker(shot, x, y, n=6):
    """Whether pixels from (x, y) rightwards alternate between black and
    white, as the dragged outline's checkerboard does."""
    seen = {shot.pixel(x + i, y) for i in range(n)}
    return seen == {(0, 0, 0), (255, 255, 255)}


@scenario(CLASSIC_INI + "\n[Effects]\nFullWindowDrag=false\n")
def outline_drag(s):
    """"Show window contents while dragging" off: dragging a caption moves
    only a checkered outline; the window stays put until the button is let
    go, and then jumps to where the outline was."""
    empty = s.settle("desktop")
    s.launch("xd-files", os.path.join(s.home, "Documents"))
    s.wait_window(app_id="org.xeondesktop.Files", state="activated")
    area = (0, 0, WIDTH, HEIGHT - 40)  # above the taskbar
    before = s.settle("explorer")
    x, y, w, h = before.changed_box(empty, area)
    cx, cy = x + w // 2, y + 10  # on the caption
    # One xd-inject holds the button throughout (its pointer lets go as it
    # exits); the screenshot is taken meanwhile.
    steps = ("move", cx, cy, "down", "sleep", 100, "move", cx + 50, cy + 30, "sleep", 100,
             "move", cx + 100, cy + 60, "sleep", 2500, "up")
    drag = s.launch(s.tool("xd-inject"), *map(str, steps))
    time.sleep(1.2)
    held = s.shot("dragging")
    # Left of the outline the window is as it was; the outline's left edge
    # is checkered, the window's is not.
    s.check(held.differs(before, (x, y, 96, h), step=2, tolerance=0) == 0,
            "while the button is held the window hasn't moved")  # fmt: skip
    s.check(is_checker(held, x + 100, y + 60 + h // 2), "a checkered outline where it's dragged")
    s.check(not is_checker(held, x, y + h // 2), "and none round the window itself")
    drag.wait(timeout=10)
    mx, my, _, _ = s.settle("dropped").changed_box(empty, area)
    s.check(abs(mx - (x + 100)) <= 4 and abs(my - (y + 60)) <= 4,
            f"on release the window went there ({mx},{my}, expected {x + 100},{y + 60})")
    s.check(not is_checker(s.shot("after"), x + 100, y + 60 + h // 2), "and the outline is gone")


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


@scenario(AERO_INI)
def window_color(s):
    """Aero: Display Properties' Advanced opens Window Color and Appearance;
    its choices reach desktop.ini on OK, and Cancel leaves them alone."""
    s.launch("xd-desk", "--page", "2")
    s.wait_window(title="Display Properties", state="activated")
    s.settle("appearance")
    # Tabs, style, scheme, font size, then Advanced.
    s.inject("key", "Tab", "key", "Tab", "key", "Tab", "key", "Tab", "sleep", 200, "key", "Return")
    s.wait_window(title="Window Color and Appearance", state="activated")
    s.settle("window-color")
    # Esc is Cancel: nothing changes.
    s.inject("key", "Escape")
    s.wait_window(title="Window Color and Appearance", gone=True)
    s.inject("key", "Tab", "key", "Tab", "key", "Tab", "key", "Tab", "sleep", 200, "key", "Return")
    s.wait_window(title="Window Color and Appearance", state="activated")
    # Focus starts on Enable transparency: off; then the intensity to its
    # least; Enter is Save changes, and Display Properties' OK.
    s.inject("key", "space", "sleep", 200, "key", "Tab", "key", "Home", "sleep", 300)
    s.settle("chosen")
    s.inject("key", "Return")
    s.wait_window(title="Window Color and Appearance", gone=True)
    s.inject("key", "Return")
    s.wait_window(title="Display Properties", gone=True)

    def written():
        a = s.ini()["Appearance"]
        return a.get("GlassTransparency") == "false" and a.get("GlassColor") == "0074B8FC"

    s.wait(written, "desktop.ini to take the glass colour")
    s.log("ok: GlassColor=0074B8FC, GlassTransparency=false (Sky at no intensity)")


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
            # Alt+A opens Action, as in Windows (it once opened File, the
            # menu under where GDK last saw the pointer).
            s.inject("key", "alt+a")
            shot = s.settle(label)
            box = shot.changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
            # A menu of the selected volume's actions and Refresh: well over
            # a few items tall (it once came up empty), right of File.
            s.check(
                box is not None and box[3] > 100 and box[0] > 40,
                f"the Action menu opened with items ({box})",
            )
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


def mock_logind(s, display_manager=False):
    """A private system bus with dbusmock's logind: our session (c1, found
    by pid) and another user's switched away from (c2); with a mock
    display manager too if asked. Returns the logind mock and a function
    giving what the mocks were called with so far."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    logs = []

    def spawn(template):
        path = os.path.join(s.base, f"mock-{len(logs)}.log")
        log = open(path, "w")
        proc, obj = DBusTestCase.spawn_server_template(
            template, {}, stdout=log, system_bus=True
        )
        s.children.append(proc)
        logs.append(path)
        return obj

    logind = spawn("logind")
    logind.AddSession("c1", "seat0", dbus.UInt32(os.getuid()), "user", True,
                      dbus_interface=MOCK_IFACE)  # fmt: skip
    logind.AddSession("c2", "seat0", dbus.UInt32(os.getuid() + 1), "guest", False,
                      dbus_interface=MOCK_IFACE)  # fmt: skip
    logind.AddMethod("org.freedesktop.login1.Manager", "GetSessionByPID", "u", "o",
                     'ret = dbus.ObjectPath("/org/freedesktop/login1/session/c1")',
                     dbus_interface=MOCK_IFACE)  # fmt: skip
    if display_manager:
        spawn(os.path.join(SOURCE_ROOT, "tests", "dbusmock", "displaymanager.py"))

    def calls():
        text = ""
        for path in logs:
            with open(path) as f:
                text += f.read()
        return text

    return logind, calls


def logind_session_call(method):
    """Our mock session's Lock or Unlock, as logind does it for a display
    manager: the session object sends the signal."""
    from dbusmock import DBusTestCase

    bus = DBusTestCase.get_dbus(system_bus=True)
    session = bus.get_object("org.freedesktop.login1", "/org/freedesktop/login1/session/c1")
    session.get_dbus_method(method, "org.freedesktop.login1.Session")()


@scenario(LUNA_INI, requires=("dbusmock",))
def switch_user_and_back(s):
    """XP's Log Off box (on a dbusmock logind and display manager): Switch
    User locks the screen and goes to the greeter; logind's Unlock, which
    the display manager sends once its greeter has let the user back in,
    unlocks without asking for the password again."""
    from dbusmock import DBusTestCase

    try:
        _, calls = mock_logind(s, display_manager=True)
        before = s.settle("desktop")
        s.launch("xd-shutdown", "--logoff")
        s.wait_window(title="Log Off XeonDesktop", state="activated")
        dialog = s.settle("log-off")
        box = dialog.changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
        s.check(box is not None and 300 < box[2] < 340, f"XP's Log Off box opened ({box})")
        # Switch User's tile, from the box's top left (see shutdown.c).
        s.inject("move", box[0] + 110, box[1] + 113, "click")
        s.wait(lambda: "SwitchToGreeter" in calls(), "the switch to the greeter")
        s.log("ok: Switch User asked the display manager for its greeter")
        locked = s.settle("locked")
        s.check(locked.differs(before, (0, HEIGHT - 30, WIDTH, 30)) > 0.5,
                "the screen was locked first")  # fmt: skip
        s.check("SetLockedHint True" in calls(), "xd-lock told logind it locked")
        logind_session_call("Unlock")
        s.wait(lambda: "SetLockedHint False" in calls(), "xd-lock to unlock")
        back = s.settle("unlocked")
        s.check(back.differs(before, (0, 0, WIDTH, HEIGHT - 30)) < 0.02,
                "logind's Unlock unlocked without a password")  # fmt: skip

        # Lock turns a running unprotected screen saver into a lock.
        s.launch("xd-lock", "--saver", "blank", "--no-lock")
        s.wait(lambda: s.shot("saver").pixel(WIDTH // 2, HEIGHT - 4) == (0, 0, 0), "the saver")
        logind_session_call("Lock")
        s.wait(lambda: calls().count("SetLockedHint True") == 2, "the saver to lock")
        s.inject("move", 100, 100, "move", 400, 400)
        dialog = s.settle("saver-locked")
        # Computer Locked: no taskbar, a dialog in the middle.
        s.check(dialog.differs(before, (0, HEIGHT - 30, WIDTH, 30)) > 0.5
                and dialog.differs(before, (310, 280, 400, 200)) > 0.3,
                "input showed Computer Locked, not the desktop")  # fmt: skip
        logind_session_call("Unlock")
        s.wait(lambda: calls().count("SetLockedHint False") == 2, "xd-lock to unlock again")
        s.log("ok: Lock made the saver a lock; Unlock ended it")
    finally:
        DBusTestCase.tearDownClass()


@scenario(LUNA_INI, requires=("dbusmock",))
def task_manager_users(s):
    """Task Manager's Users tab (on a dbusmock logind) lists the sessions;
    Logoff on another user's ends it through logind."""
    from dbusmock import DBusTestCase

    try:
        _, calls = mock_logind(s)
        before = s.settle("desktop")
        s.launch("xd-taskmgr")
        s.wait_window(title="Task Manager", state="activated")
        # Applications, Processes, Performance, Networking, Users.
        s.inject(*(["key", "ctrl+Tab", "sleep", 200] * 4))
        users = s.settle("users")
        box = users.changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
        s.check(box is not None, f"Task Manager is on screen ({box})")
        s.wait(lambda: "ListSessions" in calls(), "the Users tab to ask logind")
        # The second row (guest), then Logoff (see users.c), from the
        # window's top left.
        s.inject("move", box[0] + 44, box[1] + 133, "click", "sleep", 300)
        selected = s.settle("selected")
        s.inject("move", box[0] + 270, box[1] + 443, "click")
        # "Are you sure...?" over the list.
        s.wait(lambda: s.shot("confirm").differs(selected, (box[0] + 60, box[1] + 180, 320, 140)) > 0.2,
               "the confirmation")  # fmt: skip
        s.settle("confirm")
        # No is the default: Shift+Tab to Yes.
        s.inject("key", "shift+Tab", "key", "Return")
        s.wait(lambda: 'TerminateSession "c2"' in calls(), "Logoff to end the guest's session")
        s.check('TerminateSession "c1"' not in calls(), "Logoff ended the selected session only")
    finally:
        DBusTestCase.tearDownClass()


@scenario()
def access_indicators(s):
    """MouseKeys turned on puts its status icon in the notification area;
    unchecking "Show MouseKey status on screen" takes it away."""
    tray = (WIDTH - 200, HEIGHT - 30, 140, 30)  # left of the clock, which may tick
    before = s.settle("no-indicator")

    def set_access(**keys):
        ini = s.ini()
        if not ini.has_section("Accessibility"):
            ini.add_section("Accessibility")
        for k, v in keys.items():
            ini["Accessibility"][k] = v
        with open(os.path.join(s.config, "desktop.ini"), "w") as f:
            ini.write(f, space_around_delimiters=False)

    set_access(MouseKeys="true")
    on = s.wait(lambda: (lambda shot: shot if shot.differs(before, tray, step=2) > 0 else None)(
        s.shot("indicator")), "the MouseKeys icon")  # fmt: skip
    s.check(on.changed_box(before, tray) is not None, "MouseKeys' icon is in the tray")
    set_access(MouseKeysIndicator="false")
    s.wait(lambda: s.shot("indicator-off").differs(before, tray, step=2) == 0,
           "the icon to go")  # fmt: skip
    s.log("ok: the icon goes with its Show ... status on screen")


@scenario(LUNA_INI, requires=("dbusmock",))
def power_alarms(s):
    """Power Options on a laptop (a dbusmock UPower battery): the Alarms and
    Power Meter tabs are there, and the critical alarm's Alarm Action box
    saves its action."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    try:
        for template, params in (("upower", {"OnBattery": True}), ("logind", {})):
            proc, _ = DBusTestCase.spawn_server_template(
                template, params, stdout=subprocess.DEVNULL, system_bus=True
            )
            s.children.append(proc)
        bus = dbus.SystemBus()
        upower = bus.get_object("org.freedesktop.UPower", "/org/freedesktop/UPower")
        mock = dbus.Interface(upower, MOCK_IFACE)
        mock.AddDischargingBattery("BAT0", "Battery", 50.0, 3600)
        mock.SetupDisplayDevice(2, 2, 50.0, 50.0, 100.0, 10.0, 3600, 0, True, "battery-good", 1)

        empty = s.settle("desktop")
        s.launch("xd-power", "--page", "1")
        s.wait_window(title="Power Options Properties", state="activated")
        dialog = s.settle("alarms")
        x, y, _, _ = dialog.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
        # The critical alarm's Alarm Action...: a dialog box of the window
        # (not on the taskbar, so not in xd-windows' list).
        s.inject("move", x + 78, y + 259, "click", "sleep", 500)
        actions = s.settle("alarm-actions")
        # Centred on its window (whose title bar goes inactive too).
        ax, ay = x + 69, y + 102
        s.check(actions.differs(dialog, (ax + 20, ay + 40, 240, 180)) > 0.2,
                "Alarm Action opened its box")  # fmt: skip
        # "When the alarm goes off" (Stand by, the first choice), OK, Apply.
        s.inject("move", ax + 31, ay + 150, "click", "sleep", 300, "move", ax + 153, ay + 221,
                 "click")  # fmt: skip
        s.inject("sleep", 500)
        s.inject("move", x + 372, y + 425, "click", "sleep", 500)

        def saved():
            ini = s.ini()
            return ini.has_section("Power") and ini["Power"].get("CriticalAlarmAction")

        s.check(s.wait(saved, "the alarm saved") == "standby", "Critical alarm: Stand by saved")
        s.inject("move", x + 200, y + 49, "click", "sleep", 500)  # the Power Meter tab
        meter = s.settle("power-meter")
        s.check(meter.differs(dialog, (x + 20, y + 70, 380, 250)) > 0.02,
                "the Power Meter tab shows")  # fmt: skip
    finally:
        DBusTestCase.tearDownClass()


def mock_networkmanager(s):
    """Before the compositor starts: a private system bus with dbusmock's
    NetworkManager and xd-netconn's scene (tests/dbusmock/netconn.py): a
    connected wired adapter, one unplugged, Wi-Fi with two networks."""
    from dbusmock import DBusTestCase

    DBusTestCase.start_system_bus()
    template = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "netconn.py")
    proc, _ = DBusTestCase.spawn_server_template(
        template, {}, stdout=subprocess.DEVNULL, system_bus=True
    )
    s.children.append(proc)


TRAY = (WIDTH - 300, HEIGHT - 30, 236, 30)  # the notification area, up to the clock


@scenario(LUNA_INI, requires=("dbusmock",), setup=mock_networkmanager)
def network_status(s):
    """dbusmock's NetworkManager with a connected wired adapter: a single
    click on the network icon does nothing (as in XP), a double click opens
    "Local Area Connection Status", whose Disable disconnects the adapter
    through NetworkManager and closes it."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    try:
        bus = dbus.SystemBus()
        nm = bus.get_object("org.freedesktop.NetworkManager", "/org/freedesktop")
        mock = dbus.Interface(nm, MOCK_IFACE)
        # Where the icon is: it changes while NetworkManager says it's
        # offline, and back.
        s.inject("move", 300, 300)
        before = s.settle("desktop")
        mock.SetGlobalConnectionState(dbus.UInt32(20))
        _, box = wait_change(s, before, TRAY, "the network icon to go offline", "offline")
        x, y = box[0] + box[2] // 2, box[1] + box[3] // 2
        mock.SetGlobalConnectionState(dbus.UInt32(70))
        s.wait(lambda: s.shot("online").changed_box(before, TRAY) is None, "it to come back")
        s.inject("move", 300, 300)
        icon = s.settle("network-icon")
        s.inject("move", x, y, "click", "sleep", 1000, "move", 300, 300, "sleep", 300)
        s.check(s.settle("single-click").differs(icon, (WIDTH - 400, HEIGHT - 400, 400, 360)) == 0,
                "a single click opens nothing")  # fmt: skip
        s.inject("move", x, y, "click", "click")
        s.wait_window(title="Local Area Connection Status", state="activated")
        s.log("ok: a double click opened Local Area Connection Status")
        dialog = s.settle("status")
        s.check(dialog.differs(icon, (WIDTH // 2 - 150, HEIGHT // 2 - 150, 300, 300)) > 0.2,
                "the Status dialog shows")  # fmt: skip
        s.inject("key", "ctrl+Tab")  # Support
        support = s.settle("support")
        s.check(support.differs(dialog, (WIDTH // 2 - 150, HEIGHT // 2 - 150, 300, 300)) > 0.02,
                "its Support tab shows")  # fmt: skip
        s.inject("key", "ctrl+Tab", "sleep", 300, "key", "alt+d")  # General's Disable

        def eth0_state():
            dev = bus.get_object("org.freedesktop.NetworkManager",
                                 "/org/freedesktop/NetworkManager/Devices/mock_eth0")  # fmt: skip
            return dev.Get("org.freedesktop.NetworkManager.Device", "State",
                           dbus_interface=dbus.PROPERTIES_IFACE)  # fmt: skip

        s.wait(lambda: eth0_state() == 30, "the adapter to be disconnected")
        s.log("ok: Disable disconnected the adapter")
        s.wait_window(title="Local Area Connection Status", gone=True)
        s.log("ok: Status closed with the connection")
    finally:
        DBusTestCase.tearDownClass()


def mock_laptop(s):
    """Before the compositor starts: a private system bus with dbusmock's
    UPower (a battery at 50%, discharging) and logind (logging its calls)."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    for template, params in (("upower", {"OnBattery": True}), ("logind", {})):
        log = open(os.path.join(s.base, template + ".log"), "w")
        proc, _ = DBusTestCase.spawn_server_template(template, params, stdout=log, system_bus=True)
        s.children.append(proc)
    upower = dbus.SystemBus().get_object("org.freedesktop.UPower", "/org/freedesktop/UPower")
    mock = dbus.Interface(upower, MOCK_IFACE)
    mock.AddDischargingBattery("BAT0", "Battery", 50.0, 3600)
    mock.SetupDisplayDevice(2, 2, 50.0, 50.0, 100.0, 10.0, 3600, 0, True, "battery-good", 1)


@scenario(LUNA_INI + "\n[Power]\nCriticalAlarmAction=standby\n", requires=("dbusmock",),
          setup=mock_laptop)  # fmt: skip
def battery_alarms(s):
    """A dbusmock battery running down: at 10% the low battery alarm sounds
    and shows "Low Battery Alarm"; at 3% the critical one sounds, shows
    "Critical Battery Alarm" and stands by (its Alarm Action) through
    logind. A double click on the power meter opens Power Options."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    display = "/org/freedesktop/UPower/devices/DisplayDevice"
    sounds = os.path.join(os.environ["XDG_RUNTIME_DIR"], "sounds.log")

    def played(name):
        try:
            with open(sounds) as f:
                return any(line.startswith(name + "\t") for line in f)
        except OSError:
            return False

    def logind_called(method):
        with open(os.path.join(s.base, "logind.log")) as f:
            return any(f" {method} " in line for line in f)

    try:
        upower = dbus.SystemBus().get_object("org.freedesktop.UPower", "/org/freedesktop/UPower")
        mock = dbus.Interface(upower, MOCK_IFACE)
        s.inject("move", 300, 300)
        before = s.settle("desktop")
        mock.SetDeviceProperties(display, {"Percentage": dbus.Double(9.0)})
        s.wait_window(title="Low Battery Alarm")
        s.log("ok: at 9% the low battery alarm's message shows")
        s.check(s.wait(lambda: played("Low Battery Alarm"), "its sound"), "it sounded")
        s.check(not logind_called("Suspend"), "the low alarm does nothing more")
        # Where the meter is: it drew the lower charge.
        _, box = wait_change(s, before, TRAY, "the power meter to change", "meter")
        x, y = box[0] + box[2] // 2, box[1] + box[3] // 2
        s.inject("move", 300, 300)
        mock.SetDeviceProperties(display, {"Percentage": dbus.Double(2.0)})
        s.wait_window(title="Critical Battery Alarm")
        s.log("ok: at 2% the critical battery alarm's message shows")
        s.check(s.wait(lambda: played("Critical Battery Alarm"), "its sound"), "it sounded")
        s.check(s.wait(lambda: logind_called("Suspend"), "logind's Suspend"),
                "the critical alarm stood the computer by")  # fmt: skip
        s.inject("move", x, y, "click", "click")
        s.wait_window(title="Power Options Properties", state="activated")
        s.settle("power-meter")
        s.log("ok: a double click on the power meter opened Power Options")
    finally:
        DBusTestCase.tearDownClass()


def private_pipewire(s):
    """Before the compositor starts: a sound server of the session's own,
    PipeWire and its pulse server in the session's runtime dir, with one
    null sink. With no session manager, no real sound device is opened."""
    if not shutil.which("pipewire-pulse") or not shutil.which("pactl"):
        return
    log = open(os.path.join(s.base, "shots", "pipewire.log"), "w")  # kept with --keep
    for program in ("pipewire", "pipewire-pulse"):
        s.children.append(subprocess.Popen([program], stdout=log, stderr=log,
                                           preexec_fn=die_with_parent))  # fmt: skip
    socket = os.path.join(os.environ["XDG_RUNTIME_DIR"], "pulse", "native")
    s.wait(lambda: os.path.exists(socket), "the pulse server", 15)
    subprocess.run(["pactl", "load-module", "module-null-sink", "sink_name=test-sink"],
                   stdout=subprocess.DEVNULL, timeout=10, check=True)  # fmt: skip


def sink_state():
    """The test sink's volume (percent, the loudest channel) and mute."""
    import re

    vol = subprocess.run(["pactl", "get-sink-volume", "test-sink"], capture_output=True,
                         text=True, timeout=10).stdout  # fmt: skip
    mute = subprocess.run(["pactl", "get-sink-mute", "test-sink"], capture_output=True,
                          text=True, timeout=10).stdout  # fmt: skip
    percents = [int(p) for p in re.findall(r"(\d+)%", vol)]
    return (max(percents) if percents else None), "yes" in mute


@scenario(LUNA_INI, setup=private_pipewire)
def volume_keys(s):
    """The volume flyout from the keyboard, as XP's: Home and End go to the
    top and bottom, Page Up a fifth up, Up/Left a step up and Down/Right a
    step down, Space checks and unchecks Mute, Esc closes it."""
    if not shutil.which("pipewire-pulse") or not shutil.which("pactl"):
        s.log("skipped: needs pipewire-pulse and pactl")
        return
    subprocess.run(["pactl", "set-sink-volume", "test-sink", "50%"], timeout=10, check=True)
    s.inject("move", 300, 300)
    before = s.settle("desktop")
    # Where the icon is: it changes when muted, and back.
    subprocess.run(["pactl", "set-sink-mute", "test-sink", "1"], timeout=10, check=True)
    _, box = wait_change(s, before, TRAY, "the volume icon to show Mute", "muted")
    subprocess.run(["pactl", "set-sink-mute", "test-sink", "0"], timeout=10, check=True)
    s.wait(lambda: s.shot("unmuted").changed_box(before, TRAY) is None, "it to come back")
    x, y = box[0] + box[2] // 2, box[1] + box[3] // 2
    s.inject("move", x, y, "click", "move", 300, 300)
    flyout = s.settle("flyout")
    s.check(flyout.differs(before, (x - 60, HEIGHT - 200, 120, 160)) > 0.2, "the flyout opened")

    def keys_give(keys, want, what):
        s.inject(*[a for k in keys for a in ("key", k)])
        s.wait(lambda: sink_state() == want, f"{what} ({want}, now {sink_state()})", 10)
        s.log(f"ok: {what}")

    keys_give(["Home"], (100, False), "Home: 100%")
    keys_give(["End"], (0, False), "End: 0%")
    keys_give(["Page_Up"], (20, False), "Page Up: 20%")
    keys_give(["Up", "Up", "Left"], (26, False), "Up, Up, Left: 26%")
    keys_give(["Down", "Right"], (22, False), "Down, Right: 22%")
    keys_give(["space"], (22, True), "Space: muted")
    keys_give(["space"], (22, False), "Space again: not muted")
    s.inject("key", "Escape")
    s.wait(lambda: s.shot("closed").differs(before, (x - 60, HEIGHT - 200, 120, 160)) < 0.02,
           "Esc to close the flyout")  # fmt: skip
    s.log("ok: Esc closed the flyout")


# Another program's GTK 3 window with a combo box, which drops down once
# the file named on the command line exists.
GTK_COMBO = r"""
import os, sys
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk
w = Gtk.Window(title="Combo test")
w.set_default_size(300, 200)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, margin=10)
combo = Gtk.ComboBoxText()
for t in ("One", "Two", "Three", "Four", "Five"):
    combo.append_text(t)
combo.set_active(0)
box.pack_start(combo, False, False, 0)
w.add(box)
w.connect("destroy", Gtk.main_quit)
w.show_all()
def poll():
    if os.path.exists(sys.argv[1]):
        combo.popup()
        return False
    return True
GLib.timeout_add(100, poll)
Gtk.main()
"""


# A window that draws its own title bar (a GTK header bar).
GTK_CSD = r"""
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
w = Gtk.Window(title="Ghost test")
bar = Gtk.HeaderBar(title="Ghost test", show_close_button=True)
w.set_titlebar(bar)
w.set_default_size(420, 300)
w.connect("destroy", Gtk.main_quit)
w.show_all()
Gtk.main()
"""


@scenario()
def hung_csd_window_ghost_frame(s):
    """A window drawing its own title bar that stops responding gets our
    frame, as Windows' ghost window: its own close button can't work, ours
    offers End Program. It goes back where it was when it answers again."""
    probe = subprocess.run(
        [sys.executable, "-c", "import gi; gi.require_version('Gtk', '3.0')"],
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode != 0:
        s.log("skipped: no GTK 3 introspection data for Python")
        return
    script = os.path.join(s.base, "csd.py")
    with open(script, "w") as f:
        f.write(GTK_CSD)
    empty = s.settle("desktop")
    area = (0, 0, WIDTH, HEIGHT - 40)
    app = s.launch(sys.executable, script, env={"GSETTINGS_BACKEND": "memory"})
    s.wait_window(title="Ghost test", state="activated")
    before = s.settle("answering").changed_box(empty, area)
    s.check(before is not None, "the window is drawn")
    try:
        app.send_signal(signal.SIGSTOP)
        s.wait(lambda: s.window(title="Ghost test (Not Responding)"), "Not Responding",
               timeout=15)  # fmt: skip
        x, y, w, h = s.settle("hung").changed_box(empty, area)
        s.check(h >= before[3] + 15 and w >= before[2],
                f"our frame around it ({before[2]}x{before[3]} -> {w}x{h})")  # fmt: skip
        # The frame's Close button, at its caption's right end.
        s.inject("move", x + w - 12, y + 12, "click")
        s.wait(lambda: any(t["title"].startswith("End Program") for t in s.windows()),
               "End Program")  # fmt: skip
        s.log("ok: our frame's Close button offers End Program")
        s.inject("key", "Escape")  # Cancel
    finally:
        app.send_signal(signal.SIGCONT)
    s.wait(lambda: s.window(title="Ghost test") is not None, "it to answer again", timeout=15)
    after = s.settle("answering again").changed_box(empty, area)
    s.check(after is not None and all(abs(a - b) <= 2 for a, b in zip(after, before)),
            f"back where it was, without our frame ({before} -> {after})")  # fmt: skip


# A window slow to close: "hang" stops answering for 12s when asked to
# close, then closes; "ask" asks "Save changes?" instead.
GTK_SLOW_CLOSE = r"""
import sys, time
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
w = Gtk.Window(title="Slow close")
w.set_default_size(360, 240)
def delete(win, event):
    if sys.argv[1] == "hang":
        time.sleep(12)
        return False
    d = Gtk.MessageDialog(transient_for=win, message_type=Gtk.MessageType.QUESTION,
                          buttons=Gtk.ButtonsType.YES_NO, text="Save changes?")
    d.show()
    return True
w.connect("delete-event", delete)
w.connect("destroy", Gtk.main_quit)
w.show_all()
Gtk.main()
"""


def have_gtk3(s):
    probe = subprocess.run(
        [sys.executable, "-c", "import gi; gi.require_version('Gtk', '3.0')"],
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode != 0:
        s.log("skipped: no GTK 3 introspection data for Python")
    return probe.returncode == 0


def end_program_box(s):
    return any(t["title"].startswith("End Program") for t in s.windows())


@scenario()
def end_program_after_close(s):
    """A window that stops responding once asked to close (Alt+F4) gets
    Windows' End Program box after the hung-window time, which goes away by
    itself when the program does close; one that asks "Save changes?"
    instead is responding and gets none."""
    if not have_gtk3(s):
        return
    script = os.path.join(s.base, "slow.py")
    with open(script, "w") as f:
        f.write(GTK_SLOW_CLOSE)
    env = {"GSETTINGS_BACKEND": "memory"}
    s.launch(sys.executable, script, "ask", env=env)
    s.wait_window(title="Slow close", state="activated")
    s.settle("asking")
    s.inject("key", "alt+F4")
    time.sleep(9)  # past the 5s timeout and a ping round after it
    s.settle("save changes")
    s.check(not end_program_box(s), "no End Program box for a window asking Save changes?")
    s.check(s.window(title="Slow close") is not None, "and the window is still open")

    p = s.launch(sys.executable, script, "hang", env=env)
    s.wait(lambda: sum(w["title"] == "Slow close" for w in s.windows()) == 2, "the second window")
    s.wait(lambda: any(w["title"] == "Slow close" and "activated" in w["states"]
                       for w in s.windows()), "it to be focused")  # fmt: skip
    s.settle("hanging one")
    s.inject("key", "alt+F4")
    start = time.monotonic()
    s.wait(lambda: end_program_box(s), "End Program", timeout=10)
    waited = time.monotonic() - start
    s.settle("end program")
    s.check(waited >= 4, f"End Program after the hung-window time ({waited:.1f}s)")
    s.wait(lambda: p.poll() is not None, "the program to close by itself", timeout=15)
    s.wait(lambda: not end_program_box(s), "the box to go with it", timeout=5)
    s.log("ok: the box went away when the program closed")


def window_edges(shot, empty, inside):
    """The box (x, y, w, h) of the window around the point inside, exact to
    the pixel: how far what differs from the empty desktop reaches along
    its row and column."""
    x, y = inside

    def diff(px, py):
        return max(abs(a - b) for a, b in zip(shot.pixel(px, py), empty.pixel(px, py))) > 8

    left, right, top, bottom = x, x, y, y
    while left > 0 and diff(left - 1, y):
        left -= 1
    while right < WIDTH - 1 and diff(right + 1, y):
        right += 1
    while top > 0 and diff(x, top - 1):
        top -= 1
    while bottom < HEIGHT - 1 and diff(x, bottom + 1):
        bottom += 1
    return (left, top, right - left + 1, bottom - top + 1)


@scenario()
def keyboard_move_size(s):
    """The window menu's Move and Size: Ctrl+arrows step 1px; the mouse
    drags the window, or the chosen edges, until a click; two arrows pick
    a corner (two edges) to size from, the opposite corner staying put."""
    empty = s.settle("desktop")
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    x, y, w, h = s.settle("window").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))

    def box(label):
        b = window_edges(s.settle(label), empty, (x + w // 2, y + h // 2))
        s.log(f"{label}: {b}")
        return b

    start = box("start")

    def menu(letter):
        s.inject("key", "alt+space", "sleep", 400, "key", letter, "sleep", 300)

    # Move: Ctrl+Right three times, Ctrl+Down twice: 3px and 2px.
    menu("m")
    s.inject("key", "ctrl+Right", "key", "ctrl+Right", "key", "ctrl+Right",
             "key", "ctrl+Down", "key", "ctrl+Down", "sleep", 200, "key", "Return")  # fmt: skip
    b = box("moved-ctrl")
    s.check(b == (start[0] + 3, start[1] + 2, start[2], start[3]),
            f"Ctrl+arrows moved it 1px at a time ({start} -> {b})")  # fmt: skip

    # Move with the mouse: the pointer starts on the caption; the window
    # follows it until a click.
    menu("m")
    cx, cy = b[0] + b[2] // 2, b[1] + 10  # where Move put the pointer (about)
    s.inject("move", cx + 20, cy + 15, "sleep", 100, "move", cx + 60, cy + 40, "sleep", 300)
    dragging = box("move-mouse")
    s.check(dragging[2:] == b[2:] and dragging[0] > b[0] + 30 and dragging[1] > b[1] + 20,
            f"the window follows the mouse ({b} -> {dragging})")  # fmt: skip
    s.inject("click", "sleep", 200, "move", cx + 200, cy + 200, "sleep", 300)
    kept = box("move-clicked")
    s.check(kept == dragging, f"a click left it there ({kept})")
    s.check(s.window(title="Control Panel") is not None, "and closed nothing")

    # Size from the top-left corner: Left picks the left edge, Up the top;
    # then Left and Up (8px each) move them out, Ctrl+Left 1px more.
    menu("s")
    s.inject("key", "Left", "key", "Up", "key", "Left", "key", "Up", "key", "ctrl+Left",
             "sleep", 200, "key", "Return")  # fmt: skip
    sized = box("sized-corner")
    want = (kept[0] - 9, kept[1] - 8, kept[2] + 9, kept[3] + 8)
    s.check(sized == want, f"sized from the top-left corner ({kept} -> {sized}, want {want})")

    # Size the bottom-right corner with the mouse after picking it.
    menu("s")
    s.inject("key", "Right", "key", "Down", "sleep", 200)
    rx, ry = sized[0] + sized[2] - 1, sized[1] + sized[3] - 1  # the pointer's on the corner
    s.inject("move", rx + 15, ry + 10, "sleep", 100, "move", rx + 40, ry + 30, "sleep", 300,
             "click", "sleep", 200, "move", 900, 600)  # fmt: skip
    final = box("sized-mouse")
    want = (sized[0], sized[1], sized[2] + 40, sized[3] + 30)
    s.check(final == want, f"the mouse sized the corner ({sized} -> {final}, want {want})")

    # Esc puts it back as it was when Size began.
    menu("s")
    s.inject("key", "Left", "key", "Left", "key", "Left", "sleep", 200)
    s.check(box("sizing")[0] == final[0] - 16, "two more Lefts moved the left edge 16px")
    s.inject("key", "Escape")
    s.check(box("escaped") == final, "Esc undid the sizing")


@scenario()
def client_window_menu(s):
    """A window drawing its own title bar (a GTK 3 header bar) asks for the
    window menu on a right-click there (xdg_toplevel.show_window_menu): the
    shell's menu opens at the pointer, and works (Maximize)."""
    if not have_gtk3(s):
        return
    script = os.path.join(s.base, "csd.py")
    with open(script, "w") as f:
        f.write(GTK_CSD)
    empty = s.settle("desktop")
    s.launch(sys.executable, script, env={"GSETTINGS_BACKEND": "memory"})
    s.wait_window(title="Ghost test", state="activated")
    shown = s.settle("window")
    x, y, w, h = shown.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    # On the header bar, left of its title.
    px, py = x + w // 4, y + 14
    s.inject("move", px, py, "sleep", 200, "click", "right")
    _, menu = wait_change(s, shown, (0, 0, WIDTH, HEIGHT - 40), "the window menu", "menu")
    s.check(abs(menu[0] - px) <= 1 and abs(menu[1] - py) <= 1 and menu[3] > 100,
            f"the window menu opened at the pointer ({menu}, pointer {px},{py})")  # fmt: skip
    s.inject("key", "Escape")
    s.wait(lambda: s.shot("closed").differs(shown, menu) == 0, "the menu to close")
    s.log("ok: Esc closed it")
    s.inject("move", px, py, "sleep", 200, "click", "right")
    wait_change(s, shown, (0, 0, WIDTH, HEIGHT - 40), "the window menu", "menu")
    s.inject("key", "x")
    s.wait_window(title="Ghost test", state="maximized")
    s.log("ok: its Maximize maximized the window")


def taskbar_menu(s, item, label):
    """Picks item from the taskbar's menu (classic, bottom taskbar): a row
    counted from Cascade Windows (the third row, after Toolbars and a
    line), or from the bottom when negative (-1 Properties, -2 Lock the
    Taskbar); returns the screen once it's settled."""
    shown = s.shot("before menu")
    s.inject("move", 700, HEIGHT - 14, "click", "right")
    # Below the windows' captions (the menu takes the focus from them).
    _, (mx, my, mw, mh) = wait_change(s, shown, (650, 450, 374, 290), "the menu", "menu")
    y = my + 44 + 22 * item if item >= 0 else my + mh + 9 + 22 * item
    s.inject("move", mx + 40, y, "sleep", 200, "click")
    return s.settle(label)


@scenario()
def taskbar_menu_arrange(s):
    """The taskbar's menu: Cascade Windows, Tile Windows Horizontally, Undo
    Tile (back to the cascade), and Lock the Taskbar toggling."""
    empty = s.settle("desktop")
    for folder in ("Documents", "Pictures"):
        s.launch("xd-files", os.path.join(s.home, folder))
        s.wait_window(title=folder, state="activated")
    s.settle("two windows")

    work = HEIGHT - 40  # above the taskbar

    def captions(shot, x=600):
        """Where title bars cross column x: runs of 12 or more rows in the
        classic active (blue) or inactive (grey) caption colours."""
        runs, start = [], None
        for y in range(work + 1):
            r, g, b = shot.pixel(x, y) if y < work else (255, 255, 255)
            caption = b - r > 60 or (max(r, g, b) - min(r, g, b) <= 2 and 120 <= r <= 200)
            if caption and start is None:
                start = y
            elif not caption and start is not None:
                if y - start >= 12:
                    runs.append(start)
                start = None
        return runs

    def desktop_at(shot, x, y):
        return shot.pixel(x, y) == empty.pixel(x, y)

    cascaded = taskbar_menu(s, 0, "cascaded")
    bars = captions(cascaded)
    s.check(len(bars) == 2 and 20 <= bars[1] - bars[0] <= 32 and bars[0] < 8,
            f"Cascade: two title bars a caption apart at the top ({bars})")  # fmt: skip
    s.check(not desktop_at(cascaded, 10, 10) and not desktop_at(cascaded, WIDTH - 10, 40)
            and desktop_at(cascaded, WIDTH - 10, 10) and desktop_at(cascaded, 10, work - 10),
            "each a step right and down from the one behind")  # fmt: skip

    tiled = taskbar_menu(s, 1, "tiled")
    bars = captions(tiled)
    s.check(len(bars) == 2 and bars[0] < 8 and abs(bars[1] - work // 2) < 12,
            f"Tile Horizontally: one window above the other ({bars})")  # fmt: skip
    s.check(tiled.differs(empty, (0, 0, WIDTH, work), tolerance=0) > 0.99,
            "the two fill the screen above the taskbar")  # fmt: skip
    s.check(not any("maximized" in w["states"] for w in s.windows()), "neither maximized")

    undone = taskbar_menu(s, 3, "undone")
    s.check(undone.differs(cascaded, (0, 0, WIDTH, work), tolerance=0) == 0,
            "Undo Tile put them back as they were (cascaded)")  # fmt: skip

    def locked_key():
        return s.ini().get("Taskbar", "Locked", fallback="")

    s.check(locked_key() in ("", "true"), "the taskbar starts locked")
    taskbar_menu(s, -2, "unlocked")
    s.wait(lambda: locked_key() == "false", "Locked=false")
    unlocked = s.settle("unlocked")
    bar = (0, work, WIDTH, 40)
    s.check(unlocked.differs(undone, bar, step=1) > 0, "Lock the Taskbar unticked: grips show")
    taskbar_menu(s, -2, "locked")
    s.wait(lambda: locked_key() == "true", "Locked=true")
    locked = s.settle("locked")
    s.check(locked.differs(undone, bar, step=1) == 0, "ticked again: as it was")


@scenario()
def csd_window_tiled(s):
    """A window drawing its own title bar and shadow (GTK 3) tiled alone
    fills the screen above the taskbar to the pixel: its window geometry,
    not its shadow, is placed (the shadow's offset used to be taken off
    twice, leaving it short of the right and bottom edges)."""
    if not have_gtk3(s):
        return
    script = os.path.join(s.base, "csd.py")
    with open(script, "w") as f:
        f.write(GTK_CSD)
    s.launch(sys.executable, script, env={"GSETTINGS_BACKEND": "memory"})
    s.wait_window(title="Ghost test", state="activated")
    s.settle("window")
    tiled = taskbar_menu(s, 2, "tiled")  # Tile Windows Vertically
    work = HEIGHT - 40

    def near(a, b):
        return max(abs(p - q) for p, q in zip(tiled.pixel(*a), tiled.pixel(*b))) <= 12

    s.check(near((WIDTH - 1, work // 2), (WIDTH - 5, work // 2)),
            "the window reaches the right edge")  # fmt: skip
    s.check(near((WIDTH // 2, work - 1), (WIDTH // 2, work - 5)),
            "the window reaches the taskbar")  # fmt: skip
    s.check(near((0, 14), (4, 14)) and near((WIDTH // 2, 0), (WIDTH // 2, 4)),
            "and the left and top edges")  # fmt: skip


@scenario()
def gtk_combo_list(s):
    """Other programs' GTK 3 combo boxes (themed through GTK_THEME) drop
    down a white list box below the field, as in Windows, rather than a
    grey menu over it."""
    probe = subprocess.run(
        [sys.executable, "-c", "import gi; gi.require_version('Gtk', '3.0')"],
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode != 0:
        s.log("skipped: no GTK 3 introspection data for Python")
        return
    script = os.path.join(s.base, "combo.py")
    trigger = os.path.join(s.base, "drop-down")
    with open(script, "w") as f:
        f.write(GTK_COMBO)
    empty = s.settle("desktop")
    s.launch(sys.executable, script, trigger,
             env={"GTK_THEME": "XeonDesktop", "GSETTINGS_BACKEND": "memory"})  # fmt: skip
    s.wait_window(title="Combo test", state="activated")
    closed = s.settle("closed")
    win = closed.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(win is not None, "the window is drawn")
    open(trigger, "w").close()
    s.wait(lambda: s.shot("opening").changed_box(closed) is not None, "the drop-down")
    opened = s.settle("open")
    x, y, w, h = opened.changed_box(closed)
    s.check(h >= 5 * 15, f"a list of five rows dropped down ({w}x{h})")
    s.check(y > win[1] + 30, "below the field, not over it")
    white = sum(
        1
        for py in range(y + 2, y + h - 2, 2)
        for px in range(x + 2, x + w - 2, 2)
        if opened.pixel(px, py) == (255, 255, 255)
    )
    total = len(range(y + 2, y + h - 2, 2)) * len(range(x + 2, x + w - 2, 2))
    s.check(white > total // 2, "a white list box, not a menu")


@scenario(LUNA_INI)
def xp_start_letters_and_drag(s):
    """The XP Start menu: a letter picks an item (U: Turn Off Computer, as
    XP's Win, U, U), and a program dragged from it onto Quick Launch
    becomes a shortcut there."""
    with open(os.path.join(s.config, "start-pinned"), "w") as f:
        f.write("org.xeondesktop.Files.desktop\n")
    s.inject("key", "ctrl+Escape", "sleep", 600, "key", "u")
    s.wait_window(title="Shut Down XeonDesktop", state="activated")
    s.log("ok: U chose Turn Off Computer")
    s.inject("key", "Escape")
    s.wait_window(title="Shut Down XeonDesktop", gone=True)
    # The one pinned program is the list's first row; Quick Launch starts
    # just right of the Start button.
    s.inject("key", "ctrl+Escape", "sleep", 600)
    s.settle("menu")
    s.inject("move", 90, 352, "sleep", 200, "down", "sleep", 100, "move", 100, 360, "sleep", 100,
             "move", 110, 600, "sleep", 100, "move", 110, 752, "sleep", 200, "move", 111, 752,
             "sleep", 300, "up", "sleep", 500)
    folder = os.path.join(s.data, "xeondesktop", "quicklaunch")

    def added():
        for name in os.listdir(folder):
            path = os.path.join(folder, name)
            if os.path.islink(path) and os.readlink(path).endswith("org.xeondesktop.Files.desktop"):
                return name
        return None

    name = s.wait(added, "the dropped program in Quick Launch")
    s.check(name.startswith("10-"), f"it went in first, where it was dropped ({name})")


@scenario()
def screen_saver_settings(s):
    """Display Properties' Screen Saver tab: the preview shows each saver
    (moving, or Blank Screen's black standing still); a saver's Settings
    dialog saves its options on OK (Marquee's text, 3D Pipes' Multiple)."""
    empty = s.settle("desktop")
    s.launch("xd-desk", "--page", "1")
    s.wait_window(title="Display Properties", state="activated")
    dialog = s.settle("screen-saver-tab").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dialog is not None, f"Display Properties is up at {dialog}")
    # Focus starts on the tabs; the saver list is next. (None) is chosen.
    s.inject("key", "Tab", "sleep", 200)
    savers = ["3D Pipes", "3D Text", "Beziers", "Blank Screen", "Flying Penguins",
              "Marquee Display", "Mystify Your Mind", "Starfield Simulation"]
    for name in savers:
        label = name.replace(" ", "-").lower()
        s.inject("key", "Down", "sleep", 600)
        a = s.shot(label)
        if name == "Blank Screen":
            time.sleep(0.5)
            moved = s.shot(label).differs(a, dialog, step=2, tolerance=8)
            s.check(moved == 0, "the preview's Blank Screen stands still")
        else:
            # Until it moves: a loaded machine (CI's) starts it late.
            s.wait(lambda: s.shot(label).differs(a, dialog, step=2, tolerance=8) > 0,
                   f"the preview to show {name} moving", timeout=10)  # fmt: skip
            s.log(f"ok: the preview shows {name} moving")

    # Marquee Display (two up): Settings... is the next control.
    s.inject("key", "Up", "sleep", 200, "key", "Up", "sleep", 300, "key", "Tab", "sleep", 200,
             "key", "space")
    s.wait_window(title="Marquee Setup", state="activated")
    s.shot("marquee-setup")  # its Text Example keeps moving
    s.inject("key", "ctrl+a", "type", "Hello there", "sleep", 300)
    s.shot("marquee-text")
    s.inject("key", "Return")
    s.wait_window(title="Marquee Setup", gone=True)
    s.wait(lambda: s.ini().get("ScreenSaver.Marquee", "Text", fallback="") == "Hello there",
           "desktop.ini to have the Marquee's text")
    s.log("ok: Marquee Setup's OK saved the text at once")

    # 3D Pipes: Single to Multiple with the arrow keys.
    s.wait_window(title="Display Properties", state="activated")
    s.inject("key", "shift+Tab", "sleep", 200, "key", "Home", "sleep", 200, "key", "Down",
             "sleep", 300, "key", "Tab", "sleep", 200, "key", "space")
    s.wait_window(title="3D Pipes Setup", state="activated")
    s.settle("pipes-setup")
    s.inject("key", "Down", "sleep", 200, "key", "Return")
    s.wait_window(title="3D Pipes Setup", gone=True)
    s.wait(lambda: s.ini().get("ScreenSaver.Pipes", "Pipes", fallback="") == "multiple",
           "desktop.ini to have Multiple pipes")
    s.log("ok: 3D Pipes Setup saved Multiple")


# ---- several monitors -------------------------------------------------------------

# The primary monitor at scale 1, and a second one to its right at 1.5
# (683x512 logical).
TWO_MONITORS = [(WIDTH, HEIGHT, 1), (WIDTH, HEIGHT, 1.5)]
SECOND_X, SECOND_W, SECOND_H = WIDTH, round(WIDTH / 1.5), round(HEIGHT / 1.5)
BOTH = (0, 0, SECOND_X + SECOND_W, HEIGHT)
# The second monitor alone: the first's taskbar clock may change meanwhile.
SECOND = (SECOND_X, 0, SECOND_W, SECOND_H)


def share_unlike(shot, rect, colour, step=4, tolerance=8):
    """The share (0..1) of sampled pixels in rect not of that colour (a
    monitor at another scale is resampled, a little)."""
    x0, y0, w, h = rect
    total = other = 0
    for y in range(y0, y0 + h, step):
        for x in range(x0, x0 + w, step):
            total += 1
            other += max(abs(p - q) for p, q in zip(shot.pixel(x, y), colour)) > tolerance
    return other / max(total, 1)


def wait_change(s, before, rect, what, label="changed"):
    """Waits until rect differs from `before`, then until it holds still;
    returns the last shot and the bounding box of what changed."""
    s.wait(lambda: s.shot(label).changed_box(before, rect) is not None, what)
    prev = s.shot(label)
    for _ in range(20):
        time.sleep(0.3)
        cur = s.shot(label)
        if cur.changed_box(prev, rect) is None:
            break
        prev = cur
    return cur, cur.changed_box(before, rect)


@scenario(outputs=TWO_MONITORS)
def two_monitors(s):
    """Two monitors at different scales (1 and 1.5): the desktop icons are
    on the primary one (at 0,0), the second monitor's taskbar opens its
    Start menu there, and a window opened there shows its window menu
    (Alt+Space) and maximizes to that monitor's work area."""
    desk = s.settle("desktop")
    bg = desk.pixel(500, 300)
    s.check(share_unlike(desk, (0, 0, 80, 240), bg) > 0.05, "icons on the primary monitor")
    s.check(share_unlike(desk, (SECOND_X + 2, 0, 80, 240), bg) == 0, "none on the second")
    s.check(desk.pixel(SECOND_X + SECOND_W // 2, SECOND_H - 4) != bg,
            "a taskbar on the second monitor")  # fmt: skip
    s.inject("move", SECOND_X + 20, SECOND_H - 12, "click")
    # On the second monitor; the first stays as it was above its taskbar.
    menu_shot, box = wait_change(s, desk, SECOND, "the Start menu", "start-menu")
    s.check(box[3] > 100, f"the Start menu opened on the second monitor ({box})")
    s.check(menu_shot.changed_box(desk, (0, 0, SECOND_X, HEIGHT - 30)) is None,
            "nothing opened on the first")  # fmt: skip
    s.inject("key", "Escape", "move", SECOND_X + 300, 200)
    s.wait(lambda: s.shot("closed").changed_box(desk, SECOND) is None, "the Start menu to close")

    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    opened = s.settle("window")
    # (Above the taskbars, which both have its button now.)
    win = opened.changed_box(desk, (0, 0, SECOND_X + SECOND_W, SECOND_H - 30))
    s.check(win is not None and win[0] >= SECOND_X, f"the window opened on the second ({win})")
    s.inject("key", "alt+space")
    _, menu = wait_change(s, opened, SECOND, "the window menu", "window-menu")
    s.check(menu[0] >= SECOND_X and menu[1] < 120,
            f"Alt+Space's menu under the caption, on the second monitor ({menu})")  # fmt: skip
    # Restore, Move, Size, Minimize, Maximize.
    s.inject("key", "Down", "key", "Down", "key", "Down", "key", "Down", "key", "Down",
             "key", "Return")  # fmt: skip
    s.wait_window(title="Control Panel", state="maximized")
    big = s.settle("maximized")
    s.check(share_unlike(big, (SECOND_X + 40, 60, SECOND_W - 80, SECOND_H - 120), bg) > 0.9,
            "maximized, it fills the second monitor")  # fmt: skip
    start = (SECOND_X + 20, SECOND_H - 12)
    s.check(big.pixel(*start) == desk.pixel(*start), "but not its taskbar")
    s.check(share_unlike(big, (200, 200, 600, 400), bg) == 0, "and not the first monitor")


@scenario(CLASSIC_INI + "\n[Taskbar]\nEdge=left\nAllDisplays=false\n", outputs=TWO_MONITORS)
def window_menu_without_taskbar(s):
    """A left taskbar only on the primary monitor: a window on the second
    still gets its window menu (Alt+Space) under its caption icon there."""
    desk = s.settle("desktop")
    bg = desk.pixel(500, 300)
    s.check(desk.pixel(10, HEIGHT // 2) != bg, "the taskbar is on the primary monitor's left")
    s.check(share_unlike(desk, (SECOND_X + 2, 2, SECOND_W - 6, SECOND_H - 6), bg) == 0,
            "and not on the second")  # fmt: skip
    s.inject("move", SECOND_X + 300, 200)
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    opened = s.settle("window")
    s.inject("key", "alt+space")
    _, menu = wait_change(s, opened, SECOND, "the window menu", "window-menu")
    s.check(menu[0] >= SECOND_X and menu[1] < 120,
            f"the menu under the caption, on the second monitor ({menu})")  # fmt: skip
    s.inject("key", "Escape")
    s.wait(lambda: s.shot("closed").changed_box(opened, SECOND) is None, "the menu to close")
    s.log("ok: Esc closed it")


@scenario(outputs=TWO_MONITORS)
def lock_two_monitors(s):
    """Lock Computer covers both monitors, and its dialog shows on the one
    with the pointer (the second), as Unlock Computer then does."""
    desk = s.settle("desktop")
    s.inject("move", SECOND_X + 300, 200, "key", "super+l")
    bars = ((WIDTH // 2, HEIGHT - 4), (SECOND_X + SECOND_W // 2, SECOND_H - 4))
    s.wait(lambda: all(s.shot("locking").pixel(x, y) != desk.pixel(x, y) for x, y in bars),
           "both taskbars to be covered", 30)  # fmt: skip
    locked = s.settle("locked")
    bg = locked.pixel(100, 100)
    s.check(share_unlike(locked, (0, 0, WIDTH, HEIGHT), bg) == 0,
            "the first monitor is covered, without the dialog")  # fmt: skip
    s.check(share_unlike(locked, (SECOND_X, 0, SECOND_W, SECOND_H), bg) > 0.1,
            "the Computer Locked dialog is on the second")  # fmt: skip
    s.inject("key", "ctrl+alt+Delete")
    _, box = wait_change(s, locked, BOTH, "Unlock Computer", "unlock")
    s.check(box[0] >= SECOND_X, f"Unlock Computer is on the second monitor too ({box})")


@scenario(outputs=TWO_MONITORS)
def identify_monitors(s):
    """Display Properties' Identify (Settings tab): each monitor shows its
    number, big, white outlined in black, at its top left, for a few
    seconds."""
    s.inject("move", WIDTH // 2, HEIGHT // 2)  # Display Properties opens on the first
    empty = s.settle("desktop")
    s.launch("xd-desk", "--page", "4")
    s.wait_window(title="Display Properties", state="activated")
    tab = s.settle("settings-tab")
    x, y, w, h = tab.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    # The dialog's client area is 404x448, framed evenly at the sides and
    # bottom; Identify is at (309, 338), 75x23.
    edge = (w - 404) // 2
    left, top = x + edge, y + h - edge - 448
    s.inject("move", left + 346, top + 349, "click", "move", WIDTH // 2, HEIGHT // 2)
    first = (0, 0, 300, 300)
    second = (SECOND_X, 0, 300, 300)
    shot = s.wait(lambda: (lambda sh: sh if sh.changed_box(tab, first) and
                           sh.changed_box(tab, second) else None)(s.shot("identify")),
                  "numbers on both monitors", 10)  # fmt: skip
    boxes = [shot.changed_box(tab, r) for r in (first, second)]
    for name, (bx, by, bw, bh) in zip(("first", "second"), boxes):
        s.check(bh > 120 and bx < (SECOND_X if name == "second" else 0) + 80,
                f"a big number at the top left of the {name} monitor ({bx},{by} {bw}x{bh})")
    s.check(abs(boxes[0][2] - boxes[1][2]) > 8, f"and the two are different numbers ({boxes})")
    s.wait(lambda: s.shot("gone").changed_box(tab, first) is None and
           s.shot("gone").changed_box(tab, second) is None, "the numbers to go", 10)  # fmt: skip
    s.log("ok: the numbers went away by themselves")


LANG_INI = LUNA_INI + "\n[Keyboard]\nLayout=us,de\nOptions=grp:alt_shift_toggle\n"

ENTRY_TEST = r"""
import sys
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
w = Gtk.Window(title="Entry test")
w.set_default_size(300, 100)
e = Gtk.Entry()
def changed(entry):
    with open(sys.argv[1], "w") as f:
        f.write(entry.get_text())
e.connect("changed", changed)
w.add(e)
w.connect("destroy", Gtk.main_quit)
w.show_all()
Gtk.main()
"""


def inject_two_layouts(s, *steps):
    """xd-inject with the session's two layouts and switching keys, so
    Alt+Shift switches and typing follows the active layout."""
    env = dict(os.environ, XKB_DEFAULT_LAYOUT="us,de", XKB_DEFAULT_OPTIONS="grp:alt_shift_toggle")
    r = subprocess.run([s.tool("xd-inject"), *map(str, steps)], env=env,
                       stderr=subprocess.PIPE, timeout=60)  # fmt: skip
    if r.returncode != 0:
        raise Failure("xd-inject: " + r.stderr.decode().strip())


@scenario(LANG_INI)
def language_indicator(s):
    """With two keyboard layouts, the taskbar shows the language indicator
    (EN); Alt+Shift switches the layout and it shows DE; its menu switches
    back, and typing follows the layout chosen there (German: y and z
    swapped)."""
    desk = s.settle("desktop")
    near_tray = (WIDTH - 300, HEIGHT - 30, 236, 30)  # up to the clock
    inject_two_layouts(s, "hold", "alt", "key", "Shift_L", "release", "alt")
    german, box = wait_change(s, desk, near_tray, "the indicator to change", "alt-shift")
    s.check(box[2] < 40, f"Alt+Shift changed the indicator only ({box})")
    s.inject("move", box[0] + box[2] // 2, box[1] + box[3] // 2, "click")
    _, menu = wait_change(s, german, (WIDTH - 400, HEIGHT - 300, 400, 270), "the menu", "menu")
    s.check(menu[1] + menu[3] <= HEIGHT - 28, f"its menu opened above the taskbar ({menu})")
    # The first item: English.
    s.inject("key", "Down", "key", "Return", "move", 500, 300)
    s.wait(lambda: s.shot("english").changed_box(desk, near_tray) is None, "EN again")
    s.log("ok: the menu switched back to English")

    probe = subprocess.run(
        [sys.executable, "-c", "import gi; gi.require_version('Gtk', '3.0')"],
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode != 0:
        s.log("skipped typing: no GTK 3 introspection data for Python")
        return
    script = os.path.join(s.base, "entry.py")
    typed = os.path.join(s.base, "typed")
    with open(script, "w") as f:
        f.write(ENTRY_TEST)
    s.launch(sys.executable, script, typed, env={"GSETTINGS_BACKEND": "memory"})
    s.wait_window(title="Entry test", state="activated")
    s.inject("move", box[0] + box[2] // 2, box[1] + box[3] // 2, "click", "sleep", 500,
             "key", "Down", "key", "Down", "key", "Return", "move", 500, 300)  # fmt: skip
    s.wait(lambda: s.shot("german").changed_box(german, near_tray) is None, "DE again")
    inject_two_layouts(s, "type", "yz")

    def text():
        try:
            with open(typed) as f:
                return f.read()
        except OSError:
            return ""

    s.wait(lambda: len(text()) == 2, "the typing")
    s.check(text() == "zy", f"typed with the German layout ({text()!r})")


def mock_polkitd(s):
    """Before the compositor starts: a private system bus with dbusmock's
    polkitd, for the session's agent to register with."""
    from dbusmock import DBusTestCase

    DBusTestCase.start_system_bus()
    log = open(os.path.join(s.base, "polkitd.log"), "w")
    proc, _ = DBusTestCase.spawn_server_template("polkitd", {}, stdout=log, system_bus=True)
    s.children.append(proc)


def session_program(name):
    """Pids of this session's processes running `name`."""
    pids = []
    for pid in session_pids(os.environ["XDG_RUNTIME_DIR"]):
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip() == name:
                    pids.append(pid)
        except OSError:
            pass
    return pids


@scenario(requires=("dbusmock",), setup=mock_polkitd)
def polkit_agent_restarts(s):
    """The polkit agent the compositor started is started again when it
    crashes, as the shell is."""

    def registrations():
        with open(os.path.join(s.base, "polkitd.log")) as f:
            return f.read().count("RegisterAuthenticationAgent")

    deadline = time.monotonic() + 15
    while registrations() == 0 and time.monotonic() < deadline:
        time.sleep(0.2)
    if registrations() == 0:
        # polkit finds the login session from the process's cgroup: not
        # there when the tests run outside a login session.
        s.log("skipped: the agent couldn't register (no login session?)")
        return
    first = s.wait(lambda: session_program("xd-polkit"), "the agent")
    os.kill(first[0], signal.SIGABRT)
    s.wait(lambda: [p for p in session_program("xd-polkit") if p not in first],
           "the agent to be started again")  # fmt: skip
    s.wait(lambda: registrations() >= 2, "it to register again")
    s.log("ok: the crashed agent was started again and registered")


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
