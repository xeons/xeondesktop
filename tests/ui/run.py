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
# for the programs it starts); "x11", whether Xwayland can run (a private
# /tmp/.X11-unix, as the real one's owner, root, isn't ours in the session's
# user namespace).
SCENARIO_OPTIONS = {}


def scenario(desktop_ini=CLASSIC_INI, requires=(), outputs=None, setup=None, x11=False):
    """Registers a scenario: a function of the session (Session), run with
    that desktop.ini; it's skipped without the Python modules it requires."""

    def register(fn):
        SCENARIOS[fn.__name__] = (fn, desktop_ini, requires)
        SCENARIO_OPTIONS[fn.__name__] = {
            "outputs": outputs or [(WIDTH, HEIGHT, 1)],
            "setup": setup,
            "x11": x11,
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

    # Nothing from the desktop the tests run on (its GTK and Qt themes,
    # cursor, XeonDesktop's own session variables): a scenario sees the
    # same environment here as in CI, setting what it needs itself.
    session_prefixes = ("GTK_", "QT_", "XEONDESKTOP_", "XCURSOR_", "XDG_SESSION_", "MOZ_",
                        "SDL_", "_JAVA_")
    env = {
        k: v
        for k, v in os.environ.items()
        if not k.startswith(session_prefixes)
        and k not in ("XDG_CURRENT_DESKTOP", "XMODIFIERS", "GDK_BACKEND")
        and k
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
    # Xwayland's sockets: an empty directory of the session's own, ours
    # (as root here is) once back to our uid.
    if SCENARIO_OPTIONS[name]["x11"]:
        binds.append("if [ -d /tmp/.X11-unix ]; then mount -t tmpfs -o mode=1777 tmpfs "
                     "/tmp/.X11-unix; fi")  # fmt: skip
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


FILES = "org.xeondesktop.Files"


def files_windows(s):
    return [w for w in s.windows() if w["app_id"] == FILES]


def files_title(s, title, what):
    """Waits for the one xd-files window to be titled title."""
    s.wait(lambda: [w["title"] for w in files_windows(s)] == [title], what)


def open_documents(s):
    """xd-files on Documents (Letters, then Notes.txt), the pointer out of
    the way; returns the window's top left corner and its screenshot."""
    empty = s.settle("desktop")
    s.launch("xd-files", os.path.join(s.home, "Documents"))
    s.wait_window(app_id=FILES, state="activated")
    files_title(s, "Documents", "Documents to open")
    s.inject("move", WIDTH - 24, HEIGHT - 80)
    shot = s.settle("documents")
    x, y, _, _ = shot.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    return x, y, shot


# From the window's top left corner (Classic): the first item (Letters),
# the Folders tree's first root (Home), and where the tab strip shows.
LETTERS = (264, 148)
TREE_HOME = (60, 143)


def strip_rect(x, y):
    return (x + 222, y + 115, 360, 24)


@scenario()
def files_tabs(s):
    """xd-files' tabs: Ctrl+T opens My Computer in a new tab of the same
    window, the tab strip shows with two tabs and goes again at one, a tab
    dragged along the strip changes places (Ctrl+1, Ctrl+2 follow), and
    Ctrl+W closes the tab, then the window with the last one."""
    x, y, one = open_documents(s)
    s.inject("key", "ctrl+t")
    files_title(s, "My Computer", "Ctrl+T to open My Computer in a tab")
    two = s.settle("two tabs")
    s.check(len(files_windows(s)) == 1, "Ctrl+T opened a tab, not a window")
    s.check(two.differs(one, strip_rect(x, y)) > 0.1, "the tab strip shows with two tabs")
    # Dragged to the left of Documents, My Computer is the first tab.
    steps = ["move", x + 400, y + 128, "down", "sleep", 200]
    for dx in (380, 350, 300, 260, 230):
        steps += ["move", x + dx, y + 128, "sleep", 100]
    s.inject(*steps, "up", "sleep", 500, "key", "ctrl+2")
    files_title(s, "Documents", "Ctrl+2 to show Documents, now second")
    s.inject("key", "ctrl+1")
    files_title(s, "My Computer", "Ctrl+1 to show My Computer, dragged first")
    s.log("ok: dragging a tab reordered the tabs")
    s.inject("key", "ctrl+w")
    files_title(s, "Documents", "Ctrl+W to close the tab")
    again = s.settle("one tab again")
    s.check(again.differs(one, strip_rect(x, y)) < 0.02, "the strip went with the second tab")
    s.inject("key", "ctrl+w")
    s.wait_window(app_id=FILES, gone=True)
    s.log("ok: Ctrl+W on the last tab closed the window")


@scenario()
def files_tab_middle_click(s):
    """The middle button opens a folder in a new tab behind the one shown,
    from the view and from the Folders tree; Ctrl+Tab and Ctrl+9 go to
    them."""
    x, y, one = open_documents(s)
    s.inject("move", x + LETTERS[0], y + LETTERS[1], "click", "middle")
    s.wait(lambda: s.shot("middle").differs(one, strip_rect(x, y)) > 0.1, "a second tab")
    s.check([w["title"] for w in files_windows(s)] == ["Documents"],
            "Letters opened in a tab behind Documents")  # fmt: skip
    s.inject("key", "ctrl+Tab")
    files_title(s, "Letters", "Ctrl+Tab to show Letters")
    s.log("ok: Ctrl+Tab went to the Letters tab")
    home = os.path.basename(os.path.realpath(s.home))
    s.inject("move", x + TREE_HOME[0], y + TREE_HOME[1], "click", "middle", "sleep", 1000)
    s.check([w["title"] for w in files_windows(s)] == ["Letters"],
            "the tree's Home opened behind Letters")  # fmt: skip
    s.inject("key", "ctrl+9")
    files_title(s, home, "Ctrl+9 to show the last tab, Home")
    s.inject("key", "ctrl+shift+Tab")
    files_title(s, "Letters", "Ctrl+Shift+Tab to go back to Letters")
    s.inject("key", "ctrl+1")
    files_title(s, "Documents", "Ctrl+1 to show the first tab")
    s.settle("three tabs")


@scenario()
def files_tab_history(s):
    """Each tab has its own Back history: a new tab can't go back, the
    first one goes back where it was."""
    x, y, _ = open_documents(s)
    s.inject("move", x + LETTERS[0], y + LETTERS[1], "click", "sleep", 100, "click")
    files_title(s, "Letters", "Letters to open")
    s.inject("key", "ctrl+t")
    files_title(s, "My Computer", "Ctrl+T")
    s.inject("key", "alt+Left", "sleep", 1000)
    s.check([w["title"] for w in files_windows(s)] == ["My Computer"],
            "the new tab has nothing to go back to")  # fmt: skip
    s.inject("key", "ctrl+1")
    files_title(s, "Letters", "Ctrl+1 to show the first tab")
    s.inject("key", "alt+Left")
    files_title(s, "Documents", "Back in the first tab to Documents")
    s.inject("key", "ctrl+2")
    files_title(s, "My Computer", "the second tab still on My Computer")
    s.settle("history")


@scenario()
def files_undo_copy_merge(s):
    """Undo Copy after a copy that merged into a folder and replaced a file
    sends only what the copy brought in to the Recycle Bin: the files that
    were in the folder before stay, and so does the replaced file (its old
    contents are gone either way)."""
    def write(path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)

    incoming = os.path.join(s.home, "Incoming")
    archive = os.path.join(s.home, "Archive")
    write(os.path.join(incoming, "notes.txt"), "notes")
    write(os.path.join(incoming, "project", "new.txt"), "new")
    write(os.path.join(incoming, "project", "same.txt"), "copied")
    write(os.path.join(archive, "project", "important.txt"), "keep me")
    write(os.path.join(archive, "project", "same.txt"), "old")

    s.launch("xd-files", incoming)
    s.wait_window(app_id=FILES, state="activated")
    files_title(s, "Incoming", "Incoming to open")
    s.settle("incoming")
    s.inject("key", "ctrl+a", "key", "ctrl+c", "key", "ctrl+l", "key", "ctrl+a",
             "type", archive, "key", "Return")  # fmt: skip
    files_title(s, "Archive", "Archive to open")
    s.settle("archive")
    # Confirm Folder Replace (merge), then Confirm File Replace: Yes to each.
    s.inject("key", "ctrl+v", "sleep", 1500, "key", "Return", "sleep", 1500, "key", "Return")
    same = os.path.join(archive, "project", "same.txt")
    s.wait(lambda: os.path.exists(os.path.join(archive, "notes.txt"))
           and os.path.exists(os.path.join(archive, "project", "new.txt"))
           and file_text(same) == "copied", "the copy, merged")  # fmt: skip
    s.settle("copied")
    # Undo Copy asks whether to send the copies to the Recycle Bin: Yes.
    s.inject("key", "ctrl+z", "sleep", 1500, "key", "Return")
    s.wait(lambda: not os.path.exists(os.path.join(archive, "notes.txt"))
           and not os.path.exists(os.path.join(archive, "project", "new.txt")),
           "Undo Copy to take back what the copy brought in")  # fmt: skip
    s.inject("sleep", 1000)
    s.check(file_text(os.path.join(archive, "project", "important.txt")) == "keep me",
            "the folder's own file stayed")  # fmt: skip
    s.check(file_text(same) == "copied", "and the replaced file stayed where it was")
    trashed = sorted(os.listdir(os.path.join(s.data, "Trash", "files")))
    s.check(trashed == ["new.txt", "notes.txt"], f"only the new files were sent ({trashed})")


@scenario()
def files_extract(s):
    """An archive's right-click menu: Extract Here puts what's in it beside
    it (Undo Extract sends that to the Recycle Bin), and Extract to
    "photos" makes a folder of the archive's name."""
    import zipfile

    if not os.path.exists(os.path.join(s.build, "xd-files")):
        return
    box = os.path.join(s.home, "Box")
    os.makedirs(box)
    with zipfile.ZipFile(os.path.join(box, "photos.zip"), "w") as z:
        z.writestr("a.txt", "first")
        z.writestr("sub/b.txt", "second")
    empty = s.settle("desktop")
    s.launch("xd-files", box)
    s.wait_window(app_id=FILES, state="activated")
    files_title(s, "Box", "Box to open")
    s.inject("move", WIDTH - 24, HEIGHT - 80)
    x, y, _, _ = s.settle("box").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    item = (x + LETTERS[0], y + LETTERS[1])

    s.inject("move", *item, "click", "sleep", 200, "click", "right", "sleep", 600)
    s.settle("menu")
    s.inject("key", "h")
    a = os.path.join(box, "a.txt")
    b = os.path.join(box, "sub", "b.txt")
    s.wait(lambda: file_text(a) == "first" and file_text(b) == "second", "Extract Here")
    s.log("ok: Extract Here put the archive's files beside it")
    s.check(not any(n.startswith(".xd-extract") for n in os.listdir(box)),
            "and left no working folder behind")  # fmt: skip

    s.inject("sleep", 500, "key", "ctrl+z", "sleep", 1500, "key", "Return")
    s.wait(lambda: not os.path.exists(a) and not os.path.exists(os.path.join(box, "sub")),
           "Undo Extract")  # fmt: skip
    s.log("ok: Undo Extract took them back")

    s.inject("sleep", 500, "move", *item, "click", "sleep", 200, "click", "right", "sleep", 600)
    s.settle("menu again")
    s.inject("key", "x")
    folder = os.path.join(box, "photos")
    s.wait(lambda: file_text(os.path.join(folder, "a.txt")) == "first"
           and file_text(os.path.join(folder, "sub", "b.txt")) == "second",
           'Extract to "photos"')  # fmt: skip
    s.log('ok: Extract to "photos" made the folder')
    s.check(sorted(os.listdir(box)) == ["photos", "photos.zip"],
            f"and nothing else ({sorted(os.listdir(box))})")  # fmt: skip


@scenario()
def folder_options_tabs(s):
    """Folder Options' "Open folders in new tabs instead of new windows":
    saved in [Folders], and with "Open each folder in its own window" a
    folder opens in a tab of the same window."""
    empty = s.settle("desktop")
    s.launch("xd-folders")
    s.wait_window(title="Folder Options", state="activated")
    x, y, w, h = s.settle("folder options").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.inject("key", "alt+w", "sleep", 300, "key", "alt+n", "sleep", 300)
    s.settle("ticked")
    # OK, bottom right (Alt+A is also "s_ame window"'s).
    s.inject("move", x + w - 208, y + h - 21, "click")
    s.wait(lambda: s.ini().has_section("Folders")
           and s.ini()["Folders"].get("NewTabs") == "true"
           and s.ini()["Folders"].get("NewWindow") == "true", "NewTabs=true saved")  # fmt: skip
    s.log("ok: NewTabs=true and NewWindow=true saved in [Folders]")
    s.wait_window(title="Folder Options", gone=True)
    x, y, _ = open_documents(s)
    s.inject("move", x + LETTERS[0], y + LETTERS[1], "click", "sleep", 100, "click")
    files_title(s, "Letters", "Letters to open in a tab of the same window")
    s.settle("letters tab")
    s.inject("key", "ctrl+w")
    files_title(s, "Documents", "Ctrl+W to close the Letters tab")
    s.log("ok: Letters opened in a tab, not a window of its own")


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
def advanced_appearance(s):
    """Display Properties' Advanced Appearance: a click on the preview's
    title bar selects Active Title Bar (its Color 2 comes on); the Desktop's
    Color 1 drop-down sets red; OK and OK write [CustomScheme], the desktop
    turns red, and the editor shows red again when opened anew."""
    empty = s.settle("desktop")
    s.launch("xd-desk", "--page", "2")
    s.wait_window(title="Display Properties", state="activated")
    props = s.settle("appearance")
    dp = props.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dp is not None, f"Display Properties is up at {dp}")

    def open_advanced():
        # Tabs, style, scheme, font size, then Advanced.
        s.inject("key", "Tab", "key", "Tab", "key", "Tab", "key", "Tab", "sleep", 200,
                 "key", "Return")
        s.wait_window(title="Advanced Appearance", state="activated")
        shot = s.settle("advanced")
        # Below Display Properties' caption (which only turns inactive).
        below = dp[1] + 30
        box = shot.changed_box(props, (0, below, WIDTH, HEIGHT - 40 - below))
        s.check(box is not None, f"Advanced Appearance is up at {box}")
        # The client area (404 x 362), inside the dialog frame, from the
        # bottom: what changed at the top may include Display Properties'
        # tabs losing focus.
        return shot, box[0] + 2, box[1] + box[3] - 365

    shot, cx, cy = open_advanced()
    color2 = (cx + 340, cy + 243, 40, 12)  # Color 2's swatch
    # The active window's title bar in the preview.
    s.inject("move", cx + 150, cy + 52, "click", "sleep", 400)
    title = s.settle("title-bar-picked")
    s.check(title.differs(shot, color2, step=2) > 0.3,
            "a click on the preview's title bar picked Active Title Bar (Color 2 is on)")

    # Back to the Desktop: the preview's desktop, right of the inactive
    # window. Its Color 1 drops down the palette; red is in the fourth row.
    s.inject("move", cx + 372, cy + 22, "click", "sleep", 400)
    s.check(s.settle("desktop-picked").differs(title, color2, step=2) > 0.3,
            "a click on the preview's desktop picked Desktop (Color 2 is off)")
    s.inject("move", cx + 298, cy + 248, "click", "sleep", 500)
    s.settle("palette")
    s.inject("move", cx + 270 + 31, cy + 260 + 67, "click", "sleep", 400)
    picked = s.settle("red-picked")
    r, g, b = picked.pixel(cx + 22, cy + 160)  # the preview's desktop, bottom left
    s.check(r > 200 and g < 40 and b < 40, f"the preview's desktop is red ({r},{g},{b})")
    s.inject("key", "Return")  # OK
    s.wait_window(title="Advanced Appearance", gone=True)
    s.wait_window(title="Display Properties", state="activated")
    s.inject("key", "Return")  # OK: apply and close
    s.wait_window(title="Display Properties", gone=True)

    def written():
        c = s.ini()
        return c.has_section("CustomScheme") and c["CustomScheme"].get("Background") == "255 0 0"

    s.wait(written, "desktop.ini to take the custom scheme")
    s.log("ok: [CustomScheme] Background=255 0 0")

    def red_desktop():
        r, g, b = s.shot("desktop-red").pixel(WIDTH - 60, 60)
        return r > 200 and g < 40 and b < 40

    s.wait(red_desktop, "the desktop to turn red")
    s.log("ok: the shell took the custom scheme")

    # Opened again, the editor starts from what was saved.
    empty = s.settle("desktop-red")
    s.launch("xd-desk", "--page", "2")
    s.wait_window(title="Display Properties", state="activated")
    props = s.settle("appearance-again")
    dp = props.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    shot, cx, cy = open_advanced()
    r, g, b = shot.pixel(cx + 290, cy + 248)  # Color 1's swatch
    s.check(r > 200 and g < 40 and b < 40, f"Desktop's Color 1 is red again ({r},{g},{b})")
    s.inject("key", "Escape")
    s.wait_window(title="Advanced Appearance", gone=True)
    s.inject("key", "Escape")
    s.wait_window(title="Display Properties", gone=True)


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


@scenario(requires=("dbusmock",))
def disk_management_startup_format(s):
    """Disk Management (on a dbusmock UDisks2): Change Mount Point adds,
    changes and removes the USB stick's /etc/fstab entry through UDisks2's
    configuration items (UUID=, nofail, the user's uid for FAT), mounting
    it there; then a full format with a chosen allocation unit size asks
    Format for "erase": "zero" and mkfs's cluster size."""
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    try:
        mock = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "udisks2.py")
        proc, _ = DBusTestCase.spawn_server_template(
            mock, {}, stdout=subprocess.DEVNULL, system_bus=True
        )
        s.children.append(proc)
        udisks = DBusTestCase.get_dbus(system_bus=True).get_object(
            "org.freedesktop.UDisks2", "/org/freedesktop/UDisks2"
        )
        sdd1 = "/org/freedesktop/UDisks2/block_devices/sdd1"

        def calls():
            return [str(c) for c in udisks.MockCalls(dbus_interface=MOCK_IFACE)]

        def called(what):
            return what in calls()

        empty = s.settle("desktop")
        s.launch("xd-diskmgmt")
        s.wait_window(title="Disk Management", state="activated")
        shot = s.settle("disk-management")
        x, y, _, _ = shot.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
        def opened(what, label):
            """Waits for a dialog over Disk Management (xd-windows doesn't
            list dialogs, but the main window loses the focus), so a key
            gone astray fails here rather than several steps on."""
            s.wait(lambda: "activated" not in s.window(title="Disk Management")["states"],
                   f"the {what} dialog")  # fmt: skip
            s.settle(label)

        def change_mount_point(label):
            s.inject("key", "alt+a", "sleep", 400, "key", "p")
            opened("Change Mount Point", label)

        # The volume list's second row, root (/): the system's own
        # /etc/fstab entry can't be removed (or changed) here.
        s.inject("move", x + 120, y + 70, "click", "sleep", 300, "key", "Down", "sleep", 300)
        change_mount_point("root's mount points")
        s.inject("key", "alt+r", "sleep", 500, "key", "alt+h", "sleep", 500)
        s.check(not any("ConfigurationItem" in c for c in calls()), "root's entry stays as it is")
        s.inject("key", "Escape", "sleep", 500)
        s.log("ok: the system volume's startup mount point is locked")

        # Down to USBSTICK (sda1-3, Data, Stuff, USBSTICK).
        for _ in range(4):
            s.inject("key", "Down", "sleep", 100)
        s.settle("usbstick")

        uid, gid = os.getuid(), os.getgid()
        opts = f"nofail,x-mount.mkdir,uid={uid},gid={gid}"
        entry = (f"fsname=UUID=uuid-sdd1 dir=/mnt/USBSTICK type=vfat opts={opts} freq=0 "
                 "passno=0")

        # Add: the suggested folder and options; it isn't mounted, so it's
        # mounted there.
        change_mount_point("change mount point")
        s.inject("key", "alt+a")
        opened("Add Startup Mount Point", "add startup mount point")
        s.inject("key", "Return")
        s.wait(lambda: called(f"AddConfigurationItem {sdd1} {entry}"), "AddConfigurationItem")
        s.wait(lambda: called(f"Mount {sdd1}"), "the stick mounted at its startup mount point")
        s.log("ok: Add made the fstab entry through UDisks2 and mounted it")

        # Change: a new folder; it's mounted at the old one, so it moves
        # (Yes, the default).
        s.settle("added")
        change_mount_point("change mount point 2")
        s.inject("key", "alt+h")
        opened("Change Startup Mount Point", "change startup mount point")
        s.inject("key", "ctrl+a", "type", "/mnt/stick", "key", "Return")
        opened("Disk Management", "move it now")
        s.inject("key", "Return")
        moved = entry.replace("dir=/mnt/USBSTICK", "dir=/mnt/stick")
        s.wait(lambda: called(f"UpdateConfigurationItem {sdd1} {entry} -> {moved}"),
               "UpdateConfigurationItem")  # fmt: skip
        s.wait(lambda: calls()[-2:] == [f"Unmount {sdd1}", f"Mount {sdd1}"],
               "the stick moved to the new folder")  # fmt: skip
        s.log("ok: Change updated the entry and moved the stick")

        # Remove, after a confirmation (No is its default).
        s.settle("changed")
        change_mount_point("change mount point 3")
        s.inject("key", "alt+r")
        opened("Remove Startup Mount Point", "remove?")
        s.inject("key", "alt+y")
        s.wait(lambda: called(f"RemoveConfigurationItem {sdd1} {moved}"),
               "RemoveConfigurationItem")  # fmt: skip
        s.log("ok: Remove removed the entry")

        # Format: FAT32, 4096 bytes (after Default, the sizes from the
        # disk's sector size up: 512, 1024, 2048, 4096), quick format
        # unticked; it's mounted, so unmounted first. The sector size is
        # read from sysfs, by the device's name.
        sector = 512
        try:
            with open("/sys/class/block/sdd/queue/logical_block_size") as f:
                sector = int(f.read())
        except (OSError, ValueError):
            pass
        s.settle("removed")
        # The context menu: the Action menu's F is Format and Refresh both.
        s.inject("key", "Menu", "sleep", 400, "key", "f")
        opened("Format USBSTICK", "format")
        s.inject("key", "alt+a", "sleep", 200)
        for _ in [u for u in (512, 1024, 2048, 4096) if u >= sector]:
            s.inject("key", "Down", "sleep", 100)
        s.inject("key", "alt+q", "sleep", 200)
        s.settle("format full 4096")
        s.inject("key", "alt+v", "sleep", 200, "key", "Return")
        opened("Format", "format confirm")
        s.inject("key", "alt+y")
        sectors = 4096 // sector
        full = f"Format {sdd1} vfat label=USBSTICK tear-down=True erase=zero mkfs-args=-s,{sectors}"
        s.wait(lambda: calls()[-2:] == [f"Unmount {sdd1}", full],
               "an unmount, then a full format with 4096-byte clusters")  # fmt: skip
        s.log("ok: a full format with 4096-byte clusters")
        s.inject("key", "alt+F4")
        s.wait_window(title="Disk Management", gone=True, timeout=10)
    finally:
        DBusTestCase.tearDownClass()


@scenario(requires=("dbusmock",))
def drive_rename_tools_cleanup(s):
    """My Computer's drive (on a dbusmock UDisks2, where / is sda2, label
    "root"): F2 renames it through Filesystem.SetLabel; its Properties'
    Tools tab checks it (Automatically fix: Repair, refused as it's
    mounted), Hardware lists the mock's drives, and Disk Cleanup empties
    the Recycle Bin, the thumbnails and Firefox's cache in the scratch
    home, leaving everything else."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    cache = os.environ["XDG_CACHE_HOME"]
    keep = os.path.join(cache, "other", "keep")
    junk = [
        os.path.join(s.data, "Trash", "files", "old report.txt"),
        os.path.join(s.data, "Trash", "info", "old report.txt.trashinfo"),
        os.path.join(cache, "thumbnails", "normal", "0123.png"),
        os.path.join(cache, "mozilla", "firefox", "abc.default", "cache2", "entries", "E1"),
    ]
    for path in junk + [keep]:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b"x" * 100000)

    DBusTestCase.start_system_bus()
    try:
        mock = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "udisks2.py")
        proc, _ = DBusTestCase.spawn_server_template(
            mock, {}, stdout=subprocess.DEVNULL, system_bus=True
        )
        s.children.append(proc)
        udisks = DBusTestCase.get_dbus(system_bus=True).get_object(
            "org.freedesktop.UDisks2", "/org/freedesktop/UDisks2"
        )
        sda2 = "/org/freedesktop/UDisks2/block_devices/sda2"

        def called(what):
            return any(c == what for c in udisks.MockCalls(dbus_interface=MOCK_IFACE))

        empty = s.settle("desktop")
        s.launch("xd-files", "computer:///")
        s.wait_window(title="My Computer", state="activated")
        x, y, _, _ = s.settle("my computer").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
        # The first of Hard Disk Drives, under My Documents' group.
        s.inject("move", x + 264, y + 278, "click", "sleep", 300, "key", "F2", "sleep", 300,
                 "key", "ctrl+a", "type", "Win2Root", "key", "Return")  # fmt: skip
        s.wait(lambda: called(f"SetLabel {sda2} Win2Root"), "F2 to set the label")
        s.log("ok: renaming the drive in place called SetLabel")

        def files_active():
            w = s.window(title="My Computer")
            return w is not None and "activated" in w["states"]

        # Its Properties: Tools (Ctrl+Tab), Check Now... with Automatically
        # fix ticked asks for Repair, which UDisks2 refuses: / is mounted.
        s.inject("key", "alt+Return")
        s.wait(lambda: not files_active(), "the drive's Properties")
        s.settle("properties")
        s.inject("key", "ctrl+Tab", "sleep", 300, "key", "alt+c", "sleep", 800, "key", "alt+a",
                 "sleep", 200, "key", "alt+s")  # fmt: skip
        s.wait(lambda: called(f"Repair {sda2}"), "Check Disk to ask UDisks2 for Repair")
        s.check(not called(f"Check {sda2}"), "Automatically fix asked for Repair, not Check")
        s.settle("check refused")
        s.inject("key", "Return", "sleep", 500)  # the message's OK
        # Hardware: the mock's drives; Properties opens the selected one's.
        s.inject("key", "ctrl+Tab", "sleep", 300)
        hardware = s.settle("hardware")
        s.inject("key", "alt+r")
        device = s.settle("device properties")
        box = device.changed_box(hardware, (0, 0, WIDTH, HEIGHT - 40))
        s.check(box is not None and box[2] > 200 and box[3] > 200,
                f"the disk drive's Properties opened ({box})")  # fmt: skip
        s.inject("key", "Escape", "sleep", 500)

        # General's Disk Cleanup: tick the Recycle Bin (second), OK, Yes.
        s.inject("key", "ctrl+Tab", "sleep", 300, "key", "alt+u")
        s.settle("disk cleanup")
        s.inject("key", "Down", "sleep", 200, "key", "space", "sleep", 200, "key", "Return",
                 "sleep", 800)  # fmt: skip
        s.settle("are you sure")
        s.inject("key", "Return")
        s.wait(lambda: not any(os.path.exists(p) for p in junk), "Disk Cleanup to delete")
        s.check(os.path.exists(keep) and os.path.isdir(os.path.dirname(os.path.dirname(junk[3]))),
                "Disk Cleanup left the rest of the cache, and the cache folders")  # fmt: skip
        s.settle("cleaned")
        s.inject("key", "Escape")
        s.wait(files_active, "the Properties to close")
    finally:
        DBusTestCase.tearDownClass()


@scenario(requires=("dbusmock",))
def disk_management_encryption(s):
    """Disk Management (on a dbusmock UDisks2 with a locked LUKS stick):
    Unlock... asks for the passphrase (again after a wrong one); Change
    Mount Point's Add for the unlocked volume adds an /etc/crypttab entry
    that asks for the passphrase at startup (the default) with the fstab
    entry for what's inside, Remove takes both away, and Add with a key
    file has UDisks2 keep the passphrase in /etc/luks-keys; Lock unmounts
    and locks it; Change Passphrase...; Format with "Encrypt this volume
    (LUKS)" refuses passphrases that differ and passes encrypt.type and
    encrypt.passphrase."""
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    try:
        mock = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "udisks2.py")
        proc, _ = DBusTestCase.spawn_server_template(
            mock, {"encrypted": True}, stdout=subprocess.DEVNULL, system_bus=True
        )
        s.children.append(proc)
        udisks = DBusTestCase.get_dbus(system_bus=True).get_object(
            "org.freedesktop.UDisks2", "/org/freedesktop/UDisks2"
        )
        sde1 = "/org/freedesktop/UDisks2/block_devices/sde1"
        dm0 = "/org/freedesktop/UDisks2/block_devices/dm_2d0"

        def calls():
            return [str(c) for c in udisks.MockCalls(dbus_interface=MOCK_IFACE)]

        def called(what):
            return what in calls()

        empty = s.settle("desktop")
        s.launch("xd-diskmgmt")
        s.wait_window(title="Disk Management", state="activated")
        x, y, _, _ = s.settle("disk-management").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))

        def opened(what, label):
            s.wait(lambda: "activated" not in s.window(title="Disk Management")["states"],
                   f"the {what} dialog")  # fmt: skip
            s.settle(label)

        def back(label):
            s.wait(lambda: "activated" in s.window(title="Disk Management")["states"],
                   "Disk Management again")  # fmt: skip
            s.settle(label)

        def select_stick(label):
            # The volume list: sda1-3, Data, Stuff, USBSTICK, then sde1.
            s.inject("move", x + 120, y + 70, "click", "sleep", 300)
            for _ in range(6):
                s.inject("key", "Down", "sleep", 100)
            s.settle(label)

        def menu(key, what, label):
            s.inject("key", "Menu", "sleep", 400, "key", key)
            opened(what, label)

        # Unlock...: a wrong passphrase, then the right one.
        select_stick("locked stick")
        menu("k", "Unlock", "unlock")
        s.inject("type", "nope", "key", "Return")
        s.wait(lambda: called(f"Unlock {sde1} wrong"), "Unlock with the wrong passphrase")
        s.settle("unlock again")
        s.inject("type", "correct horse", "key", "Return")
        s.wait(lambda: called(f"Unlock {sde1} right"), "Unlock with the right passphrase")
        back("unlocked")
        s.log("ok: Unlock asked again after a wrong passphrase, then unlocked")

        # Add a startup mount point: crypttab (asking at startup), then
        # fstab for what's inside, then mounted there.
        select_stick("unlocked stick")
        s.inject("key", "alt+a", "sleep", 400, "key", "p")
        opened("Change Mount Point", "change mount point")
        s.inject("key", "alt+a")
        opened("Add Startup Mount Point", "add startup")
        s.inject("key", "Return")
        ask = (f"AddConfigurationItem {sde1} crypttab name=luks-uuid-sde1 device=UUID=uuid-sde1 "
               "passphrase-path= options=nofail passphrase-contents=")
        fstab = (f"AddConfigurationItem {dm0} fsname=UUID=uuid-private dir=/mnt/Private type=ext4 "
                 "opts=nofail,x-mount.mkdir freq=0 passno=0")
        s.wait(lambda: calls()[-3:] == [ask, fstab, f"Mount {dm0}"],
               "the crypttab and fstab entries, then a mount")  # fmt: skip
        s.log("ok: Add made an /etc/crypttab entry that asks, and the fstab entry")

        # Remove: both entries go.
        back("added")
        s.inject("key", "alt+a", "sleep", 400, "key", "p")
        opened("Change Mount Point", "change mount point 2")
        s.inject("key", "alt+r")
        opened("Remove Startup Mount Point", "remove?")
        s.inject("key", "alt+y")
        s.wait(lambda: calls()[-1].startswith(f"RemoveConfigurationItem {sde1} crypttab"),
               "both entries removed")  # fmt: skip
        s.check(calls()[-2].startswith(f"RemoveConfigurationItem {dm0} fsname=UUID=uuid-private"),
                "the fstab entry went first")  # fmt: skip
        s.log("ok: Remove took the fstab and crypttab entries away")

        # Add again, with a key file: the passphrase twice.
        back("removed")
        s.inject("key", "alt+a", "sleep", 400, "key", "p")
        opened("Change Mount Point", "change mount point 3")
        s.inject("key", "alt+a")
        opened("Add Startup Mount Point", "add startup 2")
        s.inject("key", "alt+f", "sleep", 200, "key", "alt+s", "type", "correct horse",
                 "key", "alt+c", "type", "correct horse", "sleep", 200)  # fmt: skip
        s.settle("key file")
        s.inject("key", "Return")
        keyfile = (f"AddConfigurationItem {sde1} crypttab name=luks-uuid-sde1 "
                   "device=UUID=uuid-sde1 passphrase-path=/etc/luks-keys/luks-uuid-sde1 "
                   "options=nofail passphrase-contents=correct horse")
        s.wait(lambda: called(keyfile), "the crypttab entry with a key file")
        s.log("ok: Add with a key file had UDisks2 keep the passphrase in /etc/luks-keys")

        # Lock: it's mounted, so unmounted first.
        back("key file added")
        select_stick("mounted stick")
        s.inject("key", "Menu", "sleep", 400, "key", "k")
        s.wait(lambda: calls()[-2:] == [f"Unmount {dm0}", f"Lock {sde1}"], "Unmount, then Lock")
        s.log("ok: Lock unmounted and locked it")

        # Change Passphrase...
        s.settle("locked")
        select_stick("locked again")
        menu("s", "Change Passphrase", "change passphrase")
        s.inject("type", "correct horse", "key", "Tab", "type", "battery staple", "key", "Tab",
                 "type", "battery staple", "key", "Return")  # fmt: skip
        s.wait(lambda: called(f"ChangePassphrase {sde1} right -> battery staple"),
               "ChangePassphrase")  # fmt: skip
        s.settle("changed")
        s.inject("key", "Return")
        back("passphrase changed")
        s.log("ok: Change Passphrase changed it")

        # Format, encrypted: passphrases that differ are refused first.
        select_stick("to format")
        menu("f", "Format", "format")
        s.inject("key", "alt+l", "sleep", 200, "key", "alt+s", "type", "pw1", "key", "alt+r",
                 "type", "pw2", "key", "Return")  # fmt: skip
        s.settle("passphrases differ")
        s.inject("key", "Return", "sleep", 400)
        s.check(not any(c.startswith(f"Format {sde1}") for c in calls()),
                "passphrases that differ didn't format")  # fmt: skip
        s.inject("key", "alt+s", "type", "pw1", "key", "alt+r", "type", "pw1", "key", "Return")
        opened("Format", "format confirm")
        s.inject("key", "alt+y")
        fmt = (f"Format {sde1} ext4 label=New Volume tear-down=True encrypt.type=luks2 "
               "encrypt.passphrase=pw1")
        s.wait(lambda: called(fmt), "an encrypted format")
        back("formatted")
        s.log("ok: Format encrypted the volume with the passphrase")

        # The New Partition Wizard in Disk 1's unallocated space: Next
        # stays greyed while the passphrases differ.
        s.inject("move", x + 778, y + 353, "click", "sleep", 300)
        menu("n", "New Partition Wizard", "wizard")
        for i in range(3):
            s.inject("key", "alt+n", "sleep", 400)
        s.inject("key", "alt+l", "sleep", 200, "key", "alt+s", "type", "pw", "key", "alt+r",
                 "type", "px", "sleep", 300, "key", "alt+n", "sleep", 400)  # fmt: skip
        s.settle("wizard passphrases differ")
        s.inject("key", "alt+r", "key", "ctrl+a", "type", "pw", "sleep", 200, "key", "alt+n",
                 "sleep", 500)  # fmt: skip
        s.settle("wizard summary")
        s.inject("key", "alt+a")
        s.wait(lambda: any(c.startswith("Format ") and c.endswith(
            "encrypt.type=luks2 encrypt.passphrase=pw") for c in calls()),
            "the new partition formatted encrypted")  # fmt: skip
        s.check(sum(c.startswith("CreatePartitionAndFormat") for c in calls()) == 1,
                "one partition made")  # fmt: skip
        # Mounted: what's inside, not the encrypted partition itself.
        s.wait(lambda: calls()[-1] == "Mount /org/freedesktop/UDisks2/block_devices/dm_2d1",
               "the new volume's cleartext mounted")  # fmt: skip
        back("partition made")
        s.log("ok: the New Partition Wizard made an encrypted volume, and mounted it")
        s.inject("key", "alt+F4")
        s.wait_window(title="Disk Management", gone=True, timeout=10)
    finally:
        DBusTestCase.tearDownClass()


@scenario(requires=("dbusmock",))
def encrypted_drive(s):
    """My Computer's encrypted drive (on a dbusmock UDisks2's locked LUKS
    stick and a dbusmock Secret Service): opening it asks for the
    passphrase, a wrong one asks again, the right one with "Remember my
    passphrase" unlocks (Encrypted.Unlock), mounts and opens it, and
    stores the passphrase under gvfs's schema; Lock unmounts and locks
    it; opened again, it's unlocked with the remembered passphrase
    without asking."""
    from dbusmock import DBusTestCase, MOCK_IFACE

    media = os.path.join(s.home, "media")
    os.makedirs(os.path.join(media, "Private"))
    with open(os.path.join(media, "Private", "diary.txt"), "w") as f:
        f.write("Dear diary\n")

    DBusTestCase.start_system_bus()
    try:
        mock = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "udisks2.py")
        proc, _ = DBusTestCase.spawn_server_template(
            mock, {"encrypted": True, "media": media}, stdout=subprocess.DEVNULL,
            system_bus=True)  # fmt: skip
        s.children.append(proc)
        secrets = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "luks_secrets.py")
        proc, _ = DBusTestCase.spawn_server_template(
            secrets, {}, stdout=subprocess.DEVNULL, system_bus=False)  # fmt: skip
        s.children.append(proc)
        udisks = DBusTestCase.get_dbus(system_bus=True).get_object(
            "org.freedesktop.UDisks2", "/org/freedesktop/UDisks2"
        )
        keyring = DBusTestCase.get_dbus(system_bus=False).get_object(
            "org.freedesktop.secrets", "/org/freedesktop/secrets"
        )
        sde1 = "/org/freedesktop/UDisks2/block_devices/sde1"
        dm0 = "/org/freedesktop/UDisks2/block_devices/dm_2d0"

        def calls():
            return [str(c) for c in udisks.MockCalls(dbus_interface=MOCK_IFACE)]

        def items():
            return [(str(l), {str(k): str(v) for k, v in a.items()}, str(p))
                    for l, a, p in keyring.MockItems(dbus_interface=MOCK_IFACE)]  # fmt: skip

        empty = s.settle("desktop")
        s.launch("xd-files", "computer:///")
        s.wait_window(title="My Computer", state="activated")
        x, y, _, _ = s.settle("my computer").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))

        def files_active(title):
            w = s.window(title=title)
            return w is not None and "activated" in w["states"]

        def open_stick():
            # The stick is the last drive (Devices with Removable Storage,
            # after any the computer itself has): the first item, then End.
            s.inject("move", x + 264, y + 168, "click", "sleep", 300, "key", "End", "sleep", 300,
                     "key", "Return")  # fmt: skip

        # Locked: the passphrase box. A wrong passphrase is asked again.
        open_stick()
        s.wait(lambda: not files_active("My Computer"), "the passphrase box")
        s.settle("passphrase")
        s.inject("type", "guess", "key", "Return")
        s.wait(lambda: f"Unlock {sde1} wrong" in calls(), "Unlock with the wrong passphrase")
        s.settle("passphrase again")
        s.check(not files_active("Private"), "a wrong passphrase didn't open it")
        # The right one, remembered: unlocked, mounted and opened.
        s.inject("type", "correct horse", "key", "alt+r", "sleep", 200, "key", "Return")
        s.wait(lambda: files_active("Private"), "the stick to open")
        s.check(calls()[-2:] == [f"Unlock {sde1} right", f"Mount {dm0}"],
                f"unlocked and mounted ({calls()[-3:]})")  # fmt: skip
        s.wait(lambda: items(), "the passphrase to be remembered")
        label, attrs, secret = items()[0]
        s.check(label == "Encryption passphrase for 7.45 GB Encrypted Volume"
                and attrs.get("gvfs-luks-uuid") == "uuid-sde1" and secret == "correct horse",
                f"remembered under gvfs's schema ({items()})")  # fmt: skip
        s.settle("opened")
        s.log("ok: unlocked with the passphrase, opened, and remembered")

        # Back in My Computer, Lock from its menu: unmounted, then locked.
        s.inject("key", "alt+Left")
        s.wait(lambda: files_active("My Computer"), "My Computer again")
        s.settle("unlocked in my computer")
        s.inject("move", x + 264, y + 168, "click", "sleep", 300, "key", "End", "sleep", 300,
                 "key", "Menu", "sleep", 500)  # fmt: skip
        s.settle("context menu")
        s.inject("key", "k")
        s.wait(lambda: calls()[-2:] == [f"Unmount {dm0}", f"Lock {sde1}"], "Unmount, then Lock")
        s.log("ok: Lock unmounted and locked it")

        # Opened again: the remembered passphrase, without asking.
        s.settle("locked")
        open_stick()
        s.wait(lambda: files_active("Private"), "the stick to open with its remembered passphrase")
        s.check(calls()[-2:] == [f"Unlock {sde1} right", f"Mount {dm0}"],
                f"unlocked with the keyring's passphrase ({calls()[-3:]})")  # fmt: skip
        s.log("ok: the remembered passphrase unlocked it without asking")

        # Its Properties' Encryption tab (after General, Tools and
        # Hardware): Change Passphrase..., the old one and the new one
        # twice; the remembered passphrase follows.
        s.inject("key", "alt+Left")
        s.wait(lambda: files_active("My Computer"), "My Computer again")
        s.inject("move", x + 264, y + 168, "click", "sleep", 300, "key", "End", "sleep", 300,
                 "key", "alt+Return")  # fmt: skip
        s.wait(lambda: not files_active("My Computer"), "the drive's Properties")
        s.inject("key", "ctrl+Tab", "sleep", 200, "key", "ctrl+Tab", "sleep", 200, "key",
                 "ctrl+Tab", "sleep", 300)  # fmt: skip
        s.settle("encryption tab")
        s.inject("key", "alt+h", "sleep", 500)
        s.settle("change passphrase")
        s.inject("type", "correct horse", "key", "Tab", "type", "battery staple", "key", "Tab",
                 "type", "battery staple", "key", "Return")  # fmt: skip
        s.wait(lambda: f"ChangePassphrase {sde1} right -> battery staple" in calls(),
               "ChangePassphrase")  # fmt: skip
        s.wait(lambda: [p for _, _, p in items()] == ["battery staple"],
               "the remembered passphrase to change too")  # fmt: skip
        s.settle("passphrase changed")
        s.inject("key", "Return", "sleep", 300, "key", "Escape")
        s.wait(lambda: files_active("My Computer"), "the Properties to close")
        s.log("ok: Change Passphrase changed it, and the remembered one")
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


def mock_logind_docked(s):
    """Before the compositor starts: a private system bus with dbusmock's
    logind (logging its calls), not docked."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    log = open(os.path.join(s.base, "logind.log"), "w")
    proc, obj = DBusTestCase.spawn_server_template("logind", {}, stdout=log, system_bus=True)
    s.children.append(proc)
    obj.AddProperty("org.freedesktop.login1.Manager", "Docked", dbus.Boolean(False),
                    dbus_interface=MOCK_IFACE)  # fmt: skip


@scenario(LUNA_INI + "\n[Power]\nPowerButtonAction=standby\nLidAction=shutdown\n",
          requires=("dbusmock",), setup=mock_logind_docked)  # fmt: skip
def power_buttons(s):
    """Power Options' Power buttons (on a dbusmock logind): the shell blocks
    logind's handling of the buttons set, and carries their actions out:
    the power button stands by, closing the lid shuts down, but not while
    docked; the sleep button, left to logind, does nothing here. Setting it
    to Hibernate in Power Options' Advanced tab takes it over too."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    def calls(method):
        with open(os.path.join(s.base, "logind.log")) as f:
            return [line for line in f if f" {method} " in line]

    def inhibited(what):
        return any(f'"{what}"' in line and '"block"' in line for line in calls("Inhibit"))

    try:
        s.wait(lambda: inhibited("handle-lid-switch:handle-power-key"),
               "the shell to block logind's lid and power key")  # fmt: skip
        s.log("ok: the shell holds logind's block inhibitor for the lid and power key")
        s.inject("key", "XF86PowerOff")
        s.wait(lambda: calls("Suspend"), "the power button to stand by")
        s.log("ok: the power button stood by (logind's Suspend)")
        s.inject("lid", "close", "lid", "open")
        s.wait(lambda: calls("PowerOff"), "closing the lid to shut down")
        s.log("ok: closing the lid shut down (logind's PowerOff)")

        logind = dbus.SystemBus().get_object("org.freedesktop.login1", "/org/freedesktop/login1")
        logind.UpdateProperties("org.freedesktop.login1.Manager",
                                {"Docked": dbus.Boolean(True)},
                                dbus_interface=MOCK_IFACE)  # fmt: skip
        # Docked: the lid does nothing. The sleep button is logind's, so
        # nothing here either. The power button again marks the end.
        s.inject("lid", "close", "sleep", 1000, "key", "XF86Sleep", "sleep", 1000,
                 "key", "XF86PowerOff")  # fmt: skip
        s.wait(lambda: len(calls("Suspend")) == 2, "the power button again")
        s.inject("sleep", 1000)
        s.check(len(calls("PowerOff")) == 1, "closing the lid while docked did nothing")
        s.check(len(calls("Suspend")) == 2 and not calls("Hibernate"),
                "the sleep button, left to logind, did nothing in the session")  # fmt: skip
        s.log("ok: docked, closing the lid does nothing; the unset sleep button is logind's")

        # Power Options (no battery: Advanced is the second tab): the sleep
        # button from logind's Stand by to Hibernate, then Apply.
        s.launch("xd-power", "--page", "1")
        s.wait_window(title="Power Options Properties", state="activated")
        s.settle("advanced")
        s.inject("key", "alt+e", "sleep", 300, "key", "Down", "sleep", 300)
        s.settle("hibernate-chosen")
        s.inject("key", "alt+a")

        def saved():
            ini = s.ini()
            return ini.has_section("Power") and ini["Power"].get("SleepButtonAction")

        s.check(s.wait(saved, "the sleep button saved") == "hibernate", "Hibernate saved")
        power = s.ini()["Power"]
        s.check(power.get("PowerButtonAction") == "standby" and
                power.get("LidAction") == "shutdown", "the others kept theirs")  # fmt: skip
        s.wait(lambda: inhibited("handle-lid-switch:handle-power-key:handle-suspend-key:"
                                 "handle-hibernate-key"),
               "the shell to block logind's sleep keys too")  # fmt: skip
        # (Besides sleep.c's delay inhibitor, for locking before sleep.)
        held = lambda: [str(i[0]) for i in logind.ListInhibitors(
            dbus_interface="org.freedesktop.login1.Manager") if str(i[3]) == "block"]  # fmt: skip
        s.wait(lambda: len(held()) == 1, "the old inhibitor to go")
        s.check(held()[0].endswith("handle-hibernate-key"), "one inhibitor, the new one")
        s.inject("sleep", 500, "key", "XF86Sleep")
        s.wait(lambda: calls("Hibernate"), "the sleep button to hibernate")
        s.log("ok: set in Power Options, the sleep button hibernates")

        # "Ask me what to do": the Shut Down dialog.
        ini = s.ini()
        ini["Power"]["PowerButtonAction"] = "ask"
        with open(os.path.join(s.config, "desktop.ini"), "w") as f:
            ini.write(f, space_around_delimiters=False)
        s.inject("sleep", 1000, "key", "XF86PowerOff")
        s.wait_window(title="Shut Down XeonDesktop")
        s.check(len(calls("Suspend")) == 2, "asking stood nothing by")
        s.log("ok: Ask me what to do opens the Shut Down dialog")
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
        # Taller by our caption; no narrower. (What was measured before
        # includes the client's own shadow where its theme draws one, which
        # our frame then covers.)
        s.check(h > before[3] + 5 and w >= before[2],
                f"our frame around it ({before[2]}x{before[3]} -> {w}x{h})")  # fmt: skip
        # The frame's Close button, at its caption's right end: found on
        # the caption's row from the desktop inwards, since the box above
        # may take in the client's shadow beside the frame.
        hung = s.shot("hung-frame")
        bg = empty.pixel(x + w + 2, y + 12) if x + w + 2 < WIDTH else empty.pixel(WIDTH - 1, y + 12)
        right = x + w - 1
        while right > x and max(abs(p - q) for p, q in zip(hung.pixel(right, y + 12), bg)) <= 30:
            right -= 1
        s.inject("move", right - 11, y + 12, "click")
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
    twice, leaving it short of the right and bottom edges), and told it's
    tiled, its corners are square (no desktop through a rounded one)."""
    if not have_gtk3(s):
        return
    desk = s.settle("desktop").pixel(WIDTH // 2, HEIGHT // 2)
    script = os.path.join(s.base, "csd.py")
    with open(script, "w") as f:
        f.write(GTK_CSD)
    s.launch(sys.executable, script, env={"GSETTINGS_BACKEND": "memory"})
    s.wait_window(title="Ghost test", state="activated")
    s.settle("window")
    tiled = taskbar_menu(s, 2, "tiled")  # Tile Windows Vertically
    work = HEIGHT - 40

    # The window's own pixels there, not the desktop's: its theme may draw
    # a border or a highlight line on the edge itself.
    def window(*points):
        return all(max(abs(p - q) for p, q in zip(tiled.pixel(*xy), desk)) > 30 for xy in points)

    s.check(window((WIDTH - 1, work // 2)), "the window reaches the right edge")
    s.check(window((WIDTH // 2, work - 1)), "the window reaches the taskbar")
    s.check(window((0, 14), (WIDTH // 2, 0)), "and the left and top edges")
    s.check(window((0, 0), (WIDTH - 1, 0), (0, work - 1), (WIDTH - 1, work - 1)),
            "and its corners are square")  # fmt: skip


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


# A push button and a checked check box; the check box has the focus, so
# the button is drawn in its normal state.
GTK_BUTTON_CHECK = r"""
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
w = Gtk.Window(title="Package test")
w.set_default_size(240, 120)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin=16)
button = Gtk.Button(label="Push")
box.pack_start(button, False, False, 0)
check = Gtk.CheckButton(label="Checked")
check.set_active(True)
box.pack_start(check, False, False, 0)
w.add(box)
w.connect("destroy", Gtk.main_quit)
w.show_all()
check.grab_focus()
Gtk.main()
"""

# The tiny theme package's colours: the button's edge and its five states'
# fills, the check box's twelve states, the dialog face.
PKG_EDGE = (0x20, 0x30, 0x60)
PKG_BUTTON = [(0x40, 0xC0, 0x40), (0x40, 0x40, 0xC0), (0xC0, 0x40, 0x40), (0x80, 0x80, 0x80),
              (0xC0, 0xC0, 0x40)]  # fmt: skip
PKG_CHECK = [(0x10 * (i + 1), 0x10, 0xF0 - 0x10 * i) for i in range(12)]
PKG_FACE = (200, 100, 50)


def write_tiny_package(s):
    """setup: a theme package of our own (theme.xml, a style, images drawn
    here), used by the session through XD_THEME_PACKAGE."""
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    root = os.path.join(s.base, "tiny-package")
    os.makedirs(os.path.join(root, "styles"))
    os.makedirs(os.path.join(root, "images"))
    with open(os.path.join(root, "theme.xml"), "w") as f:
        f.write('<xdtheme version="1" name="Tiny" base="luna">\n'
                '  <variant id="v" name="V" size="Normal" style="styles/v.xml"/>\n</xdtheme>\n')  # fmt: skip
    with open(os.path.join(root, "styles", "v.xml"), "w") as f:
        f.write(
            "<style>\n  <sysmetrics>\n"
            f'    <color name="BtnFace" value="{" ".join(map(str, PKG_FACE))}"/>\n'
            "  </sysmetrics>\n  <class name=\"Button\">\n"
            '    <part name="PushButton" BgType="ImageFile" ImageFile="images/button.png"'
            ' ImageCount="5" SizingMargins="4 4 4 4" ContentMargins="3 3 3 3"'
            ' SizingType="Stretch" TextColor="0 0 0"/>\n'
            '    <part name="CheckBox" BgType="ImageFile" ImageFile="images/check.png"'
            ' ImageCount="12" SizingType="TrueSize"/>\n'
            "  </class>\n</style>\n"
        )

    def strip(name, size, fills, border):
        pb = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, size, size * len(fills))
        for i, fill in enumerate(fills):
            frame = pb.new_subpixbuf(0, i * size, size, size)
            frame.fill(int.from_bytes(bytes(PKG_EDGE) + b"\xff", "big"))
            inner = frame.new_subpixbuf(border, border, size - 2 * border, size - 2 * border)
            inner.fill(int.from_bytes(bytes(fill) + b"\xff", "big"))
        pb.savev(os.path.join(root, "images", name), "png", [], [])

    strip("button.png", 12, PKG_BUTTON, 2)
    strip("check.png", 13, PKG_CHECK, 1)
    os.environ["XD_THEME_PACKAGE"] = root


@scenario(LUNA_INI, setup=write_tiny_package)
def gtk_theme_package(s):
    """With a theme package, GTK 3 programs (through GTK_THEME) draw their
    widgets with its images: a push button's edges and middle from its
    PushButton image, a checked check box from its CheckBox image's Checked
    frame, the window in its [SysMetrics] button face."""
    if not have_gtk3(s):
        return
    gtk3 = os.path.join(s.data, "themes", "XeonDesktop", "gtk-3.0")
    with open(os.path.join(gtk3, "gtk.css")) as f:
        css = f.read()
    s.check("assets/pkg/button-pushbutton-1-b.png" in css and "4 4 4 4 stretch" in css,
            "the generated CSS cuts the package's button image at its margins")  # fmt: skip
    s.check(os.path.exists(os.path.join(gtk3, "assets", "pkg", "button-checkbox-5-w@2.png")),
            "and its images are written beside it, at 2x too")  # fmt: skip
    script = os.path.join(s.base, "package.py")
    with open(script, "w") as f:
        f.write(GTK_BUTTON_CHECK)
    empty = s.settle("desktop")
    s.launch(sys.executable, script, env={"GTK_THEME": "XeonDesktop", "GSETTINGS_BACKEND": "memory"})
    s.wait_window(title="Package test", state="activated")
    shot = s.settle("window")
    win = shot.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(win is not None, "the window is drawn")
    x0, y0, w, h = win

    def count(colour):
        return sum(1 for y in range(y0, y0 + h) for x in range(x0, x0 + w)
                   if shot.pixel(x, y) == colour)  # fmt: skip

    face = count(PKG_FACE)
    s.check(face > w * h // 4, f"the window is the package's button face ({face} pixels)")
    # The button: 4-pixel corners and edges of the image, its normal fill
    # stretched across the middle (at least a 60x10 label's worth).
    fill = count(PKG_BUTTON[0])
    s.check(fill > 60 * 10, f"the button's middle is its Normal frame's ({fill} pixels)")
    s.check(not any(count(c) for c in PKG_BUTTON[1:]), "and no other state's")
    edge = count(PKG_EDGE)
    s.check(edge > 2 * (60 + 15), f"with the image's edges around it ({edge} pixels)")
    # The check box: the CheckedNormal frame (5th), 11x11 inside its edge.
    checked = count(PKG_CHECK[4])
    s.check(checked == 11 * 11, f"the check box is its Checked frame ({checked} pixels)")
    s.check(count(PKG_CHECK[0]) == 0, "not the Unchecked one")


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
def window_menu_letters(s):
    """Alt+Space opens the window menu, whose letters work as Windows': X
    maximizes, R restores, N minimizes."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    opened = s.settle("window")
    s.inject("key", "alt+space")
    wait_change(s, opened, (0, 0, 400, 300), "the window menu", "window-menu")
    s.inject("key", "x")
    s.wait_window(title="Control Panel", state="maximized")
    s.log("ok: Alt+Space, X maximized")
    s.inject("sleep", 300, "key", "alt+space", "sleep", 500, "key", "r")
    s.wait(lambda: "maximized" not in s.window(title="Control Panel")["states"], "restore")
    s.log("ok: Alt+Space, R restored")
    s.inject("sleep", 300, "key", "alt+space", "sleep", 500, "key", "n")
    s.wait_window(title="Control Panel", state="minimized")
    s.log("ok: Alt+Space, N minimized")


# Display Properties' Effects: "Hide underlined letters for keyboard
# navigation until I press the Alt key".
HIDDEN_CUES_INI = LUNA_INI + "\n[Effects]\nHideKeyboardCues=true\n"
XP_ALL_PROGRAMS = (40, 662, 100, 30)  # "All Programs", in an empty XP Start menu


@scenario(HIDDEN_CUES_INI)
def xp_start_underlines(s):
    """With underlines hidden until the keyboard is used, the XP Start menu
    opened with the mouse has none; opened with Ctrl+Esc it underlines
    All Programs' P (only that, in that line), and R opens Run."""
    s.settle("desktop")
    s.inject("move", 30, 755, "click")
    clicked = s.settle("clicked")
    s.inject("key", "Escape")
    s.settle("closed")
    s.inject("key", "ctrl+Escape")
    keyed = s.settle("keyed")
    box = keyed.changed_box(clicked, XP_ALL_PROGRAMS, step=1)
    s.check(box is not None and box[2] <= 10 and box[3] <= 2,
            f"Ctrl+Esc: a short line under one letter of All Programs ({box})")  # fmt: skip
    s.inject("key", "r")
    s.wait_window(title="Run", state="activated")
    s.log("ok: R opened Run")


@scenario(LUNA_INI)
def xp_start_send_to(s):
    """A program's right-click menu in the XP Start menu: Send To > Desktop
    (create shortcut) puts it on the desktop (by its letters, N then D)."""
    with open(os.path.join(s.config, "start-pinned"), "w") as f:
        f.write("org.xeondesktop.Files.desktop\n")
    s.inject("key", "ctrl+Escape", "sleep", 600)
    s.settle("menu")
    s.inject("move", 90, 352, "click", "right", "sleep", 600, "key", "n", "sleep", 600, "key", "d")
    shortcut = os.path.join(s.home, "Desktop", "org.xeondesktop.Files.desktop")
    s.wait(lambda: os.path.exists(shortcut), "the program's shortcut on the desktop")
    s.log("ok: Send To > Desktop (create shortcut) put it there")


@scenario(LUNA_INI)
def xp_new_programs_balloon(s):
    """A program installed while the session runs: XP's "New programs
    installed" balloon over the Start button; a click puts it away."""
    before = s.settle("desktop")
    with open(os.path.join(s.data, "applications", "xd-test-new.desktop"), "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=New Program\nExec=true\n")
    above_start = (0, 600, 300, 135)
    shot, box = wait_change(s, before, above_start, "the balloon", "balloon")
    s.check(box[1] + box[3] >= 725 and box[0] < 60, f"a balloon pointing at Start ({box})")
    s.inject("move", box[0] + box[2] // 2, box[1] + 12, "click")
    s.wait(lambda: s.shot("gone").differs(before, above_start) < 0.01, "the balloon to go")
    s.log("ok: a click put it away")

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
    # bottom; Identify is at (228, 368), 75x23.
    edge = (w - 404) // 2
    left, top = x + edge, y + h - edge - 448
    s.inject("move", left + 265, top + 379, "click", "move", WIDTH // 2, HEIGHT // 2)
    first = (0, 0, 300, 300)
    second = (SECOND_X, 0, 300, 300)
    shot = s.wait(lambda: (lambda sh: sh if sh.changed_box(tab, first) and
                           sh.changed_box(tab, second) else None)(s.shot("identify")),
                  "numbers on both monitors", 10)  # fmt: skip
    boxes = [shot.changed_box(tab, r) for r in (first, second)]
    for name, (bx, by, bw, bh) in zip(("first", "second"), boxes):
        s.check(bh > 120 and bx < (SECOND_X if name == "second" else 0) + 80,
                f"a big number at the top left of the {name} monitor ({bx},{by} {bw}x{bh})")
    # A 1 and a 2 have different shapes in any font: laid over each other
    # (from their boxes' top left), much of their white fill doesn't match.
    def white(box, dx, dy):
        bx, by, _, _ = box
        return min(shot.pixel(bx + dx, by + dy)) >= 235

    cw, ch = min(b[2] for b in boxes), min(b[3] for b in boxes)
    both = either = 0
    for dy in range(0, ch, 2):
        for dx in range(0, cw, 2):
            a, b = white(boxes[0], dx, dy), white(boxes[1], dx, dy)
            both += a and b
            either += a or b
    apart = 1 - both / either if either else 0
    s.check(apart > 0.3, f"and the two are different numbers ({boxes}, {apart:.2f} apart)")
    s.wait(lambda: s.shot("gone").changed_box(tab, first) is None and
           s.shot("gone").changed_box(tab, second) is None, "the numbers to go", 10)  # fmt: skip
    s.log("ok: the numbers went away by themselves")


def vrr_always(s):
    """Before the compositor starts: monitors.ini asks for a variable
    refresh rate all the time, which the headless output can't do."""
    path = os.path.join(s.config, "monitors.ini")
    with open(path) as f:
        text = f.read()
    with open(path, "w") as f:
        f.write(text.replace("[HEADLESS-1]\n", "[HEADLESS-1]\nAdaptiveSync=always\n", 1))


@scenario(setup=vrr_always)
def display_adaptive_sync(s):
    """The game protocols are offered as the renderer allows: tearing-control
    always, explicit sync (linux-drm-syncobj) not with the software
    renderer. A monitor that can't do variable refresh rates reports it off
    through wlr-output-management (even with AdaptiveSync=always in
    monitors.ini) and refuses having it turned on; Display Properties >
    Settings > Advanced shows the Monitor tab with "Variable refresh rate"
    unchecked and greyed."""
    r = subprocess.run([s.tool("xd-windows")], env=dict(os.environ, WAYLAND_DEBUG="1"),
                       capture_output=True, timeout=10)  # fmt: skip
    globals_ = r.stderr.decode(errors="replace")
    s.check('"wp_tearing_control_manager_v1"' in globals_, "tearing-control is offered")
    s.check('"wp_linux_drm_syncobj_manager_v1"' not in globals_,
            "explicit sync isn't, with the software renderer")  # fmt: skip
    if shutil.which("wlr-randr"):
        def randr(*args):
            return subprocess.run(["wlr-randr", *args], capture_output=True, text=True, timeout=10)

        s.check("Adaptive Sync: disabled" in randr().stdout,
                "wlr-output-management reports adaptive sync off")  # fmt: skip
        r = randr("--output", "HEADLESS-1", "--adaptive-sync", "enabled")
        s.check(r.returncode != 0 or "failed" in r.stdout + r.stderr,
                "turning it on is refused")  # fmt: skip
        s.check("Adaptive Sync: disabled" in randr().stdout, "and it stays off")
        s.check(s.ini("monitors.ini")["HEADLESS-1"].get("AdaptiveSync") == "always",
                "monitors.ini keeps its own setting")  # fmt: skip
    else:
        s.log("wlr-randr isn't installed: wlr-output-management not checked")

    empty = s.settle("desktop")
    s.launch("xd-desk", "--page", "4")
    s.wait_window(title="Display Properties", state="activated")
    tab = s.settle("settings-tab")
    x, y, w, h = tab.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    # Framed as in identify_monitors; Advanced is at (309, 368), 75x23.
    edge = (w - 404) // 2
    left, top = x + edge, y + h - edge - 448
    s.inject("move", left + 346, top + 379, "click")
    s.wait_window(title="Headless output 1 Properties", state="activated")
    s.inject("move", WIDTH // 2, HEIGHT - 60)
    dialog = s.settle("monitor-tab")
    x, y, w, h = dialog.changed_box(tab, (0, 0, WIDTH, HEIGHT - 40))
    # Its client area is 360x296; the check box's square is at (31, 183),
    # 13x13: white inside when it can be checked, the face colour greyed.
    edge = (w - 360) // 2
    left, top = x + edge, y + h - edge - 296
    inside = dialog.pixel(left + 37, top + 189)
    s.check(min(inside) < 240 and max(inside) > 150,
            f"Variable refresh rate is greyed and unchecked ({inside})")
    s.inject("key", "Escape")
    s.wait_window(title="Headless output 1 Properties", gone=True)


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


def mock_timedate(s, timesync=None):
    """A private system bus with dbusmock's timedated (synchronizing,
    hardware clock in UTC), systemd's manager with RestartUnit, and
    timesyncd (tests/dbusmock/timesync.py) with `timesync` as its
    parameters, unless it's None (another time service). Returns the
    timesyncd mock and a function giving what the mocks were called with."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    logs = []

    def log():
        logs.append(os.path.join(s.base, f"mock-{len(logs)}.log"))
        return open(logs[-1], "w")

    for template, params in (("timedated", {"Timezone": "Europe/London", "NTP": True}),):
        proc, _ = DBusTestCase.spawn_server_template(template, params, stdout=log(),
                                                     system_bus=True)  # fmt: skip
        s.children.append(proc)
    proc = DBusTestCase.spawn_server("org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                                     "org.freedesktop.systemd1.Manager", system_bus=True,
                                     stdout=log())  # fmt: skip
    s.children.append(proc)
    bus = dbus.SystemBus()
    systemd = bus.get_object("org.freedesktop.systemd1", "/org/freedesktop/systemd1")
    systemd.AddMethod("org.freedesktop.systemd1.Manager", "RestartUnit", "ss", "o",
                      'ret = dbus.ObjectPath("/org/freedesktop/systemd1/job/1")',
                      dbus_interface=MOCK_IFACE)  # fmt: skip
    ts = None
    if timesync is not None:
        template = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "timesync.py")
        proc, obj = DBusTestCase.spawn_server_template(template, timesync, stdout=log(),
                                                       system_bus=True)  # fmt: skip
        s.children.append(proc)
        ts = dbus.Interface(obj, MOCK_IFACE)

    def calls():
        text = ""
        for path in logs:
            with open(path) as f:
                text += f.read()
        return text

    return ts, calls


@scenario(requires=("dbusmock",))
def timedate_internet_time(s):
    """Date and Time Properties' Internet Time tab on dbusmock's timedated,
    timesyncd and systemd: Update Now restarts timesyncd and reports the
    answer; a server typed in goes to SetRuntimeNTPServers; the Time Zone
    tab's hardware clock box calls SetLocalRTC."""
    from dbusmock import DBusTestCase

    try:
        ts, calls = mock_timedate(s, {"SystemNTPServers": ["time.example.org"],
                                      "ServerName": "time.example.org",
                                      "Received": time.time() - 3600})
        empty = s.settle("desktop")
        s.launch("xd-timedate", "--page", "2")
        s.wait_window(title="Date and Time Properties", state="activated")
        dialog = s.settle("internet-time")
        x, y, _, _ = dialog.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
        status = (x + 20, y + 115, 520, 40)  # under the server
        update = (x + 512, y + 96)

        def called(text, n=1):
            s.wait(lambda: calls().count(text) >= n, text)

        # Update Now restarts timesyncd, then waits for its answer.
        s.inject("move", *update, "click")
        called('RestartUnit "systemd-timesyncd.service" "replace"')
        waiting = s.settle("waiting")
        s.check(waiting.differs(dialog, status) > 0.01, "Update Now waits")
        ts.Answer("time.example.org")
        answered = s.settle("answered")
        s.check(answered.differs(waiting, status) > 0.01, "the answer was reported")
        # A server typed in is handed to timesyncd (not restarted, which
        # would forget it).
        s.inject("move", x + 200, y + 96, "click", "key", "ctrl+a", "type", "ntp.example.com",
                 "move", *update, "click")  # fmt: skip
        called('SetRuntimeNTPServers ["ntp.example.com"]')
        ts.Answer("ntp.example.com")
        s.settle("answered-new-server")
        # Update Now again hands it over again; with no answer, it fails.
        s.inject("move", *update, "click")
        called('SetRuntimeNTPServers ["ntp.example.com"]', 2)
        s.check(calls().count("RestartUnit") == 1, "timesyncd was restarted only once")
        waiting = s.settle("waiting-again")
        time.sleep(16)
        failed = s.settle("failed")
        s.check(failed.differs(waiting, status) > 0.01, "no answer was reported")
        # The Time Zone tab's hardware clock box.
        s.inject("move", x + 122, y + 40, "click", "sleep", 500, "move", x + 28, y + 291,
                 "click", "sleep", 300, "move", x + 524, y + 332, "click")  # fmt: skip
        called("SetLocalRTC True False True")
        s.settle("local-rtc")
        log = calls()
        s.check("SetNTP" not in log and "SetTime " not in log and "SetTimezone" not in log,
                "nothing else changed")  # fmt: skip
    finally:
        DBusTestCase.tearDownClass()


@scenario(requires=("dbusmock",))
def timedate_other_time_service(s):
    """Internet Time without timesyncd (another time service): no server,
    Update Now greyed. Once a timesyncd older than systemd 255 appears, its
    server shows, greyed (it can't take one while it runs), and Update Now
    restarts it."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    try:
        _, calls = mock_timedate(s)
        empty = s.settle("desktop")
        s.launch("xd-timedate", "--page", "2")
        s.wait_window(title="Date and Time Properties", state="activated")
        dialog = s.settle("no-timesyncd")
        x, y, _, _ = dialog.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
        s.inject("move", x + 512, y + 96, "click", "sleep", 1000)
        s.check("RestartUnit" not in calls(), "Update Now is greyed")
        template = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "timesync.py")
        proc, obj = DBusTestCase.spawn_server_template(template, {"runtime": False},
                                                       stdout=subprocess.DEVNULL,
                                                       system_bus=True)  # fmt: skip
        s.children.append(proc)
        # dbusmock takes the bus name before it has the properties, unlike
        # timesyncd: they're sent again as changed, for xd-timedate to see.
        manager = "org.freedesktop.timesync1.Manager"
        props = obj.GetAll(manager, dbus_interface=dbus.PROPERTIES_IFACE)
        obj.UpdateProperties(manager, props, dbus_interface=MOCK_IFACE)
        server = (x + 80, y + 85, 360, 20)
        s.wait(lambda: s.shot("started").differs(dialog, server) > 0.01,
               "timesyncd's server to show")  # fmt: skip
        s.settle("timesyncd")
        s.inject("move", x + 512, y + 96, "click")
        s.wait(lambda: 'RestartUnit "systemd-timesyncd.service"' in calls(), "the restart")
        s.log("ok: Update Now restarted timesyncd")
    finally:
        DBusTestCase.tearDownClass()


def mock_polkitd(s):
    """Before the compositor starts: a private system bus with dbusmock's
    polkitd, for the session's agent to register with."""
    from dbusmock import DBusTestCase

    DBusTestCase.start_system_bus()
    log = open(os.path.join(s.base, "polkitd.log"), "w")
    proc, _ = DBusTestCase.spawn_server_template("polkitd", {}, stdout=log, system_bus=True)
    s.children.append(proc)


def mock_cups(s, auth=False):
    """A stand-in for cupsd (xd-cupsmock, cupsd itself needs root) on a
    socket in the session's runtime dir, with two printers, a few devices
    and printer sharing off (and with auth, changes that want a password);
    the environment that points libcups at it, and the log of the changes
    it was asked for."""
    runtime = os.environ["XDG_RUNTIME_DIR"]
    sock = os.path.join(runtime, "cups.sock")
    log = os.path.join(s.base, "shots", "cups.log")  # kept with --keep
    s.launch(s.tool("xd-cupsmock"), sock, log, *(["--auth"] if auth else []))
    s.wait(lambda: os.path.exists(sock), "the CUPS stand-in")
    return {"CUPS_SERVER": sock}, log


def file_text(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


@scenario(LUNA_INI)
def printer_ports_sharing(s):
    """A printer's Properties (on xd-cupsmock): Sharing turns the
    scheduler's printer sharing on and shares the printer; Ports' Add
    Port lists CUPS-Get-Devices' ports and takes a typed one; Apply sends
    one CUPS-Add-Modify-Printer with the new device-uri and
    printer-is-shared."""
    env, log = mock_cups(s)
    empty = s.settle("desktop")
    s.launch("xd-printers", "--properties", "Office_Laser", env=env)
    s.wait_window(title="Office_Laser Properties", state="activated")
    general = s.settle("general")
    x, y, w, h = general.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.log(f"properties at {x},{y} {w}x{h}")

    # Sharing: printer sharing is off, so XP SP2's note and link show.
    s.inject("key", "alt+h")
    sharing = s.settle("sharing-off")
    s.check(sharing.differs(general, (x, y + 60, w, h - 120)) > 0.05, "the Sharing tab shows")
    # Its link turns sharing on: cupsAdminSetServerSettings puts back a
    # cupsd.conf that browses.
    s.inject("move", x + 100, y + 168, "click")
    s.wait(lambda: "cupsd.conf Browsing On" in file_text(log), "sharing turned on")
    s.log("ok: the link turned the scheduler's printer sharing on")
    s.inject("sleep", 300, "key", "alt+s")
    shared = s.settle("sharing-on")
    s.check(shared.differs(sharing, (x + 10, y + 100, w - 20, 120)) > 0.05,
            "the note went and Share this printer is chosen")  # fmt: skip

    # Ports: Add Port lists what CUPS-Get-Devices found; a typed URI is
    # taken (Return is New Port).
    s.inject("key", "alt+o")
    ports = s.settle("ports")
    s.inject("key", "alt+t")
    s.wait(lambda: "get-devices" in file_text(log), "CUPS-Get-Devices")
    s.inject("sleep", 500)
    found = s.settle("add-port")
    s.check(found.differs(ports, (x + 20, y + 60, w - 40, 200)) > 0.1, "Add Port opened")
    s.inject("type", "socket://10.0.0.9", "key", "Return")
    added = s.settle("port-added")
    s.check(added.differs(ports, (x + 10, y + 100, w - 20, 200)) > 0.01, "the new port is listed")
    # Configure Port changes the new port's address (it's selected).
    s.inject("key", "alt+c", "sleep", 500, "key", "ctrl+a", "type", "socket://10.0.0.10",
             "key", "Return")  # fmt: skip
    configured = s.settle("port-configured")
    s.check(configured.changed_box(added, (x + 10, y + 100, w - 20, 200), step=1) is not None,
            "Configure Port changed the port")  # fmt: skip

    # Apply: one CUPS-Add-Modify-Printer with the port and sharing.
    s.inject("key", "alt+a")
    s.wait(lambda: "device-uri=socket://10.0.0.10" in file_text(log), "the new port saved")
    s.check("Office_Laser printer-is-shared=true" in file_text(log), "the printer is shared")
    s.inject("move", x + 190, y + 391, "click")
    s.wait_window(title="Office_Laser Properties", gone=True)
    s.log("ok: OK closed Properties")


@scenario(LUNA_INI)
def printer_queue_pause(s):
    """A printer's queue (on xd-cupsmock): Pause Printing, made in a worker
    thread, pauses it, and the title says so; again resumes it."""
    env, log = mock_cups(s)
    s.launch("xd-printers", "--queue", "Office_Laser", env=env)
    s.wait_window(title="Office_Laser", state="activated")
    s.settle("queue")
    s.inject("key", "alt+p", "sleep", 300, "key", "p")
    s.wait(lambda: "pause Office_Laser" in file_text(log), "Pause-Printer")
    s.wait_window(title="Office_Laser - Paused")
    s.log("ok: Pause Printing paused the printer")
    s.inject("key", "alt+p", "sleep", 300, "key", "p")
    s.wait(lambda: "resume Office_Laser" in file_text(log), "Resume-Printer")
    s.wait_window(title="Office_Laser")
    s.log("ok: Pause Printing again resumed it")


@scenario(LUNA_INI)
def printer_apply_password(s):
    """Properties' Apply on a scheduler that wants a printer
    administrator's password (xd-cupsmock --auth): the worker's request
    asks with XP's Connect to box, and goes through with the password."""
    env, log = mock_cups(s, auth=True)
    s.launch("xd-printers", "--properties", "Office_Laser", env=env)
    s.wait_window(title="Office_Laser Properties", state="activated")
    general = s.settle("general")
    s.inject("key", "alt+c", "key", "ctrl+a", "type", "Front desk", "key", "alt+a")
    s.wait(lambda: "password asked" in file_text(log), "the password asked for")
    s.inject("sleep", 500)
    box = s.settle("connect-to")
    s.check(box.differs(general, (0, 0, WIDTH, HEIGHT - 40)) > 0.01, "the Connect to box shows")
    s.inject("type", "secret", "key", "Return")
    s.wait(lambda: "printer-info=Front desk" in file_text(log), "the comment saved")
    me = pwd.getpwuid(os.getuid()).pw_name  # root in CI's container
    s.check(f"authorized for /admin/ as {me} with a password" in file_text(log),
            "it went with the password")  # fmt: skip


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


def mock_accounts(s, systemd=None):
    """A private system bus with tests/dbusmock/accounts.py (us, an
    administrator, and bob, whose home is in the scratch tree), and our
    systemd mock with `systemd`'s parameters if given. Returns a function
    giving an AccountsService object's calls so far, by path."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    bob_home = os.path.join(s.base, "bob")
    os.makedirs(os.path.join(bob_home, "Documents"), exist_ok=True)
    templates = [("accounts.py", {"BobHome": bob_home})]
    if systemd is not None:
        templates.append(("systemd.py", systemd))
    for template, params in templates:
        proc, _ = DBusTestCase.spawn_server_template(
            os.path.join(SOURCE_ROOT, "tests", "dbusmock", template), params,
            stdout=subprocess.DEVNULL, system_bus=True,
        )  # fmt: skip
        s.children.append(proc)
    bus = DBusTestCase.get_dbus(system_bus=True)

    def calls(path="/org/freedesktop/Accounts"):
        obj = bus.get_object("org.freedesktop.Accounts", path)
        return [(str(c[1]), [a for a in c[2]]) for c in obj.GetCalls(dbus_interface=MOCK_IFACE)]

    return bob_home, calls


@scenario(LUNA_INI, requires=("dbusmock",))
def sysdm_delete_profile(s):
    """System Properties' User Profiles (on a dbusmock AccountsService):
    Delete is greyed for our own profile; for bob's it asks XP's "Are you
    sure you want to delete this profile?" and, on Yes, deletes the
    account and its files through AccountsService's DeleteUser."""
    from dbusmock import DBusTestCase

    try:
        bob_home, calls = mock_accounts(s)
        me = pwd.getpwuid(os.getuid())
        passwd = os.path.join(s.base, "passwd")
        with open(passwd, "w") as f:
            f.write(f"{me.pw_name}:x:{me.pw_uid}:{me.pw_gid}:Me:{s.home}:/bin/sh\n")
            f.write(f"bob:x:1999:1999:Bob:{bob_home}:/bin/sh\n")
        empty = s.settle("desktop")
        s.launch("xd-sysdm", "--page", "3", env={"XD_SYSDM_PASSWD": passwd})
        s.wait_window(title="System Properties", state="activated")
        page = s.settle("user profiles")
        x, y, w, h = page.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
        def sysdm_active():
            w = s.window(title="System Properties")
            return w is not None and "activated" in w["states"]

        # The list's first row (below the window's top edge). Only regular
        # accounts are listed: as root (CI's container) ours isn't, and the
        # first row is bob's.
        s.inject("move", x + 80, y + 180, "click", "sleep", 300)
        if me.pw_uid >= 1000:
            # Ours, then Alt+D: nothing (greyed).
            s.inject("key", "alt+d", "sleep", 800)
            s.check(not any(c[0] == "DeleteUser" for c in calls()) and sysdm_active(),
                    "Delete does nothing for our own profile")  # fmt: skip
            s.inject("key", "Down", "sleep", 300)
        else:
            s.log("skipped: our own profile isn't listed when running as a system user")
        # Bob's: the question (a message box), No, then Yes.
        s.inject("key", "alt+d")
        s.wait(lambda: not sysdm_active(), "the Confirm Delete box")
        s.settle("are you sure")
        s.inject("key", "Return")  # No has the focus
        s.wait(sysdm_active, "No to close the box")
        s.check(not any(c[0] == "DeleteUser" for c in calls()), "No deleted nothing")
        s.inject("key", "alt+d")
        s.wait(lambda: not sysdm_active(), "the Confirm Delete box again")
        s.settle("are you sure again")
        s.inject("key", "alt+y")
        s.wait(lambda: ("DeleteUser", [1999, True]) in calls(), "DeleteUser(1999, true)")
        s.log("ok: Yes called DeleteUser(1999, true)")
        gone = s.settle("deleted")
        s.check(gone.differs(page, (x, y, w, h)) > 0, "the profile left the list")
    finally:
        DBusTestCase.tearDownClass()


@scenario(LUNA_INI, requires=("dbusmock",))
def sysdm_boot_next_time(s):
    """Startup and Recovery (on a dbusmock logind that can start a boot
    loader entry the next time only): choosing another entry ticks "Start
    this one the next time only" and OK sets it through
    SetRebootToBootLoaderEntry; unticking it later clears it with ""."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    try:
        proc, logind = DBusTestCase.spawn_server_template(
            "logind", {}, stdout=subprocess.DEVNULL, system_bus=True
        )
        s.children.append(proc)
        manager = "org.freedesktop.login1.Manager"
        logind.AddMethod(manager, "CanRebootToBootLoaderEntry", "", "s", 'ret = "yes"',
                         dbus_interface=MOCK_IFACE)  # fmt: skip
        logind.AddProperty(manager, "BootLoaderEntries",
                           dbus.Array(["arch.conf", "windows.conf"], signature="s"),
                           dbus_interface=MOCK_IFACE)  # fmt: skip
        logind.AddProperty(manager, "RebootToBootLoaderEntry", "", dbus_interface=MOCK_IFACE)
        logind.AddMethod(manager, "SetRebootToBootLoaderEntry", "s", "",
                         f'self.Set("{manager}", "RebootToBootLoaderEntry", args[0])',
                         dbus_interface=MOCK_IFACE)  # fmt: skip

        def set_calls():
            return [str(c[1][0]) for c in logind.GetMethodCalls("SetRebootToBootLoaderEntry",
                                                               dbus_interface=MOCK_IFACE)]

        def sysdm_active():
            w = s.window(title="System Properties")
            return w is not None and "activated" in w["states"]

        s.launch("xd-sysdm", "--page", "4")
        s.wait_window(title="System Properties", state="activated")
        s.settle("advanced")
        s.inject("key", "alt+s")
        s.wait(lambda: not sysdm_active(), "Startup and Recovery")
        s.settle("startup and recovery")
        # The list (Alt+D), its last entry (whatever the host's EFI
        # variables call the default): the check box ticks itself.
        s.inject("key", "alt+d", "sleep", 300, "key", "End", "sleep", 300)
        s.settle("windows chosen")
        # On to OK past the check box (Enter would open the list).
        s.inject("key", "Tab", "key", "Tab", "key", "Return")
        s.wait(lambda: set_calls() == ["windows.conf"], 'SetRebootToBootLoaderEntry("windows.conf")')
        s.wait(sysdm_active, "Startup and Recovery to close")
        s.log("ok: OK set windows.conf for the next time only")

        s.inject("key", "alt+s")
        s.wait(lambda: not sysdm_active(), "Startup and Recovery again")
        s.settle("set for next time")
        s.inject("key", "alt+n", "sleep", 300, "key", "Tab", "key", "Return")
        s.wait(lambda: set_calls() == ["windows.conf", ""], 'SetRebootToBootLoaderEntry("")')
        s.log("ok: unticking it cleared the one-off entry")
    finally:
        DBusTestCase.tearDownClass()


@scenario(LUNA_INI, requires=("dbusmock",))
def users_automatic_logon(s):
    """XP's advanced User Accounts (xd-users --advanced, on a dbusmock
    AccountsService and a systemd running SDDM): unticking "Users must
    enter a user name and password" asks which user logs on
    automatically and sets AutomaticLogin; ticking it again turns it off."""
    from dbusmock import DBusTestCase

    try:
        _, calls = mock_accounts(s, systemd={"display_manager": "sddm.service"})
        mine = f"/org/freedesktop/Accounts/User{os.getuid()}"
        def users_active():
            w = s.window(title="User Accounts")
            return w is not None and "activated" in w["states"]

        s.launch("xd-users", "--advanced")
        s.wait_window(title="User Accounts", state="activated")
        s.settle("user accounts")
        s.inject("key", "alt+m", "sleep", 300, "key", "alt+a")
        s.wait(lambda: not users_active(), "the Automatically Log On box")
        s.settle("automatically log on")
        s.inject("key", "Return")
        s.wait(lambda: ("SetAutomaticLogin", [True]) in calls(mine), "SetAutomaticLogin(true)")
        s.log("ok: automatic logon was turned on for us (the selected user)")
        s.settle("applied")
        s.inject("key", "alt+m", "sleep", 300, "key", "alt+a")
        s.wait(lambda: ("SetAutomaticLogin", [False]) in calls(mine), "SetAutomaticLogin(false)")
        s.log("ok: ticking it again turned it off")
        s.settle("turned off")
        s.inject("key", "Escape")
        s.wait_window(title="User Accounts", gone=True)
    finally:
        DBusTestCase.tearDownClass()


PRIVACY_STORE = {
    "location": {"location": {"org.example.Maps": ["EXACT", "1800000000"]}},
    "devices": {"camera": {"org.example.Cam": ["yes"]}},
    "notifications": {"notification": {"org.example.Cam": ["no"]}},
}


def privacy_setup(s):
    """Before the compositor starts: GSettings in a keyfile of the scratch
    home (shared by the applet and the shell, never the real dconf); the
    shell's housekeeping on in this test session, with a temporary folder
    of its own; a dbusmock GeoClue on a private system bus (in use), and a
    dbusmock permission store on the session bus. In the Recycle Bin, an
    item deleted 60 days ago and one deleted now."""
    from dbusmock import DBusTestCase

    os.environ["GSETTINGS_BACKEND"] = "keyfile"
    os.environ["XD_HOUSEKEEPING"] = "1"
    tmp = os.path.join(s.home, "tmp")
    os.makedirs(tmp)
    os.environ["TMPDIR"] = tmp
    with open(os.path.join(tmp, "in-use.txt"), "w") as f:
        f.write("a program's\n")
    trash = os.path.join(s.data, "Trash")
    for name, days in (("old.txt", 60), ("new.txt", 0)):
        os.makedirs(os.path.join(trash, "files"), exist_ok=True)
        os.makedirs(os.path.join(trash, "info"), exist_ok=True)
        with open(os.path.join(trash, "files", name), "w") as f:
            f.write("deleted\n")
        when = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(time.time() - days * 86400))
        with open(os.path.join(trash, "info", name + ".trashinfo"), "w") as f:
            f.write(f"[Trash Info]\nPath={s.home}/{name}\nDeletionDate={when}\n")
    doc = os.path.join(s.home, "Documents", "Notes.txt")
    os.makedirs(os.path.dirname(doc), exist_ok=True)
    with open(doc, "w") as f:
        f.write("notes\n")
    write_recent(s, [("file://" + urllib.parse.quote(doc), "2026-10-01T10:00:00Z")])

    DBusTestCase.start_system_bus()
    for template, system, params in (
        ("geoclue.py", True, {"InUse": True}),
        ("permissionstore.py", False, {"tables": PRIVACY_STORE}),
    ):
        log = open(os.path.join(s.base, f"mock-{template}.log"), "w")
        proc, _ = DBusTestCase.spawn_server_template(
            os.path.join(SOURCE_ROOT, "tests", "dbusmock", template), params, stdout=log,
            system_bus=system,
        )  # fmt: skip
        s.children.append(proc)


def gsettings_keyfile(s):
    c = configparser.ConfigParser(interpolation=None, strict=False)
    c.optionxform = str
    c.read(os.path.join(os.environ["XDG_CONFIG_HOME"], "glib-2.0", "settings", "keyfile"))
    return c


@scenario(LUNA_INI, requires=("dbusmock",), setup=privacy_setup)
def privacy_settings(s):
    """Privacy Properties (on a dbusmock GeoClue and permission store):
    turning on "Empty old items from the Recycle Bin" makes the shell
    delete the item deleted 60 days ago (not today's, nor a temporary file
    in use); Clear List empties recently-used.xbel; Location turns on and
    denies a program; Program Permissions denies one program's camera and
    forgets another's answers."""
    import dbus

    store = dbus.Interface(
        dbus.SessionBus().get_object(
            "org.freedesktop.impl.portal.PermissionStore",
            "/org/freedesktop/impl/portal/PermissionStore",
        ),
        "org.freedesktop.impl.portal.PermissionStore",
    )

    def lookup(table, entry):
        try:
            return {str(k): [str(x) for x in v] for k, v in store.Lookup(table, entry)[0].items()}
        except dbus.exceptions.DBusException:
            return {}

    s.launch("xd-privacy")
    s.wait_window(title="Privacy Properties", state="activated")
    s.settle("general")
    trash = os.path.join(s.data, "Trash")

    # Empty old items from the Recycle Bin, Apply.
    s.inject("key", "alt+e", "sleep", 200, "key", "alt+a", "sleep", 500)

    def privacy_key(key):
        c = gsettings_keyfile(s)
        return c.get("org/gnome/desktop/privacy", key, fallback=None)

    s.wait(lambda: privacy_key("remove-old-trash-files") == "true", "the setting saved")
    s.log("ok: remove-old-trash-files saved")
    s.wait(lambda: not os.path.exists(os.path.join(trash, "files", "old.txt")),
           "the old item to go", timeout=30)  # fmt: skip
    s.check(not os.path.exists(os.path.join(trash, "info", "old.txt.trashinfo")) and
            os.path.exists(os.path.join(trash, "files", "new.txt")) and
            os.path.exists(os.path.join(trash, "info", "new.txt.trashinfo")) and
            os.path.exists(os.path.join(s.home, "tmp", "in-use.txt")),
            "only the item deleted 60 days ago went")  # fmt: skip

    # Clear List, at once.
    xbel = os.path.join(s.data, "recently-used.xbel")
    s.inject("key", "alt+c")

    def cleared():
        with open(xbel) as f:
            return "<bookmark " not in f.read()

    s.wait(cleared, "the recent documents list to clear")
    s.log("ok: Clear List emptied recently-used.xbel")

    # Location: turn it on, deny Maps.
    s.inject("key", "ctrl+Tab", "sleep", 300)
    s.settle("location")
    s.inject("key", "alt+u", "sleep", 200, "key", "alt+k", "sleep", 200)
    s.inject("key", "space", "sleep", 200, "key", "alt+d", "sleep", 500)
    s.wait(lambda: lookup("location", "location").get("org.example.Maps") == ["NONE", "1800000000"],
           "Maps denied its location")  # fmt: skip
    s.log("ok: Maps denied, its last use kept")
    s.inject("key", "alt+a", "sleep", 500)
    s.wait(lambda: gsettings_keyfile(s).get("org/gnome/system/location", "enabled",
                                            fallback=None) == "true",
           "location turned on")  # fmt: skip
    s.log("ok: location services on")
    s.settle("location-on")

    # Program Permissions: Cam's camera denied, then all Maps' answers removed.
    s.inject("key", "ctrl+Tab", "sleep", 300)
    s.settle("permissions")
    # The rows: Cam, Camera, Notifications, Maps, Location.
    s.inject("key", "alt+n", "sleep", 200, "key", "Home", "key", "Down", "sleep", 200,
             "key", "alt+d", "sleep", 500)  # fmt: skip
    s.wait(lambda: lookup("devices", "camera").get("org.example.Cam") == ["no"],
           "Cam's camera denied")  # fmt: skip
    s.log("ok: camera denied")
    s.inject("key", "alt+n", "sleep", 200, "key", "End", "key", "Up", "sleep", 200,
             "key", "alt+r", "sleep", 500)  # fmt: skip
    s.wait(lambda: "org.example.Maps" not in lookup("location", "location"),
           "Maps' answers removed")  # fmt: skip
    s.check(lookup("devices", "camera").get("org.example.Cam") == ["no"] and
            lookup("notifications", "notification").get("org.example.Cam") == ["no"],
            "the other program's answers stay")  # fmt: skip
    s.settle("permissions-after")


FLATPAK_APP = "org.xd.Test.Hello"


def scratch_flatpak(s):
    """Before the compositor starts: scratch user and system Flatpak
    installations (FLATPAK_USER_DIR, FLATPAK_SYSTEM_DIR), the real ones
    untouched, holding a tiny runtime and app built and installed offline
    from a local repository, and a remote in each."""
    fp = os.path.join(s.base, "flatpak")
    os.environ.update(
        {
            "FLATPAK_USER_DIR": os.path.join(fp, "user"),
            "FLATPAK_SYSTEM_DIR": os.path.join(fp, "system"),
            "FLATPAK_CONFIG_DIR": os.path.join(fp, "config"),
        }
    )
    s.flatpak = shutil.which("flatpak")
    if s.flatpak is None:
        return
    rt, app, repo = (os.path.join(fp, d) for d in ("src-rt", "src-app", "repo"))
    for d in (os.path.join(rt, "files", "bin"), os.path.join(app, "files", "bin"),
              os.path.join(app, "export")):  # fmt: skip
        os.makedirs(d)
    with open(os.path.join(rt, "metadata"), "w") as f:
        f.write("[Runtime]\nname=org.xd.Test.Platform\n")
    with open(os.path.join(app, "files", "bin", "hello"), "w") as f:
        f.write("#!/bin/sh\necho hello\n")
    with open(os.path.join(app, "metadata"), "w") as f:
        f.write(f"[Application]\nname={FLATPAK_APP}\nruntime=org.xd.Test.Platform/"
                f"{subprocess.check_output([s.flatpak, '--default-arch'], text=True).strip()}/1\n"
                "command=hello\n\n[Context]\nshared=network;\nsockets=wayland;pulseaudio;\n"
                "filesystems=xdg-download:ro;\n")  # fmt: skip

    def run(*cmd, user_dir=None):
        env = dict(os.environ)
        if user_dir:
            env["FLATPAK_USER_DIR"] = user_dir
        subprocess.run([s.flatpak, *cmd], env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)  # fmt: skip

    run("build-export", "--runtime", "--files=files", repo, rt, "1")
    run("build-export", repo, app, "stable")
    run("build-update-repo", repo)
    run("--user", "remote-add", "--no-gpg-verify", "test-repo", "file://" + repo)
    run("--user", "install", "-y", "--noninteractive", "test-repo", FLATPAK_APP + "//stable")
    # The system installation is filled as a user one would be: the real
    # one needs root (Flatpak's system helper), which the tests never use.
    run("--user", "remote-add", "--no-gpg-verify", "sys-repo", "file://" + repo,
        user_dir=os.environ["FLATPAK_SYSTEM_DIR"])  # fmt: skip


@scenario(LUNA_INI, setup=scratch_flatpak)
def flatpak_permissions(s):
    """Flatpak (xd-flatpak) on scratch installations with a test app: by
    keyboard, Permissions..., Network off and Apply write the user's
    override as `flatpak override --user` would; a filesystem added read
    only joins it; Restore Defaults removes the file; on Remotes, Disable
    disables the user's remote through libflatpak."""
    if s.flatpak is None:
        s.log("skipped: flatpak isn't installed")
        return
    override = os.path.join(os.environ["FLATPAK_USER_DIR"], "overrides", FLATPAK_APP)

    def context():
        c = configparser.ConfigParser(interpolation=None, strict=False, delimiters=("=",))
        c.optionxform = str
        c.read(override)
        return c["Context"] if c.has_section("Context") else {}

    empty = s.settle("desktop")
    s.launch("xd-flatpak")
    s.wait_window(title="Flatpak", state="activated")
    apps = s.settle("applications")
    s.check(apps.differs(empty, (40, 150, 400, 60)) > 0.05, "the test app is listed")
    # The list has the focus and its first row is selected.
    s.inject("key", "alt+p", "sleep", 800)
    perms = s.settle("permissions")
    s.check(perms.differs(apps, (0, 0, WIDTH, HEIGHT - 40)) > 0.05, "Permissions... opened")
    s.inject("key", "alt+n", "sleep", 300, "key", "alt+a", "sleep", 500)
    s.wait(lambda: context().get("shared") == "!network;", "shared=!network; written")
    s.log("ok: Network off wrote shared=!network;")
    # File System (third tab): Add..., xdg-music read only, OK, Apply.
    s.inject("key", "ctrl+Tab", "key", "ctrl+Tab", "sleep", 300, "key", "alt+d", "sleep", 800,
             "type", "xdg-music", "key", "alt+o", "key", "Return", "sleep", 500, "key", "alt+a",
             "sleep", 500)  # fmt: skip
    s.wait(lambda: "xdg-music:ro;" in context().get("filesystems", ""), "xdg-music:ro written")
    s.check(context().get("shared") == "!network;", "the earlier change kept")
    s.settle("filesystem")
    # Restore Defaults, Apply: nothing of the user's is left, so no file.
    s.inject("key", "alt+f", "sleep", 300, "key", "alt+a", "sleep", 500)
    s.wait(lambda: not os.path.exists(override), "the override file to go")
    s.log("ok: Restore Defaults removed the override")
    s.inject("key", "Escape", "sleep", 800)
    # Remotes: the user's remote comes first; Disable.
    s.inject("key", "alt+m", "sleep", 500)
    s.settle("remotes")
    s.inject("key", "alt+s", "sleep", 1000)

    def disabled():
        out = subprocess.run([s.flatpak, "remotes", "--user", "--show-disabled",
                              "--columns=name,options"], capture_output=True, text=True)  # fmt: skip
        return any(l.startswith("test-repo") and "disabled" in l for l in out.stdout.splitlines())

    s.wait(disabled, "test-repo disabled")
    s.settle("remote-disabled")
    s.log("ok: Disable disabled the user's remote")


# ---- input methods ---------------------------------------------------------------

# A text box that writes what it holds to argv[1] whenever it changes.
GTK_ENTRY = r"""
import sys
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
w = Gtk.Window(title="IME test")
w.set_default_size(360, 120)
e = Gtk.Entry()
e.set_input_purpose(Gtk.InputPurpose.ALPHA)
def changed(entry):
    with open(sys.argv[1], "w") as f:
        f.write(entry.get_text())
e.connect("changed", changed)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, border_width=20)
box.pack_start(e, False, False, 0)
w.add(box)
w.connect("destroy", Gtk.main_quit)
w.show_all()
Gtk.main()
"""

IME_INI = CLASSIC_INI + "\n[Keyboard]\nInputMethod=fcitx5\n"


def stand_in_ime(s):
    """The compositor starts xd-test-ime in place of Fcitx 5."""
    os.environ["XD_IME_COMMAND"] = s.tool("xd-test-ime")
    os.environ["XD_TEST_IME_LOG"] = os.path.join(s.base, "ime.log")


def magenta(shot, rect):
    """The bounding box of xd-test-ime's candidate window in rect, or None."""
    x0, y0, w, h = rect
    xs, ys = [], []
    for y in range(y0, y0 + h, 2):
        for x in range(x0, x0 + w, 2):
            r, g, b = shot.pixel(x, y)
            if r > 240 and g < 20 and b > 240:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)


@scenario(IME_INI, setup=stand_in_ime)
def input_method_relay(s):
    """The input method the compositor started (a stand-in for Fcitx 5)
    activates for a GTK text box, hears its content type and surrounding
    text, types into it through the compositor (A becomes 日本語; other
    keys come back through its own virtual keyboard), and shows its
    candidate window at the caret. Other programs aren't offered the input
    method protocol."""
    if not have_gtk3(s):
        return
    log = os.path.join(s.base, "ime.log")

    def heard(what):
        try:
            with open(log) as f:
                return what in f.read()
        except OSError:
            return False

    s.wait(lambda: heard("ready"), "the input method to connect")
    probe = subprocess.run([s.tool("xd-test-ime"), "--probe"], stdout=subprocess.PIPE,
                           timeout=10).stdout.decode()  # fmt: skip
    s.check("input-method no" in probe, "another program isn't offered the input method")
    s.check("virtual-keyboard yes" in probe, "virtual keyboards are (--allow-virtual-input)")
    shell = s.wait(lambda: session_program("xd-shell"), "the shell")[0]
    with open(f"/proc/{shell}/environ", "rb") as f:
        env = dict(v.split(b"=", 1) for v in f.read().split(b"\0") if b"=" in v)
    s.check(env.get(b"XMODIFIERS") == b"@im=fcitx" and env.get(b"QT_IM_MODULE") == b"fcitx"
            and b"GTK_IM_MODULE" not in env,
            "programs get XMODIFIERS and QT_IM_MODULE for Fcitx 5, and no GTK_IM_MODULE")  # fmt: skip

    script = os.path.join(s.base, "entry.py")
    with open(script, "w") as f:
        f.write(GTK_ENTRY)
    text = os.path.join(s.base, "entry.txt")
    # Our GTK theme, as programs get it in a session (the positions below
    # are its layout's).
    s.launch(sys.executable, script, text,
             env={"GTK_THEME": "XeonDesktop", "GSETTINGS_BACKEND": "memory"})  # fmt: skip
    s.wait_window(title="IME test", state="activated")
    # GTK 3 asks for the input method when its text box gets the focus
    # with the protocol ready, which isn't so yet when the window first
    # comes up: the desktop is clicked, then the text box.
    s.settle("window")
    s.inject("move", 600, 400, "click", "sleep", 300, "move", 190, 65, "click")
    s.wait(lambda: heard("activate"), "the input method to be activated")
    s.check(heard("content-type"), "it heard the text box's content type")
    s.check(heard('surrounding ""'), "it heard the (empty) surrounding text")
    box = s.wait(lambda: magenta(s.shot("popup"), (0, 0, WIDTH, HEIGHT)),
                 "the candidate window")  # fmt: skip
    # The text box is at y 55-75 (its window cascades to the top left).
    s.check(30 <= box[0] <= 60 and 60 <= box[1] <= 85,
            f"the candidate window is under the caret, at the text box's start ({box})")  # fmt: skip
    s.check(heard("rectangle"), "it was told where the caret is")
    s.inject("type", "xa")

    def entry():
        try:
            with open(text) as f:
                return f.read()
        except OSError:
            return ""

    s.wait(lambda: entry() == "x日本語", f"the text box to hold x日本語 (has {entry()!r})")
    s.check(heard("forwarded"), "the X key came back through its virtual keyboard")
    s.log(f"ok: the text box holds {entry()!r}")
    s.wait(lambda: heard('surrounding "x日本語"'), "the new surrounding text")
    moved = s.wait(lambda: (b := magenta(s.shot("typed"), (0, 0, WIDTH, HEIGHT))) and
                   b[0] > box[0] + 20 and b, "the candidate window to follow the caret")  # fmt: skip
    s.log(f"ok: the candidate window followed the caret ({box} -> {moved})")
    # Another window takes the keyboard: the input method goes inactive and
    # its candidate window goes.
    s.inject("key", "super")
    s.wait(lambda: heard("deactivate"), "the input method to be deactivated")
    s.wait(lambda: magenta(s.shot("menu"), (0, 0, WIDTH, HEIGHT)) is None,
           "the candidate window to hide")  # fmt: skip


def fake_frameworks(s):
    """Fcitx 5 "installed": fcitx5 and fcitx5-configtool stand-ins on PATH
    that say they ran, and its autostart entry in the system's."""
    bin_dir = os.path.join(s.base, "bin")
    os.makedirs(bin_dir)
    for name in ("fcitx5", "fcitx5-configtool"):
        path = os.path.join(bin_dir, name)
        with open(path, "w") as f:
            f.write("#!/bin/sh\n"
                    f'env > "{s.base}/{name}.ran"\n'
                    'exec sleep 600\n')  # fmt: skip
        os.chmod(path, 0o755)
    os.environ["PATH"] = bin_dir + os.pathsep + os.environ["PATH"]
    etc = os.path.join(s.base, "etc-xdg")
    os.makedirs(os.path.join(etc, "autostart"))
    with open(os.path.join(etc, "autostart", "org.fcitx.Fcitx5.desktop"), "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Fcitx 5\nExec=fcitx5\n")
    os.environ["XDG_CONFIG_DIRS"] = etc


@scenario(setup=fake_frameworks)
def keyboard_text_services(s):
    """Keyboard Properties' Text Services tab: the installed input method
    is chosen, its Properties... opens its own configuration tool, a Compose
    key is picked; OK saves both, the compositor starts the input method
    with XMODIFIERS for X11 programs, and its own autostart entry no longer
    starts it in XeonDesktop."""
    s.launch(s.tool("xd-keyboard"), "--page", "2", env={"GSETTINGS_BACKEND": "memory"})
    s.wait_window(title="Keyboard Properties", state="activated")
    s.settle("text services")
    # Input method: (None) -> Fcitx 5, then its Properties...
    s.inject("key", "alt+m", "sleep", 200, "key", "Down", "sleep", 300, "key", "alt+p")
    ran = os.path.join(s.base, "fcitx5-configtool.ran")
    s.wait(lambda: os.path.exists(ran), "Properties... to run fcitx5-configtool")
    s.log("ok: Properties... ran fcitx5-configtool")
    # Compose key: (None) -> the first one xkeyboard-config lists.
    s.wait_window(title="Keyboard Properties", state="activated")
    s.inject("key", "alt+c", "sleep", 200, "key", "Down", "sleep", 300)
    s.settle("chosen")
    s.inject("key", "alt+a")  # Apply
    s.wait(lambda: s.ini().get("Keyboard", "InputMethod", fallback="") == "fcitx5",
           "InputMethod=fcitx5")  # fmt: skip
    s.log("ok: InputMethod=fcitx5 saved")
    s.check(s.ini()["Keyboard"].get("Options", "").startswith("compose:"),
            f"a Compose key saved ({s.ini()['Keyboard'].get('Options')!r})")  # fmt: skip
    started = os.path.join(s.base, "fcitx5.ran")
    s.wait(lambda: os.path.exists(started), "the compositor to start fcitx5")
    with open(started) as f:
        env = f.read().splitlines()
    s.check("XMODIFIERS=@im=fcitx" in env and not any(l.startswith("GTK_IM_MODULE=") for l in env),
            "it runs with XMODIFIERS=@im=fcitx, without GTK_IM_MODULE")  # fmt: skip
    entry = os.path.join(os.environ["XDG_CONFIG_HOME"], "autostart", "org.fcitx.Fcitx5.desktop")
    s.wait(lambda: os.path.exists(entry), "Fcitx 5's autostart entry to be overridden")
    with open(entry) as f:
        text = f.read()
    s.check("NotShowIn=XeonDesktop;" in text and "Exec=fcitx5" in text,
            "its autostart entry is kept for other desktops, not XeonDesktop")  # fmt: skip


def mock_geoclue(s):
    """Before the compositor starts: a private system bus with GeoClue
    (tests/dbusmock/geoclue.py, finding London), and the compositor's gamma
    trace, since headless outputs have no gamma to see."""
    from dbusmock import DBusTestCase

    os.environ["XD_GAMMA_TRACE"] = os.path.join(s.base, "shots", "gamma.log")
    DBusTestCase.start_system_bus()
    template = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "geoclue.py")
    proc, _ = DBusTestCase.spawn_server_template(
        template, {"latitude": 51.5074, "longitude": -0.1278}, stdout=subprocess.DEVNULL,
        system_bus=True,
    )
    s.children.append(proc)


def gamma_trace(s):
    try:
        with open(os.environ["XD_GAMMA_TRACE"]) as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def client_origin(s, before, title, width, height, top=0):
    """The top left of a new window's client area (width x height), from
    the bottom right of what changed since `before` below `top` (the
    window it came from changes too: its caption goes inactive)."""
    shot = s.settle(title.lower().replace(" ", "-"))
    box = shot.changed_box(before, (0, top, WIDTH, HEIGHT - 40 - top))
    s.check(box is not None, f"{title} is up at {box}")
    return shot, box[0] + box[2] - 3 - width, box[1] + box[3] - 2 - height


@scenario(requires=("dbusmock",), setup=mock_geoclue)
def night_light(s):
    """Display Properties' Linux tab: Night Light... turns it on from
    sunset to sunrise, with GeoClue (dbusmock) finding the place; Apply
    writes [NightLight]; then All the time, and the compositor's gamma ramp
    (seen through XD_GAMMA_TRACE) fades to 3400 K's colours."""
    s.check(not gamma_trace(s) or "night-light" not in "".join(gamma_trace(s)),
            "night light starts off")  # fmt: skip
    empty = s.settle("desktop")
    s.launch("xd-desk", "--page", "5")
    s.wait_window(title="Display Properties", state="activated")
    props, x, y = client_origin(s, empty, "Display Properties", 404, 448)

    def open_dialog(label):
        s.inject("move", x + 32 + 55, y + 260 + 11, "click")
        s.wait_window(title="Night Light", state="activated")
        return client_origin(s, props, "Night Light", 320, 424, top=y + 10)

    _, nx, ny = open_dialog("night-light")
    # Turn on night light, then Sunset to sunrise: GeoClue is asked.
    s.inject("move", nx + 20, ny + 20, "click", "sleep", 200,
             "move", nx + 32, ny + 202, "click")  # fmt: skip
    s.wait(lambda: s.window(title="Night Light") is not None, "the dialog")
    time.sleep(1.5)  # the mock's fix takes 300 ms
    found = s.settle("location-found")
    # The latitude box shows the place GeoClue gave (greyed: GeoClue's).
    lat_box = (nx + 82, ny + 318, 66, 17)
    dark = sum(1 for yy in range(lat_box[1], lat_box[1] + lat_box[3], 2)
               for xx in range(lat_box[0], lat_box[0] + lat_box[2], 2)
               if max(found.pixel(xx, yy)) < 170)  # fmt: skip
    s.check(dark > 5, f"the latitude was filled in ({dark} dark pixels)")
    s.inject("key", "Return")  # OK
    s.wait_window(title="Night Light", gone=True)
    s.inject("move", x + 322 + 37, y + 416 + 11, "click")  # Apply

    def saved():
        c = s.ini()
        return c.has_section("NightLight") and c["NightLight"].get("Enabled") == "true" and c[
            "NightLight"].get("Schedule") == "sunset" and c["NightLight"]

    nl = s.wait(saved, "[NightLight] to be saved")
    s.check(nl.get("Latitude") == "51.5074" and nl.get("Longitude") == "-0.1278",
            f"GeoClue's place was saved ({nl.get('Latitude')}, {nl.get('Longitude')})")

    # All the time: the ramp fades to 3400 K.
    open_dialog("night-light-again")
    s.inject("move", nx + 32, ny + 180, "click", "sleep", 200, "key", "Return")
    s.wait_window(title="Night Light", gone=True)
    s.inject("move", x + 322 + 37, y + 416 + 11, "click")
    s.wait(lambda: s.ini()["NightLight"].get("Schedule") == "always", "Schedule=always")

    def warm():
        lines = gamma_trace(s)
        return lines and lines[-1].startswith("HEADLESS-1 night-light 3400 ") and lines[-1]

    last = s.wait(warm, "the ramp to reach 3400 K")
    s.check("r=1.000 g=0.746 b=0.541" in last, f"3400 K's ramp: {last}")
    steps = [l for l in gamma_trace(s) if " night-light " in l]
    s.check(len(steps) > 5, f"it faded in over {len(steps)} steps")
    s.inject("key", "Escape")
    s.wait_window(title="Display Properties", gone=True)


def write_calibration_profile(path, red=0.5):
    """A minimal ICC v2 monitor profile with only a vcgt tag: a 256-entry
    table, red scaled by `red`, green and blue straight. (Written byte by
    byte: there's no ICC writer for Python here.)"""
    import struct

    n = 256
    table = b"".join(
        struct.pack(">H", round(i / (n - 1) * scale * 65535))
        for scale in (red, 1.0, 1.0)
        for i in range(n)
    )
    vcgt = b"vcgt" + b"\0" * 4 + struct.pack(">IHHH", 0, 3, n, 2) + table
    offset = 128 + 4 + 12
    size = offset + len(vcgt)
    header = struct.pack(
        ">I4sI4s4s4s12s4s4sIII8sI12s4s16s28s",
        size, b"lcms", 0x02100000, b"mntr", b"RGB ", b"XYZ ", b"\0" * 12, b"acsp", b"\0" * 4,
        0, 0, 0, b"\0" * 8, 0,
        struct.pack(">iii", 0xF6D6, 0x10000, 0xD32D),  # D50
        b"\0" * 4, b"\0" * 16, b"\0" * 28,
    )  # fmt: skip
    assert len(header) == 128
    tags = struct.pack(">I4sII", 1, b"vcgt", offset, len(vcgt))
    with open(path, "wb") as f:
        f.write(header + tags + vcgt)


def mock_colord(s):
    """Before the compositor starts: a private system bus with colord
    (tests/dbusmock/colord.py: a printer, a scanner, colord's profiles), a
    calibrated profile in the user's store, and the gamma trace."""
    from dbusmock import DBusTestCase

    os.environ["XD_GAMMA_TRACE"] = os.path.join(s.base, "shots", "gamma.log")
    store = os.path.join(s.data, "icc")
    os.makedirs(store, exist_ok=True)
    write_calibration_profile(os.path.join(store, "Calibrated.icc"))
    DBusTestCase.start_system_bus()
    log = open(os.path.join(s.base, "shots", "colord.log"), "w")
    template = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "colord.py")
    proc, _ = DBusTestCase.spawn_server_template(template, {}, stdout=log, system_bus=True)
    s.children.append(proc)


@scenario(requires=("dbusmock",), setup=mock_colord)
def color_management(s):
    """Color Management (on a dbusmock colord): the monitor is registered
    and shown first, the store's profile is installed; Add... associates
    it, which makes it the default, so [ColorProfiles] names it and the
    compositor loads its calibration (vcgt) into the gamma ramp; Set as
    Default Profile on sRGB moves both on."""
    import dbus

    bus = dbus.SystemBus()
    colord = dbus.Interface(bus.get_object("org.freedesktop.ColorManager",
                                           "/org/freedesktop/ColorManager"),
                            "org.freedesktop.ColorManager")  # fmt: skip
    empty = s.settle("desktop")
    s.launch("xd-color")
    s.wait_window(title="Color Management", state="activated")
    shot = s.settle("color-management")
    box = shot.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(box is not None, f"Color Management is up at {box}")
    device = s.wait(lambda: _find(colord.FindDeviceById, "xrandr-HEADLESS-1"),
                    "the monitor to be registered")  # fmt: skip
    stored = os.path.join(s.data, "icc", "Calibrated.icc")
    mine = str(s.wait(lambda: _find(colord.FindProfileByFilename, stored), "the store's profile"))
    s.log(f"ok: monitor {device} registered, {mine} installed")

    def device_profiles():
        obj = bus.get_object("org.freedesktop.ColorManager", device)
        return [str(p) for p in obj.Get("org.freedesktop.ColorManager.Device", "Profiles",
                                        dbus_interface=dbus.PROPERTIES_IFACE)]  # fmt: skip

    def associate(name):
        # Add... (the monitor is the device shown); the profile list has
        # the keyboard: typing finds a profile, Enter picks it.
        before = s.settle("before-add")
        s.inject("key", "alt+a")
        s.wait(lambda: s.shot("associate").differs(before, box) > 0.1,
               "Associate Color Profile")  # fmt: skip
        s.settle("associate")
        s.inject("type", name, "sleep", 300, "key", "Return")

    associate("Calibrated")
    s.wait(lambda: device_profiles() == [mine], "the profile to be associated")
    s.log("ok: Add... associated the store's profile with the monitor")

    def profile_setting():
        c = s.ini()
        return c.has_section("ColorProfiles") and c["ColorProfiles"].get("HEADLESS-1")

    s.check(s.wait(profile_setting, "[ColorProfiles]") == stored,
            "desktop.ini names it for HEADLESS-1")  # fmt: skip

    def trace_ends(prefix):
        lines = gamma_trace(s)
        return lines and lines[-1].startswith(prefix) and lines[-1]

    last = s.wait(lambda: trace_ends("HEADLESS-1 calibration 6500 "),
                  "the compositor to load the calibration")  # fmt: skip
    s.check("r=0.500 g=1.000 b=1.000" in last, f"the ramp is the profile's vcgt: {last}")

    # sRGB too, then made the default: colord's sRGB has no curves, so
    # the compositor drops them.
    associate("sRGB")
    srgb = str(colord.FindProfileById("icc-srgb"))
    s.wait(lambda: device_profiles() == [mine, srgb], "sRGB to be associated")
    s.settle("two-profiles")
    # Its row, the second in the list, then Set as Default Profile.
    s.inject("move", box[0] + 90, box[1] + 162, "click", "sleep", 300, "key", "alt+s")
    s.wait(lambda: device_profiles()[0] == srgb, "sRGB to be the default")
    s.wait(lambda: profile_setting() == "/usr/share/color/icc/colord/sRGB.icc",
           "[ColorProfiles] to follow")  # fmt: skip
    s.wait(lambda: trace_ends("HEADLESS-1 none 6500 "), "the calibration to go")
    s.log("ok: Set as Default Profile moved the monitor to sRGB, without curves")
    s.inject("key", "Escape")
    s.wait_window(title="Color Management", gone=True)


def _find(method, arg):
    try:
        return method(arg)
    except Exception:
        return None


# ---- Passwords and Keys -------------------------------------------------------------


def secret_service_call(path, iface, method, args=None, sig=None):
    """A call to the session's (mock) Secret Service; its unpacked result."""
    from gi.repository import Gio, GLib

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    params = GLib.Variant(sig, args) if sig else None
    r = bus.call_sync("org.freedesktop.secrets", path, iface, method, params, None,
                      Gio.DBusCallFlags.NONE, 5000, None)  # fmt: skip
    return r.unpack()


def secret_prop(path, iface, name):
    return secret_service_call(path, "org.freedesktop.DBus.Properties", "Get", (iface, name),
                               "(ss)")[0]  # fmt: skip


def stop_scratch_gpg(home):
    """gpg-agent keeps its environment unreadable, so the session's
    teardown can't tell it's ours: it's found by its scratch home on its
    command line instead, and its socket directory (in the real
    /run/user/UID/gnupg) removed after it."""
    want = b"--homedir\0" + home.encode() + b"\0"
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            if os.stat(f"/proc/{entry}").st_uid != os.getuid():
                continue
            with open(f"/proc/{entry}/cmdline", "rb") as f:
                if want in f.read():
                    os.kill(int(entry), signal.SIGTERM)
        except OSError:
            pass
    sock = subprocess.run(["gpgconf", "--homedir", home, "--list-dirs", "agent-socket"],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.decode().strip()  # fmt: skip
    deadline = time.monotonic() + 5
    while sock and os.path.exists(sock) and time.monotonic() < deadline:
        time.sleep(0.1)
    subprocess.run(["gpgconf", "--homedir", home, "--remove-socketdir"], stderr=subprocess.DEVNULL)


def keys_dialog(s, before, label):
    """Waits for one of xd-keys' dialogs to come up over `before` (they're
    transient, so not in the window list) and to hold still."""
    shot, box = wait_change(s, before, (0, 0, WIDTH, HEIGHT - 40), label, label)
    return shot


@scenario(LUNA_INI, requires=("dbusmock",))
def passwords_and_keys(s):
    """Passwords and Keys (xd-keys) on a mock Secret Service, an ssh-agent
    and a GnuPG home of its own: unlocking the locked Work keyring (through
    the service's prompt) and adding a password to it; loading an SSH key
    into the agent and making a new one with a passphrase; making a PGP key
    pair."""
    from dbusmock import DBusTestCase

    for tool in ("ssh-keygen", "ssh-agent", "ssh-add", "gpg", "gpgconf"):
        if shutil.which(tool) is None:
            s.log(f"skipped: {tool} isn't installed")
            return
    runtime = os.environ["XDG_RUNTIME_DIR"]
    # Not ~/.gnupg: the scratch home stands in for the real one, but the
    # default home's sockets in /run/user/UID/gnupg are the real agent's.
    gnupg = os.path.join(runtime, "gnupg")
    os.mkdir(gnupg, 0o700)
    with open(os.path.join(gnupg, "gpg-agent.conf"), "w") as f:
        f.write("disable-scdaemon\n")
    # Never the user's agent (SSH_AUTH_SOCK comes from their environment).
    agent_sock = os.path.join(runtime, "ssh-agent.sock")
    env = {"SSH_AUTH_SOCK": agent_sock, "GNUPGHOME": gnupg}
    tool_env = dict(os.environ, **env)
    ssh = os.path.join(s.home, ".ssh")
    os.mkdir(ssh, 0o700)
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "first@test", "-f",
                    os.path.join(ssh, "id_ed25519")], check=True)  # fmt: skip
    mock = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "secrets.py")
    proc, _ = DBusTestCase.spawn_server_template(mock, {}, stdout=subprocess.DEVNULL)
    s.children.append(proc)
    s.launch("ssh-agent", "-D", "-a", agent_sock)
    s.wait(lambda: os.path.exists(agent_sock), "the SSH agent")
    try:
        s.launch("xd-keys", env=env)
        s.wait_window(title="Passwords and Keys", state="activated")
        s.settle("passwords")

        # The Work keyring (second), locked: Unlock goes through the
        # service's prompt.
        work = "/org/freedesktop/secrets/collection/work"
        coll = "org.freedesktop.Secret.Collection"
        s.inject("key", "alt+k", "sleep", 200, "key", "Down", "sleep", 500, "key", "alt+u")
        s.wait(lambda: not secret_prop(work, coll, "Locked"), "the Work keyring to unlock")
        unlocked = s.settle("unlocked")
        s.log("ok: Unlock unlocked the Work keyring")
        s.inject("key", "alt+a")
        keys_dialog(s, unlocked, "add-password")
        s.inject("type", "Bank PIN", "key", "Tab", "type", "4321", "key", "Tab", "type", "4321")
        s.settle("add-password-typed")
        s.inject("key", "Return")

        def labels():
            return [secret_prop(i, "org.freedesktop.Secret.Item", "Label")
                    for i in secret_prop(work, coll, "Items")]  # fmt: skip

        s.wait(lambda: "Bank PIN" in labels(), "the new password in the Work keyring")
        s.settle("added")
        s.log("ok: Add stored the password in the Work keyring")

        # SSH Keys: the key, loaded into our agent.
        s.inject("key", "ctrl+Tab", "sleep", 500, "key", "Tab", "sleep", 200, "key", "Down",
                 "sleep", 300, "key", "alt+a")  # fmt: skip

        def agent_keys():
            r = subprocess.run(["ssh-add", "-l"], env=tool_env, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)  # fmt: skip
            return r.stdout.decode().count("SHA256:")

        s.wait(lambda: agent_keys() == 1, "the key in the SSH agent")
        ssh_tab = s.settle("ssh-keys")
        s.log("ok: Add to Agent loaded the key")
        # A new key with a passphrase (focus starts on it).
        s.inject("key", "alt+n")
        keys_dialog(s, ssh_tab, "new-ssh-key")
        s.inject("type", "pw123", "key", "Tab", "type", "pw123")
        s.settle("new-ssh-key")
        s.inject("key", "Return")
        new_key = os.path.join(ssh, "id_ed25519_2")
        s.wait(lambda: os.path.exists(new_key + ".pub"), "the new key pair")
        r = subprocess.run(["ssh-keygen", "-y", "-P", "pw123", "-f", new_key],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # fmt: skip
        s.check(r.returncode == 0, "the new key is protected by the passphrase typed")
        s.settle("ssh-new")

        # PGP Keys: a key pair without a passphrase.
        s.inject("key", "ctrl+Tab")
        pgp_tab = s.settle("pgp-tab")
        s.inject("key", "alt+n")
        keys_dialog(s, pgp_tab, "new-pgp-key")
        s.inject("key", "alt+n", "type", "Test User", "key", "alt+e", "type", "test@example.com")
        s.settle("new-pgp-key-typed")
        s.inject("key", "Return")

        def secret_uids():
            r = subprocess.run(["gpg", "--batch", "--with-colons", "--list-secret-keys"],
                               env=tool_env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)  # fmt: skip
            return r.stdout.decode()

        s.wait(lambda: "Test User <test@example.com>" in secret_uids(), "the PGP key pair",
               timeout=60)  # fmt: skip
        s.log("ok: New made the PGP key pair")
        s.settle("pgp-keys")
        s.inject("key", "Escape")
        s.wait_window(title="Passwords and Keys", gone=True)
        s.log("ok: Escape closed it")
    finally:
        stop_scratch_gpg(gnupg)


def mock_startup(s):
    """Before the compositor starts: dbusmock's user manager on the session
    bus (tests/dbusmock/systemd_startup.py: a sync client you enabled,
    PipeWire enabled for every user, others not starting with the session),
    and system autostart entries in a scratch $XDG_CONFIG_DIRS."""
    from dbusmock import DBusTestCase

    log = open(os.path.join(s.base, "systemd.log"), "w")
    template = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "systemd_startup.py")
    proc, _ = DBusTestCase.spawn_server_template(template, {}, stdout=log, system_bus=False)
    s.children.append(proc)
    xdg = os.path.join(s.base, "xdg")
    os.makedirs(os.path.join(xdg, "autostart"))
    entries = {
        "alpha.desktop": "Name=Alpha Sync\nExec=true --sync\n",
        "gnome.desktop": "Name=Gnome Only\nExec=true\nOnlyShowIn=GNOME;\n",
    }
    for name, body in entries.items():
        with open(os.path.join(xdg, "autostart", name), "w") as f:
            f.write("[Desktop Entry]\nType=Application\n" + body)
    mine = os.path.join(os.environ["XDG_CONFIG_HOME"], "autostart")
    os.makedirs(mine)
    with open(os.path.join(mine, "mine.desktop"), "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=My Notes\nExec=true --notes\n")
    os.environ["XDG_CONFIG_DIRS"] = xdg
    os.environ["XDG_CURRENT_DESKTOP"] = "XeonDesktop"


@scenario(LUNA_INI, requires=("dbusmock",), setup=mock_startup)
def msconfig_startup(s):
    """Run's "msconfig" opens the System Configuration Utility. On its
    Startup tab, clearing a system entry writes your copy with Hidden=true;
    on Services, clearing a service disables it through the user manager
    and remembers it; General then shows Selective Startup, and Normal
    Startup turns both back on."""
    import dbus

    title = "System Configuration Utility"
    autostart = os.path.join(os.environ["XDG_CONFIG_HOME"], "autostart")
    alpha = os.path.join(autostart, "alpha.desktop")
    remembered = os.path.join(s.config, "startup.ini")
    manager = dbus.Interface(
        dbus.SessionBus().get_object("org.freedesktop.systemd1", "/org/freedesktop/systemd1"),
        "org.freedesktop.systemd1.Manager",
    )

    def sync_enabled():
        return len(manager.GetUnitFileLinks("syncthing.service", False)) > 0

    def read(path):
        try:
            with open(path) as f:
                return f.read()
        except OSError:
            return None

    desk = s.settle("desktop")
    s.launch("xd-run")
    s.wait_window(title="Run", state="activated")
    s.inject("type", "msconfig", "key", "Return")
    s.wait_window(title=title, state="activated")
    s.log("ok: Run's msconfig opened the System Configuration Utility")
    general = s.settle("general")
    # The window, from what changed on the desktop (it opens on General).
    box = general.changed_box(desk, (0, 0, WIDTH, HEIGHT - 40))
    s.check(box is not None and box[2] > 400, f"the window shows ({box})")

    # The lists' check boxes, rows 1 and 2 (the same place on both tabs).
    check_x, row1, row2 = box[0] + 33, box[1] + 100, box[1] + 121

    # Startup: the system's Alpha Sync (first, by name) cleared.
    s.inject("key", "alt+t", "sleep", 300, "move", check_x, row1, "click", "sleep", 300,
             "key", "alt+a")  # fmt: skip
    s.wait(lambda: read(alpha) is not None, "your copy of Alpha Sync")
    s.check("Hidden=true" in read(alpha) and "Name=Alpha Sync" in read(alpha),
            "clearing a system entry wrote your copy with Hidden=true")  # fmt: skip

    # Add...: a command of your own. (Dialogs have no taskbar button, so
    # they're seen on the screen.)
    startup = s.settle("startup")
    s.inject("key", "alt+d")
    wait_change(s, startup, box, "the Add Startup Item dialog", "add")
    s.inject("key", "alt+c", "type", "true --added", "key", "Return")
    added = os.path.join(autostart, "true.desktop")
    s.wait(lambda: read(added) is not None, "the added entry")
    s.check("Exec=true --added" in read(added), "Add... wrote a command's entry")

    # Services: Syncthing (after PipeWire, which is greyed) cleared.
    s.inject("key", "alt+s", "sleep", 300, "move", check_x, row2, "click", "sleep", 300,
             "key", "alt+a")  # fmt: skip
    s.wait(lambda: not sync_enabled(), "Syncthing to be disabled")
    s.check("syncthing.service" in (read(remembered) or ""), "the disabled service is remembered")
    s.check(len(manager.GetUnitFileLinks("pipewire.service", False)) == 1,
            "PipeWire (every user's) was left alone")  # fmt: skip

    # General: Selective now; Normal turns both back on.
    s.inject("key", "alt+g", "sleep", 300)
    s.settle("selective")
    s.inject("key", "alt+n", "sleep", 300, "key", "alt+a")
    s.wait(sync_enabled, "Syncthing to be enabled again")
    s.wait(lambda: read(alpha) is None, "your copy of Alpha Sync to go")
    s.check("syncthing" not in (read(remembered) or ""), "nothing remembered as disabled")
    s.log("ok: Normal Startup turned the service and the entry back on")

    # OK: XP's restart question, here to log off; not now.
    before = s.settle("general-again")
    s.inject("key", "Return")
    wait_change(s, before, box, "the log off question", "log-off-question")
    s.inject("key", "alt+x")
    s.wait_window(title=title, gone=True)
    time.sleep(1)
    s.check(not session_program("xd-shutdown"), "Exit Without Log Off didn't log off")


# XP's Start menu: its pinned programs' rows, from (90, 352) down.
XP_PINNED_ROW = 38
GPU_PROBES = {"xd-gpu-game.desktop": ("GPU Game", "game.env", True),
              "xd-gpu-probe.desktop": ("GPU Probe", "probe.env", False)}  # fmt: skip


def mock_hybrid_graphics(s):
    """Before the compositor starts: a private system bus with
    switcheroo-control (tests/dbusmock/switcheroo.py: an integrated card,
    the default, and a dedicated one taking DRI_PRIME=1), and two games
    that write their environment to a file, one with
    PrefersNonDefaultGPU=true, pinned to the XP Start menu."""
    from dbusmock import DBusTestCase

    DBusTestCase.start_system_bus()
    template = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "switcheroo.py")
    proc, _ = DBusTestCase.spawn_server_template(
        template, {}, stdout=subprocess.DEVNULL, system_bus=True
    )
    s.children.append(proc)
    probe = os.path.join(s.base, "probe.sh")
    with open(probe, "w") as f:
        f.write('#!/bin/sh\nenv > "$1.tmp" && mv "$1.tmp" "$1"\n')
    os.chmod(probe, 0o755)
    for name, (title, out, prefers) in GPU_PROBES.items():
        with open(os.path.join(s.data, "applications", name), "w") as f:
            f.write(f"[Desktop Entry]\nType=Application\nName={title}\n"
                    f"Exec={probe} {os.path.join(s.base, out)}\nCategories=Game;\n"
                    + ("PrefersNonDefaultGPU=true\n" if prefers else ""))  # fmt: skip
    with open(os.path.join(s.config, "start-pinned"), "w") as f:
        f.write("".join(name + "\n" for name in GPU_PROBES))


@scenario(LUNA_INI, requires=("dbusmock",), setup=mock_hybrid_graphics)
def dedicated_graphics(s):
    """With two graphics cards (dbusmock switcheroo-control): a game with
    PrefersNonDefaultGPU=true starts on the dedicated card; another's
    right-click menu in the Start menu runs it there once ("Run with
    dedicated graphics card") or always (kept in gpu.ini); the first one's
    offers the integrated card instead."""
    game, probe = (90, 352), (90, 352 + XP_PINNED_ROW)
    gpu_ini = os.path.join(s.config, "gpu.ini")

    def env_of(out):
        try:
            with open(os.path.join(s.base, out)) as f:
                lines = f.read().splitlines()
        except OSError:
            return None
        return dict(l.split("=", 1) for l in lines if "=" in l)

    def run(row, out, what, *keys):
        """Starts a program from the Start menu: clicked, or with `keys`
        in its right-click menu; returns its environment."""
        try:
            os.remove(os.path.join(s.base, out))
        except FileNotFoundError:
            pass
        s.inject("key", "ctrl+Escape", "sleep", 600)
        s.settle("menu")
        if keys:
            s.inject("move", *row, "sleep", 200, "click", "right", "sleep", 600)
            s.settle("program-menu")
            steps = []
            for k in keys:
                steps += ["key", k]
            s.inject(*steps)
        else:
            s.inject("move", *row, "sleep", 200, "click")
        return s.wait(lambda: env_of(out), what)

    env = run(game, "game.env", "GPU Game to start")
    s.check(env.get("DRI_PRIME") == "1" and env.get("__GLX_VENDOR_LIBRARY_NAME") == "mesa",
            "PrefersNonDefaultGPU started it on the dedicated card")  # fmt: skip
    env = run(probe, "probe.env", "GPU Probe to start")
    s.check("DRI_PRIME" not in env, "another program starts on the default card")

    # Its menu: Open, Run with dedicated graphics card, Always run with...
    env = run(probe, "probe.env", "Run with dedicated graphics card", "Down", "Down", "Return")
    s.check(env.get("DRI_PRIME") == "1", "Run with dedicated graphics card used its variables")
    s.check(not os.path.exists(gpu_ini), "running it there once remembers nothing")

    s.inject("key", "ctrl+Escape", "sleep", 600)
    s.settle("menu")
    s.inject("move", *probe, "sleep", 200, "click", "right", "sleep", 600,
             "key", "Down", "key", "Down", "key", "Down", "key", "Return")  # fmt: skip
    s.wait(lambda: os.path.exists(gpu_ini), "gpu.ini")
    with open(gpu_ini) as f:
        s.check("xd-gpu-probe.desktop=dedicated" in f.read(),
                "Always run with dedicated graphics card is kept")  # fmt: skip
    env = run(probe, "probe.env", "GPU Probe to start again")
    s.check(env.get("DRI_PRIME") == "1", "it starts on the dedicated card from then on")

    # GPU Game's menu offers the integrated card.
    env = run(game, "game.env", "Run with integrated graphics card", "Down", "Down", "Return")
    s.check(env.get("DRI_PRIME") == "0", "Run with integrated graphics card used the default's")


# gvfs (GIO's other file systems) for the network scenarios: the sessions
# run with GIO_USE_VFS=local, so programs are started without it. gvfsd
# comes up on the session's own bus. Its "localtest" backend (local files
# through gvfs, no password) stands in for a network share: gvfs-smb and a
# Samba server aren't there to test with.
GVFS = ("env", "-u", "GIO_USE_VFS")


def gvfs_mounts():
    env = {k: v for k, v in os.environ.items() if k != "GIO_USE_VFS"}
    r = subprocess.run(["gio", "mount", "-l"], env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, timeout=20)  # fmt: skip
    return r.stdout.decode(errors="replace")


def have_localtest(s):
    if not os.path.exists("/usr/share/gvfs/mounts/localtest.mount") or not shutil.which("gio"):
        s.log("skipped: no gvfs (its localtest backend) or gio")
        return False
    return True


@scenario()
def network_drives(s):
    """Reconnect at logon (xd-files --reconnect: a drive mounted, one whose
    kind GIO can't open skipped, one mapped without Reconnect forgotten);
    Tools > Map Network Drive... (\\\\server\\share explains what isn't
    installed; a folder GIO can mount is connected, remembered and opened);
    Tools > Disconnect Network Drive... unmounts and forgets."""
    if not have_localtest(s):
        return
    ini = os.path.join(s.config, "network.ini")
    with open(ini, "w") as f:
        f.write("[Drive 1]\nURI=localtest:///\nReconnect=true\n\n"
                "[Drive 2]\nURI=smb://fileserver/public\nReconnect=true\n\n"
                "[Drive 3]\nURI=smb://old/session\nReconnect=false\n")  # fmt: skip
    p = s.launch(*GVFS, "xd-files", "--reconnect")
    s.wait(lambda: p.poll() is not None, "xd-files --reconnect to finish", 60)
    s.check("localtest -> localtest:///" in gvfs_mounts(), "the drive was reconnected")
    drives = s.ini("network.ini")
    s.check(drives.has_section("Drive 2") and not drives.has_section("Drive 3"),
            "the unreachable drive kept, the one without Reconnect forgotten")  # fmt: skip

    s.launch(*GVFS, "xd-files", "computer:///")
    s.wait_window(title="My Computer", state="activated")
    s.settle("my computer")

    # Dialogs of the window aren't in the window list: the window loses
    # the focus to them, and the screen changes.
    def dialog_up(what):
        s.wait(lambda: "activated" not in s.window(title="My Computer")["states"], what)
        return s.settle(what)

    # Tools > Map Network Drive...: a Windows share needs gvfs-smb. Where
    # it's missing the wizard says so; where it's installed, a share is
    # never tried (it would go looking on the network).
    s.inject("key", "alt+t", "sleep", 400, "key", "n")
    wizard = dialog_up("map network drive")
    if not os.path.exists("/usr/share/gvfs/mounts/smb.mount"):
        s.inject("type", "\\\\server\\share", "key", "Return")
        wait_change(s, wizard, (0, 0, WIDTH, HEIGHT - 40), "the explanation", "needs gvfs-smb")
        s.inject("key", "Return", "sleep", 400)
    else:
        s.log("skipped: gvfs-smb is installed, so the missing-backend explanation isn't shown")
    s.inject("key", "ctrl+a", "type", "localtest:///etc", "key", "Return")
    s.wait_window(title="etc", timeout=30)
    def mapped():
        c = s.ini("network.ini")
        return [c[g] for g in c.sections() if c[g].get("URI") == "localtest:///etc"]

    drive = s.wait(mapped, "the drive remembered")[0]
    s.check(drive.get("Reconnect") == "true",
            "Map Network Drive connected, remembered and opened the folder")  # fmt: skip
    s.settle("mapped")
    s.inject("key", "ctrl+w")
    s.wait_window(title="My Computer", state="activated")

    # Tools > Disconnect Network Drive...: the first (localtest:///), OK.
    s.inject("key", "alt+t", "sleep", 400, "key", "d")
    dialog_up("disconnect")
    s.inject("key", "Return")
    s.wait(lambda: "localtest" not in gvfs_mounts(), "the drive to be unmounted")
    left = s.ini("network.ini")
    uris = [left[g]["URI"] for g in left.sections() if g.startswith("Drive ")]
    s.check(uris == ["smb://fileserver/public"],
            f"Disconnect forgot the drive and the one inside it ({uris})")  # fmt: skip


@scenario()
def folder_sharing(s):
    """A folder's Properties, Sharing: "Share this folder on the network"
    with "Allow network users to change my files" makes a Samba user share
    (net usershare, on a scratch smb.conf); with user shares turned off the
    tab says what an administrator must do and shares nothing."""
    if not shutil.which("net"):
        s.log("skipped: Samba's net isn't installed")
        return
    shares = os.path.join(s.base, "usershares")
    os.mkdir(shares, 0o1770)
    on = os.path.join(s.base, "on.conf")
    with open(on, "w") as f:
        f.write(f"[global]\n  usershare path = {shares}\n  usershare max shares = 10\n")
    off = os.path.join(s.base, "off.conf")
    with open(off, "w") as f:
        f.write("[global]\n  workgroup = TEST\n")
    docs = os.path.join(s.home, "Documents")

    s.launch("xd-files", "--properties", docs, env={"XD_SMB_CONF": on})
    s.wait_window(title="Documents Properties", state="activated")
    s.inject("key", "ctrl+Tab", "sleep", 400)
    s.settle("sharing")
    s.inject("key", "alt+h", "sleep", 200, "key", "alt+w", "sleep", 200, "key", "Return")
    made = os.path.join(shares, "documents")
    s.wait(lambda: os.path.exists(made), "the user share")
    with open(made) as f:
        text = f.read()
    s.check(f"path={docs}\n" in text and "usershare_acl=S-1-1-0:F" in text,
            "the folder is shared, writable by everyone")  # fmt: skip
    s.wait_window(title="Documents Properties", gone=True)

    music = os.path.join(s.home, "Music")
    s.launch("xd-files", "--properties", music, env={"XD_SMB_CONF": off})
    s.wait_window(title="Music Properties", state="activated")
    before = s.settle("general")
    s.inject("key", "ctrl+Tab", "sleep", 400)
    shot = s.settle("sharing turned off")
    box = shot.changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
    s.check(box is not None, "the Sharing tab shows")
    s.inject("key", "alt+h", "sleep", 200, "key", "Return")
    s.wait_window(title="Music Properties", gone=True)
    s.check(os.listdir(shares) == ["documents"], "nothing shared while sharing is off")


def mock_updates(s):
    """Before the compositor starts: a private system bus with the PackageKit
    and fwupd mocks (tests/dbusmock), logging what they're asked to change;
    empty Flatpak installations of the session's own; and the shell's first
    scheduled check two seconds after logon, not five minutes."""
    from dbusmock import DBusTestCase

    DBusTestCase.start_system_bus()
    mocks = (
        ("packagekit", {"Log": os.path.join(s.base, "pk.log"), "Delay": 100}),
        ("fwupd", {"CabDir": os.path.join(s.base, "cabs"), "Log": os.path.join(s.base, "fw.log"),
                   "Delay": 150}),
    )  # fmt: skip
    for name, params in mocks:
        template = os.path.join(SOURCE_ROOT, "tests", "dbusmock", name + ".py")
        proc, _ = DBusTestCase.spawn_server_template(
            template, params, stdout=subprocess.DEVNULL, system_bus=True
        )
        s.children.append(proc)
    for var in ("FLATPAK_USER_DIR", "FLATPAK_SYSTEM_DIR", "FLATPAK_CONFIG_DIR"):
        path = os.path.join(s.base, var.lower())
        os.makedirs(path, exist_ok=True)
        os.environ[var] = path
    os.environ["XD_UPDATES_DELAY"] = "2"


def info_window_box(shot, rect, colour=(255, 255, 225)):
    """The bounding box of the balloon's InfoWindow yellow within rect, and
    how many sampled pixels have it."""
    x0, y0, w, h = rect
    xs, ys = [], []
    for y in range(y0, y0 + h, 2):
        for x in range(x0, x0 + w, 2):
            if max(abs(a - b) for a, b in zip(shot.pixel(x, y), colour)) <= 4:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None, 0
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1), len(xs)


@scenario(LUNA_INI, requires=("dbusmock",), setup=mock_updates)
def automatic_updates(s):
    """Automatic Updates on dbusmock PackageKit and fwupd: the shell's
    scheduled check refreshes and downloads (only-download, never an
    install) and XP's balloon points at the new notification area icon; a
    click opens Software Updates, whose Install updates the checked ones
    (asking about the restart a kernel needs); the Firmware tab updates a
    device through fwupd with the downloaded, checksummed cabinet file;
    Device Security shows the HSI; the options save [Updates]."""
    state_path = os.path.join(os.environ["XDG_STATE_HOME"], "xeondesktop", "updates.ini")
    pk_log = os.path.join(s.base, "pk.log")
    fw_log = os.path.join(s.base, "fw.log")

    def state():
        c = configparser.ConfigParser(interpolation=None)
        c.optionxform = str
        c.read(state_path)
        return dict(c["Updates"]) if c.has_section("Updates") else None

    def lines(path):
        try:
            with open(path) as f:
                return f.read().splitlines()
        except OSError:
            return []

    st = s.wait(state, "the scheduled check", 40)
    s.check(st["Software"] == "2" and st["Firmware"] == "2" and st["Important"] == "3",
            f"the check found two package and two firmware updates ({st})")  # fmt: skip
    s.check(st["Downloaded"] == "true", "and downloaded the packages")
    updates = [l for l in lines(pk_log) if l.startswith("update-packages")]
    s.check("refresh-cache" in lines(pk_log), "the check refreshed PackageKit's lists")
    s.check(updates and all(int(l.split()[1]) & 8 for l in updates),
            f"the check only downloaded, never installed ({updates})")  # fmt: skip

    near_tray = (WIDTH - 460, HEIGHT - 130, 460, 100)

    def balloon():
        box, n = info_window_box(s.shot("balloon"), near_tray)
        return box if n > 300 else None

    box = s.wait(balloon, "the balloon over the notification area")
    s.check(box[0] + box[2] > WIDTH - 120, f"the balloon points at the tray ({box})")
    desk = s.settle("desktop")
    # Its text, away from the close box: Software Updates opens.
    s.inject("move", box[0] + 40, box[1] + box[3] // 2 + 6, "click")
    s.wait_window(title="Automatic Updates", state="activated")
    s.log("ok: a click on the balloon opened Automatic Updates")
    s.wait(lambda: s.window(title="Automatic Updates") and balloon() is None,
           "the balloon to go")  # fmt: skip
    software = s.settle("software")
    x, y, _, _ = software.changed_box(desk, (0, 0, WIDTH, 600))

    # Install, with both updates checked: the kernel needs a restart.
    s.inject("key", "alt+i")
    s.wait(lambda: any(not int(l.split()[1]) & 8 for l in lines(pk_log)
                       if l.startswith("update-packages")), "the install")  # fmt: skip
    installs = [l for l in lines(pk_log) if l.startswith("update-packages")][-1]
    s.check("xdtest-editor" in installs and "xdtest-kernel" in installs,
            f"Install updated both packages ({installs})")  # fmt: skip
    s.wait(lambda: state()["Software"] == "0", "the count to drop")
    restart = s.settle("restart-question")
    s.check(restart.differs(software, (WIDTH // 2 - 150, HEIGHT // 2 - 60, 300, 120)) > 0.1,
            "a restart question shows")  # fmt: skip
    s.inject("key", "Escape")  # Restart Later
    s.inject("sleep", 500)

    # Firmware: the first device (System Firmware, with an update), Update...
    s.inject("key", "ctrl+Tab")
    firmware = s.settle("firmware")
    s.inject("move", x + 84, y + 157, "click")
    details = s.settle("firmware-details")
    s.check(details.differs(firmware, (x + 30, y + 365, 500, 90)) > 0.02,
            "the device's details show")  # fmt: skip
    s.inject("key", "alt+p", "sleep", 800, "key", "Return")  # Update..., Yes
    s.wait(lambda: any(l.startswith("install ") for l in lines(fw_log)), "fwupd's Install")
    s.check(lines(fw_log)[0].startswith("install a45df35ac0e948ee180fe216a5f703f32dda163f"),
            f"the system firmware's cabinet went to fwupd ({lines(fw_log)})")  # fmt: skip
    s.wait(lambda: state()["Firmware"] == "1", "the firmware count to drop")
    s.settle("firmware-restart")
    s.inject("key", "Escape", "sleep", 500)  # Restart Later
    flashed = s.settle("firmware-done")
    s.check(flashed.differs(details, (x + 380, y + 150, 160, 30)) > 0.02,
            "the device shows its new version")  # fmt: skip

    s.inject("key", "ctrl+Tab")
    security = s.settle("device-security")
    s.check(security.differs(flashed, (x + 20, y + 80, 520, 400)) > 0.1,
            "Device Security shows the HSI")  # fmt: skip

    # Automatic Updates: notify only, applied.
    s.inject("key", "alt+u", "sleep", 500, "key", "alt+n", "sleep", 300, "key", "alt+a")
    s.wait(lambda: s.ini().has_section("Updates") and s.ini()["Updates"].get("Mode") == "notify",
           "Mode=notify saved")  # fmt: skip
    s.settle("options")
    s.log("ok: notify only saved in [Updates]")


# ---- touchscreens, drawing tablets, touchpad gestures (xd-test-input-v1) ------------


ORANGE = (255, 128, 0)  # xd-test-input's window


def orange_box(shot):
    """xd-test-input's client area on screen (x, y, w, h), or None."""
    xs, ys = [], []
    for y in range(0, HEIGHT, 4):
        for x in range(0, WIDTH, 4):
            if shot.pixel(x, y) == ORANGE:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
    while x0 > 0 and shot.pixel(x0 - 1, y0) == ORANGE:
        x0 -= 1
    while y0 > 0 and shot.pixel(x0, y0 - 1) == ORANGE:
        y0 -= 1
    while x1 + 1 < WIDTH and shot.pixel(x1 + 1, y1) == ORANGE:
        x1 += 1
    while y1 + 1 < HEIGHT and shot.pixel(x1, y1 + 1) == ORANGE:
        y1 += 1
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1)


def input_window(s, *args, title="Input test"):
    """Opens xd-test-input (args: --no-touch, --no-tablet) and returns its
    log's path and its client area's box (the orange), found on screen."""
    log = os.path.join(s.base, f"{title.replace(' ', '-')}.log")
    s.launch(s.tool("xd-test-input"), log, "--title", title, *args)
    s.wait_window(title=title, state="activated")
    box = orange_box(s.settle("input-window"))
    if box is None:
        raise Failure(f"no {title} window on screen")
    return log, box


def logged(path):
    try:
        with open(path) as f:
            return f.read().splitlines()
    except OSError:
        return []


def wait_logged(s, path, prefix, what):
    """Waits for a line starting with prefix in the log; returns it."""

    def found():
        return next((l for l in logged(path) if l.startswith(prefix)), None)

    return s.wait(found, what, timeout=10)


@scenario()
def touch_reaches_client(s):
    """A finger on a program that reads touch reaches it as wl_touch, at the
    right place on its surface, and isn't turned into clicks; a tap on the
    taskbar's Start button (the shell reads only the pointer) opens the
    Start menu, the pointer worked by the finger."""
    log, (x, y, w, h) = input_window(s)
    s.inject("touch", "down", 1, x + 50, y + 40, "touch", "move", 1, x + 80, y + 60,
             "touch", "up", 1)  # fmt: skip
    down = wait_logged(s, log, "touch down 1", "the touch")
    s.check(down == "touch down 1 50 40", f"the finger landed at 50,40 on the surface ({down})")
    s.check("touch motion 1 80 60" in logged(log), "and moved to 80,60")
    wait_logged(s, log, "touch up 1", "the finger to lift")
    s.check(not any(l.startswith("pointer button") for l in logged(log)),
            "a program that reads touch gets no emulated clicks")  # fmt: skip
    before = s.settle("before-start")
    s.inject("touch", "down", 2, 20, HEIGHT - 12, "touch", "up", 2)
    menu = s.settle("start-menu")
    s.check(menu.differs(before, (0, 400, 160, 320)) > 0.2, "a tap on Start opened the Start menu")


@scenario()
def touch_close_button(s):
    """A tap on a caption's Close button closes the window (touch works the
    pointer on our frames)."""
    log, (x, y, w, h) = input_window(s)
    # The Close button: right of the caption, above the client area.
    s.inject("touch", "down", 1, x + w - 12, y - 11, "sleep", 100, "touch", "up", 1)
    wait_logged(s, log, "close", "the window to be asked to close")
    s.wait_window(title="Input test", gone=True)
    s.check(not any(l.startswith("touch") and l != "touchscreen" for l in logged(log)),
            "the program saw no touch on the frame")  # fmt: skip


@scenario()
def touch_drag_caption(s):
    """A finger dragging a caption moves the window; so does one on a
    program that moves its window itself from a touch (as a header bar
    does), the window following the finger."""
    log, (x, y, w, h) = input_window(s, "--drag")
    cx, cy = x + w // 3, y - 11  # on the caption
    s.inject("touch", "down", 1, cx, cy, "sleep", 100, "touch", "move", 1, cx + 40, cy + 30,
             "sleep", 100, "touch", "move", 1, cx + 90, cy + 70, "sleep", 300,
             "touch", "up", 1)  # fmt: skip
    mx, my, _, _ = input_window_box(s)
    s.check(abs(mx - (x + 90)) <= 2 and abs(my - (y + 70)) <= 2,
            f"the window followed the finger ({mx},{my}, expected {x + 90},{y + 70})")
    s.check(not any(l.startswith("touch down") for l in logged(log)),
            "the program saw no touch on its frame")  # fmt: skip
    px, py = mx + 100, my + 100  # in the client area: the program asks to move
    s.inject("touch", "down", 3, px, py, "sleep", 100, "touch", "move", 3, px - 30, py + 20,
             "sleep", 100, "touch", "move", 3, px - 60, py + 40, "sleep", 300,
             "touch", "up", 3)  # fmt: skip
    wait_logged(s, log, "touch down 3 100 100", "the program's touch")
    nx, ny, _, _ = input_window_box(s)
    s.check(abs(nx - (mx - 60)) <= 2 and abs(ny - (my + 40)) <= 2,
            f"the program's own move followed the finger ({nx},{ny}, expected "
            f"{mx - 60},{my + 40})")  # fmt: skip


def input_window_box(s):
    box = orange_box(s.settle("moved"))
    if box is None:
        raise Failure("the window is gone")
    return box


@scenario()
def pen_pressure(s):
    """A pen on a program that reads tablets (tablet-v2) reaches it as a pen,
    with its pressure and tilt; the tablet's pad follows the pen there and
    its buttons and ring reach the program too."""
    log, (x, y, w, h) = input_window(s)
    s.inject("pen", "in", x + 100, y + 100, "pen", "move", x + 110, y + 105, 0, 0, 0,
             "pen", "down", "pen", "move", x + 120, y + 110, 0.5, 20, -10,
             "pen", "press", 1, "pen", "lift", 1, "pen", "up",
             "pad", "press", 2, "pad", "lift", 2, "pad", "ring", 0, 90,
             "pen", "out")  # fmt: skip
    wait_logged(s, log, "tool proximity out", "the pen to leave")
    got = logged(log)
    s.check("tool proximity in" in got and "tool down" in got and "tool up" in got,
            "the pen came in, touched and lifted")  # fmt: skip
    s.check("tool motion 120 110" in got, "it moved on the surface's coordinates")
    s.check("tool pressure 32767" in got, "with half pressure (32767 of 65535)")
    s.check("tool tilt 20.0 -10.0" in got, "and its tilt")
    s.check("tool button 331 pressed" in got, "its barrel button reached the program")
    s.check("pad enter" in got and "pad button 2 pressed" in got and "pad ring 90.0" in got,
            "the pad followed the pen: its button and ring reached the program")  # fmt: skip
    s.check(not any(l.startswith("pointer button") for l in got),
            "the pen made no emulated clicks")  # fmt: skip


@scenario()
def pen_without_tablet_support(s):
    """A program without tablet-v2 gets the pen as the pointer: the tip
    clicks, the barrel button right-clicks; on our frames the pen drags the
    caption."""
    log, (x, y, w, h) = input_window(s, "--no-tablet")
    s.inject("pen", "in", x + 60, y + 50, "pen", "down", "pen", "up",
             "pen", "press", 1, "pen", "lift", 1)  # fmt: skip
    wait_logged(s, log, "pointer button 273 released", "the right button's release")
    got = logged(log)
    s.check("pointer button 272 pressed" in got and "pointer button 272 released" in got,
            "the tip worked the left button")  # fmt: skip
    s.check("pointer button 273 pressed" in got, "the barrel button the right one")
    s.check(not any(l.startswith("tool") for l in got), "and no pen events")
    cx, cy = x + w // 3, y - 11
    s.inject("pen", "move", cx, cy, "pen", "down", "sleep", 100, "pen", "move", cx + 50, cy + 40,
             "sleep", 100, "pen", "move", cx + 80, cy + 60, "sleep", 300, "pen", "up",
             "pen", "out")  # fmt: skip
    mx, my, _, _ = input_window_box(s)
    s.check(abs(mx - (x + 80)) <= 2 and abs(my - (y + 60)) <= 2,
            f"the pen dragged the caption ({mx},{my}, expected {x + 80},{y + 60})")


@scenario()
def touchpad_pinch(s):
    """Touchpad gestures go to the program under the pointer: a pinch (and
    a swipe and a hold) reach it with their fingers and scale."""
    log, (x, y, w, h) = input_window(s)
    s.inject("move", x + 100, y + 100, "pinch", "begin", 2, "pinch", "update", 0, 0, 1.5, 10,
             "pinch", "end", "swipe", "begin", 3, "swipe", "update", 30, 0, "swipe", "end",
             "rest", "begin", 2, "rest", "end", "cancel")  # fmt: skip
    wait_logged(s, log, "hold end", "the hold to end")
    got = logged(log)
    s.check("pinch begin 2" in got and "pinch update scale 1.50 rotation 10.0" in got
            and "pinch end" in got, "the pinch reached the program, scale 1.5")  # fmt: skip
    s.check("swipe begin 3" in got and "swipe update 30.0 0.0" in got and "swipe end" in got,
            "a three-finger swipe too")  # fmt: skip
    s.check("hold begin 2" in got and "hold end cancelled" in got, "and a cancelled hold")


# ---- programs capturing the mouse and keyboard (constraints.c) ---------------------

GRAB_BLUE = (0x20, 0x50, 0xC0)  # xd-test-grab's window
X11_GREEN = (0x20, 0xA0, 0x40)  # xd-test-x11grab's


def colour_box(shot, rgb, tolerance=6):
    """The bounding box (x, y, w, h) of pixels of colour rgb, or None."""
    xs, ys = [], []
    for y in range(0, HEIGHT, 4):
        for x in range(0, WIDTH, 4):
            if max(abs(p - q) for p, q in zip(shot.pixel(x, y), rgb)) <= tolerance:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)


class GrabLog:
    """What xd-test-grab or xd-test-x11grab heard, a line each."""

    def __init__(self, s):
        self.s = s
        self.path = os.path.join(s.base, "grab.log")
        self.mark = 0

    def lines(self):
        try:
            with open(self.path) as f:
                return f.read().splitlines()
        except OSError:
            return []

    def since(self):
        """The lines since the last mark()."""
        return self.lines()[self.mark :]

    def set_mark(self):
        self.mark = len(self.lines())

    def wait(self, line, what=None):
        """Waits for `line` (exactly) after the mark."""
        try:
            return self.s.wait(lambda: line in self.since(),
                               what or f"the program to hear {line!r}")  # fmt: skip
        except Failure as e:
            raise Failure(f"{e}; it heard {self.lines()[-40:]}") from None

    def motions(self):
        """The absolute positions it heard since the mark."""
        return [tuple(int(v) for v in l.split()[1:3]) for l in self.since()
                if l.startswith("motion ")]  # fmt: skip


def start_grab(s, *args):
    """Starts xd-test-grab with args; returns its log and its window's box."""
    log = GrabLog(s)
    # The seat has its pointer and keyboard once xd-inject has connected.
    s.inject("sleep", 1)
    s.launch(s.tool("xd-test-grab"), *args, env={"XD_TEST_GRAB_LOG": log.path})
    s.wait_window(title="Grab test", state="activated")
    log.wait("ready")
    box = s.wait(lambda: colour_box(s.settle("grab-window"), GRAB_BLUE), "the blue window")
    s.check(box[2] >= 390 and box[3] >= 290, f"the program's window is on screen at {box}")
    return log, box


def ctrl_alt(s):
    """Ctrl+Alt pressed and let go on its own."""
    s.inject("hold", "ctrl", "hold", "alt", "release", "alt", "release", "ctrl")


@scenario()
def pointer_lock(s):
    """A program locking the pointer (a game) hears relative motion, raw and
    accelerated, and no absolute motion, while the pointer stays put; Ctrl+Alt
    gives the pointer back where the program hinted, a click in the window
    locks it again, and Alt+Tab ends the lock (pointer-constraints,
    relative-pointer)."""
    probe = subprocess.run([s.tool("xd-test-grab"), "--probe"], stdout=subprocess.PIPE,
                           timeout=10).stdout.decode()  # fmt: skip
    s.check("pointer-constraints yes" in probe and "relative-pointer yes" in probe
            and "shortcuts-inhibit yes" in probe, "the protocols are offered")  # fmt: skip
    s.check("xwayland-keyboard-grab no" in probe,
            "Xwayland's keyboard grab isn't offered to other programs")
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    log, (x, y, w, h) = start_grab(s, "--lock", "--hint", "100,50")
    s.inject("move", x + w // 2, y + h // 2)
    log.wait("locked", "the pointer to lock when it's over the window")

    log.set_mark()
    s.inject("rmove", 30, 0, "rmove", 0, -20)
    log.wait("relative 30 0 30 0")
    log.wait("relative 0 -20 0 -20")
    # An absolute pointer (a tablet, a nested session's) is held too.
    s.inject("move", 5, 5, "sleep", 300)
    lines = log.since()
    s.check(not log.motions() and "pointer leave" not in lines,
            f"no absolute motion while locked: {lines}")
    s.check(any(l.startswith("relative ") and l != "relative 30 0 30 0"
                and l != "relative 0 -20 0 -20" for l in lines),
            "the absolute move came as relative motion")  # fmt: skip
    s.log("ok: relative motion only while locked")

    # Ctrl+Alt on its own: the pointer is back, at the hint (100, 50).
    log.set_mark()
    ctrl_alt(s)
    log.wait("unlocked", "Ctrl+Alt to unlock the pointer")
    s.inject("rmove", 1, 0)
    log.wait("motion 101 50", "the pointer to move from the program's hint")
    s.log("ok: Ctrl+Alt unlocked at the hint")
    s.inject("rmove", 10, 10, "sleep", 200)
    s.check("locked" not in log.since(), "it stays unlocked until the window is clicked")
    log.set_mark()
    s.inject("click")
    log.wait("locked", "a click in the window to lock the pointer again")
    s.log("ok: a click locks it again")

    # Alt+Tab: unlocked at once, and the other window has the keyboard.
    log.set_mark()
    s.inject("key", "alt+Tab")
    log.wait("unlocked", "Alt+Tab to unlock the pointer")
    s.wait_window(title="Control Panel", state="activated")
    s.log("ok: Alt+Tab unlocked the pointer")


@scenario()
def pointer_confine(s):
    """A program confining the pointer (a strategy game, a remote desktop)
    keeps it inside its window, also when moved far outside or by a
    tablet's absolute motion, and gets the clicks there; confinement to a
    region starts only once the pointer is in it; Ctrl+Alt lets it go."""
    log, (x, y, w, h) = start_grab(s, "--confine")
    s.inject("move", x + w // 2, y + h // 2)
    log.wait("confined", "the pointer to be confined once over the window")
    log.set_mark()
    s.inject("rmove", 2000, 2000, "sleep", 200)
    s.check("pointer leave" not in log.since() and log.motions()[-1] == (399, 299),
            f"moved far right and down, it stopped at the corner: {log.motions()[-1:]}")
    s.inject("move", 2, 2, "sleep", 200)
    s.check("pointer leave" not in log.since() and log.motions()[-1] == (0, 0),
            f"moved (absolute) to the top left, it stopped at the corner: {log.motions()[-1:]}")
    log.set_mark()
    s.inject("click")
    log.wait("button 272 pressed", "the click to reach the program")
    s.log("ok: confined to the window")

    log.set_mark()
    ctrl_alt(s)
    log.wait("unconfined", "Ctrl+Alt to end the confinement")
    s.inject("move", 900, 700)
    log.wait("pointer leave", "the pointer to leave the window")
    s.log("ok: Ctrl+Alt let the pointer go")


@scenario()
def pointer_confine_region(s):
    """Confinement to a region of the window (a client's rectangle) starts
    when the pointer enters that region and keeps it there."""
    log, (x, y, w, h) = start_grab(s, "--confine", "--region", "100,100,50,50")
    s.inject("move", x + 20, y + 20, "rmove", 10, 10, "sleep", 300)
    s.check("confined" not in log.lines(), "outside the region, the pointer is free")
    s.inject("move", x + 120, y + 120)
    log.wait("confined", "the pointer to be confined once in the region")
    log.set_mark()
    s.inject("rmove", 500, 500, "sleep", 200)
    last = log.motions()[-1]
    s.check(149 <= last[0] <= 150 and 149 <= last[1] <= 150,
            f"it stopped at the region's corner: {last}")
    s.inject("rmove", -500, -500, "sleep", 200)
    last = log.motions()[-1]
    s.check(last == (100, 100), f"and at its other corner: {last}")


@scenario()
def shortcuts_inhibit(s):
    """A program taking the shortcuts (a virtual machine) gets the Windows
    key, Ctrl+Esc, Alt+Tab and Alt+F4 while our Start menu stays closed and
    no window changes; Ctrl+Alt+Delete still opens Windows Security, and the
    inhibitor ends as the window loses the keyboard."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    log, box = start_grab(s, "--inhibit")
    log.wait("inhibitor active", "the inhibitor to be active for the focused window")
    before = s.settle("inhibiting")
    log.set_mark()
    s.inject("key", "super", "key", "ctrl+Escape", "key", "alt+Tab", "key", "alt+F4",
             "sleep", 500)  # fmt: skip
    for code, name in ((125, "the Windows key"), (1, "Esc"), (15, "Tab"), (62, "F4")):
        log.wait(f"key {code} pressed", f"the program to get {name}")
    after = s.settle("keys-sent")
    s.check(after.differs(before, (0, 400, 200, 368)) < 0.02, "the Start menu stayed closed")
    # (Its keyboard leaves and comes back with xd-inject's own; the focus
    # stays, and so the inhibitor.)
    s.check("activated" in s.window(title="Grab test")["states"]
            and "inhibitor inactive" not in log.since(),
            "Alt+Tab and Alt+F4 didn't switch or close the window")  # fmt: skip
    s.log("ok: the program got the Windows key, Ctrl+Esc, Alt+Tab and Alt+F4")

    s.inject("key", "ctrl+alt+Delete")
    s.wait_window(app_id="org.xeondesktop.Security", state="activated")
    log.wait("inhibitor inactive", "the inhibitor to end with the focus")
    s.log("ok: Ctrl+Alt+Delete still opened Windows Security")


@scenario()
def shortcuts_inhibit_focus(s):
    """An inhibitor works only while its window has the keyboard: Ctrl+Alt
    gives the shortcuts back (Alt+Tab switches windows again), and when its
    window gets the keyboard back the inhibitor is active again."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    log, box = start_grab(s, "--inhibit")
    log.wait("inhibitor active")
    log.set_mark()
    ctrl_alt(s)
    log.wait("inhibitor inactive", "Ctrl+Alt to give the shortcuts back")
    s.inject("key", "alt+Tab")
    s.wait_window(title="Control Panel", state="activated")
    s.check("key 15 pressed" not in log.since(), "Alt+Tab switched windows")
    log.set_mark()
    s.inject("key", "alt+Tab")
    s.wait_window(title="Grab test", state="activated")
    log.wait("inhibitor active", "the inhibitor to come back with the keyboard")
    s.inject("key", "alt+Tab", "sleep", 500)
    log.wait("key 15 pressed", "the program to get Alt+Tab again")
    s.check("activated" in s.window(title="Grab test")["states"], "and the window stayed in front")


def session_display():
    """The DISPLAY the session's programs get (from the shell's environment)."""
    for pid in session_program("xd-shell"):
        with open(f"/proc/{pid}/environ", "rb") as f:
            for v in f.read().split(b"\0"):
                if v.startswith(b"DISPLAY="):
                    return v[8:].decode()
    return None


@scenario(x11=True)
def x11_keyboard_grab(s):
    """An X11 program grabbing the keyboard and the pointer (as VirtualBox's
    and rdesktop's windows do): Xwayland's keyboard grab gives it Alt+Tab and
    the Windows key, its pointer grab keeps the pointer in its window (with
    no cursor of its own, Xwayland locks the pointer and moves its own),
    and Ctrl+Alt+Delete still reaches us."""
    if shutil.which("Xwayland") is None:
        s.log("skipped: Xwayland is not installed")
        return
    display = s.wait(session_display, "the session's DISPLAY")
    # Xwayland sets its keyboard grab up only for a keyboard the seat has as
    # it starts: one stays connected meanwhile (headless has none).
    s.launch(s.tool("xd-inject"), "sleep", "60000")
    log = GrabLog(s)
    s.launch(s.tool("xd-test-x11grab"), "--confine",
             env={"DISPLAY": display, "XD_TEST_GRAB_LOG": log.path})  # fmt: skip
    s.wait_window(title="X11 grab test", state="activated", timeout=40)
    log.wait("grab keyboard 0", "the X11 program to grab the keyboard")
    x, y, w, h = s.wait(lambda: colour_box(s.settle("x11-window"), X11_GREEN), "the green window")

    before = s.settle("x11-grabbed")
    log.set_mark()
    s.inject("key", "alt+Tab", "key", "super", "sleep", 500)
    log.wait("key 23 pressed", "the X11 program to get Tab")
    log.wait("key 133 pressed", "the X11 program to get the Windows key")
    after = s.settle("x11-keys")
    s.check(after.differs(before, (0, 400, 200, 368)) < 0.02, "the Start menu stayed closed")
    s.log("ok: the X11 grab took Alt+Tab and the Windows key")

    # A click grabs the pointer (confine_to the window; no cursor, so
    # Xwayland locks ours and moves its own within the window): moved
    # far away, the next click still lands in it.
    s.inject("move", x + w // 2, y + h // 2, "click")
    log.wait("grab pointer 0", "the X11 program to grab the pointer")
    s.inject("sleep", 300, "move", 1000, 740, "sleep", 300)
    log.set_mark()
    s.inject("click")
    log.wait("button 1 pressed", "the click to stay in the X11 window")
    s.log("ok: the X11 pointer grab kept the pointer in its window")

    s.inject("key", "ctrl+alt+Delete")
    s.wait_window(app_id="org.xeondesktop.Security", state="activated")
    s.log("ok: Ctrl+Alt+Delete still reached us")


# A theme package written by the scenario (our own flat colours, never
# anyone's art): caption, borders, close button and push buttons each in a
# colour nothing else on the screen has.
PKG_CAPTION = (200, 30, 160)
PKG_BORDER = (30, 160, 200)
PKG_CLOSE = (20, 200, 20)
PKG_BUTTON = (30, 60, 230)


def write_theme_package(s):
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    root = os.path.join(s.base, "testpkg")
    os.makedirs(os.path.join(root, "images"))
    os.makedirs(os.path.join(root, "styles"))

    def png(name, w, h, rgb):
        pb = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, w, h)
        pb.fill((rgb[0] << 24) | (rgb[1] << 16) | (rgb[2] << 8) | 0xFF)
        pb.savev(os.path.join(root, "images", name), "png", [], [])

    png("caption.png", 8, 30, PKG_CAPTION)
    png("border.png", 4, 4, PKG_BORDER)
    png("close.png", 21, 21, PKG_CLOSE)
    png("button.png", 10, 10, PKG_BUTTON)
    with open(os.path.join(root, "theme.xml"), "w") as f:
        f.write('<xdtheme version="1" name="UI Test" base="classic">\n'
                '  <variant id="plain" name="Plain" size="normal" style="styles/plain.xml"/>\n'
                "</xdtheme>\n")  # fmt: skip
    with open(os.path.join(root, "styles", "plain.xml"), "w") as f:
        f.write("""<style>
  <class name="Window">
    <part name="Caption" ImageFile="images/caption.png" SizingMargins="0 0 0 0"/>
    <part name="FrameLeft" ImageFile="images/border.png"/>
    <part name="FrameRight" ImageFile="images/border.png"/>
    <part name="FrameBottom" ImageFile="images/border.png"/>
    <part name="CloseButton" ImageFile="images/close.png" SizingType="TrueSize"/>
  </class>
  <class name="Button">
    <part name="PushButton" ImageFile="images/button.png" SizingMargins="2 2 2 2"
          ContentMargins="3 3 3 3" TextColor="255 255 255"/>
  </class>
</style>
""")
    os.environ["XD_THEME_PACKAGE"] = root
    os.environ["XD_UXTHEME_NAMES"] = os.path.join(SOURCE_ROOT, "data", "theme", "uxtheme.ini")


def color_box(shot, rgb):
    """How many pixels are exactly rgb, and their bounding box (x0, y0, x1, y1)."""
    n, x0, y0, x1, y1 = 0, shot.width, shot.height, -1, -1
    want = bytes(rgb)
    for y in range(shot.height):
        row = y * shot.stride
        for x in range(shot.width):
            i = row + x * shot.channels
            if shot.data[i : i + 3] == want:
                n += 1
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return n, (x0, y0, x1, y1)


@scenario(setup=write_theme_package)
def theme_package(s):
    """With $XD_THEME_PACKAGE set, a window's frame and caption button
    (drawn by the compositor) and its push buttons (libxdui) come from the
    package's images."""
    s.launch("xd-run")
    s.wait_window(title="Run", state="activated")
    shot = s.settle("run")
    n, cap = color_box(shot, PKG_CAPTION)
    s.check(n > 200 * 20, f"the caption is the package's image ({n} pixels at {cap})")
    s.check(cap[3] - cap[1] + 1 == 30, f"the caption is as tall as its image ({cap})")
    n, border = color_box(shot, PKG_BORDER)
    s.check(n > 2 * 4 * 100 and border[0] < cap[0] + 1 and border[3] > cap[3] + 50,
            f"the borders are the package's ({n} pixels at {border})")  # fmt: skip
    n, close = color_box(shot, PKG_CLOSE)
    s.check(n == 21 * 21 and close[2] - close[0] == 20 and close[3] - close[1] == 20,
            f"the close button is the package's 21x21 image ({n} pixels at {close})")  # fmt: skip
    s.check(cap[0] <= close[0] and close[2] <= cap[2] + 8 and cap[1] <= close[1]
            and close[3] <= cap[3], "the close button is on the caption")  # fmt: skip
    n, buttons = color_box(shot, PKG_BUTTON)
    s.check(n > 3 * 60 * 15 and buttons[1] > cap[3] and cap[0] <= buttons[0]
            and buttons[2] <= cap[2], f"the push buttons are the package's ({n} pixels at {buttons})")  # fmt: skip


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
